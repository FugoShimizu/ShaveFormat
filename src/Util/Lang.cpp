#include "Lang.hpp"
#include <algorithm>
#include <cctype>
#include <string_view>
#include <unordered_map>

extern "C" {
	const TSLanguage *tree_sitter_c();
	const TSLanguage *tree_sitter_cpp();
	const TSLanguage *tree_sitter_c_sharp();
	const TSLanguage *tree_sitter_java();
	const TSLanguage *tree_sitter_go();
	const TSLanguage *tree_sitter_rust();
	const TSLanguage *tree_sitter_kotlin();
	const TSLanguage *tree_sitter_swift();
	const TSLanguage *tree_sitter_ruby();
	const TSLanguage *tree_sitter_python();
	const TSLanguage *tree_sitter_javascript();
	const TSLanguage *tree_sitter_tsx();
	const TSLanguage *tree_sitter_typescript();
	const TSLanguage *tree_sitter_json();
	const TSLanguage *tree_sitter_php();
	const TSLanguage *tree_sitter_html();
	const TSLanguage *tree_sitter_scss();
}

/**
 * tree-sitter 言語定義取得関数
 * @return 対応する TSLanguage ポインタ（未対応言語では nullptr）
 */
const TSLanguage *Lang::TsLang() const {
	// 各識別子をリンク済の文法定義へ結び，解析器の生成側へ所有権を渡さない
	switch(Id) {
	case Lang::C:
		return tree_sitter_c();
	case Lang::Cpp:
		return tree_sitter_cpp();
	case Lang::CSharp:
		return tree_sitter_c_sharp();
	case Lang::Java:
		return tree_sitter_java();
	case Lang::Go:
		return tree_sitter_go();
	case Lang::Rust:
		return tree_sitter_rust();
	case Lang::Kotlin:
		return tree_sitter_kotlin();
	case Lang::Swift:
		return tree_sitter_swift();
	case Lang::Ruby:
		return tree_sitter_ruby();
	case Lang::Python:
		return tree_sitter_python();
	case Lang::JavaScript:
		// JSX の有無は同じ言語識別子の付加情報で文法だけを切り替える
		return tree_sitter_javascript();
	case Lang::TypeScript:
		return IsJsx ? tree_sitter_tsx() : tree_sitter_typescript();
	case Lang::PHP:
		return tree_sitter_php();
	case Lang::JSON:
		return tree_sitter_json();
	case Lang::HTML:
		return tree_sitter_html();
	case Lang::CSS:
		return tree_sitter_scss();
	default:
		return nullptr;
	}
}

/**
 * ASCII 小文字化関数（拡張子・ファイル名比較の共通前処理，其の場変換）
 * @param Text 変換対象の文字列
 */
void Lang::ToLowerAscii(std::string &Text) {
	// 負値に為る非 ASCII バイトを `tolower` へ直接渡すと未定義動作の為，符号無へ広げてから渡す
	std::transform(
		Text.begin(),
		Text.end(),
		Text.begin(),
		[](const char Char) -> char {
			// 対象文字の処理
			return static_cast<char>(std::tolower(static_cast<unsigned char>(Char)));
		}
	);
	// 終了
	return;
}

/**
 * 言語種別生成関数（ID から Lang を取得）
 * @param Id 言語 ID
 * @return 対応する Lang（範囲外は Unknown）
 */
Lang Lang::Get(const ID Id) {
	// 符号無型へのキャストで負値も１回の比較で範囲外判定する事の返戻（負値はラップアラウンドして N 以上になる）
	return { static_cast<size_t>(Id) <= static_cast<size_t>(Unknown) ? Id : Unknown };
}

/**
 * 言語名判定関数（VSCode の languageId を其のまま受付）
 * @param Name 言語名 (VSCode languageId)
 * @return 対応する `Lang`（未知の場合 `Lang::Unknown`）
 */
