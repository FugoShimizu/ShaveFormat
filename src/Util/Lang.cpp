#include "Lang.hpp"
#include "CharClass.hpp"
#include <tree_sitter/api.h>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

// tree-sitter の各言語の文法の取得関数
extern "C" {
	const TSLanguage *tree_sitter_c(), *tree_sitter_cpp(), *tree_sitter_c_sharp(); // C・C++・C# の文法の取得関数
	const TSLanguage *tree_sitter_java(), *tree_sitter_go(), *tree_sitter_rust(); // Java・Go・Rust の文法の取得関数
	const TSLanguage *tree_sitter_kotlin(), *tree_sitter_swift(), *tree_sitter_php(); // Kotlin・Swift・PHP の文法の取得関数
	const TSLanguage *tree_sitter_javascript(), *tree_sitter_typescript(), *tree_sitter_tsx(); // JavaScript・TypeScript・TSX の文法の取得関数
	const TSLanguage *tree_sitter_ruby(), *tree_sitter_python(), *tree_sitter_json(); // Ruby・Python・JSON の文法の取得関数
	const TSLanguage *tree_sitter_html(), *tree_sitter_scss(); // HTML の文法と，CSS・SCSS の共通の文法の取得関数
}

/**
 * 言語種別生成関数（ID から Lang を取得）
 * 計算量：O(1)
 * @param Id 言語 ID
 * @return 対応する Lang（範囲外は Unknown）
 */
::Lang Lang::Get(const ID Id) {
	// 符号無型へのキャストで負値も１回の比較で範囲外判定する事の返戻（負値はラップアラウンドして N 以上に為る）
	return { static_cast<size_t>(Id) <= static_cast<size_t>(Unknown) ? Id : Unknown };
}

