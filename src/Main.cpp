#include "Formatter.hpp"
#include "Pass/Layout.hpp"
#include "Pass/LineSplit.hpp"
#include "Pass/Lint.hpp"
#include "Util/FileIO.hpp"
#include "Util/Lang.hpp"
#include "Util/Parallel.hpp"
#include "Version.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)

#include <fcntl.h>
#include <io.h>

#elif !defined(__APPLE__)

#include <sys/resource.h>

#endif

// ファイルシステム名前空間の別名
namespace Filesystem = std::filesystem;

// 検査工程が道具の誤用と対象の未整形を区別する使用法誤りの終了コード
static constexpr int UsageErrorCode = 2;

/**
 * 標準出力の確定関数
 * @return 出力成功時０，失敗時１
 */
static int FlushStdout() {
	// 標準出力の書込失敗
	if(std::fflush(stdout) || std::ferror(stdout)) {
		std::fputs("Error: failed to write stdout\n", stderr);
		// 書込失敗時の返戻
		return 1;
	}
	// 出力成功の返戻
	return 0;
}

/**
 * 使用方法表示関数
 * @param UsesStderr 誤りに伴う表示なら true（誤りの手掛りが `> /dev/null` で消えない様に標準エラーへ出す）
 */
static void PrintUsage(const bool UsesStderr) {
	// 使用法を出す流れ
	std::FILE *const Out = UsesStderr ? stderr : stdout;
	std::fprintf(Out, "shavefmt %.*s\n", static_cast<int>(BuildInfo::Version.size()), BuildInfo::Version.data());
	std::fputs(
		"\n"
		"Usage: shavefmt [OPTIONS] <FILE|DIR>...\n"
		"       shavefmt --stdin --lang ID [OPTIONS]   (read stdin, write stdout)\n"
		"\n"
		"Options:\n"
		"  -w, --write       Write changes to files (default: dry-run; not with -c)\n"
		"  -c, --check       Check only (exit code 1 if changes needed)\n"
		"  --diff            Show diff\n"
		"  -l, --lang ID     Override language detection (see \"Language IDs\" below)\n"
		"  -m, --chars N     Maximum characters per line (0 means unlimited; default: 128; no effect on CSS/SCSS/HTML)\n"
		"  --stdin           Read from stdin and write formatted result to stdout (--lang required)\n"
		"  --no-lint         Skip lint warnings (faster format-only mode)\n"
		"  --fail-on-skip    Treat skipped files and named paths through symbolic links as failures (exit code 1)\n"
		"  -h, --help        Show this help and exit (other arguments are ignored)\n"
		"  -v, --version     Show version and exit (other arguments are ignored)\n"
		"  --                Treat every later argument as a path (for names starting with -)\n"
		"\n"
		"Language IDs (case-insensitive; common extensions such as ts, py, rb also work):\n"
		"  c, cpp, csharp, java, go, rust, kotlin, swift, php, javascript, javascriptreact,\n"
		"  typescript, typescriptreact, ruby, python, json, jsonc, html, css, scss\n"
		"  (h: C or C++ by content, as for .h files)\n"
		"\n"
		"Exit codes:\n"
		"  0  success (with -c: no file needs formatting)\n"
		"  1  formatting needed (-c), a file could not be processed, formatting was reverted by the syntax guard\n"
		"     or by a formatter defect,\n"
		"     or a file or a named symbolic link was skipped with --fail-on-skip\n"
		"  2  usage error (unknown option, missing value, conflicting flags)\n",
		Out
	);
	return;
}

// コマンドライン指定構造体
struct Options {
	bool ShouldWrite = false;
	bool ShouldCheck = false;
	bool ShowsDiff = false;
	bool ReadsStdin = false;
	bool SkipsLint = false;
	bool FailsOnSkip = false;
	size_t MaxChars = 128; // 行最大文字数（`0` は無制限）
	Lang OverrideLang = Lang::Get(Lang::Unknown); // `--lang` 指定値（`Lang::Unknown` は未指定）
	bool IsLangAmbiguous = false; // `--lang h`：`.h` のファイルと同じく中身で C / C++ を判別するか
	std::vector<std::string> Paths; // 指定された入力経路列
};

/**
 * コマンドライン引数解析関数
 * @param Argc 引数の数
 * @param Argv 引数の配列
 * @return 解析結果の `Options` 構造体
 */
