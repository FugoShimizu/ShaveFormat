#include "Lang.hpp"
#include "CharClass.hpp"
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
	const TSLanguage *tree_sitter_php();
	const TSLanguage *tree_sitter_javascript();
	const TSLanguage *tree_sitter_typescript();
	const TSLanguage *tree_sitter_tsx();
	const TSLanguage *tree_sitter_ruby();
	const TSLanguage *tree_sitter_python();
	const TSLanguage *tree_sitter_json();
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
	case Lang::PHP:
		return tree_sitter_php();
	case Lang::JavaScript:
		// JSX の有無に依らない JavaScript の文法の返戻（JSX の有無は同じ言語識別子の付加情報で文法だけを切り替える）
		return tree_sitter_javascript();
	case Lang::TypeScript:
		return IsJsx ? tree_sitter_tsx() : tree_sitter_typescript();
	case Lang::Ruby:
		return tree_sitter_ruby();
	case Lang::Python:
		return tree_sitter_python();
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
 * 言語種別生成関数（ID から Lang を取得）
 * @param Id 言語 ID
 * @return 対応する Lang（範囲外は Unknown）
 */
Lang Lang::Get(const ID Id) {
	// 符号無型へのキャストで負値も１回の比較で範囲外判定する事の返戻（負値はラップアラウンドして N 以上になる）
	return { static_cast<size_t>(Id) <= static_cast<size_t>(Unknown) ? Id : Unknown };
}

/**
 * 拡張子からの言語判定関数
 * @param Ext 拡張子（ドット付，小文字）
 * @return 対応する `Lang`（未知の場合 `Lang::Unknown`）
 */
