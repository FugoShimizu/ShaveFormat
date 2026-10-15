#pragma once

#include "ReadingRules.hpp"
#include "../Core/CommentRepository.hpp"
#include "../Core/SyntaxRole.hpp"
#include "../Util/Lang.hpp"
#include <tree_sitter/api.h>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// 文法と種別から１度だけ確定する投影の読違判定の対象
struct ProjectionChecks {
	bool CanReadContextualType = false; // 文脈と型の束縛で推論指定を区別するか
	bool CanReadGenericCall = false; // 比較と誤読したジェネリック呼出の候補か
	bool CanReadTemplateDecl = false; // 比較と誤読した型引数付宣言の候補か
	bool HasTemplateContext = false; // 山括弧又は其の先頭の括弧の控えを持ち得るか
	bool IsLiteralText = false; // リテラルの本文・エスケープ・終端の部品か
	bool IsLiteralBody = false; // 本文と補間を子に並べる領域か
};

// 文法の定義クラス（整形仕様 3.1, 4.3，文法・投影対応・生成形式を持ち，入力側と検証側だけが使う）
class GrammarProfile {
private:

	using DocumentForms = std::vector<Core::DocumentNotation>; // 言語毎のドキュメント記法と本文形式
	using LexemeVariants = std::unordered_map<std::string_view, const Core::KindDef *>; // 固定の綴り毎の種別の定義

	// 属性から確定する表示の指定
	struct StyleFacts {
		bool ShouldPreserveWhitespace = false; // 空白又は改行を保持する指定か
		bool HasDisplayOverride = false; // 既定の要素の表示を変更する指定か
	};

	struct Names; // 文法のノード名の表（此の定義だけがノード名を役割・記法へ対応付ける，整形仕様 4.3）
	::Lang Lang; // 対象言語と方言
	std::vector<Core::KindDef> Kinds; // 記号毎の種別の導出値
	std::vector<ProjectionChecks> ProjectionKinds; // 記号毎の投影の判定の対象（文法の構築後は読取専用）
	ProjectionChecks ErrorProjectionChecks; // 記号表の外のエラー領域は読違の判定の対象外
	std::vector<uint32_t> BlankKeys; // 記号毎の等値比較専用の空行比較の値
	uint32_t ErrorBlankKey = 0; // エラー領域の空行比較の値
	std::vector<std::string_view> SymbolNames; // 文法の記号毎のノード名
	TSSymbol EofTerminator = 0; // パーサーが入力末尾で補う自動終端の記号（未登録は０）
	std::vector<TSSymbol> NameSymbols; // 文法の記号毎の同じノード名の最初の記号
	std::unordered_map<std::string_view, TSSymbol> TokenSymbols; // トークンのテキスト毎のパーサーの記号
	std::deque<Core::KindDef> SharedKinds; // 同じ性質の記号が共有する種別の定義
	std::unordered_map<const Core::KindDef *, const Core::KindDef *> LexemeBases; // 文脈又は語の役割を加えた固定定義から基底の種別への対応
	std::unordered_map<std::string_view, Core::Role> BindingOwners; // 標準対象の効果を確認する所有者の役割
	std::unordered_map<const Core::KindDef *, LexemeVariants> BindingKinds; // 確認済の標準対象の効果を加えた種別
	std::unordered_map<std::string_view, Core::WordSet> WordMeanings; // 入力の語から確定する意味
	std::unordered_map<std::string, Core::WordSet> FoldedWordMeanings; // 大小を区別しない語の意味
	size_t WordTextLimit = 0; // 語として読むテキストの最大長
	std::unordered_map<const Core::KindDef *, LexemeVariants> LexemeKinds; // 固定の綴りに依るトークンの種別
	std::unordered_map<std::string_view, uint64_t> AccessSymbols; // アクセス修飾子の語から意味の同一性への投影
	size_t AccessLimit = 0; // アクセス修飾の登録の最長値
	size_t LexemeLimit = 0; // 固定の意味を持つトークンの最長の綴り
	std::unordered_map<const Core::KindDef *, const Core::KindDef *> ConstructorKinds; // 所属から確定したコンストラクタの種別
	std::unordered_map<const Core::KindDef *, const Core::KindDef *> ReturnlessKinds; // 戻値型を書けないアクセサ・特殊メンバの種別
	std::vector<const Core::KindDef *> KindsBySymbol; // 文法の記号から共通の種別への対応
	std::vector<Core::ChildRole> FieldRoles; // 文法の場の識別子毎の共通の子の役割
	std::vector<std::vector<TSFieldId>> FieldsByRole; // 子の役割毎の文法の場の識別子
	uint64_t ChildRoleMask = 0; // 文法に有る子の役割
	Core::SyntaxRules Rules; // 文法の構文・意味の固定の定義（コメントの記法の定義を含む）
	ReadingRules Reading; // 入力の読取と投影の規則
	std::vector<std::vector<Core::CanonicalForm>> FormDefs; // 文法の記号毎の生成形式の定義
	Core::KindDef EmptyStmtKind; // 独立したセミコロンに文の役割を付ける投影用の種別
	Core::KindDef ErrorKind; // パーサーのエラー領域の種別（文法の記号表の外の記号）
	Core::KindDef OuterTextKind; // 本体の外の地の文の種別（文法の記号表の外の記号）
	Core::KindDef TrailingDataKind; // 実行停止後の逐語データの種別（文法の記号表の外の記号）
	static std::string_view TrimCss(std::string_view Text); // CSS の空白の前後の除去関数
	static bool ShouldPreserveCssWhitespace(const std::string_view Property, const std::string_view Val); // CSS の空白保持の指定の判定関数
	static std::span<const std::string_view> GetStrPrefixes(const ::Lang Lang); // 文字列の開きの前の接頭辞の取得関数
	static StyleFacts ReadStyleFacts(const std::string_view Style); // 属性から表示の指定を読む関数
	static DocumentForms GetDocFormsOf(const ::Lang Lang); // ドキュメント記法と本文形式の取得関数
	static Core::ChildRole CommonChildRole(const Core::ChildRole Role); // 入力側だけが区別する場の役割の中核の子の役割への対応関数
	static Core::RoleSet DeriveRoles(const ::Lang Lang, const std::string_view Name, const bool IsNamed); // ノード名の役割の取得関数
	static Core::BindingSyntax GetBindingSyntax(const ::Lang Lang, const std::string_view Name, const Core::RoleSet &Roles); // 束縛の共通構造の固定定義の取得関数
	static Core::EvaluationDef GetEvaluationDef(const ::Lang Lang, const std::string_view Name, const Core::RoleSet &Roles); // 子の評価時機の固定契約の取得関数
	static Core::BindingDef GetBindingDef(const ::Lang Lang, const std::string_view Name); // 名前空間・暗黙仮引数・コンストラクタの固定契約の取得関数
	const Core::KindDef *Find(const std::string_view Name, const bool IsNamed) const; // ノード名の種別の検索関数
	void AttachForms(); // 生成形式の付与関数