/**
 * 拡張子取得関数（小文字化済）
 * 計算量：パス長 P に対し O(P)
 * @param Path ファイルパス
 * @return 拡張子（ドット付，小文字）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::string Lang::GetExt(const std::string &Path) {
	// パス中の拡張子境界の探索
	const size_t Pos = Path.rfind('.');
	// ドットが無い場合の返戻
	if(Pos == std::string::npos) return "";
	std::string Ext = Path.substr(Pos);
	ToLowerAscii(Ext);
	// 小文字化した拡張子の返戻
	return Ext;
}

/**
 * 拡張子からの言語判定関数
 * 計算量：拡張子長 L に対し平均 O(L)，最悪時は対応表の照合費用を加算
 * @param Ext 拡張子（ドット付，小文字）
 * @return 対応する `Lang`（未知の場合 `Lang::Unknown`）
 * @throws std::bad_alloc 対応表又は照合するテキストの確保に失敗した場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
::Lang Lang::FromExt(const std::string &Ext) {
	// 静的な表を共有し，呼出毎の表の再構築を避ける
	static const std::unordered_map<std::string, ::Lang> ExtMap = {
		{ ".c", { .Id = Lang::C } },
		{ ".h", { .Id = Lang::C } },
		{ ".i", { .Id = Lang::C } },
		{ ".c++", { .Id = Lang::Cpp } },
		{ ".c++m", { .Id = Lang::Cpp } },
		{ ".cc", { .Id = Lang::Cpp } },
		{ ".ccm", { .Id = Lang::Cpp } },
		{ ".cpp", { .Id = Lang::Cpp } },
		{ ".cppm", { .Id = Lang::Cpp } },
		{ ".cxx", { .Id = Lang::Cpp } },
		{ ".cxxm", { .Id = Lang::Cpp } },
		{ ".h++", { .Id = Lang::Cpp } },
		{ ".hh", { .Id = Lang::Cpp } },
		{ ".hpp", { .Id = Lang::Cpp } },
		{ ".hxx", { .Id = Lang::Cpp } },
		{ ".ii", { .Id = Lang::Cpp } },
		{ ".inl", { .Id = Lang::Cpp } },
		{ ".ino", { .Id = Lang::Cpp } },
		{ ".ipp", { .Id = Lang::Cpp } },
		{ ".ixx", { .Id = Lang::Cpp } },
		{ ".tpp", { .Id = Lang::Cpp } },
		{ ".txx", { .Id = Lang::Cpp } },
		{ ".cake", { .Id = Lang::CSharp } },
		{ ".cs", { .Id = Lang::CSharp } },
		{ ".csx", { .Id = Lang::CSharp } },
		{ ".jav", { .Id = Lang::Java } },
		{ ".java", { .Id = Lang::Java } },
		{ ".go", { .Id = Lang::Go } },
		{ ".rs", { .Id = Lang::Rust } },
		{ ".kt", { .Id = Lang::Kotlin } },
		{ ".ktm", { .Id = Lang::Kotlin } },
		{ ".kts", { .Id = Lang::Kotlin } },
		{ ".swift", { .Id = Lang::Swift } },
		{ ".php", { .Id = Lang::PHP } },
		{ ".php3", { .Id = Lang::PHP } },
		{ ".php4", { .Id = Lang::PHP } },
		{ ".php5", { .Id = Lang::PHP } },
		{ ".phps", { .Id = Lang::PHP } },
		{ ".phtml", { .Id = Lang::PHP } },
		{ ".cjs", { .Id = Lang::JavaScript } },
		{ ".es6", { .Id = Lang::JavaScript } },
		{ ".js", { .Id = Lang::JavaScript } },
		{ ".jsx", { .Id = Lang::JavaScript } },
		{ ".mjs", { .Id = Lang::JavaScript } },
		{ ".pac", { .Id = Lang::JavaScript } },
		{ ".cts", { .Id = Lang::TypeScript } },
		{ ".mts", { .Id = Lang::TypeScript } },
		{ ".ts", { .Id = Lang::TypeScript } },
		{ ".tsx", { .Id = Lang::TypeScript } },
		{ ".gemspec", { .Id = Lang::Ruby } },
		{ ".podspec", { .Id = Lang::Ruby } },
		{ ".rake", { .Id = Lang::Ruby } },
		{ ".rb", { .Id = Lang::Ruby } },
		{ ".rbi", { .Id = Lang::Ruby } },
		{ ".rbw", { .Id = Lang::Ruby } },
		{ ".rbx", { .Id = Lang::Ruby } },
		{ ".rjs", { .Id = Lang::Ruby } },
		{ ".ru", { .Id = Lang::Ruby } },
		{ ".cpy", { .Id = Lang::Python } },
		{ ".gyp", { .Id = Lang::Python } },
		{ ".gypi", { .Id = Lang::Python } },
		{ ".ipy", { .Id = Lang::Python } },
		{ ".py", { .Id = Lang::Python } },
		{ ".pyi", { .Id = Lang::Python } },
		{ ".pyt", { .Id = Lang::Python } },
		{ ".pyw", { .Id = Lang::Python } },
		{ ".babelrc", { .Id = Lang::JSON, .IsJsonc = true } },
		{ ".bowerrc", { .Id = Lang::JSON } },
		{ ".eslintrc", { .Id = Lang::JSON, .IsJsonc = true } },
		{ ".geojson", { .Id = Lang::JSON } },
		{ ".hintrc", { .Id = Lang::JSON, .IsJsonc = true } },
		{ ".jscsrc", { .Id = Lang::JSON } },
		{ ".jsfmtrc", { .Id = Lang::JSON, .IsJsonc = true } },
		{ ".jshintrc", { .Id = Lang::JSON, .IsJsonc = true } },
		{ ".jslintrc", { .Id = Lang::JSON } },
		{ ".json", { .Id = Lang::JSON } },
		{ ".jsonc", { .Id = Lang::JSON, .IsJsonc = true } },
		{ ".jsonld", { .Id = Lang::JSON } },
		{ ".swcrc", { .Id = Lang::JSON, .IsJsonc = true } },
		{ ".vuerc", { .Id = Lang::JSON } },
		{ ".webmanifest", { .Id = Lang::JSON } },
		{ ".htm", { .Id = Lang::HTML } },
		{ ".html", { .Id = Lang::HTML } },
		{ ".mdoc", { .Id = Lang::HTML } },
		{ ".shtml", { .Id = Lang::HTML } },
		{ ".xht", { .Id = Lang::HTML } },
		{ ".xhtml", { .Id = Lang::HTML } },
		{ ".css", { .Id = Lang::CSS } },
		{ ".scss", { .Id = Lang::CSS } }
	};
	// 拡張子の対応表からの検索
	const std::unordered_map<std::string, ::Lang>::const_iterator Iter = ExtMap.find(Ext);
	::Lang Result = Iter != ExtMap.end() ? Iter->second : Lang::Get(Lang::Unknown);
	// 拡張子由来の派生形式だけを付加情報へ保存する
	Result.IsJsx = Ext == ".tsx";
	Result.IsScss = Ext == ".scss";
	Result.Goal = Ext == ".mjs" ? SrcGoal::Module : Ext == ".cjs" ? SrcGoal::Script : SrcGoal::Undetermined;
	// 拡張子から定まる言語の返戻
	return Result;
}

/**
 * 複数言語が共有する拡張子の名前かの判定関数（`--lang h` は `.h` のファイルと同じく中身で言語を判別する）
 * 計算量：言語名長 L に対し O(L)
 * @param Name 点を除く拡張子又は言語名（大文字小文字を区別しない）
 * @return 拡張子だけでは言語が定まらない名前なら true
 */