Lang Lang::FromName(const std::string_view Name) {
	// 静的な表を共有し，呼出毎の別名集合の再構築を避ける
	static const std::unordered_map<std::string_view, Lang::ID> Map = {
		{ "c", Lang::C },
		{ "cpp", Lang::Cpp },
		{ "csharp", Lang::CSharp },
		{ "java", Lang::Java },
		{ "go", Lang::Go },
		{ "rust", Lang::Rust },
		{ "kotlin", Lang::Kotlin },
		{ "swift", Lang::Swift },
		{ "ruby", Lang::Ruby },
		{ "python", Lang::Python },
		{ "javascript", Lang::JavaScript },
		{ "javascriptreact", Lang::JavaScript },
		{ "typescript", Lang::TypeScript },
		{ "typescriptreact", Lang::TypeScript },
		{ "php", Lang::PHP },
		{ "json", Lang::JSON },
		{ "jsonc", Lang::JSON },
		{ "html", Lang::HTML },
		{ "css", Lang::CSS },
		{ "scss", Lang::CSS },
		{ "h", Lang::C },
		{ "cc", Lang::Cpp },
		{ "cxx", Lang::Cpp },
		{ "c++", Lang::Cpp },
		{ "hpp", Lang::Cpp },
		{ "cs", Lang::CSharp },
		{ "kt", Lang::Kotlin },
		{ "rs", Lang::Rust },
		{ "rb", Lang::Ruby },
		{ "py", Lang::Python },
		{ "js", Lang::JavaScript },
		{ "jsx", Lang::JavaScript },
		{ "mjs", Lang::JavaScript },
		{ "cjs", Lang::JavaScript },
		{ "ts", Lang::TypeScript },
		{ "tsx", Lang::TypeScript },
		{ "htm", Lang::HTML }
	};
	// 編集器や作業手順書は `TypeScript` / `C` の様に大文字を混ぜる為，照合前に小文字へ揃える
	std::string Key(Name);
	ToLowerAscii(Key);
	const std::unordered_map<std::string_view, Lang::ID>::const_iterator Iter = Map.find(Key);
	Lang Result = Lang::Get(Iter != Map.end() ? Iter->second : Lang::Unknown);
	Result.IsJsx = Key == "typescriptreact" || Key == "tsx";
	Result.IsScss = Key == "scss";
	// 言語 ID 名から Lang の返戻（見付からなければ Unknown）
	return Result;
}

/**
 * 拡張子取得関数（小文字化済）
 * @param Path ファイルパス
 * @return 拡張子（ドット付，小文字）
 */
std::string Lang::GetExtension(const std::string &Path) {
	// 経路中の拡張子境界の探索
	const size_t Pos = Path.rfind('.');
	// ドットが無い場合の返戻
	if(Pos == std::string::npos) return "";
	std::string Ext = Path.substr(Pos);
	ToLowerAscii(Ext);
	// 小文字化した拡張子の返戻
	return Ext;
}

/**
 * パスからの言語判定関数（拡張子・ファイル名から検出）
 * @param Path ファイルパス
 * @return 検出された `Lang`（未知の場合 `Lang::Unknown`）
 */