static Options ParseArgs(const int Argc, char *const Argv[]) {
	// 解析中の設定
	Options Opts;
	// 値を要する引数の検査関数
	const auto RequireValue = [Argc, &Argv](const int Idx) -> void {
		// 必須値の有無の検査
		if(Idx + 1 == Argc) {
			std::fprintf(stderr, "Error: %s requires a value\n", FileIO::ForDisplay(Argv[Idx]).c_str());
			std::exit(UsageErrorCode);
		}
		// 終了
		return;
	};
	// 誤った引数の報告より先に行う早期終了オプションの走査
	for(int Idx = 1; Idx < Argc && std::strcmp(Argv[Idx], "--"); ++Idx) {
		// ヘルプ指定の場合
		if(!std::strcmp(Argv[Idx], "-h") || !std::strcmp(Argv[Idx], "--help")) {
			PrintUsage(false);
			std::exit(FlushStdout());
		}
		// 版表示指定の場合
		if(!std::strcmp(Argv[Idx], "-v") || !std::strcmp(Argv[Idx], "--version")) {
			// 不具合報告と検査工程向の機械可読な版の単独行出力
			std::printf("shavefmt %.*s\n", static_cast<int>(BuildInfo::Version.size()), BuildInfo::Version.data());
			std::exit(FlushStdout());
		}
	}
	// 通常指定の左からの反映と値を取る指定での走査位置更新
	for(int Idx = 1; Idx < Argc; ++Idx) {
		if(!std::strcmp(Argv[Idx], "-w") || !std::strcmp(Argv[Idx], "--write")) Opts.ShouldWrite = true;
		else if(!std::strcmp(Argv[Idx], "-c") || !std::strcmp(Argv[Idx], "--check")) Opts.ShouldCheck = true;
		else if(!std::strcmp(Argv[Idx], "--diff")) Opts.ShowsDiff = true;
		// 言語指定の読取
		else if(!std::strcmp(Argv[Idx], "-l") || !std::strcmp(Argv[Idx], "--lang")) {
			RequireValue(Idx);
			const char *const Value = Argv[++Idx];
			const Lang Parsed = Lang::FromName(Value);
			// 未対応の言語識別子
			if(Parsed == Lang::Unknown) {
				std::fprintf(
					stderr,
					"Error: --lang unsupported language ID '%s' (run --help for the list)\n",
					FileIO::ForDisplay(Value).c_str()
				);
				std::exit(UsageErrorCode);
			}
			// 指定言語を保存
			Opts.OverrideLang = Parsed;
			Opts.IsLangAmbiguous = Lang::IsAmbiguousName(Value);
			// 行幅指定の読取
		} else if(!std::strcmp(Argv[Idx], "-m") || !std::strcmp(Argv[Idx], "--chars")) {
			// 十進指定値の読取
			RequireValue(Idx);
			const char *const Value = Argv[++Idx];
			size_t Parsed = 0;
			const char *Ptr = Value;
			bool IsOverflow = false;
			for(; *Ptr >= '0' && *Ptr <= '9'; ++Ptr) {
				const size_t Digit = static_cast<size_t>(*Ptr - '0');
				// 小さな幅への桁溢れ防止
				if(Parsed > (std::numeric_limits<size_t>::max() - Digit) / 10) IsOverflow = true;
				Parsed = 10 * Parsed + Digit;
			}
			// 空入力・非数字・桁溢れの拒否
			if(Ptr == Value || *Ptr || IsOverflow) {
				const char *const Reason = IsOverflow ? "is out of range" : "requires a non-negative integer";
				std::fprintf(stderr, "Error: --chars %s, got '%s'\n", Reason, FileIO::ForDisplay(Value).c_str());
				std::exit(UsageErrorCode);
			}
			Opts.MaxChars = Parsed;
		} else if(!std::strcmp(Argv[Idx], "--stdin")) Opts.ReadsStdin = true;
		else if(!std::strcmp(Argv[Idx], "--no-lint")) Opts.SkipsLint = true;
		else if(!std::strcmp(Argv[Idx], "--fail-on-skip")) Opts.FailsOnSkip = true;
		// オプション終端後を経路として収集
		else if(!std::strcmp(Argv[Idx], "--")) for(++Idx; Idx < Argc; ++Idx) Opts.Paths.emplace_back(Argv[Idx]);
		// 未知のオプションの場合
		else if(Argv[Idx][0] == '-' && Argv[Idx][1]) {
			// 綴り誤りの指定を経路扱いにしない報告
			std::fprintf(stderr, "Error: unknown option '%s'\n", FileIO::ForDisplay(Argv[Idx]).c_str());
			std::exit(UsageErrorCode);
			// 通常の入力経路を追加
		} else Opts.Paths.emplace_back(Argv[Idx]);
	}
	// 検査時の誤書込を防ぐ `--check` と `--write` の同時指定拒否
	if(Opts.ShouldCheck && Opts.ShouldWrite) {
		std::fprintf(stderr, "Error: -c and -w are mutually exclusive\n");
		std::exit(UsageErrorCode);
	}
	// `--stdin` で作用しない書戻・検査・差分表示指定の拒否
	if(Opts.ReadsStdin && (Opts.ShouldCheck || Opts.ShouldWrite || Opts.ShowsDiff)) {
		std::fprintf(stderr, "Error: --stdin cannot be combined with -c, -w or --diff\n");
		std::exit(UsageErrorCode);
	}
	// `--stdin` と併記したパスの拒否
	if(Opts.ReadsStdin && !Opts.Paths.empty()) {
		std::fprintf(
			stderr,
			"Error: --stdin does not take file arguments (got '%s')\n",
			FileIO::ForDisplay(Opts.Paths.front()).c_str()
		);
		std::exit(UsageErrorCode);
	}
	// 確定したオプションを返戻
	return Opts;
}

// 対象ファイルと其の言語（`--lang` の上書きは明示指定した単一ファイルにのみ及ぶ為，収集時に確定させて持ち回る）
struct TargetFile {
	std::string Path;
	Lang Language;
	bool IsLangAssumed = false; // 拡張子から言語が定まらず `--lang` の指定だけで決めたか
	bool IsLangAmbiguous = false; // 拡張子だけでは言語が定まらないか（`.h` 等，中身での判別が要る）
	std::string Display; // 報告に使う経路（空なら `Path`）
};

// ディレクトリ走査で読まなかった経路の数（読まれて居ない事を要約へ出し，検査工程が「全て検査した」と読まない様にする）
struct ScanExclusions {
	size_t ByName = 0; // 名前で除いた経路の数
	size_t Links = 0; // 未読リンク数（明示経路と走査中の対象ファイル・ディレクトリ）
	size_t NamedLinks = 0; // 明示リンク数（`--fail-on-skip` では未検査の失敗扱い）
};

/**
 * ASCII の大小を無視した名前の一致の判定関数
 * 計算量：名前の長さ N に対し O(N)
 * @param Name 検査する名前
 * @param Target 比べる名前
 * @return 大小を無視して一致すれば true
 */
static bool MatchesIgnoringAsciiCase(const std::string_view Name, const std::string_view Target) {
	// 名前長の照合
	// 長さの違う名前の返戻
	if(Name.size() != Target.size()) return false;
	for(size_t Index = 0; Index < Name.size(); ++Index) {
		// 左側の小文字形
		const char Left = Name[Index] >= 'A' && Name[Index] <= 'Z' ? Name[Index] + ('a' - 'A') : Name[Index];
		// 右側の小文字形との照合
		if(
			const char Right = Target[Index] >= 'A' && Target[Index] <= 'Z' ? Target[Index] + ('a' - 'A') : Target[Index];
			Left != Right
			// 一致しない字が有る事の返戻
		) return false;
	}
	// 全ての字が一致した事の返戻
	return true;
}

/**
 * ASCII の大小を無視した末尾の一致の判定関数
 * @param Name 検査する名前
 * @param Suffix 比べる末尾
 * @return 大小を無視して末尾が一致すれば true
 */
static bool EndsWithIgnoringAsciiCase(const std::string_view Name, const std::string_view Suffix) {
	// 末尾の一致の返戻
	return Name.size() >= Suffix.size() && MatchesIgnoringAsciiCase(Name.substr(Name.size() - Suffix.size()), Suffix);
}

/**
 * 整形対象ファイル収集関数
 * 計算量：項目数 E に対し O(E log E)
 * @param Path ファイル又はディレクトリのパス
 * @param OverrideLang `--lang` の指定値（`Lang::Unknown` は未指定）
 * @param IsAmbiguousOverride `--lang h` の指定か（`.h` と拡張子の無いファイルを中身で判別する）
 * @param Files 収集先のファイル一覧
 * @param IsFileUnsupported 明示指定されたファイルが対応外だった場合に true を設定
 * @param IsLangConflicting 明示指定されたファイルの拡張子が `--lang` と食い違う場合に true を設定
 * @param Excluded 走査で読まなかった経路の数の加算先
 * @return 指定パスが存在し走査を全う出来れば true（存在しないパス・走査の打切は呼出側が誤りとして扱う）
 */