	explicit GrammarProfile(const ::Lang Lang); // コンストラクタ

public:

	enum class MarkupContext : uint8_t { Html, MathML, MathMLAnnotation, MathMLText, Svg }; // 要素を読む名前空間と統合点の文脈
	enum MarkupTrait : uint8_t { Inline = 0, Block = 1, Hidden = 2, Preserve = 4, Raw = 8, Scanned = 128, Void = 16 }; // 表示の境界と逐語内容を決める要素の性質 // パーサーのスキャナーが種別の１バイトで持つ名前
	static bool IsBuiltinCastType(std::string_view Type); // 明示キャストの組込型の読取関数
	static const GrammarProfile &Resolve(const ::Lang Lang); // 言語の文法の定義の取得関数
	static bool HasMarkupDisplayRules(const MarkupContext Context, const std::string_view Name, const std::string_view Content); // ドキュメント内の表示規則が既定の空白又は表示を変え得るかの判定関数
	static std::optional<std::string> DecodeMarkupAttribute(const std::string_view Val); // 属性値の ASCII 文字参照の読取用デコード関数

	static bool HasMarkupDisplayRef(
		const MarkupContext Context,
		const std::string_view Name,
		const std::string_view AttributeName,
		const std::string_view Val
	); // 外部の表示規則を参照する属性の判定関数

	static MarkupContext GetMarkupContext(const MarkupContext Parent, const std::string_view Name); // 親の文脈から要素の名前空間を確定する関数

	static MarkupContext GetMarkupChildContext(
		const MarkupContext Context,
		const std::string_view Name,
		const std::string_view Encoding
	); // 統合点の子を読む文脈の取得関数

	static uint8_t GetMarkupTraits(const MarkupContext Context, const std::string_view Name); // 名前空間に対応する表示と内容の性質の取得関数
	const ReadingRules &GetReading() const; // 入力の読取規則の取得関数
	Core::ChildRole GetChildRoleOf(const std::string_view Name) const; // 文法の場の名前に対応する子の役割の取得関数
	Core::ChildRole GetChildRoleOf(const TSFieldId Field) const; // 文法の場に対応する子の役割の取得関数
	const Core::KindDef *GetBaseKindOf(const Core::KindDef *const Kind) const; // 文脈又は語の役割を加える前の共通種別の取得関数
	const Core::KindDef &GetKindOf(const TSSymbol Symbol) const; // 記号の共通のノード種別の取得関数
	const Core::KindDef &GetDeclarationKindOf(const TSNode Node, const std::string_view Name) const; // 所属と見出から確定した関数の種別の取得関数