Lang Lang::Detect(const std::string &Path) {
	// VSCode と同じくファイル名を小文字化して完全一致判定
	static const std::unordered_map<std::string, Lang::ID> NameMap = {
		{ "rakefile", Lang::Ruby },
		{ "gemfile", Lang::Ruby },
		{ "guardfile", Lang::Ruby },
		{ "podfile", Lang::Ruby },
		{ "capfile", Lang::Ruby },
		{ "cheffile", Lang::Ruby },
		{ "hobofile", Lang::Ruby },
		{ "vagrantfile", Lang::Ruby },
		{ "appraisals", Lang::Ruby },
		{ "rantfile", Lang::Ruby },
		{ "berksfile", Lang::Ruby },
		{ "berksfile.lock", Lang::Ruby },
		{ "thorfile", Lang::Ruby },
		{ "puppetfile", Lang::Ruby },
		{ "dangerfile", Lang::Ruby },
		{ "brewfile", Lang::Ruby },
		{ "fastfile", Lang::Ruby },
		{ "appfile", Lang::Ruby },
		{ "deliverfile", Lang::Ruby },
		{ "matchfile", Lang::Ruby },
		{ "scanfile", Lang::Ruby },
		{ "snapfile", Lang::Ruby },
		{ "gymfile", Lang::Ruby },
		{ "snakefile", Lang::Python },
		{ "sconstruct", Lang::Python },
		{ "sconscript", Lang::Python },
		{ "jakefile", Lang::JavaScript },
		{ "composer.lock", Lang::JSON },
		{ "bun.lock", Lang::JSON },
		{ ".watchmanconfig", Lang::JSON },
		{ ".ember-cli", Lang::JSON }
	};
	static const std::unordered_map<std::string, Lang::ID> ExtMap = {
		{ ".c", Lang::C },
		{ ".h", Lang::C },
		{ ".i", Lang::C },
		{ ".cpp", Lang::Cpp },
		{ ".cppm", Lang::Cpp },
		{ ".cc", Lang::Cpp },
		{ ".ccm", Lang::Cpp },
		{ ".cxx", Lang::Cpp },
		{ ".cxxm", Lang::Cpp },
		{ ".c++", Lang::Cpp },
		{ ".c++m", Lang::Cpp },
		{ ".hpp", Lang::Cpp },
		{ ".hh", Lang::Cpp },
		{ ".hxx", Lang::Cpp },
		{ ".h++", Lang::Cpp },
		{ ".ii", Lang::Cpp },
		{ ".ino", Lang::Cpp },
		{ ".inl", Lang::Cpp },
		{ ".ipp", Lang::Cpp },
		{ ".ixx", Lang::Cpp },
		{ ".tpp", Lang::Cpp },
		{ ".txx", Lang::Cpp },
		{ ".cs", Lang::CSharp },
		{ ".csx", Lang::CSharp },
		{ ".cake", Lang::CSharp },
		{ ".java", Lang::Java },
		{ ".jav", Lang::Java },
		{ ".go", Lang::Go },
		{ ".rs", Lang::Rust },
		{ ".kt", Lang::Kotlin },
		{ ".kts", Lang::Kotlin },
		{ ".ktm", Lang::Kotlin },
		{ ".swift", Lang::Swift },
		{ ".php", Lang::PHP },
		{ ".php3", Lang::PHP },
		{ ".php4", Lang::PHP },
		{ ".php5", Lang::PHP },
		{ ".phtml", Lang::PHP },
		{ ".phps", Lang::PHP },
		{ ".rb", Lang::Ruby },
		{ ".rbw", Lang::Ruby },
		{ ".rbx", Lang::Ruby },
		{ ".rjs", Lang::Ruby },
		{ ".rbi", Lang::Ruby },
		{ ".gemspec", Lang::Ruby },
		{ ".rake", Lang::Ruby },
		{ ".ru", Lang::Ruby },
		{ ".podspec", Lang::Ruby },
		{ ".py", Lang::Python },
		{ ".pyw", Lang::Python },
		{ ".pyi", Lang::Python },
		{ ".cpy", Lang::Python },
		{ ".gyp", Lang::Python },
		{ ".gypi", Lang::Python },
		{ ".ipy", Lang::Python },
		{ ".pyt", Lang::Python },
		{ ".rpy", Lang::Python },
		{ ".js", Lang::JavaScript },
		{ ".jsx", Lang::JavaScript },
		{ ".mjs", Lang::JavaScript },
		{ ".cjs", Lang::JavaScript },
		{ ".es6", Lang::JavaScript },
		{ ".pac", Lang::JavaScript },
		{ ".ts", Lang::TypeScript },
		{ ".tsx", Lang::TypeScript },
		{ ".cts", Lang::TypeScript },
		{ ".mts", Lang::TypeScript },
		{ ".json", Lang::JSON },
		{ ".jsonc", Lang::JSON },
		{ ".jsonld", Lang::JSON },
		{ ".geojson", Lang::JSON },
		{ ".webmanifest", Lang::JSON },
		{ ".bowerrc", Lang::JSON },
		{ ".jscsrc", Lang::JSON },
		{ ".jslintrc", Lang::JSON },
		{ ".jshintrc", Lang::JSON },
		{ ".jsfmtrc", Lang::JSON },
		{ ".eslintrc", Lang::JSON },
		{ ".babelrc", Lang::JSON },
		{ ".swcrc", Lang::JSON },
		{ ".hintrc", Lang::JSON },
		{ ".vuerc", Lang::JSON },
		{ ".html", Lang::HTML },
		{ ".htm", Lang::HTML },
		{ ".shtml", Lang::HTML },
		{ ".xhtml", Lang::HTML },
		{ ".xht", Lang::HTML },
		{ ".mdoc", Lang::HTML },
		{ ".jsp", Lang::HTML },
		{ ".asp", Lang::HTML },
		{ ".aspx", Lang::HTML },
		{ ".jshtm", Lang::HTML },
		{ ".volt", Lang::HTML },
		{ ".ejs", Lang::HTML },
		{ ".rhtml", Lang::HTML },
		{ ".css", Lang::CSS },
		{ ".scss", Lang::CSS }
	};
	// 拡張子検査を優先（一般的なファイルは此処で一致しファイル名部分の抽出と小文字化を回避）
	const std::string Ext = GetExtension(Path);
	// 拡張子由来の派生形式だけを付加情報へ保存する
	if(const std::unordered_map<std::string, Lang::ID>::const_iterator ExtIt = ExtMap.find(Ext); ExtIt != ExtMap.end()) {
		Lang Result = Lang::Get(ExtIt->second);
		Result.IsJsx = Ext == ".tsx";
		Result.IsScss = Ext == ".scss";
		// 拡張子から定まる言語の返戻
		return Result;
	}
	// 拡張子で見付からない場合はファイル名部分の完全一致を試行（Rakefile/composer.lock 等）
	const size_t SlashPos = Path.find_last_of("/\\");
	std::string Base = SlashPos == std::string::npos ? Path : Path.substr(SlashPos + 1);
	ToLowerAscii(Base);
	const std::unordered_map<std::string, Lang::ID>::const_iterator NameIt = NameMap.find(Base);
	// 検出された Lang の返戻（何方にも見付からなければ Unknown）
	return Lang::Get(NameIt != NameMap.end() ? NameIt->second : Lang::Unknown);
}