static bool CollectFiles(
	const std::string &Path,
	const Lang OverrideLang,
	const bool IsAmbiguousOverride,
	std::vector<TargetFile> &Files,
	bool &IsFileUnsupported,
	bool &IsLangConflicting,
	ScanExclusions &Excluded
) {
	// 状態の１回取得と到達不能・自己参照リンクの誤りコードによる処理
	std::error_code StatusError;
	const Filesystem::file_status Status = Filesystem::status(Path, StatusError);
	// 不在経路の対応外言語と区別した失敗の返戻
	if(StatusError || !Filesystem::exists(Status)) return false;
	// 現在地の実体を基準に指定の全成分を検査し，リンク経由の書込を拒否して未読件数へ加算
	std::error_code AbsoluteError;
	const Filesystem::path Absolute = Filesystem::absolute(Path, AbsoluteError);
	// 絶対経路の取得失敗
	if(AbsoluteError) {
		std::fprintf(stderr, "Error: cannot inspect: %s: %s\n", FileIO::ForDisplay(Path).c_str(), AbsoluteError.message().c_str());
		// 絶対経路取得失敗時の返戻
		return false;
	}
	// 経路各階層のリンク検査
	Filesystem::path Prefix;
	for(const Filesystem::path &Part : Absolute) {
		Prefix /= Part;
		std::error_code LinkError;
		const Filesystem::file_status LinkStatus = Filesystem::symlink_status(Prefix, LinkError);
		// 階層情報の取得失敗
		if(LinkError) {
			std::fprintf(stderr, "Error: cannot inspect: %s: %s\n", FileIO::ForDisplay(Path).c_str(), LinkError.message().c_str());
			// 階層情報取得失敗時の返戻
			return false;
		}
		// 名前付経路がシンボリックリンクの場合
		if(Filesystem::is_symlink(LinkStatus)) {
			++Excluded.Links;
			++Excluded.NamedLinks;
			// 名前付リンク除外時の返戻
			return true;
		}
	}
	// 通常ファイルの場合
	if(Filesystem::is_regular_file(Status)) {
		// 単一の明示ファイルの言語判定
		const Lang ByPath = Lang::Detect(Path);
		// 確定した拡張子への別言語指定の拒否
		if(
			OverrideLang != Lang::Unknown && ByPath != Lang::Unknown && ByPath != OverrideLang.Id &&
			!Lang::IsAmbiguousCandidate(Path, OverrideLang)
		) {
			IsLangConflicting = true;
			// 言語指定競合時の返戻
			return true;
		}
		// 明示指定を反映した適用言語の確定
		if(
			const Lang Detected = OverrideLang != Lang::Unknown && ByPath != OverrideLang.Id ? OverrideLang : ByPath;
			Detected != Lang::Unknown
		) {
			Files.push_back(
				{
					Path,
					Detected,
					ByPath == Lang::Unknown && OverrideLang != Lang::Unknown,
					Lang::IsAmbiguousExtension(Path) ?
					OverrideLang == Lang::Unknown || IsAmbiguousOverride :
					IsAmbiguousOverride && ByPath == Lang::Unknown,
					{}
				}
			);
			// 未対応の通常ファイルを記録
		} else IsFileUnsupported = true;
		// ディレクトリの場合
	} else if(Filesystem::is_directory(Status)) {
		// ディレクトリ内で収集した経路の安定した辞書順への統一
		const size_t CollectStart = Files.size();
		std::error_code ScanError;
		const Filesystem::path ScanRoot = Filesystem::canonical(Path, ScanError);
		// 走査開始に失敗した場合
		if(ScanError) return false;
		// `canonical` が除く末尾区切文字を絶対経路側からも除いた比較用の実体根
		const Filesystem::path NormalizedAbsolute = Absolute.lexically_normal();
		const Filesystem::path ExpectedRoot = NormalizedAbsolute.has_filename() ? NormalizedAbsolute : NormalizedAbsolute.parent_path();
		// 成分検査後のリンク差替えに依る指定範囲外への実体根の移動防止
		if(ScanRoot != ExpectedRoot) {
			++Excluded.Links;
			++Excluded.NamedLinks;
			// 実体根移動時の返戻
			return true;
		}
		// ディレクトリエントリの走査
		Filesystem::recursive_directory_iterator Iter(ScanRoot, ScanError);
		const Filesystem::recursive_directory_iterator IterEnd;
		for(; Iter != IterEnd; Iter.increment(ScanError)) {
			std::string EntryPath = Iter->path().string();
			// 再生成される領域と点始まりのディレクトリの枝刈り
			std::error_code EntryError;
			if(Iter->is_directory(EntryError)) {
				static constexpr std::string_view ExcludedDirNames[] =
				{ "Carthage", "Pods", "bower_components", "build", "dist", "node_modules", "obj", "out", "target", "vendor", "venv" };
				const std::string DirName = Iter->path().filename().string();
				bool IsExcludedDir = !DirName.empty() && DirName.front() == '.';
				if(!IsExcludedDir) for(const std::string_view Name : ExcludedDirNames) if(MatchesIgnoringAsciiCase(DirName, Name)) {
					IsExcludedDir = true;
					break;
				}
				// 除外ディレクトリの場合
				if(IsExcludedDir) {
					Iter.disable_recursion_pending();
					++Excluded.ByName;
					continue;
				}
			}
			// シンボリックリンクの非追跡
			const Filesystem::file_status EntryStatus = Iter->symlink_status(EntryError);
			// エントリ状態の取得失敗
			if(EntryError) {
				std::fprintf(stderr, "Error: cannot inspect: %s: %s\n", FileIO::ForDisplay(EntryPath).c_str(), EntryError.message().c_str());
				// エントリ状態取得失敗時の返戻
				return false;
			}
			// 未追跡リンクの対応対象だけの計数
			if(Filesystem::is_symlink(EntryStatus)) {
				std::error_code TargetError;
				if(Filesystem::is_directory(Iter->status(TargetError)) || Lang::Detect(EntryPath) != Lang::Unknown) ++Excluded.Links;
				continue;
			}
			// 通常ファイル以外を除外
			if(!Filesystem::is_regular_file(EntryStatus)) continue;
			// 再生成される依存固定ファイルの名前除外
			static constexpr std::string_view ExcludedFileNames[] = { "composer.lock", "package-lock.json", "yarn.lock" };
			const std::string FileName = Iter->path().filename().string();
			// 圧縮済ファイル名かを判定
			bool IsExcludedFile = EndsWithIgnoringAsciiCase(FileName, ".min.js") || EndsWithIgnoringAsciiCase(FileName, ".min.css");
			// 固定除外名との照合
			if(!IsExcludedFile) for(const std::string_view Name : ExcludedFileNames) if(MatchesIgnoringAsciiCase(FileName, Name)) {
				// 除外対象の発見を記録
				IsExcludedFile = true;
				// 除外名の探索を終了
				break;
			}
			// 除外ファイルの場合
			if(IsExcludedFile) {
				// 名前除外数を加算
				++Excluded.ByName;
				// 次のエントリへ移動
				continue;
			}
			// 走査中の拡張子に依る言語決定
			if(const Lang Detected = Lang::Detect(EntryPath); Detected != Lang::Unknown) {
				const bool IsAmbiguous = Lang::IsAmbiguousExtension(EntryPath);
				std::string Display = (Filesystem::path(Path) / Iter->path().lexically_relative(ScanRoot)).string();
				Files.push_back({ std::move(EntryPath), Detected, false, IsAmbiguous, std::move(Display) });
			}
		}
		// 走査途中で失敗した場合
		if(ScanError) {
			std::fprintf(stderr, "Error: directory scan stopped: %s: %s\n", FileIO::ForDisplay(Path).c_str(), ScanError.message().c_str());
			// 走査失敗時の返戻
			return false;
		}
		// 今回追加した対象の整列
		std::sort(
			Files.begin() + static_cast<std::ptrdiff_t>(CollectStart),
			Files.end(),
			[](const TargetFile &FileA, const TargetFile &FileB) -> bool {
				// 経路の辞書順比較
				return FileA.Path < FileB.Path;
			}
		);
		// 明示した FIFO・端末・装置等を対象０件の成功として扱わない
	} else IsFileUnsupported = true;
	// 収集成功の返戻
	return true;
}

