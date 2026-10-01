#pragma once

#include "../Core/SyntaxRole.hpp"
#include "../Util/Lang.hpp"
#include "ReadingRules.hpp"
#include <tree_sitter/api.h>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

// 位置を実行時の値として観測する構文（整形仕様 4.4，名前は節点の最初の名前付の子，無ければ節点の字面の綴りを区切りの位置で末尾と照合する）
struct SourceObservation {
	TSSymbol Symbol; // 観測する構文の記号
	std::string_view Name; // 名前の綴り
	bool IsCaseInsensitive = false; // 綴りの大小を区別しないか
	bool ObservesColumn = false; // 桁位置も観測するか
	std::optional<TSSymbol> CallerSymbol; // 呼出側の位置を観測する仮引数の属性なら観測する呼出の記号（其の他は無し）
};

// 文法の定義クラス（整形仕様 3.1, 4.3，文法・投影対応・生成形式を持ち，入力側と検証側だけが使う）
class GrammarProfile {
private:

	struct Names; // 文法の節点名の表（此の定義だけが節点名を役割・記法へ対応付ける，整形仕様 4.3）
	Lang Language; // 対象言語と方言
	std::vector<Core::KindDefinition> Kinds; // 記号毎の種別の導出値
	std::vector<std::string_view> SymbolNames; // 文法の記号毎の節点名
	std::vector<TSSymbol> NameSymbols; // 文法の記号毎の同じ節点名の最初の記号
	std::unordered_map<std::string_view, TSSymbol> TokenSymbols; // 字句の字面毎の解析器の記号
	std::vector<SourceObservation> Observations; // 位置を観測する構文の記号と綴り
	std::deque<Core::KindDefinition> SharedKinds; // 同じ性質の記号が共有する種別の定義
	std::vector<const Core::KindDefinition *> KindsBySymbol; // 文法の記号から共通の種別への対応
	std::vector<Core::ChildRole> FieldRoles; // 文法の場の識別子毎の共通の子の役割
	std::vector<std::vector<TSFieldId>> FieldsByRole; // 子の役割毎の文法の場の識別子
	uint64_t ChildRoleMask = 0; // 文法に在る子の役割
	Core::SyntaxRules Rules; // 文法の構文・意味の固定の定義（コメントの記法の定義を含む）
	ReadingRules Reading; // 入力の読取と投影の規則
	std::vector<std::vector<Core::CanonicalForm>> FormDefinitions; // 文法の記号毎の生成形式の定義
	Core::KindDefinition ErrorKind; // 解析器の誤り区画の種別（文法の記号表の外の記号）
	Core::KindDefinition OuterTextKind; // 本体の外の地の文の種別（文法の記号表の外の記号）
	Core::KindDefinition TrailingDataKind; // 実行停止後の逐語データの種別（文法の記号表の外の記号）
	static Core::ChildRole CommonChildRole(const Core::ChildRole Role); // 入力側だけが区別する場の役割の中核の子の役割への対応関数
	static std::span<const std::string_view> StringPrefixes(const Lang Language); // 文字列の開きの前の接頭辞の取得関数
	static Core::RoleSet DeriveRoles(const Lang Language, const std::string_view Name, const bool IsNamed); // 節点名の役割の取得関数
	const Core::KindDefinition *Find(const std::string_view Name, const bool IsNamed) const; // 節点名の種別の検索関数
	void AttachForms(); // 生成形式の付与関数

	explicit GrammarProfile(const Lang Language); // コンストラクタ

public:

	enum MarkupTrait : uint8_t {
		Inline = 0,
		Block = 1,
		Hidden = 2,
		Preserve = 4,
		Raw = 8,
		Void = 16,
		Foreign = 32,
		Text = 64,
		Scanned = 128 // 解析器の走査器が種別の１バイトで持つ名前
	}; // 表示の境界と逐語内容を決める要素の性質

	bool PreservesMarkupWhitespace(std::string_view AttributeName, std::string_view Value) const; // 属性が本文の空白を保持するかの判定関数
	static uint8_t MarkupTraits(const std::string_view Name); // 要素名の表示と内容の性質の取得関数
	static const GrammarProfile &Resolve(const Lang Language); // 言語の文法の定義の取得関数
	Lang GetLanguage() const; // 対象言語の取得関数
	const Core::KindDefinition *KindWithRole(const Core::Role Wanted) const; // 役割を持つ共通の種別の検索関数
	TSSymbol SymbolWithRole(const Core::Role Wanted) const; // 役割を持つ解析器の記号の検索関数
	std::string_view SymbolName(const TSSymbol Symbol) const; // 記号の節点名の取得関数
	bool IsSameName(const TSSymbol Left, const TSSymbol Right) const; // 記号の節点名の一致の判定関数
	TSSymbol TokenSymbol(const std::string_view Text) const; // 字句の字面の解析器の記号の取得関数
	Core::ChildRole ChildRoleOf(const TSFieldId Field) const; // 文法の場に対応する子の役割の取得関数
	uint64_t FieldBits(const TSFieldId Field) const; // 文法の場に対応する中核の子の役割の印の取得関数
	std::span<const TSFieldId> FieldsOf(const Core::ChildRole Role) const; // 子の役割に対応する文法の場の取得関数
	uint64_t GetChildRoleMask() const; // 文法に在る子の役割の取得関数
	const Core::SyntaxRules &GetRules() const; // 文法の構文・意味の固定の定義の取得関数
	const ReadingRules &GetReading() const; // 入力の読取規則の取得関数
	Core::CanonicalFormList FormsOf(const TSSymbol Symbol) const; // 記号の生成形式の取得関数
	const Core::KindDefinition &KindOf(const TSSymbol Symbol) const; // 記号の共通の節点種別の取得関数
	bool Is(const TSNode Node, const Core::Role Wanted) const; // 解析器の節点の記号が役割を持つかの判定関数
	const Core::KindDefinition &GetOuterTextKind() const; // 本体の外の地の文の種別の取得関数
	const Core::KindDefinition &GetTrailingDataKind() const; // 実行停止後の逐語データの種別の取得関数
	std::span<const SourceObservation> GetObservations() const; // 位置を実行時の値として観測する構文の取得関数
};