Lang Lang::FromExtension(const std::string &Ext) {
	// 静的な表を共有し，呼出毎の表の再構築を避ける
	static const std::unordered_map<std::string, Lang::ID> ExtMap = {
		{ ".c", Lang::C },
		{ ".h", Lang::C },
		{ ".i", Lang::C },
		{ ".c++", Lang::Cpp },
		{ ".c++m", Lang::Cpp },
		{ ".cc", Lang::Cpp },
		{ ".ccm", Lang::Cpp },
		{ ".cpp", Lang::Cpp },
		{ ".cppm", Lang::Cpp },
		{ ".cxx", Lang::Cpp },
		{ ".cxxm", Lang::Cpp },
		{ ".h++", Lang::Cpp },
		{ ".hh", Lang::Cpp },
		{ ".hpp", Lang::Cpp },
		{ ".hxx", Lang::Cpp },
		{ ".ii", Lang::Cpp },
		{ ".inl", Lang::Cpp },
		{ ".ino", Lang::Cpp },
		{ ".ipp", Lang::Cpp },
		{ ".ixx", Lang::Cpp },
		{ ".tpp", Lang::Cpp },
		{ ".txx", Lang::Cpp },
		{ ".cake", Lang::CSharp },
		{ ".cs", Lang::CSharp },
		{ ".csx", Lang::CSharp },
		{ ".jav", Lang::Java },
		{ ".java", Lang::Java },
		{ ".go", Lang::Go },
		{ ".rs", Lang::Rust },
		{ ".kt", Lang::Kotlin },
		{ ".ktm", Lang::Kotlin },
		{ ".kts", Lang::Kotlin },
		{ ".swift", Lang::Swift },
		{ ".php", Lang::PHP },
		{ ".php3", Lang::PHP },
		{ ".php4", Lang::PHP },
		{ ".php5", Lang::PHP },
		{ ".phps", Lang::PHP },
		{ ".phtml", Lang::PHP },
		{ ".cjs", Lang::JavaScript },
		{ ".es6", Lang::JavaScript },
		{ ".js", Lang::JavaScript },
		{ ".jsx", Lang::JavaScript },
		{ ".mjs", Lang::JavaScript },
		{ ".pac", Lang::JavaScript },
		{ ".cts", Lang::TypeScript },
		{ ".mts", Lang::TypeScript },
		{ ".ts", Lang::TypeScript },
		{ ".tsx", Lang::TypeScript },
		{ ".gemspec", Lang::Ruby },
		{ ".podspec", Lang::Ruby },
		{ ".rake", Lang::Ruby },
		{ ".rb", Lang::Ruby },
		{ ".rbi", Lang::Ruby },
		{ ".rbw", Lang::Ruby },
		{ ".rbx", Lang::Ruby },
		{ ".rjs", Lang::Ruby },
		{ ".ru", Lang::Ruby },
		{ ".cpy", Lang::Python },
		{ ".gyp", Lang::Python },
		{ ".gypi", Lang::Python },
		{ ".ipy", Lang::Python },
		{ ".py", Lang::Python },
		{ ".pyi", Lang::Python },
		{ ".pyt", Lang::Python },
		{ ".pyw", Lang::Python },
		{ ".rpy", Lang::Python },
		{ ".babelrc", Lang::JSON },
		{ ".bowerrc", Lang::JSON },
		{ ".eslintrc", Lang::JSON },
		{ ".geojson", Lang::JSON },
		{ ".hintrc", Lang::JSON },
		{ ".jscsrc", Lang::JSON },
		{ ".jsfmtrc", Lang::JSON },
		{ ".jshintrc", Lang::JSON },
		{ ".jslintrc", Lang::JSON },
		{ ".json", Lang::JSON },
		{ ".jsonc", Lang::JSON },
		{ ".jsonld", Lang::JSON },
		{ ".swcrc", Lang::JSON },
		{ ".vuerc", Lang::JSON },
		{ ".webmanifest", Lang::JSON },
		{ ".asp", Lang::HTML },
		{ ".aspx", Lang::HTML },
		{ ".ejs", Lang::HTML },
		{ ".htm", Lang::HTML },
		{ ".html", Lang::HTML },
		{ ".jshtm", Lang::HTML },
		{ ".jsp", Lang::HTML },
		{ ".mdoc", Lang::HTML },
		{ ".rhtml", Lang::HTML },
		{ ".shtml", Lang::HTML },
		{ ".volt", Lang::HTML },
		{ ".xht", Lang::HTML },
		{ ".xhtml", Lang::HTML },
		{ ".css", Lang::CSS },
		{ ".scss", Lang::CSS }
	};
	// 拡張子の対応表からの検索
	const std::unordered_map<std::string, Lang::ID>::const_iterator Iter = ExtMap.find(Ext);
	Lang Result = Lang::Get(Iter != ExtMap.end() ? Iter->second : Lang::Unknown);
	// 拡張子由来の派生形式だけを付加情報へ保存する
	Result.IsJsx = Ext == ".tsx";
	Result.IsScss = Ext == ".scss";
	Result.Goal = Ext == ".mjs" ? SourceGoal::Module : Ext == ".cjs" ? SourceGoal::Script : SourceGoal::Undetermined;
	// 拡張子から定まる言語の返戻
	return Result;
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
 * 言語名判定関数（VSCode の languageId を其の儘受付）
 * @param Name 言語名 (VSCode languageId)
 * @return 対応する `Lang`（未知の場合 `Lang::Unknown`）
 */
Lang Lang::FromName(const std::string_view Name) {
	// 静的な表を共有し，呼出毎の別名集合の再構築を避ける
	static const std::unordered_map<std::string_view, Lang::ID> Map = {
		{ "c", Lang::C },
		{ "h", Lang::C },
		{ "c++", Lang::Cpp },
		{ "cc", Lang::Cpp },
		{ "cpp", Lang::Cpp },
		{ "cxx", Lang::Cpp },
		{ "hpp", Lang::Cpp },
		{ "cs", Lang::CSharp },
		{ "csharp", Lang::CSharp },
		{ "java", Lang::Java },
		{ "go", Lang::Go },
		{ "rs", Lang::Rust },
		{ "rust", Lang::Rust },
		{ "kotlin", Lang::Kotlin },
		{ "kt", Lang::Kotlin },
		{ "swift", Lang::Swift },
		{ "php", Lang::PHP },
		{ "cjs", Lang::JavaScript },
		{ "javascript", Lang::JavaScript },
		{ "javascriptreact", Lang::JavaScript },
		{ "js", Lang::JavaScript },
		{ "jsx", Lang::JavaScript },
		{ "mjs", Lang::JavaScript },
		{ "ts", Lang::TypeScript },
		{ "tsx", Lang::TypeScript },
		{ "typescript", Lang::TypeScript },
		{ "typescriptreact", Lang::TypeScript },
		{ "rb", Lang::Ruby },
		{ "ruby", Lang::Ruby },
		{ "py", Lang::Python },
		{ "python", Lang::Python },
		{ "json", Lang::JSON },
		{ "jsonc", Lang::JSON },
		{ "htm", Lang::HTML },
		{ "html", Lang::HTML },
		{ "css", Lang::CSS },
		{ "scss", Lang::CSS }
	};
	// 編集器や作業手順書は `TypeScript` / `C` の様に大文字を混ぜる為，照合前に小文字へ揃える
	std::string Key(Name);
	ToLowerAscii(Key);
	const std::unordered_map<std::string_view, Lang::ID>::const_iterator Iter = Map.find(Key);
	// 表に無い名前はファイルの拡張子として判定（`hh` / `kts` / `mts` / `pyi` / `phtml` 等）
	Lang Result = Iter == Map.end() ? FromExtension("." + Key) : Lang::Get(Iter->second);
	if(Iter != Map.end()) {
		Result.IsJsx = Key == "typescriptreact" || Key == "tsx";
		Result.IsScss = Key == "scss";
		Result.Goal = Key == "mjs" ? SourceGoal::Module : Key == "cjs" ? SourceGoal::Script : SourceGoal::Undetermined;
	}
	// 共有する拡張子と同じ名前は中身での判別が要る事を言語と一緒に持たせる
	Result.IsAmbiguous = IsAmbiguousName(Key);
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
 * 複数言語が共有する拡張子かの判定関数
 * @param Path ファイルパス
 * @return 拡張子だけでは言語が定まらないなら true
 */
bool Lang::IsAmbiguousExtension(const std::string &Path) {
	// 此の場合の明示指定は誤用ではなく利用者の唯一の手段の為，食違として拒んではならない事の返戻
	return GetExtension(Path) == ".h";
}

/**
 * パスからの言語判定関数（拡張子・ファイル名から検出）
 * @param Path ファイルパス
 * @return 検出された `Lang`（未知の場合 `Lang::Unknown`）
 */
Lang Lang::Detect(const std::string &Path) {
	// VSCode と同じくファイル名を小文字化して完全一致判定
	static const std::unordered_map<std::string, Lang::ID> NameMap = {
		{ "jakefile", Lang::JavaScript },
		{ "appfile", Lang::Ruby },
		{ "appraisals", Lang::Ruby },
		{ "berksfile", Lang::Ruby },
		{ "berksfile.lock", Lang::Ruby },
		{ "brewfile", Lang::Ruby },
		{ "capfile", Lang::Ruby },
		{ "cheffile", Lang::Ruby },
		{ "dangerfile", Lang::Ruby },
		{ "deliverfile", Lang::Ruby },
		{ "fastfile", Lang::Ruby },
		{ "gemfile", Lang::Ruby },
		{ "guardfile", Lang::Ruby },
		{ "gymfile", Lang::Ruby },
		{ "hobofile", Lang::Ruby },
		{ "matchfile", Lang::Ruby },
		{ "podfile", Lang::Ruby },
		{ "puppetfile", Lang::Ruby },
		{ "rakefile", Lang::Ruby },
		{ "rantfile", Lang::Ruby },
		{ "scanfile", Lang::Ruby },
		{ "snapfile", Lang::Ruby },
		{ "thorfile", Lang::Ruby },
		{ "vagrantfile", Lang::Ruby },
		{ "sconscript", Lang::Python },
		{ "sconstruct", Lang::Python },
		{ "snakefile", Lang::Python },
		{ ".ember-cli", Lang::JSON },
		{ ".watchmanconfig", Lang::JSON },
		{ "bun.lock", Lang::JSON },
		{ "composer.lock", Lang::JSON }
	};
	// 拡張子検査を優先（一般的なファイルは此処で一致しファイル名部分の抽出と小文字化を回避）
	Lang Result = FromExtension(GetExtension(Path));
	if(Result == Lang::Unknown) {
		// 拡張子で見付からない場合はファイル名部分の完全一致を試行（Rakefile/composer.lock 等）
		const size_t SlashPos = Path.find_last_of("/\\");
		std::string Base = SlashPos == std::string::npos ? Path : Path.substr(SlashPos + 1);
		ToLowerAscii(Base);
		const std::unordered_map<std::string, Lang::ID>::const_iterator NameIt = NameMap.find(Base);
		Result = Lang::Get(NameIt != NameMap.end() ? NameIt->second : Lang::Unknown);
	}
	// 共有する拡張子は中身での判別が要る事を言語と一緒に持たせ，呼出側が別の欄で持ち回らない様にする
	Result.IsAmbiguous = IsAmbiguousExtension(Path);
	// 検出された Lang の返戻（何方にも見付からなければ Unknown）
	return Result;
}

/**
 * C 系判定関数
 * @return C 又は C++ なら true
 */
bool Lang::IsCFamily() const {
	// C と C++ だけを同族とするヘッダ共有判定の返戻
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
 * 等価比較演算子（Lang と ID の一致判定）
 * @param Target 比較対象の ID
 * @return Id が Target と一致する場合 true
 */
bool Lang::operator==(const ID Target) const {
	// 言語識別値の一致の返戻
	return Id == Target;
}