// 整列用のファイル情報構造体（大きさで並べつつ走査順を保持する）
struct SizedFile {
	uintmax_t Size;
	size_t Order; // 走査順の添字
	TargetFile Target;
};

// 対象ファイルパスは `Files` と添字で対応（パス文字列の複製保持を避ける）
struct FileJobResult {
	FormatResult Format;
	bool IsReadOk = false;
	bool IsWriteFailed = false;
	std::error_code WriteError; // 書込に失敗した処理系の理由（競合・照合の失敗では空）
	size_t BomCount = 0; // 原本の先頭に在った BOM の個数（書戻で同じ数を復元する）
	bool HadCrLf = false; // 原本の改行が CRLF だったか（書戻で復元する）
	bool HadCrOnly = false; // 原本が CR 単独で行を区切って居たか（正規化で LF へ変わる）
	bool IsFoldedCrLf = false; // CRLF と LF の混在する原本を LF へ揃えたか
	bool IsHardLinkBroken = false; // 書戻で別名との実体共有が切り離されたか
	std::string Original; // 差分表示用の原文
	std::string Text; // 整形対象及び結果
};

/**
 * CRLF の個数の計数関数
 * 計算量：原稿の長さ N に対し O(N)（一度だけ走査する）
 * @param Text 数える対象
 * @return CRLF の個数
 */
static size_t CountCrLf(const std::string_view Text) {
	// CRLF の計数
	size_t Count = 0;
	// CRLF 位置の走査
	for(size_t Found = Text.find("\r\n"); Found != std::string_view::npos; Found = Text.find("\r\n", Found + 2)) ++Count;
	// CRLF の個数を返戻
	return Count;
}

/**
 * 符号化・改行様式の告知関数（規約が求める UTF-8（BOM 無）・LF との差を標準エラーへ）
 * @param Result 告知の材料を持つ整形結果
 * @param PathLabel ファイル識別ラベル（行頭表示用）
 * @return 出した警告の件数
 */
static size_t PrintEncodingNotices(const FileJobResult &Result, const char *const PathLabel) {
	// 実際に出した告知数
	size_t Count = 0;
	// UTF-8 BOM が有る場合
	if(Result.BomCount) {
		std::fprintf(stderr, "[warn] %s:1:1: File has a UTF-8 BOM\n", PathLabel);
		++Count;
	}
	// CRLF 原本の場合
	if(Result.HadCrLf) {
		std::fprintf(stderr, "[warn] %s:1:1: File uses CRLF line endings\n", PathLabel);
		++Count;
	}
	// CR のみの改行が有る場合
	if(Result.HadCrOnly) {
		std::fprintf(stderr, "[warn] %s:1:1: File uses CR-only line endings; they become LF\n", PathLabel);
		++Count;
	}
	// 混在改行を LF へ畳んだ場合
	if(Result.IsFoldedCrLf) {
		std::fprintf(stderr, "[warn] %s:1:1: File mixes CRLF and LF line endings; they become LF outside literals\n", PathLabel);
		++Count;
	}
	// 符号化警告数を返戻
	return Count;
}

/**
 * 規約違反警告出力関数（`[warn] file:line:col: <メッセージ>` 形式で標準エラーへ）
 * @param Warnings 警告メッセージ列
 * @param FileLabel ファイル識別ラベル（行頭表示用）
 */
static void PrintWarnings(const std::vector<LintWarning> &Warnings, const char *const FileLabel) {
	// 安全な表示名による警告列の出力
	const std::string SafeLabel = FileIO::ForDisplay(FileLabel);
	for(const LintWarning &Warning : Warnings) {
		std::fprintf(
			stderr,
			"[warn] %s:%u:%u: %s\n",
			SafeLabel.c_str(),
			Warning.Row + 1,
			Warning.Column + 1,
			FileIO::ForDisplay(Warning.Message).c_str()
		);
	}
	// 終了
	return;
}

/**
 * 除外件数出力関数（要約行の末尾へ付ける）
 * @param Excluded 走査で読まなかった経路の数
 */
static void PrintExcluded(const ScanExclusions &Excluded) {
	// 未検査件数を含む除外要約の出力
	if(Excluded.ByName) std::printf(", %zu path(s) excluded by name", Excluded.ByName);
	if(Excluded.Links) std::printf(", %zu symbolic link(s) not followed", Excluded.Links);
	// 要約行の終端
	std::puts(".");
	// 終了
	return;
}