bool Lang::IsAmbiguousName(const std::string_view Name) {
	// `.h` は C / C++ / Objective-C が共有し，中身を見てもマーカーの無いヘッダは判別出来ない事の返戻
	return Name == "h";
}

/**
 * 複数言語が共有する拡張子かの判定関数
 * 計算量：パス長 P に対し O(P)
 * @param Path ファイルパス
 * @return 拡張子だけでは言語が定まらないなら true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool Lang::IsAmbiguousExt(const std::string &Path) {
	// 此の場合の明示指定は誤用ではなく利用者の唯一の手段の為，食違として拒んでは為らない事の返戻
	return GetExt(Path) == ".h";
}

/**
 * C 系判定関数
 * 計算量：O(1)
 * @return C 又は C++ なら true
 */
bool Lang::IsCFamily() const {
	// C と C++ だけを同族とするヘッダ共有判定の返戻
	return Id == C || Id == Cpp;
}

/**
 * tree-sitter 言語定義取得関数
 * 計算量：O(1)
 * @return 対応する TSLanguage ポインタ（未対応言語では nullptr）
 */
const TSLanguage *Lang::GetTreeSitterLang() const {
	// 各識別子をリンク済の文法定義へ結び，パーサーの生成側へ所有権を渡さない
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
 * 言語名判定関数（VSCode の languageId を其の儘受付）
 * 計算量：言語名長 L に対し平均 O(L)，最悪時は対応表の照合費用を加算
 * @param Name 言語名 (VSCode languageId)
 * @return 対応する `Lang`（未知の場合 `Lang::Unknown`）
 * @throws std::bad_alloc 対応表又は照合するテキストの確保に失敗した場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
::Lang Lang::FromName(const std::string_view Name) {
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
	// エディターや作業手順書は `TypeScript` / `C` の様に大文字を混ぜる為，照合前に小文字へ揃える
	std::string Key(Name);
	ToLowerAscii(Key);
	const std::unordered_map<std::string_view, Lang::ID>::const_iterator Iter = Map.find(Key);
	// 表に無い名前はファイルの拡張子として判定（`hh` / `kts` / `mts` / `pyi` / `phtml` 等）
	::Lang Result = Iter == Map.end() ? FromExt("." + Key) : Lang::Get(Iter->second);
	if(Iter != Map.end()) {
		Result.IsJsx = Key == "typescriptreact" || Key == "tsx";
		Result.IsScss = Key == "scss";
		Result.IsJsonc = Key == "jsonc";
		Result.Goal = Key == "mjs" ? SrcGoal::Module : Key == "cjs" ? SrcGoal::Script : SrcGoal::Undetermined;
	}
	// 共有する拡張子と同じ名前は中身での判別が要る事を言語と一緒に持たせる
	Result.IsAmbiguous = IsAmbiguousName(Key);
	// 言語 ID 名から Lang の返戻（見付からなければ Unknown）
	return Result;
}

/**
 * 拡張子だけでは定まらなかった名前の言語判定関数
 * 計算量：名前長 N に対し平均 O(N)，追加領域 O(N)
 * @param Name パスを除いたファイル名
 * @return 対応する言語（表に無い場合は Unknown）
 * @throws std::bad_alloc 対応表又は照合する名前を確保出来ない場合
 * @throws std::length_error 照合する名前が文字列の上限を超える場合
 */
::Lang Lang::FromBaseName(const std::string_view Name) {
	// 名前の対応表を共有し，言語を拡張子だけで定めた対象では此の表を使わない
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
	std::string Base(Name);
	ToLowerAscii(Base);
	const std::unordered_map<std::string, Lang::ID>::const_iterator Found = NameMap.find(Base);
	// 名前の完全一致から定まる言語の返戻
	return Lang::Get(Found != NameMap.end() ? Found->second : Lang::Unknown);
}

/**
 * パスからの言語判定関数（拡張子・ファイル名から検出）
 * 計算量：パス長 P と固定の拡張子数 K に対し O(P + K)
 * @param Path ファイルパス
 * @return 検出された `Lang`（未知の場合 `Lang::Unknown`）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
::Lang Lang::Detect(const std::string &Path) {
	// 拡張子検査を優先（一般的なファイルは此処で一致しファイル名部分の抽出と小文字化を回避）
	const std::string Ext = GetExt(Path);
	::Lang Result = FromExt(Ext);
	if(Result == Lang::Unknown) {
		// 拡張子で見付からない場合はファイル名部分の完全一致を試行（Rakefile/composer.lock 等）
		const size_t SlashPos = Path.find_last_of("/\\");
		Result = FromBaseName(std::string_view(Path).substr(SlashPos == std::string::npos ? 0 : SlashPos + 1));
	}
	// 共有する拡張子は中身での判別が要る事を言語と一緒に持たせ，呼出側が別のフィールドで持ち回らない様にする
	Result.IsAmbiguous = !Ext.empty() && IsAmbiguousName(std::string_view(Ext).substr(1));
	// 検出された Lang の返戻（何方にも見付からなければ Unknown）
	return Result;
}

#if defined(_WIN32)

/**
 * 処理系の名前を UTF-8 へ変換せずに言語を判定する関数
 * 計算量：名前長 P に対し平均 O(P)，追加領域 O(P)
 * @param Path ワイド文字列のパス又は名前
 * @return 既存の拡張子・名前の表に一致する言語（其の他は Unknown）
 * @throws std::bad_alloc 対応表又は照合するテキストを確保出来ない場合
 * @throws std::length_error 照合するテキストが文字列の上限を超える場合
 */
::Lang Lang::Detect(const std::wstring_view Path) {
	// 表に無いバイトで ASCII 以外を置き換え，区切と拡張子を既存の判定へ渡す
	std::string Key;
	Key.reserve(Path.size());
	for(const wchar_t Point : Path) Key += static_cast<uint32_t>(Point) < 0X80 ? static_cast<char>(Point) : '\x80';
	// 拡張子・名前・方言と曖昧さを，同じ判定で確定した返戻
	return Detect(Key);
}

#endif

/**
 * 共有拡張子の候補言語かの判定関数
 * 計算量：パス長 P に対し O(P)
 * @param Path ファイルパス
 * @param Override `--lang` の指定値
 * @return 指定が其の拡張子を共有する言語なら true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool Lang::IsAmbiguousCandidate(const std::string &Path, const ::Lang Override) {
	// 内容を壊す別言語指定を拒み，共有拡張子に対応する C 系かの返戻
	return Override.IsCFamily() && IsAmbiguousExt(Path);
}

/**
 * JS / TS 判定関数
 * 計算量：O(1)
 * @return JavaScript 又は TypeScript なら true
 */
bool Lang::IsJsTs() const {
	// JS 又は TS であるかの返戻
	return Id == JavaScript || Id == TypeScript;
}

/**
 * 等価比較演算子（Lang と ID の一致判定）
 * 計算量：O(1)
 * @param Target 比較対象の ID
 * @return Id が Target と一致する場合 true
 */
bool Lang::operator==(const ID Target) const {
	// 言語識別値の一致の返戻
	return Id == Target;
}