/**
 * 複数言語が共有する拡張子の名前かの判定関数（`--lang h` は `.h` のファイルと同じく中身で言語を判別する）
 * @param Name 点を除く拡張子又は言語名（大文字小文字を区別しない）
 * @return 拡張子だけでは言語が定まらない名前なら true
 */
bool Lang::IsAmbiguousName(const std::string_view Name) {
	// `.h` は C / C++ / Objective-C が共有し，中身を見ても印の無いヘッダは判別出来ない事の返戻
	return Name == "h" || Name == "H";
}

/**
 * 複数言語が共有する拡張子かの判定関数
 * @param Path ファイルパス
 * @return 拡張子だけでは言語が定まらないなら true
 */
bool Lang::IsAmbiguousExtension(const std::string &Path) {
	// 小文字化済拡張子の取得
	const std::string Ext = GetExtension(Path);
	// 此の場合の明示指定は誤用ではなく利用者の唯一の手段の為，食違として拒んではならない事の返戻
	return !Ext.empty() && IsAmbiguousName(std::string_view(Ext).substr(1));
}

/**
 * C 系判定関数
 * @return C 又は C++ なら true
 */
bool Lang::IsCFamily() const {
	// ヘッダ共有判定では C と C++ だけを同族として扱う
	return Id == C || Id == Cpp;
}

/**
 * 共有拡張子の候補言語かの判定関数
 * @param Path ファイルパス
 * @param Override `--lang` の指定値
 * @return 指定が其の拡張子を共有する言語なら true
 */
bool Lang::IsAmbiguousCandidate(const std::string &Path, const Lang Override) {
	// 内容を壊す別言語指定を拒み，共有拡張子に対応する C 系かの返戻
	return Override.IsCFamily() && IsAmbiguousExtension(Path);
}

/**
 * JS / TS 判定関数
 * @return JavaScript 又は TypeScript なら true
 */
bool Lang::IsJsTs() const {
	// JS 又は TS であるかの返戻
	return Id == JavaScript || Id == TypeScript;
}

/**
 * 波括弧言語判定関数（C 系波括弧ブロックを共通に持つ言語群＝波括弧正規化／宣言併合／行折返の安全対象）
 * @return 該当する７言語なら true
 */
bool Lang::IsBraceLang() const {
	// 波括弧言語であるかの返戻
	return IsCFamily() || Id == CSharp || Id == Java || Id == PHP || IsJsTs();
}

/**
 * インデントが構文を成す言語の判定関数
 * @return インデントで文のネストを表す言語なら true
 */
bool Lang::IsIndentSensitive() const {
	// インデントが意味を持つ言語では，壊れた入力の部分整形がインデントを剥がした時点で意味其の物が変わる事の返戻
	return Id == Python;
}

/**
 * ネストの段数の上限の取得関数
 * @return インデントの表示幅４を推奨する言語は３２，幅２を推奨する言語は６４
 */
size_t Lang::NestingLimit() const {
	// 表示幅の狭い言語は同じ桁数へ倍の段が収まる事の返戻
	return IsJsTs() || Id == Ruby || Id == JSON || Id == HTML || Id == CSS ? NarrowIndentNestingLimit : WideIndentNestingLimit;
}

/**
 * `'` を文字列区切りとして扱えない言語判定関数（字句走査で区切りと看做すと領域を誤って飲み込む言語群）
 * @return Rust / C / C++ なら true
 */
bool Lang::IsApostropheAmbiguous() const {
	// 単独のアポストロフィが字面で区別不能な言語の返戻（文字内の括弧は構文木の範囲で保護）
	return Id == Rust || IsCFamily();
}

/**
 * 改行の後の `{` を前の式の末尾ラムダとして読む言語の判定関数
 * @return Kotlin / Swift なら true
 */
bool Lang::JoinsBraceAcrossNewline() const {
	// 此れ等の言語は改行で文を区切るが，次の行が `{` で始まると前の式の末尾ラムダ（クロージャ）として繋ぐ事の返戻
	return Id == Kotlin || Id == Swift;
}

/**
 * 等価比較演算子（Lang と ID の一致判定）
 * @param Target 比較対象の ID
 * @return Id が Target と一致する場合 true
 */
bool Lang::operator==(const ID Target) const {
	// 言語識別値の一致の返戻
	return Id == Target;
}