/**
 * ファイル一覧整形関数（結果を入力順に格納）
 * 計算量：入力総量 N に対し概ね O(N)
 * @param Files 対象ファイル一覧（言語は収集時に確定済）
 * @param Sizes 各ファイルの大きさ（`Files` と添字で対応，並列に抱える入力の総量の制御に使う）
 * @param ShouldWrite true で整形結果をファイルへ書き戻す
 * @param KeepsOriginal true で整形前のテキストを `Original` に保持（差分表示用）
 * @param SkipsLint true で規約検査を省略
 * @return 各ファイルの処理結果（`Files` と添字で対応）
 */
static std::vector<FileJobResult> ProcessFiles(
	const std::vector<TargetFile> &Files,
	const std::vector<uintmax_t> &Sizes,
	const bool ShouldWrite,
	const bool KeepsOriginal,
	const bool SkipsLint
) {
	// 結果領域と並列実行の構成
	std::vector<FileJobResult> Results(Files.size());
	const size_t NumThreads = Parallel::DecideThreads(Files.size(), 12, 2);
	std::atomic<size_t> NextIdx(0);
	std::mutex BudgetLock;
	std::condition_variable BudgetFreed;
	uintmax_t InFlight = 0;
	// ファイル処理の並列実行
	Parallel::ForChunks(
		NumThreads,
		NumThreads,
		[&](size_t, size_t, size_t) -> void {
			// 未処理ファイルの取得ループ
			while(true) {
				// 此の走脈へ割り当てた対象
				const size_t Idx = NextIdx.fetch_add(1, std::memory_order_relaxed);
				// 全ファイル取得後に終了
				if(Idx >= Files.size()) break;
				// 不明な大きさを上限と看做した同時処理量の算出
				const uintmax_t Weight = std::min<uintmax_t>(Sizes[Idx], FileIO::MaxInputBytes);
				{
					// 待機中に解放出来る施錠
					std::unique_lock<std::mutex> Lock(BudgetLock);
					BudgetFreed.wait(
						Lock,
						[&InFlight, Weight]() -> bool {
							// 総量に収まるか，他に処理中の物が無いかの返戻
							return !InFlight || InFlight + Weight <= FileIO::MaxInputBytes;
						}
					);
					InFlight += Weight;
				}
				// 全終了経路での保有枠返却と待機者の起動
				struct BudgetRelease {
					const uintmax_t Weight; // 此のファイルが抱えた分
					std::mutex &Lock; // 総量の排他
					std::condition_variable &Freed; // 空きを待つ者への通知
					uintmax_t &Total; // 処理中の入力の大きさの和

					/**
					 * デストラクタ（抱えた分を総量から返し，空きを待つ者を起こす）
					 */
					~BudgetRelease() {
						{
							// 総量更新の排他
							const std::lock_guard<std::mutex> Guard(Lock);
							Total -= Weight;
						}
						Freed.notify_all();
						// 終了
						return;
					}
				} const Release { Weight, BudgetLock, BudgetFreed, InFlight };
				// 此の対象専用の結果欄
				FileJobResult &Result = Results[Idx];
				// 例外が書込失敗に当たる段階の記録
				bool IsWriting = false;
				try {
					// 原本の同一性を監視する入出力口
					const FileIO File(Files[Idx].Path);
					if(!File.ReadFile(Result.Text, &Result.BomCount)) continue;
					Result.IsReadOk = true;
					// BOM・改行様式の保存
					const bool IsMixed = FileIO::IsNewlineMixed(Result.Text);
					// 正規化前の改行対数
					const size_t CrLfBefore = IsMixed ? CountCrLf(Result.Text) : 0;
					Result.HadCrLf = !IsMixed && Result.Text.find("\r\n") != std::string::npos;
					Result.HadCrOnly = FileIO::IsCrOnlyNewline(Result.Text);
					if(KeepsOriginal) Result.Original = Result.Text;
					Result.Format = Formatter::Format(Result.Text, Files[Idx].Language, SkipsLint, Files[Idx].IsLangAmbiguous);
					// リテラル外 CRLF の正規化時だけの通知
					Result.IsFoldedCrLf = IsMixed && Result.Format.Outcome == FormatOutcome::Changed && CountCrLf(Result.Text) < CrLfBefore;
					// 拡張子不明への言語指定で構文破損が出た時は，取違に依る内容破壊を防ぐ為に書戻せず見送
					if(Files[Idx].IsLangAssumed && Result.Format.Outcome == FormatOutcome::Changed && Result.Format.HasInputError) {
						Result.Format.Outcome = FormatOutcome::Skipped;
						Result.Format.Warnings.clear();
						Result.Format.SkipReason = "--lang does not match the file content";
					}
					if(ShouldWrite && Result.Format.Outcome == FormatOutcome::Changed) {
						IsWriting = true;
						// BOM・改行様式の復元と書戻
						if(Result.HadCrLf || Result.BomCount) {
							// 原本体裁を戻す書込内容の生成
							std::string Out = KeepsOriginal ? Result.Text : std::move(Result.Text);
							if(Result.HadCrLf) FileIO::ExpandLf(Out);
							if(Result.BomCount) {
								// 原本と同数の先頭印
								std::string Bom;
								Bom.reserve(3 * Result.BomCount);
								for(size_t BomIdx = 0; BomIdx < Result.BomCount; ++BomIdx) Bom += "\xEF\xBB\xBF";
								Out.insert(0, Bom);
							}
							Result.IsWriteFailed = !File.WriteFile(Out, &Result.IsHardLinkBroken, &Result.WriteError);
						} else Result.IsWriteFailed = !File.WriteFile(Result.Text, &Result.IsHardLinkBroken, &Result.WriteError);
					}
				} catch(const std::exception &) {
					Result.IsReadOk = true;
					Result.Format.Outcome = FormatOutcome::Failed;
					Result.IsWriteFailed = IsWriting;
					Result.Format.Warnings.clear();
				}
				// 差分非表示時は次のファイルの前に整形結果を解放（全対象量に比例する常駐量を抑制）
				if(!KeepsOriginal) {
					Result.Text.clear();
					Result.Text.shrink_to_fit();
				}
			}
			// 終了
			return;
		}
	);
	// 各ファイルの処理結果の返戻
	return Results;
}

/**
 * エントリポイント関数（指定ファイル／ディレクトリの整形，`-w` 書込／`-c` チェック／`--diff` 差分表示）
 * @param Argc コマンドライン引数の数
 * @param Argv コマンドライン引数の配列
 * @return 正常終了時 0，処理失敗又は要修正時 1，引数不正時 2
 */