	const Core::KindDef &GetBindingKindOf(
		const Core::KindDef &Kind,
		const std::string_view Target,
		const Core::KindDef *const Owner = nullptr
	) const; // 解決済の標準対象の役割を含む種別の取得関数

	const Core::KindDef &GetLexemeKindOf(const Core::KindDef &Kind, const std::string_view Lexeme) const; // 固定の語の役割を含む種別の取得関数
	const ProjectionChecks &GetProjectionChecks(const TSSymbol Symbol) const; // 記号の投影の判定の対象の取得関数
	const GrammarProfile &GetSettledHeader() const; // 判別候補の曖昧さを除いた文法の取得関数
	const GrammarProfile &GetPromotedHeader() const; // C からも読まれる昇格先の文法の取得関数
	Core::AccessMeaning ReadAccessMeaning(const std::string_view Text) const; // アクセス修飾の意味と長さの読取関数
	Core::NumericLiteralKind ReadNumericLiteral(const std::string_view Text) const; // 数値の基数・全桁・型接尾辞の分類関数

	Core::ConstantTruth GetLiteralTruth(
		const std::string_view Text,
		const Core::RoleSet &Roles,
		Core::StructureWorkAccount *const Work = nullptr
	) const; // 言語の真偽変換で確定するリテラルの真偽の取得関数

	bool IsSpelling(const std::string_view Text, const Core::Word Meaning) const; // 入力の語の意味の判定関数
	Core::WordSet ReadResultMeanings(const std::string_view Text) const; // 戻値型の供給元を確認する関数
	const Core::KindDef *ReadTokenDef(const std::string_view Text) const; // 入力トークンの役割を確定する関数
	Core::WordSet ReadWordMeanings(const std::string_view Text) const; // 入力のテキストから語の意味を確定する関数
	size_t GetWordTextLimit() const; // 語の最大長の取得関数
	const Core::SyntaxRules &GetRules() const; // 文法の構文・意味の固定の定義の取得関数
	const Core::KindDef &GetEmptyStmtKind() const; // 独立した無名の空文・空宣言の種別の取得関数
	const Core::KindDef &GetOuterTextKind() const; // 本体の外の地の文の種別の取得関数
	const Core::KindDef &GetTrailingDataKind() const; // 実行停止後の逐語データの種別の取得関数
	bool ShouldPreserveMarkupWhitespace(const std::string_view AttributeName, const std::string_view Val) const; // 本文空白保持の判定関数

	bool HasMarkupDisplayOverride(
		const MarkupContext Context,
		const std::string_view Name,
		const std::string_view AttributeName,
		const std::string_view Val
	) const; // 既定の表示を変更する属性の判定関数

	::Lang GetLang() const; // 対象言語の取得関数
	const Core::KindDef *FindKindWithRole(const Core::Role Wanted) const; // 役割を持つ共通の種別の検索関数
	TSSymbol FindSymbolWithRole(const Core::Role Wanted) const; // 役割を持つパーサーの記号の検索関数
	std::string_view GetSymbolName(const TSSymbol Symbol) const; // 記号のノード名の取得関数
	bool IsSameName(const TSSymbol Left, const TSSymbol Right) const; // 記号のノード名の一致の判定関数
	TSSymbol GetTokenSymbol(const std::string_view Text) const; // トークンのテキストのパーサーの記号の取得関数
	uint64_t GetFieldBits(const Core::ChildRole Role) const; // 文法に在る場の役割の中核のマーカーの取得関数
	uint64_t GetFieldBits(const TSFieldId Field) const; // 文法の場に対応する中核の子の役割のマーカーの取得関数
	std::span<const TSFieldId> GetFieldsOf(const Core::ChildRole Role) const; // 子の役割に対応する文法の場の取得関数
	uint64_t GetChildRoleMask() const; // 文法に有る子の役割の取得関数
	bool IsEofTerminator(const TSNode Node, const uint32_t End) const; // 登録した自動終端が入力末尾の空の葉かの判定関数
	Core::CanonicalFormList GetFormsOf(const TSSymbol Symbol) const; // 記号の生成形式の取得関数
	uint32_t GetBlankKeyOf(const TSSymbol Symbol) const; // 記号に対応する空行比較の値の取得関数
	bool Is(const TSNode Node, const Core::Role Wanted) const; // パーサーのノードの記号が役割を持つかの判定関数
};
