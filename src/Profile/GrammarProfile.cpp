#include "GrammarProfile.hpp"
#include "GrammarNames.hpp"
#include "LintRules.hpp"
#include "UnicodeAlphanumeric.hpp"
#include "../Core/CommentFormat.hpp"
#include "../Core/CommentRepository.hpp"
#include "../Core/SyntaxModel.hpp"
#include "../Util/CharClass.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include <tree_sitter/api.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
	using Core::DocDescription;
	using Core::DocItemForm;
	using Core::DocStylePolicy;
	using Core::DocVocabularyEntry;

	constexpr std::string_view JsDescriptionTag = "@description"; // JSDoc の説明
	constexpr std::string_view JsSummaryTag = "@summary"; // JSDoc の短い概要
	constexpr std::string_view JsTemplateTag = "@template"; // JSDoc の型の仮引数
	constexpr std::string_view DoxygenTypeParamTag = "@tparam"; // Doxygen の型の仮引数

	constexpr DocVocabularyEntry AtTagVocabulary[] = {
		{
			DocDescription::Failure,
			DocItemForm::Tag,
			"@throws",
			"",
			"",
			{ "@exception", "@raise", "@throw", "@throws", "\\exception", "\\throw", "\\throws" }
		},
		{ DocDescription::Inheritance, DocItemForm::Tag, "@inheritdoc", "", "", { "@inheritdoc" } },
		{ DocDescription::NestedSig, DocItemForm::Tag, "@overload", "", "", { "@overload" } },
		{ DocDescription::Other, DocItemForm::Tag, "", "", "", {} },
		{
			DocDescription::Param,
			DocItemForm::Tag,
			"@param",
			"@param ",
			"",
			{ "@arg", "@argument", "@param", "\\param" },
			{},
			{ "in", "in,out", "inout", "out", "out,in" }
		},
		{ DocDescription::ParamList, DocItemForm::Tag, "", "", "", {} },
		{
			DocDescription::Return,
			DocItemForm::Tag,
			"@return",
			"@return",
			"",
			{ "@result", "@return", "@returns", "\\result", "\\return", "\\returns" },
			"@return"
		},
		{
			DocDescription::Summary,
			DocItemForm::Tag,
			"@brief",
			"",
			"",
			{ "@brief", "@desc", JsDescriptionTag, "@short", JsSummaryTag, "\\brief", "\\short" }
		},
		{ DocDescription::TypeParam, DocItemForm::Tag, "", "", "", { JsTemplateTag, DoxygenTypeParamTag, "\\tparam" } }
	}; // AtTag の説明項目・読取語・生成記法

	constexpr DocVocabularyEntry CalloutVocabulary[] = {
		{ DocDescription::Failure, DocItemForm::List, "Throws", "- Throws:", "", { "throws" } },
		{ DocDescription::Inheritance, DocItemForm::Prose, "", "", "", {} },
		{
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
		{ DocDescription::Param, DocItemForm::List, "Parameter", "- Parameter ", ":", { "parameter" } },
		{ DocDescription::ParamList, DocItemForm::List, "Parameters", "- Parameters:", "", { "parameters" } },
		{ DocDescription::Return, DocItemForm::List, "Returns", "- Returns:", "", { "returns" }, "- Returns:" },
		{ DocDescription::Summary, DocItemForm::Prose, "", "", "", {} }
	}; // Callout の説明項目・読取語・生成記法

	constexpr DocVocabularyEntry DocstringVocabulary[] = {
		{ DocDescription::Failure, DocItemForm::Prose, "", "", "", {} },
		{ DocDescription::Inheritance, DocItemForm::Prose, "", "", "", {} },
		{ DocDescription::Other, DocItemForm::Prose, "", "", "", {} },
		{ DocDescription::Param, DocItemForm::Prose, "", "", "", {} },
		{ DocDescription::ParamList, DocItemForm::Prose, "", "", "", {} },
		{ DocDescription::Return, DocItemForm::Prose, "", "", "", {} },
		{ DocDescription::Summary, DocItemForm::Prose, "", "", "", {}, {}, {}, {}, DocStylePolicy::SrcOwned }
	}; // Docstring の説明項目・読取語・生成記法

	constexpr DocVocabularyEntry MarkdownHeadingVocabulary[] = {
		{ DocDescription::Failure, DocItemForm::Heading, "Errors", "# Errors", "", { "Errors", "Panics" } },
		{ DocDescription::Inheritance, DocItemForm::Prose, "", "", "", {} },
		{ DocDescription::Other, DocItemForm::Heading, "", "", "", {} },
		{ DocDescription::Param, DocItemForm::HeadingList, "", "* `", "`", {} },
		{ DocDescription::ParamList, DocItemForm::Heading, "Arguments", "# Arguments", "", { "Arguments" }, "# Arguments" },
		{ DocDescription::Return, DocItemForm::Heading, "Returns", "# Returns", "", { "Returns" }, "# Returns" },
		{ DocDescription::Summary, DocItemForm::Prose, "", "", "", {} }
	}; // MarkdownHeading の説明項目・読取語・生成記法

	constexpr DocVocabularyEntry ProseVocabulary[] = {
		{ DocDescription::Failure, DocItemForm::Prose, "", "", "", {} },
		{ DocDescription::Inheritance, DocItemForm::Prose, "", "", "", {} },
		{ DocDescription::Other, DocItemForm::Prose, "", "", "", {} },
		{ DocDescription::Param, DocItemForm::Prose, "", "", "", {} },
		{ DocDescription::ParamList, DocItemForm::Prose, "", "", "", {} },
		{ DocDescription::Return, DocItemForm::Prose, "", "", "", {} },
		{ DocDescription::Summary, DocItemForm::Prose, "", "", "", {}, {}, {}, {}, DocStylePolicy::NameSummary }
	}; // Prose の説明項目・読取語・生成記法

	constexpr DocVocabularyEntry XmlTagVocabulary[] = {
		{ DocDescription::Failure, DocItemForm::XmlElement, "exception", "<exception", "", { "exception" } },
		// 先頭の綴りを継承項目の識別に用いる
		{ DocDescription::Inheritance, DocItemForm::XmlElement, "inheritdoc", "", "", { "inheritdoc", "include" } },
		{ DocDescription::Other, DocItemForm::XmlElement, "", "", "", {} },
		{ DocDescription::Param, DocItemForm::XmlElement, "param", "<param name=\"", "\"></param>", { "param" }, {}, {}, "name" },
		{ DocDescription::ParamList, DocItemForm::XmlElement, "", "", "", {} },
		{ DocDescription::Return, DocItemForm::XmlElement, "returns", "<returns>", "</returns>", { "returns" }, "<returns></returns>" },
		{
			DocDescription::Summary,
			DocItemForm::XmlElement,
			"summary",
			"<summary>",
			"</summary>",
			{ "summary" },
			"<summary></summary>"
		},
		{
			DocDescription::TypeParam,
			DocItemForm::XmlElement,
			"typeparam",
			"<typeparam name=\"",
			"\"></typeparam>",
			{ "typeparam" },
			{},
			{},
			"name"
		}
	}; // XmlTag の説明項目・読取語・生成記法

	/**
	 * Profile が採用するドキュメント語彙の取得関数
	 * @param Style 項目の記法
	 * @return 静的な定義だけを参照する変更不能な語彙
	 */
	constexpr std::span<const DocVocabularyEntry> GetDocVocabulary(const Core::DocStyle Style) {
		switch(Style) {
		case Core::DocStyle::AtTag:
			// 対応する語彙の返戻
			return AtTagVocabulary;
		case Core::DocStyle::Callout:
			// 対応する語彙の返戻
			return CalloutVocabulary;
		case Core::DocStyle::Docstring:
			// 対応する語彙の返戻
			return DocstringVocabulary;
		case Core::DocStyle::MarkdownHeading:
			// 対応する語彙の返戻
			return MarkdownHeadingVocabulary;
		case Core::DocStyle::Prose:
			// 対応する語彙の返戻
			return ProseVocabulary;
		case Core::DocStyle::XmlTag:
			// 対応する語彙の返戻
			return XmlTagVocabulary;
		}
		// 対応する記法が無い事の返戻
		return {};
	}
} // namespace

/**
 * アクセス修飾の意味と長さの読取関数
 * @param Text 空白で区切ったアクセス修飾
 * @return 語の順に依らない意味と長さ（未知の語は未登録の集合）
 * @throws std::bad_alloc 可視性のトークン列を確保出来ない場合
 * @throws std::length_error トークン列の長さが上限を超える場合
 */
Core::AccessMeaning GrammarProfile::ReadAccessMeaning(const std::string_view Text) const {
	// 登録より長い本文の未確定な意味の返戻
	if(Text.size() > AccessLimit) return { UINT64_MAX, 0 };
	Core::AccessMeaning Result;
	std::string Canonical;
	for(size_t Start = 0; Start < Text.size();) {
		while(Start < Text.size() && std::isspace(static_cast<unsigned char>(Text[Start]))) ++Start;
		if(Start == Text.size()) break;
		size_t End = Start;
		while(End < Text.size() && !std::isspace(static_cast<unsigned char>(Text[End]))) ++End;
		const std::string_view Token = Text.substr(Start, End - Start);
		const std::unordered_map<std::string_view, uint64_t>::const_iterator Found = AccessSymbols.find(Token);
		// 登録されて居ない語の集合の返戻
		if(Found == AccessSymbols.end()) return { UINT64_MAX, 0 };
		Result.Mask |= Found->second;
		if(!Canonical.empty()) Canonical += ' ';
		Canonical += Token;
		Start = End;
	}
	Result.Length = Canonical.size();
	if(
		const std::vector<std::string_view>::const_iterator Exact = std::ranges::find(Rules.AccessOrder, Canonical);
		Exact != Rules.AccessOrder.end()
	) Result.ExactRank = static_cast<int>(Exact - Rules.AccessOrder.begin());
	// 入力側で確定したアクセス修飾の意味の返戻
	return Result;
}

/**
 * 明示キャストの組込型の読取関数
 * @param Type 入力側が確定した変換先の型のテキスト
 * @return 修飾と参照・間接参照を除き組込型だけから成れば true
 */
bool GrammarProfile::IsBuiltinCastType(std::string_view Type) {
	// 変換先の型のトークンを読み，型名・配列・関数等を対象に含めない
	bool HasBuiltin = false;
	while(!Type.empty()) {
		Type.remove_prefix(std::min(Type.find_first_not_of(" \t\r\n*&"), Type.size()));
		if(Type.empty()) break;
		if(Type.starts_with("/*")) {
			const size_t End = Type.find("*/", 2);
			// 閉じて居ないコメントを持つ型の対象外の返戻
			if(End == std::string_view::npos) return false;
			Type.remove_prefix(End + 2);
			continue;
		}
		if(Type.starts_with("//")) {
			Type.remove_prefix(std::min(Type.find('\n'), Type.size()));
			continue;
		}
		const size_t End = std::min(Type.find_first_of(" \t\r\n*&/"), Type.size());
		// 型のトークンに使わない記号の対象外の返戻
		if(!End) return false;
		const std::string_view Token = Type.substr(0, End);
		Type.remove_prefix(End);
		if(Token == LintRules::ConstQualifier || Token == LintRules::VolatileQualifier) continue;
		// 組込型以外のトークンを持つ変換先の対象外の返戻
		if(std::ranges::find(LintRules::CastBuiltinWords, Token) == std::end(LintRules::CastBuiltinWords)) return false;
		HasBuiltin = true;
	}
	// 組込型のトークンだけで構成する型かの返戻
	return HasBuiltin;
}

/**
 * 数値リテラルの分類関数
 * 整数部・小数部・指数の後に残る接尾辞を文法の固定登録で照合し，虚数の係数を整数又は実数へ読み替えない
 * 計算量：テキストの長さ L と登録した接尾辞の総長 S に対し O(L + S)
 * @param Text リテラルの文字列
 * @return 整数・浮動小数又は其の他の分類
 */
Core::NumericLiteralKind GrammarProfile::ReadNumericLiteral(const std::string_view Text) const {
	using Core::NumericLiteralKind;
	const NumericHead Head = ReadNumericHead(Text);
	size_t Pos = Head.End;
	bool IsFloating = false;
	if(Pos < Text.size() && Text[Pos] == '.') {
		IsFloating = true;
		++Pos;
		for(; Pos < Text.size(); ++Pos) {
			const unsigned char Char = Text[Pos];
			if(!(Head.Base == 16 ? std::isxdigit(Char) : std::isdigit(Char)) && Char != '_' && Char != '\'') break;
		}
	}
	if(Pos < Text.size() && (Head.Base == 16 ? Text[Pos] == 'p' || Text[Pos] == 'P' : Text[Pos] == 'e' || Text[Pos] == 'E')) {
		IsFloating = true;
		++Pos;
		if(Pos < Text.size() && (Text[Pos] == '+' || Text[Pos] == '-')) ++Pos;
		for(; Pos < Text.size(); ++Pos) {
			const unsigned char Char = Text[Pos];
			if(!std::isdigit(Char) && Char != '_' && Char != '\'') break;
		}
	}
	const std::string_view Suffix = Text.substr(Pos);
	const bool HasImaginarySuffix =
	Suffix.size() == 1 && Rules.ImaginaryLiteralSuffixes.find(Suffix.front()) != std::string_view::npos;
	// 虚数の型と係数の記法を分けた分類の返戻
	if(HasImaginarySuffix) return IsFloating ? NumericLiteralKind::ImaginaryFloatingPoint : NumericLiteralKind::ImaginaryInteger;
	const bool HasTypeSuffix = std::ranges::find(Rules.NumericTypeSuffixes, Suffix) != Rules.NumericTypeSuffixes.end();
	const bool IsFloatType = std::ranges::find(LintRules::FloatTypeSuffixes, Suffix) != std::end(LintRules::FloatTypeSuffixes);
	const bool HasFloatType = Head.Base == 10 && HasTypeSuffix && IsFloatType;
	const bool HasIntegerSuffix = std::ranges::find(Rules.IntegerLiteralSuffixes, Suffix) != Rules.IntegerLiteralSuffixes.end();
	const bool HasFloatSuffix = Suffix.size() == 1 && Rules.FloatingLiteralSuffixes.find(Suffix.front()) != std::string_view::npos;
	// 型接尾辞を持つ実数又は小数部・指数を持つ実数の分類の返戻
	if(IsFloating || HasFloatType || HasFloatSuffix && !HasIntegerSuffix) {
		// 登録した型接尾辞を持つ浮動小数の返戻
		if(Suffix.empty() || HasFloatType || HasFloatSuffix) return NumericLiteralKind::FloatingPoint;
		// 実数の型接尾辞でない値の返戻
		return NumericLiteralKind::Other;
	}
	const bool IsInteger =
	Head.End > ReadNumericPrefix(Text, false).End && (Suffix.empty() || HasTypeSuffix && !IsFloatType || HasIntegerSuffix);
	// 整数の全桁と登録した型接尾辞を持つ分類の返戻
	if(IsInteger) return NumericLiteralKind::Integer;
	// 未登録の接尾辞又は整数の桁を持たない値の返戻
	return NumericLiteralKind::Other;
}

/**
 * 入力リテラルの値を対象言語の真偽へ変換する関数
 * 型やエスケープの値を確定出来ない綴りは未確定とする
 * 計算量：リテラル長 L と固定の型接尾辞の総長 S に対し O(L + S)
 * @param Text 解析用マーカーを除いたリテラル
 * @param Roles 入力ノードの構文の役割
 * @param Work リテラルの読取と固定表の照合の作業予算（無なら計上しない）
 * @return 確定した真偽と数値の区分，又は未確定
 * @throws FormatLimitExceeded リテラルの読取が処理予算を超える場合
 */
Core::ConstantTruth GrammarProfile::GetLiteralTruth(
	const std::string_view Text,
	const Core::RoleSet &Roles,
	Core::StructureWorkAccount *const Work
) const {
	using Core::ConstantTruth;
	const bool IsNumericLanguage =
	Lang == Lang::C || Lang == Lang::Cpp || Lang == Lang::PHP || Lang.IsJsTs() || Lang == Lang::Ruby || Lang == Lang::Python;
	// リテラルを数値・文字列から真偽へ変換しない言語の返戻
	if(!IsNumericLanguage || Text.empty()) return ConstantTruth::Unknown;
	if(Roles.test(static_cast<size_t>(Core::Role::NumLiteral))) {
		// Ruby の数値は０を含めて真である事の返戻
		if(Lang == Lang::Ruby) return ConstantTruth::NumericTrue;
		const size_t Sign = Text.front() == '+' || Text.front() == '-' ? 1 : 0;
		const std::string_view Number = Text.substr(Sign);
		const NumericHead Prefix = ReadNumericPrefix(Number, Lang == Lang::C || Lang == Lang::Cpp || Lang == Lang::PHP);
		if(Work) Work->Charge(Sign + Prefix.End);
		bool HasDigit = false, HasValue = false, HasPoint = false;
		size_t FractionPlaces = 0, FirstValuePlace = 0, Pos = Prefix.End;
		for(; Pos < Number.size(); ++Pos) {
			if(Work) Work->Charge(1);
			const unsigned char Char = Number[Pos];
			if(Char == Rules.DigitSeparator) continue;
			if(Char == '.' && !HasPoint && (Prefix.Base == 10 || Prefix.Base == 16)) {
				HasPoint = true;
				continue;
			}
			const unsigned char Lower = static_cast<unsigned char>(Char | 0X20);
			const unsigned Digit = Char >= '0' && Char <= '9' ?
			static_cast<unsigned>(Char - '0') :
			Lower >= 'a' && Lower <= 'f' ? static_cast<unsigned>(Lower - 'a' + 10) : Prefix.Base;
			if(Digit >= Prefix.Base) break;
			HasDigit = true;
			if(HasPoint) ++FractionPlaces;
			if(Digit && !HasValue) FirstValuePlace = FractionPlaces;
			HasValue = HasValue || static_cast<bool>(Digit);
		}
		// 桁の無いトークンの返戻
		if(!HasDigit) return ConstantTruth::Unknown;
		int Exponent = 0;
		const bool HasExponent = Pos < Number.size() &&
		(Prefix.Base == 16 ? Number[Pos] == 'p' || Number[Pos] == 'P' : Number[Pos] == 'e' || Number[Pos] == 'E');
		if(HasExponent) {
			++Pos;
			if(Pos < Number.size() && Number[Pos] == '+') {
				if(Work) Work->Charge(1);
				++Pos;
			}
			const std::from_chars_result Read = std::from_chars(Number.data() + Pos, Number.data() + Number.size(), Exponent);
			if(Work) Work->Charge(static_cast<size_t>(Read.ptr - Number.data()) - Pos);
			// 指数を確定出来ない場合の返戻
			if(Read.ec != std::errc()) return ConstantTruth::Unknown;
			Pos = static_cast<size_t>(Read.ptr - Number.data());
		}
		const std::string_view Suffix = Number.substr(Pos);
		if(Work && Suffix.size() == 1) Work->Charge(Rules.FloatingLiteralSuffixes.size());
		const bool IsFloatSuffix = Suffix.size() == 1 && Rules.FloatingLiteralSuffixes.find(Suffix.front()) != std::string_view::npos;
		if(Work && Suffix.size() == 1 && !IsFloatSuffix) Work->Charge(Rules.ImaginaryLiteralSuffixes.size());
		bool IsKnownSuffix = Suffix.empty() || IsFloatSuffix ||
		Suffix.size() == 1 && Rules.ImaginaryLiteralSuffixes.find(Suffix.front()) != std::string_view::npos;
		if(!IsKnownSuffix) for(const std::string_view Known : Rules.IntegerLiteralSuffixes) {
			if(Work) Work->Charge(Known.size() + 1);
			if(Suffix == Known) {
				IsKnownSuffix = true;
				break;
			}
		}
		// 利用者定義の型への変換等を持ち得る接尾辞の返戻
		if(!IsKnownSuffix) return ConstantTruth::Unknown;
		// BigInt の真偽は確定出来るが，単項の加算が数値型と同じ契約でない事の返戻
		if(Lang.IsJsTs() && Suffix == "n") return HasValue ? ConstantTruth::True : ConstantTruth::False;
		// 非零の浮動小数が０へ丸められ得る範囲は，対象の表現を証明せず真にしない
		if(HasValue && (HasPoint || HasExponent || IsFloatSuffix)) {
			const int Minimum = std::numeric_limits<float>::min_exponent10;
			// １６進の小数・指数は２進の丸めの事実が必要である事の返戻
			if(
				Prefix.Base != 10 || FirstValuePlace > static_cast<size_t>(-Minimum) || Exponent < Minimum + static_cast<int>(FirstValuePlace)
			) return ConstantTruth::Unknown;
		}
		// ０か非零かから確定した数値の真偽の返戻
		return HasValue ? ConstantTruth::NumericTrue : ConstantTruth::NumericFalse;
	}
	// 通常の引用文字列以外は補間・専用のデコードを此処で仮定しない事の返戻
	if(!Roles.test(static_cast<size_t>(Core::Role::StrLikeAll)) || Text.size() < 2) return ConstantTruth::Unknown;
	// Ruby の文字列は空でも真である事の返戻
	if(Lang == Lang::Ruby) return ConstantTruth::True;
	if(Work) Work->Charge(2);
	const char Quote = Text.front();
	// 接頭辞・接尾辞又は専用の区切を持つ文字列の返戻
	if(Quote != '"' && Quote != '\'' || Text.back() != Quote) return ConstantTruth::Unknown;
	const std::string_view Body = Text.substr(1, Text.size() - 2);
	// エスケープ・複数の引用の連結の値を確定しない事の返戻
	for(const char Char : Body) {
		if(Work) Work->Charge(1);
		// デコード又は別のリテラルの型が必要である場合の返戻
		if(Char == '\\' || Char == Quote) return ConstantTruth::Unknown;
	}
	// C / C++ の通常の文字列は，空でも非零のアドレスへ変換する事の返戻
	if((Lang == Lang::C || Lang == Lang::Cpp) && Quote == '"') return ConstantTruth::True;
	// PHP は空文字列と１文字の０だけを偽に変換する事の返戻
	if(Lang == Lang::PHP) return Body.empty() || Body == "0" ? ConstantTruth::False : ConstantTruth::True;
	// C の文字定数の値が必要である場合の返戻
	if(Lang == Lang::C || Lang == Lang::Cpp) {
		// 単一の字の値を確定出来ない場合の返戻
		if(Body.size() != 1) return ConstantTruth::Unknown;
		// 入力に在る字の値から確定した数値の真偽の返戻
		return Body.front() ? ConstantTruth::NumericTrue : ConstantTruth::NumericFalse;
	}
	// 其の他の通常の文字列の空否からの返戻
	return Body.empty() ? ConstantTruth::False : ConstantTruth::True;
}

/**
 * 文法の場に対応する子の役割の取得関数
 * 計算量：O(1)
 * @param Field 文法の場の識別子
 * @return 共通の子の役割（未使用の場は無い）
 */
Core::ChildRole GrammarProfile::GetChildRoleOf(const TSFieldId Field) const {
	return Field < FieldRoles.size() ? FieldRoles[Field] : Core::ChildRole::None;
}

/**
 * 文法の場の名前に対応する子の役割の取得関数
 * 計算量：場の名前の長さ L と文法の場の数 F に対し O(L F)
 * @param Name パーサーから読んだ場の名前
 * @return 登録済の子の役割，場が無ければ無
 */
Core::ChildRole GrammarProfile::GetChildRoleOf(const std::string_view Name) const {
	// 文法毎に登録した場の役割の返戻
	return GetChildRoleOf(ts_language_field_id_for_name(Lang.GetTreeSitterLang(), Name.data(), static_cast<uint32_t>(Name.size())));
}

/**
 * 入力側だけが区別する場の役割の中核の子の役割への対応関数
 * 同じ中核の役割へ写る場の名前（`function` と `method` 等）を入力側の照合では区別し，中核へは共通の役割だけを渡す
 * 計算量：O(1)
 * @param Role 場の役割
 * @return 中核の子の役割
 */
Core::ChildRole GrammarProfile::CommonChildRole(const Core::ChildRole Role) {
	// 対象の種類に従う処理の選択
	switch(Role) {
	case Core::ChildRole::ArgField:
	case Core::ChildRole::BlockField:
	case Core::ChildRole::DirectiveField:
	case Core::ChildRole::ScopeField:
		// 共通の役割を持たない場の返戻
		return Core::ChildRole::Other;
	case Core::ChildRole::SizeField:
		// 配列の寸法の場の返戻
		return Core::ChildRole::Length;
	case Core::ChildRole::FunctionField:
	case Core::ChildRole::MethodField:
		// 呼ぶ関数の場の返戻
		return Core::ChildRole::Function;
	case Core::ChildRole::ObjectField:
	case Core::ChildRole::ReceiverField:
		// 受手の場の返戻
		return Core::ChildRole::Object;
	case Core::ChildRole::OperatorField:
		// 演算子の場の返戻
		return Core::ChildRole::Operator;
	case Core::ChildRole::ParamsField:
		// 仮引数の場の返戻
		return Core::ChildRole::Params;
	default:
		// 其の儘中核の役割である場の返戻
		return Role;
	}
}

/**
 * 文字列の開きの前の接頭辞の取得関数（整形仕様 4.2）
 * 計算量：O(1)
 * @param Lang 対象言語
 * @return 引用符の前に置いて文字列の種類を定める接頭辞（無ければ空）
 */
std::span<const std::string_view> GrammarProfile::GetStrPrefixes(const ::Lang Lang) {
	// 文字列接頭辞の文法別の定義
	static constexpr std::string_view CFamily[] = { "L", "LR", "R", "U", "UR", "u", "u8", "u8R", "uR" };
	static constexpr std::string_view CSharp[] = { "$", "$$", "$@", "@", "@$" }, Rust[] = { "b", "br", "c", "cr", "r" };
	static constexpr std::string_view Python[] = {
		"B",
		"BR",
		"Br",
		"F",
		"FR",
		"Fr",
		"R",
		"RB",
		"RF",
		"RT",
		"Rb",
		"Rf",
		"Rt",
		"T",
		"TR",
		"Tr",
		"U",
		"b",
		"bR",
		"br",
		"f",
		"fR",
		"fr",
		"r",
		"rB",
		"rF",
		"rT",
		"rb",
		"rf",
		"rt",
		"t",
		"tR",
		"tr",
		"u"
	};
	// C 系の接頭辞の返戻
	if(Lang.IsCFamily()) return CFamily;
	// C# の接頭辞の返戻
	if(Lang == Lang::CSharp) return CSharp;
	// Rust の接頭辞の返戻
	if(Lang == Lang::Rust) return Rust;
	// Python の接頭辞と，接頭辞を持たない言語の返戻
	return Lang == Lang::Python ? std::span<const std::string_view>(Python) : std::span<const std::string_view>();
}

/**
 * ノード名の種別の検索関数
 * 計算量：ノード名長 L と候補数 K に対し O(K L)
 * @param Name 文法のノード名
 * @param IsNamed 構文のノードか（偽はトークン）
 * @return 種別（文法に無ければ無い）
 */
const Core::KindDef *GrammarProfile::Find(const std::string_view Name, const bool IsNamed) const {
	// ノード名に対応する文法記号の検索
	const TSSymbol Symbol =
	ts_language_symbol_for_name(Lang.GetTreeSitterLang(), Name.data(), static_cast<uint32_t>(Name.size()), IsNamed);
	// 種別の返戻
	return !Symbol ? nullptr : KindsBySymbol.empty() ? &Kinds[Symbol] : KindsBySymbol[Symbol];
}

/**
 * 文法の場に対応する中核の子の役割のマーカーの取得関数
 * 計算量：O(1)
 * @param Field 場の識別子
 * @return 中核の子の役割のマーカー（場でなければ０）
 */
uint64_t GrammarProfile::GetFieldBits(const TSFieldId Field) const {
	// 文法の場に対応する子の役割の取得
	const Core::ChildRole Role = GetChildRoleOf(Field);
	// 場でない子と，中核の子の役割のマーカーの返戻
	return Role == Core::ChildRole::None ? 0 : uint64_t{ 1 } << static_cast<uint8_t>(CommonChildRole(Role));
}

/**
 * 文法に在る場の役割の中核のマーカーの取得関数
 * 計算量：O(1)
 * @param Role 入力側で区別する場の役割
 * @return 文法に場が在れば中核の子の役割のビット，無ければ０
 */
uint64_t GrammarProfile::GetFieldBits(const Core::ChildRole Role) const {
	const std::span<const TSFieldId> Fields = GetFieldsOf(Role);
	// 文法に在る場だけの中核のマーカーの返戻
	return Fields.empty() ? 0 : GetFieldBits(Fields.front());
}

/**
 * 子の評価時機の固定契約の取得関数
 * 式・宣言・制御構文の入口と作成時の評価位置を同じ契約へ投影する
 * 計算量：固定の文法定義の数 K に対し O(K)（記号表の構築時だけ）
 * @param Lang 対象言語
 * @param Name 文法のノード名
 * @param Roles 投影済の構文の役割
 * @return 名前付の子の評価時機と，既定と異なる位置
 * @throws std::bad_alloc 固定の位置の列を確保出来ない場合
 * @throws std::length_error 固定の位置の列が要素数上限を超える場合
 */
Core::EvaluationDef GrammarProfile::GetEvaluationDef(
	const ::Lang Lang,
	const std::string_view Name,
	const Core::RoleSet &Roles
) {
	using Class = Core::EvaluationClass;
	using Field = Core::ChildRole;
	// 同じ名前でも文法に依って評価が異なる為，言語毎の登録を此処に閉じる
	struct Entry {
		::Lang::ID Lang;
		std::string_view Name;
		Class Children;
	};
	static constexpr Entry Entries[] {
		{ ::Lang::C, "abstract_array_declarator", Class::Conditional },
		{ ::Lang::C, "alignof_expression", Class::Unevaluated },
		{ ::Lang::C, "array_declarator", Class::Conditional },
		{ ::Lang::C, "attribute", Class::Unevaluated },
		{ ::Lang::C, "attribute_declaration", Class::Unevaluated },
		{ ::Lang::C, "attribute_specifier", Class::Unevaluated },
		{ ::Lang::C, "compound_literal_expression", Class::Required },
		{ ::Lang::C, "extension_expression", Class::Required },
		{ ::Lang::C, "generic_expression", Class::Unknown },
		{ ::Lang::C, "initializer_list", Class::Required },
		{ ::Lang::C, "initializer_pair", Class::Required },
		{ ::Lang::C, "offsetof_expression", Class::Unevaluated },
		{ ::Lang::C, "pointer_expression", Class::Required },
		{ ::Lang::C, "sizeof_expression", Class::Unevaluated },
		{ ::Lang::C, "type_descriptor", Class::Conditional },
		{ ::Lang::C, "update_expression", Class::Required },
		{ ::Lang::Cpp, "alignof_expression", Class::Unevaluated },
		{ ::Lang::Cpp, "attribute", Class::Unevaluated },
		{ ::Lang::Cpp, "attribute_declaration", Class::Unevaluated },
		{ ::Lang::Cpp, "attribute_specifier", Class::Unevaluated },
		{ ::Lang::Cpp, "compound_literal_expression", Class::Required },
		{ ::Lang::Cpp, "decltype", Class::Unevaluated },
		{ ::Lang::Cpp, "delete_expression", Class::Required },
		{ ::Lang::Cpp, "extension_expression", Class::Required },
		{ ::Lang::Cpp, "field_initializer", Class::Unknown },
		{ ::Lang::Cpp, "field_initializer_list", Class::Unknown },
		{ ::Lang::Cpp, "initializer_pair", Class::Required },
		{ ::Lang::Cpp, "new_declarator", Class::Required },
		{ ::Lang::Cpp, "new_expression", Class::Required },
		{ ::Lang::Cpp, "noexcept", Class::Unevaluated },
		{ ::Lang::Cpp, "offsetof_expression", Class::Unevaluated },
		{ ::Lang::Cpp, "parameter_pack_expansion", Class::Required },
		{ ::Lang::Cpp, "pointer_expression", Class::Required },
		{ ::Lang::Cpp, "requires_clause", Class::Unevaluated },
		{ ::Lang::Cpp, "requires_expression", Class::Unevaluated },
		{ ::Lang::Cpp, "sizeof_expression", Class::Unevaluated },
		{ ::Lang::Cpp, "static_assert_declaration", Class::Unevaluated },
		{ ::Lang::Cpp, "template_argument_list", Class::Unevaluated },
		{ ::Lang::Cpp, "type_descriptor", Class::Unevaluated },
		{ ::Lang::Cpp, "update_expression", Class::Required },
		{ ::Lang::CSharp, "anonymous_object_creation_expression", Class::Required },
		{ ::Lang::CSharp, "argument", Class::Required },
		{ ::Lang::CSharp, "array_creation_expression", Class::Required },
		{ ::Lang::CSharp, "array_rank_specifier", Class::Required },
		{ ::Lang::CSharp, "array_type", Class::Required },
		{ ::Lang::CSharp, "as_expression", Class::Required },
		{ ::Lang::CSharp, "attribute", Class::Unevaluated },
		{ ::Lang::CSharp, "attribute_argument", Class::Unevaluated },
		{ ::Lang::CSharp, "attribute_argument_list", Class::Unevaluated },
		{ ::Lang::CSharp, "await_expression", Class::Required },
		{ ::Lang::CSharp, "checked_expression", Class::Required },
		{ ::Lang::CSharp, "collection_element", Class::Required },
		{ ::Lang::CSharp, "collection_expression", Class::Required },
		{ ::Lang::CSharp, "default_expression", Class::Unevaluated },
		{ ::Lang::CSharp, "element_access_expression", Class::Required },
		{ ::Lang::CSharp, "element_binding_expression", Class::Required },
		{ ::Lang::CSharp, "expression_element", Class::Required },
		{ ::Lang::CSharp, "from_clause", Class::Required },
		{ ::Lang::CSharp, "implicit_array_creation_expression", Class::Required },
		{ ::Lang::CSharp, "implicit_object_creation_expression", Class::Required },
		{ ::Lang::CSharp, "implicit_stackalloc_expression", Class::Required },
		{ ::Lang::CSharp, "interpolated_string_expression", Class::Required },
		{ ::Lang::CSharp, "interpolation", Class::Required },
		{ ::Lang::CSharp, "interpolation_alignment_clause", Class::Required },
		{ ::Lang::CSharp, "is_expression", Class::Required },
		{ ::Lang::CSharp, "is_pattern_expression", Class::Required },
		{ ::Lang::CSharp, "join_clause", Class::Deferred },
		{ ::Lang::CSharp, "local_declaration_statement", Class::Required },
		{ ::Lang::CSharp, "makeref_expression", Class::Required },
		{ ::Lang::CSharp, "object_creation_expression", Class::Required },
		{ ::Lang::CSharp, "postfix_unary_expression", Class::Required },
		{ ::Lang::CSharp, "prefix_unary_expression", Class::Required },
		{ ::Lang::CSharp, "ref_expression", Class::Required },
		{ ::Lang::CSharp, "sizeof_expression", Class::Unevaluated },
		{ ::Lang::CSharp, "spread_element", Class::Required },
		{ ::Lang::CSharp, "stackalloc_expression", Class::Required },
		{ ::Lang::CSharp, "tuple_expression", Class::Required },
		{ ::Lang::CSharp, "typeof_expression", Class::Unevaluated },
		{ ::Lang::CSharp, "variable_declaration", Class::Required },
		{ ::Lang::CSharp, "with_expression", Class::Required },
		{ ::Lang::Java, "annotation", Class::Unevaluated },
		{ ::Lang::Java, "annotation_argument_list", Class::Unevaluated },
		{ ::Lang::Java, "array_access", Class::Required },
		{ ::Lang::Java, "array_creation_expression", Class::Required },
		{ ::Lang::Java, "array_initializer", Class::Required },
		{ ::Lang::Java, "assert_statement", Class::Unknown },
		{ ::Lang::Java, "class_body", Class::Required },
		{ ::Lang::Java, "class_literal", Class::Unevaluated },
		{ ::Lang::Java, "dimensions_expr", Class::Required },
		{ ::Lang::Java, "field_declaration", Class::Required },
		{ ::Lang::Java, "marker_annotation", Class::Unevaluated },
		{ ::Lang::Java, "object_creation_expression", Class::Required },
		{ ::Lang::Java, "resource", Class::Required },
		{ ::Lang::Java, "resource_specification", Class::Required },
		{ ::Lang::Java, "unary_expression", Class::Required },
		{ ::Lang::Java, "update_expression", Class::Required },
		{ ::Lang::Go, "assignment_statement", Class::Required },
		{ ::Lang::Go, "communication_case", Class::Conditional },
		{ ::Lang::Go, "composite_literal", Class::Required },
		{ ::Lang::Go, "expression_list", Class::Required },
		{ ::Lang::Go, "for_clause", Class::Conditional },
		{ ::Lang::Go, "keyed_element", Class::Required },
		{ ::Lang::Go, "literal_element", Class::Required },
		{ ::Lang::Go, "literal_value", Class::Required },
		{ ::Lang::Go, "range_clause", Class::Required },
		{ ::Lang::Go, "receive_statement", Class::Conditional },
		{ ::Lang::Go, "select_statement", Class::Required },
		{ ::Lang::Go, "selector_expression", Class::Required },
		{ ::Lang::Go, "send_statement", Class::Required },
		{ ::Lang::Go, "short_var_declaration", Class::Required },
		{ ::Lang::Go, "slice_expression", Class::Required },
		{ ::Lang::Go, "type_arguments", Class::Unevaluated },
		{ ::Lang::Go, "type_assertion_expression", Class::Required },
		{ ::Lang::Go, "type_conversion_expression", Class::Required },
		{ ::Lang::Go, "var_declaration", Class::Required },
		{ ::Lang::Go, "var_spec", Class::Required },
		{ ::Lang::Go, "var_spec_list", Class::Required },
		{ ::Lang::Go, "variadic_argument", Class::Required },
		{ ::Lang::Rust, "array_expression", Class::Required },
		{ ::Lang::Rust, "attribute", Class::Unevaluated },
		{ ::Lang::Rust, "attribute_item", Class::Unevaluated },
		{ ::Lang::Rust, "await_expression", Class::Required },
		{ ::Lang::Rust, "base_field_initializer", Class::Required },
		{ ::Lang::Rust, "const_block", Class::Unevaluated },
		{ ::Lang::Rust, "const_item", Class::Unevaluated },
		{ ::Lang::Rust, "const_parameter", Class::Unevaluated },
		{ ::Lang::Rust, "field_initializer", Class::Required },
		{ ::Lang::Rust, "field_initializer_list", Class::Required },
		{ ::Lang::Rust, "inner_attribute_item", Class::Unevaluated },
		{ ::Lang::Rust, "let_condition", Class::Required },
		{ ::Lang::Rust, "macro_invocation", Class::Unknown },
		{ ::Lang::Rust, "match_pattern", Class::Conditional },
		{ ::Lang::Rust, "range_expression", Class::Required },
		{ ::Lang::Rust, "reference_expression", Class::Required },
		{ ::Lang::Rust, "struct_expression", Class::Required },
		{ ::Lang::Rust, "tuple_expression", Class::Required },
		{ ::Lang::Rust, "type_arguments", Class::Unevaluated },
		{ ::Lang::Kotlin, "annotation", Class::Unevaluated },
		{ ::Lang::Kotlin, "as_expression", Class::Required },
		{ ::Lang::Kotlin, "check_expression", Class::Required },
		{ ::Lang::Kotlin, "class_body", Class::Required },
		{ ::Lang::Kotlin, "collection_literal", Class::Required },
		{ ::Lang::Kotlin, "constructor_delegation_call", Class::Required },
		{ ::Lang::Kotlin, "constructor_invocation", Class::Required },
		{ ::Lang::Kotlin, "delegation_specifier", Class::Required },
		{ ::Lang::Kotlin, "explicit_delegation", Class::Required },
		{ ::Lang::Kotlin, "indexing_expression", Class::Required },
		{ ::Lang::Kotlin, "interpolated_expression", Class::Required },
		{ ::Lang::Kotlin, "navigation_expression", Class::Required },
		{ ::Lang::Kotlin, "navigation_suffix", Class::Required },
		{ ::Lang::Kotlin, "object_literal", Class::Required },
		{ ::Lang::Kotlin, "postfix_expression", Class::Required },
		{ ::Lang::Kotlin, "prefix_expression", Class::Required },
		{ ::Lang::Kotlin, "property_declaration", Class::Required },
		{ ::Lang::Kotlin, "property_delegate", Class::Required },
		{ ::Lang::Kotlin, "range_expression", Class::Required },
		{ ::Lang::Kotlin, "spread_expression", Class::Required },
		{ ::Lang::Kotlin, "string_literal", Class::Required },
		{ ::Lang::Kotlin, "type_arguments", Class::Unevaluated },
		{ ::Lang::Kotlin, "type_constraints", Class::Unevaluated },
		{ ::Lang::Kotlin, "type_projection", Class::Unevaluated },
		{ ::Lang::Kotlin, "value_argument", Class::Required },
		{ ::Lang::Kotlin, "when_subject", Class::Required },
		{ ::Lang::Swift, "array_literal", Class::Required },
		{ ::Lang::Swift, "as_expression", Class::Required },
		{ ::Lang::Swift, "attribute", Class::Unknown },
		{ ::Lang::Swift, "await_expression", Class::Required },
		{ ::Lang::Swift, "check_expression", Class::Required },
		{ ::Lang::Swift, "constructor_expression", Class::Required },
		{ ::Lang::Swift, "constructor_suffix", Class::Required },
		{ ::Lang::Swift, "consume_expression", Class::Required },
		{ ::Lang::Swift, "dictionary_literal", Class::Required },
		{ ::Lang::Swift, "directly_assignable_expression", Class::Required },
		{ ::Lang::Swift, "interpolated_expression", Class::Required },
		{ ::Lang::Swift, "key_path_string_expression", Class::Unevaluated },
		{ ::Lang::Swift, "line_string_literal", Class::Required },
		{ ::Lang::Swift, "multi_line_string_literal", Class::Required },
		{ ::Lang::Swift, "navigation_expression", Class::Required },
		{ ::Lang::Swift, "navigation_suffix", Class::Required },
		{ ::Lang::Swift, "open_end_range_expression", Class::Required },
		{ ::Lang::Swift, "open_start_range_expression", Class::Required },
		{ ::Lang::Swift, "postfix_expression", Class::Required },
		{ ::Lang::Swift, "prefix_expression", Class::Required },
		{ ::Lang::Swift, "range_expression", Class::Required },
		{ ::Lang::Swift, "raw_string_literal", Class::Required },
		{ ::Lang::Swift, "selector_expression", Class::Unevaluated },
		{ ::Lang::Swift, "tuple_expression", Class::Required },
		{ ::Lang::Swift, "type_annotation", Class::Unevaluated },
		{ ::Lang::Swift, "type_arguments", Class::Unevaluated },
		{ ::Lang::Swift, "unsafe_expression", Class::Required },
		{ ::Lang::Swift, "value_argument", Class::Required },
		{ ::Lang::Swift, "value_parameter_pack", Class::Unknown },
		{ ::Lang::PHP, "argument", Class::Required },
		{ ::Lang::PHP, "array_creation_expression", Class::Required },
		{ ::Lang::PHP, "array_element_initializer", Class::Required },
		{ ::Lang::PHP, "attribute", Class::Unevaluated },
		{ ::Lang::PHP, "attribute_group", Class::Unevaluated },
		{ ::Lang::PHP, "attribute_list", Class::Unevaluated },
		{ ::Lang::PHP, "cast_expression", Class::Required },
		{ ::Lang::PHP, "clone_expression", Class::Required },
		{ ::Lang::PHP, "dynamic_variable_name", Class::Required },
		{ ::Lang::PHP, "echo_statement", Class::Required },
		{ ::Lang::PHP, "encapsed_string", Class::Required },
		{ ::Lang::PHP, "error_suppression_expression", Class::Required },
		{ ::Lang::PHP, "function_static_declaration", Class::Conditional },
		{ ::Lang::PHP, "include_expression", Class::Required },
		{ ::Lang::PHP, "include_once_expression", Class::Required },
		{ ::Lang::PHP, "object_creation_expression", Class::Required },
		{ ::Lang::PHP, "pair", Class::Required },
		{ ::Lang::PHP, "print_intrinsic", Class::Required },
		{ ::Lang::PHP, "reference_assignment_expression", Class::Required },
		{ ::Lang::PHP, "require_expression", Class::Required },
		{ ::Lang::PHP, "require_once_expression", Class::Required },
		{ ::Lang::PHP, "scoped_property_access_expression", Class::Required },
		{ ::Lang::PHP, "sequence_expression", Class::Required },
		{ ::Lang::PHP, "shell_command_expression", Class::Required },
		{ ::Lang::PHP, "static_variable_declaration", Class::Conditional },
		{ ::Lang::PHP, "unary_op_expression", Class::Required },
		{ ::Lang::PHP, "unset_statement", Class::Required },
		{ ::Lang::PHP, "update_expression", Class::Required },
		{ ::Lang::PHP, "variadic_unpacking", Class::Required },
		{ ::Lang::JavaScript, "array_pattern", Class::Required },
		{ ::Lang::JavaScript, "await_expression", Class::Required },
		{ ::Lang::JavaScript, "class", Class::Required },
		{ ::Lang::JavaScript, "class_body", Class::Required },
		{ ::Lang::JavaScript, "class_declaration", Class::Required },
		{ ::Lang::JavaScript, "class_heritage", Class::Required },
		{ ::Lang::JavaScript, "class_static_block", Class::Required },
		{ ::Lang::JavaScript, "computed_property_name", Class::Required },
		{ ::Lang::JavaScript, "decorator", Class::Unknown },
		{ ::Lang::JavaScript, "field_definition", Class::Deferred },
		{ ::Lang::JavaScript, "lexical_declaration", Class::Required },
		{ ::Lang::JavaScript, "method_definition", Class::Deferred },
		{ ::Lang::JavaScript, "new_expression", Class::Required },
		{ ::Lang::JavaScript, "object", Class::Required },
		{ ::Lang::JavaScript, "object_pattern", Class::Required },
		{ ::Lang::JavaScript, "pair", Class::Required },
		{ ::Lang::JavaScript, "pair_pattern", Class::Required },
		{ ::Lang::JavaScript, "sequence_expression", Class::Required },
		{ ::Lang::JavaScript, "spread_element", Class::Required },
		{ ::Lang::JavaScript, "template_string", Class::Required },
		{ ::Lang::JavaScript, "template_substitution", Class::Required },
		{ ::Lang::JavaScript, "update_expression", Class::Required },
		{ ::Lang::TypeScript, "abstract_class_declaration", Class::Required },
		{ ::Lang::TypeScript, "array_pattern", Class::Required },
		{ ::Lang::TypeScript, "as_expression", Class::Required },
		{ ::Lang::TypeScript, "await_expression", Class::Required },
		{ ::Lang::TypeScript, "class", Class::Required },
		{ ::Lang::TypeScript, "class_body", Class::Required },
		{ ::Lang::TypeScript, "class_declaration", Class::Required },
		{ ::Lang::TypeScript, "class_heritage", Class::Required },
		{ ::Lang::TypeScript, "class_static_block", Class::Required },
		{ ::Lang::TypeScript, "computed_property_name", Class::Required },
		{ ::Lang::TypeScript, "decorator", Class::Unknown },
		{ ::Lang::TypeScript, "enum_assignment", Class::Required },
		{ ::Lang::TypeScript, "enum_body", Class::Required },
		{ ::Lang::TypeScript, "enum_declaration", Class::Required },
		{ ::Lang::TypeScript, "extends_clause", Class::Required },
		{ ::Lang::TypeScript, "implements_clause", Class::Unevaluated },
		{ ::Lang::TypeScript, "instantiation_expression", Class::Required },
		{ ::Lang::TypeScript, "lexical_declaration", Class::Required },
		{ ::Lang::TypeScript, "method_definition", Class::Deferred },
		{ ::Lang::TypeScript, "new_expression", Class::Required },
		{ ::Lang::TypeScript, "non_null_expression", Class::Required },
		{ ::Lang::TypeScript, "object", Class::Required },
		{ ::Lang::TypeScript, "object_pattern", Class::Required },
		{ ::Lang::TypeScript, "pair", Class::Required },
		{ ::Lang::TypeScript, "pair_pattern", Class::Required },
		{ ::Lang::TypeScript, "public_field_definition", Class::Deferred },
		{ ::Lang::TypeScript, "satisfies_expression", Class::Required },
		{ ::Lang::TypeScript, "sequence_expression", Class::Required },
		{ ::Lang::TypeScript, "spread_element", Class::Required },
		{ ::Lang::TypeScript, "template_string", Class::Required },
		{ ::Lang::TypeScript, "template_substitution", Class::Required },
		{ ::Lang::TypeScript, "type_annotation", Class::Unevaluated },
		{ ::Lang::TypeScript, "type_arguments", Class::Unevaluated },
		{ ::Lang::TypeScript, "type_assertion", Class::Required },
		{ ::Lang::TypeScript, "type_query", Class::Unevaluated },
		{ ::Lang::TypeScript, "update_expression", Class::Required },
		{ ::Lang::Ruby, "bare_string", Class::Required },
		{ ::Lang::Ruby, "bare_symbol", Class::Required },
		{ ::Lang::Ruby, "block_argument", Class::Required },
		{ ::Lang::Ruby, "chained_string", Class::Required },
		{ ::Lang::Ruby, "class", Class::Required },
		{ ::Lang::Ruby, "do_block", Class::Deferred },
		{ ::Lang::Ruby, "expression_reference_pattern", Class::Required },
		{ ::Lang::Ruby, "hash", Class::Required },
		{ ::Lang::Ruby, "hash_splat_argument", Class::Required },
		{ ::Lang::Ruby, "interpolation", Class::Required },
		{ ::Lang::Ruby, "match_pattern", Class::Conditional },
		{ ::Lang::Ruby, "module", Class::Required },
		{ ::Lang::Ruby, "pair", Class::Required },
		{ ::Lang::Ruby, "range", Class::Required },
		{ ::Lang::Ruby, "right_assignment_list", Class::Required },
		{ ::Lang::Ruby, "singleton_class", Class::Required },
		{ ::Lang::Ruby, "splat_argument", Class::Required },
		{ ::Lang::Ruby, "string", Class::Required },
		{ ::Lang::Ruby, "string_array", Class::Required },
		{ ::Lang::Ruby, "subshell", Class::Required },
		{ ::Lang::Ruby, "superclass", Class::Required },
		{ ::Lang::Ruby, "symbol_array", Class::Required },
		{ ::Lang::Ruby, "test_pattern", Class::Conditional },
		{ ::Lang::Python, "as_pattern", Class::Required },
		{ ::Lang::Python, "assert_statement", Class::Conditional },
		{ ::Lang::Python, "await", Class::Required },
		{ ::Lang::Python, "class_definition", Class::Required },
		{ ::Lang::Python, "concatenated_string", Class::Required },
		{ ::Lang::Python, "delete_statement", Class::Required },
		{ ::Lang::Python, "dictionary_splat", Class::Required },
		{ ::Lang::Python, "expression_list", Class::Required },
		{ ::Lang::Python, "format_expression", Class::Required },
		{ ::Lang::Python, "format_specifier", Class::Required },
		{ ::Lang::Python, "interpolation", Class::Required },
		{ ::Lang::Python, "keyword_argument", Class::Required },
		{ ::Lang::Python, "list_splat", Class::Required },
		{ ::Lang::Python, "named_expression", Class::Required },
		{ ::Lang::Python, "parenthesized_list_splat", Class::Required },
		{ ::Lang::Python, "slice", Class::Required },
		{ ::Lang::Python, "string", Class::Required },
		{ ::Lang::Python, "tuple_expression", Class::Required },
		{ ::Lang::Python, "type", Class::Unknown },
		{ ::Lang::Python, "type_alias_statement", Class::Unevaluated },
		{ ::Lang::Python, "type_parameter", Class::Unevaluated },
		{ ::Lang::Python, "with_clause", Class::Conditional },
		{ ::Lang::Python, "with_item", Class::Required }
	};
	Core::EvaluationDef Result;
	const auto Has = [&Roles](const Core::Role Role) -> bool {
		// 固定の役割の有無の返戻
		return Roles.test(static_cast<size_t>(Role));
	};
	using Core::Role;
	Result.Children = Class::Conditional;
	if(
		Has(Role::ArrayLiteral) || Has(Role::AssignmentExpr) || Has(Role::AssignmentOperation) || Has(Role::BinaryExpr) ||
		Has(Role::CallNode) || Has(Role::CastExpr) || Has(Role::CommaExpr) || Has(Role::ConditionalAccessExpr) ||
		Has(Role::EagerEvaluation) || Has(Role::FieldExpr) || Has(Role::InitDeclarator) || Has(Role::InitializerExpr) ||
		Has(Role::MemberExpr) || Has(Role::Subscript) || Has(Role::SubscriptExprNode) || Has(Role::TypedDecl) ||
		Has(Role::UnaryOperation) || Has(Role::VarDeclaratorItem)
	) Result.Children = Class::Required;
	Result.IsContainer =
	Has(Role::ArgSeq) || Has(Role::CallArgs) || Has(Role::CallSuffix) || Has(Role::CondInitializer) || Has(Role::CondParenWrap) ||
	Has(Role::CreationInputs) || Has(Role::CreationVal) || Has(Role::GroupingParen) || Has(Role::ValArgs);
	if(Result.IsContainer) Result.Children = Class::Required;
	if(Has(Role::FlowBoundary) || Has(Role::LambdaLiteral) || Has(Role::InvocationBody)) Result.Children = Class::Deferred;
	if(Has(Role::ValCategoryType)) Result.Children = Class::Unevaluated;
	for(const Entry &Item : Entries) if(Lang == Item.Lang && Name == Item.Name) {
		Result.Children = Item.Children;
		break;
	}
	if(!Lang.IsCFamily() && (Name == "argument_list" || Name == "arguments" || Name == "value_arguments")) {
		Result.Children = Class::Required;
	}
	// 型と実行式を並べるラッパーでは型の側へ降りない
	if(Result.Children == Class::Required) {
		Result.Inputs = { { Field::Type, 0, Class::Unevaluated }, { Field::ReturnType, 0, Class::Unevaluated } };
	}
	if(Lang == ::Lang::TypeScript && (Name == "as_expression" || Name == "satisfies_expression")) {
		Result.Children = Class::Unevaluated;
		Result.Inputs = { { Field::None, 0, Class::Required } };
	}
	if(Lang == ::Lang::TypeScript && Name == "type_assertion") {
		Result.Children = Class::Required;
		Result.Inputs = { { Field::None, 0, Class::Unevaluated } };
	}
	if(Lang == ::Lang::CSharp && Name == "query_expression") {
		Result.Children = Class::Deferred;
		Result.Inputs =
		{ { Field::None, 0, Class::Required }, { Field::None, 0, Class::Required, Field::None, Role::EvaluationEntry } };
	}
	if(Lang == ::Lang::Rust && Name == "let_declaration") {
		Result.Children = Class::Required;
		Result.Inputs = {
			{ Field::Alternative, 0, Class::Conditional },
			{ Field::Pattern, 0, Class::Unevaluated },
			{ Field::Type, 0, Class::Unevaluated }
		};
	}
	// 初期値だけを宣言時に評価し，計算プロパティと監視の本体は含めない
	if(Lang == ::Lang::Swift && Name == "property_declaration") Result.Inputs = { { Field::Val, 0, Class::Required } };
	// assert の有効性が到達可能性を変えるのは，内側に終端が在る場合だけ
	if(Lang == ::Lang::Python && Name == "assert_statement") Result.MaySkipInputs = true;
	if(Lang == ::Lang::Python && Name == "comparison_operator") {
		Result.Children = Class::Conditional;
		Result.Inputs = { { Field::None, 0, Class::Required }, { Field::None, 1, Class::Required } };
	}
	// 内包表記とジェネレータで入口に必須なのは，最初の反復対象だけ
	if(
		Lang == ::Lang::Python && (
			Name == "dictionary_comprehension" || Name == "generator_expression" || Name == "list_comprehension" ||
			Name == "set_comprehension"
		)
	) {
		Result.Children = Name == "generator_expression" ? Class::Deferred : Class::Conditional;
		Result.Inputs = { { Field::None, 1, Class::Required } };
	}
	if(Lang == ::Lang::Python && Name == "for_in_clause") {
		Result.Children = Class::Conditional;
		Result.Inputs = { { Field::Right, 0, Class::Required } };
	}
	if(Lang == ::Lang::Rust && Name == "array_expression") Result.Inputs.push_back({ Field::Length, 0, Class::Unevaluated });
	if(Lang == ::Lang::Cpp && Name == "new_expression") {
		Result.Inputs.push_back({ Field::Args, 0, Class::Unknown, Field::Placement });
		Result.Inputs.insert(
			Result.Inputs.begin(),
			{ Field::Type, 0, Class::Unknown, Field::None, std::nullopt, {}, {}, Role::PrimitiveType }
		);
	}
	// 配列型の次元だけは，構築時に大きさを評価する
	if(Lang == ::Lang::CSharp && Name == "array_creation_expression") Result.Inputs.clear();
	// 分解代入の既定値は欠けた値だけに適用する
	if(Lang.IsJsTs()) {
		if(Name == "assignment_pattern" || Name == "object_assignment_pattern") Result.Children = Class::Conditional;
		if(Name == "switch_statement") Result.ConditionField = Field::Val;
	}
	if(
		Lang == ::Lang::CSharp && Name == "switch_statement" || Lang == ::Lang::Go && Name == "expression_switch_statement" ||
		Lang == ::Lang::Rust && Name == "match_expression" || Lang == ::Lang::Ruby && Name == "case"
	) Result.ConditionField = Field::Val;
	if(Lang == ::Lang::Swift && Name == "switch_statement" || Lang == ::Lang::Python && Name == "match_statement") {
		Result.ConditionField = Field::EvaluationSubject;
	}
	if(Lang == ::Lang::CSharp && Name == "switch_expression" || Lang == ::Lang::Kotlin && Name == "while_statement") {
		Result.ConditionIndex = 0;
	}
	Result.IsOrdered = !Lang.IsCFamily() || Lang == ::Lang::Cpp && Name == "new_expression";
	if(Lang == ::Lang::Cpp && Name == "new_expression" || Lang == ::Lang::Java && Name == "object_creation_expression") {
		Result.ThrowBeforeField = Field::Args;
	}
	// 作成時の初期値は本体と分離し，呼出時の仮引数を此処へ含めない
	if(Has(Role::FlowBoundary) && Result.Children == Class::Deferred) {
		Result.Inputs.push_back({ Field::None, 0, Class::Required, Field::None, Role::CreationInputs });
	}
	if(Has(Role::CreationInputs)) {
		Result.Children = Class::Deferred;
		Result.Inputs = { { Field::None, 0, Class::Required, Field::None, Role::CreationVal } };
	}
	if(Has(Role::CreationVal)) {
		Result.Children = Class::Unevaluated;
		Result.Inputs = { { Has(Role::CaptureInitializer) ? Field::Right : Field::Val, 0, Class::Required } };
	}
	if(Has(Role::CallNode)) Result.Inputs.push_back({ Field::Body, 0, Class::Deferred });
	// 制御構文は入口だけが必須で，枝・反復・捕捉の本体は条件付である
	if(Has(Role::If) || Has(Role::ArmSwitch)) {
		Result.HasEntry = true;
		Result.Children = Class::Conditional;
		Result.Inputs = { { Field::Initializer, 0, Class::Required }, { Result.ConditionField, 0, Class::Required } };
		if(Result.ConditionIndex) Result.Inputs.push_back({ Field::None, *Result.ConditionIndex, Class::Required });
		Result.Inputs.push_back({ Field::None, 0, Class::Required, Field::None, Role::EvaluationCondition });
	}
	if(Has(Role::Loop)) {
		Result.HasEntry = true;
		Result.Children = Class::Required;
		Result.Inputs = {
			{ Field::Alternative, 0, Class::Conditional },
			{ Field::Body, 0, Class::Conditional },
			{ Field::Increment, 0, Class::Conditional },
			{ Field::Update, 0, Class::Conditional }
		};
		if(Has(Role::PostCondLoop)) Result.Inputs.push_back({ Result.ConditionField, 0, Class::Conditional });
		else if(Has(Role::PostfixLoop)) {
			Result.Inputs.push_back(
				{ Result.ConditionField, 0, Class::Conditional, Field::None, std::nullopt, {}, {}, std::nullopt, Role::ExplicitKeywordBody }
			);
		}
	}
	if(Has(Role::SingleBodyStmt)) {
		Result.HasEntry = true;
		Result.Children = Class::Required;
		Result.Inputs = { { Field::Body, 0, Class::Conditional } };
	}
	if(Has(Role::CatchClause)) Result.Inputs.push_back({ Field::Val, 0, Class::Required }); // 捕捉条件の値は型の照合前に評価し，静的な型・束縛と本体を含めない
	Result.IsOrdered = Result.IsOrdered || Has(Role::CommaExpr) || Has(Role::CondInitializer) || Has(Role::CondParenWrap) ||
	Has(Role::EagerEvaluation) || Has(Role::CreationInputs) && !Has(Role::CaptureList);
	if(Lang == ::Lang::C && (Name == "array_declarator" || Name == "abstract_array_declarator")) {
		Result.Inputs = { { Field::Length, 0, Class::Unknown, Field::None, std::nullopt, {}, {}, Role::NumLiteral } };
	}
	// 型に含まれる可変長の次元だけは，非評価を確定出来ない
	if(Lang == ::Lang::C && Name == "sizeof_expression") Result.Inputs = { { Field::Type, 0, Class::Conditional } };
	if(Lang == ::Lang::Cpp && Name == "parameter_pack_expansion") Result.MaySkipInputs = true;
	if(Lang == ::Lang::Cpp && Name == "call_expression") {
		Result.UnevaluatedHead = "noexcept";
		Result.UnknownHead = "typeid";
	}
	if(Lang == ::Lang::CSharp && Name == "join_clause") {
		// 型の有無に依らず in の直後が結合対象である
		Result.Inputs = { { Field::None, 0, Class::Required, Field::None, std::nullopt, {}, "in" } };
	}
	if(Lang == ::Lang::Go) {
		if(Name == "select_statement") Result.Inputs = { { Field::None, 0, Class::Required, Field::None, Role::CommunicationCase } };
		if(Name == "communication_case") Result.Inputs = { { Field::Communication, 0, Class::Required } };
		if(Name == "receive_statement" || Name == "range_clause") {
			Result.Children = Class::Conditional;
			Result.Inputs = { { Field::Right, 0, Class::Required } };
		}
		if(Name == "for_clause") Result.Inputs = { { Field::Initializer, 0, Class::Required }, { Field::Cond, 0, Class::Required } };
	}
	if(Lang.IsJsTs() && (Name == "field_definition" || Name == "public_field_definition")) {
		Result.Inputs =
		{ { Field::Name, 0, Class::Required }, { Field::Val, 0, Class::Required, Field::None, std::nullopt, "static" } };
	}
	if(Lang.IsJsTs() && Name == "method_definition") Result.Inputs = { { Field::Name, 0, Class::Required } };
	if(Lang == ::Lang::Ruby && (Name == "test_pattern" || Name == "match_pattern")) {
		Result.Inputs = { { Field::Val, 0, Class::Required }, { Field::None, 0, Class::Required, Field::None, Role::EvaluationEntry } };
	}
	if(Lang == ::Lang::Python && Name == "with_clause") Result.Inputs = { { Field::None, 0, Class::Required } };
	if(Lang == ::Lang::Python && Name == "as_pattern") Result.Inputs = { { Field::Alias, 0, Class::Unevaluated } };
	if(Lang == ::Lang::Swift && Has(Role::CallNode)) Result.HasDeferredArguments = true;
	if(Lang == ::Lang::Python && Has(Role::CreationInputs)) {
		Result.Inputs.insert(Result.Inputs.begin(), { Field::None, 0, Class::Unknown, Field::None, Role::AnnotatedParam });
	}
	if(Lang == ::Lang::Python && Name == "typed_default_parameter") Result.Inputs.push_back({ Field::Type, 0, Class::Unknown });
	if(
		Lang == ::Lang::Kotlin && (Name == "annotated_lambda" || Name == "call_suffix" || Name == "value_argument") ||
		Lang == ::Lang::Swift && (Name == "call_suffix" || Name == "value_argument")
	) Result.Inputs.push_back({ Field::None, 0, Class::Invocation, Field::None, Role::LambdaLiteral });
	if(Lang == ::Lang::Ruby && Has(Role::CallNode)) {
		Result.Inputs.insert(Result.Inputs.begin(), { Field::None, 0, Class::Invocation, Field::None, Role::InvocationBody });
	}
	if(Lang == ::Lang::Python && Name == "function_definition") Result.Inputs.push_back({ Field::ReturnType, 0, Class::Unknown });
	if(Has(Role::AssignmentExpr) || Has(Role::AssignmentOperation)) Result.ConditionalOperators = { "&&=", "?\?=", "||=" };
	if(Has(Role::BinaryExpr)) {
		Result.ConditionalOperators = { "&&", "?:", "??", "and", "or", "||" };
		// 演算子のフィールドが無くても，短絡の構文と確定する物だけを指定する
		if(
			Name == "conjunction_expression" || Name == "disjunction_expression" || Name == "elvis_expression" ||
			Name == "nil_coalescing_expression"
		) Result.ConditionalOperators.insert(Result.ConditionalOperators.begin(), "");
	}
	if(Lang == ::Lang::Go && (Name == "defer_statement" || Name == "go_statement")) {
		Result.Children = Class::Required;
		Result.DoesEvaluateChildInputs = true;
		Result.IsContainer = true;
	}
	if(Lang == ::Lang::Ruby && Name == "rescue_modifier") Result.CatchInput = Field::Alternative;
	if(Lang == ::Lang::Cpp && Name == "field_initializer_list") Result.Invocation = Class::Unknown;
	if(Lang == ::Lang::Swift && Name == "modifiers") Result.Invocation = Class::Required;
	if(Lang == ::Lang::Kotlin && Name == "object_declaration") Result.Children = Class::Deferred;
	if(Lang == ::Lang::Python && Name == "assignment") Result.FirstInput = Field::Right;
	Result.IsRequiredFirst = Lang == ::Lang::Python && Has(Role::CreationInputs);
	if(Has(Role::TryStmt) || Has(Role::CatchableBody)) {
		Result.Children = Class::Conditional;
		Result.Inputs = {
			{ Field::None, 0, Class::Required, Field::None, Role::ResourceList },
			{ Field::None, 0, Class::Required, Field::None, Role::EvaluationEntry }
		};
	}
	if(Lang == ::Lang::Kotlin && Name == "do_while_statement") {
		Result.ConditionIndex = 1;
		Result.Inputs.push_back({ Field::None, 1, Class::Conditional });
	}
	if(Has(Role::ConditionalExpr) && !Has(Role::If)) {
		Result.HasEntry = true;
		Result.ConditionIndex = Has(Role::ConditionalTailOnly) ? 1 : 0;
		Result.Children = Class::Conditional;
		Result.Inputs = { { Field::None, *Result.ConditionIndex, Class::Required } };
	}
	// 制御演算の優先順は，例外処理と独立した本体を一般の式より先に選ぶ
	using Rule = Core::EvaluationRule;
	Result.Rule = Result.IsContainer ? Rule::Container : Rule::Eager;
	if(Result.CatchInput != Field::None) Result.Rule = Rule::Try;
	else if(Has(Role::ExprStmt)) Result.Rule = Rule::Expr;
	else if(Has(Role::ReturnOrThrow) || Has(Role::GotoStmt)) Result.Rule = Rule::Transfer;
	else if(Has(Role::LabeledStmt)) Result.Rule = Rule::Labeled;
	else if(Has(Role::KeywordJump)) Result.Rule = Rule::KeywordJump;
	else if(Has(Role::TryStmt) || Has(Role::CatchableBody)) Result.Rule = Rule::Try;
	else if(Has(Role::Loop)) Result.Rule = Rule::Loop;
	else if(Has(Role::ExplicitKeywordBody) || Has(Role::StmtFlowContainer)) Result.Rule = Rule::Stmts;
	else if(Has(Role::SingleBodyStmt)) Result.Rule = Rule::SingleBody;
	else if(Has(Role::PreprocBranch)) Result.Rule = Rule::Preproc;
	else if(Has(Role::If)) Result.Rule = Rule::Branch;
	else if(Has(Role::ArmSwitch)) Result.Rule = Rule::Switch;
	else if(Has(Role::ConditionalExpr)) Result.Rule = Rule::Choice;
	// 記号毎に共有する評価契約の返戻
	return Result;
}

/**
 * ノード名の役割の取得関数（整形仕様 4.3，トークン間空白仕様 6.1）
 * 同じ役割を持つ複数のノード名を共通の役割へ投影し，同じノード名が文法毎に異なる構文を表す物は其の文法だけに役割を付ける
 * 計算量：ノード名長 L と固定の定義数 K に対し O(K L)
 * @param Lang 対象言語
 * @param Name 文法のノード名
 * @param IsNamed 構文のノードか（偽はトークン）
 * @return 役割の集合
 */
Core::RoleSet GrammarProfile::DeriveRoles(const ::Lang Lang, const std::string_view Name, const bool IsNamed) {
	// ノード名の役割の取得関数（整形仕様 4.3，トークン間空白仕様 6.1）
	using Core::Role;
	Core::RoleSet Roles;
	// ノードの役割の追加
	const auto Add = [&Roles](const Role Added) -> void {
		// 対象の状態の更新
		Roles.set(static_cast<size_t>(Added));
	};
	// 付与済の役割の照会
	const auto Has = [&Roles](const Role Wanted) -> bool {
		// 付与済の役割の返戻
		return Roles.test(static_cast<size_t>(Wanted));
	};
	// 正規化の規則・制御の流れ・シグネチャ・コメント
	if(Name == "mixin_statement") Add(Role::MixinStmt);
	if(Name == "color_value") Add(Role::ColorVal);
	if(Name == "arguments") Add(Role::CallArgs);
	if(Lang == Lang::Ruby && Name == "<<") Add(Role::HeredocAmbiguousShift);
	if(Name == "declaration") Add(Role::TypedDecl);
	if(Name == "string") Add(Role::QuotedStr);
	if(Name == "module") Add(Role::Module);
	if(Name == "block") Add(Role::LexicalBlock);
	if(Name == "jsx_attribute") Add(Role::UnescapedAttribute);
	if(Name == "for_statement") Add(Role::CountedLoop);
	if(Name == "identifier") Add(Role::Identifier);
	if(Names::NameToken.HasName(Name)) Add(Role::NameToken);
	if(Name == "binary_expression") Add(Role::BinaryOperation);
	if(Names::BinaryExpr.HasName(Name)) Add(Role::BinaryExpr);
	if(Name == "function_call_expression") Add(Role::FunctionCallExpr);
	if(Name == "attribute") Add(Role::Attribute);
	if(Lang == Lang::Python && Name == "attribute") Add(Role::Member);
	if(Lang.IsCFamily()) {
		if(Name == "attribute" || Name == "ms_declspec_modifier") Add(Role::ContractAttribute);
		if(Name == "attribute_declaration") Add(Role::ContractAttributeGroup);
		if(Name == "attribute_specifier") Add(Role::ContractAttributeList);
	}
	if(Name == "object_creation_expression") Add(Role::ObjectCreationExpr);
	if(Name == "parenthesized_expression") Add(Role::ValParens);
	if(Name == "exit_statement") Add(Role::ExitStmt);
	if(Name == "lambda_expression") Add(Role::LambdaExpr);
	if(Name == "parameter_list") Add(Role::DeclaratorParams);
	if(Name == "parameter_declaration") Add(Role::ParamDecl);
	if(Name == "primitive_type") Add(Role::PrimitiveType);
	if(Name == "inferred_parameters") Add(Role::InferredParams);
	if(Name == "parameter") Add(Role::Param);
	if(Name == "modifier") Add(Role::Modifier);
	if(Name == "lambda_function_type") Add(Role::LambdaFunctionType);
	if(Name == "lambda_parameter") Add(Role::LambdaParam);
	if(Name == "simple_identifier") Add(Role::SimpleName);
	if(Name == "call_suffix") Add(Role::CallSuffix);
	if(Name == "value_arguments") Add(Role::ValArgs);
	if(Name == "call_expression") Add(Role::CallExpr);
	if(Name == "comma_expression") Add(Role::CommaExpr);
	if(Name == "lambda_literal") Add(Role::LambdaLiteral);
	if(Name == "annotation") Add(Role::Annotation);
	if(Name == "type_modifiers") Add(Role::TypeModifiers);
	if(Name == "enum_entry") Add(Role::EnumEntry);
	if(Name == "class_declaration") Add(Role::ClassDecl);
	if(Name == "namespace_definition") Add(Role::NamespaceDef);
	if(Name == "namespace_alias_definition") Add(Role::NamespaceAliasDef);
	if(Name == "alias_declaration") Add(Role::AliasDecl);
	if(Name == "enum_specifier") Add(Role::EnumSpecifier);
	if(Name == "type_definition") Add(Role::TypeDef);
	if(Name == "sized_type_specifier") Add(Role::SizedTypeSpecifier);
	if(Name == "type_identifier") Add(Role::TypeName);
	if(Name == "qualified_identifier") Add(Role::QualifiedIdentifier);
	if(Name == "pointer_declarator") Add(Role::PtrDeclarator);
	if(Name == "array_declarator") Add(Role::ArrayDeclarator);
	if(Lang == Lang::Cpp && Name == "bitfield_clause") {
		Add(Role::DeclaratorWrapper);
		Add(Role::NestingDeclarator);
		Add(Role::SizedDeclarator);
	}
	if(Name == "function_declarator") Add(Role::FunctionDeclarator);
	if(Name == "compound_statement") Add(Role::CompoundBody);
	if(Name == "for_range_loop") Add(Role::ForRangeLoop);
	if(Name == "function_definition") Add(Role::FunctionDef);
	if(Name == "statements") Add(Role::ValStmtSeq);
	if(Name == "postfix_unary_expression") Add(Role::PostfixUnaryExpr);
	if(Name == "postfix_expression") Add(Role::PostfixExpr);
	if(Name == "integer") Add(Role::IntVal);
	if(Name == "while_expression") Add(Role::WhileExpr);
	if(Name == "boolean_literal") Add(Role::BoolLiteral);
	if(Name == "number_literal") Add(Role::NumericToken);
	if(Name == "empty_statement") Add(Role::EmptyStmt);
	if(Names::GetEmptyStmtContainers(Lang).HasName(Name)) Add(Role::EmptyStmtContainer);
	if(Name == "true") Add(Role::TrueVal);
	if(Name == "field_identifier") Add(Role::FieldName);
	if(Name == "init_declarator") Add(Role::InitDeclarator);
	if(Name == "type_qualifier") Add(Role::TypeQualifier);
	if(Name == "range") Add(Role::Range);
	if(Name == "assignment") Add(Role::AssignmentExpr);
	if(Name == "in") Add(Role::PatternMembership);
	if(Name == "argument_list") Add(Role::ArgSeq);
	if(Name == "method_parameters") Add(Role::MethodParams);
	if(Name == "lambda_parameters") Add(Role::LambdaParams);
	if(Name == "call") Add(Role::ReceiverCall);
	if(Name == "constant") Add(Role::ConstantName);
	if(Name == "yield") Add(Role::YieldVal);
	if(Name == "subscript") Add(Role::Subscript);
	if(Name == "type_parameter") Add(Role::GenericParam);
	if(Name == "generic_type") Add(Role::AppliedGenericType);
	if(Name == "expression_list") Add(Role::ExprList);
	if(Name == "pattern_list") Add(Role::PatternList);
	if(Name == "tuple_expression") Add(Role::TupleExpr);
	if(Name == "enum_body_declarations") Add(Role::EnumBodyDecls);
	if(Name == "statement_list") Add(Role::PlainStmtSeq);
	if(Name == "expression_case") Add(Role::ExprCase);
	if(Names::DefaultCase.HasName(Name)) Add(Role::DefaultCase);
	if(Name == "type_case") Add(Role::TypeCase);
	if(Name == "communication_case") Add(Role::CommunicationCase);
	if(Name == "ambient_declaration") Add(Role::AmbientDecl);
	if(Name == "function_expression") Add(Role::FunctionExpr);
	if(Name == "class") Add(Role::Class);
	if(Name == "return") Add(Role::ReturnKeyword);
	if(Name == "binary") Add(Role::Binary);
	if(Name == "unary") Add(Role::UnaryVal);
	if(Name == "element_reference") Add(Role::ElementRef);
	if(Name == "begin") Add(Role::ExplicitKeywordBody);
	if(Name == "operator_assignment") Add(Role::CompoundAssignment);
	if(Name == "assignment_statement") Add(Role::ValDiscardingAssignment);
	if(Name == "regex") Add(Role::Regex);
	if(Name == "heredoc_beginning" || Name == "heredoc_start") Add(Role::HeredocBeginning);
	if(Name == "heredoc_end") Add(Role::DeferredLiteralEnd);
	if(Name == "foreach_statement") Add(Role::ForeachStmt);
	if(Names::ScssMixinRule.HasName(Name)) Add(Role::MixinRule);
	if(Names::CssSelectorHost.HasName(Name)) Add(Role::SelectorHost);
	if(Names::PythonDocstringOwner.HasName(Name)) Add(Role::LeadingStrDocOwner);
	if(Names::StmtCtxParent.HasName(Name)) Add(Role::UnusedValHost);
	if(Names::GroupingParen.HasName(Name)) Add(Role::GroupingParen);
	if(Names::PostfixReceiverHost.HasName(Name)) Add(Role::PostfixExprHost);
	if(Names::ConstructionReceiverHost.HasName(Name)) Add(Role::ConstructionReceiverHost);
	if(Names::JavaEmptyArgHost.HasName(Name)) Add(Role::OptionalAnnotationArgs);
	if(Names::FunctionLikeAny.HasName(Name) || Lang == Lang::TypeScript && Name == "function_signature") Add(Role::FunctionScope);
	if(Names::ClassBody.HasName(Name)) Add(Role::ClassBody);
	if(Names::JavaStmtHost.HasName(Name)) Add(Role::DiscardedBranchValHost);
	if(Names::NumLiteral.HasName(Name)) Add(Role::NumLiteral);
	if(Names::CppDeclTypeSpecifier.HasName(Name)) Add(Role::DeclTypeSpecifier);
	if(Names::TupleLike.HasName(Name)) Add(Role::TupleLike);
	if(Names::PhpAltCloser.HasName(Name)) Add(Role::AlternativeBodyCloser);
	if(Name == "anonymous_class") Add(Role::AnonymousClass);
	if(Name == "lambda_function_type_parameters") Add(Role::LambdaFunctionTypeParams);
	if(Name == "constructor_invocation") Add(Role::ConstructorInvocation);
	if(Name == "attribute_argument_list") Add(Role::AttributeArgList);
	if(Name == "initializer_expression" || Lang.IsCFamily() && (Name == "initializer_list" || Name == "initializer_pair")) {
		Add(Role::InitializerExpr);
	}
	if(Name == "annotated_lambda") Add(Role::AnnotatedLambda);
	if(Name == "class_body") Add(Role::ClassMembers);
	if(Name == "enum_class_body") Add(Role::EnumClassBody);
	if(Name == "secondary_constructor") Add(Role::SecondaryConstructor);
	if(Name == "primary_constructor") Add(Role::PrimaryConstructor);
	if(Name == "goto_statement") Add(Role::GotoStmt);
	if(Name == "switch_statement") Add(Role::SwitchStmt);
	if(Name == "labeled_statement") Add(Role::LabeledStmt);
	if(Name == "named_label_statement") Add(Role::NamedLabelStmt);
	if(Name == "boolean") Add(Role::Boolean);
	if(Name == "loop_expression") Add(Role::LoopExpr);
	if(Name == "macro_invocation") Add(Role::MacroCall);
	if(Name == "method_invocation") Add(Role::MethodInvocation);
	if(Name == "switch_label") Add(Role::SwitchLabel);
	if(Name == "fallthrough_statement") Add(Role::FallthroughStmt);
	if(Name == "else") Add(Role::Else);
	if(Name == "statement_label") {
		Add(Role::StmtLabel);
		// ラベルは `:` で終わり，後の文へ続く
		Add(Role::ExprContinuationTail);
	}
	if(Name == "switch_entry") Add(Role::SwitchEntry);
	if(Name == "preproc_else") Add(Role::PreprocElse);
	if(Name == "ellipsis") Add(Role::Ellipsis);
	if(Name == "translation_unit") Add(Role::TranslationUnit);
	if(Name == "linkage_specification") Add(Role::LinkageSpec);
	if(Name == "declaration_list") Add(Role::DeclList);
	if(Name == "parenthesized_declarator") Add(Role::ParenthesizedDeclarator);
	if(Name == "operator_cast") Add(Role::OperatorCast);
	if(Name == "placeholder_type_specifier") Add(Role::PlaceholderTypeSpecifier);
	if(Name == "trailing_return_type") Add(Role::TrailingReturnType);
	if(Name == "destructor_declaration") Add(Role::DestructorDecl);
	if(Lang == Lang::Cpp && Name == "destructor_name") Add(Role::MemberDestructorName);
	if(Names::ResultlessRoutine.HasName(Name)) Add(Role::ResultlessRoutine);
	if(Name == "getter" || Name == "computed_getter") Add(Role::ReaderRoutine);
	if(Name == "accessor_declaration") Add(Role::NamedAccessor);
	if(Name == "generic_name") Add(Role::GenericName);
	if(Name == "method_declaration") Add(Role::MethodDecl);
	if(Name == "function_value_parameters") Add(Role::FunctionValParams);
	if(Name == "function_body") Add(Role::FunctionBody);
	if(Name == "asserts_annotation") Add(Role::AssertsAnnotation);
	if(Name == "body_statement") Add(Role::ImplicitResultBody);
	if(Name == "variable_declarator") Add(Role::VarDeclaratorItem);
	if(Name == "variable_declaration") Add(Role::VarDecl);
	if(Name == "reference_expression") Add(Role::RefExpr);
	if(Name == "unary_expression") Add(Role::UnaryOperation);
	if(Name == "label") Add(Role::JumpLabel);
	if(Name == "if_expression") Add(Role::ConditionalVal);
	if(Name == "for_expression") Add(Role::IterationVal);
	if(Name == "match_block") Add(Role::MatchBlock);
	if(Names::ScopeBody.HasName(Name)) Add(Role::ScopeBody);
	if(IsNamed && Names::FlowReturn.HasName(Name)) Add(Role::FlowReturn);
	if(Names::KeywordJump.HasName(Name)) Add(Role::KeywordJump);
	if(Names::Comment.HasName(Name)) Add(Role::Comment);
	if(Names::LoopExitStmt.HasName(Name)) Add(Role::LoopExitStmt);
	if(Name == "volatile") Add(Role::ObservableQualifier);
	if(Names::ContinueKeyword.HasName(Name)) Add(Role::ContinueKeyword);
	if(Names::BreakKeyword.HasName(Name)) Add(Role::BreakKeyword);
	if(Names::IntLiteralLike.HasName(Name)) Add(Role::IntLiteralCandidate);
	if(Names::CondParenWrap.HasName(Name)) Add(Role::CondParenWrap);
	if(Names::WhileLoop.HasName(Name) || Name == "while" || Name == "while_modifier") Add(Role::WhileLoop);
	if(Names::CallNode.HasName(Name)) Add(Role::CallNode);
	if(Names::SwitchBody.HasName(Name)) Add(Role::SwitchBody);
	if(IsNamed && Names::CaseClause.HasName(Name)) Add(Role::CaseClause);
	if(Names::CaseBodyOpener.HasName(Name)) Add(Role::CaseBodyOpener);
	if(Lang == Lang::PHP && (Name == "match_conditional_expression" || Name == "match_default_expression")) Add(Role::CaseExprArm);
	if(Names::ConditionalBranchClause.HasName(Name)) Add(Role::ConditionalBranchClause);
	if(Names::ElseAny.HasName(Name)) Add(Role::ElseAny);
	if(Names::CatchClause.HasName(Name)) Add(Role::CatchClause);
	if(Names::FinallyClause.HasName(Name)) Add(Role::FinallyClause);
	if(IsNamed && Names::ReturnOrThrow.HasName(Name)) Add(Role::ReturnOrThrow);
	if(Names::StmtFlowContainer.HasName(Name)) Add(Role::StmtFlowContainer);
	if(Names::SingleBodyStmt.HasName(Name)) Add(Role::SingleBodyStmt);
	if(
		Lang == Lang::CSharp && Name == "using_statement" || Lang == Lang::Java && Name == "try_with_resources_statement" ||
		Lang == Lang::Python && Name == "with_statement"
	) Add(Role::ResourceBody);
	if(Lang == Lang::Python && Name == "with_clause" || Lang == Lang::Java && Name == "resource_specification") {
		Add(Role::ResourceList);
	}
	if(Lang == Lang::Python && Name == "with_statement") Add(Role::SuppressingBody);
	if(Names::TryStmt.HasName(Name)) Add(Role::TryStmt);
	if(Names::FunctionBodyContainer.HasName(Name)) Add(Role::FunctionBodyContainer);
	if(Names::PythonPlaceholder.HasName(Name)) Add(Role::PlaceholderStmt);
	if(Names::CDeclSpecifier.HasName(Name)) Add(Role::DeclSpecifier);
	if(Names::CFunctionOrFieldDeclarator.HasName(Name)) Add(Role::DeclaratorWrapper);
	if(Name == "reference_declarator" || Lang.IsCFamily() && Name == "abstract_reference_declarator") Add(Role::RefDeclarator);
	if(Name == "structured_binding_declarator") Add(Role::StructuredBindingDeclarator);
	if(Names::PtrOrRefDeclarator.HasName(Name)) Add(Role::PtrOrRefDeclarator);
	if(Names::KotlinReturnType.HasName(Name)) Add(Role::PostParamResultType);
	if(Names::VarDeclarator.HasName(Name)) Add(Role::VarDeclarator);
	if(Names::NonConstantExpr.HasName(Name)) Add(Role::NonConstantExpr);
	if(Names::QualifiedMember.HasName(Name)) Add(Role::QualifiedMember);
	if(Names::ThisRef.HasName(Name)) Add(Role::ThisRef);
	if(Names::RustDeclStmt.HasName(Name)) Add(Role::NonValDecl);
	if(Names::RustPlaceProjection.HasName(Name)) Add(Role::PlaceProjection);
	if(Names::RustScrutineeHost.HasName(Name)) Add(Role::ScrutineeHost);
	if(Names::RustBorrowingOperator.HasName(Name)) Add(Role::BorrowingOperator);
	if(Names::RustBodyOnlyLoop.HasName(Name)) Add(Role::BodyTempScopeLoop);
	if(Names::RustLetCond.HasName(Name)) Add(Role::PatternCond);
	if(Names::LazyLogicalOperator.HasName(Name)) Add(Role::LazyLogicalOperator);
	if(Names::RustPlaceOrPlainVal.HasName(Name)) Add(Role::PlaceOrPlainVal);
	if(Name == "for_clause") Add(Role::ForClause);
	if(Name == "range_clause") Add(Role::RangeClause);
	if(Name == "method_definition") Add(Role::MethodDef);
	if(Name == "object") Add(Role::ObjectVal);
	if(Name == "self") {
		Add(Role::SelfVal);
		if(Lang == Lang::Ruby) Add(Role::ThisRef);
	}
	if(Name == "pointer_type_declarator") Add(Role::PtrTypeDeclarator);
	if(Names::GenericParamList.HasName(Name)) Add(Role::GenericParams);
	if(Names::DocWrapper.HasName(Name)) Add(Role::DocWrapper);
	if(Names::AttachPrefix.HasName(Name)) Add(Role::AttachPrefix);
	if(Names::DocumentedDef.HasName(Name)) Add(Role::DocumentedDef);
	// ドキュメントを付けれる宣言（同じノード名が文法毎に異なる構文を表す物は宣言を表す文法だけに付け，CSS，SCSS の性質の宣言と Python の根を除く）
	if(
		Names::DocumentableDecl.HasName(Name) && !(Lang == Lang::CSS && Name == "declaration") &&
		!(Lang == Lang::Python && Name == "module") || Lang == Lang::Ruby && (Name == "assignment" || Name == "call") ||
		Lang == Lang::TypeScript && Name == "property_identifier"
	) Add(Role::DocumentableDecl);
	if(Names::DocHostSeq.HasName(Name)) Add(Role::DocHostSeq);
	if(Names::ParamContainerLike.HasName(Name)) Add(Role::ParamContainerLike);
	if(Names::DestructurePattern.HasName(Name) || Lang == Lang::Rust && Names::RustDestructurePattern.HasName(Name)) {
		Add(Role::DestructurePattern);
	}
	if(Names::DeclTypeChild.HasName(Name)) Add(Role::DeclTypeChild);
	if(Names::DefBody.HasName(Name) || Lang == Lang::Cpp && Name == "try_statement") Add(Role::DefBody);
	if(Names::SectionKeyword.HasName(Name)) Add(Role::SectionKeyword);
	if(Names::CloserToken.HasName(Name)) Add(Role::CloserToken);
	if(Names::StrLikeInnerPreserve.HasName(Name)) Add(Role::LiteralInterior);
	if(Names::LiteralContent.HasName(Name)) Add(Role::LiteralContent);
	if(Lang == Lang::Ruby && Names::RubyArrayLiteral.HasName(Name)) Add(Role::ArrayVal);
	if(Names::OpaqueLineText.HasName(Name)) Add(Role::OpaqueLineText);
	if(Names::GeneratorMarker.HasName(Name)) Add(Role::GeneratorMarker);
	if(Names::ExitKeyword.HasName(Name)) Add(Role::ExitKeyword);
	if(Lang == Lang::Ruby && Name == "body_statement") Add(Role::HoistedDocBody);
	if(Lang == Lang::Ruby && Has(Role::DocumentedDef) || Names::KotlinBraceBodyHost.HasName(Name)) Add(Role::InlineDefBody);
	if((Lang == Lang::Kotlin || Lang == Lang::Swift) && Has(Role::DocumentedDef)) Add(Role::SeparateParamParts);
	if(Lang.IsCFamily() && Has(Role::DocumentedDef)) Add(Role::DeclaratorSig);
	if(Lang == Lang::Swift && Has(Role::DocumentedDef)) Add(Role::DirectParamSig);
	if(Lang == Lang::Rust && Name == "parameter") Add(Role::PatternParam);
	if(
		Name ==
		(Lang == Lang::Kotlin || Lang == Lang::Swift ? "simple_identifier" : Lang == Lang::PHP ? "variable_name" : "identifier") ||
		Lang == Lang::Rust && Name == "self"
	) Add(Role::ParamName);
	if(Lang == Lang::Go && Name == "source_file") Add(Role::AdjacentDocSeq);
	// 静的解析
	if(Names::AccessOrderMember.HasName(Name)) Add(Role::AccessOrderMember);
	if(Names::AssignmentExpr.HasName(Name)) Add(Role::AssignmentOperation);
	if(Names::CondFieldHost.HasName(Name)) Add(Role::CondFieldHost);
	if(Names::ConditionalAlternative.HasName(Name)) Add(Role::ConditionalAlternative);
	if(Names::DeclInitDeclarator.HasName(Name)) Add(Role::DeclInitDeclarator);
	if(Names::DeclLike.HasName(Name)) Add(Role::DeclLike);
	if(Names::FlowExit.HasName(Name)) Add(Role::FlowExit);
	if(Names::FunctionLikeDef.HasName(Name)) Add(Role::NamedFunctionDef);
	if(Names::FunctionNameIdentifier.HasName(Name) || Lang == Lang::Cpp && Name == "field_identifier") {
		Add(Role::FunctionNameIdentifier);
	}
	if(Names::IdentifierLeafLike.HasName(Name)) Add(Role::IdentifierLeafLike);
	if(Names::IfNode.HasName(Name)) Add(Role::Conditional);
	if(Names::JsxElement.HasName(Name)) Add(Role::TagElement);
	if(Names::JumpWithLabel.HasName(Name)) Add(Role::JumpWithLabel);
	if(Names::KotlinAssignableWrapper.HasName(Name)) Add(Role::AssignableWrapper);
	if(Names::LabeledStmt.HasName(Name)) Add(Role::LabeledConstruct);
	if(Names::Loop.HasName(Name)) Add(Role::RepeatedControl);
	if(Names::MemberFieldRank.HasName(Name)) Add(Role::MemberFieldRank);
	if(Lang == Lang::Cpp && Name == "friend_declaration") Add(Role::MemberRelation);
	if(Names::MemberMethodRank.HasName(Name)) Add(Role::MemberMethodRank);
	if(Lang.IsCFamily() && Name == "storage_class_specifier") Add(Role::MemberStorageSpecifier);
	if(Names::MemberTypeRank.HasName(Name)) Add(Role::MemberTypeRank);
	// 各文法のメンバ構造を宣言順の共通の役割へ対応付ける
	if(
		Names::ClassBody.HasName(Name) || Name == "interface_body" || Name == "annotation_type_body" || Name == "protocol_body" ||
		Name == "enum_class_body" || Name == "body_statement" || Lang == Lang::PHP && Name == "enum_declaration_list" ||
		Lang == Lang::Python && Name == "block" || Lang == Lang::Go && Name == "interface_type"
	) Add(Role::MemberOrderBody);
	if(
		Names::MemberTypeRank.HasName(Name) || Name == "class" || Name == "class_definition" || Name == "struct_type" ||
		Name == "struct_item" || Name == "enum_item" || Name == "trait_item" || Name == "impl_item" || Name == "interface_type" ||
		Name == "object_declaration" || Name == "companion_object" || Name == "protocol_declaration" ||
		Name == "annotation_type_declaration" || Name == "singleton_class" || Name == "module"
	) Add(Role::MemberOwner);
	if(
		Name == "constructor_declaration" || Name == "compact_constructor_declaration" || Name == "secondary_constructor" ||
		Name == "primary_constructor" || Name == "init_declaration" || Name == "anonymous_initializer" ||
		Name == "static_initializer" || Name == "class_static_block"
	) Add(Role::MemberConstructor);
	if(Lang.IsJsTs() && Name == "private_property_identifier") Add(Role::MemberPrivateName);
	if(Name == "destructor_declaration" || Name == "deinit_declaration") Add(Role::MemberDestructor);
	if(Lang == Lang::PHP && Name == "use_declaration") Add(Role::MemberRelation);
	if(Lang == Lang::Ruby && Name == "call") Add(Role::MemberDeclCall);
	if(Lang == Lang::Go && Name == "field_declaration") Add(Role::MemberEmbeddedField);
	if(Lang == Lang::Go && Name == "type_elem") Add(Role::MemberRelation);
	if(Lang == Lang::Java && Name == "object_creation_expression") Add(Role::MemberOwner);
	if(
		Name == "type_alias_declaration" || Name == "typealias_declaration" || Name == "type_alias" || Name == "type_item" ||
		Name == "associated_type" || Name == "associatedtype_declaration" || Name == "delegate_declaration" || Name == "struct_item" ||
		Name == "enum_item" || Name == "trait_item" || Name == "object_declaration" || Name == "class_definition" || Name == "class"
	) Add(Role::MemberTypeRank);
	if(
		Name == "const_item" || Name == "static_item" || Name == "const_declaration" || Name == "constant_declaration" ||
		Name == "class_variable" || Name == "companion_object" || Lang == Lang::Ruby && Name == "constant" ||
		Lang == Lang::Python && Name == "assignment"
	) Add(Role::MemberTypeState);
	if(
		Name == "field_definition" || Name == "public_field_definition" || Name == "field_declaration" ||
		Name == "property_declaration" || Name == "protocol_property_declaration" || Name == "property_signature" ||
		Name == "instance_variable"
	) Add(Role::MemberFieldRank);
	if(
		Name == "function_item" || Name == "function_signature_item" || Name == "function_declaration" || Name == "method" ||
		Name == "singleton_method" || Name == "method_signature" || Name == "abstract_method_signature" ||
		Name == "annotation_type_element_declaration" || Name == "method_elem"
	) Add(Role::MemberMethodRank);
	if(Name == "singleton_method") Add(Role::MemberTypeOperation);
	if(Lang == Lang::Kotlin && (Name == "object_declaration" || Name == "companion_object")) Add(Role::MemberStaticOwner);
	if(Lang == Lang::Java && (Name == "interface_declaration" || Name == "annotation_type_declaration")) {
		Add(Role::MemberTypeFields);
	}
	if(Name == "self_parameter") Add(Role::MemberReceiver);
	if(Lang == Lang::Ruby && Name == "identifier") Add(Role::MemberVisibilitySection);
	if(
		Name == "modifiers" || Name == "modifier" || Name == "static_modifier" || Name == "member_modifier" ||
		Name == "property_modifier" || Name == "function_modifier" || Name == "visibility_modifier" || Name == "decorator" ||
		Name == "annotation" || Name == "marker_annotation"
	) Add(Role::MemberModifier);
	if(Name == "decorated_definition" || Name == "expression_statement") Add(Role::MemberWrapper);
	if(Names::MutatingExpr.HasName(Name)) Add(Role::MutatingExpr);
	if(Names::NameDeclarator.HasName(Name)) Add(Role::NameDeclarator);
	if(Names::NamedParamKind.HasName(Name)) Add(Role::NamedParamKind);
	if(Names::OrdinaryFunction.HasName(Name)) Add(Role::OrdinaryFunctionDef);
	if(Names::ScopeContainer.HasName(Name)) Add(Role::ScopeContainer);
	if(Names::StrLikeAll.HasName(Name)) {
		Add(Role::BreakableDelimiterInterior);
		Add(Role::StrLikeAll);
	}
	if(Names::SwitchGotoTarget.HasName(Name)) Add(Role::SwitchGotoTarget);
	if(Names::TypeNameNode.HasName(Name) || Lang.IsCFamily() && (Name == "qualified_identifier" || Name == "template_type")) {
		Add(Role::TypeNameNode);
	}
	if(Lang.IsCFamily() && Name == "template_argument_list") Add(Role::TypeArgList);
	if(
		Names::TypedDecl.HasName(Name) || Lang.IsCFamily() && (Name == "for_range_loop" || Name == "field_declaration") ||
		Lang.IsJsTs() && Name == "variable_declarator" ||
		Lang == Lang::TypeScript && (Name == "required_parameter" || Name == "optional_parameter") ||
		Lang == Lang::Python && (Name == "assignment" || Name == "typed_parameter" || Name == "typed_default_parameter")
	) Add(Role::DeclWithType);
	if(Names::UnaryPre.HasName(Name)) Add(Role::PrefixUnary);
	if(Names::UnaryPrefixExpr.HasName(Name)) Add(Role::UnaryPrefixExpr);
	if(Names::VarDecl.HasName(Name)) Add(Role::VarDeclStmt);
	if(Name == "access_specifier") Add(Role::AccessSection);
	if(Name == "concatenated_string") Add(Role::ConcatenatedLiteral);
	if(Name == "control_transfer_statement") Add(Role::ControlTransfer);
	if(Name == "custom_operator") Add(Role::UserDefinedOperator);
	if(Name == "else_clause") Add(Role::AlternativeClause);
	if(Name == "enum_body") Add(Role::EnumerationBody);
	if(Name == "field_declaration") Add(Role::FieldDef);
	if(
		Lang == Lang::CSharp && Name == "join_clause" || Lang == Lang::Ruby && Name == "expression_reference_pattern" ||
		Lang == Lang::Cpp && Name == "field_initializer_list"
	) Add(Role::EvaluationEntry);
	if(Lang == Lang::Kotlin && Name == "when_subject") Add(Role::EvaluationCondition);
	if(Name == "init_statement") Add(Role::CondInitializer);
	// 作成時の評価を呼出時の仮引数・本体から分ける
	if(
		Lang == Lang::Cpp && Name == "lambda_capture_specifier" || Lang == Lang::Swift && Name == "capture_list" ||
		Lang == Lang::Python && (Name == "lambda_parameters" || Name == "parameters")
	) Add(Role::CreationInputs);
	if(
		Lang == Lang::Cpp && Name == "lambda_capture_initializer" || Lang == Lang::Swift && Name == "capture_list_item" ||
		Lang == Lang::Python && (Name == "default_parameter" || Name == "typed_default_parameter")
	) Add(Role::CreationVal);
	if(Lang == Lang::Cpp && Name == "subscript_argument_list") Add(Role::ArgSeq);
	if(
		Lang == Lang::Cpp && Name == "initializer_list" || Lang == Lang::Python && (
			Name == "attribute" || Name == "decorated_definition" || Name == "decorator" || Name == "dictionary" || Name == "list" ||
			Name == "pair" || Name == "set" || Name == "tuple"
		)
	) Add(Role::EagerEvaluation);
	if(Name == "jsx_expression") Add(Role::EmbeddedExpr);
	if(Name == "jump_expression") Add(Role::JumpExpr);
	if(Name == "method") Add(Role::InstanceMethodDef);
	if(Name == "modifiers") Add(Role::ModifierSeq);
	if(Name == "rescue_modifier") Add(Role::RescueExpr);
	if(Name == "return_statement") Add(Role::ReturnStmt);
	if(Name == "singleton_method") Add(Role::SingletonMethod);
	if(Name == "throw_expression") Add(Role::ThrowVal);
	if(Name == "throw_keyword") Add(Role::ThrowKeyword);
	if(Name == "throw_statement") Add(Role::ThrowStmt);
	if(Name == "throws") Add(Role::ThrowsDecl);
	if(Name == "user_type") Add(Role::UserType);
	if(
		Has(Role::FunctionScope) || Has(Role::OrdinaryFunctionDef) || Name == "lambda_literal" && Lang != Lang::Kotlin ||
		Has(Role::ClassBody) || Name == "async_block"
	) Add(Role::FlowBoundary);
	const bool IsSwitchStmtLike = Names::SwitchStmtLike.HasName(Name);
	if(IsSwitchStmtLike || Lang == Lang::Java && Name == "switch_expression") Add(Role::ArmSwitch);
	if(Lang == Lang::CSharp && Name == "goto_statement") Add(Role::CaseLabelJump);
	if(Lang == Lang::Ruby && Name == "return") Add(Role::ArgReturn);
	if(Lang == Lang::Rust && Has(Role::LoopExitStmt)) Add(Role::LabeledValExit);
	const bool IsLoopOrSwitch = Names::LoopOrSwitch.HasName(Name);
	if(IsLoopOrSwitch && !(Lang == Lang::Swift && Name == "do_statement") || Lang == Lang::Java && Name == "switch_expression") {
		Add(Role::BreakTarget);
	}
	if(Lang == Lang::PHP && Name == "switch_statement") Add(Role::ContinueExitsSwitch);
	if(Lang == Lang::Kotlin && Name == "annotation") Add(Role::TransparentLabelPrefix);
	if(Lang == Lang::Go && Name == "for_statement") Add(Role::ClauseOrCondLoop);
	if(
		(Lang.IsCFamily() || Lang.IsJsTs() || Lang == Lang::CSharp || Lang == Lang::Java || Lang == Lang::PHP) &&
		Name == "for_statement"
	) Add(Role::OmittableCondLoop);
	if((Name == "do_statement" || Name == "do_while_statement") && Lang != Lang::Swift || Name == "repeat_while_statement") {
		Add(Role::PostCondLoop);
	}
	if(Lang == Lang::Rust && Name == "macro_invocation") Add(Role::TerminatingMacroCall);
	if((Lang == Lang::Go || Lang == Lang::Kotlin || Lang == Lang::Swift) && IsSwitchStmtLike) Add(Role::DirectSwitchArms);
	if(
		Lang == Lang::Go && Name == "select_statement" || (Lang == Lang::Rust || Lang == Lang::PHP) && Name == "match_expression" ||
		Lang == Lang::Swift && Name == "switch_statement"
	) Add(Role::ExhaustiveSwitch);
	if(Lang == Lang::Swift && Name == "fallthrough") Add(Role::BareFallthrough);
	if(Lang == Lang::Swift && (Name == "if_statement" || Name == "if_expression")) Add(Role::SiblingLabeledBranch);
	if(Lang == Lang::Swift && Name == "simple_identifier") Add(Role::FallthroughName);
	if(Lang == Lang::Python && IsLoopOrSwitch) Add(Role::LoopElse);
	if(Lang == Lang::Rust && Has(Role::StmtFlowContainer)) Add(Role::LabeledBlockExit);
	if(Lang == Lang::Swift && Name == "do_statement") Add(Role::CatchableBody);
	if(Lang != Lang::Ruby && Has(Role::OrdinaryFunctionDef)) Add(Role::ExplicitResultFunction);
	if(Lang.IsCFamily() && Has(Role::OrdinaryFunctionDef)) Add(Role::CompoundFunctionBody);
	if(Name == (Lang == Lang::Go ? "statement_list" : "statements")) Add(Role::InnerFunctionStmts);
	if(Lang.IsJsTs() && (Has(Role::OrdinaryFunctionDef) || Has(Role::FunctionScope))) Add(Role::StarGeneratorFunction);
	if(Lang == Lang::Python && Name == "block") Add(Role::PlaceholderBody);
	if(Lang == Lang::Python && Has(Role::OrdinaryFunctionDef)) Add(Role::MayDeclarePlaceholderFunction);
	if(Lang == Lang::Swift && Has(Role::OrdinaryFunctionDef)) Add(Role::SingleExprResultFunction);
	if(Lang == Lang::Swift && Name == "function_declaration") Add(Role::ImplicitReturnCandidate);
	if(Lang == Lang::Swift && Name == "class_declaration") Add(Role::ImplicitReturnNominal);
	if(Lang == Lang::Swift && Name == "inheritance_specifier") Add(Role::ImplicitReturnConformance);
	if(Lang == Lang::Swift && Name == "extension") Add(Role::ImplicitReturnExtToken);
	if(Lang == Lang::Swift) {
		if(Name.ends_with("_expression") || Name.ends_with("_literal") || Name == "if_statement" || Name == "switch_statement") {
			Add(Role::ImplicitReturnCandidateExpr);
		}
		if(
			static constexpr std::string_view ImplicitReturnExprs[] {
				"additive_expression",
				"array_literal",
				"as_expression",
				"bitwise_operation",
				"boolean_literal",
				"comparison_expression",
				"conjunction_expression",
				"dictionary_literal",
				"disjunction_expression",
				"equality_expression",
				"infix_expression",
				"integer_literal",
				"line_string_literal",
				"multi_line_string_literal",
				"multiplicative_expression",
				"navigation_expression",
				"nil_coalescing_expression",
				"nil_literal",
				"postfix_expression",
				"prefix_expression",
				"range_expression",
				"real_literal",
				"simple_identifier",
				"ternary_expression",
				"tuple_expression"
			};
			std::ranges::find(ImplicitReturnExprs, Name) != std::end(ImplicitReturnExprs)
		) {
			Add(Role::ImplicitReturnCandidateExpr);
			Add(Role::ImplicitReturnExpr);
		}
	}
	if(Name == "parameters") Add(Role::MixinParams);
	if(Lang == Lang::Python && Name == "string") Add(Role::DocStr);
	if(Lang == Lang::Python && Name == "class_definition") Add(Role::OptionalEmptySuperclass);
	if(
		Lang == Lang::PHP &&
		(Name == "function_call_expression" || Name == "attribute" || Name == "object_creation_expression" || Name == "exit_statement")
	) Add(Role::OptionalConstructionArgs);
	if(Lang == Lang::Cpp && Name == "lambda_expression") Add(Role::OptionalLambdaDeclarator);
	if(Lang == Lang::Cpp && !Lang.IsCShared && Name == "parameter_list") Add(Role::OptionalVoidParam);
	if(Lang != Lang::Go && Names::PostfixOrUpdateExpr.HasName(Name)) Add(Role::PrefixableIncrement);
	if(Lang == Lang::Cpp && Has(Role::PrefixableIncrement)) Add(Role::OverloadableIncrement);
	if(Lang == Lang::Java && Name == "switch_rule") Add(Role::ValSwitchRule);
	if(Lang == Lang::Rust && Name == "while_expression") Add(Role::UnconditionalLoopKeyword);
	if((Lang.IsCFamily() || Lang == Lang::CSharp || Lang == Lang::Java || Lang.IsJsTs() || Lang == Lang::PHP) && IsLoopOrSwitch) {
		Add(Role::ParenthesizedLoopCond);
	}
	if(Lang.IsCFamily() && Names::BoolInitCtx.HasName(Name)) Add(Role::BoolInitializerDecl);
	if(Lang.IsCFamily() && Name == "declaration") Add(Role::LeadingConstDecl);
	if(Lang != Lang::HTML && Lang != Lang::CSS && IsNamed && Has(Role::NumLiteral)) Add(Role::NormalizedNum);
	if(Lang == Lang::CSS && (Has(Role::MixinRule) || Name == "color_value" || Name == "declaration")) Add(Role::StyleLexeme);
	if(
		(Lang.IsJsTs() || Lang == Lang::PHP || Lang == Lang::Ruby || Lang == Lang::Python || Lang == Lang::CSS) && IsNamed &&
		Has(Role::StrLikeAll)
	) Add(Role::NormalizedQuote);
	if(
		Lang == Lang::Ruby &&
		(Name == "while" || Name == "range" || Name == "argument_list" || Name == "method_parameters" || Name == "lambda_parameters")
	) Add(Role::BareArgNotation);
	if(Lang == Lang::Python && (Name == "subscript" || Name == "type_parameter")) Add(Role::TupleTypeComma);
	if(
		Lang == Lang::Python &&
		(Name == "expression_list" || Name == "pattern_list" || Name == "lambda_parameters" || Name == "tuple_expression")
	) Add(Role::BareListTrailingComma);
	if(
		Lang == Lang::Go && Names::GoSemicolonHost.HasName(Name) || Lang == Lang::Python && (Name == "module" || Name == "block") ||
		Lang == Lang::Swift && Name == "enum_entry"
	) Add(Role::OptionalSemicolonHost);
	if(Lang == Lang::Go && Name == "statement_list") Add(Role::EmptySemicolonSeq);
	if(Lang == Lang::Rust && Has(Role::OrdinaryFunctionDef)) Add(Role::AttributedTailFunction);
	if(
		Lang == Lang::PHP &&
		(IsLoopOrSwitch || Name == "if_statement" || Name == "declare_statement" || Name == "switch_block" || Name == "colon_block")
	) Add(Role::OptionalLoopTerminator);
	// 中間表現の構築と配置
	if(Name == "text") Add(Core::Role::LiteralText);
	if(Name == "%w(" || Name == "%i(") Add(Core::Role::WordArrayOpen);
	if(Name == ")") Add(Core::Role::WordArrayClose);
	if(Lang.IsJsTs() && (Name == "if_statement" || Name == "else_clause")) Add(Core::Role::DependentBranchLayout);
	if(Names::BreakOpaque.HasName(Name)) Add(Core::Role::BreakOpaque);
	if(Names::CompactRecurse.HasName(Name)) Add(Core::Role::CompactRecurse);
	if(Names::Compact.HasName(Name)) Add(Core::Role::Compact);
	if(Names::StrSeqContainer.HasName(Name)) Add(Core::Role::StrSeqContainer);
	if(Names::RubyArrayLiteral.HasName(Name)) Add(Core::Role::WordArray);
	if(Names::JsxTag.HasName(Name)) Add(Core::Role::ExprTag);
	if(Names::OptionalTypeMember.HasName(Name)) Add(Core::Role::OptionalTypeMember);
	if(Names::BreakColonHost.HasName(Name)) Add(Core::Role::BreakColonHost);
	if(Names::InfixOperator.HasName(Name)) Add(Core::Role::InfixOperator);
	if(Names::InitHeader.HasName(Name)) Add(Core::Role::InitHeader);
	if(Names::PipeParamList.HasName(Name)) Add(Core::Role::PipeParamList);
	if(Name == "super_expression") Add(Core::Role::BaseRef);
	if(Name == "typed_parameter") Add(Core::Role::AnnotatedParam);
	if(Name == "typed_default_parameter") Add(Core::Role::AnnotatedDefaultParam);
	if(Name == "union_type") Add(Core::Role::UnionType);
	if(Name == "type") Add(Core::Role::TypeSlot);
	if(Name == "optional_type") Add(Core::Role::OptionalType);
	if(Name == "lambda_capture_initializer") Add(Role::CaptureInitializer);
	if(Name == "lambda_capture_specifier") Add(Core::Role::CaptureList);
	if(Name == "type_arguments") Add(Core::Role::GenericArgs);
	if(Names::StrInterpolation.HasName(Name)) Add(Core::Role::StrInterpolation);
	if(Names::ElseLikeClause.HasName(Name)) Add(Core::Role::ElseLikeClause);
	if(Name == "guard_statement") Add(Core::Role::GuardStmt);
	if(Names::PreprocConditional.HasName(Name)) Add(Core::Role::PreprocConditional);
	if(Name == "php_tag") Add(Core::Role::CodeOpeningTag);
	if(Names::ExpandContainer.HasName(Name)) Add(Core::Role::ExpandContainer);
	if(Names::AngleBracketList.HasName(Name)) Add(Core::Role::AngleBracketList);
	if(Names::NamespaceLike.HasName(Name)) Add(Core::Role::NamespaceLike);
	if(Names::Transparent.HasName(Name)) Add(Core::Role::Transparent);
	if(Names::AmbiguousBodyContainer.HasName(Name)) Add(Core::Role::AmbiguousBodyContainer);
	if(Lang == Lang::Ruby && Name == "heredoc_body") Add(Core::Role::DeferredLiteralBody);
	if(Name == "import_spec_list") {
		Add(Core::Role::BreakableDelimiterInterior);
		Add(Core::Role::DelimitedMemberSeq);
		Add(Core::Role::ImportItems);
	}
	if(Name == "import_spec") Add(Core::Role::ImportItem);
	if(Name == "do") Add(Core::Role::IterationSection);
	if(Names::ObjectLike.HasName(Name)) Add(Core::Role::ObjectLike);
	if(Names::RubyClassDefLike.HasName(Name)) Add(Core::Role::InheritedClass);
	if(Names::IndentContainer.HasName(Name)) Add(Core::Role::IndentContainer);
	if(Names::StmtList.HasName(Name)) Add(Core::Role::StmtList);
	if(Names::GoGroupedDecl.HasName(Name)) Add(Core::Role::GroupedDecl);
	if(Names::PhpAltHost.HasName(Name)) Add(Core::Role::ColonBodyHost);
	if(Names::JsxContainer.HasName(Name)) Add(Core::Role::RenderedChildren);
	if(Names::StmtsOnlyBreak.HasName(Name)) Add(Core::Role::StmtsOnlyBreak);
	if(Names::RubyMethodDef.HasName(Name)) Add(Core::Role::EndDelimitedMethod);
	if(Names::UnaryPreOrUpdate.HasName(Name)) Add(Core::Role::UnaryPreOrUpdate);
	if(Names::JsxTextLike.HasName(Name)) Add(Core::Role::RenderedText);
	if(Name == "array") Add(Core::Role::ArrayLiteral);
	if(Name == "decorator") Add(Core::Role::DeclDecorator);
	if(Lang == Lang::Ruby && Name == "undef") Add(Core::Role::UncertainLocalBindings);
	if(Lang == Lang::Python && Name == "decorator" || Lang == Lang::Kotlin && Name == "annotation") {
		Add(Core::Role::MemberBindingModifier);
	}
	if(Name == "text_interpolation") Add(Core::Role::CodeTextBoundary);
	if(Names::RangeOperator.HasName(Name)) Add(Core::Role::RangeOperator);
	if(Names::ParenLeadType.HasName(Name)) Add(Core::Role::ParenLeadType);
	if(Names::PostfixReceiverChain.HasName(Name)) Add(Core::Role::PostfixReceiverChain);
	if(Names::RubyNumericLiteralSuffix.HasName(Name)) Add(Core::Role::NumericUnitSuffix);
	if(Names::MisparsedTemplateHost.HasName(Name)) Add(Core::Role::MisparsedTemplateHost);
	if(Names::OptionalMarker.HasName(Name)) Add(Core::Role::OptionalMarker);
	if(Names::AttachColonOrDot.HasName(Name)) Add(Core::Role::AttachColonOrDot);
	if(Names::SpaceSkip.HasName(Name)) Add(Core::Role::SpaceSkip);
	if(Names::NormColon.HasName(Name)) Add(Core::Role::NormColon);
	if(Names::TypeCombination.HasName(Name)) Add(Core::Role::TypeCombination);
	if(Names::CombinatorSelector.HasName(Name)) Add(Core::Role::CombinatorSelector);
	if(Names::CssNumericVal.HasName(Name)) Add(Core::Role::DimensionNum);
	if(Names::CssSpacedValue.HasName(Name)) Add(Core::Role::SpacedValue);
	if(Names::KotlinNotIsIn.HasName(Name)) Add(Core::Role::NegatableTestWord);
	if(Names::GeneratorStarHost.HasName(Name)) Add(Core::Role::GeneratorStarHost);
	if(Names::NewParenCall.HasName(Name)) Add(Core::Role::NewParenCall);
	if(Names::CastLikeExpr.HasName(Name)) Add(Core::Role::CastLikeExpr);
	if(Names::CssSelectorName.HasName(Name)) Add(Core::Role::SelectorName);
	if(Names::MacroLike.HasName(Name)) Add(Core::Role::MacroLike);
	if(Names::AttachBracket.HasName(Name)) Add(Core::Role::AttachBracket);
	if(Names::LeadingTypeArgs.HasName(Name)) Add(Core::Role::LeadingTypeArgs);
	if(Names::AttachParen.HasName(Name)) Add(Core::Role::AttachParen);
	if(Names::ReturnTuple.HasName(Name)) Add(Core::Role::ReturnTuple);
	if(Names::Control.HasName(Name)) Add(Core::Role::Control);
	if(Names::CParenDeclarator.HasName(Name)) Add(Core::Role::CompoundDeclarator);
	if(Names::BracketType.HasName(Name)) Add(Core::Role::BracketType);
	if(Names::SwiftKeywordNode.HasName(Name)) Add(Core::Role::NamedKeyword);
	if(Names::BraceAttach.HasName(Name)) Add(Core::Role::BraceAttach);
	if(Names::PtrDecl.HasName(Name)) Add(Core::Role::PtrDecl);
	if(Names::OpenTailToken.HasName(Name)) Add(Core::Role::OpenTailToken);
	if(Has(Role::PrefixableIncrement) || Lang == Lang::Go && Names::PostfixOrUpdateExpr.HasName(Name)) {
		Add(Core::Role::PostfixOrUpdateExpr);
	}
	if(Names::ColonPairHost.HasName(Name)) Add(Core::Role::ColonPairHost);
	if(Names::RangeLike.HasName(Name)) Add(Core::Role::RangeLike);
	if(Names::CCastTypeNameInner.HasName(Name)) Add(Core::Role::CastTypeAtom);
	if(Names::UnaryPrefixToken.HasName(Name)) Add(Core::Role::UnaryPrefixToken);
	if(Names::SeparatorToken.HasName(Name)) Add(Core::Role::SeparatorToken);
	if(Names::ParamContainer.HasName(Name)) Add(Core::Role::ParamContainer);
	if(Names::HtmlTag.HasName(Name)) Add(Core::Role::TagSyntax);
	if(Names::GoType.HasName(Name)) Add(Core::Role::BracketedType);
	if(Names::SpacelessBrace.HasName(Name)) Add(Core::Role::SpacelessBrace);
	if(Names::ColonControl.HasName(Name)) Add(Core::Role::ColonControl);
	if(Names::Member.HasName(Name)) Add(Core::Role::Member);
	if(Names::BraceNoSpace.HasName(Name)) Add(Core::Role::BraceNoSpace);
	if(Names::ControlKeyword.HasName(Name)) Add(Core::Role::ControlKeyword);
	if(Names::PatternKind.HasName(Name)) Add(Core::Role::PatternKind);
	if(Names::RubyFlowExpr.HasName(Name)) Add(Core::Role::ValJump);
	if(Names::SpaceKeyword.HasName(Name)) Add(Core::Role::SpaceKeyword);
	if(Names::ArrowToken.HasName(Name)) Add(Core::Role::ArrowToken);
	if(Names::KotlinFunctionHead.HasName(Name)) Add(Core::Role::GenericFunctionHead);
	if(Name == "conditional") Add(Core::Role::ConditionalOperator);
	if(Name == "interpolation" || Name == "raw_str_interpolation" || Name == "template_substitution") {
		Add(Core::Role::BreakableDelimiterInterior);
		Add(Core::Role::Interpolation);
	}
	if(Name == "template_function") Add(Core::Role::GenericFunction);
	if(Name == "binary_operator") Add(Core::Role::BinaryOperator);
	if(Name == "plain_value") Add(Core::Role::UnitText);
	if(Name == "variadic_type_parameter_declaration") Add(Core::Role::VariadicTypeParam);
	if(Lang != Lang::Python && Names::NamedTypeParam.HasName(Name)) Add(Core::Role::TemplateTypeParam);
	if(Lang == Lang::Cpp) {
		if(Name == "base_class_clause") Add(Core::Role::BindingBaseClause);
		if(
			Name == "optional_type_parameter_declaration" || Name == "template_template_parameter_declaration" ||
			Name == "type_parameter_declaration" || Name == "variadic_type_parameter_declaration"
		) Add(Core::Role::TemplateTypeParam);
		if(Name == "optional_parameter_declaration" || Name == "parameter_declaration" || Name == "variadic_parameter_declaration") {
			Add(Core::Role::TemplateValParam);
		}
	}
	if(Name == "function_type") Add(Core::Role::FunctionType);
	if(Name == "spread_parameter") Add(Core::Role::SpreadParam);
	if(Name == "parameter_pack_expansion" || Name == "base_class_clause" || Name == "lambda_capture_initializer") {
		Add(Core::Role::PackExpansion);
	}
	if(Name == "sizeof_expression") Add(Core::Role::SizeofExpr);
	if(Name == "this_expression") Add(Core::Role::SelfRef);
	if(Name == "indexer_declaration") Add(Core::Role::Indexer);
	if(Name == "bracketed_parameter_list") Add(Core::Role::IndexerParams);
	if(Name == "implicit_array_creation_expression") Add(Core::Role::InferredArrayConstruction);
	if(Lang == Lang::Cpp && Name == "delete_expression") Add(Core::Role::AttachedKeywordBrackets);
	if(Name == "type_annotation" || Lang == Lang::Python && Name == "type") Add(Core::Role::TypeAnnotation);
	if(Name == "attribute_declaration") Add(Core::Role::AttributeDecl);
	if(
		Name == "template_string" || Lang == Lang::PHP && (Name == "encapsed_string" || Name == "heredoc" || Name == "heredoc_body")
	) {
		if(Name == "heredoc_body") Add(Core::Role::BreakableDelimiterInterior);
		Add(Core::Role::InterpolatedLiteral);
	}
	if(Name == "template_substitution") Add(Core::Role::TemplateInterpolation);
	if(Name == "interpolation_alignment_clause") Add(Core::Role::InterpolationAlignment);
	if(Name == "block_argument") Add(Core::Role::BlockArg);
	if(Name == "class_selector") Add(Core::Role::ClassSelector);
	if(Name == "nesting_selector") Add(Core::Role::NestingSelector);
	if(Name == "id_selector") Add(Core::Role::IdSelector);
	if(Name == "attribute_selector") Add(Core::Role::AttributeSelector);
	if(Name == "optional_chain") Add(Core::Role::OptionalChain);
	if(Name == "bang") Add(Core::Role::ExclamationMark);
	if(Name == "except_clause") Add(Core::Role::ExceptionClause);
	if(Name == "pointer_type") Add(Core::Role::PtrType);
	if(Name == "value_argument") Add(Core::Role::ValArg);
	if(Name == "operator_name") Add(Core::Role::OperatorName);
	if(Name == "channel_type") Add(Core::Role::ChannelType);
	if(Name == "key_path_expression") Add(Core::Role::KeyPath);
	if(Name == "reference_assignment_expression") Add(Core::Role::RefAssignment);
	if(Name == "field_declaration_list") Add(Core::Role::FieldList);
	if(Name == "export_statement") Add(Core::Role::ExportDecl);
	if(Names::JsxHtmlTagOpenClose.HasName(Name)) Add(Core::Role::ContentTagBoundary);
	if(Name == "jsx_text") Add(Core::Role::ElementText);
	if(Name == "when_entry") Add(Core::Role::ValArm);
	if(Name == "block_body") Add(Core::Role::ClosureBody);
	if(Name == "enumerator_list") Add(Core::Role::EnumeratorSeq);
	if(Lang == Lang::Ruby && Name == "heredoc_beginning") Add(Core::Role::DeferredLiteralStart);
	if(Names::PhpAltLeadingColon.HasName(Name)) Add(Core::Role::LeadingColonBody);
	if(Name == "colon_block") Add(Core::Role::ColonBody);
	if(Names::PreprocDirectiveOpen.HasName(Name)) Add(Core::Role::PreprocDirectiveOpen);
	if(Names::PreprocDirectiveClose.HasName(Name)) Add(Core::Role::PreprocDirectiveClose);
	if(IsSwitchStmtLike) Add(Core::Role::SwitchStmtLike);
	if(Name == "switch_expression") Add(Core::Role::SwitchVal);
	if(Name == "in_clause") Add(Core::Role::PatternSection);
	if(Names::ClassBodyContainer.HasName(Name)) Add(Core::Role::ClassBodyContainer);
	if(Names::ClassLike.HasName(Name) || Lang == Lang::CSharp && Name == "record_declaration") Add(Core::Role::ClassLike);
	if(Lang.IsJsTs() && (Name == "identifier" || Name == "await")) Add(Core::Role::AwaitCallHead);
	if(Lang == Lang::PHP && Has(Role::AlternativeBodyCloser)) Add(Core::Role::ColonBodyEnd);
	if(Names::CloserWord.HasName(Name)) Add(Core::Role::CloserWord);
	if(Lang == Lang::Swift && Has(Role::InfixOperator)) Add(Core::Role::AdjacencySensitiveInfix);
	if(Lang == Lang::Ruby && Names::RubyStmtSeq.HasName(Name) || Lang == Lang::Swift && Name == "statements") {
		Add(Core::Role::DelimitedStmtSeq);
	}
	if(Lang == Lang::Swift && (Name == "diagnostic" || Name == "directive")) Add(Core::Role::HardLineDirective);
	if(Lang == Lang::Ruby && Name == "exception_variable") Add(Core::Role::UnbrokenExceptionBinding);
	if(Lang == Lang::Swift && Names::SwiftPostfixCall.HasName(Name)) Add(Core::Role::OptionalInvocation);
	if(Lang == Lang::PHP && Names::PhpUnsplittable.HasName(Name)) Add(Core::Role::UnbrokenParenSyntax);
	if(Lang == Lang::Swift && Name == "import_declaration") Add(Core::Role::UnbrokenImport);
	if(Lang == Lang::Kotlin && Name == "super_expression") Add(Core::Role::UnbrokenBaseRef);
	if(Lang == Lang::Swift && Name == "optional_type") Add(Core::Role::UnbrokenOptionalType);
	if(Lang == Lang::Go && Names::GoMemberList.HasName(Name)) Add(Core::Role::DelimitedMemberSeq);
	if(Lang == Lang::Ruby && Names::RubyCondHost.HasName(Name)) Add(Core::Role::EmptyCondBody);
	if(Lang == Lang::Kotlin && Name == "for_statement") Add(Core::Role::EmptyLoopBody);
	if(Lang == Lang::Go && !Names::MultilineCommaContainer.HasName(Name) && !Has(Role::ImportItems)) {
		Add(Core::Role::NoDelimiterBreak);
	}
	if(Lang == Lang::TypeScript && Name == "binary_expression") Add(Core::Role::GenericLookalikeOperator);
	if(Lang == Lang::Ruby && Name == "block") Add(Core::Role::ClosureBlock);
	if(Lang == Lang::Ruby && (Name == "block" || Name == "do_block")) Add(Core::Role::InvocationBody);
	if(Lang == Lang::PHP && Names::PhpOpenTagEnd.HasName(Name)) Add(Core::Role::EmbeddedOpeningBoundary);
	if(Lang == Lang::Ruby && Name == "chained_string") Add(Core::Role::UnbrokenStrSeq);
	if(Lang == Lang::Ruby && Has(Role::ExpandContainer)) Add(Core::Role::KeywordExpandedContainer);
	if(Lang == Lang::Go && Has(Role::AngleBracketList)) Add(Core::Role::ExpandedTypeParams);
	if(Lang == Lang::Kotlin && Names::KotlinAccessor.HasName(Name)) Add(Core::Role::AttachedAccessor);
	if(Lang == Lang::Go && Name == "import_declaration") Add(Core::Role::GroupedImport);
	if(
		Lang.IsCFamily() &&
		(Has(Role::PreprocConditional) || Name == "preproc_elif" || Name == "preproc_elifdef" || Name == "preproc_else")
	) Add(Core::Role::DirectiveBlankBoundary);
	if(Lang == Lang::Swift && Name == "switch_statement") Add(Core::Role::SwitchBodyLayout);
	if(Lang == Lang::Kotlin && Has(Role::InlineDefBody) || Lang == Lang::Swift && Has(Role::OrdinaryFunctionDef)) {
		Add(Core::Role::DirectBraceLayout);
	}
	if(Lang == Lang::Swift && Names::SwiftBraceBodyHost.HasName(Name)) Add(Core::Role::DirectControlBraceLayout);
	if(Lang == Lang::PHP && Has(Role::ColonBodyHost)) Add(Core::Role::AlternateBodyLayout);
	if(Lang == Lang::Go && Names::GoSwitch.HasName(Name)) Add(Core::Role::TypedSwitchLayout);
	if(Lang == Lang::Rust && Name == "empty_statement") Add(Core::Role::AttachedEmptyStmt);
	if(Lang != Lang::Ruby && Has(Role::InheritedClass)) Add(Core::Role::NonKeywordClass);
	if(Lang == Lang::CSharp && Name == "implicit_type") Add(Core::Role::DeconstructionType);
	if(Lang == Lang::Python && Name == "binary_operator") Add(Core::Role::AnnotationUnionOperator);
	if(Lang == Lang::Ruby && Has(Role::NumericUnitSuffix)) Add(Core::Role::AttachedNumericUnit);
	if(Lang == Lang::Swift && Names::RangeOrUnaryExpr.HasName(Name)) Add(Core::Role::AttachedRangeOrUnary);
	if(Lang == Lang::Rust && Name == "assignment_expression") Add(Core::Role::RangeAssignment);
	if(Lang == Lang::Rust && Name == "label") Add(Core::Role::AttachedLabel);
	if(Lang == Lang::HTML && Name == "start_tag") Add(Core::Role::SpacedAttributeTag);
	if(Lang == Lang::CSharp && Name == "lambda_expression") Add(Core::Role::TypedLambda);
	if(Lang == Lang::Kotlin && Name == "constructor_invocation") Add(Core::Role::AnnotationConstructor);
	if(Lang == Lang::CSharp && Has(Role::PtrDecl)) Add(Core::Role::PostfixPtrType);
	if(Lang == Lang::Kotlin && Name == "spread_expression") Add(Core::Role::ArgSpread);
	if(Lang == Lang::Rust && Name == "range_expression") Add(Core::Role::OpenRangeOperand);
	if(Lang == Lang::CSS && Names::CssNamespacePrefix.HasName(Name)) Add(Core::Role::AttachedSelectorNamespace);
	if(Lang == Lang::Ruby && Name == "lambda") Add(Core::Role::AttachedArrowLambda);
	if(Lang == Lang::Cpp && Name == "if_statement") Add(Core::Role::ConstantCondHost);
	if(Lang == Lang::Swift && Name == "function_declaration") Add(Core::Role::OperatorFunctionDecl);
	if(Lang.IsCFamily() && Name == "binary_expression") Add(Core::Role::CastLookalikeOperator);
	if(Lang == Lang::Ruby && Name == "block_body") Add(Core::Role::ClosureStmts);
	if(Lang == Lang::Ruby && Names::RubyBlock.HasName(Name)) Add(Core::Role::KeywordControlLayout);
	if(Lang == Lang::Kotlin && Name == "if_expression") Add(Core::Role::BracedConditionalVal);
	if(Lang == Lang::Python && (Name == "binary_operator" || Name == "union_type" || Name == "type")) {
		Add(Core::Role::AnnotationUnionHost);
	}
	if(
		Lang == Lang::Python && (
			Has(Role::AssignmentOperation) || Name == "typed_parameter" || Name == "typed_default_parameter" || Name == "for_statement" ||
			Name == "union_type" || Name == "type" || Name == "binary_operator" || Name == "type_alias_statement"
		)
	) Add(Core::Role::ContinuationHeader);
	if(Lang == Lang::Swift && Names::SwiftHeaderStmt.HasName(Name)) Add(Core::Role::TightHeaderEquals);
	if(Lang == Lang::Swift && IsNamed && Name == "parameter") Add(Core::Role::TightParamEquals);
	if(Lang == Lang::Ruby && Names::RubyAnonymousForward.HasName(Name)) Add(Core::Role::TightAnonymousForward);
	if(
		Lang == Lang::HTML && (
			Name == "document" || Name == "element" || Name == "script_element" || Name == "style_element" || Has(Role::TagSyntax) ||
			Name == "attribute" || Name == "attribute_name" || Name == "attribute_value" || Name == "quoted_attribute_value" ||
			Name == "text" || Name == "raw_text" || Name == "entity" || Name == "doctype" || Name == "erroneous_end_tag"
		)
	) Add(Core::Role::MarkupSyntax);
	if(Lang == Lang::Kotlin && Name == "label") Add(Core::Role::PrefixLabel);
	if(Name == "then") Add(Core::Role::ThenSection);
	if(Name == "ensure") Add(Core::Role::EnsureSection);
	if(Name == "rescue") Add(Core::Role::RescueSection);
	if(Lang == Lang::Ruby && (Name == "assignment" || Name == "operator_assignment")) Add(Core::Role::KeywordValAssignment);
	if(Lang == Lang::Ruby && (Name == "binary" || Name == "conditional")) Add(Core::Role::CompactInterpolationOperator);
	if((Lang.IsCFamily() || Lang == Lang::CSharp || Lang == Lang::TypeScript) && Has(Role::MisparsedTemplateHost)) {
		Add(Core::Role::RecoverableTemplate);
	}
	if(Lang == Lang::Ruby && Names::HashSplat.HasName(Name)) Add(Core::Role::AttachedHashSplat);
	if(Lang == Lang::Ruby && Names::RubyLowUnary.HasName(Name)) Add(Core::Role::AttachedLowUnaryWord);
	if(Lang == Lang::PHP && Name == "reference_modifier") Add(Core::Role::AttachedRefModifier);
	if(Lang == Lang::PHP && Names::PhpNamespaceUse.HasName(Name)) Add(Core::Role::AttachedNamespaceImport);
	if(Lang == Lang::Kotlin && Name == "control_structure_body") Add(Core::Role::SpacedControlBody);
	if(Lang == Lang::Kotlin && Name == "prefix_expression") Add(Core::Role::LabeledPrefix);
	if(Lang == Lang::Kotlin && Names::KotlinNegatableTest.HasName(Name)) Add(Core::Role::AttachedNegationTest);
	if(Lang == Lang::Kotlin && Names::KotlinNumericLiteralSuffix.HasName(Name)) Add(Core::Role::AttachedMagnitudeSuffix);
	if(Lang == Lang::CSS && Name == "%") Add(Core::Role::AttachedPercent);
	// セレクタの接頭記号と名前は密着させ，実引数は其の子の役割で扱う
	if(Lang == Lang::CSS && (Names::CssSelectorAtomic.HasName(Name) || Has(Role::SelectorHost) && Has(Role::AttachColonOrDot))) {
		Add(Core::Role::AttachedSelector);
	}
	if((Lang == Lang::Swift || Lang == Lang::TypeScript) && Name == "!") Add(Core::Role::PostfixAssertionMark);
	if(Lang == Lang::Ruby && Has(Role::AttachParen)) Add(Core::Role::BareCallArgs);
	if(Lang == Lang::Ruby && Has(Role::UnaryPreOrUpdate)) Add(Core::Role::SignedOperandHost);
	if((Lang == Lang::Python || Lang.IsJsTs()) && Has(Role::NumLiteral)) Add(Core::Role::IntMemberBoundary);
	if(Name == "sizeof" || Lang == Lang::CSharp && Name == "typeof") Add(Core::Role::AttachedTypeQuery);
	if(Lang != Lang::Swift && Name == "function_declaration") Add(Core::Role::SpacedFunctionHead);
	if(Name == "infix_expression") Add(Core::Role::NamedInfix);
	if(Lang == Lang::Ruby && Names::RubyBraceBlock.HasName(Name)) Add(Core::Role::InlineBraceBody);
	if(Has(Role::RepeatedControl) && !(Lang == Lang::Swift && Name == "do_statement")) Add(Role::LabeledLoop);
	if(!(Lang == Lang::Swift && Name == "do_statement") && (IsLoopOrSwitch || Names::SwitchOrWithExpr.HasName(Name))) {
		Add(Role::NestedBreakTarget);
	}
	if(Names::ClassLikeStrict.HasName(Name)) {
		Add(Role::AccessOrderedClass);
		Add(Role::MemberOwner);
	}
	if(
		(Lang == Lang::CSharp || Lang == Lang::Java) && Name == "interface_declaration" ||
		Lang == Lang::Java && Name == "annotation_type_declaration"
	) Add(Role::ImplicitPublicMembers);
	if(Lang == Lang::Cpp && Name == "class_specifier") Add(Role::ImplicitPrivateClass);
	if(
		Name == (Lang == Lang::Java ? "class_body" : "declaration_list") ||
		(Lang == Lang::Kotlin || Lang == Lang::Swift || Lang == Lang::TypeScript) && Name == "class_body" ||
		Lang == Lang::Java && (Name == "annotation_type_body" || Name == "interface_body") ||
		(Lang == Lang::Kotlin || Lang == Lang::Swift) && Name == "enum_class_body" ||
		Lang == Lang::PHP && Name == "enum_declaration_list"
	) Add(Role::AccessOrderedBody);
	if(
		Name == (Lang == Lang::PHP ? "visibility_modifier" : "modifier") || Name == "visibility_modifier" ||
		Name == "accessibility_modifier"
	) Add(Role::AccessModifier);
	if((Lang.IsCFamily() || Lang == Lang::CSharp || Lang == Lang::Go || Lang == Lang::PHP) && Name == "goto_statement") {
		Add(Role::GotoInspection);
	}
	if(
		(Lang == Lang::Java || Lang.IsJsTs() || Lang == Lang::Go) && Name == "labeled_statement" ||
		(Lang == Lang::Rust || Lang == Lang::Kotlin) && Name == "label" || Lang == Lang::Swift && Name == "statement_label"
	) Add(Role::LabelInspection);
	if(
		Lang == Lang::Cpp && Name == "placeholder_type_specifier" || Lang == Lang::CSharp && Name == "implicit_type" ||
		Lang == Lang::Java && Name == "type_identifier" || Lang == Lang::TypeScript && Name == "predefined_type"
	) Add(Role::TypeInferenceInspection);
	if(
		Lang == Lang::PHP && Name == "method_declaration" ||
		Lang.IsJsTs() && (Name == "abstract_method_signature" || Name == "method_definition" || Name == "method_signature")
	) Add(Role::ReturnHintContext);
	// 型を記述出来る関数形だけを検査し，戻値型を書けないラムダは含めない
	if(
		Lang == Lang::C && Name == "function_definition" || Lang == Lang::Cpp && Name == "lambda_expression" ||
		Lang == Lang::CSharp && Name == "lambda_expression" ||
		Lang == Lang::Rust && (Name == "closure_expression" || Name == "function_item") ||
		Lang == Lang::Kotlin && (Name == "anonymous_function" || Name == "function_declaration") ||
		Lang == Lang::Swift && (Name == "function_declaration" || Name == "lambda_literal") || Lang == Lang::PHP &&
		(Name == "anonymous_function" || Name == "arrow_function" || Name == "function_definition" || Name == "method_declaration") ||
		Lang == Lang::TypeScript && (
			Name == "abstract_method_signature" || Name == "arrow_function" || Name == "call_signature" || Name == "construct_signature" ||
			Name == "function_declaration" || Name == "function_expression" || Name == "function_signature" ||
			Name == "generator_function" || Name == "generator_function_declaration" || Name == "method_definition" ||
			Name == "method_signature"
		) || Lang == Lang::Python && Name == "function_definition"
	) Add(Role::ReturnHintRequired);
	if(
		Lang == Lang::Go && (Name == "const_declaration" || Name == "short_var_declaration" || Name == "var_spec") ||
		Lang == Lang::Rust && Name == "let_declaration" ||
		(Lang == Lang::Kotlin || Lang == Lang::Swift) && (Name == "for_statement" || Name == "property_declaration") ||
		Lang == Lang::PHP && Name == "property_declaration" || Lang == Lang::TypeScript && (
			Name == "catch_clause" || Name == "property_signature" || Name == "public_field_definition" || Name == "variable_declarator"
		) || Lang == Lang::Python && Name == "assignment"
	) Add(Role::OptionalDeclType);
	if(Lang == Lang::Kotlin && Name == "multi_variable_declaration") Add(Role::TypeHintBindingGroup);
	if(Lang == Lang::Python && (Name == "global_statement" || Name == "nonlocal_statement")) Add(Role::TypeHintExternalBinding);
	if(
		Lang == Lang::Java && Name == "inferred_parameters" || Lang == Lang::Rust && Name == "closure_parameters" ||
		Lang == Lang::Kotlin && Name == "lambda_parameters" || Lang == Lang::Swift && Name == "lambda_function_type_parameters"
	) Add(Role::TypeHintParamContainer);
	if(
		Lang == Lang::CSharp && (Name == "implicit_parameter" || Name == "parameter") || Lang == Lang::Java && Name == "identifier" ||
		Lang == Lang::Rust && Names::RustParamPattern.HasName(Name) ||
		Lang == Lang::Kotlin && (Name == "multi_variable_declaration" || Name == "variable_declaration") ||
		Lang == Lang::Swift && Name == "lambda_parameter" ||
		Lang == Lang::PHP && (Name == "property_promotion_parameter" || Name == "simple_parameter" || Name == "variadic_parameter") ||
		Lang == Lang::TypeScript && (Name == "identifier" || Name == "optional_parameter" || Name == "required_parameter") ||
		Lang == Lang::Python &&
		(Name == "default_parameter" || Name == "dictionary_splat_pattern" || Name == "identifier" || Name == "list_splat_pattern")
	) Add(Role::OptionalParamType);
	if(
		Lang == Lang::Rust && Names::RustParamNameWrapper.HasName(Name) ||
		Lang == Lang::TypeScript && (Name == "optional_parameter" || Name == "required_parameter" || Name == "rest_pattern")
	) Add(Role::ParamNameWrapper);
	if(Lang.IsCFamily() && Name == "pointer_expression") Add(Role::AddressOperation);
	if(Lang.IsCFamily() && Name == "preproc_def") Add(Role::MacroConstantInspection);
	if(Lang.IsCFamily() && Name == "using_declaration") Add(Role::ScopeImport);
	if(Lang.IsJsTs() && Name == "binary_expression") Add(Role::StrictEqualityInspection);
	if(Lang == Lang::Cpp && !Lang.IsCShared && Name == "cast_expression") Add(Role::CastInspection);
	if(
		Has(Role::Conditional) || Has(Role::ConditionalAlternative) || Has(Role::ConditionalBranchClause) ||
		Lang == Lang::Ruby && (Name == "elsif" || Name == "if" || Name == "if_modifier")
	) Add(Role::CondMergeInspection);
	if(
		Has(Role::IdentifierLeafLike) || Lang == Lang::PHP && Name == "variable_name" ||
		Lang == Lang::Ruby && (Name == "instance_variable" || Name == "class_variable" || Name == "global_variable")
	) Add(Role::BoolFlipName);
	if(Has(Role::BoolFlipName) || Has(Role::FieldName)) Add(Role::NameToken);
	if(Has(Role::AssignmentOperation) || Name == "operator_assignment" || Name == "assignment_statement") {
		Add(Role::BoolFlipInspection);
		Add(Role::IncrementInspection);
	}
	if(Has(Role::PostfixOrUpdateExpr)) Add(Role::PostfixInspection);
	if(Name == "number" || Name == "number_literal" || Name == "integer") Add(Role::ZeroLiteral);
	// 括弧・波括弧・宣言の正規化
	if(Names::RubyLowUnary.HasName(Name)) Add(Role::LowPrecUnaryKeyword);
	if(Names::RubyPatternTest.HasName(Name)) Add(Role::LowPrecPatternTest);
	if(Names::TypeColonParent.HasName(Name)) Add(Role::TypeColonParent);
	if(Names::OpenPrefixExpr.HasName(Name)) Add(Role::OpenPrefixExpr);
	if(Names::KeywordUnary.HasName(Name)) Add(Role::KeywordUnary);
	if(Names::SwitchOrWithExpr.HasName(Name)) Add(Role::SwitchOrWithExpr);
	if(Names::PythonPostfix.HasName(Name)) Add(Role::CallAttributeOrSubscript);
	if(Names::IterationHeader.HasName(Name)) Add(Role::IterationHeader);
	if(Names::PostfixReceiverLeaf.HasName(Name)) Add(Role::PostfixReceiverLeaf);
	if(Names::PostfixCallHost.HasName(Name)) Add(Role::PostfixCallHost);
	if(Names::GoTypeName.HasName(Name)) Add(Role::CompositeTypeName);
	if(Names::ParenInnerNoUnwrap.HasName(Name)) Add(Role::ParenInnerNoUnwrap);
	if(Names::RubyStmtHost.HasName(Name)) Add(Role::CommandStmtHost);
	if(Names::TypeArgLookalikeSlot.HasName(Name)) Add(Role::TypeArgLookalikeSlot);
	if(Names::JsInRestoring.HasName(Name)) Add(Role::MembershipExprBoundary);
	if(Names::PythonNum.HasName(Name)) Add(Role::AttributeSensitiveNum);
	if(Names::ReturnValParent.HasName(Name)) Add(Role::ReturnValParent);
	if(Names::ReturnValHost.HasName(Name)) Add(Role::ReturnValHost);
	if(Names::ForEachHeader.HasName(Name)) Add(Role::ForEachHeader);
	if(Names::AwaitTryCastExpr.HasName(Name)) Add(Role::AwaitTryCastExpr);
	if(Names::CsPostfixChain.HasName(Name)) Add(Role::ConditionalAccessChain);
	if(Names::CCastInner.HasName(Name)) Add(Role::CastTypePart);
	if(Names::ParenValSlot.HasName(Name)) Add(Role::ParenValSlot);
	if(Names::ExprStmtLiteral.HasName(Name)) Add(Role::ExprStmtLiteral);
	if(Names::GreedyTailExpr.HasName(Name)) Add(Role::GreedyTailExpr);
	if(Names::ParenInnerFirstChildNoUnwrap.HasName(Name)) Add(Role::ParenInnerFirstChildNoUnwrap);
	if(Names::RustBlockLike.HasName(Name)) Add(Role::StmtEndingBlock);
	if(Names::RustStmtHead.HasName(Name)) Add(Role::BlockExprStmtHost);
	if(Names::RustValJump.HasName(Name)) Add(Role::OptionalValJump);
	if(Names::RangeOrUnaryExpr.HasName(Name)) Add(Role::RangeOrUnaryExpr);
	if(Names::TypeCastRight.HasName(Name)) Add(Role::TypeCastRight);
	if(Names::GroupingTypeParen.HasName(Name)) Add(Role::GroupingTypeParen);
	if(Names::TsGreedyType.HasName(Name)) Add(Role::GreedyType);
	if(Names::TsFunctionType.HasName(Name)) Add(Role::CallableType);
	if(Names::SwiftTypeOperatorHost.HasName(Name)) Add(Role::ComposedTypeOperandHost);
	if(Lang == Lang::Kotlin && Names::KotlinTypeOperatorHost.HasName(Name)) Add(Role::NullableTypeOperandHost);
	if(Lang == Lang::Rust && Names::RustTypeOperatorHost.HasName(Name)) Add(Role::BorrowedTypeOperandHost);
	if(Names::GoConversionType.HasName(Name)) Add(Role::UnambiguousConversionType);
	if(Names::TypeOperatorInner.HasName(Name)) Add(Role::TypeOperatorInner);
	if(Name == "tuple") Add(Role::TupleVal);
	if(Name == "tuple_pattern") Add(Role::TuplePattern);
	if(Name == "case_pattern") Add(Role::CasePattern);
	if(Name == "as_pattern") Add(Role::AliasPattern);
	if(Name == "union_pattern") Add(Role::AlternativePattern);
	if(Name == "list_splat") Add(Role::SeqExpansion);
	if(Name == "named_expression") Add(Role::NamedVal);
	if(Lang == Lang::Python && Name == "generator_expression") Add(Role::GeneratorExpr);
	if(Lang == Lang::Python && (Name == "argument_list" || Name == "list" || Name == "tuple")) {
		Add(Role::UnparenthesizedBindingValSlot);
	}
	if(Lang.IsJsTs() && (Name == "function_expression" || Name == "generator_function" || Name == "class")) {
		Add(Role::PostfixReceiverLeaf);
	}
	if(Name == "for_in_clause") Add(Role::ComprehensionIteration);
	if(
		Lang == Lang::Python && (
			Name == "dictionary_comprehension" || Name == "generator_expression" || Name == "list_comprehension" ||
			Name == "set_comprehension"
		)
	) Add(Role::ComprehensionScope);
	if(Lang == Lang::PHP && Name == "assignment_expression" || Lang == Lang::Ruby && Name == "assignment") {
		Add(Role::FirstAssignmentBinding);
	}
	if((Lang == Lang::PHP || Lang == Lang::Ruby) && Has(Role::FunctionScope)) Add(Role::FunctionBindingScope);
	if(Lang == Lang::Ruby && Name == "argument_list") Add(Role::ReturnValList);
	if(
		Lang == Lang::Ruby && (Name == "block" || Name == "do_block" || Name == "lambda") || Lang == Lang::PHP && (
			Name == "by_ref" || Name == "dynamic_variable_name" || Name == "function_static_declaration" || Name == "global_declaration" ||
			Name == "include_expression" || Name == "include_once_expression" || Name == "reference_assignment_expression" ||
			Name == "require_expression" || Name == "require_once_expression"
		)
	) Add(Role::UncertainLocalBindings);
	if(Name == "delete_statement") Add(Role::DeleteStmt);
	if(Name == "member_expression") Add(Role::MemberExpr);
	if(Name == "prefix_expression") Add(Role::PrefixExpr);
	if(Name == "open_start_range_expression") Add(Role::RangeWithoutStart);
	if(Name == "update_expression") Add(Role::UpdateExpr);
	if(Name == "open_end_range_expression") Add(Role::RangeWithoutEnd);
	if(Name == "invocation_expression") Add(Role::InvocationExpr);
	if(Name == "await") Add(Role::AwaitVal);
	if(Name == "conditional_expression") Add(Role::ConditionalExpr);
	if(Name == "not_operator") Add(Role::LogicalNegation);
	if(Name == "field_expression") Add(Role::FieldExpr);
	if(Name == "navigation_suffix") Add(Role::NavigationSuffix);
	if(Name == "type_cast_expression") Add(Role::TrailingTypeCast);
	if(Name == "cast_expression") Add(Role::CastExpr);
	if(Name == "abstract_pointer_declarator") Add(Role::AbstractPtr);
	if(Name == "abstract_function_declarator") Add(Role::AbstractFunction);
	if(Name == "abstract_array_declarator") Add(Role::AbstractArray);
	if(Name == "if_clause") Add(Role::ComprehensionFilter);
	if(Name == "for_in_statement") Add(Role::ForInStmt);
	if(Name == "decltype") Add(Role::ValCategoryType);
	if(Name == "type_switch_statement") Add(Role::TypeSwitch);
	if(Name == "conditional_access_expression") Add(Role::ConditionalAccessExpr);
	if(Name == "argument") Add(Role::Arg);
	if(Name == "auto") Add(Role::DeducedTypeKeyword);
	if(Name == "ternary_expression") Add(Role::TernaryExpr);
	if(Name == "fold_expression") Add(Role::FoldExpr);
	if(Name == "range_expression") Add(Role::RangeExpr);
	if(Name == "tuple_type_item") Add(Role::TupleTypeItem);
	if(Name == "type_pack_expansion") Add(Role::TypePackExpansion);
	if(Name == "conditional_type") Add(Role::ConditionalType);
	if(Name == "intersection_type") Add(Role::IntersectionType);
	if(Name == "infer_type") Add(Role::InferredType);
	if(Name == "protocol_composition_type") Add(Role::ProtocolComposition);
	if(Name == "type_conversion_expression") Add(Role::TypeConversion);
	if(Name == "value_binding_pattern") Add(Role::ValBindingPattern);
	if(Name == "reference_modifier") Add(Role::RefModifier);
	if(Names::DeclaratorHead.HasName(Name)) Add(Role::DeclaratorHead);
	if(Names::JsMemberTarget.HasName(Name)) Add(Role::MemberMutationTarget);
	if(Names::JsBindingName.HasName(Name)) Add(Role::BindingName);
	if(Names::RustBaseTraversal.HasName(Name)) Add(Role::MutationBaseTraversal);
	if(Names::RustRefPattern.HasName(Name)) Add(Role::RefBindingPattern);
	if(Names::JavaTypeBody.HasName(Name)) Add(Role::ParamScopeBoundary);
	if(Names::JavaParamScope.HasName(Name)) Add(Role::ParamScope);
	if(Names::WritableAssignment.HasName(Name)) Add(Role::WritableAssignment);
	if(Name == "attribute_item") Add(Role::OuterExpansionAttribute);
	if(Name == "inner_attribute_item") Add(Role::InnerExpansionAttribute);
	if(Name == "import_statement" || Lang == Lang::Python && Name == "future_import_statement") Add(Role::ImportStmt);
	if(Lang == Lang::Python) {
		if(Name == "import_from_statement") Add(Role::ImportedMemberBinding);
		else if(Name == "import_statement") Add(Role::ImportedModuleBinding);
	}
	if(
		Lang.IsCFamily() && Name == "preproc_include" ||
		Lang == Lang::Cpp && (Name == "using_declaration" || Name == "namespace_alias_definition") ||
		Lang == Lang::CSharp && Name == "using_directive" || Lang == Lang::Java && Name == "import_declaration" ||
		Lang == Lang::Rust && Name == "use_declaration" || Lang == Lang::Kotlin && Name == "import_header" ||
		Lang == Lang::Swift && Name == "import_declaration" || Lang == Lang::PHP && Name == "namespace_use_declaration"
	) Add(Role::ImportedPathBinding);
	if(Lang == Lang::Rust && (Name == "use_list" || Name == "scoped_use_list")) Add(Role::ImportPathGroup);
	if(Lang == Lang::PHP) {
		if(Name == "namespace_use_declaration" || Name == "namespace_use_group") Add(Role::ImportPathGroup);
		if(Name == "namespace_use_clause") Add(Role::ImportPathItem);
		if(!IsNamed && Name == "function") Add(Role::ImportKindMarker);
	}
	if(Lang == Lang::Rust && Name == "self") Add(Role::ImportSelfName);
	if(Lang == Lang::Kotlin && Name == "import_alias") Add(Role::ImportAlias);
	if(Lang == Lang::CSharp && Name == "using_directive") Add(Role::ImportAliasOnly);
	if(Name == "use_wildcard" || Name == "wildcard_import" || Name == "asterisk") Add(Role::ImportWildcard);
	if(Lang == Lang::Swift) {
		if(Name == "import_declaration") Add(Role::ImportRequiresKind);
		if(
			!IsNamed && (
				Name == "func" || Name == "class" || Name == "struct" || Name == "enum" || Name == "protocol" || Name == "typealias" ||
				Name == "let" || Name == "var"
			)
		) Add(Role::ImportKindMarker);
	}
	if(Lang == Lang::Cpp && !IsNamed && (Name == "namespace" || Name == "enum")) Add(Role::ImportAllNamesMarker);
	if(Name == "ref_pattern") Add(Role::RefPattern);
	if(Name == "mut_pattern") Add(Role::MutablePattern);
	if(Name == "compact_constructor_declaration") Add(Role::CompactConstructor);
	if(Name == "record_declaration") Add(Role::RecordDecl);
	if(Name == "captured_pattern") Add(Role::CapturedPattern);
	if(Name == "let_condition") Add(Role::BindingCond);
	if(Name == "closure_expression") Add(Role::ClosureExpr);
	if(Name == "global_statement") Add(Role::GlobalStmt);
	if(Name == "mutable_specifier") Add(Role::MutableSpecifier);
	if(Name == "binding_pattern_kind") Add(Role::BindingMarker);
	if(Lang == Lang::Swift && Name == "infix_expression") Add(Role::CmpInfix);
	if(Lang == Lang::Swift && Name == "prefix_expression") Add(Role::PrefixAbsorbsInfix);
	if(Lang == Lang::Swift && Name == "call_expression") Add(Role::OperatorCall);
	if(Lang == Lang::Kotlin && Name == "infix_expression") Add(Role::NamedInfixCall);
	if(Lang == Lang::Swift && Name == "as_expression") Add(Role::ComposedTypeCast);
	if(Lang == Lang::Ruby && Name == "call") Add(Role::CommandCall);
	if((Lang == Lang::CSharp || Lang == Lang::Java) && Name == "object_creation_expression") Add(Role::AtomicConstruction);
	if((Lang == Lang::CSharp || Lang == Lang::Kotlin || Lang == Lang::Swift) && Has(Role::NumLiteral)) Add(Role::AtomicNum);
	if(Lang == Lang::Rust && Name == "try_expression") Add(Role::PostfixTry);
	if(Lang == Lang::Swift && Name == "?") Add(Role::OptionalNavigationMark);
	if(Lang == Lang::Rust && Name == "type_cast_expression") Add(Role::UnaryReceiverCast);
	if(Lang == Lang::Rust && Has(Role::AwaitTryCastExpr)) Add(Role::PostfixValOperation);
	if(Lang == Lang::Rust && Name == "await_expression") Add(Role::PostfixAwait);
	if(Lang == Lang::CSharp && Name == "switch_expression") Add(Role::UnwrappedSwitchSubject);
	if(Lang == Lang::Go && Name == "unary_expression") Add(Role::ChannelReceive);
	if(Lang == Lang::Cpp && Names::DecltypeIdExpr.HasName(Name)) Add(Role::DeducedValCategoryName);
	if(Lang == Lang::CSharp && Name == "interpolation") Add(Role::ColonSensitiveInterpolation);
	if(Lang == Lang::Python && Name == "conditional_expression") Add(Role::ConditionalTailOnly);
	if(Lang == Lang::Ruby && (Name == "unary" || Has(Role::LowPrecPatternTest))) Add(Role::LowKeywordPrec);
	if((Lang.IsJsTs() || Lang == Lang::CSharp) && Name == "await_expression") Add(Role::PrefixAwait);
	if(Lang == Lang::TypeScript && Name == "await_expression") Add(Role::PrefixAbsorbsTypeCast);
	if(Lang.IsJsTs() && Has(Role::PostfixReceiverChain)) Add(Role::AwaitNameReceiver);
	if(Lang == Lang::CSharp && (Name == "invocation_expression" || Has(Role::SwitchOrWithExpr))) Add(Role::SwitchWithPrec);
	if(
		Lang == Lang::Python &&
		(Name == "await" || Has(Role::CallAttributeOrSubscript) || Name == "conditional_expression" || Name == "not_operator")
	) Add(Role::KeywordExprPrec);
	if(
		(Lang == Lang::Rust || Lang == Lang::Kotlin || Lang == Lang::PHP) && Names::FieldCallee.HasName(Name) ||
		Lang == Lang::Kotlin && Name == "simple_identifier" || Lang == Lang::PHP && Name == "name"
	) Add(Role::CalleeNeedsGrouping);
	if(Lang.IsJsTs() && Has(Role::GroupingParen)) Add(Role::ConstructionReceiverParen);
	if(Lang == Lang::Ruby && Has(Role::GroupingParen)) Add(Role::CommandArgParen);
	if(Lang == Lang::Python && (Name == "if_clause" || Name == "attribute" || Name == "await")) Add(Role::RestrictedExprOperand);
	if(Lang == Lang::PHP && Has(Role::GroupingParen)) Add(Role::RefValParen);
	if(Lang == Lang::PHP && Has(Role::ForEachHeader)) Add(Role::DelimitedIterationTarget);
	if(
		(Lang == Lang::Go || Lang == Lang::Rust || Lang == Lang::Swift || Lang == Lang::Ruby || Lang == Lang::Python) &&
		Names::ParenOptionalHeader.HasName(Name)
	) Add(Role::OptionalCondParen);
	if(Lang.IsCFamily() && Name == "cast_expression") Add(Role::KeywordCastOperand);
	if(Lang == Lang::CSharp && Has(Role::GroupingParen)) Add(Role::ConditionalAccessParen);
	if((Lang == Lang::Cpp || Lang == Lang::CSharp || Lang == Lang::TypeScript) && Has(Role::GroupingParen)) {
		Add(Role::TypeArgAmbiguousParen);
	}
	if((Lang.IsCFamily() || Lang == Lang::CSharp) && Name == "cast_expression") Add(Role::CastBeforeUnary);
	if(Lang.IsCFamily() && Has(Role::GroupingParen)) Add(Role::CastAmbiguousParen);
	if(Lang != Lang::Swift || Names::SwiftHeaderExprHost.HasName(Name)) Add(Role::BodyAmbiguousHeader);
	if(Lang == Lang::Rust && Has(Role::GroupingParen)) Add(Role::BlockEndingParen);
	if(Lang == Lang::Swift && Has(Role::GroupingParen)) Add(Role::OperatorBoundaryParen);
	if(Lang == Lang::Go && Name == "composite_literal") Add(Role::NamedCompositeBody);
	if(Lang == Lang::Rust && Name == "struct_expression" || Lang == Lang::Swift && Name == "lambda_literal") {
		Add(Role::HeaderBodyLikeVal);
	}
	if(Lang == Lang::Swift && Name == "tuple_expression") Add(Role::TransparentTupleParen);
	if(Lang == Lang::TypeScript && Has(Role::TypeCastRight)) Add(Role::LeftAbsorbingTypeCast);
	if((Lang.IsJsTs() || Lang == Lang::Swift) && Has(Role::TypeCastRight)) Add(Role::ReopenableTypeCast);
	if(Lang == Lang::Swift && Has(Role::GroupingTypeParen)) Add(Role::WrappedTypeItem);
	if(Lang.IsJsTs() && Has(Role::GroupingTypeParen)) Add(Role::AlgebraicTypeParen);
	if(
		(Lang.IsJsTs() || Lang == Lang::Go || Lang == Lang::Rust || Lang == Lang::Kotlin || Lang == Lang::Swift) &&
		Has(Role::GroupingTypeParen)
	) Add(Role::RemovableTypeParen);
	if(Lang == Lang::Swift && Name == "function_type") Add(Role::TupleParamFunctionType);
	if(Lang == Lang::Go && Has(Role::GroupingTypeParen)) Add(Role::ConversionTypeParen);
	if(Lang == Lang::Swift && (Name == "pattern" || Has(Role::TightHeaderEquals))) Add(Role::RemovablePatternParen);
	if((Lang == Lang::Go || Lang == Lang::Rust || Lang == Lang::Swift) && Has(Role::Control)) Add(Role::BodyAmbiguousControl);
	if(Lang == Lang::Java && Name == "local_variable_declaration") Add(Role::FinalizableLocal);
	if(Lang == Lang::Kotlin && Name == "property_declaration") Add(Role::ReplaceableBindingKeyword);
	if(Lang == Lang::Kotlin && Name == "do_while_statement") Add(Role::NestedLoopBodyBraces);
	if(Name == "catch_type") Add(Role::CatchTypeAlternatives);
	if(Name == "disjunctive_normal_form_type") Add(Role::NormalFormType);
	if(Name == "not_nullable_type") Add(Role::NonNullableType);
	if(Name == "type_bound") Add(Role::TypeBounds);
	if(Name == "type_elem") Add(Role::TypeElements);
	if(Name == "type_list") Add(Role::TypeList);
	if(Name == "pattern") Add(Role::Pattern);
	if(Lang == Lang::Rust && Name == "identifier") Add(Role::NormalizedBindingName);
	if(Lang == Lang::Rust && Name == "reference_expression") Add(Role::BorrowExpr);
	if(Lang == Lang::Rust && Name == "call_expression") Add(Role::ReceiverMutationCall);
	if(Lang == Lang::Rust && Name == "match_expression") Add(Role::BorrowingMatch);
	if(Lang == Lang::Rust && Names::RustLetBinding.HasName(Name)) Add(Role::MutablePatternBinding);
	// 入力側の読取（整形仕様 4.3，入力側と検証側はノード名を照合せず此の役割を読む）
	if(Lang == Lang::Ruby) {
		if(Names::RubyParams.HasName(Name)) Add(Role::LocalParams);
		if(Names::RubyBinding.HasName(Name)) Add(Role::BindingExpr);
		if(Names::RubyLocalScope.HasName(Name)) Add(Role::LocalScope);
		if(Names::RubyFreshScope.HasName(Name)) Add(Role::IsolatedScope);
		if(Names::RubyCommandArgHost.HasName(Name)) Add(Role::CommandArgHost);
		if(Names::RubyArgExtent.HasName(Name)) Add(Role::CommandArgExtent);
		if(!IsNamed && Names::RubyLowBinary.HasName(Name)) Add(Role::LowPrecBinaryKeyword);
		if(Names::RubyArgSeq.HasName(Name)) Add(Role::ElementSeq);
		if(IsNamed && Names::RubyArgListHost.HasName(Name)) Add(Role::ArgListHost);
		if(Names::RubyArgKeyword.HasName(Name)) Add(Role::ValTakingJump);
		if(!IsNamed && Names::RubyArgStart.HasName(Name)) Add(Role::ArgStartToken);
	}
	if(!IsNamed && Names::ContinuingKeyword.HasName(Name)) Add(Role::ContinuingWord);
	if(Lang.IsJsTs() && !IsNamed && Names::JsRestrictedKeyword.HasName(Name)) Add(Role::RestrictedLineWord);
	if(Names::MacroBody.HasName(Name)) Add(Role::MacroBody);
	if(Names::MacroDef.HasName(Name)) Add(Role::MacroDef);
	if(Names::PreprocNameQuery.HasName(Name)) Add(Role::MacroNameQuery);
	if(
		Lang.IsCFamily() && Names::CNameDecl.HasName(Name) ||
		Lang == Lang::CSharp && (Name == "parameter" || Name == "variable_declarator")
	) Add(Role::NameDecl);
	if(Lang == Lang::CSharp && Name == "parameter") Add(Role::ParamDecl);
	if(
		Lang == Lang::CSharp && (
			Has(Role::ClassLike) && Name != "namespace_declaration" || Name == "delegate_declaration" || Name == "type_parameter" ||
			Name == "using_directive"
		)
	) Add(Role::BareTypeDecl);
	if(Lang == Lang::CSharp && Name == "namespace_declaration") Add(Role::NamespaceDef);
	if(Lang.IsCFamily() && Name == "declaration") Add(Role::PotentialExprDecl);
	// C の構造体・共用体・列挙の名前は `struct` 等を伴わないと型に為らない為，指定子が裸の型名を導入するのは C++ だけ
	if(Lang.IsCFamily() && Names::CTypeAliasDecl.HasName(Name) || Lang == Lang::Cpp && Names::CppTypeSpecifier.HasName(Name)) {
		Add(Role::BareTypeDecl);
	}
	if(Names::Leaf.HasName(Name)) Add(Role::SrcLeaf);
	if(Names::Preproc.HasName(Name)) Add(Role::PreprocNode);
	if(Lang == Lang::Go && Names::GoDocumentedDecl.HasName(Name)) Add(Role::DocumentedTopDecl);
	if(Names::MultilineCommaContainer.HasName(Name)) Add(Role::DelimitedList);
	if(Lang == Lang::CSS && Names::CssDelimitedList.HasName(Name)) Add(Role::BreakableDelimiterInterior);
	if(Lang == Lang::CSS) {
		// モジュールの節は語を区切り，別名と修飾したミックスイン名は１トークンとして扱う
		if(Names::ScssModuleClause.HasName(Name)) Add(Role::SpaceKeyword);
		if(Names::ScssNameAtom.HasName(Name)) Add(Role::NameToken);
		if(Name == "variable_flag") Add(Role::NamedKeyword);
		// 単一要素のカンマはリストの種類を決める為，組と同じ保持条件へ投影する
		if(Name == "list_value") Add(Role::TupleLike);
	}
	if(Lang == Lang::CSS && Names::ScssParamHost.HasName(Name)) Add(Role::ParamRule);
	if(Lang == Lang::CSS && Names::ScssModuleRule.HasName(Name)) Add(Role::ModuleRule);
	if(Lang.IsCFamily() && Names::CDeclaratorWrapper.HasName(Name)) Add(Role::NestingDeclarator);
	if(Lang == Lang::Cpp && Names::CppScopeBoundary.HasName(Name)) Add(Role::TemplateScanBoundary);
	if(
		Lang == Lang::Kotlin && (Name == "constructor_invocation" || Name == "spread_expression") ||
		Lang == Lang::Ruby && Name == "unary"
	) Add(Role::AdjacencyRecordingHost);
	if(Lang == Lang::Ruby && Name == "argument_list") Add(Role::AdjacencyRecordingOperand);
	if(Names::ReturnStmt.HasName(Name)) Add(Role::ReturningStmt);
	if(Lang == Lang::Go && Names::GoFunctionDecl.HasName(Name)) Add(Role::PragmaTargetFunction);
	if(Names::ClauseBody.HasName(Name)) Add(Role::BodyEndingClause);
	if(Names::BraceClause.HasName(Name)) Add(Role::FollowingBodyClause);
	if(Lang == Lang::PHP && Names::PhpIfBranch.HasName(Name)) Add(Role::IfFollowingBranch);
	if(Lang == Lang::Ruby && Names::RubyDelimitedLiteral.HasName(Name)) Add(Role::DelimitedLiteral);
	if(Names::SequentialStmtContainer.HasName(Name)) Add(Role::SequentialStmts);
	if(Names::TransparentWrap.HasName(Name)) Add(Role::TransparentWrapper);
	if(Names::JsStatementBlockOrBlock.HasName(Name)) Add(Role::BracedStmtBlock);
	if(Names::ThenDo.HasName(Name)) Add(Role::BodyTriggerWord);
	if(Lang.IsJsTs() && Names::JsMemberHost.HasName(Name)) Add(Role::OperandHeadedAccess);
	if(Lang == Lang::HTML && Names::HtmlRawTextElement.HasName(Name)) Add(Role::EmbeddedCodeElement);
	if(Lang == Lang::Java && Names::JavaEscapeHost.HasName(Name)) Add(Role::UnicodeEscapeHost);
	if(Lang == Lang::Java && Names::JavaExhaustiveCaseLabel.HasName(Name)) Add(Role::ExhaustiveCaseLabel);
	if(Lang == Lang::HTML && Names::HtmlElementWrap.HasName(Name)) Add(Role::MarkupElementNode);
	if(Lang.IsJsTs() && Names::JsStatementSequence.HasName(Name)) Add(Role::StmtOrMemberSeq);
	if(Lang == Lang::Swift) {
		if(Names::SwiftLiteralText.HasName(Name)) Add(Role::OpaqueLiteral);
		if(Has(Role::TightHeaderEquals)) Add(Role::BracedHeaderStmt);
		if(Names::SwiftStrayBlock.HasName(Name)) Add(Role::StrayBlock);
		if(!IsNamed && Names::SwiftHeaderLead.HasName(Name)) Add(Role::HeaderExprLead);
	}
	if(Lang == Lang::CSharp && Names::CsNameType.HasName(Name)) Add(Role::ExprTypeName);
	if(Lang.IsJsTs() && Names::JsModuleStatement.HasName(Name)) Add(Role::ModuleStmt);
	if(Lang.IsJsTs() && Name == "html_comment") Add(Role::UnsupportedComment);
	if(Name == "line_continuation") Add(Role::LineContinuation);
	if(Name == "array_rank_specifier") Add(Role::ArrayRank);
	if(Name == "array_type") Add(Role::ArrayTypeSyntax);
	if(Name == "block_parameters") Add(Role::BlockParamList);
	if(Name == "callable_reference") Add(Role::CallableRef);
	if(Name == "comment") Add(Role::CommentNode);
	if(Name == "composite_literal") Add(Role::CompositeLiteral);
	if(Name == "const_declaration") Add(Role::ConstantDeclNode);
	if(Name == "const_spec") Add(Role::ConstantSpec);
	if(Name == "declaration_expression") Add(Role::DeclExpr);
	if(Name == "doctype") Add(Role::DocTypeDecl);
	if(Name == "enumerator") Add(Role::EnumerationItem);
	if(Name == "erroneous_end_tag") Add(Role::ErroneousEndTag);
	if(Name == "func_literal") Add(Role::FunctionLiteral);
	if(Name == "function_declaration") Add(Role::FunctionDeclNode);
	if(Name == "heredoc_body") Add(Role::HeredocBody);
	if(Name == "if_statement") Add(Role::IfStmtNode);
	if(Name == "import_declaration") Add(Role::ImportDeclNode);
	if(Name == "include_statement") Add(Role::IncludeStmtNode);
	if(Name == "integer_value") Add(Role::IntLiteralVal);
	if(Name == "interpolation_format_clause") Add(Role::InterpolationFormat);
	if(Name == "label_name") Add(Role::LabelName);
	if(Name == "left_assignment_list") Add(Role::LeftAssignmentList);
	if(Name == "line_comment") Add(Role::LineCommentNode);
	if(Name == "metatype") Add(Role::MetatypeNode);
	if(Name == "object_literal") Add(Role::ObjectLiteralNode);
	if(Name == "placeholder") Add(Role::PlaceholderNode);
	if(Name == "prefix_unary_expression") Add(Role::PrefixUnaryOperation);
	if(Name == "preproc_call") Add(Role::PreprocCall);
	if(Name == "preproc_function_def") Add(Role::PreprocFunctionDef);
	if(Name == "preproc_include") Add(Role::PreprocInclude);
	if(Name == "property_name") Add(Role::PropertyNameNode);
	if(Name == "qualified_name") Add(Role::QualifiedNameNode);
	if(Name == "raw_text") Add(Role::RawTextNode);
	if(Name == "receive_statement") Add(Role::ReceiveStmt);
	if(Name == "regex_literal") Add(Role::RegexLiteralNode);
	if(Name == "rule_set") Add(Role::SelectorRule);
	if(Name == "scoped_identifier") Add(Role::ScopedIdentifier);
	if(Name == "selectors") Add(Role::SelectorList);
	if(Name == "self_closing_tag") Add(Role::SelfClosingTag);
	if(Name == "short_var_declaration") Add(Role::ShortVarDecl);
	if(Name == "source_file") Add(Role::SrcFileNode);
	if(Name == "string_literal") Add(Role::StrLiteralNode);
	if(Name == "struct_expression") Add(Role::StructExpr);
	if(Name == "super") Add(Role::SuperRef);
	if(Name == "var_declaration") Add(Role::VarDeclNode);
	if(Name == "var_spec") Add(Role::VarSpec);
	if(Name == "var_spec_list") Add(Role::VarSpecList);
	if(Name == "parameter_modifiers") Add(Role::ParamModifierList);
	if(Name == "variadic_parameter_declaration") Add(Role::VariadicParamDecl);
	if(
		Name == "variadic_parameter_declaration" || Name == "variadic_parameter" || Name == "spread_parameter" ||
		Name == "rest_pattern" || Name == "list_splat_pattern" || Name == "dictionary_splat_pattern" || Name == "splat_parameter" ||
		Name == "hash_splat_parameter" ||
		!IsNamed && (Name == "..." || Lang == Lang::Kotlin && Name == "vararg" || Lang == Lang::CSharp && Name == "params")
	) Add(Role::RestParamPart);
	if(Name == "when_condition") Add(Role::WhenCond);
	if(Name == "where_clause") Add(Role::WhereClauseNode);
	if(Name == "(") Add(Role::OpeningParenToken);
	if(Name == "}") Add(Role::ClosingBraceToken);
	if(Name == ">") Add(Role::GreaterThanToken);
	if(Name == "<") Add(Role::LessThanToken);
	if(Name == "::") Add(Role::DoubleColonToken);
	if(Name == "=") Add(Role::AssignmentToken);
	if(Name == ":=") Add(Role::ShortAssignmentToken);
	if(Name == ".") Add(Role::DotToken);
	if(Name == ",") Add(Role::CommaToken);
	if(Name == "arrow_function") Add(Role::ArrowFunctionNode);
	if(Name == "tag_name") Add(Role::TagNameNode);
	if(Name == "php_end_tag") Add(Role::PhpEndTag);
	if(Name == "start_tag") Add(Role::StartTag);
	if(Name == "non_null_expression") Add(Role::NonNullExpr);
	if(Name == "subscript_expression") Add(Role::SubscriptExprNode);
	if(Name == "macro_rules!") Add(Role::MacroRulesToken);
	if(Name == "[") Add(Role::OpeningBracketToken);
	if(Name.starts_with('>')) Add(Role::AngleCloseToken);
	if(Lang == Lang::Swift && Name == "infix_expression") Add(Role::SpacingSensitiveInfix);
	if(Lang == Lang::CSS && Name == "ERROR") Add(Role::LaggingGrammarError);
	if(Lang == Lang::Python && Name == "type_alias_statement") Add(Role::TypeAliasStmt);
	if(Lang == Lang::Python && Name == "string") Add(Role::VerbatimQuotedStr);
	if(Lang == Lang::Python && Name == "interpolation") Add(Role::DebugInterpolation);
	if(Lang == Lang::Rust && Name == "macro_definition") Add(Role::MacroRuleDef);
	if(Lang.IsCFamily() && Name == "template_type") Add(Role::TemplateInstantiationType);
	if(Lang == Lang::Cpp && Name == "template_declaration") Add(Role::TypeParamScope);
	if(Lang == Lang::Cpp && Name == "namespace_identifier") Add(Role::NamespaceName);
	if(Lang == Lang::Go && Name == "short_var_declaration") Add(Role::ReassigningShortDecl);
	if(Lang == Lang::Go && Name == ":=") Add(Role::TrailingOperatorToken);
	if(Lang == Lang::Go && Name == "send_statement") {
		Add(Role::InfixOperator);
		Add(Role::UnfieldedOperator);
	}
	if(Lang == Lang::Swift && (Name == "metatype" || Name == "user_type")) Add(Role::SpacelessTypeName);
	if(Lang == Lang::CSharp && Name == "pointer_type") Add(Role::ProductAmbiguousPtrType);
	if(Lang.IsCFamily() && Name == "parenthesized_expression") Add(Role::MacroGuardedParens);
	if(Lang == Lang::CSS && Name == "identifier") Add(Role::PlainValWord);
	if(Lang == Lang::Ruby && Name == "uninterpreted") Add(Role::TrailingDataSection);
	if(Lang == Lang::Swift && Name == "range_expression") Add(Role::SpacingSensitiveRange);
	if(Lang.IsJsTs() && IsNamed && Names::JsLetBindingHead.HasName(Name)) Add(Role::SplitBindingHead);
	if(Names::VerbatimNode.HasName(Name) || Lang == Lang::Swift && Name == "diagnostic") Add(Role::SrcCopiedNode);
	if(Names::Unformattable.HasName(Name)) Add(Role::VerbatimSubtree);
	if(Lang == Lang::Kotlin) {
		// 修飾名だけを逐語に保ち，インポートの宣言と項目は構文へ投影する
		if(Name == "identifier") Add(Role::ImportLeaf);
		if(Names::KotlinStmtCtx.HasName(Name)) Add(Role::LineTerminatedCtx);
		if(Names::KotlinJointSuffix.HasName(Name)) Add(Role::LineJoiningSuffix);
		if(Names::KotlinClassMember.HasName(Name)) Add(Role::ClassMember);
	}
	if(Lang == Lang::HTML && Name == "script_element") Add(Role::EmbeddedScriptElement);
	// トークンの折返位置と区切をノードの役割として記録する
	static constexpr std::array<std::string_view, 5> LeadingBreakTokens { ")", ".", "?.", "]", "}" };
	static constexpr std::array<std::string_view, 6> TrailingBreakTokens { "(", ",", ";", "=>", "[", "{" };
	static constexpr std::array<std::string_view, 42> TrailingOperatorTokens {
		"!=",
		"!==",
		"%",
		"%=",
		"&",
		"&&",
		"&&=",
		"&=",
		"*",
		"**",
		"**=",
		"*=",
		"+",
		"+=",
		"-",
		"-=",
		"/",
		"/=",
		":",
		"<",
		"<<",
		"<<=",
		"<=",
		"<=>",
		"=",
		"==",
		"===",
		">",
		">=",
		">>",
		">>=",
		">>>",
		">>>=",
		"?",
		"?\?",
		"?\?=",
		"^",
		"^=",
		"|",
		"|=",
		"||",
		"||="
	};
	if(std::ranges::find(LeadingBreakTokens, Name) != LeadingBreakTokens.end()) Add(Role::LeadingBreakToken);
	if(std::ranges::find(TrailingBreakTokens, Name) != TrailingBreakTokens.end()) Add(Role::TrailingBreakToken);
	if(std::ranges::find(TrailingOperatorTokens, Name) != TrailingOperatorTokens.end()) Add(Role::TrailingOperatorToken);
	if(Name == "->" || Name == "?->") Add(Role::MemberAccessArrow);
	if(Name == ";") Add(Role::StmtTerminatorToken);
	if(Name == "{") Add(Role::OpeningBraceToken);
	if(Name == ")") Add(Role::ClosingParenToken);
	if(Name == "]") Add(Role::ClosingSquareToken);
	if(Name == ":") Add(Role::ColonToken);
	if(Name == "?") Add(Role::QuestionToken);
	if(Name == "!") Add(Role::ExclamationToken);
	if(Name == "/>") Add(Role::SelfClosingTagToken);
	if(Name == "**") Add(Role::ExponentToken);
	if(Name == "...") Add(Role::SpreadToken);
	if(Name == ".." || Name == "...") Add(Role::OpenRangeToken);
	if(Name == "*") Add(Role::AsteriskToken);
	if(Name == "&") Add(Role::AmpersandToken);
	if(Name == "::") Add(Role::ScopeQualifierToken);
	if(Name == "|") Add(Role::PipeToken);
	if(Name == "??") Add(Role::CoalescingToken);
	if(Name == "?.") Add(Role::OptionalAccessToken);
	if(Name == "&.") Add(Role::SafeAccessToken);
	// 参照の継続が許される側を，共通の折返の役割へ対応付ける
	if(Has(Role::ScopeQualifierToken)) {
		if(Lang == Lang::Kotlin || Lang == Lang::Ruby) Add(Role::TrailingOperatorToken);
		else Add(Role::LeadingBreakToken);
	}
	if(Has(Role::SafeAccessToken)) Add(Role::LeadingBreakToken);
	if(Name == "->") Add(Role::RightArrowToken);
	if(Name == "=>") Add(Role::FatArrowToken);
	if(Name == "?->") Add(Role::OptionalArrowToken);
	if(Name == "/") Add(Role::SlashToken);
	if(Name == "</") Add(Role::ClosingTagOpenToken);
	if(Name == "@") Add(Role::AtToken);
	if(Name == "\\") Add(Role::BackslashToken);
	if(Name == "#") Add(Role::HashToken);
	if(Name == "#[") Add(Role::AttributeOpenToken);
	if(Name == "<!") Add(Role::MarkupDeclOpenToken);
	if(
		Name == "#{" || Name == "${" || Name == "\\(" || Name == "interpolation_expression_start" ||
		Name == "raw_str_interpolation_start"
	) Add(Role::InterpolationOpenToken);
	if(Name == "\\(" || Name == "raw_str_interpolation_start") Add(Role::OpeningParenToken);
	if(Name == "interpolation_expression_end") Add(Role::InterpolationCloseToken);
	if(Name == "[[") Add(Role::DoubleSquareOpenToken);
	if(Name == "]]") Add(Role::DoubleSquareCloseToken);
	if(Name == "~") Add(Role::TildeToken);
	if(!IsNamed && Name == "_") Add(Role::WildcardToken);
	if(Name == "++" || Name == "--") Add(Role::ChangeByOneToken);
	if(Name == "then") Add(Role::CondBodyDelimiterToken);
	if(Name == "do") Add(Role::IterationBodyDelimiterToken);
	// トークンの端の記号の形（配置の段が挿入位置の前後の繋がりを判ずる，整形仕様 7.6）
	if(!IsNamed && !Name.empty()) {
		const unsigned char First = Name.front(), Last = Name.back();
		if(First == '(' || First == '[' || First == '{') Add(Role::OpeningBracketLead);
		switch(First) {
		case '(':
			Add(Role::OpeningParenLead);
			break;
		case '[':
			Add(Role::OpeningSquareLead);
			break;
		case '{':
			Add(Role::OpeningBraceLead);
			break;
		}
		if(First == ')' || First == ']' || First == '}') Add(Role::ClosingBracketLead);
		if(First == ']') Add(Role::ClosingSquareLead);
		if(Names::UnaryPrefixToken.HasName(Name.substr(0, 1))) Add(Role::UnaryPrefixLead);
		switch(First) {
		case '<':
			Add(Role::OpeningAngleLead);
			break;
		case ';':
			Add(Role::StmtTerminatorLead);
			break;
		case ',':
			Add(Role::CommaLead);
			break;
		case ':':
			Add(Role::ColonLead);
			break;
		case '?':
			Add(Role::QuestionLead);
			break;
		case '|':
			Add(Role::PipeLead);
			break;
		case '}':
			Add(Role::ClosingBraceLead);
			break;
		case ')':
			Add(Role::ClosingParenLead);
			break;
		}
		if(std::string_view("+-*/%<>=!&|^~?.").find(First) != std::string_view::npos) Add(Role::ExprContinuationLead);
		if(First == ',' || First == ';') Add(Role::SeparatorLead);
		if(Name.starts_with("()")) Add(Role::EmptyParensLead);
		if(Last == '(' || Last == '[' || Last == '{') Add(Role::OpeningBracketTail);
		switch(Last) {
		case ')':
			Add(Role::ClosingParenTail);
			break;
		case ';':
			Add(Role::StmtTerminatorTail);
			break;
		case ',':
			Add(Role::CommaTail);
			break;
		case '{':
			Add(Role::OpeningBraceTail);
			break;
		}
		if(
			std::string_view("([{,;:=?+-*%&|^~<>.").find(Last) != std::string_view::npos &&
			!(Name.size() > 1 && (Last == '+' || Last == '-') && Name[Name.size() - 2] == Last)
		) Add(Role::ExprContinuationTail);
	}
	// 宣言へ読み替え得る演算子と，実引数を調べる構造の投影
	if(Lang.IsCFamily() && !IsNamed && (Name == "&" || Name == "&&" || Name == "*" || Name == "<" || Name == ">" || Name == ">>")) {
		Add(Role::DeclaratorOperator);
	}
	if(!IsNamed) {
		if(Names::StrDelimiter.HasName(Name)) Add(Role::QuoteToken);
		else if(Name.size() > 1 && Name.back() == '"') {
			if(
				const std::span<const std::string_view> Prefixes = GetStrPrefixes(Lang);
				std::ranges::find(Prefixes, Name.substr(0, Name.size() - 1)) != Prefixes.end()
			) Add(Role::QuoteToken);
		}
	}
	if(Lang == Lang::Swift) {
		static constexpr std::array<std::string_view, 10> BinaryOperations {
			"additive_expression",
			"bitwise_operation",
			"comparison_expression",
			"conjunction_expression",
			"disjunction_expression",
			"equality_expression",
			"infix_expression",
			"multiplicative_expression",
			"nil_coalescing_expression",
			"range_expression"
		};
		static constexpr std::array<std::string_view, 3> PostfixOperands {
			"additive_expression",
			"multiplicative_expression",
			"prefix_expression"
		};
		static constexpr std::array<std::string_view, 3> PostfixExprs {
			"call_expression",
			"navigation_expression",
			"postfix_expression"
		};
		static constexpr std::array<std::string_view, 9> PrecBoundaries {
			"as_expression",
			"assignment",
			"await_expression",
			"check_expression",
			"consume_expression",
			"open_end_range_expression",
			"open_start_range_expression",
			"try_expression",
			"unsafe_expression"
		};
		if(std::ranges::find(BinaryOperations, Name) != BinaryOperations.end()) Add(Role::BinaryPrecExpr);
		if(std::ranges::find(PostfixOperands, Name) != PostfixOperands.end()) Add(Role::PostfixBindingOperand);
		if(std::ranges::find(PostfixExprs, Name) != PostfixExprs.end()) Add(Role::PostfixBindingExpr);
		if(std::ranges::find(PrecBoundaries, Name) != PrecBoundaries.end()) Add(Role::OperatorPrecBoundary);
		if(Name == "ternary_expression") Add(Role::ConditionalOperatorExpr);
	}
	if(Lang == Lang::HTML && Name == "element") Add(Role::ContentElement);
	if(Lang == Lang::Java && (Name == "character_literal" || Name == "string_literal")) Add(Role::EscapeLiteralHost);
	if(Lang == Lang::Java && Name == "block_comment") Add(Role::BlockCommentNode);
	if(Names::StructuralParenBoundary.HasName(Name)) Add(Role::StructuralParenBoundary);
	if(Names::MeaningfulParenGroup.HasName(Name)) Add(Role::MeaningfulParenGroup);
	if(Names::ReceiverParenHost.HasName(Name)) Add(Role::ReceiverParenHost);
	if(Name == "<-") Add(Role::ChannelReceiveToken);
	if(Name == "!!") Add(Role::NonNullAssertionToken);
	if(!IsNamed && !Name.empty()) {
		const unsigned char First = Name.front();
		if(Name.back() == '*') Add(Role::PtrMemberAccess);
		if(std::string_view("<>+-.[(`").find(First) != std::string_view::npos) Add(Role::InstantiationContinuationLead);
		if(Name.find_first_of("<>") != std::string_view::npos) Add(Role::AngleConflictToken);
		if(Name.find('>') != std::string_view::npos) Add(Role::AngleCloseCandidate);
		if(std::string_view("-+*&^").find(First) != std::string_view::npos) Add(Role::CastOperandLead);
		if(
			std::string_view("*&-+").find(First) != std::string_view::npos && !(Name.size() > 1 && Name[1] == '=') &&
			!Name.starts_with("->")
		) Add(Role::CastContinuationToken);
		if(First == '*' || First == '(') Add(Role::CastUpdateFollowerLead);
		if(Name.find(':') != std::string_view::npos) Add(Role::InterpolationFormatSeparator);
		if(std::string_view("\"'@$").find(First) != std::string_view::npos) Add(Role::InterpolationOpaqueLead);
		if(Name == ":]") Add(Role::ClosedValTerminator);
		if(std::string_view("(`+-[").find(First) != std::string_view::npos) Add(Role::TypeArgOpeningLead);
		if(std::string_view("\"#'`([{").find(First) != std::string_view::npos) Add(Role::BodyDelimiterLead);
		if(First == '.') Add(Role::MemberAccessLead);
		if(Name.back() == '.') Add(Role::MemberAccessTail);
	}
	if(
		Lang == Lang::Rust && !IsNamed && !Name.empty() && Name.find_first_not_of("!#$%&'*+,-./:;<=>?@^|~") == std::string_view::npos
	) Add(Role::JointPunctuationToken);
	if(Lang == Lang::Rust && !IsNamed && Name == "!") Add(Role::ExclamationMark);
	if(!IsNamed) {
		if(Name == "{") Add(Role::OpenBrace);
		if(Name.size() == 1 && std::string_view(")]},;").find(Name) != std::string_view::npos) Add(Role::ExprTerminator);
		else if(Name == "else") Add(Role::ElseKeyword);
		else if(Name == "using") Add(Role::UsingKeyword);
		// トークンの役割の返戻
		return Roles;
	}
	// 文と要素と型に固有の役割
	if(Lang.IsJsTs()) {
		static constexpr std::array<std::string_view, 13> Stmts {
			"break_statement",
			"continue_statement",
			"debugger_statement",
			"do_statement",
			"export_statement",
			"expression_statement",
			"function_signature",
			"import_statement",
			"lexical_declaration",
			"return_statement",
			"throw_statement",
			"type_alias_declaration",
			"variable_declaration"
		};
		static constexpr std::array<std::string_view, 5> Members {
			"abstract_method_signature",
			"field_definition",
			"index_signature",
			"method_signature",
			"public_field_definition"
		};
		if(std::ranges::binary_search(Stmts, Name)) Add(Role::TerminatedStmt);
		if(std::ranges::binary_search(Members, Name)) Add(Role::TerminatedMember);
		if(Name == "jsx_text") Add(Role::CollapsibleText);
		if(Name == "jsx_element") Add(Role::MarkupElement);
		if(Name == "jsx_opening_element") Add(Role::MarkupOpen);
		if(Name == "jsx_closing_element") Add(Role::MarkupClose);
		if(Name == "jsx_self_closing_element") {
			Add(Role::MarkupOpen);
			Add(Role::SelfClosingTag);
		}
		if(Name == "jsx_expression") Add(Role::MarkupExpr);
		if(Name == "jsx_attribute") Add(Role::MarkupAttribute);
		if(Name == "non_null_expression") Add(Role::NonNullAssertion);
		if(Name == "required_parameter") Add(Role::RequiredParam);
		if(Name == "type_parameters") Add(Role::TypeParamList);
		if(Name == "type_arguments") Add(Role::TypeArgList);
		if(Name == "class_body") Add(Role::MemberBody);
		if(Name == "binary_expression") Add(Role::TypeofCmp);
		if(Name == "named_imports") Add(Role::NamedImports);
		if(Name == "arrow_function") Add(Role::ArrowFunction);
		if(Name == "new_expression") Add(Role::Construction);
		if(Name == "export_statement") Add(Role::Export);
		if(Name == "internal_module") Add(Role::Namespace);
		if(Name == "for_in_statement") Add(Role::Iteration);
		if(Name == "as_expression" || Name == "type_assertion" || Name == "satisfies_expression") Add(Role::TypeAssertion);
		if(Name == "instantiation_expression") Add(Role::Instantiation);
	}
	if(Lang == Lang::TypeScript) {
		static constexpr std::array<std::string_view, 5> TypeMembers {
			"call_signature",
			"construct_signature",
			"index_signature",
			"method_signature",
			"property_signature"
		};
		if(std::ranges::binary_search(TypeMembers, Name)) Add(Role::TypeMember);
	}
	// 複数文法で共有する構文の集合
	const bool IsKotlin = Lang == Lang::Kotlin;
	static constexpr std::array<std::string_view, 10> CommaDecls {
		"const_declaration",
		"declaration",
		"field_declaration",
		"lexical_declaration",
		"local_declaration_statement",
		"local_variable_declaration",
		"property_declaration",
		"short_var_declaration",
		"var_declaration",
		"variable_declaration"
	};
	static constexpr std::array<std::string_view, 4> BracedBlocks {
		"block",
		"compound_statement",
		"control_structure_body",
		"statement_block"
	};
	static constexpr std::array<std::string_view, 17> BodyOwners {
		"declare_statement",
		"do_statement",
		"do_while_statement",
		"else_clause",
		"else_if_clause",
		"enhanced_for_statement",
		"fixed_statement",
		"for_in_statement",
		"for_range_loop",
		"for_statement",
		"foreach_statement",
		"if_expression",
		"if_statement",
		"labeled_statement",
		"lock_statement",
		"using_statement",
		"while_statement"
	};
	static constexpr std::array<std::string_view, 28> BodyDecls {
		"abstract_class_declaration",
		"alias_declaration",
		"class_declaration",
		"class_specifier",
		"declaration",
		"enum_declaration",
		"enum_specifier",
		"function_declaration",
		"function_definition",
		"function_static_declaration",
		"generator_function_declaration",
		"global_declaration",
		"interface_declaration",
		"lexical_declaration",
		"local_declaration_statement",
		"local_function_statement",
		"local_variable_declaration",
		"namespace_alias_definition",
		"property_declaration",
		"record_declaration",
		"static_assert_declaration",
		"struct_specifier",
		"trait_declaration",
		"type_alias_declaration",
		"type_definition",
		"union_specifier",
		"using_declaration",
		"variable_declaration"
	};
	static constexpr std::array<std::string_view, 11> DefaultParams {
		"assignment_pattern",
		"class_parameter",
		"default_parameter",
		"function_value_parameters",
		"optional_parameter",
		"optional_parameter_declaration",
		"parameter",
		"property_promotion_parameter",
		"required_parameter",
		"simple_parameter",
		"typed_default_parameter"
	};
	static constexpr std::array<std::string_view, 7> TightEquals {
		"attribute",
		"attribute_argument",
		"declare_directive",
		"keyword_argument",
		"keyword_pattern",
		"setter",
		"value_argument"
	};
	static constexpr std::array<std::string_view, 12> Loops {
		"do_statement",
		"do_while_statement",
		"enhanced_for_statement",
		"for_expression",
		"for_in_statement",
		"for_range_loop",
		"for_statement",
		"foreach_statement",
		"loop_expression",
		"repeat_while_statement",
		"while_expression",
		"while_statement"
	};
	static constexpr std::array<std::string_view, 19> Preprocessors {
		"preproc_call",
		"preproc_def",
		"preproc_define",
		"preproc_elif",
		"preproc_elifdef",
		"preproc_else",
		"preproc_endregion",
		"preproc_error",
		"preproc_function_def",
		"preproc_if",
		"preproc_if_in_attribute_list",
		"preproc_ifdef",
		"preproc_include",
		"preproc_line",
		"preproc_nullable",
		"preproc_pragma",
		"preproc_region",
		"preproc_undef",
		"preproc_warning"
	};
	static constexpr std::array<std::string_view, 5> PreprocBranches {
		"preproc_elif",
		"preproc_elifdef",
		"preproc_else",
		"preproc_if",
		"preproc_ifdef"
	};
	static constexpr std::array<std::string_view, 13> PreprocLines {
		"preproc_call",
		"preproc_def",
		"preproc_define",
		"preproc_endregion",
		"preproc_error",
		"preproc_function_def",
		"preproc_include",
		"preproc_line",
		"preproc_nullable",
		"preproc_pragma",
		"preproc_region",
		"preproc_undef",
		"preproc_warning"
	};
	static constexpr std::array<std::string_view, 22> SectionHeaders {
		"access_specifier",
		"case_statement",
		"communication_case",
		"default_case",
		"default_statement",
		"else",
		"elsif",
		"ensure",
		"expression_case",
		"in_clause",
		"labeled_statement",
		"named_label_statement",
		"rescue",
		"statement_label",
		"switch_block_statement_group",
		"switch_case",
		"switch_default",
		"switch_entry",
		"switch_label",
		"switch_section",
		"type_case",
		"when"
	};
	static constexpr std::array<std::string_view, 11> StmtHosts {
		"block",
		"block_body",
		"body_statement",
		"compound_statement",
		"control_structure_body",
		"program",
		"source_file",
		"statement_block",
		"statement_list",
		"statements",
		"translation_unit"
	};
	static constexpr std::array<std::string_view, 5> TrailingCommaHosts {
		"argument_list",
		"literal_value",
		"parameter_list",
		"type_arguments",
		"type_parameter_list"
	};
	if(Lang == Lang::Python) {
		if(Name == "type") Add(Role::TypeSyntax);
		if(Name == "generic_type") Add(Role::TypeApplication);
		if(Name == "type_alias_statement") Add(Role::TypeAlias);
		if(Name == "call" || Name == "subscript") Add(Role::PostfixReceiverHost);
		if(Name == "assignment" || Name == "augmented_assignment") Add(Role::Assignment);
		if(Name == "expression_list" || Name == "pattern_list" || Name == "tuple_expression") Add(Role::BareTuple);
		if(Names::PythonContinuationExpr.HasName(Name)) Add(Role::ContinuationExpr);
	}
	if(Lang == Lang::Python && Name == "block") Add(Role::IndentedBody);
	if(
		Lang == Lang::Python && (Name == "elif_clause" || Name == "except_clause" || Name == "finally_clause" || Name == "else_clause")
	) Add(Role::Clause);
	if(Lang == Lang::PHP) {
		if(Name == "php_tag") Add(Role::CodeOpener);
		if(Name == "text_interpolation") Add(Role::CodeBoundary);
		if(Name == "static_variable_declaration") Add(Role::DeclaratorItem);
		if(Name == "function_static_declaration") {
			Add(Role::CommaDecl);
			Add(Role::Decl);
		}
		if(Names::PhpSemicolonStmt.HasName(Name)) Add(Role::TerminatedStmt);
	}
	if(Lang == Lang::PHP && Name == "colon_block") Add(Role::DelimitedBody);
	if(Name == "ERROR") Add(Role::Error);
	if(Name == "annotation" || Name == "file_annotation" || Name == "use_site_target") Add(Role::AnnotationParts);
	if(Name == "assignment_expression") Add(Role::Assignment);
	if(Name == "await_expression") Add(Role::Await);
	if(Lang == Lang::Cpp && (Name == "placeholder_type_specifier" || Name == "type_identifier")) Add(Role::DeducedType);
	if(std::find(DefaultParams.begin(), DefaultParams.end(), Name) != DefaultParams.end()) {
		Add(Role::DefaultParam);
		Add(Role::EqualsSpaced);
	}
	if(Name == "public_field_definition") Add(Role::EqualsSpaced);
	if(std::find(TightEquals.begin(), TightEquals.end(), Name) != TightEquals.end()) Add(Role::EqualsTight);
	if(std::ranges::binary_search(BracedBlocks, Name)) Add(Role::Block);
	if(std::ranges::binary_search(BodyOwners, Name)) Add(Role::BodyOwner);
	if(Name == "control_structure_body") Add(Role::BodyWrapper);
	// 枝の値を結果にする本体・枝・ラムダ・try 式と値を持たない文は，其れらを式とする文法の物だけ
	if(IsKotlin && (Name == "catch_block" || Name == "control_structure_body")) Add(Role::BranchBody);
	if(IsKotlin && Name == "when_entry") Add(Role::CaseArm);
	if(IsKotlin && Name == "lambda_literal") Add(Role::LastValLambda);
	if(IsKotlin && (Name == "assignment" || Name == "for_statement" || Name == "while_statement")) Add(Role::StmtOnly);
	if(IsKotlin && Name == "try_expression") Add(Role::ValTry);
	if(std::ranges::binary_search(BodyDecls, Name)) Add(Role::Decl);
	if(std::ranges::binary_search(CommaDecls, Name)) {
		Add(Role::CommaDecl);
		Add(Role::Decl);
	}
	if(Name == "do_statement" || Name == "do_while_statement") Add(Role::DoLoop);
	if(Name == "else_clause") Add(Role::ElseClause);
	if(Name == "else_if_clause") Add(Role::ElseIfClause);
	if(Name == "expression_statement") Add(Role::ExprStmt);
	if(Name == "if_expression" || Name == "if_statement") Add(Role::If);
	// 地の文は PHP の文法の物だけ（HTML の本文と区別する）
	if(Lang == Lang::PHP && (Name == "text" || Name == "text_interpolation")) Add(Role::InlineText);
	if(Name == "annotation" || Name == "label" || Name == "statement_label") Add(Role::Label);
	if(Name == "labeled_statement" || Name == "named_label_statement") Add(Role::Labeled);
	if(std::ranges::binary_search(Loops, Name)) Add(Role::Loop);
	if(Lang == Lang::CSS && (Name == "property_name" || Name == "tag_name")) Add(Role::NameParts);
	if(Name == "prefix_expression") Add(Role::PrefixOperation);
	if(std::ranges::binary_search(Preprocessors, Name)) Add(Role::Preproc);
	if(std::ranges::binary_search(PreprocBranches, Name)) Add(Role::PreprocBranch);
	if(std::ranges::binary_search(PreprocLines, Name)) Add(Role::PreprocLine);
	if(Lang == Lang::Rust && (Name == "token_tree_pattern" || Name == "token_tree")) Add(Role::PreservedTokenSeq);
	if(std::ranges::binary_search(SectionHeaders, Name)) {
		Add(Role::SectionHeader);
		Add(Role::ClauseHeading);
	}
	if(std::ranges::binary_search(StmtHosts, Name)) Add(Role::StmtHost);
	if(Name == "statements") Add(Role::StmtSeq);
	if(Lang == Lang::Go && std::ranges::binary_search(TrailingCommaHosts, Name)) Add(Role::TrailingCommaHost);
	if(Name == "if_expression" || Name == "when_expression") Add(Role::ValBranching);
	if(Lang == Lang::HTML) {
		if(Name == "document" || Name == "element" || Name == "script_element" || Name == "style_element") Add(Role::MarkupContainer);
		if(Name == "start_tag") Add(Role::MarkupOpen);
		if(Name == "end_tag") Add(Role::MarkupClose);
		// 属性の間と閉じの前を折返の候補にする開始タグ（行分割仕様 2.1 の JSX / HTML の属性）
		if(Name == "start_tag" || Name == "self_closing_tag") Add(Role::ExprTag);
		if(Name == "text" || Name == "entity" || Name == "raw_text") Add(Role::MarkupText);
	}
	// 末尾の値と語で囲む本体の役割
	if(Lang == Lang::Ruby) {
		if(Name == "while_modifier" || Name == "until_modifier") Add(Role::PostfixLoop);
		if(IsNamed && (Name == "for" || Name == "until" || Name == "while" || Has(Role::PostfixLoop))) {
			Add(Role::Loop);
			Add(Role::BreakTarget);
		}
		if(IsNamed && (Name == "break" || Name == "next")) Add(Role::LoopExitStmt);
		if(!IsNamed && Name == "next") Add(Role::ContinueKeyword);
		Roles.reset(static_cast<size_t>(Role::SectionHeader));
		if(Name == "method" || Name == "singleton_method") Add(Role::TailValFunction);
		if(
			static constexpr std::string_view Vals[] = {
				"array",
				"assignment",
				"binary",
				"call",
				"case",
				"case_match",
				"class_variable",
				"complex",
				"conditional",
				"constant",
				"element_reference",
				"false",
				"float",
				"for",
				"global_variable",
				"hash",
				"heredoc_beginning",
				"identifier",
				"if",
				"if_modifier",
				"instance_variable",
				"integer",
				"lambda",
				"nil",
				"operator_assignment",
				"parenthesized_statements",
				"range",
				"rational",
				"regex",
				"rescue_modifier",
				"scope_resolution",
				"self",
				"simple_symbol",
				"string",
				"string_array",
				"super",
				"symbol_array",
				"true",
				"unary",
				"unless",
				"unless_modifier",
				"until",
				"until_modifier",
				"while",
				"while_modifier",
				"yield"
			};
			std::ranges::find(Vals, Name) != std::end(Vals)
		) Add(Role::ReturnVal);
		static constexpr std::string_view Blocks[] = {
			"begin",
			"block",
			"case",
			"case_match",
			"class",
			"do_block",
			"for",
			"if",
			"lambda",
			"method",
			"module",
			"singleton_class",
			"singleton_method",
			"unless",
			"until",
			"while"
		};
		static constexpr std::string_view Sections[] = { "do", "else", "elsif", "ensure", "in_clause", "rescue", "then", "when" };
		if(std::ranges::find(Blocks, Name) != std::end(Blocks)) Add(Role::KeywordBlock);
		if(std::ranges::find(Sections, Name) != std::end(Sections)) Add(Role::KeywordSection);
		if(Name == "then" || Name == "do") Add(Role::TransparentSection);
		if(
			Name == "begin" || Name == "class" || Name == "module" || Name == "do_block" || Name == "method" ||
			Name == "singleton_method" || Name == "singleton_class"
		) Add(Role::ExpandedBody);
		if(Name == "case_match") Add(Role::PatternBranch);
		if(Name == "if" || Name == "unless" || Name == "elsif" || Name == "when" || Name == "in_clause") Add(Role::CondSection);
		if(Name == "rescue") Add(Role::ExceptionSection);
		if(Name == "ensure") Add(Role::FinalSection);
		if(Name == "body_statement" || Name == "block_body" || Name == "then" || Name == "do" || Name == "interpolation") {
			Add(Role::StmtSeq);
		}
	}
	// 型の本体の分類と，カンマで区切るメンバ又は本体を持たないアクセサの要素列の分類を分ける
	if(Names::TypeMemberBody.HasName(Name)) Add(Role::TypeMemberBody);
	if(Has(Role::TypeMemberBody)) {
		if(Names::GetElementBodies(Lang).HasName(Name)) Add(Role::ObjectLike);
		else Add(Role::IndentContainer);
	}
	// 文・メンバ・最上位を並べる共通の文法分類から宣言のコンテナを確定する
	if(
		Has(Role::DocHostSeq) || Has(Role::IndentContainer) || Has(Role::StmtFlowContainer) || Has(Role::CaseClause) ||
		Has(Role::Transparent)
	) Add(Role::DeclSeq);
	if(Lang.IsCFamily()) {
		if(Has(Role::Identifier) || Has(Role::QualifiedIdentifier) || Has(Role::GenericFunction) || Has(Role::PrimitiveType)) {
			Add(Role::DeclaratorCallee);
		}
		if(Has(Role::ArgSeq) || Has(Role::GroupingParen)) Add(Role::DeclaratorContainer);
		if(Has(Role::AddressOperation) || Has(Role::SubscriptExprNode)) Add(Role::DeclaratorFirstChild);
		if(Has(Role::BinaryOperation)) Add(Role::DeclaratorBinary);
		if(
			Has(Role::ExprStmtLiteral) || Has(Role::FieldExpr) || Has(Role::NumLiteral) || Has(Role::TernaryExpr) ||
			Has(Role::UnaryOperation) && !Has(Role::AddressOperation)
		) Add(Role::DeclaratorInvalid);
	}
	// 役割の返戻
	return Roles;
}

/**
 * 束縛の共通構造の固定定義の取得関数
 * 計算量：固定の対応数 K とノード名の長さ L に対し O(KL)
 * @param Lang 入力の言語
 * @param Name 文法のノード名
 * @param Roles 同じ種別に投影した構文の役割
 * @return 共通の導入・利用・範囲の構造（対象外の言語は無い）
 */
Core::BindingSyntax GrammarProfile::GetBindingSyntax(
	const ::Lang Lang,
	const std::string_view Name,
	const Core::RoleSet &Roles
) {
	// 束縛の共通構造の固定定義の取得
	using Shape = Core::BindingSyntax;
	// 型の仮引数は値の仮引数や型の参照と区別し，名前の並び自体は新しい範囲を作らない
	if(Lang != Lang::Cpp && Roles.test(static_cast<size_t>(Core::Role::TemplateTypeParam))) return Shape::TypeParam;
	// 型仮引数の並び自体は新しい有効範囲を作らない事の返戻
	if(Roles.test(static_cast<size_t>(Core::Role::GenericParams))) return Shape::None;
	// 共通の構造だけでは表せない文法の導入形
	struct LocalBinding {
		::Lang::ID Language;
		std::string_view Name;
		Shape Binding;
	};
	static constexpr LocalBinding Local[] {
		{ Lang::Go, "expression_list", Shape::Pattern },
		{ Lang::Go, "identifier", Shape::Name },
		{ Lang::Go, "parameter_declaration", Shape::Pattern },
		{ Lang::Go, "parameter_list", Shape::Params },
		{ Lang::Go, "variadic_parameter_declaration", Shape::Pattern },
		{ Lang::Swift, "lambda_function_type", Shape::Params },
		{ Lang::Swift, "lambda_function_type_parameters", Shape::Params },
		{ Lang::Swift, "lambda_parameter", Shape::Param },
		{ Lang::Swift, "simple_identifier", Shape::Name },
		{ Lang::Swift, "typealias_declaration", Shape::ClassDecl },
		{ Lang::PHP, "formal_parameters", Shape::Params },
		{ Lang::PHP, "simple_parameter", Shape::Param },
		{ Lang::PHP, "variable_name", Shape::Name },
		{ Lang::PHP, "variadic_parameter", Shape::Param },
		{ Lang::Ruby, "alias", Shape::Declarator },
		{ Lang::Python, "assignment", Shape::Declarator },
		{ Lang::Python, "default_parameter", Shape::Param },
		{ Lang::Python, "dictionary_splat_pattern", Shape::RestPattern },
		{ Lang::Python, "identifier", Shape::Name },
		{ Lang::Python, "lambda_parameters", Shape::Params },
		{ Lang::Python, "list_pattern", Shape::Pattern },
		{ Lang::Python, "list_splat_pattern", Shape::RestPattern },
		{ Lang::Python, "parameters", Shape::Params },
		{ Lang::Python, "pattern_list", Shape::Pattern },
		{ Lang::Python, "tuple_pattern", Shape::Pattern },
		{ Lang::Python, "typed_default_parameter", Shape::Param },
		{ Lang::Python, "typed_parameter", Shape::Param }
	};
	// 導入形が登録された場合の返戻
	for(const LocalBinding &Def : Local) if(Lang == Def.Language && Name == Def.Name) return Def.Binding;
	// 既存の構文分類から，同じ種類の導入・利用・範囲を共通構造へ登録する
	if(!Lang.IsJsTs() && Lang != Lang::Kotlin && Lang != Lang::Java && Lang != Lang::Rust) {
		const auto Has = [&](const Core::Role Role) -> bool {
			// 投影済の役割の有無の返戻
			return Roles.test(static_cast<size_t>(Role));
		};
		// インポートは周囲へ名前を導入し，型のメンバの順位に依る独立した範囲を作らない
		if(Has(Core::Role::ImportedPathBinding) || Has(Core::Role::ImportedMemberBinding) || Has(Core::Role::ImportedModuleBinding)) {
			// 周囲の名前収集へ供給元を渡す分類の返戻
			return Shape::Ignore;
		}
		// 名前・関数・仮引数の役割を，其れ等にも付く一般の宣言・本体の役割より先に採る
		static constexpr std::pair<Core::Role, Shape> Common[] {
			{ Core::Role::IdentifierLeafLike, Shape::Name },
			{ Core::Role::FieldName, Shape::Name },
			{ Core::Role::NamespaceName, Shape::Name },
			{ Core::Role::LambdaExpr, Shape::Closure },
			{ Core::Role::LambdaLiteral, Shape::Closure },
			{ Core::Role::FunctionScope, Shape::Function },
			{ Core::Role::NamespaceDef, Shape::ModuleDecl },
			{ Core::Role::MemberOwner, Shape::ClassDecl },
			{ Core::Role::ComprehensionScope, Shape::Closure },
			{ Core::Role::ParamContainer, Shape::Params },
			{ Core::Role::ParamContainerLike, Shape::Params },
			{ Core::Role::Param, Shape::Param },
			{ Core::Role::ParamDecl, Shape::Param },
			{ Core::Role::VariadicParamDecl, Shape::Param },
			{ Core::Role::FieldExpr, Shape::NamedMember },
			{ Core::Role::TypeNameNode, Shape::TypeName },
			{ Core::Role::TypeAnnotation, Shape::Ignore },
			{ Core::Role::TypeArgList, Shape::Ignore },
			{ Core::Role::TypeParamList, Shape::Ignore },
			{ Core::Role::DeclInitDeclarator, Shape::Declarator },
			{ Core::Role::CaptureInitializer, Shape::Declarator },
			{ Core::Role::ShortVarDecl, Shape::Declarator },
			{ Core::Role::AssignmentOperation, Shape::Write },
			{ Core::Role::ForRangeLoop, Shape::Loop },
			{ Core::Role::DeclWithType, Shape::Decl },
			{ Core::Role::VarDeclStmt, Shape::Decl },
			{ Core::Role::StructuredBindingDeclarator, Shape::Pattern },
			{ Core::Role::Pattern, Shape::Pattern },
			{ Core::Role::DeclaratorWrapper, Shape::NamePattern },
			{ Core::Role::ParenthesizedDeclarator, Shape::NamePattern },
			{ Core::Role::ArrayDeclarator, Shape::NamePattern },
			{ Core::Role::MemberOrderBody, Shape::ClassBody }
		};
		for(const std::pair<Core::Role, Shape> &Def : Common) if(Has(Def.first)) {
			// 既存の構文分類に対応する束縛の役割の返戻
			return Def.second == Shape::Function && Has(Core::Role::NamedFunctionDef) ? Shape::FunctionDecl : Def.second;
		}
		// 関数全体のローカル名は制御本体の中へ隔離しない
		if(Lang == Lang::PHP || Lang == Lang::Ruby || Lang == Lang::Python) return Shape::None;
		// 反復の頭部と本体の共有の有効範囲の返戻
		if(Has(Core::Role::Loop)) return Shape::Loop;
		// 非宣言の本体と条件部の有効範囲の返戻
		if(Has(Core::Role::CompoundBody) || Has(Core::Role::ScopeBody) || Has(Core::Role::Conditional) || Has(Core::Role::SwitchStmt)) {
			// 本体と条件部が作る有効範囲の返戻
			return Shape::Block;
		}
		// 名前の作用を持たない構造の返戻
		return Shape::None;
	}
	if(
		static constexpr std::pair<std::string_view, Shape> Java[] {
			{ "identifier", Shape::Name },
			{ "method_declaration", Shape::MemberFunction },
			{ "constructor_declaration", Shape::MemberFunction },
			{ "compact_constructor_declaration", Shape::MemberFunction },
			{ "lambda_expression", Shape::Closure },
			{ "class_declaration", Shape::ClassDecl },
			{ "interface_declaration", Shape::ClassDecl },
			{ "enum_declaration", Shape::ClassDecl },
			{ "record_declaration", Shape::ClassDecl },
			{ "class_body", Shape::ClassBody },
			{ "interface_body", Shape::ClassBody },
			{ "enum_body", Shape::ClassBody },
			{ "block", Shape::Block },
			{ "constructor_body", Shape::Block },
			{ "formal_parameters", Shape::Params },
			{ "inferred_parameters", Shape::Params },
			{ "formal_parameter", Shape::Param },
			{ "spread_parameter", Shape::Param },
			{ "catch_formal_parameter", Shape::Param },
			{ "local_variable_declaration", Shape::Decl },
			{ "field_declaration", Shape::Decl },
			{ "variable_declarator", Shape::Declarator },
			{ "catch_clause", Shape::Catch },
			{ "for_statement", Shape::Loop },
			{ "enhanced_for_statement", Shape::Loop },
			{ "assignment_expression", Shape::Write },
			{ "update_expression", Shape::Update },
			{ "field_access", Shape::NamedMember },
			{ "array_access", Shape::Member },
			{ "method_invocation", Shape::NamedMember },
			{ "method_reference", Shape::NamedMember },
			{ "parenthesized_expression", Shape::RestPattern },
			{ "type_identifier", Shape::Ignore },
			{ "scoped_type_identifier", Shape::Ignore },
			{ "generic_type", Shape::Ignore },
			{ "array_type", Shape::Ignore },
			{ "type_parameters", Shape::Ignore },
			{ "type_arguments", Shape::Ignore },
			{ "modifiers", Shape::Ignore },
			{ "underscore_pattern", Shape::Ignore },
			{ "import_declaration", Shape::Ignore },
			{ "package_declaration", Shape::Ignore },
			{ "annotation", Shape::Ignore },
			{ "marker_annotation", Shape::Ignore }
		}, Rust[] {
			{ "identifier", Shape::Name },
			{ "field_identifier", Shape::Name },
			{ "self", Shape::Name },
			{ "function_item", Shape::FunctionDecl },
			{ "struct_item", Shape::ClassDecl },
			{ "union_item", Shape::ClassDecl },
			{ "enum_item", Shape::ClassDecl },
			{ "mod_item", Shape::ModuleDecl },
			{ "field_declaration", Shape::MemberPart },
			{ "block", Shape::Block },
			{ "parameters", Shape::Params },
			{ "closure_parameters", Shape::Params },
			{ "parameter", Shape::Param },
			{ "self_parameter", Shape::Param },
			{ "closure_expression", Shape::Closure },
			{ "let_declaration", Shape::Declarator },
			{ "tuple_pattern", Shape::Pattern },
			{ "slice_pattern", Shape::Pattern },
			{ "tuple_struct_pattern", Shape::Pattern },
			{ "struct_pattern", Shape::Pattern },
			{ "captured_pattern", Shape::Pattern },
			{ "field_pattern", Shape::NamePattern },
			{ "reference_pattern", Shape::Pattern },
			{ "ref_pattern", Shape::RestPattern },
			{ "mut_pattern", Shape::Pattern },
			{ "for_expression", Shape::Loop },
			{ "match_arm", Shape::Catch },
			{ "match_pattern", Shape::RestPattern },
			{ "or_pattern", Shape::Pattern },
			{ "assignment_expression", Shape::Write },
			{ "compound_assignment_expr", Shape::Write },
			{ "field_expression", Shape::NamedMember },
			{ "index_expression", Shape::Member },
			{ "parenthesized_expression", Shape::RestPattern },
			{ "reference_expression", Shape::Pattern },
			{ "type_identifier", Shape::TypeName },
			{ "scoped_type_identifier", Shape::Ignore },
			{ "generic_type", Shape::Ignore },
			{ "reference_type", Shape::Ignore },
			{ "array_type", Shape::Ignore },
			{ "primitive_type", Shape::Ignore },
			{ "type_parameters", Shape::Ignore },
			{ "type_arguments", Shape::Ignore },
			{ "mutable_specifier", Shape::Ignore },
			{ "lifetime", Shape::Ignore },
			{ "attribute_item", Shape::Ignore },
			{ "inner_attribute_item", Shape::Ignore },
			{ "macro_invocation", Shape::Ignore },
			{ "macro_definition", Shape::Ignore },
			{ "token_tree", Shape::Ignore },
			{ "use_declaration", Shape::Ignore },
			{ "scoped_identifier", Shape::Ignore },
			{ "_", Shape::Ignore }
		};
		Lang == Lang::Java || Lang == Lang::Rust
	) {
		const std::span<const std::pair<std::string_view, Shape>> Defs = Lang == Lang::Java ?
		std::span<const std::pair<std::string_view, Shape>>(Java) :
		std::span<const std::pair<std::string_view, Shape>>(Rust);
		// 固定の導入・利用の構造が一致した場合の返戻
		for(const std::pair<std::string_view, Shape> &Def : Defs) if(Name == Def.first) return Def.second;
		// 束縛を導入しない普通の構造の返戻
		return Shape::None;
	}
	static constexpr std::pair<std::string_view, Shape> Common[] {
		{ "identifier", Shape::Name },
		{ "simple_identifier", Shape::Name },
		{ "function_declaration", Shape::FunctionDecl },
		{ "anonymous_function", Shape::Function },
		{ "class_declaration", Shape::ClassDecl },
		{ "class_body", Shape::ClassBody },
		{ "parameter", Shape::Param },
		{ "type_annotation", Shape::Ignore },
		{ "type_identifier", Shape::TypeName },
		{ "type_parameters", Shape::Ignore },
		{ "type_arguments", Shape::Ignore },
		{ "function_type", Shape::Ignore }
	};
	// 共通の束縛構造が一致した場合の返戻
	for(const std::pair<std::string_view, Shape> &Def : Common) if(Name == Def.first) return Def.second;
	static constexpr std::pair<std::string_view, Shape> Lexical[] {
		{ "statement_block", Shape::Block },
		{ "switch_body", Shape::Block },
		{ "class_static_block", Shape::VarBlock },
		{ "arrow_function", Shape::Closure },
		{ "function_expression", Shape::NamedExpr },
		{ "generator_function", Shape::NamedExpr },
		{ "generator_function_declaration", Shape::FunctionDecl },
		{ "method_definition", Shape::MemberFunction },
		{ "class", Shape::ClassExpr },
		{ "abstract_class_declaration", Shape::ClassDecl },
		{ "enum_declaration", Shape::ClassDecl },
		{ "internal_module", Shape::ClassDecl },
		{ "formal_parameters", Shape::Params },
		{ "required_parameter", Shape::Param },
		{ "optional_parameter", Shape::Param },
		{ "lexical_declaration", Shape::Decl },
		{ "variable_declaration", Shape::Decl },
		{ "variable_declarator", Shape::Declarator },
		{ "array_pattern", Shape::Pattern },
		{ "object_pattern", Shape::Pattern },
		{ "pair_pattern", Shape::PairPattern },
		{ "assignment_pattern", Shape::DefaultPattern },
		{ "object_assignment_pattern", Shape::DefaultPattern },
		{ "rest_pattern", Shape::RestPattern },
		{ "shorthand_property_identifier_pattern", Shape::Name },
		{ "shorthand_property_identifier", Shape::Name },
		{ "catch_clause", Shape::Catch },
		{ "for_statement", Shape::Loop },
		{ "for_in_statement", Shape::Loop },
		{ "assignment_expression", Shape::Write },
		{ "augmented_assignment_expression", Shape::Write },
		{ "update_expression", Shape::Update },
		{ "member_expression", Shape::Member },
		{ "subscript_expression", Shape::Member },
		{ "parenthesized_expression", Shape::RestPattern },
		{ "import_statement", Shape::Import },
		{ "import_clause", Shape::Pattern },
		{ "named_imports", Shape::Pattern },
		{ "namespace_import", Shape::RestPattern },
		{ "import_specifier", Shape::ImportSpecifier },
		{ "predefined_type", Shape::Ignore },
		{ "with_statement", Shape::DynamicScope }
	};
	static constexpr std::pair<std::string_view, Shape> Ordered[] {
		{ "statements", Shape::Block },
		{ "function_body", Shape::Block },
		{ "control_structure_body", Shape::Block },
		{ "lambda_literal", Shape::Closure },
		{ "function_value_parameters", Shape::Params },
		{ "lambda_parameters", Shape::Params },
		{ "property_declaration", Shape::Decl },
		{ "primary_constructor", Shape::Params },
		{ "class_parameter", Shape::ConstructorParam },
		{ "anonymous_initializer", Shape::Initializer },
		{ "property_delegate", Shape::Initializer },
		{ "delegation_specifier", Shape::Initializer },
		{ "secondary_constructor", Shape::Function },
		{ "getter", Shape::Function },
		{ "setter", Shape::Function },
		{ "parameter_with_optional_type", Shape::Param },
		{ "parameters_with_optional_type", Shape::Params },
		{ "enum_class_body", Shape::ClassBody },
		{ "destructuring_declaration", Shape::Decl },
		{ "variable_declaration", Shape::NamePattern },
		{ "multi_variable_declaration", Shape::Pattern },
		{ "catch_block", Shape::Catch },
		{ "for_statement", Shape::Loop },
		{ "assignment", Shape::Write },
		{ "prefix_expression", Shape::Update },
		{ "postfix_expression", Shape::Update },
		{ "directly_assignable_expression", Shape::RestPattern },
		{ "parenthesized_expression", Shape::RestPattern },
		{ "navigation_expression", Shape::Member },
		{ "indexing_expression", Shape::Member },
		{ "navigation_suffix", Shape::MemberPart },
		{ "interpolated_identifier", Shape::Name },
		{ "object_literal", Shape::ClassExpr },
		{ "object_declaration", Shape::ClassDecl },
		{ "user_type", Shape::Ignore },
		{ "nullable_type", Shape::Ignore },
		{ "not_nullable_type", Shape::Ignore },
		{ "receiver_type", Shape::Ignore },
		{ "type_constraints", Shape::Ignore },
		{ "parenthesized_type", Shape::Ignore }
	};
	const std::span<const std::pair<std::string_view, Shape>> Defs = Lang.IsJsTs() ?
	std::span<const std::pair<std::string_view, Shape>>(Lexical) :
	std::span<const std::pair<std::string_view, Shape>>(Ordered);
	// 対象言語の束縛構造が一致した場合の返戻
	for(const std::pair<std::string_view, Shape> &Def : Defs) if(Name == Def.first) return Def.second;
	// 束縛を導入しない普通の構造の返戻
	return Shape::None;
}

/**
 * 投影で共有する名前の導入・利用の固定契約の取得関数
 * 計算量：固定の比較数 K とノード名の長さ L に対し O(KL)
 * @param Lang 入力の言語
 * @param Name 文法のノード名
 * @return 名前空間・暗黙仮引数・パターン頭部・コンストラクタと定数参照の固定契約
 */
Core::BindingDef GrammarProfile::GetBindingDef(const ::Lang Lang, const std::string_view Name) {
	// 束縛の導入と参照の定義の構築準備
	Core::BindingDef Def;
	if(Lang == Lang::Ruby && Name == "alias") Def.ValueInput = Core::ChildRole::Alias;
	if(Lang == Lang::Java) {
		if(
			Name == "class_declaration" || Name == "interface_declaration" || Name == "enum_declaration" || Name == "record_declaration"
		) Def.Namespace = Core::BindingNamespace::Type;
		if(Name == "compact_constructor_declaration") Def.Params = Core::BindingParamSrc::EnclosingType;
		if(Name == "type_parameter") Def.Namespace = Core::BindingNamespace::Type;
		if(Name == "field_declaration") Def.ShouldRequireImmutableConstantSrc = true;
	} else if(Lang == Lang::PHP) {
		if(Name == "function_definition") Def.IsStaticItem = true;
	} else if(Lang == Lang::Rust) {
		if(Name == "struct_item" || Name == "union_item" || Name == "enum_item" || Name == "mod_item") {
			Def.Namespace = Core::BindingNamespace::Type;
			Def.IsStaticItem = true;
		}
		if(Name == "function_item" || Name == "use_declaration") Def.IsStaticItem = true;
		if(Name == "type_parameter") Def.Namespace = Core::BindingNamespace::Type;
		if(Name == "struct_item") Def.Constructor = Core::BindingConstructorForm::Nominal;
		else if(Name == "ordered_field_declaration_list") Def.Constructor = Core::BindingConstructorForm::PositionalFields;
		if(Name == "tuple_struct_pattern") Def.PatternRef = Core::ChildRole::Type;
	}
	// 投影先へ渡す変更不能な契約の返戻
	return Def;
}

/**
 * 言語毎に受理するドキュメント記法と本文形式の取得関数
 * 計算量：登録する記法数 F に対し O(F)
 * @param Lang 対象言語
 * @return 優先順を保ったドキュメント記法と本文形式（ドキュメント記法が無ければ空）
 * @throws std::bad_alloc ドキュメント記法の表の領域を確保出来ない場合
 */
GrammarProfile::DocumentForms GrammarProfile::GetDocFormsOf(const ::Lang Lang) {
	// 言語毎に受理するドキュメント記法と本文形式の取得
	using Form = Core::DocumentFormId;
	switch(Lang.Id) {
	case Lang::C:
	case Lang::Cpp:
		return {
			{ CommentNotation::TripleSlashLine, Form::Doxygen },
			{ CommentNotation::SlashBangLine, Form::Doxygen },
			{ CommentNotation::DoubleStarBlock, Form::Doxygen },
			{ CommentNotation::StarBangBlock, Form::Doxygen }
		};
	case Lang::CSharp:
		return { { CommentNotation::TripleSlashLine, Form::XmlDoc }, { CommentNotation::DoubleStarBlock, Form::XmlDoc } };
	case Lang::Java:
		return { { CommentNotation::TripleSlashLine, Form::JavadocMarkdown }, { CommentNotation::DoubleStarBlock, Form::Javadoc } };
	case Lang::Go:
		return { { CommentNotation::SlashLine, Form::Godoc }, { CommentNotation::Block, Form::Godoc } };
	case Lang::Rust:
		return {
			{ CommentNotation::TripleSlashLine, Form::Rustdoc },
			{ CommentNotation::SlashBangLine, Form::Rustdoc, true },
			{ CommentNotation::DoubleStarBlock, Form::Rustdoc },
			{ CommentNotation::StarBangBlock, Form::Rustdoc, true }
		};
	case Lang::Kotlin:
		return { { CommentNotation::DoubleStarBlock, Form::KDoc } };
	case Lang::Swift:
		return { { CommentNotation::TripleSlashLine, Form::SwiftMarkup }, { CommentNotation::DoubleStarBlock, Form::SwiftMarkup } };
	case Lang::PHP:
		return { { CommentNotation::DoubleStarBlock, Form::PHPDoc } };
	case Lang::JavaScript:
	case Lang::TypeScript:
		return { { CommentNotation::DoubleStarBlock, Form::JSDoc } };
	case Lang::Ruby:
		return { { CommentNotation::HashLine, Form::Yard }, { CommentNotation::HashColonLine, Form::Yard } };
	default:
		return {};
	}
}

/**
 * 生成形式の付与関数（整形仕様 3.2，5.5）
 * 固定トークン・返戻・無条件反復の共通形式と，単一文の本体を包むブロック等のノード毎の形式を定める
 * 計算量：文法の記号数 N，記号名の長さの和 L，生成形式の登録・部品の語彙検索の費用 Q に対し O(N + L + Q)
 * @throws FormatLimitExceeded 文法が形式の部品の種別を持たない場合（文法の定義の不具合）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void GrammarProfile::AttachForms() {
	// 共通の固定トークンの生成形式
	Core::CanonicalForm Lexemes;
	Lexemes.Operation = Core::FormOperation::GeneratedLexemes;
	Lexemes.Parts = {
		{ Core::FormPartRole::BlockOpen, nullptr, "{" },
		{ Core::FormPartRole::Separator, nullptr, "," },
		{ Core::FormPartRole::Terminator, nullptr, ";" },
		{ Core::FormPartRole::ValDelimiter, nullptr, ":" },
		{ Core::FormPartRole::BlockClose, nullptr, "}" },
		{ Core::FormPartRole::CallOpen, nullptr, "(" },
		{ Core::FormPartRole::CallClose, nullptr, ")" }
	};
	Rules.GeneratedForms.push_back(std::move(Lexemes));
	// 名前の並び全体が名前の場を持つ定数仕様の区切
	Core::CanonicalForm ConstantNames {
		Core::FormOperation::GeneratedLexemes,
		{ { Core::FormPartRole::Separator, Find(",", false), ",", Core::FormPartRole::Result, Core::ChildRole::Name } }
	};
	ConstantNames.ApplicableRoles.set(static_cast<size_t>(Core::Role::ConstantSpec));
	for(size_t Symbol = 0; Symbol < Kinds.size(); ++Symbol) {
		if(Kinds[Symbol].Roles.test(static_cast<size_t>(Core::Role::ConstantSpec))) FormDefs[Symbol].push_back(ConstantNames);
	}
	// 返戻と反復の形式に使うノード種別の取得
	const Core::KindDef *const ReturnDef = Find(
		Lang == Lang::Kotlin ?
		"jump_expression" :
		Lang == Lang::Swift ? "control_transfer_statement" : Lang == Lang::Rust ? "return_expression" : "return_statement",
		true
	);
	const Core::KindDef *const ReturnStmtDef = Find("return_statement", true);
	const Core::KindDef *const WhileStmtDef = Find("while_statement", true);
	const std::vector<std::string_view> &ReturnWords = Rules.Words[static_cast<size_t>(Core::Word::Return)];
	// 引数を持たない返戻の形式
	if(ReturnDef && !ReturnWords.empty()) {
		Core::CanonicalForm Return;
		Return.Operation = Core::FormOperation::ArglessReturn;
		const bool HasWrapper = Lang == Lang::Rust && Rules.ExprStmtDef.Def != nullptr;
		Return.Parts.push_back(
			{
				Core::FormPartRole::Result,
				HasWrapper ? Rules.ExprStmtDef.Def : ReturnDef,
				{},
				Core::FormPartRole::Result,
				Core::ChildRole::None,
				true
			}
		);
		if(HasWrapper) {
			Return.Parts.push_back({ Core::FormPartRole::Args, ReturnDef, {}, Core::FormPartRole::Result, Core::ChildRole::None, true });
		}
		Return.Parts.push_back(
			{
				Core::FormPartRole::Keyword,
				nullptr,
				ReturnWords.front(),
				HasWrapper ? Core::FormPartRole::Args : Core::FormPartRole::Result
			}
		);
		if(HasWrapper || Lang.IsCFamily() || Lang.IsJsTs() || Lang == Lang::CSharp || Lang == Lang::Java || Lang == Lang::PHP) {
			Return.Parts.push_back({ Core::FormPartRole::Terminator, nullptr, ";" });
		}
		Rules.GeneratedForms.push_back(std::move(Return));
	}
	// 零値を返す形式
	if(ReturnStmtDef && !ReturnWords.empty()) {
		Core::CanonicalForm Return;
		Return.Operation = Core::FormOperation::ReturnWithZero;
		Return.Parts = {
			{ Core::FormPartRole::Result, ReturnStmtDef, {}, Core::FormPartRole::Result, Core::ChildRole::None, true },
			{ Core::FormPartRole::Keyword, nullptr, ReturnWords.front() },
			{ Core::FormPartRole::IntVal, Find("number_literal", true), "0", Core::FormPartRole::Result, Core::ChildRole::None, true },
			{ Core::FormPartRole::Terminator, nullptr, ";" }
		};
		Rules.GeneratedForms.push_back(std::move(Return));
	}
	// 無条件反復の形式
	if(WhileStmtDef && !Rules.Words[static_cast<size_t>(Core::Word::While)].empty()) {
		Core::CanonicalForm Loop;
		Loop.Operation = Core::FormOperation::UnconditionalLoop;
		Loop.Parts.push_back({ Core::FormPartRole::Result, WhileStmtDef, {}, Core::FormPartRole::Result, Core::ChildRole::None, true });
		Loop.Parts.push_back({ Core::FormPartRole::Keyword, nullptr, Rules.Words[static_cast<size_t>(Core::Word::While)].front() });
		if(Lang == Lang::CSharp) {
			Loop.Parts.push_back({ Core::FormPartRole::Open, nullptr, "(" });
			Loop.Parts.push_back(
				{ Core::FormPartRole::Cond, Find("boolean_literal", true), {}, Core::FormPartRole::Result, Core::ChildRole::Cond, true }
			);
			Loop.Parts.push_back(
				{ Core::FormPartRole::TrueVal, nullptr, Rules.Words[static_cast<size_t>(Core::Word::True)].front(), Core::FormPartRole::Cond }
			);
			Loop.Parts.push_back({ Core::FormPartRole::Close, nullptr, ")" });
		} else {
			const Core::KindDef *const Cond = Lang == Lang::Cpp ? Find("condition_clause", true) : Find("parenthesized_expression", true);
			Loop.Parts.push_back({ Core::FormPartRole::Cond, Cond, {}, Core::FormPartRole::Result, Core::ChildRole::Cond, true });
			Loop.Parts.push_back({ Core::FormPartRole::Open, nullptr, "(", Core::FormPartRole::Cond });
			Loop.Parts.push_back(
				{
					Core::FormPartRole::TrueVal,
					Rules.TrueDef.Def,
					Rules.Words[static_cast<size_t>(Core::Word::True)].front(),
					Core::FormPartRole::Cond,
					Lang == Lang::Cpp ? Core::ChildRole::Val : Core::ChildRole::None,
					true
				}
			);
			Loop.Parts.push_back(
				{
					Core::FormPartRole::IntVal,
					Find("number_literal", true),
					"1",
					Core::FormPartRole::Cond,
					Lang == Lang::Cpp ? Core::ChildRole::Val : Core::ChildRole::None,
					true
				}
			);
			Loop.Parts.push_back({ Core::FormPartRole::Close, nullptr, ")", Core::FormPartRole::Cond });
		}
		Loop.Parts.push_back({ Core::FormPartRole::Body, nullptr, {}, Core::FormPartRole::Result, Core::ChildRole::Body });
		Rules.GeneratedForms.push_back(std::move(Loop));
	}
	using Core::Role;
	// 後置条件と末尾返戻の形式
	switch(Lang.Id) {
	case Lang::Ruby:
		{
			for(const std::string_view Name : { "if", "unless", "until", "while" }) {
				Core::CanonicalForm Modifier;
				Modifier.Operation = Core::FormOperation::PostfixCond;
				Modifier.Parts = { { Core::FormPartRole::Result, Find(std::string(Name) + "_modifier", true) } };
				for(TSSymbol Symbol = 0; Symbol < Kinds.size(); ++Symbol) if(SymbolNames[Symbol] == Name && Kinds[Symbol].IsNamed) {
					FormDefs[Symbol].push_back(Modifier);
				}
			}
			Core::CanonicalForm ReturnForm;
			ReturnForm.Operation = Core::FormOperation::ExplicitTailReturn;
			ReturnForm.Parts = {
				{ Core::FormPartRole::Result, Find("return", true) },
				{ Core::FormPartRole::Keyword, Find("return", false), "return" },
				{ Core::FormPartRole::Args, Find("argument_list", true) },
				{ Core::FormPartRole::Open, Find("(", false), "(" },
				{ Core::FormPartRole::Close, Find(")", false), ")" }
			};
			for(TSSymbol Symbol = 0; Symbol < Kinds.size(); ++Symbol) {
				if(Kinds[Symbol].Roles.test(static_cast<size_t>(Role::TailValFunction))) FormDefs[Symbol].push_back(ReturnForm);
			}
		}
		break;
	// 同じ合併型の二項連鎖（先頭の区切記号だけを持つ形は右項と同値）
	case Lang::TypeScript:
		if(const Core::KindDef *const Result = Find("union_type", true)) {
			Core::CanonicalForm Association {
				Core::FormOperation::LeftAssociateBinary,
				{
					{ Core::FormPartRole::Result, Result },
					{ Core::FormPartRole::Left, nullptr },
					{ Core::FormPartRole::Separator, Find("|", false), "|" },
					{ Core::FormPartRole::Right, nullptr }
				}
			};
			Association.Cond = Core::FormCond::BinaryChainWithOptionalLeft;
			Association.ApplicableRoles.set(static_cast<size_t>(Role::TypeCombination));
			FormDefs[static_cast<size_t>(Result - Kinds.data())].push_back(std::move(Association));
		}
		break;
	// ラッパーを外した要素列の生成先（単独の値，代入対象，式の中の値）
	case Lang::Python:
		{
			Core::CanonicalForm Sequence {
				Core::FormOperation::UnwrapSeq,
				{
					{ Core::FormPartRole::Result, Find("tuple_expression", true) },
					{ Core::FormPartRole::Target, Find("pattern_list", true) },
					{ Core::FormPartRole::Val, Find("expression_list", true) }
				}
			};
			Sequence.ApplicableRoles.set(static_cast<size_t>(Role::TupleVal));
			Sequence.ApplicableRoles.set(static_cast<size_t>(Role::TuplePattern));
			for(const std::string_view Name : { "tuple", "tuple_pattern" }) if(const Core::KindDef *const Src = Find(Name, true)) {
				FormDefs[static_cast<size_t>(Src - Kinds.data())].push_back(Sequence);
			}
		}
		// 継続式を包む形式
		for(size_t Symbol = 0; Symbol < Kinds.size(); ++Symbol) {
			const Core::KindDef &Kind = Kinds[Symbol];
			if(
				!Kind.Roles.test(static_cast<size_t>(Role::ContinuationExpr)) && SymbolNames[Symbol] != "tuple" &&
				SymbolNames[Symbol] != "tuple_pattern"
			) continue;
			const std::string_view RawName = SymbolNames[Symbol];
			const std::string_view Name = RawName == "expression_list" || RawName == "tuple" || RawName == "tuple_expression" ?
			"tuple" :
			RawName == "pattern_list" || RawName == "tuple_pattern" ? "tuple_pattern" : "parenthesized_expression";
			Core::CanonicalForm Form { Core::FormOperation::Continuation, { { Core::FormPartRole::Result, Find(Name, true) } } };
			Form.ApplicableRoles.set(static_cast<size_t>(Role::ContinuationExpr));
			if(Kind.Roles.test(static_cast<size_t>(Role::TypeSyntax))) {
				Form.Parts.push_back({ Core::FormPartRole::Body, Find("subscript", true) });
			}
			FormDefs[Symbol].push_back(std::move(Form));
		}
		break;
	// 空のアノテーション引数を省く形式
	case Lang::Java:
		{
			const Core::KindDef *const Annotation = Find("annotation", true), *const Marker = Find("marker_annotation", true);
			if(!Annotation || !Marker) {
				throw FormatLimitExceeded("the grammar has no kind for a marker annotation", FailureReason::FormatterDefect);
			}
			Core::CanonicalForm MarkerForm { Core::FormOperation::OmitEmptyArgs, { { Core::FormPartRole::Result, Marker } } };
			MarkerForm.ApplicableRoles.set(static_cast<size_t>(Role::AnnotationParts));
			MarkerForm.Cond = Core::FormCond::EmptyArgs;
			FormDefs[static_cast<TSSymbol>(Annotation - Kinds.data())].push_back(std::move(MarkerForm));
		}
		break;
	// 末尾カンマの形式
	case Lang::Go:
		{
			Core::CanonicalForm CommaForm {
				Core::FormOperation::TrailingComma,
				{ { Core::FormPartRole::LastElement, nullptr }, { Core::FormPartRole::Close, nullptr } },
				","
			};
			CommaForm.ApplicableRoles.set(static_cast<size_t>(Role::TrailingCommaHost));
			CommaForm.Cond = Core::FormCond::BrokenListWithLastElement;
			CommaForm.ClosingLexemes = ")]}";
			for(size_t Symbol = 0; Symbol < Kinds.size(); ++Symbol) {
				if(Kinds[Symbol].Roles.test(static_cast<size_t>(Role::TrailingCommaHost))) FormDefs[Symbol].push_back(CommaForm);
			}
		}
		break;
	}
	// 本体を包むブロックと其の中の文の並びのノード名
	std::string_view BlockName, SeqName;
	if(Lang == Lang::CSharp || Lang == Lang::Java) BlockName = "block";
	else if(Lang.IsJsTs()) BlockName = "statement_block";
	else if(Lang.IsCFamily() || Lang == Lang::PHP) BlockName = "compound_statement";
	else if(Lang == Lang::Kotlin) {
		BlockName = "control_structure_body";
		SeqName = "statements";
	}
	// 波括弧を省略出来ない文法の終了
	if(BlockName.empty()) return;
	Core::CanonicalForm Form {
		Core::FormOperation::BraceBody,
		{
			{ Core::FormPartRole::Block, Find(BlockName, true) },
			{ Core::FormPartRole::Open, Find("{", false), "{" },
			{ Core::FormPartRole::Body, nullptr },
			{ Core::FormPartRole::Close, Find("}", false), "}" }
		}
	};
	if(!SeqName.empty()) Form.Parts.push_back({ Core::FormPartRole::Stmts, Find(SeqName, true) });
	for(const Role OwnerRole : { Role::BodyOwner, Role::ElseClause, Role::If, Role::ValBranching }) {
		Form.ApplicableRoles.set(static_cast<size_t>(OwnerRole));
	}
	for(const Core::FormPart &Part : Form.Parts) if(Part.Role != Core::FormPartRole::Body && !Part.Def) {
		throw FormatLimitExceeded("the grammar has no kind for a brace body", FailureReason::FormatterDefect);
	}
	for(size_t Symbol = 0; Symbol < Kinds.size(); ++Symbol) {
		if(const Core::KindDef &Kind = Kinds[Symbol]; (Kind.Roles & Form.ApplicableRoles).any()) FormDefs[Symbol].push_back(Form);
	}
	// 終了
	return;
}

/**
 * CSS の空白の前後の除去関数
 * 計算量：テキスト長 L に対し O(L)
 * @param Text 読み取るテキスト
 * @return 前後の CSS の空白を除いたテキスト
 */
std::string_view GrammarProfile::TrimCss(std::string_view Text) {
	// CSS の空白の字
	constexpr std::string_view Spaces = " 	\n\r\f";
	Text.remove_prefix(std::min(Text.find_first_not_of(Spaces), Text.size()));
	Text.remove_suffix(Text.size() - std::min(Text.find_last_not_of(Spaces) + 1, Text.size()));
	// 前後の空白を除いたテキストの返戻
	return Text;
}

/**
 * CSS の空白の指定の値が空白又は改行を保持するかの判定関数
 * 計算量：空白分割した語の数 N，本文の長さ L に対し O(N + L)
 * @param Property 小文字へ揃えた宣言の名前
 * @param Val 小文字へ揃えた宣言の値
 * @return 保持する値又は未確定の値なら true
 */
bool GrammarProfile::ShouldPreserveCssWhitespace(const std::string_view Property, const std::string_view Val) {
	// 空白の畳方だけを指定する宣言の返戻
	if(Property == "white-space-collapse") return Val != "collapse" && Val != "discard";
	// 単独で保持を表す値の返戻
	if(Val == "pre" || Val == "pre-wrap" || Val == "pre-line") return true;
	bool ShouldKeep = false;
	// 一括指定の語毎の判定
	for(size_t Start = 0; Start != std::string_view::npos;) {
		const size_t End = std::min(Val.find_first_of(" 	\n\r\f", Start), Val.size());
		if(
			const std::string_view Token = Val.substr(Start, End - Start);
			Token == "preserve" || Token == "preserve-breaks" || Token == "preserve-spaces" || Token == "break-spaces"
		) ShouldKeep = true;
		// 未知の語（確定出来ない値）の返戻
		else if(
			Token != "normal" && Token != "nowrap" && Token != "wrap" && Token != "collapse" && Token != "discard" && Token != "auto" &&
			Token != "balance" && Token != "stable" && Token != "pretty" && Token != "suppress" && Token != "discard-before" &&
			Token != "discard-after" && Token != "discard-inner"
		) return true;
		Start = Val.find_first_not_of(" 	\n\r\f", End);
	}
	// 保持を表す語の有無の返戻
	return ShouldKeep;
}

/**
 * 属性値の ASCII 文字参照のデコード関数
 * 計算量：値の長さ L に対し O(L)
 * @param Val 属性のテキスト
 * @return デコードした値（未知又は ASCII でない参照が在れば空）
 * @throws std::bad_alloc デコードした属性値の領域を確保出来ない場合
 */
std::optional<std::string> GrammarProfile::DecodeMarkupAttribute(const std::string_view Val) {
	// 文字参照のデコードテキストの構築準備
	std::string Decoded;
	for(size_t Idx = 0; Idx < Val.size(); ++Idx) {
		if(Val[Idx] != '&') {
			Decoded += Val[Idx];
			continue;
		}
		size_t Cursor = Idx + 1;
		// 未対応の名前付参照を含む値の返戻
		if(Cursor == Val.size()) return std::nullopt;
		if(Val[Cursor] != '#') {
			static constexpr std::pair<std::string_view, unsigned char> Names[] {
				{ "NewLine;", '\n' },
				{ "Tab;", '	' },
				{ "amp;", '&' },
				{ "apos;", '\'' },
				{ "colon;", ':' },
				{ "gt;", '>' },
				{ "lt;", '<' },
				{ "quot;", '"' },
				{ "semi;", ';' },
				{ "sol;", '/' }
			};
			bool HasRef = false;
			for(const std::pair<std::string_view, unsigned char> &Name : Names) if(Val.substr(Cursor).starts_with(Name.first)) {
				Decoded += Name.second;
				Idx = Cursor + Name.first.size() - 1;
				HasRef = true;
				break;
			}
			// 識別不能な参照の返戻
			if(!HasRef) return std::nullopt;
			continue;
		}
		++Cursor;
		const bool IsHex = Cursor < Val.size() && (Val[Cursor] == 'x' || Val[Cursor] == 'X');
		if(IsHex) ++Cursor;
		const size_t Digits = Cursor;
		char32_t Point = 0;
		for(; Cursor < Val.size(); ++Cursor) {
			const unsigned char Char = Val[Cursor];
			const uint32_t Digit = Char >= '0' && Char <= '9' ?
			Char - '0' :
			IsHex && Char >= 'a' && Char <= 'f' ? Char + 10 - 'a' : IsHex && Char >= 'A' && Char <= 'F' ? Char + 10 - 'A' : 16;
			if(Digit >= (IsHex ? 16U : 10U)) break;
			Point = Point * (IsHex ? 16U : 10U) + Digit;
			// ASCII でない参照を含む値の返戻
			if(Point > 0X7F) return std::nullopt;
		}
		// 数字を持たない参照と NUL を含む値の返戻
		if(Cursor == Digits || !Point) return std::nullopt;
		Decoded += static_cast<char>(Point);
		if(Cursor < Val.size() && Val[Cursor] == ';') ++Cursor;
		Idx = Cursor - 1;
	}
	// デコードした属性値の返戻
	return Decoded;
}

/**
 * トークンの間の行内空白の判定関数
 * 計算量：O(1)
 * @param Point コードポイント
 * @return 文法が空白として読めば true
 */
bool ReadingRules::IsSpace(const char32_t Point) const {
	// 全ての文法が空白と読む ASCII の字の返戻
	if(Point == ' ' || Point == '	' || Point == '\v' || Point == '\f') return true;
	// BOM を空白と読むかの返戻
	if(Point == 0XFEFF) return CanReadByteOrderMarkAsSpace;
	// Zs の字を空白と読むかの返戻
	return CanReadUnicodeSpaces && IsUnicodeSpace(Point);
}

/**
 * コンストラクタ
 * 文法の全ての記号の役割と配置を導出し，同じ性質の共通種別を共有する（整形仕様 4.3）
 * 計算量：文法の記号数 N，場の数 F，記号名の長さの和 L，役割の導出・インデックス構築・生成形式登録の費用 Q に対し O(N + F + L + Q)
 * @param Lang 対象言語と方言
 * @throws std::bad_alloc 文法の役割・テキスト・インデックス又は生成形式の確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
GrammarProfile::GrammarProfile(const ::Lang Lang) : Lang(Lang) {
	Rules.HasDetailedMemberOrder = Lang == Lang::Cpp;
	if(Rules.HasDetailedMemberOrder) Rules.MemberVirtualWord = "virtual";
	Rules.InferredType = Lang == Lang::Cpp ?
	Core::InferredTypeNotation::Placeholder :
	Lang == Lang::CSharp ? Core::InferredTypeNotation::Implicit : Lang == Lang::Java ?
	Core::InferredTypeNotation::Named :
	Lang == Lang::TypeScript ? Core::InferredTypeNotation::DynamicTop : Core::InferredTypeNotation::None;
	Rules.Notation.Doc =
	Lang == Lang::CSharp ? Core::DocStyle::XmlTag : Lang == Lang::Rust ? Core::DocStyle::MarkdownHeading : Lang == Lang::Swift ?
	Core::DocStyle::Callout :
	Lang == Lang::Go ? Core::DocStyle::Prose : Lang == Lang::Python ? Core::DocStyle::Docstring : Core::DocStyle::AtTag;
	Rules.Notation.Vocabulary = GetDocVocabulary(Rules.Notation.Doc);
	Rules.Notation.TagVocabulary = AtTagVocabulary;
	Rules.Notation.DocTagTerminators = " \t[{";
	Rules.Notation.DocReferenceAttributes = {
		{ "paramref", "name" },
		{ "see", "cref" },
		{ "see", "href" },
		{ "see", "langword" },
		{ "seealso", "cref" },
		{ "seealso", "href" },
		{ "seealso", "langword" },
		{ "typeparamref", "name" }
	};
	Rules.DocForm = Lang.IsCFamily() ? "Doxygen block-comment syntax" : Lang == Lang::CSharp ?
	"XML block-comment syntax" :
	Lang == Lang::Java ? "Javadoc block-comment syntax" : Lang == Lang::Go ? "Go declaration documentation" : Lang == Lang::Rust ?
	"outer Rust block documentation" :
	Lang == Lang::Kotlin ? "KDoc block-comment syntax" : Lang == Lang::Swift ? "Swift block documentation" : Lang == Lang::PHP ?
	"PHPDoc block-comment syntax" :
	Lang.IsJsTs() ? "JSDoc block-comment syntax" : Lang == Lang::Ruby ? "YARD line documentation" : "";
	Rules.Notation.FailureTag =
	Lang == Lang::Ruby ? "@raise" : Core::VocabularyOf(Rules.Notation.TagVocabulary, Core::DocDescription::Failure).Canonical;
	if(Lang.IsCFamily()) {
		Rules.Notation.DocTagTerminators = {};
		Rules.Notation.DocParamNameTerminators = ":";
		Rules.Notation.SummaryTag = Core::VocabularyOf(Rules.Notation.TagVocabulary, Core::DocDescription::Summary).Canonical;
		Rules.Notation.TypeParamTag = DoxygenTypeParamTag;
	} else if(Lang.IsJsTs()) {
		Rules.Notation.SummaryTag = JsDescriptionTag;
		Rules.Notation.AlternateSummaryTag = JsSummaryTag;
		Rules.Notation.TypeParamTag = JsTemplateTag;
	}
	if(Lang == Lang::CSharp) Rules.Notation.DocTextEscapes = { { '&', "&amp;" }, { '<', "&lt;" }, { '>', "&gt;" } };
	else if(Lang == Lang::Java) Rules.Notation.DocHeadEscapes = { { '@', "&#64;" }, { '\\', "&#92;", true } };
	else if(Lang == Lang::Rust || Lang == Lang::Swift) {
		Rules.Notation.DocHeadEscapes = { { '#', "\\#" }, { '*', "\\*" }, { '+', "\\+" }, { '-', "\\-" } };
	} else if(Lang != Lang::Go && Lang != Lang::Python) Rules.Notation.DocHeadEscapes = { { '@', "\\@" }, { '\\', "\\\\" } };
	Rules.DefaultIntType = Lang == Lang::Java ? "int" : "";
	// 表示幅４・２のインデントを推奨する言語のネストの段数の上限（幅の狭い言語は同じ桁数へ倍の段が収まる）
	constexpr size_t WideIndentNestingLimit = 32, NarrowIndentNestingLimit = 64;
	Rules.NestingLimit = Lang.IsJsTs() || Lang == Lang::Ruby || Lang == Lang::JSON || Lang == Lang::HTML || Lang == Lang::CSS ?
	NarrowIndentNestingLimit :
	WideIndentNestingLimit;
	// 算術型の証拠は文法の基本型の種別に含まれる別名でなく，言語の予約語から定める
	if(Lang == Lang::C || Lang == Lang::Cpp) {
		std::vector<std::string_view> &ArithmeticSpellings = Rules.LexemeRoleSpellings[Core::Role::BuiltinArithmeticType];
		ArithmeticSpellings = { "char", "double", "float", "int", "long", "short", "signed", "unsigned" };
		Rules.LexemeRoleSpellings[Core::Role::SignedNumericType] = { "double", "float", "int", "long", "short", "signed" };
		Rules.LexemeRoleSpellings[Core::Role::NonnegativeNumericType] = { "unsigned" };
		if(Lang == Lang::Cpp) {
			ArithmeticSpellings.insert(ArithmeticSpellings.begin() + 1, { "char16_t", "char32_t", "char8_t" });
			ArithmeticSpellings.push_back("wchar_t");
			Rules.LexemeRoleSpellings[Core::Role::NonnegativeNumericType] = { "char16_t", "char32_t", "char8_t", "unsigned" };
		}
	}
	if(Lang == Lang::Python) Rules.LexemeRoleSpellings[Core::Role::SignedNumericType] = { "float", "int" };
	Rules.HasLeadingZeroOctal = Lang.IsCFamily() || Lang.IsJsTs() || Lang == Lang::Java || Lang == Lang::Go || Lang == Lang::PHP;
	if(Lang.IsCFamily()) Rules.MemberStaticWord = "static";
	Rules.ShouldOrderMembers =
	Lang == Lang::Cpp || Lang == Lang::CSharp || Lang == Lang::Java || Lang == Lang::Go || Lang == Lang::Rust ||
	Lang == Lang::Kotlin || Lang == Lang::Swift || Lang == Lang::PHP || Lang.IsJsTs() || Lang == Lang::Ruby || Lang == Lang::Python;
	std::unordered_map<std::string_view, Core::RoleSet> MemberRoles;
	const auto RegisterMember = [this, &MemberRoles](
		const std::string_view Target,
		const std::initializer_list<Core::Role> Roles,
		const Core::Role Owner = Core::Role::Count
	) -> void {
		for(const Core::Role Role : Roles) MemberRoles[Target].set(static_cast<size_t>(Role));
		if(Owner != Core::Role::Count) BindingOwners.emplace(Target, Owner);
		// 終了
		return;
	};
	Rules.MemberTypeModifiers = { "static" };
	switch(Lang.Id) {
	case Lang::CSharp:
		Rules.MemberTypeModifiers = { "const", "static" };
		break;
	case Lang::Rust:
		Rules.HasAssociatedFunctions = true;
		break;
	case Lang::Kotlin:
		Rules.MemberTypeModifiers = { "const" };
		Rules.HasBoundMemberModifiers = true;
		RegisterMember("kotlin.jvm.JvmField", { Core::Role::MemberTypeOperation }, Core::Role::MemberStaticOwner);
		RegisterMember("kotlin.jvm.JvmStatic", { Core::Role::MemberTypeOperation }, Core::Role::MemberStaticOwner);
		break;
	case Lang::Swift:
		Rules.MemberTypeModifiers = { "class", "static" };
		break;
	case Lang::Ruby:
		Rules.HasBoundMemberCalls = true;
		RegisterMember("attr", { Core::Role::MemberDocumentableCall });
		for(const std::string_view Name : { "attr_accessor", "attr_reader", "attr_writer" }) {
			RegisterMember(Name, { Core::Role::MemberDocumentableCall, Core::Role::MemberInstanceState });
		}
		for(const std::string_view Name : { "extend", "include", "prepend" }) RegisterMember(Name, { Core::Role::MemberRelation });
		Rules.MemberConstructorNames = { "initialize" };
		break;
	case Lang::Python:
		Rules.MemberTypeModifiers.clear();
		Rules.HasBoundMemberModifiers = true;
		RegisterMember("builtins.classmethod", { Core::Role::MemberTypeOperation });
		RegisterMember("builtins.property", { Core::Role::MemberProperty });
		RegisterMember("builtins.staticmethod", { Core::Role::MemberTypeOperation });
		Rules.MemberConstructorNames = { "__init__", "__new__" };
		Rules.MemberDestructorNames = { "__del__" };
		break;
	}
	Rules.AccessOrder = Lang == Lang::CSharp ?
	std::vector<std::string_view>{ "private", "private protected", "protected", "internal", "protected internal", "public" } :
	Lang == Lang::Java ?
	std::vector<std::string_view>{ "private", "package-private", "protected", "public" } :
	Lang == Lang::Kotlin ? std::vector<std::string_view>{ "private", "protected", "internal", "public" } : Lang == Lang::Swift ?
	std::vector<std::string_view>{ "private", "fileprivate", "internal", "package", "public", "open" } :
	Lang == Lang::Cpp || Lang == Lang::PHP || Lang == Lang::TypeScript || Lang == Lang::Ruby ?
	std::vector<std::string_view>{ "private", "protected", "public" } :
	Lang == Lang::JavaScript ? std::vector<std::string_view>{ "private", "public" } : Lang == Lang::Rust ?
	std::vector<std::string_view>{ "private", "pub(self)", "pub(super)", "pub(crate)", "pub" } :
	std::vector<std::string_view>{};
	Rules.ImplicitAccess = Lang == Lang::Java ?
	"package-private" :
	Lang == Lang::Swift ? "internal" : Lang == Lang::Cpp || Lang == Lang::CSharp || Lang == Lang::Rust ? "private" : "public";
	Rules.ShouldRequireExplicitPrivate = Lang == Lang::Cpp;
	Rules.ShouldRequireExplicitInternal = Lang == Lang::Swift;
	Rules.HasAccessSections = Lang == Lang::Cpp;
	Rules.HasModifierGroups = Lang == Lang::Java;
	Rules.HasReturnLabels = Lang == Lang::Kotlin;
	Rules.HasReturnComments = Lang != Lang::CSS;
	Rules.HasStrictEquality = Lang.IsJsTs();
	Rules.HasPositiveZeroCmp = Lang.IsCFamily() || Lang.IsJsTs() || Lang == Lang::Python;
	Rules.HasBoundaryCmp = Lang != Lang::JSON && Lang != Lang::HTML && Lang != Lang::CSS;
	Rules.HasZeroCmp = Rules.HasPositiveZeroCmp || Lang == Lang::PHP || Lang == Lang::Ruby;
	Rules.HasZeroInequality = Lang != Lang::Ruby;
	Rules.ShouldPreferBoolXorAssignment =
	Lang.IsCFamily() || Lang == Lang::CSharp || Lang == Lang::Java || Lang == Lang::Rust || Lang == Lang::Ruby ||
	Lang == Lang::Python;
	Rules.ShouldPreferUnitUpdate =
	Lang.IsCFamily() || Lang.IsJsTs() || Lang == Lang::CSharp || Lang == Lang::Java || Lang == Lang::Go || Lang == Lang::Kotlin ||
	Lang == Lang::PHP;
	Rules.ShouldPreferPrefixUpdate = Rules.ShouldPreferUnitUpdate && Lang != Lang::Go;
	// 演算子の各フィールドを位置で初期化する為，フィールドの宣言順を保つ
	Rules.Operators =
	{ "=", "==", "!=", "===", "!==", "&&", "||", "!", "+", "-", "+=", "-=", "++", "--", ">=", "<=", ">", "<", "." };
	// 論理演算を語で綴る記法
	if(Lang == Lang::Python) {
		Rules.Operators.LogicalAnd = "and";
		Rules.Operators.LogicalOr = "or";
		Rules.Operators.LogicalNot = "not";
	}
	Rules.Notation.ShouldReviewMarkdownLine = Lang == Lang::Java;
	Rules.Notation.HasMarkupComments = Lang == Lang::HTML;
	Rules.MarkupCommentOpen = Lang == Lang::HTML ? "<!--" : "";
	Rules.MarkupCommentClose = Lang == Lang::HTML ? "-->" : "";
	Rules.Notation.MarkupCommentOpen = Rules.MarkupCommentOpen;
	Rules.Notation.MarkupCommentClose = Rules.MarkupCommentClose;
	if(Lang == Lang::HTML) {
		Reading.PreserveSpaceVal = "preserve";
		Reading.NativeNamespaceVal = "http://www.w3.org/1999/xhtml";
		Reading.MarkupEncodingAttribute = "encoding";
		Reading.EmbeddedStyleType = "text/css";
		Reading.EmbeddedModuleType = "module";
		Reading.StyleRefElement = "link";
		Reading.StyleRefAttribute = "rel";
		Reading.StyleRefVal = "stylesheet";
		Reading.EmbeddedTypeAttribute = "type";
		Reading.AttributeValQuotes = "\"'";
		Reading.EmbeddedScriptLang = Lang::JavaScript;
		Reading.EmbeddedStyleLang = Lang::CSS;
		Reading.EmbeddedDataLang = Lang::JSON;
		Reading.EmbeddedScriptTypes = {
			"application/ecmascript",
			"application/javascript",
			"application/x-ecmascript",
			"application/x-javascript",
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
		Reading.EmbeddedDataTypes = { "application/json", "importmap", "speculationrules" };
		Reading.StructuredDataTypePrefix = "application/";
		Reading.StructuredDataTypeSuffix = "+json";
	}
	// 文法の場の共通役割の定義
	const TSLanguage *const Grammar = Lang.GetTreeSitterLang();
	static constexpr std::pair<std::string_view, Core::ChildRole> ChildRoles[] {
		{ "alias", Core::ChildRole::Alias },
		{ "alternative", Core::ChildRole::Alternative },
		{ "argument", Core::ChildRole::ArgField },
		{ "arguments", Core::ChildRole::Args },
		{ "block", Core::ChildRole::BlockField },
		{ "body", Core::ChildRole::Body },
		{ "communication", Core::ChildRole::Communication },
		{ "condition", Core::ChildRole::Cond },
		{ "consequence", Core::ChildRole::Consequence },
		{ "declarator", Core::ChildRole::Declarator },
		{ "directive", Core::ChildRole::DirectiveField },
		{ "exceptions", Core::ChildRole::Exceptions },
		{ "expr", Core::ChildRole::EvaluationSubject },
		{ "function", Core::ChildRole::FunctionField },
		{ "guard", Core::ChildRole::Guard },
		{ "handler", Core::ChildRole::Alternative },
		{ "if_true", Core::ChildRole::IfTrue },
		{ "increment", Core::ChildRole::Increment },
		{ "initializer", Core::ChildRole::Initializer },
		{ "left", Core::ChildRole::Left },
		{ "length", Core::ChildRole::Length },
		{ "macro", Core::ChildRole::Macro },
		{ "method", Core::ChildRole::MethodField },
		{ "name", Core::ChildRole::Name },
		{ "namespace", Core::ChildRole::SharedQualifier },
		{ "object", Core::ChildRole::ObjectField },
		{ "op", Core::ChildRole::Operator },
		{ "operand", Core::ChildRole::Operand },
		{ "operation", Core::ChildRole::Operator },
		{ "operator", Core::ChildRole::OperatorField },
		{ "parameter", Core::ChildRole::Params },
		{ "parameters", Core::ChildRole::ParamsField },
		{ "path", Core::ChildRole::Path },
		{ "pattern", Core::ChildRole::Pattern },
		{ "placement", Core::ChildRole::Placement },
		{ "prefix", Core::ChildRole::Qualifier },
		{ "receiver", Core::ChildRole::ReceiverField },
		{ "result", Core::ChildRole::Result },
		{ "return_type", Core::ChildRole::ReturnType },
		{ "returns", Core::ChildRole::ReturnType },
		{ "right", Core::ChildRole::Right },
		{ "scope", Core::ChildRole::ScopeField },
		{ "size", Core::ChildRole::SizeField },
		{ "subject", Core::ChildRole::EvaluationSubject },
		{ "superclass", Core::ChildRole::Superclass },
		{ "target", Core::ChildRole::Target },
		{ "type", Core::ChildRole::Type },
		{ "update", Core::ChildRole::Update },
		{ "value", Core::ChildRole::Val },
		{ "variable", Core::ChildRole::Var }
	};
	// 共通の役割を持たない場も，子が自身の場を持つ事実（隠れた規則の場を受け継がない）を保つ為に其の他の役割へコピーする
	FieldRoles.resize(ts_language_field_count(Grammar) + 1, Core::ChildRole::Other);
	FieldRoles.front() = Core::ChildRole::None;
	for(TSFieldId Field = 1; Field < FieldRoles.size(); ++Field) {
		const std::string_view Name = ts_language_field_name_for_id(Grammar, Field);
		for(const std::pair<std::string_view, Core::ChildRole> &Def : ChildRoles) if(Name == Def.first) {
			FieldRoles[Field] = Def.second;
			break;
		}
		ChildRoleMask |= GetFieldBits(Field);
		const size_t RoleIdx = static_cast<size_t>(FieldRoles[Field]);
		if(FieldsByRole.size() <= RoleIdx) FieldsByRole.resize(RoleIdx + 1);
		FieldsByRole[RoleIdx].push_back(Field);
	}
	// 記号毎の定義領域の初期化
	const uint32_t Count = ts_language_symbol_count(Grammar);
	Kinds.resize(Count);
	if(Lang == Lang::Go) Reading.AutomaticTerminators.resize(Count);
	if(Lang == Lang::TypeScript) Reading.PrefixTypes.resize(Count);
	BlankKeys.resize(Count);
	SymbolNames.resize(Count);
	NameSymbols.resize(Count);
	FormDefs.resize(Count);
	std::unordered_map<std::string_view, TSSymbol> FirstByName;
	// 公開ノード型の名前と名前付か否かを，パーサーの記号から独立した空行比較の値へ投影する
	std::unordered_map<std::string_view, std::array<uint32_t, 2>> BlankKeysByName;
	uint32_t NextBlankKey = 0;
	for(TSSymbol Symbol = 0; Symbol < Count; ++Symbol) {
		Core::KindDef &Kind = Kinds[Symbol];
		SymbolNames[Symbol] = ts_language_symbol_name(Grammar, Symbol);
		NameSymbols[Symbol] = FirstByName.try_emplace(SymbolNames[Symbol], Symbol).first->second;
		const TSSymbolType SymbolType = ts_language_symbol_type(Grammar, Symbol);
		Kind.IsNamed = SymbolType == TSSymbolTypeRegular;
		Kind.Roles = DeriveRoles(Lang, SymbolNames[Symbol], Kind.IsNamed);
		Kind.InitRoleList();
		if(Lang == Lang::Go) {
			const std::string_view Name = SymbolNames[Symbol];
			AutomaticTerminatorReading &Terminator = Reading.AutomaticTerminators[Symbol];
			Terminator.IsHost = Names::GoSemicolonHost.HasName(Name);
			Terminator.IsWholeToken = Names::GoSemicolonLiteral.HasName(Name);
			Terminator.IsToken = Terminator.IsWholeToken || Kind.Roles.test(static_cast<size_t>(Core::Role::Identifier)) ||
			Kind.Roles.test(static_cast<size_t>(Core::Role::NumLiteral)) || Names::GoSemicolonNamedToken.HasName(Name) ||
			!Kind.IsNamed && Names::GoSemicolonToken.HasName(Name);
		}
		if(Lang == Lang::TypeScript) {
			// 優先順位は条件・関数・合併・交差・前置・後置の順に強く為る
			const std::string_view Name = SymbolNames[Symbol];
			// 連続する前置型の読違を修復する為のトークンと被演算子の対応
			if(Names::TypePrefixOperand.HasName(Name)) Reading.PrefixTypes[Symbol] = PrefixTypePart::Operand;
			else if(Names::TypePrefixWeak.HasName(Name)) Reading.PrefixTypes[Symbol] = PrefixTypePart::Weak;
			else if(Names::TypePrefixOperator.HasName(Name)) Reading.PrefixTypes[Symbol] = PrefixTypePart::Operator;
			else if(Names::TypePrefixKeyword.HasName(Name)) Reading.PrefixTypes[Symbol] = PrefixTypePart::Prefix;
			else if(Names::TypePrefixWrapper.HasName(Name)) Reading.PrefixTypes[Symbol] = PrefixTypePart::Wrapper;
			Core::TypeBindingDef &Binding = Kind.TypeBinding;
			if(Name == "conditional_type") Binding.Precedence = 1;
			else if(Name == "constructor_type" || Name == "function_type" || Name == "infer_type") Binding.Precedence = 2;
			else if(Name == "union_type") {
				Binding = { .Precedence = 3, .OperandPrecedence = 3, .HasOperand = true, .RequiresCallableGroup = true };
			} else if(Name == "intersection_type") {
				Binding =
				{ .Precedence = 4, .OperandPrecedence = 4, .HasOperand = true, .PreservesGrouping = true, .RequiresCallableGroup = true };
			} else if(Name == "index_type_query" || Name == "readonly_type") {
				Binding = { .Precedence = 5, .OperandPrecedence = 5, .HasOperand = true };
			} else if(Name == "array_type" || Name == "optional_type") {
				Binding = { .Precedence = 6, .OperandPrecedence = 6, .HasOperand = true };
			} else if(Name == "lookup_type") Binding = { .Precedence = 6, .OperandPrecedence = 6, .HasOperand = true, .IsIndex = true };
			else if(Name == "rest_type") Binding = { .Precedence = 1, .HasOperand = true };
			else if(Name == "as_expression" || Name == "satisfies_expression") Binding = { .HasOperand = true, .IsOpenTail = true };
		}
		if(Kind.IsNamed) {
			Kind.Evaluation = GetEvaluationDef(Lang, SymbolNames[Symbol], Kind.Roles);
			Kind.Binding = GetBindingSyntax(Lang, SymbolNames[Symbol], Kind.Roles);
			Kind.Bindings = GetBindingDef(Lang, SymbolNames[Symbol]);
			if(
				Lang.IsCFamily() || Lang == Lang::CSharp || Lang == Lang::Java || Lang == Lang::Go || Lang == Lang::Kotlin ||
				Lang == Lang::Swift
			) {
				Kind.Bindings.IsStaticItem = Kind.Bindings.IsStaticItem || Kind.Binding == Core::BindingSyntax::FunctionDecl ||
				Kind.Binding == Core::BindingSyntax::MemberFunction;
			}
			if(Lang.IsCFamily() && Kind.Binding == Core::BindingSyntax::ClassDecl) Kind.Bindings.Namespace = Core::BindingNamespace::Type;
		}
		uint32_t &BlankKey = BlankKeysByName[SymbolNames[Symbol]][Kind.IsNamed];
		if(!BlankKey) BlankKey = ++NextBlankKey;
		BlankKeys[Symbol] = BlankKey;
		// 生成するトークンの種別はテキスト毎に最初の見えるトークンの記号で引き，テキストのパーサーの記号はパーサーがノードに公開する記号をテキスト毎に１度だけ引く
		if(SymbolType == TSSymbolTypeAnonymous) {
			// Go の文法の `\0` は改行・`;` と同じ終端で，EOF では幅０の無名の葉に為る
			if(Lang == Lang::Go && SymbolNames[Symbol].empty()) EofTerminator = Symbol;
			Rules.TokenDefs.try_emplace(SymbolNames[Symbol], Core::FormPart{ .Def = &Kind });
			if(const std::string_view Name = SymbolNames[Symbol]; !TokenSymbols.contains(Name)) {
				TokenSymbols.emplace(Name, ts_language_symbol_for_name(Grammar, Name.data(), static_cast<uint32_t>(Name.size()), false));
			}
		}
	}
	// エラー領域の種別の準備
	ErrorKind.IsNamed = true;
	ErrorKind.Roles = DeriveRoles(Lang, "ERROR", true);
	ErrorKind.InitRoleList();
	ErrorBlankKey = ++NextBlankKey;
	// 本体の外の地の文は投影が加える逐語の子の為，文法の地の文の役割を持たせず本体の配置から外す（整形仕様 4.4）
	OuterTextKind.IsNamed = true;
	OuterTextKind.Roles.set(static_cast<size_t>(Core::Role::OuterText));
	TrailingDataKind.IsNamed = true;
	TrailingDataKind.Roles.set(static_cast<size_t>(Core::Role::OuterText));
	TrailingDataKind.Roles.set(static_cast<size_t>(Core::Role::TrailingDataSection));
	OuterTextKind.InitRoleList();
	TrailingDataKind.InitRoleList();
	// 使用可能な記法（井桁の行コメントを持つ言語，原文の先頭の `#!` を読む言語，ドキュメントの囲みを持つ言語，処理系が定める名前付のディレクティブ記法，整形仕様 3.2，4.5）
	// 開始記号の最初の一致を採る為，重なる記法の登録順を保つ
	const bool HasHashComment = Lang == Lang::PHP || Lang == Lang::Ruby || Lang == Lang::Python;
	const bool HasBlockComment = Lang != Lang::Ruby && Lang != Lang::Python && Lang != Lang::HTML;
	std::vector<Core::AvailableNotation> &Available = Rules.Notation.AvailableNotations;
	if(HasBlockComment && (Lang != Lang::CSS || Lang.IsScss)) Available.push_back({ CommentNotation::SlashLine, false });
	if(HasHashComment) Available.push_back({ CommentNotation::HashLine, false });
	if(Lang == Lang::PHP) Reading.LineCommentCodeHeads = { "#[" };
	if(HasHashComment || Lang.IsJsTs() || Lang == Lang::Rust) Available.push_back({ CommentNotation::HashBangLine, false });
	if(HasBlockComment) Available.push_back({ CommentNotation::Block, false });
	if(HasBlockComment && Lang != Lang::Go && Lang != Lang::JSON && Lang != Lang::CSS) {
		Available.push_back({ CommentNotation::DoubleStarBlock, false });
	}
	if(Lang.IsCFamily() || Lang == Lang::Rust || Lang.IsScss) Available.push_back({ CommentNotation::StarBangBlock, false });
	if(Lang.IsCFamily() || Lang == Lang::Rust) Available.push_back({ CommentNotation::SlashBangLine, false });
	if(
		Lang.IsCFamily() || Lang == Lang::CSharp || Lang == Lang::Java || Lang == Lang::Rust || Lang == Lang::Swift ||
		Lang == Lang::TypeScript
	) Available.push_back({ CommentNotation::TripleSlashLine, false });
	if(Lang == Lang::Ruby) {
		Available.push_back({ CommentNotation::HashColonLine, false });
		Available.push_back({ CommentNotation::RubyEmbeddedDocument, false });
	}
	Rules.Notation.IsBlockCommentNested = Lang == Lang::Rust || Lang == Lang::Kotlin || Lang == Lang::Swift;
	Reading.HasCommentDocContent = Lang == Lang::HTML;
	Reading.HasDirectiveHeader =
	Lang == Lang::Go || Lang == Lang::Ruby || Lang == Lang::Python || Lang.IsCFamily() || Lang == Lang::TypeScript;
	Reading.HasSingleLineEncodingHeader = Lang == Lang::Ruby;
	Reading.HasConditionalSecondLineEncoding = Lang == Lang::Python;
	Reading.HasOutputCommentGroups = Lang == Lang::Go;
	Reading.HasToolchainCommentDirectives = Lang == Lang::Go;
	Reading.HasHeaderBuildConstraints = Lang.IsCFamily();
	Reading.HasPosDirectiveComments = Lang == Lang::Go || Lang.IsCFamily();
	Reading.CanReadUnicodeSpaces = Lang.IsJsTs() || Lang == Lang::CSharp;
	Reading.ShouldRejectExtraLeadingBom = Lang.IsCFamily() || Lang == Lang::Java || Lang == Lang::Python || Lang == Lang::JSON;
	Reading.CanReadByteOrderMarkAsSpace = Lang.IsJsTs();
	if(Lang == Lang::C && Lang.IsAmbiguous) {
		Reading.ForeignDialectHeads = { "#import", "@implementation", "@interface", "@protocol" };
	}
	if(Lang == Lang::TypeScript) {
		Reading.MarkupDocHeads = { "<!DOCTYPE", "<?xml" };
		if(!Lang.IsJsx) {
			Reading.MarkupDocRoot = "TS";
			Reading.MarkupDocAttribute = "version";
		}
	}
	if(Lang == Lang::HTML) {
		Reading.RawTextEscapeElement = "script";
		Reading.RawTextEscapeOpen = "<!--";
		Reading.RawTextEndOpen = "</";
		Reading.RawTextNameTerminators = "	\n\f\r />";
		Reading.RawTextPlainElement = "style";
	}
	if(Lang == Lang::HTML || Lang == Lang::CSS && !Lang.IsScss) {
		// Encoding Standard のラベルと ASCII の値を常に保つエンコーディングの登録
		Reading.EncodingPrescanBytes = 1024;
		Reading.BomEncodingName = "utf-8";
		Reading.EncodingLabels = {
			{ "big5", { "big5", "big5-hkscs", "cn-big5", "csbig5", "x-x-big5" } },
			{ "euc-jp", { "cseucpkdfmtjapanese", "euc-jp", "x-euc-jp" } },
			{
				"euc-kr",
				{
					"cseuckr",
					"csksc56011987",
					"euc-kr",
					"iso-ir-149",
					"korean",
					"ks_c_5601-1987",
					"ks_c_5601-1989",
					"ksc5601",
					"ksc_5601",
					"windows-949"
				}
			},
			{ "gb18030", { "gb18030" } },
			{ "gbk", { "chinese", "csgb2312", "csiso58gb231280", "gb2312", "gb_2312", "gb_2312-80", "gbk", "iso-ir-58", "x-gbk" } },
			{ "ibm866", { "866", "cp866", "csibm866", "ibm866" } },
			{ "iso-2022-jp", { "csiso2022jp", "iso-2022-jp" } },
			{ "iso-8859-10", { "csisolatin6", "iso-8859-10", "iso-ir-157", "iso8859-10", "iso885910", "l6", "latin6" } },
			{ "iso-8859-13", { "iso-8859-13", "iso8859-13", "iso885913" } },
			{ "iso-8859-14", { "iso-8859-14", "iso8859-14", "iso885914" } },
			{ "iso-8859-15", { "csisolatin9", "iso-8859-15", "iso8859-15", "iso885915", "iso_8859-15", "l9" } },
			{ "iso-8859-16", { "iso-8859-16" } },
			{
				"iso-8859-2",
				{ "csisolatin2", "iso-8859-2", "iso-ir-101", "iso8859-2", "iso88592", "iso_8859-2", "iso_8859-2:1987", "l2", "latin2" }
			},
			{
				"iso-8859-3",
				{ "csisolatin3", "iso-8859-3", "iso-ir-109", "iso8859-3", "iso88593", "iso_8859-3", "iso_8859-3:1988", "l3", "latin3" }
			},
			{
				"iso-8859-4",
				{ "csisolatin4", "iso-8859-4", "iso-ir-110", "iso8859-4", "iso88594", "iso_8859-4", "iso_8859-4:1988", "l4", "latin4" }
			},
			{
				"iso-8859-5",
				{ "csisolatincyrillic", "cyrillic", "iso-8859-5", "iso-ir-144", "iso8859-5", "iso88595", "iso_8859-5", "iso_8859-5:1988" }
			},
			{
				"iso-8859-6",
				{
					"arabic",
					"asmo-708",
					"csiso88596e",
					"csiso88596i",
					"csisolatinarabic",
					"ecma-114",
					"iso-8859-6",
					"iso-8859-6-e",
					"iso-8859-6-i",
					"iso-ir-127",
					"iso8859-6",
					"iso88596",
					"iso_8859-6",
					"iso_8859-6:1987"
				}
			},
			{
				"iso-8859-7",
				{
					"csisolatingreek",
					"ecma-118",
					"elot_928",
					"greek",
					"greek8",
					"iso-8859-7",
					"iso-ir-126",
					"iso8859-7",
					"iso88597",
					"iso_8859-7",
					"iso_8859-7:1987",
					"sun_eu_greek"
				}
			},
			{
				"iso-8859-8",
				{
					"csiso88598e",
					"csisolatinhebrew",
					"hebrew",
					"iso-8859-8",
					"iso-8859-8-e",
					"iso-ir-138",
					"iso8859-8",
					"iso88598",
					"iso_8859-8",
					"iso_8859-8:1988",
					"visual"
				}
			},
			{ "iso-8859-8-i", { "csiso88598i", "iso-8859-8-i", "logical" } },
			{ "koi8-r", { "cskoi8r", "koi", "koi8", "koi8-r", "koi8_r" } },
			{ "koi8-u", { "koi8-ru", "koi8-u" } },
			{ "macintosh", { "csmacintosh", "mac", "macintosh", "x-mac-roman" } },
			{ "replacement", { "csiso2022kr", "hz-gb-2312", "iso-2022-cn", "iso-2022-cn-ext", "iso-2022-kr", "replacement" } },
			{ "shift_jis", { "csshiftjis", "ms932", "ms_kanji", "shift-jis", "shift_jis", "sjis", "windows-31j", "x-sjis" } },
			{ "utf-16be", { "unicodefffe", "utf-16be" } },
			{ "utf-16le", { "csunicode", "iso-10646-ucs-2", "ucs-2", "unicode", "unicodefeff", "utf-16", "utf-16le" } },
			{ "utf-8", { "unicode-1-1-utf-8", "unicode11utf8", "unicode20utf8", "utf-8", "utf8", "x-unicode20utf8" } },
			{ "windows-1250", { "cp1250", "windows-1250", "x-cp1250" } },
			{ "windows-1251", { "cp1251", "windows-1251", "x-cp1251" } },
			{
				"windows-1252",
				{
					"ansi_x3.4-1968",
					"ascii",
					"cp1252",
					"cp819",
					"csisolatin1",
					"ibm819",
					"iso-8859-1",
					"iso-ir-100",
					"iso8859-1",
					"iso88591",
					"iso_8859-1",
					"iso_8859-1:1987",
					"l1",
					"latin1",
					"us-ascii",
					"windows-1252",
					"x-cp1252"
				}
			},
			{ "windows-1253", { "cp1253", "windows-1253", "x-cp1253" } },
			{
				"windows-1254",
				{
					"cp1254",
					"csisolatin5",
					"iso-8859-9",
					"iso-ir-148",
					"iso8859-9",
					"iso88599",
					"iso_8859-9",
					"iso_8859-9:1989",
					"l5",
					"latin5",
					"windows-1254",
					"x-cp1254"
				}
			},
			{ "windows-1255", { "cp1255", "windows-1255", "x-cp1255" } },
			{ "windows-1256", { "cp1256", "windows-1256", "x-cp1256" } },
			{ "windows-1257", { "cp1257", "windows-1257", "x-cp1257" } },
			{ "windows-1258", { "cp1258", "windows-1258", "x-cp1258" } },
			{ "windows-874", { "dos-874", "iso-8859-11", "iso8859-11", "iso885911", "tis-620", "windows-874" } },
			{ "x-mac-cyrillic", { "x-mac-cyrillic", "x-mac-ukrainian" } },
			{ "x-user-defined", { "x-user-defined" } }
		};
		Reading.AllowedEncodingNames = {
			"euc-jp",
			"ibm866",
			"iso-8859-10",
			"iso-8859-13",
			"iso-8859-14",
			"iso-8859-15",
			"iso-8859-16",
			"iso-8859-2",
			"iso-8859-3",
			"iso-8859-4",
			"iso-8859-5",
			"iso-8859-6",
			"iso-8859-7",
			"iso-8859-8",
			"iso-8859-8-i",
			"koi8-r",
			"koi8-u",
			"macintosh",
			"utf-8",
			"windows-1250",
			"windows-1251",
			"windows-1252",
			"windows-1253",
			"windows-1254",
			"windows-1255",
			"windows-1256",
			"windows-1257",
			"windows-1258",
			"windows-874",
			"x-mac-cyrillic",
			"x-user-defined"
		};
		// ASCII の宣言に書かれた UTF-16 は，宣言のバイト列を読めたエンコーディングへ対応する
		Reading.EncodingPrescanReplacements = { { "utf-16be", "utf-8" }, { "utf-16le", "utf-8" } };
		if(Lang == Lang::HTML) {
			Reading.HasMarkupEncodingPrescan = true;
			Reading.EncodingElementName = "meta";
			Reading.EncodingCharsetAttribute = "charset";
			Reading.EncodingContentAttribute = "content";
			Reading.MetaEncodingReplacements = { { "x-user-defined", "windows-1252" } };
		} else {
			Reading.EncodingBytePrefix = "@charset \"";
			Reading.EncodingByteSuffix = "\";";
		}
	}
	// 行コメントの登録形の判定条件（登録定義の内の記法・本文・本文の中の位置の判定に要る物）
	Core::RegisteredLineForms &Forms = Rules.Notation.OutputForms.Conditions;
	if(Lang == Lang::Ruby || Lang == Lang::Python) {
		Forms.EncodingDeclWord = "coding";
		Forms.EncodingDeclSeparators = ":=";
	}
	switch(Lang.Id) {
	case Lang::Python:
		{
			// 登録語の大小文字を区別して直後の区切と名前を読み，２行目は行頭の空白と書式送りの後の記号だけを読む (PEP 263)
			Forms.EncodingNameChars = "-_.";
			Reading.EncodingLineIndent = " 	\f";
			Reading.EncodingNameJoiners = "_";
			Reading.BomEncodingName = "utf-8";
			// ASCII の値を保つ状態の無いエンコーディングだけを読み，多バイトの後続バイトに ASCII の値を許さない
			Reading.AllowedEncodingNames = {
				"1125",
				"1250",
				"1251",
				"1252",
				"1253",
				"1254",
				"1255",
				"1256",
				"1257",
				"1258",
				"437",
				"646",
				"775",
				"850",
				"852",
				"855",
				"857",
				"858",
				"860",
				"861",
				"862",
				"863",
				"865",
				"866",
				"869",
				"8859",
				"ansi-x3-4-1968",
				"ansi-x3.4-1968",
				"ansi-x3.4-1986",
				"arabic",
				"ascii",
				"asmo-708",
				"chinese",
				"cp-gr",
				"cp-is",
				"cp1006",
				"cp1125",
				"cp1250",
				"cp1251",
				"cp1252",
				"cp1253",
				"cp1254",
				"cp1255",
				"cp1256",
				"cp1257",
				"cp1258",
				"cp367",
				"cp437",
				"cp65001",
				"cp720",
				"cp737",
				"cp775",
				"cp819",
				"cp850",
				"cp852",
				"cp855",
				"cp856",
				"cp857",
				"cp858",
				"cp860",
				"cp861",
				"cp862",
				"cp863",
				"cp865",
				"cp866",
				"cp866u",
				"cp869",
				"cp874",
				"csascii",
				"csibm855",
				"csibm857",
				"csibm858",
				"csibm860",
				"csibm861",
				"csibm863",
				"csibm865",
				"csibm866",
				"csibm869",
				"csiso58gb231280",
				"csisolatin1",
				"csisolatin2",
				"csisolatin3",
				"csisolatin4",
				"csisolatin5",
				"csisolatin6",
				"csisolatinarabic",
				"csisolatincyrillic",
				"csisolatingreek",
				"csisolatinhebrew",
				"cskoi8r",
				"cspc775baltic",
				"cspc850multilingual",
				"cspc862latinhebrew",
				"cspc8codepage437",
				"cspcp852",
				"cyrillic",
				"ecma-114",
				"ecma-118",
				"elot-928",
				"euc-cn",
				"euc-jis-2004",
				"euc-jis2004",
				"euc-jisx0213",
				"euc-jp",
				"euc-kr",
				"euccn",
				"eucgb2312-cn",
				"eucjis2004",
				"eucjisx0213",
				"eucjp",
				"euckr",
				"gb2312",
				"gb2312-1980",
				"gb2312-80",
				"greek",
				"greek8",
				"hebrew",
				"ibm1125",
				"ibm367",
				"ibm437",
				"ibm775",
				"ibm819",
				"ibm850",
				"ibm852",
				"ibm855",
				"ibm857",
				"ibm858",
				"ibm860",
				"ibm861",
				"ibm862",
				"ibm863",
				"ibm865",
				"ibm866",
				"ibm869",
				"iso-646.irv-1991",
				"iso-8859-1",
				"iso-8859-1-1987",
				"iso-8859-10",
				"iso-8859-10-1992",
				"iso-8859-11",
				"iso-8859-11-2001",
				"iso-8859-13",
				"iso-8859-14",
				"iso-8859-14-1998",
				"iso-8859-15",
				"iso-8859-16",
				"iso-8859-16-2001",
				"iso-8859-2",
				"iso-8859-2-1987",
				"iso-8859-3",
				"iso-8859-3-1988",
				"iso-8859-4",
				"iso-8859-4-1988",
				"iso-8859-5",
				"iso-8859-5-1988",
				"iso-8859-6",
				"iso-8859-6-1987",
				"iso-8859-7",
				"iso-8859-7-1987",
				"iso-8859-8",
				"iso-8859-8-1988",
				"iso-8859-9",
				"iso-8859-9-1989",
				"iso-celtic",
				"iso-ir-100",
				"iso-ir-101",
				"iso-ir-109",
				"iso-ir-110",
				"iso-ir-126",
				"iso-ir-127",
				"iso-ir-138",
				"iso-ir-144",
				"iso-ir-148",
				"iso-ir-157",
				"iso-ir-199",
				"iso-ir-226",
				"iso-ir-58",
				"iso-ir-6",
				"iso-latin-1",
				"iso646-us",
				"iso8859",
				"iso8859-1",
				"iso8859-10",
				"iso8859-11",
				"iso8859-13",
				"iso8859-14",
				"iso8859-15",
				"iso8859-16",
				"iso8859-2",
				"iso8859-3",
				"iso8859-4",
				"iso8859-5",
				"iso8859-6",
				"iso8859-7",
				"iso8859-8",
				"iso8859-9",
				"jisx0213",
				"koi8-r",
				"koi8-t",
				"koi8-u",
				"korean",
				"ks-c-5601",
				"ks-c-5601-1987",
				"ks-x-1001",
				"ksc5601",
				"ksx1001",
				"l1",
				"l10",
				"l2",
				"l3",
				"l4",
				"l5",
				"l6",
				"l7",
				"l8",
				"l9",
				"latin",
				"latin-1",
				"latin1",
				"latin10",
				"latin2",
				"latin3",
				"latin4",
				"latin5",
				"latin6",
				"latin7",
				"latin8",
				"latin9",
				"mac-centeuro",
				"mac-croatian",
				"mac-cyrillic",
				"mac-greek",
				"mac-iceland",
				"mac-latin2",
				"mac-roman",
				"mac-romanian",
				"mac-turkish",
				"maccentraleurope",
				"maccyrillic",
				"macgreek",
				"maciceland",
				"macintosh",
				"maclatin2",
				"macroman",
				"macturkish",
				"ruscii",
				"thai",
				"u-jis",
				"u8",
				"ujis",
				"us",
				"us-ascii",
				"utf",
				"utf-8",
				"utf8",
				"utf8-ucs2",
				"utf8-ucs4",
				"windows-1250",
				"windows-1251",
				"windows-1252",
				"windows-1253",
				"windows-1254",
				"windows-1255",
				"windows-1256",
				"windows-1257",
				"windows-1258",
				"x-mac-korean",
				"x-mac-simp-chinese"
			};
			Reading.EncodingNamePrefixes =
			{ { "iso-8859-1-", "iso-8859-1" }, { "iso-latin-1-", "iso-8859-1" }, { "latin-1-", "iso-8859-1" }, { "utf-8-", "utf-8" } };
		}
		break;
	case Lang::Ruby:
		{
			// シバンの処理系名と起動オプションの値・引数の消費を定義する
			Reading.ShebangInterpreterName = "ruby";
			Reading.ShebangEncodingOption = 'K';
			Reading.ShebangEncodingNames = {
				{ 'A', "ascii-8bit" },
				{ 'E', "euc-jp" },
				{ 'N', "ascii-8bit" },
				{ 'S', "windows-31j" },
				{ 'U', "utf-8" },
				{ 'a', "ascii-8bit" },
				{ 'e', "euc-jp" },
				{ 'n', "ascii-8bit" },
				{ 's', "windows-31j" },
				{ 'u', "utf-8" }
			};
			Reading.ShebangFlagOptions = "SUacdlnpsvwy";
			Reading.ShebangOptions = {
				{ "0", ShebangArgument::Optional, {}, "01234567", 3 },
				{ "C", ShebangArgument::Required },
				{ "E", ShebangArgument::Required },
				{ "F", ShebangArgument::Optional },
				{ "I", ShebangArgument::Required },
				{ "W", ShebangArgument::Optional, ":", "01234567", 1 },
				{ "X", ShebangArgument::Required },
				{ "backtrace-limit", ShebangArgument::Required, "=" },
				{ "copyright", ShebangArgument::None },
				{ "crash-report", ShebangArgument::Required, "-=" },
				{ "debug", ShebangArgument::Optional, "-=" },
				{ "disable", ShebangArgument::Required, "-=" },
				{ "dump", ShebangArgument::Required, "=" },
				{ "e", ShebangArgument::Required },
				{ "enable", ShebangArgument::Required, "-=" },
				{ "encoding", ShebangArgument::Required, "=" },
				{ "external-encoding", ShebangArgument::Required, "=" },
				{ "i", ShebangArgument::Optional },
				{ "internal-encoding", ShebangArgument::Required, "=" },
				{ "jit", ShebangArgument::None },
				{ "parser", ShebangArgument::Required, "=" },
				{ "r", ShebangArgument::Required },
				{ "rjit", ShebangArgument::Optional, "-=" },
				{ "verbose", ShebangArgument::None },
				{ "version", ShebangArgument::None },
				{ "x", ShebangArgument::Optional },
				{ "yjit", ShebangArgument::Optional, "-=" },
				{ "yydebug", ShebangArgument::None }
			};
			// 注記全体の組か囲みの中の組を先に読み，組として読めない注記だけから大小文字を問わず空白を挟み得る登録語を探す
			Forms.IsEncodingWordCaseless = true;
			Forms.CanAllowEncodingSeparatorSpace = true;
			Forms.EncodingMagicCommentNames = { "coding", "encoding" };
			// ASCII の値を保ち，後続のバイトも ASCII と重ならない名前と処理系の別名だけを読む
			Reading.AllowedEncodingNames = {
				"646",
				"ansi_x3.4-1968",
				"ascii",
				"ascii-8bit",
				"binary",
				"cesu-8",
				"cp1250",
				"cp1251",
				"cp1252",
				"cp1253",
				"cp1254",
				"cp1255",
				"cp1256",
				"cp1257",
				"cp1258",
				"cp437",
				"cp51932",
				"cp65001",
				"cp720",
				"cp737",
				"cp775",
				"cp850",
				"cp852",
				"cp855",
				"cp857",
				"cp860",
				"cp861",
				"cp862",
				"cp863",
				"cp864",
				"cp865",
				"cp866",
				"cp869",
				"cp874",
				"cp878",
				"emacs-mule",
				"euc-cn",
				"euc-jis-2004",
				"euc-jisx0213",
				"euc-jp",
				"euc-jp-ms",
				"euc-kr",
				"euc-tw",
				"euccn",
				"eucjp",
				"eucjp-ms",
				"euckr",
				"euctw",
				"gb12345",
				"gb1988",
				"gb2312",
				"ibm437",
				"ibm720",
				"ibm737",
				"ibm775",
				"ibm850",
				"ibm852",
				"ibm855",
				"ibm857",
				"ibm860",
				"ibm861",
				"ibm862",
				"ibm863",
				"ibm864",
				"ibm865",
				"ibm866",
				"ibm869",
				"iso-8859-1",
				"iso-8859-10",
				"iso-8859-11",
				"iso-8859-13",
				"iso-8859-14",
				"iso-8859-15",
				"iso-8859-16",
				"iso-8859-2",
				"iso-8859-3",
				"iso-8859-4",
				"iso-8859-5",
				"iso-8859-6",
				"iso-8859-7",
				"iso-8859-8",
				"iso-8859-9",
				"iso8859-1",
				"iso8859-10",
				"iso8859-11",
				"iso8859-13",
				"iso8859-14",
				"iso8859-15",
				"iso8859-16",
				"iso8859-2",
				"iso8859-3",
				"iso8859-4",
				"iso8859-5",
				"iso8859-6",
				"iso8859-7",
				"iso8859-8",
				"iso8859-9",
				"koi8-r",
				"koi8-u",
				"maccenteuro",
				"maccroatian",
				"maccyrillic",
				"macgreek",
				"maciceland",
				"macroman",
				"macromania",
				"macthai",
				"macturkish",
				"macukraine",
				"tis-620",
				"us-ascii",
				"utf-8",
				"utf-8-hfs",
				"utf-8-mac",
				"utf8-docomo",
				"utf8-kddi",
				"utf8-mac",
				"utf8-softbank",
				"windows-1250",
				"windows-1251",
				"windows-1252",
				"windows-1253",
				"windows-1254",
				"windows-1255",
				"windows-1256",
				"windows-1257",
				"windows-1258",
				"windows-874"
			};
			Forms.EncodingNameChars = "-_";
			Reading.EncodingLineIndent = "\t\v\f\r ";
			// 文字列の凍結は最初のトークンより前，定数の共有はコメントだけの行，インデントの警告は全てのコメントで読む
			Reading.MagicCommentWords = {
				{ "frozen_string_literal", MagicCommentScope::Header, { "false", "true" } },
				{
					"shareable_constant_value",
					MagicCommentScope::CommentLine,
					{ "experimental_copy", "experimental_everything", "literal", "none" }
				},
				{ "warn_indent", MagicCommentScope::Anywhere, { "false", "true" } }
			};
			for(const MagicCommentWord &Word : Reading.MagicCommentWords) Forms.MagicCommentValues.emplace(Word.Name, Word.Vals);
			Forms.MagicCommentMarker = "-*-";
			Forms.MagicCommentSeparators = ":";
			Forms.MagicCommentDelimiters = ";";
			Forms.MagicCommentQuotes = "\"'";
			Forms.MagicCommentNameJoiners = "-_";
		}
		break;
	case Lang::Go:
		{
			Reading.OutputFunctionPrefix = "Example";
			Reading.OutputFunctionEmptyFields = { Core::ChildRole::ParamsField, Core::ChildRole::Result };
			Reading.OutputFunctionExcludedInitialRanges = Core::UnicodeAlphanumeric::LowercaseRanges;
			Reading.OutputCommentWords = { "output:", "unordered output:" };
		}
		break;
	}
	Reading.HasHaltedData = Lang == Lang::PHP;
	Reading.HaltCallName = Lang == Lang::PHP ? "__halt_compiler" : "";
	Reading.TrailingDataMarker = Lang == Lang::Ruby ? "__END__" : "";
	Rules.StandaloneDataMarker = Reading.TrailingDataMarker;
	Reading.ShouldReadLoneCrAsNewline = Lang != Lang::Go && Lang != Lang::Rust && Lang != Lang::Ruby;
	if(Lang == Lang::CSharp) Reading.UnicodeLineSeparators = { "\xC2\x85", "\xE2\x80\xA8", "\xE2\x80\xA9" };
	if(Lang.IsJsTs()) Reading.UnicodeLineSeparators = { "\xE2\x80\xA8", "\xE2\x80\xA9" };
	Reading.HasOuterText = Lang == Lang::PHP;
	Reading.HasEmbeddedScripts = Lang == Lang::HTML;
	// tree-sitter-html 0.23.2 の要素名のシリアライズの上限
	Reading.HasLimitedElementNameSerialization = Lang == Lang::HTML;
	Reading.CanMisreadCssLineComments = Lang == Lang::CSS && !Lang.IsScss;
	if(Lang == Lang::CSS) Reading.OrdinalSelectorNames = { "nth-child", "nth-last-child", "nth-last-of-type", "nth-of-type" };
	if(Lang == Lang::CSS && Lang.IsScss) {
		Reading.ValueInterpolationOpen = "#{";
		Reading.ValueInterpolationClose = "}";
		Reading.ValueInterpolationJoinChars = "%+-.";
		Reading.ColonPairStandIn = "==";
		Reading.ColonPairValueHeads = {
			"@debug",
			"@each",
			"@else",
			"@error",
			"@for",
			"@forward",
			"@function",
			"@if",
			"@include",
			"@mixin",
			"@return",
			"@use",
			"@warn",
			"@while"
		};
	}
	Reading.HasJsxText = Lang == Lang::JavaScript || Lang.IsJsx;
	Reading.ShouldRequireFunctionDeclarator = Lang.IsCFamily();
	Reading.HasAmbiguousHeaderDialect = Lang == Lang::C && Lang.IsAmbiguous;
	Reading.CanContainMarkupDoc = Lang == Lang::TypeScript;
	Reading.SrcGoal = Lang.Goal;
	Reading.ShouldNormalizeLineCommentBreaks = Lang == Lang::CSS && Lang.IsScss;
	Reading.ShouldMaskEscapedBackslashes = Lang == Lang::CSS;
	Reading.CanSelectorCommentActAsSpace = Lang == Lang::CSS && Lang.IsScss;
	Reading.CanMisreadTemplateArgs = Lang.IsCFamily() || Lang == Lang::CSharp || Lang == Lang::TypeScript;
	Reading.CanMisreadGenericCalls = Lang == Lang::TypeScript;
	Reading.CanMisreadTemplateDecls = Lang == Lang::Cpp;
	Reading.CanMisreadCommandArgBoundary = Lang == Lang::Ruby;
	Reading.HasKeywordClosedBlocks = Lang == Lang::Ruby;
	Reading.CanMisreadCasts = Lang == Lang::CSharp;
	Reading.HasIndentBlocks = Lang == Lang::Python;
	if(Reading.HasIndentBlocks) {
		Reading.SourceIndentTabWidth = 8;
		Reading.IndentResetChars = "\f";
	}
	Reading.HasLineSensitiveBoundary = Lang.IsJsTs() || Lang == Lang::Go || Lang == Lang::Swift;
	Reading.CanCommentLineSeparatorsEndStmts = Lang.IsJsTs();
	const std::span<const std::string_view> Prefixes = GetStrPrefixes(Lang);
	Reading.StrPrefixes.assign(Prefixes.begin(), Prefixes.end());
	Reading.ShouldKeepInterpolationHead = Lang == Lang::PHP;
	Reading.ShouldKeepInterpolationQuotes = Lang == Lang::Python;
	if(Lang == Lang::Python) Reading.LineBoundInterpolationQuotes = "\"'";
	Reading.MeasuredStrPrefixes = Reading.StrPrefixes;
	if(Lang == Lang::C) {
		std::erase_if(
			Reading.MeasuredStrPrefixes,
			[](const std::string_view Prefix) -> bool {
				// 生文字列を持たない文法の引用に含めない接頭辞かの返戻
				return !Prefix.empty() && Prefix.back() == 'R';
			}
		);
	}
	if(Lang == Lang::Rust) Reading.NameQuoteHeads = "'";
	switch(Lang.Id) {
	case Lang::Swift:
		Reading.HashQuotePrefixes = { "" };
		Reading.LineBoundInterpolationQuotes = "\"";
		break;
	case Lang::Rust:
		Reading.HashQuotePrefixes = { "br", "cr", "r" };
		break;
	case Lang::CSharp:
		Reading.VerbatimStrPrefixes = { "$@", "@", "@$" };
		Reading.LineBoundInterpolationQuotes = "\"";
		break;
	}
	if(Lang.IsJsTs()) {
		Reading.CommentLineBreaks = { "\n", "\r" };
		Reading.CommentLineBreaks.insert(
			Reading.CommentLineBreaks.end(),
			Reading.UnicodeLineSeparators.begin(),
			Reading.UnicodeLineSeparators.end()
		);
	}
	if(Lang == Lang::Swift) Reading.CommentLineBreaks = { "\n", "\r" };
	Reading.HasRestrictedLineWords = Lang.IsJsTs();
	Reading.RestrictedLineArrow = Lang.IsJsTs() ? "=>" : "";
	Reading.LeadingBinaryOperatorChars = Lang == Lang::Swift ? "/=-+!*%<>&|^~?" : "";
	Reading.ExprContinuationChars = Lang.IsJsTs() || Lang == Lang::Go || Lang == Lang::Swift ? "([{,;:=?+-*%&|^~<>." : "";
	Reading.HasLeadingBinaryOperators = Lang == Lang::Swift;
	Reading.HasShortVarDecls = Lang == Lang::Go;
	Reading.HasCompositeHeaderExpr = Lang == Lang::Go;
	Reading.HasStructHeaderExpr = Lang == Lang::Rust;
	Reading.HasPrefixCallHeaderExpr = Lang == Lang::Swift;
	Reading.HasContextualTypeRepresentation = Lang == Lang::Swift;
	Reading.HasPreprocMacros = Lang.IsCFamily();
	if(Reading.HasPreprocMacros) Reading.NullDirectivePlaceholder = "#pragma";
	Reading.PredefinedMacroNames = { "__DATE__", "__FILE__", "__LINE__", "__STDC_HOSTED__", "__TIME__" };
	if(Lang == Lang::Cpp) Reading.PredefinedMacroNames.insert({ "__STDCPP_DEFAULT_NEW_ALIGNMENT__", "__cplusplus" });
	else Reading.PredefinedMacroNames.insert({ "__STDC_VERSION__", "__STDC__" });
	Reading.ConditionalMacroNames = {
		"__STDCPP_BFLOAT16_T__",
		"__STDCPP_FLOAT128_T__",
		"__STDCPP_FLOAT16_T__",
		"__STDCPP_FLOAT32_T__",
		"__STDCPP_FLOAT64_T__",
		"__STDCPP_STRICT_POINTER_SAFETY__",
		"__STDCPP_THREADS__",
		"__STDC_ANALYZABLE__",
		"__STDC_IEC_559_COMPLEX__",
		"__STDC_IEC_559__",
		"__STDC_IEC_60559_BFP__",
		"__STDC_IEC_60559_COMPLEX__",
		"__STDC_IEC_60559_DFP__",
		"__STDC_IEC_60559_TYPES__",
		"__STDC_ISO_10646__",
		"__STDC_LIB_EXT1__",
		"__STDC_MB_MIGHT_NEQ_WC__",
		"__STDC_NO_ATOMICS__",
		"__STDC_NO_COMPLEX__",
		"__STDC_NO_THREADS__",
		"__STDC_NO_VLA__",
		"__STDC_UTF_16__",
		"__STDC_UTF_32__",
		"__STDC_VERSION__",
		"__STDC__"
	};
	if(Lang == Lang::Cpp) {
		Reading.ConditionalMacroNames.insert(
			{
				"__cpp_aggregate_bases",
				"__cpp_aggregate_nsdmi",
				"__cpp_aggregate_paren_init",
				"__cpp_alias_templates",
				"__cpp_aligned_new",
				"__cpp_attributes",
				"__cpp_auto_cast",
				"__cpp_binary_literals",
				"__cpp_capture_star_this",
				"__cpp_char8_t",
				"__cpp_concepts",
				"__cpp_conditional_explicit",
				"__cpp_consteval",
				"__cpp_constexpr",
				"__cpp_constexpr_dynamic_alloc",
				"__cpp_constexpr_in_decltype",
				"__cpp_constinit",
				"__cpp_decltype",
				"__cpp_decltype_auto",
				"__cpp_deduction_guides",
				"__cpp_delegating_constructors",
				"__cpp_designated_initializers",
				"__cpp_enumerator_attributes",
				"__cpp_exceptions",
				"__cpp_explicit_this_parameter",
				"__cpp_fold_expressions",
				"__cpp_generic_lambdas",
				"__cpp_guaranteed_copy_elision",
				"__cpp_hex_float",
				"__cpp_if_consteval",
				"__cpp_if_constexpr",
				"__cpp_impl_coroutine",
				"__cpp_impl_destroying_delete",
				"__cpp_impl_three_way_comparison",
				"__cpp_implicit_move",
				"__cpp_inheriting_constructors",
				"__cpp_init_captures",
				"__cpp_initializer_lists",
				"__cpp_inline_variables",
				"__cpp_lambdas",
				"__cpp_modules",
				"__cpp_multidimensional_subscript",
				"__cpp_named_character_escapes",
				"__cpp_namespace_attributes",
				"__cpp_noexcept_function_type",
				"__cpp_nontype_template_args",
				"__cpp_nontype_template_parameter_auto",
				"__cpp_nsdmi",
				"__cpp_range_based_for",
				"__cpp_raw_strings",
				"__cpp_ref_qualifiers",
				"__cpp_return_type_deduction",
				"__cpp_rtti",
				"__cpp_rvalue_references",
				"__cpp_size_t_suffix",
				"__cpp_sized_deallocation",
				"__cpp_static_assert",
				"__cpp_static_call_operator",
				"__cpp_structured_bindings",
				"__cpp_template_template_args",
				"__cpp_threadsafe_static_init",
				"__cpp_unicode_characters",
				"__cpp_unicode_literals",
				"__cpp_user_defined_literals",
				"__cpp_using_enum",
				"__cpp_variable_templates",
				"__cpp_variadic_templates",
				"__cpp_variadic_using"
			}
		);
	}
	Reading.BoolNames =
	Reading.HasPreprocMacros ? BoolNameSource::Macro : Lang == Lang::Go ? BoolNameSource::Shadow : BoolNameSource::Literal;
	if(Lang == Lang::C) {
		// 最上位の宣言から確かめる従来の判定の優先順
		Reading.ParenthesizedDeclOwners = { Core::Role::TranslationUnit, Core::Role::CompoundBody, Core::Role::PreprocBranch };
	}
	Reading.HasCommentAbsorbingDecls = Lang == Lang::Go;
	Reading.HasUnsupportedMarkupComments = Lang.IsJsTs();
	Reading.CanScriptAcceptMarkupComments = Lang == Lang::JavaScript;
	Reading.MarkupCommentOpen = Lang.IsJsTs() ? "<!--" : "";
	Reading.MarkupCommentClose = Lang.IsJsTs() ? "-->" : "";
	Reading.HasArrowChainMisreads = Lang.IsJsTs();
	Rules.Notation.LineCommentTerminator = Lang == Lang::PHP ? "?>" : "";
	// 行コメントを行の途中で終える改行のテキスト（改行と読む Unicode の区切と Sass の改ページと，JSON の利用先が読む行区切と，保護範囲に残る単独の CR），行末で次の行を繋ぐテキスト（バックスラッシュと其の三連字），トークン化前の Unicode のエスケープ
	Rules.Notation.LineCommentBreaks = Lang == Lang::CSS ?
	std::vector<std::string_view>{ "\f" } :
	Lang == Lang::JSON ? std::vector<std::string_view>{ "\xE2\x80\xA8", "\xE2\x80\xA9" } : Reading.UnicodeLineSeparators;
	if(Reading.ShouldReadLoneCrAsNewline) Rules.Notation.LineCommentBreaks.push_back("\r");
	if(Lang.IsCFamily()) Rules.Notation.LineSplices = { "?\?/", "\\" };
	Rules.Notation.ShouldDecodeUnicodeEscapes = Lang == Lang::Java;
	if(Lang == Lang::PHP) Rules.CodeOpeningSuffixes = { "<?=", "<?php" };
	if(Lang == Lang::TypeScript) {
		Reading.RefDirectiveElements = { "<amd-dependency", "<amd-module", "<reference" };
		Reading.RefDirectiveTerminators = " 	/";
	}
	Rules.Notation.HasBracketedDocType = Lang == Lang::Ruby;
	Rules.Notation.HasBareDocType = Lang == Lang::PHP;
	if(Lang == Lang::PHP) {
		Rules.Notation.DocParamSigil = "$";
		Rules.Notation.DocParamMarks = { "&", "..." };
	}
	Rules.Notation.ShouldExcludeDirectiveLines = Lang == Lang::Go;
	// コメントの中のコード例の書式（godoc はインデントでコード例を示し囲み・要素を持たない，本文は行頭にインデントを持たない為に引用・箇条のマーカーの後のインデントだけが残る，C# の XML ドキュメントは `<c>` と `<code>` の要素）
	Rules.Notation.CodeExamples = {
		Lang == Lang::Go,
		Lang != Lang::Go,
		Lang == Lang::Java ? std::vector<std::string_view>{ "{@code", "{@literal" } : std::vector<std::string_view>{},
		"@\\",
		Lang.IsCFamily() ? std::vector<std::string_view>{ "code", "dot", "msc", "verbatim" } : std::vector<std::string_view>{},
		Lang.IsCFamily() ? "{@" : "",
		"end",
		'@',
		Lang == Lang::Go ?
		std::vector<std::string_view>{} :
		Lang == Lang::CSharp ? std::vector<std::string_view>{ "c", "code" } : std::vector<std::string_view>{ "code", "pre" },
		Lang == Lang::Go ?
		std::vector<std::pair<std::string_view, std::string_view>>{} :
		std::vector<std::pair<std::string_view, std::string_view>>{ { "<!--", "-->" }, { "<![CDATA[", "]]>" } },
		Lang.IsCFamily() ?
		std::vector<std::pair<std::string_view, std::string_view>>{
			{ "@f$", "@f$" },
			{ "@f[", "@f]" },
			{ "@f{", "@f}" },
			{ "\\f$", "\\f$" },
			{ "\\f[", "\\f]" },
			{ "\\f{", "\\f}" }
		} :
		std::vector<std::pair<std::string_view, std::string_view>>{}
	};
	Rules.Notation.HasOptionalFailureType = Lang.IsJsTs();
	Rules.Notation.MemberDocHead = Lang.IsCFamily() ? "<" : "";
	// ドキュメントに使う記法と其の入力のドキュメント形式（同じ形式の行と囲みのドキュメントだけを揃え，Java の Markdown と従来形式は揃えない，整形仕様 7.6）
	// 囲みのドキュメントが無い時は先頭の記法を出力に採る為，記法の優先順を保つ
	Rules.Notation.DocumentForms = GetDocFormsOf(Lang);
	// 通常の独立・行末コメント，ドキュメントの用途毎の採用記法（整形仕様 7.6，HTML のコメントはドキュメントの内容）
	if(Lang != Lang::HTML) {
		using Use = Core::CommentUse;
		const CommentNotation Plain = Lang == Lang::Ruby || Lang == Lang::Python ?
		CommentNotation::HashLine :
		Lang == Lang::CSS && !Lang.IsScss ? CommentNotation::Block : CommentNotation::SlashLine;
		Rules.Notation.OutputForms.Forms.push_back({ Use::Plain, Plain });
		// ドキュメントは囲みのドキュメントの形式を持つ言語で其の囲み，其の他はドキュメントに使う行の記法
		if(Rules.Notation.GetDocFormOf(CommentNotation::DoubleStarBlock)) {
			Rules.Notation.OutputForms.Forms.push_back({ Use::Doc, CommentNotation::DoubleStarBlock });
		} else if(!Rules.Notation.DocumentForms.empty()) {
			Rules.Notation.OutputForms.Forms.push_back({ Use::Doc, Rules.Notation.DocumentForms.front().Notation });
		}
	}
	// 本文を値として持つ明示的なドキュメント属性
	if(Lang == Lang::Rust) {
		Rules.Notation.OutputForms.Forms.push_back({ Core::CommentUse::Doc, CommentNotation::StarBangBlock, true });
	}
	Rules.Notation.DocAttributeOpen = Lang == Lang::Rust ? "#[" : "";
	// 脱出と返戻の読取規則
	Rules.HasLabelJump = Lang.IsCFamily() || Lang == Lang::CSharp || Lang == Lang::Go || Lang == Lang::PHP;
	Rules.IsLabelJumpScoped = Lang == Lang::CSharp;
	Rules.HasNumericExitDepth = Lang == Lang::PHP;
	Rules.DoesContinueCountSwitch = Lang == Lang::PHP;
	Rules.IsExitLabelBodyChild = Lang == Lang::Rust;
	Rules.IsExitLabelPrevSibling = Lang == Lang::Kotlin || Lang == Lang::Swift;
	Rules.CanCaseNestInStmt = Lang.IsCFamily();
	Rules.DoesCaseOpenWithStmtEnd = Lang == Lang::PHP;
	Rules.CanCaseFallThrough = Lang.IsCFamily() || Lang.IsJsTs() || Lang == Lang::PHP;
	Rules.CanColonCaseFallThrough = Lang == Lang::Java;
	Rules.CanEmptyCaseFallThrough = Lang == Lang::CSharp;
	Rules.OmittedResult = Lang == Lang::C ?
	Core::OmittedResultNotation::Int :
	Lang == Lang::Cpp || Lang == Lang::Go || Lang == Lang::Rust || Lang == Lang::Kotlin || Lang == Lang::Swift ?
	Core::OmittedResultNotation::Unit :
	Core::OmittedResultNotation::Inferred;
	Rules.DoesDeferredCallReturnVal = Lang.IsJsTs() || Lang == Lang::Python || Lang == Lang::PHP || Lang == Lang::Rust;
	Rules.IsTailExprVal = Lang == Lang::Rust;
	Rules.HasStaticCatchReachability = Lang == Lang::CSharp || Lang == Lang::Java;
	Rules.DoesReachabilityUseConstantCond = Lang == Lang::CSharp || Lang == Lang::Java;
	// 数値とリテラルの読取規則
	Rules.BasePrefixLetters = Lang == Lang::Rust || Lang == Lang::Swift ? "xbo" : "";
	Rules.DigitSeparator = Lang.IsCFamily() ? '\'' : Lang == Lang::JSON || Lang == Lang::HTML || Lang == Lang::CSS ? '\0' : '_';
	if(Lang.IsCFamily()) {
		Rules.IntegerLiteralSuffixes.assign(std::begin(LintRules::MacroIntegerSuffixes), std::end(LintRules::MacroIntegerSuffixes));
		Rules.FloatingLiteralSuffixes = LintRules::MacroFloatSuffixes;
		if(Lang == Lang::C) {
			Rules.IntegerLiteralSuffixes.insert(
				Rules.IntegerLiteralSuffixes.end(),
				{ "UWB", "Uwb", "WB", "WBU", "WBu", "uWB", "uwb", "wb", "wbU", "wbu" }
			);
			std::ranges::sort(Rules.IntegerLiteralSuffixes);
		}
	} else if(Lang == Lang::CSharp) {
		Rules.IntegerLiteralSuffixes = { "L", "LU", "Lu", "U", "UL", "Ul", "l", "lU", "lu", "u", "uL", "ul" };
		Rules.FloatingLiteralSuffixes = "DFMdfm";
	} else if(Lang == Lang::Java) {
		Rules.IntegerLiteralSuffixes = { "L", "l" };
		Rules.FloatingLiteralSuffixes = "DFdf";
	} else if(Lang == Lang::Kotlin) {
		Rules.IntegerLiteralSuffixes = { "L", "U", "UL", "u", "uL" };
		Rules.FloatingLiteralSuffixes = "Ff";
	} else if(Lang.IsJsTs()) Rules.IntegerLiteralSuffixes = { "n" };
	if(Lang == Lang::Python) Rules.ImaginaryLiteralSuffixes = "Jj";
	// 各フィールドは１６進・２進・８進の順に添字で参照する
	if(Lang == Lang::PHP) Rules.ExitDepthBasePrefixes = { "xX", "bB", "oO" };
	if(Lang == Lang::Rust) {
		// 最初に一致した接尾辞を採る為，長い物を先に置く
		Rules.NumericTypeSuffixes =
		{ "usize", "isize", "u128", "i128", "f32", "f64", "u64", "i64", "u32", "i32", "u16", "i16", "u8", "i8" };
	}
	Rules.QuotePrefixes = Lang == Lang::Python ? "bfru" : Lang == Lang::PHP ? "b" : "";
	Rules.RawQuotePrefixes = Lang == Lang::Python ? "r" : "";
	Rules.FormatQuotePrefixes = Lang == Lang::Python ? "f" : "";
	Rules.NonDocQuotePrefixes = Lang == Lang::Python ? "bf" : "";
	Rules.IsSingleQuoteBackslashLiteral = Lang == Lang::PHP || Lang == Lang::Ruby;
	Rules.DoesInterpolateDollar = Lang == Lang::PHP;
	Rules.HashInterpolationHeads = Lang == Lang::Ruby ? "{@$" : "";
	if(Lang == Lang::Ruby) Rules.RegexNamedCaptures = { { "(?'", "'" }, { "(?<", ">" } };
	Rules.HasTripleQuote = Lang == Lang::Python;
	Rules.IsEveryIntCondTrue = Lang == Lang::Ruby;
	Rules.DoesBoolNameNeedDecl = Lang == Lang::C || Lang.IsCShared;
	Rules.ShouldPadDecPoint = Lang.IsCFamily() || Lang == Lang::CSharp || Lang == Lang::Java;
	Rules.HasSpacedDimensionSigns = Lang.IsScss;
	Rules.HasTrailingCommaEmptyVal = Lang == Lang::CSS;
	Rules.DoesSingletonCommaDetermineTuple = Lang == Lang::Python;
	Rules.CanLineContinueOnlyInBrackets = Lang == Lang::Python;
	Rules.ShouldProtectBrokenDeclBoundaries = Lang.IsJsTs();
	Rules.HasAngleListSyntax =
	Lang == Lang::Cpp || Lang == Lang::CSharp || Lang == Lang::Java || Lang == Lang::Kotlin || Lang == Lang::Rust ||
	Lang == Lang::Swift || Lang == Lang::TypeScript;
	Rules.CanCallFormsDeclare = Lang.IsCFamily();
	Reading.EmptyStmtDef.Def = Find("empty_statement", true);
	// 独立したセミコロンはトークンの性質を保ち，空文の役割だけを加える
	if(const Core::KindDef *const Terminator = Find(";", false)) {
		EmptyStmtKind = *Terminator;
		EmptyStmtKind.Roles.set(static_cast<size_t>(Core::Role::EmptyStmt));
		EmptyStmtKind.InitRoleList();
	}
	Rules.FalseDef.Def = Find("false", true);
	Rules.ExprStmtDef.Def = Find("expression_statement", true);
	Rules.ReturnExprDef.Def = Find("return_expression", true);
	Rules.ParenthesizedStmtsDef.Def = Find("parenthesized_statements", true);
	Rules.TrueDef.Def = Find(Lang == Lang::PHP ? "boolean" : "true", true);
	Rules.LoopExprDef.Def = Find("loop_expression", true);
	Rules.ImplicitParamDef.Def = Find("implicit_parameter", true);
	Rules.DoubleQuotedDef.Def = Lang == Lang::PHP ? Find("encapsed_string", true) : nullptr;
	Rules.PrefixUnaryDef.Def = Find("prefix_unary_expression", true);
	Rules.PrefixDef.Def = Find("prefix_expression", true);
	// 処理系が定める名前付のディレクティブ記法を使用可能な記法へ登録（名前の後の区切にタブも読むか，記号は記法の定義が持つ）
	if(Lang == Lang::Go) {
		Rules.Notation.AvailableNotations.insert(
			Rules.Notation.AvailableNotations.end(),
			{
				{ CommentNotation::CgoExportLine, false },
				{ CommentNotation::GccgoExternLine, false },
				{ CommentNotation::GoBuildLine, true },
				{ CommentNotation::GoDebugLine, true },
				{ CommentNotation::GoEmbedLine, true },
				{ CommentNotation::GoGenerateLine, true },
				{ CommentNotation::GoLinknameLine, false },
				{ CommentNotation::GoNoescapeLine, false },
				{ CommentNotation::GoNoinlineLine, false },
				{ CommentNotation::GoNoraceLine, false },
				{ CommentNotation::GoNosplitLine, false },
				{ CommentNotation::GoUintptrescapesLine, false },
				{ CommentNotation::GoWasmexportLine, false },
				{ CommentNotation::GoWasmimportLine, false },
				{ CommentNotation::GoLineLine, false },
				{ CommentNotation::GoLineBlock, false }
			}
		);
	} else if(Lang.IsCFamily()) Rules.Notation.AvailableNotations.push_back({ CommentNotation::GoBuildLine, true });
	const std::string_view ParamTag = Core::VocabularyOf(Rules.Notation.TagVocabulary, Core::DocDescription::Param).Canonical;
	const std::string_view ReturnTag = Core::VocabularyOf(Rules.Notation.TagVocabulary, Core::DocDescription::Return).Canonical;
	if(Lang.IsCFamily()) {
		Rules.Notation.DocTagSynonyms = {
			{ "@exception", Rules.Notation.FailureTag },
			{ "@result", ReturnTag },
			{ "@returns", ReturnTag },
			{ "@short", Rules.Notation.SummaryTag },
			{ "@throw", Rules.Notation.FailureTag },
			{ "\\brief", Rules.Notation.SummaryTag },
			{ "\\exception", Rules.Notation.FailureTag },
			{ "\\param", ParamTag },
			{ "\\result", ReturnTag },
			{ "\\return", ReturnTag },
			{ "\\returns", ReturnTag },
			{ "\\short", Rules.Notation.SummaryTag },
			{ R"(\tparam)", DoxygenTypeParamTag },
			{ R"(\throw)", Rules.Notation.FailureTag },
			{ R"(\throws)", Rules.Notation.FailureTag }
		};
	}
	if(Lang.IsJsTs()) {
		Rules.Notation.DocTagSynonyms = {
			{ "@arg", ParamTag },
			{ "@argument", ParamTag },
			{ "@desc", JsDescriptionTag },
			{ "@exception", Rules.Notation.FailureTag },
			{ "@returns", ReturnTag }
		};
	}
	if(Lang == Lang::Java || Lang == Lang::Kotlin) Rules.Notation.DocTagSynonyms = { { "@exception", Rules.Notation.FailureTag } };
	// 共通工程が照合・生成する語の綴り（整形仕様 3.1，3.5）
	const auto SetWord = [this](const Core::Word Meaning, std::vector<std::string_view> &&Spellings) -> void {
		Rules.Words[static_cast<size_t>(Meaning)] = std::move(Spellings);
	};
	const bool IsC = Lang.IsCFamily(), IsCpp = Lang == Lang::Cpp, IsCSharp = Lang == Lang::CSharp, IsJava = Lang == Lang::Java;
	const bool IsGo = Lang == Lang::Go, IsRust = Lang == Lang::Rust, IsKotlin = Lang == Lang::Kotlin, IsSwift = Lang == Lang::Swift;
	const bool IsPhp = Lang == Lang::PHP, IsJs = Lang.IsJsTs(), IsTs = Lang == Lang::TypeScript, IsRuby = Lang == Lang::Ruby;
	const bool IsPython = Lang == Lang::Python;
	const bool IsCode = IsC || IsCSharp || IsJava || IsGo || IsRust || IsKotlin || IsSwift || IsPhp || IsJs || IsRuby || IsPython;
	if(IsC) {
		SetWord(Core::Word::MacroConcatenation, { "##" });
		SetWord(Core::Word::MacroStringizing, { "#" });
		SetWord(Core::Word::PreprocAlternateBranch, { "#else" });
		SetWord(Core::Word::PreprocDefined, { "defined" });
		SetWord(Core::Word::PreprocDefinedBranch, { "#elifdef", "#ifdef" });
		SetWord(Core::Word::PreprocUndefinedBranch, { "#elifndef", "#ifndef" });
	}
	if(IsCpp) {
		SetWord(Core::Word::EnumImport, { "enum" });
		SetWord(Core::Word::InlineNamespace, { "inline" });
		SetWord(Core::Word::ScopedEnumModifier, { "class", "struct" });
	}
	if(IsJs) {
		SetWord(Core::Word::AwaitNameEscape, { "\\u0061wait" });
		SetWord(Core::Word::YieldName, { "yield" });
		SetWord(Core::Word::YieldNameEscape, { "\\u0079ield" });
	}
	if(IsCpp) SetWord(Core::Word::AbsoluteNamePrefix, { "::" });
	if(IsCSharp) SetWord(Core::Word::AbsoluteNamePrefix, { "global::" });
	if(IsCSharp) {
		SetWord(
			Core::Word::QualifiedResultType,
			{
				"System.Threading.Tasks.Task",
				"System.Threading.Tasks.ValueTask",
				"global::System.Threading.Tasks.Task",
				"global::System.Threading.Tasks.ValueTask"
			}
		);
	}
	if(IsKotlin) SetWord(Core::Word::QualifiedResultType, { "kotlin.Nothing", "kotlin.Unit" });
	if(IsSwift) {
		SetWord(Core::Word::QualifiedResultType, { "Swift.Never", "Swift.Void" });
		SetWord(Core::Word::ForcedTry, { "try!" });
		SetWord(Core::Word::OptionalTry, { "try?" });
	}
	if(IsPython) SetWord(Core::Word::QualifiedResultType, { "typing.Never", "typing.NoReturn" });
	if(IsCSharp || IsRust || IsSwift || IsJs || IsPython) SetWord(Core::Word::AsyncModifier, { "async" });
	if(Lang.IsCFamily()) SetWord(Core::Word::AlternateBranch, { "else" });
	if(IsCSharp) SetWord(Core::Word::AsyncVoidType, { "Task", "ValueTask" });
	if(IsTs) SetWord(Core::Word::AsyncVoidType, { "Promise<void>" });
	if(IsJs) SetWord(Core::Word::AwaitName, { "await" });
	if(IsJs) SetWord(Core::Word::ExportName, { "export" });
	if(IsKotlin) SetWord(Core::Word::ObjectHead, { "object" });
	if(IsKotlin) SetWord(Core::Word::LabeledReturn, { "return@" });
	if(IsPhp) SetWord(Core::Word::BareCall, { "die" });
	if(IsRuby) SetWord(Core::Word::BareCapture, { "&", "*", "**" });
	if(IsJava || IsKotlin || IsSwift || IsJs || IsRuby) SetWord(Core::Word::BaseRef, { "super" });
	if(IsRuby) SetWord(Core::Word::BlockEnd, { "end" });
	if(IsC) SetWord(Core::Word::Boolean, { "_Bool", "bool" });
	if(IsC) SetWord(Core::Word::NonMacroName, { "abort", "exit" });
	if(IsC) SetWord(Core::Word::StringizingAssertion, { "assert" });
	if(IsPhp) SetWord(Core::Word::ValOutputTag, { "<?=" });
	if(IsPython) SetWord(Core::Word::LineContinuation, { "\\" });
	// 戻らない呼出は修飾の有無を問わず処理系が同じ関数と読む綴りの整列
	if(IsC) SetWord(Core::Word::TerminatingCall, { "abort", "exit" });
	if(IsCpp) {
		SetWord(
			Core::Word::TerminatingCall,
			{ "::abort", "::exit", "::std::abort", "::std::exit", "abort", "exit", "std::abort", "std::exit" }
		);
	}
	if(IsC) {
		Rules.StandardImportPrefixes = { { "<stdlib.h>", "" } };
		if(IsCpp) Rules.StandardImportPrefixes.emplace("<cstdlib>", "std::");
		Rules.Words[static_cast<size_t>(Core::Word::ImportedTarget)] = Rules.Words[static_cast<size_t>(Core::Word::TerminatingCall)];
		Rules.Words[static_cast<size_t>(Core::Word::StandardTerminalDecl)] =
		Rules.Words[static_cast<size_t>(Core::Word::TerminatingCall)];
		SetWord(Core::Word::StandardTerminalNoArgs, { "::abort", "::std::abort", "abort", "std::abort" });
	}
	if(IsCSharp) {
		SetWord(Core::Word::ImportedTarget, { "Environment.Exit", "Environment.FailFast" });
		SetWord(
			Core::Word::TerminatingCall,
			{ "Environment.Exit", "Environment.FailFast", "System.Environment.Exit", "System.Environment.FailFast" }
		);
	}
	if(IsGo) SetWord(Core::Word::TerminatingCall, { "panic" });
	if(IsPhp) SetWord(Core::Word::TerminatingCall, { "\\die", "\\exit", "die", "exit" });
	if(IsPython) {
		SetWord(Core::Word::TerminatingCall, { "sys.exit" });
		SetWord(Core::Word::ImportedTarget, { "sys.exit" });
	}
	if(IsJava) SetWord(Core::Word::TerminatingCall, { "System.exit", "java.lang.System.exit" });
	if(IsRust) {
		SetWord(
			Core::Word::EagerMacroToken,
			{
				"!=",
				"%",
				"%=",
				"&",
				"&=",
				"(",
				")",
				"*",
				"*=",
				"+",
				"+=",
				",",
				"-",
				"-=",
				".",
				"..",
				"..=",
				"/",
				"/=",
				":",
				"::",
				";",
				"<",
				"<<",
				"<<=",
				"<=",
				"=",
				"==",
				">",
				">=",
				">>",
				">>=",
				"[",
				"]",
				"^",
				"^=",
				"{",
				"}"
			}
		);
		SetWord(Core::Word::TerminatingMacro, { "core::panic", "panic", "std::panic" });
		SetWord(
			Core::Word::OrdinaryMacro,
			{ "eprint", "eprintln", "print", "println", "std::eprint", "std::eprintln", "std::print", "std::println" }
		);
		SetWord(
			Core::Word::OrdinaryAttribute,
			{ "allow", "cold", "deny", "deprecated", "doc", "forbid", "inline", "must_use", "warn" }
		);
	}
	if(IsPhp) SetWord(Core::Word::TerminatingImport, { "\\die", "\\exit", "die", "exit" });
	if(IsPython) SetWord(Core::Word::TerminatingImport, { "sys.exit" });
	if(IsJava) SetWord(Core::Word::TerminatingImport, { "java.lang.System" });
	if(IsRust) SetWord(Core::Word::TerminatingImport, { "std::panic" });
	if(IsCSharp) SetWord(Core::Word::TerminatingImport, { "System.Environment" });
	if(IsKotlin) SetWord(Core::Word::TerminatingImport, { "kotlin.TODO", "kotlin.error" });
	if(IsCpp) for(const std::string_view Name : Rules.Words[static_cast<size_t>(Core::Word::TerminatingCall)]) {
		if(Name.starts_with("std::") || Name.starts_with("::std::")) {
			Rules.Words[static_cast<size_t>(Core::Word::TerminatingImport)].push_back(Name);
		}
	}
	if(IsSwift) SetWord(Core::Word::TerminatingImport, { "Swift.fatalError", "Swift.preconditionFailure" });
	if(IsSwift) {
		SetWord(Core::Word::TerminatingCall, { "Swift.fatalError", "Swift.preconditionFailure", "fatalError", "preconditionFailure" });
	}
	if(IsKotlin) SetWord(Core::Word::TerminatingCall, { "TODO", "error", "kotlin.TODO", "kotlin.error" });
	// 終端の標準登録と同じ解決先へ，捕捉・後始末に関する効果を対応付ける
	if(IsC || IsCSharp || IsJava || IsSwift || IsPhp) {
		Rules.Words[static_cast<size_t>(Core::Word::ProcessExitCall)] = Rules.Words[static_cast<size_t>(Core::Word::TerminatingCall)];
	}
	if(IsCpp) SetWord(Core::Word::CatchAllException, { "..." });
	if(IsJs) SetWord(Core::Word::CatchAllException, { "catch" });
	if(IsPython) {
		SetWord(Core::Word::SystemExitCall, { "sys.exit" });
		SetWord(Core::Word::SystemExitHandlerType, { "BaseException", "SystemExit" });
		SetWord(
			Core::Word::SystemExitExcludedType,
			{
				"ArithmeticError",
				"AssertionError",
				"AttributeError",
				"BaseExceptionGroup",
				"BlockingIOError",
				"BrokenPipeError",
				"BufferError",
				"ChildProcessError",
				"ConnectionAbortedError",
				"ConnectionError",
				"ConnectionRefusedError",
				"ConnectionResetError",
				"EOFError",
				"Exception",
				"ExceptionGroup",
				"FileExistsError",
				"FileNotFoundError",
				"FloatingPointError",
				"GeneratorExit",
				"ImportError",
				"IndentationError",
				"IndexError",
				"InterruptedError",
				"IsADirectoryError",
				"KeyError",
				"KeyboardInterrupt",
				"LookupError",
				"MemoryError",
				"ModuleNotFoundError",
				"NameError",
				"NotADirectoryError",
				"NotImplementedError",
				"OSError",
				"OverflowError",
				"PermissionError",
				"ProcessLookupError",
				"RecursionError",
				"ReferenceError",
				"RuntimeError",
				"StopAsyncIteration",
				"StopIteration",
				"SyntaxError",
				"SystemError",
				"TabError",
				"TimeoutError",
				"TypeError",
				"UnboundLocalError",
				"UnicodeDecodeError",
				"UnicodeEncodeError",
				"UnicodeError",
				"UnicodeTranslateError",
				"ValueError",
				"ZeroDivisionError"
			}
		);
	}
	// 関数・型の名前の大文字小文字を区別しない文法の語
	if(IsPhp) {
		for(
			const Core::Word Meaning :
			{
				Core::Word::DynamicScope,
				Core::Word::NeverType,
				Core::Word::TerminatingCall,
				Core::Word::TerminatingImport,
				Core::Word::VoidType
			}
		) Rules.CaseInsensitiveWords.set(static_cast<size_t>(Meaning));
	}
	if(IsCode && !IsRust && !IsKotlin) SetWord(Core::Word::Case, { "case" });
	if(IsCpp) SetWord(Core::Word::Cast, { "static_cast", "const_cast", "reinterpret_cast", "dynamic_cast" });
	else if(IsCSharp || IsRust || IsKotlin || IsSwift || IsTs) SetWord(Core::Word::Cast, { "as" });
	if(IsGo) SetWord(Core::Word::Channel, { "chan" });
	if(IsRuby) SetWord(Core::Word::CondThen, { "then" });
	if(IsCpp) SetWord(Core::Word::ConstantCond, { "constexpr" });
	if(IsJava) SetWord(Core::Word::ConstantDecl, { "final" });
	else if(IsC || IsCSharp || IsGo || IsRust || IsKotlin || IsPhp || IsJs) SetWord(Core::Word::ConstantDecl, { "const" });
	if(IsC) SetWord(Core::Word::ConstQualifier, { "const" });
	if(IsCpp || IsCSharp || IsJava || IsPhp || IsJs) SetWord(Core::Word::Construct, { "new" });
	if(IsC || IsCSharp || IsJava || IsGo || IsSwift || IsPhp || IsJs) SetWord(Core::Word::Default, { "default" });
	if(IsRuby || IsPython) SetWord(Core::Word::Define, { "def" });
	if(IsKotlin) SetWord(Core::Word::DeferredInit, { "lateinit" });
	if(IsPython) SetWord(Core::Word::Delegation, { "from" });
	if(IsGo || IsKotlin) SetWord(Core::Word::DiscardName, { "_" });
	if(IsPhp) SetWord(Core::Word::DynamicScope, { "eval", "extract" });
	if(IsJs) SetWord(Core::Word::DynamicScope, { "eval" });
	if(IsRuby) SetWord(Core::Word::DynamicScope, { "binding", "eval" });
	if(IsC) SetWord(Core::Word::EntryFunction, { "main" });
	if(IsC) SetWord(Core::Word::EntryResultType, { "int", "signed" });
	if(IsKotlin || IsRuby) SetWord(Core::Word::ExprBody, { "=" });
	if(IsCSharp || IsPhp || IsJs) SetWord(Core::Word::ExprBody, { "=>" });
	if(IsPhp) {
		SetWord(
			Core::Word::ExternalBinding,
			{ "$GLOBALS", "$_COOKIE", "$_ENV", "$_FILES", "$_GET", "$_POST", "$_REQUEST", "$_SERVER", "$_SESSION", "$this" }
		);
	}
	if(IsGo || IsSwift) SetWord(Core::Word::Fallthrough, { "fallthrough" });
	if(IsCode) SetWord(Core::Word::False, { IsPython ? "False" : "false" });
	if(IsGo) SetWord(Core::Word::FunctionHead, { "func" });
	if(IsJs) SetWord(Core::Word::FunctionScopedBinding, { "var" });
	if(IsCSharp || IsKotlin || IsSwift || IsJs) SetWord(Core::Word::Getter, { "get" });
	if(IsJs) SetWord(Core::Word::ImmutableBinding, { "const" });
	if(IsCode) SetWord(Core::Word::IntTruth, { "1" });
	if(IsCode) SetWord(Core::Word::IntZero, { "0" });
	if(IsJava) SetWord(Core::Word::ImmutableBinding, { "final" });
	if(IsKotlin) SetWord(Core::Word::ImmutableBinding, { "val" });
	if(IsCSharp || IsRust || IsKotlin || IsSwift || IsJs || IsRuby || IsPython) SetWord(Core::Word::In, { "in" });
	if(IsCSharp || IsJava) SetWord(Core::Word::InferredType, { "var" });
	if(IsRuby) SetWord(Core::Word::IterationDo, { "do" });
	if(IsJs) SetWord(Core::Word::IterationOf, { "of" });
	if(IsKotlin) SetWord(Core::Word::LabeledReceiver, { "super@", "this@" });
	if(IsRuby || IsPython) SetWord(Core::Word::LogicalWord, { "and", "or" });
	if(Lang == Lang::HTML) SetWord(Core::Word::MetadataElement, { "head" });
	if(Lang == Lang::HTML) SetWord(Core::Word::PreserveSpaceAttribute, { "xml:space" });
	if(Lang == Lang::HTML) SetWord(Core::Word::InlineStyleAttribute, { "style" });
	if(Lang == Lang::HTML) SetWord(Core::Word::NamespaceAttribute, { "xmlns" });
	if(IsCSharp) SetWord(Core::Word::OutputArg, { "out" });
	if(IsJs) SetWord(Core::Word::ClassHead, { "class" });
	if(IsSwift) SetWord(Core::Word::MetatypeSuffix, { "Type" });
	if(IsPhp) SetWord(Core::Word::LogicalWord, { "and", "or", "xor" });
	if(IsRust) SetWord(Core::Word::Loop, { "loop" });
	if(IsRuby || IsPython) SetWord(Core::Word::Negation, { "not" });
	if(IsKotlin) SetWord(Core::Word::NeverType, { "Nothing" });
	if(IsSwift) SetWord(Core::Word::NeverType, { "Never" });
	if(IsPhp || IsTs) SetWord(Core::Word::NeverType, { "never" });
	if(IsPython) SetWord(Core::Word::NeverType, { "Never", "NoReturn" });
	if(IsRust) SetWord(Core::Word::NeverType, { "!" });
	if(IsC) {
		SetWord(Core::Word::NoReturnAttributeName, { "__noreturn__", "noreturn" });
		SetWord(Core::Word::NoReturnAttributeNamespace, { "__gnu__", "gnu" });
		SetWord(Core::Word::NoReturnSpecifier, { "_Noreturn" });
	}
	if(IsCpp) SetWord(Core::Word::OptionalityMarker, { "std::optional" });
	else if(IsSwift) SetWord(Core::Word::OptionalityMarker, { "?" });
	if(IsJs) SetWord(Core::Word::ReassignableBinding, { "let" });
	if(IsKotlin) SetWord(Core::Word::ReassignableBinding, { "var" });
	if(IsRust) SetWord(Core::Word::RefBinding, { "ref" });
	if(IsRuby) SetWord(Core::Word::RegexDelimiter, { "/" });
	if(IsCode) SetWord(Core::Word::Return, { "return" });
	if(IsCpp || IsCSharp || IsJava || IsKotlin || IsJs) SetWord(Core::Word::Self, { "this" });
	if(IsRust || IsSwift || IsRuby || IsPython) SetWord(Core::Word::Self, { "self" });
	if(IsJs) SetWord(Core::Word::StmtDeclHead, { "class", "function" });
	if(IsCode || Lang == Lang::CSS) SetWord(Core::Word::StmtEnd, { ";" });
	if(Lang == Lang::JSON) SetWord(Core::Word::ListSeparator, { "," });
	if(IsCode) SetWord(Core::Word::True, { IsPython ? "True" : "true" });
	// 生成する真値の綴りは先頭の小文字形に固定する
	if(IsPhp) {
		SetWord(
			Core::Word::True,
			{
				"true",
				"TRUE",
				"TRUe",
				"TRuE",
				"TRue",
				"TrUE",
				"TrUe",
				"TruE",
				"True",
				"tRUE",
				"tRUe",
				"tRuE",
				"tRue",
				"trUE",
				"trUe",
				"truE"
			}
		);
	}
	if(IsGo || IsRust || IsTs || IsPython) SetWord(Core::Word::TypeDecl, { "type" });
	if(IsJs) SetWord(Core::Word::TypeQuery, { "typeof" });
	if(IsJava || IsPhp || IsJs) SetWord(Core::Word::TypeTest, { "instanceof" });
	if(IsC || IsPython) SetWord(Core::Word::VariadicMarker, { "..." });
	if(IsC || IsCSharp || IsJava || IsPhp || IsTs) SetWord(Core::Word::VoidType, { "void" });
	if(IsKotlin) SetWord(Core::Word::VoidType, { "Unit" });
	if(IsSwift) SetWord(Core::Word::VoidType, { "()", "Void" });
	if(IsPython) SetWord(Core::Word::VoidType, { "None" });
	if(IsRust) SetWord(Core::Word::VoidType, { "()" });
	if(IsCode && !IsGo) SetWord(Core::Word::While, { "while" });
	if(IsCpp || IsCSharp || IsJava || IsPhp) {
		SetWord(Core::Word::PrivateAccess, { "private" });
		SetWord(Core::Word::ProtectedAccess, { "protected" });
		SetWord(Core::Word::PublicAccess, { "public" });
	}
	if(IsJs) SetWord(Core::Word::PrivateAccess, { "private" });
	if(IsTs) SetWord(Core::Word::DynamicTopType, { "any" });
	if(IsCSharp) SetWord(Core::Word::CatchFilter, { "when" });
	if(IsC || IsCSharp || IsJava || IsKotlin || IsSwift || IsPhp || IsJs) SetWord(Core::Word::CatchHead, { "catch" });
	if(IsPython) SetWord(Core::Word::CatchHead, { "except" });
	if(IsSwift) SetWord(Core::Word::CatchBinding, { "let", "var" });
	if(IsCpp || IsCSharp || IsJava || IsPhp || IsJs || IsRuby) SetWord(Core::Word::ConstructionMethod, { "new" });
	if(IsCSharp || IsJava || IsGo) SetWord(Core::Word::SignedByteType, { "byte" });
	if(IsC || IsCSharp || IsJava) SetWord(Core::Word::LongIntType, { "long" });
	if(IsJava || IsSwift) SetWord(Core::Word::ThrowsMarker, { "throws" });
	if(IsKotlin) SetWord(Core::Word::ThrowsAnnotation, { "Throws(" });
	if(IsKotlin) SetWord(Core::Word::ThrowHead, { "throw" });
	if(IsRuby) SetWord(Core::Word::ImplicitRaise, { "fail", "raise" });
	if(IsC) SetWord(Core::Word::UsingNamespace, { "namespace" });
	if(IsJs) SetWord(Core::Word::NullishVal, { "null", "undefined" });
	if(IsJs) SetWord(Core::Word::NullLiteral, { "null" });
	if(IsRust) {
		SetWord(Core::Word::ErrorResultType, { "Result" });
		SetWord(Core::Word::PanicMacro, { "assert", "assert_eq", "assert_ne", "panic" });
	}
	if(IsCpp) SetWord(Core::Word::NullMacro, { "NULL" });
	if(IsC) SetWord(Core::Word::ConstexprQualifier, { "constexpr" });
	if(IsRust) SetWord(Core::Word::DocAttributeName, { "doc" });
	if(IsC) SetWord(Core::Word::FloatingType, { "float" });
	// 標準対象の登録から暗黙名と導入の条件を導き，別名・修飾名の一覧を複製しない
	for(const std::pair<const std::string_view, Core::RoleSet> &Entry : MemberRoles) {
		const std::string_view Target = Entry.first;
		const size_t Dot = Target.rfind('.');
		Rules.ImplicitBindingTargets.emplace(Dot == std::string_view::npos ? Target : Target.substr(Dot + 1), Target);
		if(IsPython) Rules.Words[static_cast<size_t>(Core::Word::ImportedTarget)].push_back(Target);
	}
	if(IsPython) std::ranges::sort(Rules.Words[static_cast<size_t>(Core::Word::ImportedTarget)]);
	Rules.CanContinueWithBrace = Lang == Lang::Kotlin || Lang == Lang::Swift;
	// 固定の語定義を確定した時に，複合ノードの照合の上限を１度求める
	for(size_t Idx = 0; Idx < Rules.Words.size(); ++Idx) for(const std::string_view Spelling : Rules.Words[Idx]) {
		Rules.WordMaxLengths[Idx] = std::max(Rules.WordMaxLengths[Idx], Spelling.size());
	}
	Rules.CanContinueWithCall = Lang == Lang::Kotlin;
	Reading.CanMisreadCallContinuation = Lang == Lang::Kotlin;
	Rules.CanOmitTrailingSemicolon = Lang == Lang::Go || Lang == Lang::Kotlin || Lang == Lang::Swift;
	Rules.IsWhitespaceSemantic = Lang == Lang::CSS;
	Reading.CanMisreadSelectorCommentBoundary = Lang == Lang::CSS;
	Rules.LexicalSeparator = Lang == Lang::CSS ? "/**/" : "";
	Rules.HasDeferredLiteralBodies = std::ranges::any_of(
		Kinds,
		[](const Core::KindDef &Def) -> bool {
			return Def.Roles.test(static_cast<size_t>(Core::Role::DeferredLiteralStart));
		}
	);
	Rules.HasLineDirectives = Lang.IsCFamily();
	Rules.HasAutomaticLineTerminator = Lang == Lang::Go;
	Rules.CanAttachTopLevelSemicolon = Lang == Lang::Rust;
	// 解析用の本文を編集して読み直すパーサーの読違
	Reading.CanMisbindConstructionCallee = Lang == Lang::TypeScript;
	Reading.CanMisreadSoftKeywordCalls = Lang == Lang::Python;
	if(Reading.CanMisreadSoftKeywordCalls) Reading.SoftKeywordCallHead = "type";
	Reading.DoesMisbindEmbeddedBranches = Lang == Lang::PHP;
	Reading.DoesMisbindPostfixOperand = Lang == Lang::Swift;
	Reading.CanMisassociateOperators = Lang == Lang::Swift;
	// 部分整形を始めれないエラーを持つ入力の条件（整形仕様 4.6，エラーの回復が領域の境界を失わせる文法と既知の読違）
	Reading.CanRecoveryRestructureDoc = Lang == Lang::HTML;
	Reading.CanAbsorbVerbatimOnError = std::ranges::any_of(
		Kinds,
		[](const Core::KindDef &Def) -> bool {
			return Def.Roles.test(static_cast<size_t>(Core::Role::HeredocBody));
		}
	);
	Reading.VerbatimStart = Lang == Lang::PHP ? "<<<" : "";
	Reading.CanMisreadCustomProperties = Lang == Lang::CSS;
	Reading.CanAbsorbRulesIntoValComments = Lang == Lang::CSS;
	Reading.CanMisreadRuleArgs = Lang.IsScss;
	Reading.UrlFunctionHead = Lang == Lang::CSS ? "url(" : "";
	Reading.BoolNameHeader = Lang.IsCFamily() ? "<stdbool.h>" : "";
	Reading.ImportedForeignPackage = Lang == Lang::Go ? "\"C\"" : "";
	Reading.CustomPropertyHead = Lang == Lang::CSS ? "--" : "";
	if(Lang == Lang::CSS) Reading.ModuleClauseWords = { "as", "hide", "show" };
	Reading.PropertySeparator = Lang == Lang::CSS ? ":" : "";
	// 引用毎の終端・エスケープ・補間の登録（複数引用符は通常引用より前に照合する）
	if(Lang == Lang::Python) {
		Reading.Literals.push_back(
			{
				.Open = "\"\"\"",
				.Close = "\"\"\"",
				.InterpolationHeads = { "{" },
				.InterpolationPrefixes = "fFtT",
				.HasDoubledInterpolationEscape = true,
				.UnescapedInterpolationChars = "{",
				.CanCrossLine = true
			}
		);
		Reading.Literals.push_back(
			{
				.Open = "'''",
				.Close = "'''",
				.InterpolationHeads = { "{" },
				.InterpolationPrefixes = "fFtT",
				.HasDoubledInterpolationEscape = true,
				.UnescapedInterpolationChars = "{",
				.CanCrossLine = true
			}
		);
	} else if(Lang == Lang::CSharp || Lang == Lang::Java || Lang == Lang::Kotlin || Lang == Lang::Swift) {
		Reading.Literals.push_back(
			{
				.Open = "\"\"\"",
				.Close = "\"\"\"",
				.Escape = Lang == Lang::Kotlin || Lang == Lang::CSharp ? "" : "\\",
				.InterpolationHeads = Lang == Lang::Kotlin ? std::vector<std::string_view>{ "${", "$" } : Lang == Lang::Swift ?
				std::vector<std::string_view>{ "\\(" } :
				Lang == Lang::CSharp ? std::vector<std::string_view>{ "{" } : std::vector<std::string_view>(),
				.InterpolationPrefixes = Lang == Lang::CSharp ? "$" : "",
				.HasRepeatedInterpolationHead = Lang == Lang::CSharp,
				.RepeatedInterpolationPrefix = Lang == Lang::CSharp || Lang == Lang::Kotlin ? "$" : "",
				.Delimiter = Lang == Lang::CSharp ? LiteralDelimiter::Repeated : LiteralDelimiter::Fixed,
				.CanCrossLine = true
			}
		);
	}
	Reading.Literals.push_back(
		{
			.Open = "\"",
			.Close = "\"",
			.Escape = Lang == Lang::HTML ? "" : "\\",
			.InterpolationHeads = Lang == Lang::PHP ? std::vector<std::string_view>{ "${", "{$", "$" } : Lang == Lang::Kotlin ?
			std::vector<std::string_view>{ "${", "$" } :
			Lang == Lang::Ruby ? std::vector<std::string_view>{ "#{", "#@", "#$" } : Lang == Lang::Swift ?
			std::vector<std::string_view>{ "\\(" } :
			Lang == Lang::CSharp || Lang == Lang::Python ? std::vector<std::string_view>{ "{" } : std::vector<std::string_view>(),
			.InterpolationPrefixes = Lang == Lang::CSharp ? "$" : Lang == Lang::Python ? "fFtT" : "",
			.HasDoubledInterpolationEscape = Lang == Lang::CSharp || Lang == Lang::Python,
			.RepeatedInterpolationPrefix = Lang == Lang::Kotlin ? "$" : "",
			.UnescapedInterpolationChars = Lang == Lang::Python ? "{" : "",
			.CanCrossLine = Lang == Lang::PHP || Lang == Lang::Ruby || Lang == Lang::HTML
		}
	);
	if(Lang != Lang::Swift && Lang != Lang::JSON) {
		Reading.Literals.push_back(
			{
				.Open = "'",
				.Close = "'",
				.Escape = Lang == Lang::HTML ? "" : "\\",
				.InterpolationHeads = Lang == Lang::Python ? std::vector<std::string_view>{ "{" } : std::vector<std::string_view>(),
				.InterpolationPrefixes = Lang == Lang::Python ? "fFtT" : "",
				.HasDoubledInterpolationEscape = Lang == Lang::Python,
				.UnescapedInterpolationChars = Lang == Lang::Python ? "{" : "",
				.CanCrossLine = Lang == Lang::PHP || Lang == Lang::Ruby || Lang == Lang::HTML
			}
		);
	}
	if(Lang.IsJsTs() || Lang == Lang::Go || Lang == Lang::Ruby) {
		Reading.Literals.push_back(
			{
				.Open = "`",
				.Close = "`",
				.Escape = Lang == Lang::Go ? "" : "\\",
				.InterpolationHeads = Lang.IsJsTs() ?
				std::vector<std::string_view>{ "${" } :
				Lang == Lang::Ruby ? std::vector<std::string_view>{ "#{", "#@", "#$" } : std::vector<std::string_view>(),
				.CanCrossLine = true
			}
		);
	}
	if(Lang == Lang::PHP) {
		Reading.Literals.push_back(
			{
				.Open = "<<<",
				.Close = "'",
				.Escape = "",
				.Delimiter = LiteralDelimiter::LineWord,
				.CanCrossLine = true,
				.CanSpaceBeforeWord = true,
				.CanIndentEnd = true,
				.EndFollowers = "!\"#$%&'()*+,-./:;<=>?@[\\]^`{|}~ 	\r\n"
			}
		);
		Reading.Literals.push_back(
			{
				.Open = "<<<",
				.Close = "\"",
				.InterpolationHeads = { "${", "{$", "$" },
				.Delimiter = LiteralDelimiter::LineWord,
				.CanCrossLine = true,
				.CanSpaceBeforeWord = true,
				.CanIndentEnd = true,
				.EndFollowers = "!\"#$%&'()*+,-./:;<=>?@[\\]^`{|}~ 	\r\n"
			}
		);
		Reading.Literals.push_back(
			{
				.Open = "<<<",
				.Close = "",
				.InterpolationHeads = { "${", "{$", "$" },
				.Delimiter = LiteralDelimiter::LineWord,
				.CanCrossLine = true,
				.CanSpaceBeforeWord = true,
				.CanIndentEnd = true,
				.EndFollowers = "!\"#$%&'()*+,-./:;<=>?@[\\]^`{|}~ 	\r\n"
			}
		);
	} else if(Lang == Lang::Ruby) {
		// 同じ開きでは，語を囲む引用符の在る規則を裸の語より先に登録する
		for(const std::string_view Head : { "<<~", "<<-", "<<" }) for(const std::string_view Quote : { "'", "\"", "`", "" }) {
			Reading.Literals.push_back(
				{
					.Open = Head,
					.Close = Quote,
					.Escape = Quote == "'" ? "" : "\\",
					.InterpolationHeads = Quote == "'" ? std::vector<std::string_view>() : std::vector<std::string_view>{ "#{", "#@", "#$" },
					.Delimiter = LiteralDelimiter::LineWord,
					.CanShareOperatorHead = true,
					.CanCrossLine = true,
					.CanIndentEnd = Head != "<<"
				}
			);
		}
		for(const std::string_view Head : { "%q", "%w", "%i", "%s", "%Q", "%W", "%I", "%r", "%x", "%" }) {
			Reading.Literals.push_back(
				{
					.Open = Head,
					.Close = ")]}>",
					.PairedOpen = "([{<",
					.InterpolationHeads = Head.size() > 1 && (Head[1] == 'q' || Head[1] == 'w' || Head[1] == 'i' || Head[1] == 's') ?
					std::vector<std::string_view>() :
					std::vector<std::string_view>{ "#{", "#@", "#$" },
					.Delimiter = LiteralDelimiter::Paired,
					.CanShareOperatorHead = true,
					.CanCrossLine = true,
					.CanNest = true
				}
			);
		}
	}
	Reading.HasRegexLiterals = Lang.IsJsTs() || Lang == Lang::Ruby || Lang == Lang::Swift;
	Reading.CanRegexFollowName = Lang == Lang::Ruby;
	Reading.CanRegexCrossLine = Lang == Lang::Ruby || Lang == Lang::Swift;
	Reading.CanRegexStartLine = Reading.CanRegexCrossLine;
	Reading.HasHashRegexDelimiter = Lang == Lang::Swift;
	Reading.HasNestedRegexClasses = Lang == Lang::Ruby;
	if(Lang.IsJsTs()) {
		Reading.RegexWords = {
			"await",
			"break",
			"case",
			"continue",
			"debugger",
			"delete",
			"do",
			"else",
			"in",
			"instanceof",
			"new",
			"of",
			"return",
			"throw",
			"typeof",
			"void",
			"yield"
		};
		Reading.RegexBridgeWords = { "await" };
		Reading.RegexStmtHeads = { "catch", "for", "if", "switch", "while", "with" };
	} else if(Lang == Lang::Swift) {
		Reading.RegexWords = { "await", "case", "for", "guard", "if", "in", "return", "throw", "try", "while" };
	}
	Reading.CanMisreadEscapedRegexEnd = Lang == Lang::Swift;
	Reading.CanDropTokensOnRecovery = Lang == Lang::Kotlin;
	Reading.CanTolerateTrailingCommaError = Lang == Lang::JSON && Lang.IsJsonc;
	Reading.ShouldRejectComments = Lang == Lang::JSON && !Lang.IsJsonc;
	Reading.HasSingleRootVal = Lang == Lang::JSON;
	Reading.HasStrictStrEscapes = Lang == Lang::JSON;
	Reading.ShouldHaveFractionDigits = Lang == Lang::JSON;
	Reading.CanAcceptArrowOperand = Lang.IsJsTs();
	Reading.CanAcceptContinuationComment = Lang == Lang::Python;
	Reading.CanAcceptStmtLineBreak = Lang == Lang::Python;
	Reading.CanAcceptVerticalTabSpace =
	Lang == Lang::Java || Lang == Lang::Go || Lang == Lang::Kotlin || Lang == Lang::PHP || Lang == Lang::Python ||
	Lang == Lang::JSON || Lang == Lang::CSS;
	Reading.CanAcceptFormFeedSpace = Lang == Lang::Go || Lang == Lang::PHP || Lang == Lang::JSON;
	Rules.HasHeadMemberArrow = Lang.IsCFamily() || Lang == Lang::PHP;
	Rules.HasNumericUnitAdjacency = Lang == Lang::CSS;
	Rules.HasBareControlHeader =
	Lang == Lang::Go || Lang == Lang::Rust || Lang == Lang::Swift || Lang == Lang::Ruby || Lang == Lang::Python;
	if(Lang == Lang::Python) {
		Rules.LexemeRoleSpellings[Core::Role::TrailingOperatorToken] =
		{ "//", "@", "and", "else", "if", "in", "is", "is not", "not in", "or" };
	}
	if(Lang == Lang::Ruby) {
		Rules.LexemeRoleSpellings[Core::Role::LeadingClauseToken] = { "else", "elsif", "end", "ensure", "rescue", "when" };
	}
	if(Lang == Lang::Ruby) Reading.ClauseWords = { "do", "else", "elsif", "end", "ensure", "in", "rescue", "then", "when" };
	if(Lang.IsJsTs()) {
		Reading.SeparatingModifiers = { "async" };
		if(Lang == Lang::TypeScript) {
			Reading.SeparatingModifiers =
			{ "abstract", "accessor", "async", "declare", "override", "private", "protected", "public", "readonly" };
		}
		Reading.JoiningModifiers = { "get", "set", "static" };
	}
	if(Lang == Lang::Kotlin) {
		Reading.LineContinuingTokens = { "&&", ".", "?.", "?:", "as", "as?", "by", "catch", "else", "finally", "where", "{", "||" };
	}
	if(Lang == Lang::Go) Rules.LexemeRoleSpellings[Core::Role::LeadingClauseToken] = { "case", "default" };
	if(Lang.IsCFamily()) {
		Reading.CondEndDirective = "endif";
		Reading.DefDirective = "define";
		Reading.UndefinitionDirective = "#undef";
		Reading.LinkageHead = "extern";
		Reading.LinkageClose = "}";
		Reading.LinkageMarkerDirective = "#pragma ";
		Reading.RawStrPrefixes = { "LR", "R", "UR", "u8R", "uR" };
		if(Lang == Lang::Cpp) Reading.MacroRawStrPrefixes = Reading.RawStrPrefixes;
		if(Lang != Lang::C || Lang.IsAmbiguous) Reading.MeasuredRawStrPrefixes = Reading.RawStrPrefixes;
		if(Lang.IsAmbiguous) Reading.UncertainRawStrPrefixes = Reading.RawStrPrefixes;
		Reading.VariadicParam = "__VA_ARGS__";
		Reading.StmtWords = { "break", "continue", "do", "for", "goto", "if", "return", "switch", "while" };
		Reading.CondDirectives = { "if", "ifdef", "ifndef" };
		Reading.CondAlternativeDirectives = { "elif", "elifdef", "elifndef", "else" };
		// 最長一致を優先し，同じ長さの前処理トークンは文字コード順
		Reading.CondPunctuators = {
			"%:%:",
			"->*",
			"...",
			"<<=",
			"<=>",
			">>=",
			"!=",
			"##",
			"%:",
			"%=",
			"%>",
			"&&",
			"&=",
			"*=",
			"++",
			"+=",
			"--",
			"-=",
			"->",
			".*",
			"/=",
			"::",
			":>",
			"<%",
			"<:",
			"<<",
			"<=",
			"==",
			">=",
			">>",
			"^=",
			"|=",
			"||"
		};
		Reading.HeaderNameDirectives = { "import", "include", "include_next" };
		Reading.ParsedDirectives =
		{ "elif", "elifdef", "elifndef", "else", "endif", "if", "ifdef", "ifndef", "import", "include", "include_next", "undef" };
	}
	Rules.OperatorPrec = {
		{ "!=", 9 },
		{ "!==", 9 },
		{ "!in", 10 },
		{ "!is", 10 },
		{ "!~", 9 },
		{ "%", 13 },
		{ "&", 8 },
		{ "&&", 5 },
		{ "&^", 13 },
		{ "*", 13 },
		{ "**", 14 },
		{ "+", 12 },
		{ "-", 12 },
		{ "..", 10 },
		{ "...", 10 },
		{ "..<", 10 },
		{ "..=", 10 },
		{ "/", 13 },
		{ "//", 13 },
		{ ":", 2 },
		{ "<", 10 },
		{ "<<", 11 },
		{ "<=", 10 },
		{ "<=>", 9 },
		{ "==", 9 },
		{ "===", 9 },
		{ "=~", 9 },
		{ ">", 10 },
		{ ">=", 10 },
		{ ">>", 11 },
		{ ">>>", 11 },
		{ "?", 2 },
		{ "?:", 3 },
		{ "??", 3 },
		{ "@", 13 },
		{ "^", 7 },
		{ "and", 5 },
		{ "as", 10 },
		{ "as?", 10 },
		{ "in", 10 },
		{ "instanceof", 10 },
		{ "is", 10 },
		{ "is not", 10 },
		{ "not in", 10 },
		{ "not_eq", 9 },
		{ "or", 4 },
		{ "rescue", 3 },
		{ "satisfies", 10 },
		{ "|", 6 },
		{ "||", 4 }
	};
	Rules.CmpOperators = { "!=", "!==", "!~", "<", "<=", "<=>", "==", "===", "=~", ">", ">=" };
	for(std::pair<const std::string_view, int> &Entry : Rules.OperatorPrec) {
		const std::string_view Operator = Entry.first;
		int &Prec = Entry.second;
		// ノード毎の文法の種別の補正
		const auto Correct = [&]() -> int {
			switch(Lang.Id) {
			case Lang::Kotlin:
				// Kotlin の範囲・中置関数・エルビス・型判定・比較の順の返戻
				if(Operator == "as" || Operator == "as?") return 14;
				if(Operator == ".." || Operator == "..<") return 11;
				if(Operator == "?:") return 9;
				if(Operator == "in" || Operator == "!in" || Operator == "is" || Operator == "!is") return 8;
				if(Operator == "<" || Operator == ">" || Operator == "<=" || Operator == ">=") return 7;
				if(Operator == "==" || Operator == "!=" || Operator == "===" || Operator == "!==") return 6;
				break;
			case Lang::Go:
				// Go の `<<` / `>>` / `&` / `&^` の乗除格，`|` / `^` の加減格，比較の同格の左結合の返戻
				if(Operator == "<<" || Operator == ">>" || Operator == "&") return 13;
				if(Operator == "|" || Operator == "^") return 12;
				if(Rules.CmpOperators.contains(Operator)) return 10;
				break;
			case Lang::Swift:
				// Swift の << >> の乗除より上で前置単項より下，& の乗除格，| ^ の加減格，比較の加減より下の返戻
				if(Operator == "<<" || Operator == ">>") return 14;
				if(Operator == "&") return 13;
				if(Operator == "|" || Operator == "^") return 12;
				if(Rules.CmpOperators.contains(Operator)) return 7;
				if(Operator == "??") return 8;
				break;
			case Lang::Rust:
			case Lang::Python:
				// 比較より上のビット演算と，Rust の前置・乗除間の as，論理・代入間の範囲の返戻
				if(Operator == "as" && Lang == Lang::Rust) return 14;
				if((Operator == ".." || Operator == "..=") && Lang == Lang::Rust) return 3;
				if(Operator == "&") return 10;
				if(Operator == "^") return 9;
				if(Operator == "|") return 8;
				if(
					Rules.CmpOperators.contains(Operator) || Operator == "is" || Operator == "in" || Operator == "is not" || Operator == "not in"
				) return 7;
				break;
			case Lang::PHP:
				// PHP の or < xor < and < 代入は数値空間に３段を収めれない為，優先度不明の０として内外の括弧を保護する事の返戻
				if(Operator == "and" || Operator == "or" || Operator == "xor") return 0;
				break;
			case Lang::Ruby:
				// Ruby の低位演算子の再配置（`&` / `|` / `^` は同格とし，混在時の括弧を保護）の返戻
				if(Operator == "&" || Operator == "|" || Operator == "^") return 10;
				if(Operator == "<" || Operator == "<=" || Operator == ">" || Operator == ">=") return 9;
				if(Rules.CmpOperators.contains(Operator)) return 8;
				if(Operator == "&&") return 7;
				if(Operator == "||") return 6;
				if(Operator == ".." || Operator == "...") return 5;
				if(Operator == "?" || Operator == ":") return 4;
				if(Operator == "and" || Operator == "or") return 1;
				break;
			}
			// 共通の優先順位の返戻
			return Prec;
		};
		Prec = Correct();
	}
	if(Lang == Lang::Python) Reading.MisboundBinaryOperators = { "&", "^" };
	if(Lang == Lang::Swift) {
		Rules.OperatorPrec.emplace("&<<", 14);
		Rules.OperatorPrec.emplace("&>>", 14);
		Rules.OperatorPrec.emplace("&*", 13);
		Rules.OperatorPrec.emplace("&+", 12);
		Rules.OperatorPrec.emplace("&-", 12);
		Rules.OperatorPrec["..."] = 10;
		Rules.OperatorPrec["..<"] = 10;
		Rules.OperatorPrec["=="] = 7;
		Rules.OperatorPrec["!="] = 7;
		Rules.OperatorPrec["==="] = 7;
		Rules.OperatorPrec["!=="] = 7;
		Rules.OperatorPrec["~="] = 7;
		Reading.ReassociatedOperators = {
			"!=",
			"!==",
			"%",
			"&",
			"&&",
			"&*",
			"&+",
			"&-",
			"&<<",
			"&>>",
			"*",
			"+",
			"-",
			"...",
			"..<",
			"/",
			"<",
			"<<",
			"<=",
			"==",
			"===",
			">",
			">=",
			">>",
			"??",
			"^",
			"|",
			"||",
			"~="
		};
		Reading.OperatorStandIns = {
			{ "!=", "==" },
			{ "!==", "!=" },
			{ "%", "/" },
			{ "&", "^" },
			{ "*", "/" },
			{ "+", "-" },
			{ "-", "+" },
			{ "/", "*" },
			{ "<", ">" },
			{ "<<", "^" },
			{ "<=", ">" },
			{ "==", "!=" },
			{ "===", "==" },
			{ ">", "<" },
			{ ">=", "<" },
			{ ">>", "^" },
			{ "^", "&" },
			{ "|", "^" }
		};
	}
	Rules.AssignmentOperators =
	{ "%=", "&&=", "&=", "&^=", "**=", "*=", "+=", "-=", "//=", "/=", "<<=", "=", ">>=", ">>>=", "?\?=", "^=", "|=", "||=" };
	for(const std::string_view Operator : Rules.AssignmentOperators) {
		Rules.OperatorPrec.emplace(Operator, Lang == Lang::Ruby ? 2 : 1);
	}
	if(Lang != Lang::Rust && Lang != Lang::Ruby && Lang != Lang::Python) Rules.FusedPrefixPairs = { "&&", "++", "--" };
	if(Lang == Lang::Ruby) Rules.FusedPrefixPairs.insert("!~");
	if(Lang == Lang::Kotlin) Rules.FusedPrefixPairs.insert("!!");
	Rules.FusedRangePairs = { ".." };
	Rules.OperatorFusionChars = "!%&*+-/:<=>?^|~";
	Rules.AssignmentFusionBefore = ".:<>!=+-*/%&|^?~";
	Rules.AssignmentFusionAfter = "=>~";
	Rules.AttachedOperatorPrefixChars = "?!";
	Reading.MisparsedCmpLeadChars = "=>";
	Rules.OpenRangeClosers = Lang == Lang::Ruby ? ")]}," : ")]},;";
	Rules.RequiredValParenPrecs = Lang == Lang::Ruby ? std::unordered_set<int>{ 1 } : std::unordered_set<int>{};
	Rules.SignsOverPower = Lang == Lang::Python ? "-+~" : Lang == Lang::Ruby ? "-" : "";
	Rules.PowerPrec = Lang == Lang::Python || Lang.IsJsTs() || Lang == Lang::PHP || Lang == Lang::Ruby ? 14 : -1;
	Rules.AssignmentPrec = Lang == Lang::Ruby ? 2 : 1;
	Rules.ConditionalPrec = Lang == Lang::Ruby ? 4 : 2;
	Rules.TypeContinuationTokens = Lang.IsJsTs() ?
	std::unordered_set<std::string_view>{ "&", "?", "|" } :
	Lang == Lang::Swift ? std::unordered_set<std::string_view>{ "&" } : std::unordered_set<std::string_view>{};
	Rules.NonAssocPrecs = Lang == Lang::Rust || Lang == Lang::Python ?
	std::unordered_set<int>{ 7 } :
	Lang == Lang::Swift ? std::unordered_set<int>{ 7, 10, 14 } : Lang == Lang::Ruby ?
	std::unordered_set<int>{ 8 } :
	Lang == Lang::PHP ? std::unordered_set<int>{ 9, 10 } : std::unordered_set<int>{};
	Rules.RightAssocPrecs = Lang == Lang::Ruby ? std::unordered_set<int>{ 2, 4 } : Lang == Lang::Swift ?
	std::unordered_set<int>{ 1, 2, 8 } :
	Lang == Lang::CSharp ? std::unordered_set<int>{ 1, 2, 3 } : std::unordered_set<int>{ 1, 2 };
	if(Lang == Lang::Ruby) Rules.ProtectedPrecPairs.emplace_back(10, 10);
	if(Lang.IsJsTs() || Lang == Lang::Cpp) {
		Rules.ProtectedPrecPairs.emplace_back(9, 11);
		Rules.ProtectedPrecPairs.emplace_back(10, 11);
	}
	if(Lang.IsJsTs()) for(const int Prec : { 4, 5 }) {
		Rules.ProtectedPrecPairs.emplace_back(3, Prec);
		Rules.ProtectedPrecPairs.emplace_back(Prec, 3);
	}
	// 宣言と式のノード種別の参照
	Rules.PrefixExprDef.Def = Find("prefix_expression", true);
	Rules.ExprParenDef.Def = Find("parenthesized_expression", true);
	Rules.HasBackwardBindingTypeAnnotation = Lang == Lang::Swift;
	Rules.HasReassignableLexicalKeyword = Lang.IsJsTs();
	Rules.ShouldPreserveBindingTypes = IsTs;
	Rules.AreFunctionNamesCaseInsensitive = IsPhp;
	Rules.HasNamespaceFunctionImports = IsPhp;
	if(IsPhp) {
		Rules.ImportPathSeparator = "\\";
		ChildRoleMask |= uint64_t{ 1 } << static_cast<uint8_t>(Core::ChildRole::Path);
	}
	Rules.ShouldNormalizeIdentifierNfkc = Lang == Lang::Python;
	Rules.HasFunctionLocalBindings = Lang == Lang::Python;
	Rules.HasBindingIdentity =
	Lang.IsCFamily() || Lang == Lang::CSharp || Lang == Lang::Java || Lang == Lang::Go || Lang == Lang::Rust ||
	Lang == Lang::Kotlin || Lang == Lang::Swift || Lang == Lang::PHP || Lang.IsJsTs() || Lang == Lang::Ruby || Lang == Lang::Python;
	if(Rules.HasBindingIdentity) {
		ChildRoleMask |= uint64_t{ 1 } << static_cast<uint8_t>(Core::ChildRole::Name);
		ChildRoleMask |= uint64_t{ 1 } << static_cast<uint8_t>(Core::ChildRole::Body);
		ChildRoleMask |= uint64_t{ 1 } << static_cast<uint8_t>(Core::ChildRole::Params);
		ChildRoleMask |= uint64_t{ 1 } << static_cast<uint8_t>(Core::ChildRole::Val);
		ChildRoleMask |= uint64_t{ 1 } << static_cast<uint8_t>(Core::ChildRole::Type);
	}
	Rules.HasOrderedBindings =
	Lang.IsCFamily() || Lang == Lang::CSharp || Lang == Lang::Java || Lang == Lang::Go || Lang == Lang::Rust ||
	Lang == Lang::Kotlin || Lang == Lang::Swift;
	Rules.HasBindingInInitializer =
	Lang.IsCFamily() || Lang == Lang::CSharp || Lang == Lang::Java || Lang == Lang::Go || Lang == Lang::Swift ||
	Lang == Lang::PHP || Lang == Lang::Ruby;
	Rules.HasBareBindingParams = Lang == Lang::Java || Lang == Lang::Rust;
	Rules.HasOrderedBindingShadow = Lang == Lang::Rust;
	Rules.HasMemberBindingReceiver = Lang == Lang::Kotlin || Lang == Lang::Java;
	Rules.HasLocalBindingPriority = Lang == Lang::Kotlin;
	if(Lang.IsJsTs()) Rules.ImplicitFunctionBinding = "arguments";
	if(Lang == Lang::Kotlin) {
		Rules.ImplicitBindingParam = "it";
		Rules.BindingNameQuote = "`";
	}
	Rules.CanMutateBase = Lang == Lang::Rust;
	Rules.ShouldRemoveMutableMarker = Lang == Lang::Rust;
	Reading.IdentifierRawPrefix = Lang == Lang::Rust ? "r#" : Lang == Lang::CSharp ? "@" : "";
	Reading.IdentifierQuote = Lang == Lang::Kotlin || Lang == Lang::Swift ? "`" : "";
	Reading.LabelDefSuffix = Lang == Lang::Kotlin ? "@" : Lang == Lang::Swift ? ":" : "";
	Reading.ShouldDecodeIdentifierEscapes = Lang.IsCFamily() || Lang == Lang::CSharp || Lang == Lang::Java || Lang.IsJsTs();
	Reading.ShouldRestrictBasicIdentifierEscapes = Lang.IsCFamily();
	Reading.ShouldIgnoreIdentifierFormatChars = Lang == Lang::CSharp || Lang == Lang::Java;
	Rules.ShouldPreserveConstantStatus = Lang == Lang::Java;
	Rules.ModifierListDef.Def = Find("modifiers", true);
	AttachForms();
	// 読取規則と種別の固定定義から，投影で詳しく判定する対象だけを１度選ぶ
	ProjectionKinds.reserve(Kinds.size());
	for(const Core::KindDef &Kind : Kinds) {
		const std::string_view Name = GetSymbolName(static_cast<TSSymbol>(ProjectionKinds.size()));
		const bool IsBinary = Kind.Roles.test(static_cast<size_t>(Core::Role::BinaryOperation));
		ProjectionKinds.push_back(
			{
				Lang == Lang::CSharp && Kind.Roles.test(static_cast<size_t>(Core::Role::TypeInferenceInspection)),
				Reading.CanMisreadGenericCalls && IsBinary,
				Reading.CanMisreadTemplateDecls && IsBinary,
				Reading.CanMisreadTemplateArgs && (
					Kind.Roles.test(static_cast<size_t>(Core::Role::MisparsedTemplateHost)) ||
					Kind.Roles.test(static_cast<size_t>(Core::Role::AngleBracketList)) ||
					Kind.Roles.test(static_cast<size_t>(Core::Role::GroupingParen))
				),
				Names::DecodedLiteralPart.HasName(Name),
				Name == "heredoc_body"
			}
		);
	}
	// 語の意味は文法の固定定義から投影の役割へ対応付け，共通工程へ綴りの比較を持ち込まない
	static constexpr std::pair<Core::Role, Core::Word> KeywordMeanings[] = {
		{ Core::Role::AwaitKeyword, Core::Word::AwaitName },
		{ Core::Role::BaseReceiverKeyword, Core::Word::BaseRef },
		{ Core::Role::BlockCloseKeyword, Core::Word::BlockEnd },
		{ Core::Role::CaseKeyword, Core::Word::Case },
		{ Core::Role::CastKeyword, Core::Word::Cast },
		{ Core::Role::ConstantConditionKeyword, Core::Word::ConstantCond },
		{ Core::Role::ConstructionKeyword, Core::Word::Construct },
		{ Core::Role::DefinitionKeyword, Core::Word::Define },
		{ Core::Role::FallthroughKeyword, Core::Word::Fallthrough },
		{ Core::Role::FunctionKeyword, Core::Word::FunctionHead },
		{ Core::Role::IterationKeyword, Core::Word::In },
		{ Core::Role::KeywordWhile, Core::Word::While },
		{ Core::Role::LabeledReceiverKeyword, Core::Word::LabeledReceiver },
		{ Core::Role::SelfReceiverKeyword, Core::Word::Self },
		{ Core::Role::TypeDeclarationKeyword, Core::Word::TypeDecl },
		{ Core::Role::ValueOutputTag, Core::Word::ValOutputTag }
	};
	// 語の照合は入力の固定定義へ集約する
	for(size_t Meaning = 0; Meaning < static_cast<size_t>(Core::Word::Count); ++Meaning) {
		for(const std::string_view Text : Rules.Words[Meaning]) {
			WordTextLimit = std::max(WordTextLimit, Text.size());
			if(!Rules.CaseInsensitiveWords.test(Meaning)) WordMeanings[Text].set(Meaning);
			else {
				std::string Folded(Text);
				for(char &Char : Folded) Char = static_cast<char>(std::tolower(static_cast<unsigned char>(Char)));
				FoldedWordMeanings[std::move(Folded)].set(Meaning);
			}
		}
	}
	// 可視性の語の同一性と優先順は入力側で確定する
	for(const std::string_view Access : Rules.AccessOrder) {
		AccessLimit = std::max(AccessLimit, Access.size());
		for(size_t Start = 0; Start < Access.size();) {
			const size_t End = Access.find(' ', Start);
			const std::string_view Token = Access.substr(Start, End == std::string_view::npos ? Access.size() - Start : End - Start);
			if(!AccessSymbols.contains(Token)) {
				if(AccessSymbols.size() > 62) throw std::length_error("too many access modifier meanings");
				AccessSymbols.emplace(Token, uint64_t{ 1 } << AccessSymbols.size());
			}
			if(End == std::string_view::npos) break;
			Start = End + 1;
		}
	}
	for(const std::string_view Access : Rules.AccessOrder) Rules.AccessMeanings.push_back(ReadAccessMeaning(Access));
	Rules.ImplicitAccessMeaning = ReadAccessMeaning(Rules.ImplicitAccess);
	Rules.PublicAccessMeaning = ReadAccessMeaning(Rules.GetSpelling(Core::Word::PublicAccess));
	Rules.PrivateAccessMeaning = ReadAccessMeaning(Rules.GetSpelling(Core::Word::PrivateAccess));
	// 静的解析が使う語も，入力時に型・修飾・演算の役割へ確定する
	Rules.LexemeRoleSpellings[Core::Role::MemberTypeOperation] = Rules.MemberTypeModifiers;
	Rules.LexemeRoleSpellings[Core::Role::ExplicitVirtualModifier] =
	{ std::begin(LintRules::ExplicitVirtualWords), std::end(LintRules::ExplicitVirtualWords) };
	if(!Rules.MemberStaticWord.empty()) Rules.LexemeRoleSpellings[Core::Role::ModifierStatic] = { Rules.MemberStaticWord };
	Rules.LexemeRoleSpellings[Core::Role::ImmutableTypeQualifier] = { LintRules::ConstQualifier };
	Rules.LexemeRoleSpellings[Core::Role::InstanceStateInvalidModifier] =
	{ std::begin(LintRules::InvalidInstanceStateWords), std::end(LintRules::InvalidInstanceStateWords) };
	Rules.LexemeRoleSpellings[Core::Role::MemberClassKeyword] = { "class" };
	Rules.LexemeRoleSpellings[Core::Role::MemberEnumKeyword] = { "enum" };
	Rules.LexemeRoleSpellings[Core::Role::MemberStructKeyword] = { "struct" };
	Rules.LexemeRoleSpellings[Core::Role::ModifierConstantEvaluation] = { "consteval" };
	Rules.LexemeRoleSpellings[Core::Role::ModifierConstantExpression] = { "constexpr" };
	Rules.LexemeRoleSpellings[Core::Role::ModifierConstantInitialization] = { "constinit" };
	Rules.LexemeRoleSpellings[Core::Role::ModifierInline] = { "inline" };
	Rules.LexemeRoleSpellings[Core::Role::ModifierMutable] = { "mutable" };
	Rules.LexemeRoleSpellings[Core::Role::ModifierThreadLocal] = { "thread_local" };
	if(IsSpelling(LintRules::NullWord, Core::Word::NullishVal)) {
		Rules.LexemeRoleSpellings[Core::Role::NullishNull] = { LintRules::NullWord };
	}
	if(IsSpelling(LintRules::UndefinedWord, Core::Word::NullishVal)) {
		Rules.LexemeRoleSpellings[Core::Role::NullishUndefined] = { LintRules::UndefinedWord };
	}
	Rules.LexemeRoleSpellings[Core::Role::StaticLifetimeModifier] =
	{ std::begin(LintRules::StaticStorageWords), std::end(LintRules::StaticStorageWords) };
	Rules.LexemeRoleSpellings[Core::Role::VolatileTypeQualifier] = { LintRules::VolatileQualifier };
	Rules.LexemeRoleSpellings[Core::Role::XorOperator] =
	{ std::begin(LintRules::ExclusiveOrWords), std::end(LintRules::ExclusiveOrWords) };
	if(!Rules.MemberVirtualWord.empty()) Rules.LexemeRoleSpellings[Core::Role::ModifierVirtual] = { Rules.MemberVirtualWord };
	std::unordered_map<std::string_view, Core::RoleSet> LexemeRoles;
	for(const std::pair<Core::Role, Core::Word> &Meaning : KeywordMeanings) {
		Rules.LexemeRoleSpellings[Meaning.first] = Rules.Words[static_cast<size_t>(Meaning.second)];
	}
	for(const std::pair<const Core::Role, std::vector<std::string_view>> &Entry : Rules.LexemeRoleSpellings) {
		for(const std::string_view Lexeme : Entry.second) LexemeRoles[Lexeme].set(static_cast<size_t>(Entry.first));
	}
	// 同じ役割・名前付の性質を持つ記号の種別を共有し，空行比較の値は分離する
	KindsBySymbol.reserve(Kinds.size());
	for(const Core::KindDef &Kind : Kinds) {
		if(
			const std::deque<Core::KindDef>::const_iterator Found = std::find_if(
				SharedKinds.begin(),
				SharedKinds.end(),
				[&Kind](const Core::KindDef &Existing) -> bool {
					// 共有する構文上の性質の一致の返戻
					return Existing.Roles == Kind.Roles && Existing.IsNamed == Kind.IsNamed && Existing.Binding == Kind.Binding &&
					Existing.Bindings == Kind.Bindings && Existing.Evaluation == Kind.Evaluation && Existing.TypeBinding == Kind.TypeBinding;
				}
			); Found == SharedKinds.end()
		) {
			SharedKinds.push_back(Kind);
			KindsBySymbol.push_back(&SharedKinds.back());
		} else KindsBySymbol.push_back(&*Found);
	}
	// メソッドの所属でだけ確定する種別も文法の寿命で共有し，投影中には定義を作らない
	for(size_t Symbol = 0; Symbol < KindsBySymbol.size(); ++Symbol) {
		const Core::KindDef *const Base = KindsBySymbol[Symbol];
		if(!Base->Roles.test(static_cast<size_t>(Core::Role::ReturnHintContext)) || ConstructorKinds.contains(Base)) continue;
		Core::KindDef Constructor = *Base;
		Constructor.Roles.set(static_cast<size_t>(Core::Role::MemberConstructor));
		Constructor.Roles.reset(static_cast<size_t>(Core::Role::ReturnHintRequired));
		SharedKinds.push_back(std::move(Constructor));
		ConstructorKinds.emplace(Base, &SharedKinds.back());
		LexemeBases.emplace(&SharedKinds.back(), Base);
		Core::KindDef Returnless = *Base;
		Returnless.Roles.reset(static_cast<size_t>(Core::Role::ReturnHintRequired));
		if(Lang == Lang::PHP) Returnless.Roles.set(static_cast<size_t>(Core::Role::MemberDestructor));
		SharedKinds.push_back(std::move(Returnless));
		ReturnlessKinds.emplace(Base, &SharedKinds.back());
		LexemeBases.emplace(&SharedKinds.back(), Base);
	}
	// 固定の語又は解決した対象の効果を加え，基底と同じ役割を持つ定義を共有する
	const auto RegisterKind =
	[this](const Core::KindDef *const Kind, const std::string_view Lexeme, const Core::RoleSet &Extra, LexemeVariants &Variants)
	-> void {
		// 同じ固定定義の登録済の綴りは再生成しない事の返戻
		LexemeLimit = std::max(LexemeLimit, Lexeme.size());
		const LexemeVariants::const_iterator Existing = Variants.find(Lexeme);
		// 基底の固定契約を保ち，語の役割だけを加えた変更不能な定義を所有する
		Core::KindDef Fixed = Existing == Variants.end() ? *Kind : *Existing->second;
		Fixed.Roles |= Extra;
		if(Fixed.Roles == Kind->Roles) Variants.insert_or_assign(Lexeme, Kind);
		// 同じ基底と追加の役割を持つ綴りは固定定義を共有する
		else if(
			const LexemeVariants::const_iterator Found = std::ranges::find_if(
				Variants,
				[&Fixed](const std::pair<const std::string_view, const Core::KindDef *> &Entry) -> bool {
					// 同じ基底の固定定義の役割の一致の返戻
					return Entry.second->Roles == Fixed.Roles;
				}
			); Found != Variants.end()
		) Variants.insert_or_assign(Lexeme, Found->second);
		else {
			Fixed.InitRoleList();
			SharedKinds.push_back(std::move(Fixed));
			Variants.insert_or_assign(Lexeme, &SharedKinds.back());
			LexemeBases.emplace(&SharedKinds.back(), Kind);
		}
		// 終了
		return;
	};
	// 固定トークンは其の記号の綴りだけを登録し，名前付の候補は同じ種別の通常名と区別する
	for(size_t Symbol = 0; Symbol < KindsBySymbol.size(); ++Symbol) {
		const Core::KindDef *const Kind = KindsBySymbol[Symbol];
		if(!Kind->IsNamed) {
			if(
				const std::unordered_map<std::string_view, Core::RoleSet>::const_iterator Found = LexemeRoles.find(SymbolNames[Symbol]);
				Found != LexemeRoles.end()
			) RegisterKind(Kind, Found->first, Found->second, LexemeKinds[Kind]);
		} else if(
			Kind->Roles.test(static_cast<size_t>(Core::Role::Identifier)) ||
			Kind->Roles.test(static_cast<size_t>(Core::Role::CodeOpeningTag)) ||
			Kind->Roles.test(static_cast<size_t>(Core::Role::FallthroughName))
		) {
			if(!LexemeKinds.contains(Kind)) for(const std::pair<const std::string_view, Core::RoleSet> &Entry : LexemeRoles) {
				RegisterKind(Kind, Entry.first, Entry.second, LexemeKinds[Kind]);
			}
		} else if(
			const std::unordered_map<std::string_view, Core::RoleSet>::const_iterator Found = LexemeRoles.find(SymbolNames[Symbol]);
			Found != LexemeRoles.end()
			) RegisterKind(Kind, Found->first, Found->second, LexemeKinds[Kind]);
	}
	// 解決先を確認する構文だけに，標準対象の効果を加えた変更不能な種別を用意する
	for(const Core::KindDef *const Kind : KindsBySymbol) {
		if(
			BindingKinds.contains(Kind) || !Kind->Roles.test(static_cast<size_t>(Core::Role::MemberDeclCall)) &&
			!Kind->Roles.test(static_cast<size_t>(Core::Role::MemberBindingModifier))
		) continue;
		LexemeVariants &Variants = BindingKinds[Kind];
		for(const std::pair<const std::string_view, Core::RoleSet> &Entry : MemberRoles) {
			RegisterKind(Kind, Entry.first, Entry.second, Variants);
		}
	}
	const auto RegisterLexemeKind =
	[&](const Core::KindDef *const Kind, const std::string_view Lexeme, const Core::RoleSet &Extra) -> void {
		LexemeLimit = std::max(LexemeLimit, Lexeme.size());
		RegisterKind(Kind, Lexeme, Extra, LexemeKinds[Kind]);
	};
	// 静的解析用の意味は通常の名前・型の葉にも投影し，構文キーワードの適用範囲と分ける
	Core::RoleSet SemanticRoles;
	for(const std::pair<const Core::Role, std::vector<std::string_view>> &Entry : Rules.LexemeRoleSpellings) {
		if(
			Entry.first == Core::Role::TrailingOperatorToken || Entry.first == Core::Role::LeadingClauseToken || std::ranges::any_of(
				KeywordMeanings,
				[&Entry](const std::pair<Core::Role, Core::Word> &Meaning) -> bool {
					// 既存の構文キーワードとして扱う役割かの返戻
					return Entry.first == Meaning.first;
				}
			)
		) continue;
		SemanticRoles.set(static_cast<size_t>(Entry.first));
	}
	std::unordered_set<const Core::KindDef *> BoundSemantics;
	for(const Core::KindDef *const Kind : KindsBySymbol) {
		if(!BoundSemantics.insert(Kind).second) continue;
		for(const std::pair<const std::string_view, Core::RoleSet> &Entry : LexemeRoles) {
			if(const Core::RoleSet Extra = Entry.second & SemanticRoles; Extra.any()) RegisterLexemeKind(Kind, Entry.first, Extra);
		}
	}
	// フィールドを持たない中置演算も，投影で補う演算子の役割のビットを確保する
	if(FindSymbolWithRole(Core::Role::NamedInfix) || FindSymbolWithRole(Core::Role::UnfieldedOperator)) {
		ChildRoleMask |= uint64_t{ 1 } << static_cast<uint8_t>(Core::ChildRole::Operator);
	}
	// 生成する部品の比較値を採用する形式へ結付けてから，共通の種別を共有する
	Rules.TrueDef.FixedLexeme = Rules.GetSpelling(Core::Word::True);
	Rules.FalseDef.FixedLexeme = Rules.GetSpelling(Core::Word::False);
	const auto Bind = [this](Core::FormPart &Part) -> void {
		const size_t Symbol = static_cast<size_t>(Part.Def - Kinds.data());
		Part.BlankKey = BlankKeys[Symbol];
		Part.Forms = Core::CanonicalFormList(FormDefs[Symbol]);
		Part.Def = KindsBySymbol[Symbol];
		Part.Meanings = ReadWordMeanings(Part.FixedLexeme);
		if(!Part.FixedLexeme.empty()) Part.Def = &GetLexemeKindOf(*Part.Def, Part.FixedLexeme);
	};
	Core::FormPart *const FixedParts[] {
		&Reading.EmptyStmtDef,
		&Rules.DoubleQuotedDef,
		&Rules.ExprParenDef,
		&Rules.ExprStmtDef,
		&Rules.FalseDef,
		&Rules.ImplicitParamDef,
		&Rules.LoopExprDef,
		&Rules.ModifierListDef,
		&Rules.ParenthesizedStmtsDef,
		&Rules.PrefixDef,
		&Rules.PrefixExprDef,
		&Rules.PrefixUnaryDef,
		&Rules.ReturnExprDef,
		&Rules.TrueDef
	};
	for(Core::FormPart *const Part : FixedParts) if(Part->Def) {
		Part->IsNamed = Part->Def->IsNamed;
		Bind(*Part);
	}
	for(std::pair<const std::string_view, Core::FormPart> &Def : Rules.TokenDefs) {
		Bind(Def.second);
		Def.second.Meanings = ReadWordMeanings(Def.first);
		Def.second.Def = &GetLexemeKindOf(*Def.second.Def, Def.first);
		Def.second.TokenDef = Def.second.Def;
		if(Def.first.size() == 1) Rules.SingleByteTokens[static_cast<unsigned char>(Def.first.front())] = Def.second.Def;
	}
	for(Core::FormPart *const Part : FixedParts) Part->TokenDef = ReadTokenDef(Part->FixedLexeme);
	// 文法が子に出さない語を持つノードと其の語の種別（C++ の純粋仮想の `= 0` の `0`，Swift の型付の送出の `throws`）
	static constexpr std::array<std::pair<std::string_view, std::string_view>, 2> HiddenLexemes {
		{ { "pure_virtual_clause", "number_literal" }, { "throws_clause", "throws" } }
	};
	for(const std::pair<std::string_view, std::string_view> &Def : HiddenLexemes) {
		const std::string_view Host = Def.first, Lexeme = Def.second;
		if(const Core::KindDef *const Kind = Find(Lexeme, true); Kind && Find(Host, true)) {
			Reading.HiddenLexemeKinds.emplace(Host, Kind);
		}
	}
	for(std::vector<Core::CanonicalForm> &Forms : FormDefs) {
		for(Core::CanonicalForm &Form : Forms) for(Core::FormPart &Part : Form.Parts) if(Part.Def) {
			Bind(Part);
			Part.TokenDef = ReadTokenDef(Part.FixedLexeme);
		}
	}
	for(Core::CanonicalForm &Form : Rules.GeneratedForms) for(Core::FormPart &Part : Form.Parts) if(Part.Def) {
		Bind(Part);
		Part.TokenDef = ReadTokenDef(Part.FixedLexeme);
	}
	// ドキュメントが比べる種類の内容を文法毎に１度番号へ対応付ける
	std::unordered_map<std::string, std::string> DocKinds;
	const auto SetDocIdentity = [&](Core::KindDef &Kind) -> void {
		const Core::BindingDef &Binding = Kind.Bindings;
		std::string Text =
		Kind.Roles.to_string() + ":" + std::to_string(Kind.IsNamed) + ":" + std::to_string(static_cast<unsigned int>(Kind.Binding)) +
		":" + std::to_string(static_cast<unsigned int>(Binding.Namespace)) + ":" +
		std::to_string(static_cast<unsigned int>(Binding.Params)) + ":" +
		std::to_string(static_cast<unsigned int>(Binding.PatternRef)) + ":" +
		std::to_string(static_cast<unsigned int>(Binding.Constructor)) + ":" + std::to_string(Binding.IsStaticItem) + ":" +
		std::to_string(Binding.ShouldRequireImmutableConstantSrc);
		const auto [Found, Added] = DocKinds.try_emplace(std::move(Text));
		if(Added) Found->second = std::to_string(DocKinds.size());
		Kind.DocIdentity = Found->second;
		// 終了
		return;
	};
	for(Core::KindDef &Kind : SharedKinds) SetDocIdentity(Kind);
	for(Core::KindDef *const Kind : { &EmptyStmtKind, &ErrorKind, &OuterTextKind, &TrailingDataKind }) SetDocIdentity(*Kind);
	// 導出の途中にだけ使った記号毎の値を捨て，共有定義だけを残す
	std::vector<Core::KindDef>().swap(Kinds);
	// 終了
	return;
}

/**
 * 言語の文法の定義の取得関数
 * 言語と方言の組毎に１度だけ作り，呼出間で変更不能な値として共有する（整形仕様 9.4）
 * 計算量：共有済の文法に対し O(1)，初回は文法定義の構築費用
 * @param Lang 対象言語と方言
 * @return 文法の定義
 * @throws std::invalid_argument 文法を持たない言語の場合
 * @throws std::bad_alloc 文法の定義の確保に失敗した場合
 */
const GrammarProfile &GrammarProfile::Resolve(const ::Lang Lang) {
	// 文法変種の共有領域とインデックスの準備
	constexpr size_t FlagCount = 16, GoalCount = static_cast<size_t>(Lang::SrcGoal::Script) + 1;
	constexpr size_t JsoncOffset = FlagCount * GoalCount, VariantCount = JsoncOffset << 1;
	constexpr size_t Size = static_cast<size_t>(Lang::Unknown) * VariantCount;
	static_assert(!static_cast<size_t>(Lang::SrcGoal::Undetermined));
	static_assert(static_cast<size_t>(Lang::SrcGoal::Module) < GoalCount);
	static_assert((1 | 2 | 4 | 8) < FlagCount);
	static_assert(JsoncOffset + (GoalCount - 1) * FlagCount + FlagCount - 1 < VariantCount);
	static std::array<std::once_flag, Size> Flags;
	static std::array<std::unique_ptr<GrammarProfile>, Size> Profiles;
	if(Lang.Id == Lang::Unknown) throw std::invalid_argument("the language has no grammar");
	const size_t Slot = VariantCount * static_cast<size_t>(Lang.Id) + Lang.IsJsx + (Lang.IsScss << 1) + (Lang.IsCShared << 2) +
	(Lang.IsAmbiguous << 3) + static_cast<size_t>(Lang.Goal) * FlagCount + JsoncOffset * Lang.IsJsonc;
	std::call_once(
		Flags[Slot],
		[&]() -> void {
			// 共有する定義を言語と方言の組で作る
			Profiles[Slot].reset(new GrammarProfile(Lang));
		}
	);
	// 共有する文法の定義の返戻
	return *Profiles[Slot];
}

/**
 * パーサーが入力末尾で補う自動終端の判定関数
 * 計算量：O(1)
 * @param Node 判定するパーサーノード
 * @param End パーサーへ渡した入力の末尾位置
 * @return 登録した終端の記号で，入力末尾の欠落でない空の葉なら true
 */
bool GrammarProfile::IsEofTerminator(const TSNode Node, const uint32_t End) const {
	// 登録した終端の記号が入力末尾の空の葉かの返戻（他の空ノード・欠落トークンは除く）
	return EofTerminator && !ts_node_is_null(Node) && ts_node_symbol(Node) == EofTerminator && !ts_node_is_named(Node) &&
	!ts_node_is_missing(Node) && !ts_node_child_count(Node) && ts_node_start_byte(Node) == End && ts_node_end_byte(Node) == End;
}

/**
 * 記号の共通のノード種別の取得関数
 * 計算量：O(1)
 * @param Symbol 文法の記号
 * @return 種別
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
const Core::KindDef &GrammarProfile::GetKindOf(const TSSymbol Symbol) const {
	// 記号表の外の記号（パーサーのエラー領域）の種別の返戻
	if(Symbol >= KindsBySymbol.size()) return ErrorKind;
	// 種別の返戻
	return *KindsBySymbol.at(Symbol);
}

/**
 * 所属と見出から確定した関数の種別の取得関数
 * 計算量：見出の直接の子数 C と名前の長さ L に対し O(C + L)
 * @param Node 入力側の関数ノード
 * @param Name 関数名のテキスト
 * @return 文法上のコンストラクタ・型を書けないアクセサ，又は元の関数の種別
 * @throws std::bad_alloc 大小を区別しない名前の確保に失敗した場合
 * @throws std::length_error 名前の長さの上限を超える場合
 * @throws std::out_of_range 文法の記号が表に無い場合
 */
const Core::KindDef &GrammarProfile::GetDeclarationKindOf(const TSNode Node, const std::string_view Name) const {
	const Core::KindDef &Base = GetKindOf(ts_node_symbol(Node));
	const std::unordered_map<const Core::KindDef *, const Core::KindDef *>::const_iterator Constructor =
	ConstructorKinds.find(&Base);
	// 所属に依る分類を持たない関数の返戻
	if(Constructor == ConstructorKinds.end()) return Base;
	if(Lang == Lang::PHP) {
		std::string Folded(Name);
		ToLowerAscii(Folded);
		// 戻値型を書けないコンストラクタの返戻
		if(Folded == "__construct") return *Constructor->second;
		// 戻値型を書けないデストラクタの返戻
		if(Folded == "__destruct") return *ReturnlessKinds.at(&Base);
		// 型を明示出来る通常メソッドの返戻
		return Base;
	}
	bool IsStatic = false, IsGetter = false;
	for(uint32_t Idx = 0; Idx < ts_node_child_count(Node); ++Idx) {
		const TSNode Child = ts_node_child(Node, Idx);
		if(ts_node_is_named(Child)) continue;
		const std::string_view Token = GetSymbolName(ts_node_symbol(Child));
		// セッターはクラス・オブジェクト・型の何れでも戻値型を書けない
		if(Token == "set") return *ReturnlessKinds.at(&Base);
		IsStatic |= Token == "static";
		IsGetter |= Token == "get";
	}
	std::string_view Spelling = Name;
	if(Spelling.size() > 1 && (Spelling.front() == '\'' || Spelling.front() == '"') && Spelling.back() == Spelling.front()) {
		Spelling.remove_prefix(1);
		Spelling.remove_suffix(1);
	}
	// オブジェクト・interface・静的メソッド・getter は通常の戻値型を持つ
	const TSNode Parent = ts_node_parent(Node);
	if(
		!IsStatic && !IsGetter && Spelling == "constructor" && !ts_node_is_null(Parent) &&
		GetSymbolName(ts_node_symbol(Parent)) == "class_body"
	// クラス本体のコンストラクタの返戻
	) return *Constructor->second;
	// 通常の関数形の返戻
	return Base;
}

/**
 * 文脈又は語の役割を加える前の共通種別の取得関数
 * 構築時に確定した固定定義の対応を使い，投影済のテキストを照合し直さず入力側の記号対応を検査する
 * 計算量：期待値 O(1)
 * @param Kind 投影済の共通の種別（無ければ nullptr）
 * @return 語の役割を加える前の種別（追加の無い定義と nullptr は其の儘）
 */
const Core::KindDef *GrammarProfile::GetBaseKindOf(const Core::KindDef *const Kind) const {
	const std::unordered_map<const Core::KindDef *, const Core::KindDef *>::const_iterator Found = LexemeBases.find(Kind);
	// 元の種別，又は登録を持たない種別の返戻
	return Found == LexemeBases.end() ? Kind : Found->second;
}

/**
 * 解決済の標準対象の役割を含む種別の取得関数
 * 計算量：対象名の長さ L に対し期待値 O(L)
 * @param Kind 解決前の構文の種別
 * @param Target 束縛・別名から確認した対象名
 * @param Owner 所属するメンバの所有者（無ければ nullptr）
 * @return 対応する効果の種別（未登録なら元の種別）
 */
const Core::KindDef &GrammarProfile::GetBindingKindOf(
	const Core::KindDef &Kind,
	const std::string_view Target,
	const Core::KindDef *const Owner
) const {
	const std::unordered_map<std::string_view, Core::Role>::const_iterator Required = BindingOwners.find(Target);
	// 所有者の意味を確認出来ない対象の返戻
	if(Required != BindingOwners.end() && (!Owner || !Owner->Roles.test(static_cast<size_t>(Required->second)))) return Kind;
	const std::unordered_map<const Core::KindDef *, LexemeVariants>::const_iterator Variants = BindingKinds.find(&Kind);
	// 解決の対象でない種別の返戻
	if(Variants == BindingKinds.end()) return Kind;
	const LexemeVariants::const_iterator Found = Variants->second.find(Target);
	// 解決した効果の種別の返戻
	return Found == Variants->second.end() ? Kind : *Found->second;
}

/**
 * 固定の語の役割を含む種別の取得関数
 * 計算量：登録を持たない種別は O(1)，登録を持つ種別は綴りの長さ L に対し期待値 O(L)
 * @param Kind パーサーから得た共通の種別
 * @param Lexeme デコード済のトークンの綴り
 * @return 固定の語なら其の役割を加えた種別，其の他は元の種別
 */
const Core::KindDef &GrammarProfile::GetLexemeKindOf(const Core::KindDef &Kind, const std::string_view Lexeme) const {
	// 固定の綴りより長い構文の本文を読み直さない事の返戻
	if(Lexeme.size() > LexemeLimit) return Kind;
	// 固定の綴りの登録の取得
	const std::unordered_map<const Core::KindDef *, LexemeVariants>::const_iterator Variants = LexemeKinds.find(&Kind);
	// 固定の綴りの登録を持たない種別の返戻
	if(Variants == LexemeKinds.end()) return Kind;
	// 一致する綴りの役割を含む種別の取得
	const LexemeVariants::const_iterator Found = Variants->second.find(Lexeme);
	// 一致する種別，又は元の種別の返戻
	return Found == Variants->second.end() ? Kind : *Found->second;
}

/**
 * 記号の投影の判定の対象の取得関数
 * 計算量：O(1)
 * @param Symbol 文法の記号
 * @return 文法と種別から確定した判定の対象
 */
const ProjectionChecks &GrammarProfile::GetProjectionChecks(const TSSymbol Symbol) const {
	// 記号表の外はエラー領域の判定の返戻
	return Symbol < ProjectionKinds.size() ? ProjectionKinds[Symbol] : ErrorProjectionChecks;
}

/**
 * style 属性の空白と表示境界の指定の読取関数
 * 計算量：属性値の長さ L に対し O(L)
 * @param Style 属性値
 * @return 空白保持の指定と，既定の表示境界を使えない指定
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
GrammarProfile::StyleFacts GrammarProfile::ReadStyleFacts(const std::string_view Style) {
	// コメントを CSS の空白へ換え，引用符とエスケープの内部を保ったテキスト
	std::string Clean;
	Clean.reserve(Style.size());
	unsigned char Quote = '\0';
	for(size_t Pos = 0; Pos < Style.size(); ++Pos) {
		const unsigned char Char = Style[Pos];
		if(Quote) {
			Clean += Char;
			if(Char == '\\' && Pos + 1 < Style.size()) Clean += Style[++Pos];
			else if(Char == Quote) Quote = '\0';
		} else if(Char == '/' && Pos + 1 < Style.size() && Style[Pos + 1] == '*') {
			const size_t End = Style.find("*/", Pos + 2);
			// 閉じないコメント（確定出来ない指定）の返戻
			if(End == std::string_view::npos) return { true, true };
			Clean += ' ';
			Pos = End + 1;
		} else {
			if(Char == '\'' || Char == '"') Quote = Char;
			Clean += Char;
		}
	}
	// 閉じない引用符・規則のブロック（確定出来ない指定）の返戻
	if(Quote || Clean.find_first_of("{}") != std::string::npos) return { true, true };
	// 宣言の名前と値は大小文字を問わない
	ToLowerAscii(Clean);
	bool HasVal = false, IsImportant = false;
	StyleFacts Facts;
	// 宣言毎の走査（後の同順位の宣言が勝ち，重要宣言は通常の宣言より優先する）
	for(size_t Start = 0; Start < Clean.size();) {
		size_t End = Start, Colon = std::string::npos;
		uint32_t Depth = 0;
		// エスケープ・引用符・括弧の外の `:` と宣言の終わりの `;` の探索
		for(; End < Clean.size() && (Depth || Quote || Clean[End] != ';'); ++End) {
			if(const unsigned char Char = Clean[End]; Char == '\\' && End + 1 < Clean.size()) ++End;
			else if(Quote) Quote = Char == Quote ? '\0' : Quote;
			else switch(Char) {
			case '\'':
			case '"':
				Quote = Char;
				break;
			case '(':
				++Depth;
				break;
			case ')':
				// 対応しない閉括弧（確定出来ない指定）の返戻
				if(!Depth) return { true, true };
				--Depth;
				break;
			case ':':
				if(!Depth && Colon == std::string::npos) Colon = End;
				break;
			}
		}
		// 閉じない引用符・括弧（確定出来ない指定）の返戻
		if(Quote || Depth) return { true, true };
		const std::string_view Property =
		TrimCss(std::string_view(Clean).substr(Start, Colon == std::string::npos ? 0 : Colon - Start));
		// エスケープ・ネストの名前（確定出来ない指定）の返戻
		if(Property.find_first_of("\\&") != std::string_view::npos) return { true, true };
		if(Colon != std::string::npos && (Property == "white-space" || Property == "white-space-collapse")) {
			std::string_view Val = TrimCss(std::string_view(Clean).substr(Colon + 1, End - Colon - 1));
			const size_t Bang = Val.rfind('!');
			const bool IsImportantVal = Bang != std::string_view::npos && TrimCss(Val.substr(Bang + 1)) == "important";
			if(IsImportantVal) Val = TrimCss(Val.substr(0, Bang));
			// 空でない値で，先の重要宣言に負けない宣言の採用
			if(!Val.empty() && (!HasVal || IsImportantVal || !IsImportant)) {
				HasVal = true;
				IsImportant = IsImportantVal;
				Facts.ShouldPreserveWhitespace = ShouldPreserveCssWhitespace(Property, Val);
			}
		}
		// 表示の指定は親と兄弟の内容境界にも作用する為，既定の要素分類から確定しない
		if(Colon != std::string::npos && (Property == "display" || Property == "all")) Facts.HasDisplayOverride = true;
		Start = End + 1;
	}
	// 空白と表示の指定の返戻
	return Facts;
}

/**
 * 入力の読取規則の取得関数
 * 計算量：O(1)
 * @return 言語名を持たない入力の読取規則
 */
const ReadingRules &GrammarProfile::GetReading() const {
	// 入力の読取規則の返戻
	return Reading;
}

/**
 * トークン間の空白に JSX 本文固有の U+0085 と U+200B を加えた行内空白の判定関数
 * 計算量：O(1)
 * @param Point コードポイント
 * @return JSX の行端で落とす空白なら true
 */
bool ReadingRules::IsJsxSpace(const char32_t Point) const {
	// トークン間の空白又は JSX 本文固有の空白の返戻
	return IsSpace(Point) || Point == 0X85 || Point == 0X200B;
}

/**
 * 判別候補の曖昧さを除いた文法の取得関数
 * 計算量：定義済の文法に対し O(1)，初回は文法定義の構築費用
 * @return 判別を済ませた記法の文法
 * @throws std::bad_alloc 文法の定義のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 */
const GrammarProfile &GrammarProfile::GetSettledHeader() const {
	// 頭部の判別候補を除く言語設定の準備
	::Lang Settled = Lang;
	Settled.IsAmbiguous = false;
	// 確定した文法の返戻
	return Resolve(Settled);
}

/**
 * C からも読まれる昇格先の文法の取得関数
 * 計算量：定義済の文法に対し O(1)，初回は文法定義の構築費用
 * @return 共用ヘッダの C++ の文法
 * @throws std::bad_alloc 文法の定義のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 */
const GrammarProfile &GrammarProfile::GetPromotedHeader() const {
	// 頭部を読む昇格先の C++ の設定準備
	::Lang Promoted = Lang::Get(Lang::Cpp);
	Promoted.IsCShared = true;
	// 昇格先の文法の返戻
	return Resolve(Promoted);
}

/**
 * 対象言語の取得関数
 * 計算量：O(1)
 * @return 対象言語と方言
 */
::Lang GrammarProfile::GetLang() const {
	// 対象言語の返戻
	return Lang;
}

/**
 * 役割を持つ共通の種別の検索関数
 * 計算量：文法の記号数 N に対し O(N)
 * @param Wanted 探す役割
 * @return 種別（文法に無ければ無い）
 */
const Core::KindDef *GrammarProfile::FindKindWithRole(const Core::Role Wanted) const {
	// 結果の格納先
	const Core::KindDef *Result = nullptr;
	for(TSSymbol Symbol = 0; Symbol < KindsBySymbol.size(); ++Symbol) {
		if(KindsBySymbol[Symbol]->Roles.test(static_cast<size_t>(Wanted))) Result = KindsBySymbol[Symbol];
	}
	// 最後に対応した種別の返戻
	return Result;
}

/**
 * 役割を持つパーサーの記号の検索関数
 * 計算量：文法の記号数 N に対し O(N)
 * @param Wanted 探す役割
 * @return 最初に対応した記号（文法に無ければ０）
 */
TSSymbol GrammarProfile::FindSymbolWithRole(const Core::Role Wanted) const {
	// 対象範囲の走査
	for(TSSymbol Symbol = 0; Symbol < KindsBySymbol.size(); ++Symbol) {
		// 最初に対応する記号の返戻
		if(KindsBySymbol[Symbol]->Roles.test(static_cast<size_t>(Wanted))) return Symbol;
	}
	// 対応する記号が無い場合の返戻
	return 0;
}

/**
 * 記号のノード名の取得関数
 * 計算量：O(1)
 * @param Symbol 文法の記号
 * @return ノード名（記号表の外なら空）
 */
std::string_view GrammarProfile::GetSymbolName(const TSSymbol Symbol) const {
	// 記号表の内側の名前の返戻
	return Symbol < SymbolNames.size() ? SymbolNames[Symbol] : std::string_view();
}

/**
 * 記号のノード名の一致の判定関数（整形仕様 4.3，ノード名の照合を此の定義へ集める）
 * 計算量：O(1)
 * @param Left 文法の記号
 * @param Right 文法の記号
 * @return 同じノード名の記号なら true（記号表の外の記号は同じ記号だけを一致とする）
 */
bool GrammarProfile::IsSameName(const TSSymbol Left, const TSSymbol Right) const {
	// 記号表の外の記号の一致の返戻
	if(Left >= NameSymbols.size() || Right >= NameSymbols.size()) return Left == Right;
	// 同じノード名の最初の記号の一致の返戻
	return NameSymbols[Left] == NameSymbols[Right];
}

/**
 * トークンのテキストのパーサーの記号の取得関数（整形仕様 4.3，ノード名の照合を此の定義へ集める）
 * 計算量：トークン長 L に対し期待値 O(L)
 * @param Text トークンのテキスト
 * @return 同じテキストの見えるトークンの記号（文法のトークンでなければ 0）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
TSSymbol GrammarProfile::GetTokenSymbol(const std::string_view Text) const {
	// トークンのテキストに対応する文法記号の検索
	const std::unordered_map<std::string_view, TSSymbol>::const_iterator Found = TokenSymbols.find(Text);
	// トークンの記号の返戻
	return Found == TokenSymbols.end() ? 0 : Found->second;
}

/**
 * 子の役割に対応する文法の場の取得関数
 * 計算量：O(1)
 * @param Role 子の役割
 * @return 其の役割へ写る文法の場の識別子（無ければ空）
 */
std::span<const TSFieldId> GrammarProfile::GetFieldsOf(const Core::ChildRole Role) const {
	// 子の役割の表の添字の取得
	const size_t RoleIdx = static_cast<size_t>(Role);
	// 役割へ写る場の返戻
	return RoleIdx < FieldsByRole.size() ? std::span<const TSFieldId>(FieldsByRole[RoleIdx]) : std::span<const TSFieldId>();
}

/**
 * 文法に有る子の役割の取得関数
 * 計算量：O(1)
 * @return 子の役割の集合
 */
uint64_t GrammarProfile::GetChildRoleMask() const {
	return ChildRoleMask;
}

/**
 * 文法の構文・意味の固定の定義の取得関数
 * 計算量：O(1)
 * @return 言語名を持たない構文・意味の固定の定義（コメントの記法の定義を含む）
 */
const Core::SyntaxRules &GrammarProfile::GetRules() const {
	// 固定の定義の返戻
	return Rules;
}

/**
 * 記号の生成形式の取得関数
 * 計算量：O(1)
 * @param Symbol 文法の記号
 * @return 記号に使える生成形式
 */
Core::CanonicalFormList GrammarProfile::GetFormsOf(const TSSymbol Symbol) const {
	// 記号表の外には生成形式が無い事の返戻
	return Symbol < FormDefs.size() ? Core::CanonicalFormList(FormDefs[Symbol]) : Core::CanonicalFormList();
}

/**
 * パーサーのノードの記号が役割を持つかの判定関数（整形仕様 4.3，ノード名の照合を此の定義へ集める）
 * 計算量：O(1)
 * @param Node パーサーのノード（空なら偽）
 * @param Wanted 役割
 * @return ノードの記号の共通のノード種別が役割を持てば true
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
bool GrammarProfile::Is(const TSNode Node, const Core::Role Wanted) const {
	// 記号の役割の返戻
	return !ts_node_is_null(Node) && GetKindOf(ts_node_symbol(Node)).Roles.test(static_cast<size_t>(Wanted));
}

/**
 * 空行比較の値の取得関数
 * 計算量：O(1)
 * @param Symbol パーサーの記号
 * @return 公開ノード型の同異だけを表す値
 */
uint32_t GrammarProfile::GetBlankKeyOf(const TSSymbol Symbol) const {
	// 記号表の外のエラー領域と，公開ノード型の比較値の返戻
	return Symbol < BlankKeys.size() ? BlankKeys[Symbol] : ErrorBlankKey;
}

/**
 * 独立した無名の空文・空宣言の種別の取得関数
 * 計算量：O(1)
 * @return セミコロンのトークンの性質と空文の役割を持つ種別
 */
const Core::KindDef &GrammarProfile::GetEmptyStmtKind() const {
	// 空文の種別の返戻
	return EmptyStmtKind;
}

/**
 * 本体の外の地の文の種別の取得関数（整形仕様 4.4）
 * 計算量：O(1)
 * @return 投影が本体の外の地の文を逐語の子として保つ種別
 */
const Core::KindDef &GrammarProfile::GetOuterTextKind() const {
	// 種別の返戻
	return OuterTextKind;
}

/**
 * 実行停止後の逐語データの種別の取得関数
 * 計算量：O(1)
 * @return 根の最後に置くデータの種別
 */
const Core::KindDef &GrammarProfile::GetTrailingDataKind() const {
	// データの種別の返戻
	return TrailingDataKind;
}

/**
 * ドキュメント要素の属性が本文の空白を保持するかの判定関数
 * 計算量：属性名と値の総長 L に対し O(L)
 * @param AttributeName 属性名
 * @param Val 引用符を除いた属性値
 * @return 空白保持の指定又は未確定の指定なら true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool GrammarProfile::ShouldPreserveMarkupWhitespace(const std::string_view AttributeName, const std::string_view Val) const {
	// 大小文字を問わない属性名
	std::string Name(AttributeName);
	ToLowerAscii(Name);
	const bool IsPreserveSpace = IsSpelling(Name, Core::Word::PreserveSpaceAttribute);
	// 空白保持を指定しない属性の返戻
	if(!IsPreserveSpace && !IsSpelling(Name, Core::Word::InlineStyleAttribute)) return false;
	// 文字参照を含む値（デコード前のテキストで確定出来ない指定）の返戻
	if(Val.find('&') != std::string_view::npos) return true;
	// 空白保持の属性の値又は表示規則の空白保持の指定の返戻
	return IsPreserveSpace ? Val == Reading.PreserveSpaceVal : ReadStyleFacts(Val).ShouldPreserveWhitespace;
}

/**
 * 既定の表示境界を変更する属性の判定関数
 * 計算量：要素名・属性名・値の長さ L に対し O(L)
 * @param Context 要素の名前空間
 * @param Name 大小文字を揃えた要素名
 * @param AttributeName 大小文字を揃えた属性名
 * @param Val 引用符を除いた属性値
 * @return 表示の指定を持つか，指定を確定出来なければ true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool GrammarProfile::HasMarkupDisplayOverride(
	const MarkupContext Context,
	const std::string_view Name,
	const std::string_view AttributeName,
	const std::string_view Val
) const {
	// 状態に依る表示と外来内容の表示属性は，既定の表示だけでは確定しない
	if(
		Context == MarkupContext::Html && (
			AttributeName == "hidden" || AttributeName == "popover" || AttributeName == "open" && (Name == "details" || Name == "dialog") ||
			AttributeName == "type" && Name == "input"
		) || AttributeName == "display" && (Context == MarkupContext::Svg || Context == MarkupContext::MathML && Name == "math")
	) return true;
	// 表示を指定しない属性の返戻
	if(!IsSpelling(AttributeName, Core::Word::InlineStyleAttribute)) return false;
	const std::optional<std::string> Decoded = DecodeMarkupAttribute(Val);
	// デコード出来ない文字参照を含む未確定の指定の返戻
	if(!Decoded) return true;
	// 既定の表示から変わる指定の有無の返戻
	return ReadStyleFacts(*Decoded).HasDisplayOverride;
}

/**
 * ドキュメント内の表示規則が既定の空白又は表示を変え得るかの判定関数
 * 規則のブロックの区切を宣言の区切へ読み替えて宣言だけを読み，セレクタが対象とする要素は照合しない
 * 計算量：本文の長さ L に対し O(L)
 * @param Context 要素の名前空間
 * @param Name 大小文字を揃えた要素名
 * @param Content 要素の本文
 * @return 表示規則の要素が空白・表示を変える宣言，外部の規則のインポート又は確定出来ない構文を持てば true
 * @throws std::bad_alloc 本文のコピーの確保に失敗した場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool GrammarProfile::HasMarkupDisplayRules(
	const MarkupContext Context,
	const std::string_view Name,
	const std::string_view Content
) {
	// 表示規則を持つ名前空間とスタイルの要素でない場合の返戻
	if(
		Context != MarkupContext::Html && Context != MarkupContext::Svg ||
		Name != Resolve(Lang::Get(Lang::HTML)).GetReading().RawTextPlainElement
	) return false;
	std::string Decls(Content);
	ToLowerAscii(Decls);
	// 外部の規則を取り込む指定の返戻
	if(Decls.find("@import") != std::string::npos) return true;
	std::ranges::replace(Decls, '{', ';');
	std::ranges::replace(Decls, '}', ';');
	const StyleFacts Facts = ReadStyleFacts(Decls);
	// 空白の保持又は既定の表示の変更を含むかの返戻
	return Facts.ShouldPreserveWhitespace || Facts.HasDisplayOverride;
}

/**
 * 外部の表示規則を参照する属性の判定関数
 * 計算量：要素名・属性名・属性値の長さ L に対し O(L)
 * @param Context 要素の名前空間
 * @param Name 大小文字を揃えた要素名
 * @param AttributeName 大小文字を揃えた属性名
 * @param Val 引用符を除いた属性値
 * @return 外部の表示規則を参照し得れば true
 * @throws std::bad_alloc 属性名又はデコードした値の確保に失敗した場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 */
bool GrammarProfile::HasMarkupDisplayRef(
	const MarkupContext Context,
	const std::string_view Name,
	const std::string_view AttributeName,
	const std::string_view Val
) {
	// HTML の外部の表示規則の参照でない名前空間の返戻
	if(Context != MarkupContext::Html) return false;
	const ReadingRules &Rules = Resolve(Lang::Get(Lang::HTML)).GetReading();
	// 表示規則への参照でない属性の返戻
	if(Name != Rules.StyleRefElement || AttributeName != Rules.StyleRefAttribute) return false;
	std::optional<std::string> Decoded = DecodeMarkupAttribute(Val);
	// デコード出来ず用途を確定出来ない参照の返戻
	if(!Decoded) return true;
	ToLowerAscii(*Decoded);
	for(size_t Start = Decoded->find_first_not_of(" 	\n\r\f"); Start != std::string::npos;) {
		const size_t End = std::min(Decoded->find_first_of(" 	\n\r\f", Start), Decoded->size());
		// 外部の表示規則への参照が有る事の返戻
		if(std::string_view(*Decoded).substr(Start, End - Start) == Rules.StyleRefVal) return true;
		Start = Decoded->find_first_not_of(" 	\n\r\f", End);
	}
	// 外部の表示規則へ参照しない事の返戻
	return false;
}

/**
 * 親の文脈から要素の名前空間を確定する関数
 * 計算量：要素名の長さ L に対し O(L)
 * @param Parent 親の子を読む文脈
 * @param Name 大小文字を揃えた要素名
 * @return 要素を読む名前空間
 */
GrammarProfile::MarkupContext GrammarProfile::GetMarkupContext(const MarkupContext Parent, const std::string_view Name) {
	// MathML の文字の統合点で，MathML に残る開始タグの判定
	if(Parent == MarkupContext::MathMLText) {
		// MathML に残る開始タグの返戻
		if(Name == "malignmark" || Name == "mglyph") return MarkupContext::MathML;
		// HTML の開始タグとして読む事の返戻
		return GetMarkupContext(MarkupContext::Html, Name);
	}
	// 注記の統合点では SVG の開始タグだけを SVG として読む
	if(Parent == MarkupContext::MathMLAnnotation) return Name == "svg" ? MarkupContext::Svg : MarkupContext::MathML;
	// HTML の外来内容の開始又は親の名前空間の継承
	if(Parent == MarkupContext::Html) {
		// MathML の開始の返戻
		if(Name == "math") return MarkupContext::MathML;
		// SVG の開始の返戻
		if(Name == "svg") return MarkupContext::Svg;
	}
	// 親の名前空間の返戻
	return Parent;
}

/**
 * 統合点の子を読む文脈の取得関数
 * 計算量：要素名とエンコーディングの長さ L に対し O(L)
 * @param Context 要素の名前空間
 * @param Name 大小文字を揃えた要素名
 * @param Encoding デコード済の encoding 属性値
 * @return 子を読む文脈
 * @throws std::bad_alloc 属性値のコピーの確保に失敗した場合
 * @throws std::length_error 属性値の長さが保持領域の最大長を超える場合
 */
GrammarProfile::MarkupContext GrammarProfile::GetMarkupChildContext(
	const MarkupContext Context,
	const std::string_view Name,
	const std::string_view Encoding
) {
	// SVG の HTML 統合点の子は HTML として読む
	if(Context == MarkupContext::Svg && (Name == "desc" || Name == "foreignobject" || Name == "title")) return MarkupContext::Html;
	if(Context == MarkupContext::MathML) {
		// MathML の文字の統合点の子は開始タグ毎に名前空間を決める
		if(Name == "mi" || Name == "mn" || Name == "mo" || Name == "ms" || Name == "mtext") return MarkupContext::MathMLText;
		if(Name == "annotation-xml") {
			std::string Lower(Encoding);
			ToLowerAscii(Lower);
			// HTML のエンコーディングの統合点の返戻
			if(Lower == "application/xhtml+xml" || Lower == "text/html") return MarkupContext::Html;
			// SVG の開始を別に読む MathML の注記の返戻
			return MarkupContext::MathMLAnnotation;
		}
	}
	// 要素の名前空間を保つ文脈の返戻
	return Context;
}

/**
 * 名前空間に対応する表示と内容の性質の取得関数
 * 計算量：要素名長 L と固定の定義数 K に対し O(K L)
 * @param Context 要素の名前空間
 * @param Name 大小文字を揃えた要素名
 * @return 表示・逐語内容・閉方の性質の集合
 */
uint8_t GrammarProfile::GetMarkupTraits(const MarkupContext Context, const std::string_view Name) {
	// 外来内容の空白処理を HTML の表示規則で推測せず，本文の境界を保持する
	if(Context != MarkupContext::Html) return Preserve;
	// HTML のユーザーエージェントの既定の表示と要素の構文
	static constexpr std::string_view Blocks[] = {
		"address",
		"article",
		"aside",
		"blockquote",
		"body",
		"caption",
		"center",
		"dd",
		"details",
		"dir",
		"div",
		"dl",
		"dt",
		"fieldset",
		"figcaption",
		"figure",
		"footer",
		"form",
		"h1",
		"h2",
		"h3",
		"h4",
		"h5",
		"h6",
		"header",
		"hgroup",
		"hr",
		"html",
		"legend",
		"li",
		"listing",
		"main",
		"menu",
		"nav",
		"ol",
		"p",
		"plaintext",
		"pre",
		"search",
		"section",
		"summary",
		"table",
		"tbody",
		"td",
		"tfoot",
		"th",
		"thead",
		"tr",
		"ul",
		"xmp"
	};
	static constexpr std::string_view HiddenNames[] = {
		"area",
		"base",
		"basefont",
		"datalist",
		"head",
		"link",
		"meta",
		"noembed",
		"noframes",
		"param",
		"rp",
		"script",
		"style",
		"template",
		"title"
	};
	static constexpr std::string_view Voids[] =
	{ "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr" };
	static constexpr std::string_view RawNames[] = { "iframe", "noembed", "noframes", "plaintext", "textarea", "title", "xmp" };
	// 要素名に対応する表示と内容の性質の合成
	uint8_t Result = Inline;
	// open の無い dialog の既定表示（状態属性が有れば呼出側が表示を未確定とする）
	if(Name == "dialog") Result |= MarkupTrait::Hidden;
	if(std::ranges::find(Blocks, Name) != std::end(Blocks)) Result |= Block;
	if(std::ranges::find(HiddenNames, Name) != std::end(HiddenNames)) Result |= MarkupTrait::Hidden;
	if(std::ranges::find(Voids, Name) != std::end(Voids)) Result |= Void;
	if(std::ranges::find(RawNames, Name) != std::end(RawNames)) Result |= Preserve | MarkupTrait::Raw;
	if(Name == "pre" || Name == "listing") Result |= Preserve;
	if(Names::HtmlKnownTag.HasName(Name)) Result |= Scanned;
	// 性質の集合の返戻
	return Result;
}

/**
 * 入力のテキストから語の意味を確定する関数
 * 計算量：テキスト長 L に対し期待値 O(L)
 * @param Text 入力又は生成形式の固定テキスト
 * @return 文法の定義で確認出来る語の意味
 * @throws std::bad_alloc 大小を揃えるテキストを確保出来ない場合
 */
Core::WordSet GrammarProfile::ReadWordMeanings(const std::string_view Text) const {
	Core::WordSet Result;
	// 最長の定義を超える入力の未確認の意味の返戻
	if(Text.size() > WordTextLimit) return Result;
	if(
		const std::unordered_map<std::string_view, Core::WordSet>::const_iterator Found = WordMeanings.find(Text);
		Found != WordMeanings.end()
	) Result = Found->second;
	if(!FoldedWordMeanings.empty()) {
		std::string Folded(Text);
		for(char &Char : Folded) Char = static_cast<char>(std::tolower(static_cast<unsigned char>(Char)));
		if(
			const std::unordered_map<std::string, Core::WordSet>::const_iterator Found = FoldedWordMeanings.find(Folded);
			Found != FoldedWordMeanings.end()
		) Result |= Found->second;
	}
	// 入力定義から確定した意味の返戻
	return Result;
}

/**
 * 語の最大長の取得関数
 * @return 入力の語として確認する最大バイト数
 */
size_t GrammarProfile::GetWordTextLimit() const {
	return WordTextLimit;
}

/**
 * 入力の語の意味の判定関数
 * @param Text 入力の名前又はトークン
 * @param Meaning 確認する語の意味
 * @return 文法で定める語なら真
 */
bool GrammarProfile::IsSpelling(const std::string_view Text, const Core::Word Meaning) const {
	return ReadWordMeanings(Text).test(static_cast<size_t>(Meaning));
}

/**
 * 戻値型の供給元を確認する関数
 * @param Text 戻値型の名前
 * @return 確認出来た無値・非復帰の型の意味
 */
Core::WordSet GrammarProfile::ReadResultMeanings(const std::string_view Text) const {
	Core::WordSet Result;
	const size_t Separator = Text.rfind('.');
	// 未登録の供給元の未確認の型の返戻
	if(Separator != std::string_view::npos && !IsSpelling(Text, Core::Word::QualifiedResultType)) return Result;
	const Core::WordSet Tail = ReadWordMeanings(Text.substr(Separator + 1));
	for(const Core::Word Meaning : { Core::Word::VoidType, Core::Word::AsyncVoidType, Core::Word::NeverType }) {
		if(Tail.test(static_cast<size_t>(Meaning))) Result.set(static_cast<size_t>(Meaning));
	}
	// 標準の供給元を確認した戻値型の返戻
	return Result;
}

/**
 * 入力トークンの役割を確定する関数
 * 計算量：トークン長 L に対し期待値 O(L)
 * @param Text 入力又は生成形式の固定トークン
 * @return 対応する固定のトークンの定義（未確認なら無）
 */
const Core::KindDef *GrammarProfile::ReadTokenDef(const std::string_view Text) const {
	const std::unordered_map<std::string_view, Core::FormPart>::const_iterator Found = Rules.TokenDefs.find(Text);
	// 入力のトークンへ対応する固定定義の返戻
	return Found == Rules.TokenDefs.end() ? nullptr : Found->second.Def;
}