int main(const int Argc, char *const Argv[]) {
	// Windows 標準入出力のバイナリ化
	#if defined(_WIN32)
	// 改行の二重展開と制御文字での読込打切を防ぎ，入出力のバイト列を保持する（標準エラーも LF の儘出し，拡張機能が行を分ける）
	if(
		_setmode(_fileno(stdin), _O_BINARY) < 0 || _setmode(_fileno(stdout), _O_BINARY) < 0 || _setmode(_fileno(stderr), _O_BINARY) < 0
	) {
		std::fputs("Error: failed to configure binary standard streams\n", stderr);
		// 入出力の完全性を保証出来ない場合の失敗返戻
		return 1;
	}
	#endif
	// Linux の主走脈を可能な範囲で `64 MiB` へ拡張（macOS / Windows はリンカ指定済）
	#if !defined(_WIN32) && !defined(__APPLE__)
	constexpr rlim_t MinimumStackLimit = 0X2000000ULL; // 深い入力を安全に検査出来る最低 32 MiB
	constexpr rlim_t StackLimit = 0X4000000ULL; // 目標 64 MiB
	if(struct rlimit Rlim; getrlimit(RLIMIT_STACK, &Rlim)) {
		std::fprintf(stderr, "Error: could not inspect stack limit (errno %d)\n", errno);
		// 安全な走脈量を確認出来ない場合の失敗返戻
		return 1;
	} else if(Rlim.rlim_cur < StackLimit) {
		Rlim.rlim_cur = Rlim.rlim_max == RLIM_INFINITY ? StackLimit : std::min(StackLimit, Rlim.rlim_max);
		if(setrlimit(RLIMIT_STACK, &Rlim) && Rlim.rlim_cur < MinimumStackLimit) {
			std::fprintf(stderr, "Error: could not increase stack limit (errno %d)\n", errno);
			// 最低走脈量を確保出来ない場合の失敗返戻
			return 1;
		}
		if(getrlimit(RLIMIT_STACK, &Rlim) || Rlim.rlim_cur < MinimumStackLimit) {
			std::fputs("Error: stack limit is below 32 MiB\n", stderr);
			// 適用後も最低走脈量に届かない場合の失敗返戻
			return 1;
		}
	}
	#endif
	// 矛盾を検査済の実行設定
	const Options Opts = ParseArgs(Argc, Argv);
	// コメントを除く検査設定の標準入力だけへの限定
	if(!Opts.ReadsStdin && LayoutPass::DropsComments()) {
		std::fputs("Error: SHAVEFMT_DROP_COMMENTS is only for --stdin (it drops every comment from the output)\n", stderr);
		// 検査用の設定をファイルへ効かせない使用法違反の返戻
		return UsageErrorCode;
	}
	LineSplitPass::SetMaxChars(Opts.MaxChars);
	// 標準入力モードでは VSCode 拡張等の埋込利用向に `--lang` 必須
	if(Opts.ReadsStdin) {
		if(Opts.OverrideLang == Lang::Unknown) {
			std::fprintf(stderr, "Error: --stdin requires --lang ID\n");
			// `--stdin` で `--lang` 指定不在の使用法違反としての返戻
			return UsageErrorCode;
		}
		// 標準入力の一括読込
		std::string Source;
		// 64 KiB
		char Chunk[0X10000];
		while(const size_t ReadLen = std::fread(Chunk, 1, sizeof(Chunk), stdin)) {
			// 入力追加前の待機列残容量の確認
			if(ReadLen > FileIO::MaxInputBytes - Source.size()) {
				std::fputs("Error: stdin input exceeds the size limit\n", stderr);
				// 入力が大き過ぎる事の返戻
				return 1;
			}
			Source.append(Chunk, ReadLen);
		}
		// 読込途中で失敗した内容を整形すると，切り詰められた文書を「整形結果」として返す事への変化
		if(std::ferror(stdin)) {
			std::fputs("Error: failed to read stdin\n", stderr);
			// 入力が欠けた事の返戻
			return 1;
		}
		// 先頭 BOM の解析前除去と復元数の記録
		const size_t BomCount = FileIO::StripBom(Source);
		// 標準入力経路の符号化・改行差の告知材料
		FileJobResult Notices;
		Notices.BomCount = BomCount;
		// 元入力の改行混在有無
		const bool IsMixed = FileIO::IsNewlineMixed(Source);
		Notices.HadCrLf = !IsMixed && Source.find("\r\n") != std::string::npos;
		Notices.HadCrOnly = FileIO::IsCrOnlyNewline(Source);
		// 正規化前の改行対数の記録
		const size_t CrLfBefore = IsMixed ? CountCrLf(Source) : 0;
		// 確定した明示言語の使用と h だけの内容判別
		const FormatResult Result = Formatter::Format(Source, Opts.OverrideLang, Opts.SkipsLint, Opts.IsLangAmbiguous);
		// 整形未実施経路の明示
		switch(Result.Outcome) {
		case FormatOutcome::Changed:
		case FormatOutcome::Unchanged:
			break;
		case FormatOutcome::Skipped:
			std::fprintf(stderr, "[skip] <stdin>: %.*s\n", static_cast<int>(Result.SkipReason.size()), Result.SkipReason.data());
			break;
		case FormatOutcome::Defect:
			// 想定外の防壁発火時の原文返戻と不具合通知
			std::fprintf(
				stderr,
				"Error: formatter defect, left unchanged: <stdin>: %.*s\n",
				static_cast<int>(Result.SkipReason.size()),
				Result.SkipReason.data()
			);
			break;
		case FormatOutcome::SyntaxGuard:
			// 構文破壊に依る差戻の無変更との区別
			std::fprintf(stderr, "Error: formatting would break syntax, left unchanged: <stdin>\n");
			break;
		case FormatOutcome::Failed:
			std::fprintf(
				stderr,
				"Error: formatting could not be completed (out of memory or threads, or an internal error), left unchanged: <stdin>\n"
			);
			break;
		default:
			std::fputs("Error: formatter returned an unknown outcome\n", stderr);
			// 未知の結果を成功扱いしない為の失敗返戻
			return 1;
		}
		// 文書内容の BOM は書戻と同じく復元し，改行様式は呼出側に委譲（拡張機能が送信前に CR を除き受信後に復元）
		if(BomCount) {
			// 原本と同数の先頭印
			std::string Bom;
			Bom.reserve(3 * BomCount);
			for(size_t BomIdx = 0; BomIdx < BomCount; ++BomIdx) Bom += "\xEF\xBB\xBF";
			Source.insert(0, Bom);
		}
		// 混在の告知はリテラルの外の CRLF を実際に畳んだ時だけ出す（ファイルの経路と同じ判定）
		Notices.IsFoldedCrLf = IsMixed && CountCrLf(Source) < CrLfBefore;
		PrintEncodingNotices(Notices, "<stdin>");
		PrintWarnings(Result.Warnings, "<stdin>");
		// 部分書込と書出失敗での非零終了による文書切詰め防止
		if(std::fwrite(Source.data(), 1, Source.size(), stdout) != Source.size() || std::fflush(stdout)) {
			std::fputs("Error: failed to write stdout\n", stderr);
			// 出力が欠けた事の返戻
			return 1;
		}
		// 原文を失わない様に標準出力へ結果を渡し，差戻又は指定時の見送だけを非零とする終了状態の返戻
		return Result.Outcome == FormatOutcome::SyntaxGuard || Result.Outcome == FormatOutcome::Defect ||
		Result.Outcome == FormatOutcome::Failed || Opts.FailsOnSkip && Result.Outcome == FormatOutcome::Skipped ? 1 : 0;
	}
	if(Opts.Paths.empty()) {
		PrintUsage(true);
		// 引数無で使用法表示後，使用法の誤りとしての返戻
		return UsageErrorCode;
	}
	// 重複除去前の整形対象
	std::vector<TargetFile> Files;
	// 経路指定の誤り
	bool IsMissingPath = false, IsUnsupportedPath = false, IsConflictingPath = false;
	// 走査で読まなかった経路数
	ScanExclusions Excluded;
	for(const std::string &Path : Opts.Paths) {
		// 此の明示経路の不適合
		bool IsUnsupported = false, IsConflicting = false;
		if(!CollectFiles(Path, Opts.OverrideLang, Opts.IsLangAmbiguous, Files, IsUnsupported, IsConflicting, Excluded)) {
			std::fprintf(stderr, "Error: path not found: %s\n", FileIO::ForDisplay(Path).c_str());
			IsMissingPath = true;
		} else if(IsConflicting) {
			std::fprintf(stderr, "Error: --lang conflicts with the file extension: %s\n", FileIO::ForDisplay(Path).c_str());
			IsConflictingPath = true;
		} else if(IsUnsupported) {
			std::fprintf(stderr, "Error: unsupported file type: %s\n", FileIO::ForDisplay(Path).c_str());
			IsUnsupportedPath = true;
		}
	}
	// 不在・対応外・言語不一致を非零終了として偽合格を防止（不一致は道具の誤用），言語不一致時の使用法誤りの返戻
	if(IsConflictingPath) return UsageErrorCode;
	// 対象経路が使えない場合の失敗返戻
	if(IsMissingPath || IsUnsupportedPath) return 1;
	if(Files.empty()) {
		// 対象０件を非零で報告（パターンの誤記・拡張子漏れを合格扱いにせず，名前での除外件数も併記）
		if(Opts.ShouldCheck) {
			std::fputs("Error: no supported files found\n", stderr);
			if(Excluded.ByName) std::fprintf(stderr, "%zu path(s) excluded by name\n", Excluded.ByName);
			if(Excluded.Links) std::fprintf(stderr, "%zu symbolic link(s) not followed\n", Excluded.Links);
			// `--check` で検査対象が皆無だった異常として終了コード 1 の返戻
			return 1;
		}
		std::printf("No supported files found");
		PrintExcluded(Excluded);
		// 対応言語のファイルが無い事の報告結果の返戻（名指したリンクを読まなかった場合は `--fail-on-skip` で失敗）
		return FlushStdout() || Opts.FailsOnSkip && Excluded.NamedLinks;
	}
	// 正規化経路で `a.c`・`./a.c` 等を重複除去し並列書込の競合を防止（単一経路指定は重複しない為，成分毎のリンク解決も省略）
	if(Opts.Paths.size() > 1) {
		// 実体基準の既出経路
		std::unordered_set<std::string> SeenPaths;
		SeenPaths.reserve(Files.size());
		std::erase_if(
			Files,
			[&SeenPaths](const TargetFile &File) -> bool {
				// 重複鍵の正規化失敗理由
				std::error_code KeyError;
				const Filesystem::path Key = Filesystem::weakly_canonical(File.Path, KeyError);
				// 正規化出来ないパスは元の文字列を鍵にする（少なくとも完全一致の重複は除ける）既出の経路なら除外する事の返戻
				return !SeenPaths.insert(KeyError ? File.Path : Key.string()).second;
			}
		);
	}
	// 寸法の１回取得と収集順を添えた対象列の生成
	std::vector<SizedFile> SizedFiles;
	SizedFiles.reserve(Files.size());
	for(size_t Idx = 0; Idx < Files.size(); ++Idx) {
		// 寸法取得の失敗理由
		std::error_code ErrorCode;
		SizedFiles.push_back({ Filesystem::file_size(Files[Idx].Path, ErrorCode), Idx, std::move(Files[Idx]) });
	}
	std::sort(
		SizedFiles.begin(),
		SizedFiles.end(),
		[](const SizedFile &FileA, const SizedFile &FileB) -> bool {
			// サイズ降順比較で A の方が大きければ true の返戻
			return FileA.Size > FileB.Size;
		}
	);
	// 収集順から処理順への写像
	std::vector<size_t> ReportOrder(SizedFiles.size());
	// 処理順の入力寸法
	std::vector<uintmax_t> Sizes(SizedFiles.size());
	for(size_t Idx = 0; Idx < SizedFiles.size(); ++Idx) {
		ReportOrder[SizedFiles[Idx].Order] = Idx;
		Sizes[Idx] = SizedFiles[Idx].Size;
		Files[Idx] = std::move(SizedFiles[Idx].Target);
	}
	std::vector<FileJobResult> Results = ProcessFiles(Files, Sizes, Opts.ShouldWrite, Opts.ShowsDiff, Opts.SkipsLint);
	// 要約の分類別件数
	size_t ChangedCount = 0, FailureCount = 0, SkippedCount = 0, WarningCount = 0;
	for(const size_t ResIdx : ReportOrder) {
		// 収集順で報告する対象結果
		FileJobResult &Result = Results[ResIdx];
		// `Files` の添字に対応する表示経路の作成（利用者が仕込める端末制御列を１度だけ無害化）
		const std::string SafePath = FileIO::ForDisplay(Files[ResIdx].Display.empty() ? Files[ResIdx].Path : Files[ResIdx].Display);
		// 出力中だけ有効な表示名
		const char *const PathLabel = SafePath.c_str();
		// 標準エラーの前に標準出力を掃出し，`2>&1` の通知順を保持
		std::fflush(stdout);
		// 上限を超える大きさのファイルは読込が拒む為，読込の失敗でなく入力ガードの見送として理由を添えて報告
		if(!Result.IsReadOk && Sizes[ResIdx] != static_cast<uintmax_t>(-1) && Sizes[ResIdx] > FileIO::MaxInputBytes) {
			std::fprintf(stderr, "[skip] %s: input exceeds size limit\n", PathLabel);
			++SkippedCount;
			continue;
		}
		// 読取失敗ファイルでの偽の合格を防ぐ非零終了
		if(!Result.IsReadOk) {
			std::fprintf(stderr, "Error reading: %s\n", PathLabel);
			++FailureCount;
			continue;
		}
		// 書込失敗ファイルの整形済件数からの除外と終了コード通知
		if(Result.IsWriteFailed) {
			// 権限・容量・競合を区別出来る書込失敗理由の表示
			const std::string Reason =
			Result.WriteError ? Result.WriteError.message() : "the file changed on disk or could not be verified";
			std::fprintf(stderr, "Error writing: %s: %s\n", PathLabel, Reason.c_str());
			++FailureCount;
			continue;
		}
		switch(Result.Format.Outcome) {
		// 構文破壊の差戻を明示して非零終了（偽合格を防ぎ，壊れた中間木に由来する規約警告は非表示）
		case FormatOutcome::SyntaxGuard:
			std::fprintf(stderr, "Error: formatting would break syntax, left unchanged: %s\n", PathLabel);
			++FailureCount;
			continue;
		// 発火してはならない防壁も，原文を保った上で不具合として非零で終える（見送と数えると検査工程が整形器の不具合を見逃す）
		case FormatOutcome::Defect:
			std::fprintf(
				stderr,
				"Error: formatter defect, left unchanged: %s: %.*s\n",
				PathLabel,
				static_cast<int>(Result.Format.SkipReason.size()),
				Result.Format.SkipReason.data()
			);
			++FailureCount;
			continue;
		// 資源枯渇又は内部誤りによる整形失敗時の原本保持と非零終了
		case FormatOutcome::Failed:
			std::fprintf(
				stderr,
				"Error: formatting could not be completed (out of memory or threads, or an internal error), left unchanged: %s\n",
				PathLabel
			);
			++FailureCount;
			continue;
		// 入力ガードの未検査を必ず明示し，仕様上の対象外として処理失敗と別に計数
		case FormatOutcome::Skipped:
			std::fprintf(
				stderr,
				"[skip] %s: %.*s\n",
				PathLabel,
				static_cast<int>(Result.Format.SkipReason.size()),
				Result.Format.SkipReason.data()
			);
			++SkippedCount;
			continue;
		case FormatOutcome::Changed:
		case FormatOutcome::Unchanged:
			break;
		default:
			std::fprintf(stderr, "Error: formatter returned an unknown outcome: %s\n", PathLabel);
			++FailureCount;
			continue;
		}
		// 原本の符号化と改行様式保持時の規約との差の警告
		WarningCount += PrintEncodingNotices(Result, PathLabel);
		// 差替で実体共有が切れる別名ファイルの警告
		if(Result.IsHardLinkBroken) {
			std::fprintf(stderr, "[warn] %s: hard link was broken; the other names keep the old content\n", PathLabel);
			++WarningCount;
		}
		PrintWarnings(Result.Format.Warnings, PathLabel);
		WarningCount += Result.Format.Warnings.size();
		if(Result.Format.Outcome == FormatOutcome::Changed) {
			++ChangedCount;
			if(Opts.ShowsDiff) {
				std::printf("--- %s\n", PathLabel);
				// 差分表示時だけ行配列化（CRLF・旧 CR 改行は警告し，行内容比較の為に LF へ正規化）
				FileIO::Normalize(Result.Original, true);
				const std::vector<std::string> OldLines = FileIO::SplitLines(Result.Original), NewLines = FileIO::SplitLines(Result.Text);
				// 片側だけに在る行を空行とした比較値
				const std::string Empty;
				// 比較する行数
				const size_t MaxLines = std::max(OldLines.size(), NewLines.size());
				for(size_t LineIdx = 0; LineIdx < MaxLines; ++LineIdx) {
					if(
						const std::string &OldLine = LineIdx < OldLines.size() ? OldLines[LineIdx] : Empty,
						&NewLine = LineIdx < NewLines.size() ? NewLines[LineIdx] : Empty;
						OldLine != NewLine
					) {
						std::printf(
							"L%zu:\n  - %s\n  + %s\n",
							LineIdx + 1,
							FileIO::ForDisplay(OldLine, true).c_str(),
							FileIO::ForDisplay(NewLine, true).c_str()
						);
					}
				}
			}
			if(Opts.ShouldWrite) std::printf("Formatted: %s\n", PathLabel);
			// 差分表示外でも要修正の経路を示し，検査記録へ対象の保持
			else if(!Opts.ShowsDiff) std::printf("Would format: %s\n", PathLabel);
		}
	}
	if(!Opts.ShouldCheck) {
		std::printf("Done. %zu/%zu file(s) %s", ChangedCount, Files.size(), Opts.ShouldWrite ? "formatted" : "would be formatted");
		// 処理出来なかったファイルは終了コードだけでなく要約行にも出す（記録から失敗の存在が消える事態を避ける）
		if(FailureCount) std::printf(", %zu file(s) could not be processed", FailureCount);
		if(SkippedCount) std::printf(", %zu file(s) skipped", SkippedCount);
		// 警告件数の要約への追加
		if(WarningCount) std::printf(", %zu warning(s)", WarningCount);
		// 見送ったファイルは `--fail-on-skip` の指定時だけ検査失敗としての取扱
	} else if(ChangedCount || FailureCount || Opts.FailsOnSkip && (SkippedCount || Excluded.NamedLinks)) {
		// 要修正と処理失敗の何れも要約行へ出す（検査ログに結果が残らない事態を避ける）
		std::printf(
			"%zu file(s) need formatting, %zu file(s) could not be processed, %zu file(s) skipped, %zu warning(s)",
			ChangedCount,
			FailureCount,
			SkippedCount,
			WarningCount
		);
		// 合格時にも未検査件数の要約への出力
	} else if(SkippedCount) {
		std::printf("All checked files conform, %zu file(s) skipped (not checked), %zu warning(s)", SkippedCount, WarningCount);
	} else std::printf("All files are formatted, %zu warning(s)", WarningCount);
	PrintExcluded(Excluded);
	// 出力欠落・書込失敗・構文破壊時の非零終了と未検査扱い
	if(
		FlushStdout() || FailureCount || Opts.FailsOnSkip && (SkippedCount || Excluded.NamedLinks) || Opts.ShouldCheck && ChangedCount
		// 出力失敗・処理失敗又は検査不合格の返戻
	) return 1;
	// 全パス処理成功の返戻
	return 0;
}
