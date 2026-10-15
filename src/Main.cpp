#include "Formatter.hpp"
#include "Version.hpp"
#include "Core/LineBreaker.hpp"
#include "Lint/LintAnalyzer.hpp"
#include "Profile/GrammarProfile.hpp"
#include "Util/FileIO.hpp"
#include "Util/FormatLimitExceeded.hpp"
#include "Util/Lang.hpp"
#include "Util/LineDiff.hpp"
#include "Util/Parallel.hpp"
#include "Util/ParserAllocator.hpp"
#include "Util/WorkingMemory.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(ShaveFormatInterruptionTestHooks)

#include <thread>

#endif

#if defined(_WIN32)

#include <fcntl.h>
#include <io.h>
#include <windows.h>

#else

#include <signal.h>
#include <unistd.h>

#endif

// ファイルシステム名前空間の別名
namespace Filesystem = std::filesystem;

// 検査工程がツールの誤用と対象の未整形を区別する使用法エラーの終了コード
static constexpr int UsageErrorCode = 2;
// 最初の中断信号を，後始末を終えた主スレッドへ受け渡す
static std::atomic<int> InterruptedSignal(0);

static_assert(std::atomic<int>::is_always_lock_free);

/**
 * 中断要求を記録する信号ハンドラー
 * 計算量：O(1)
 * @param Signal 受けた中断信号（信号毎の通知は行わない）
 */
static void RecordInterruption(const int Signal) {
	const int SavedError = errno;
	std::signal(Signal, SIG_DFL);
	int Previous = 0;
	if(!InterruptedSignal.compare_exchange_strong(Previous, Signal)) {
		#if defined(_WIN32)
		std::_Exit(1);
		#else
		::kill(::getpid(), Signal);
		errno = SavedError;
		// 此の処理器で遮断中の信号は，戻った時に既定の動作で終わる
		return;
		#endif
	}
	ParserAllocator::RequestInterruption();
	errno = SavedError;
	// 終了
	return;
}

#if defined(_WIN32)

using ArgChar = wchar_t; // 起動引数の文字（Unicode の起動引数を受け取る）

#else

using ArgChar = char; // 起動引数の文字

#endif

// 引数の解釈，入出力及び設定済の工程の構築を行うクラス（整形仕様 3.1，変えるのは外部入出力の状態だけ）
class Main {
private:

	struct ReadOnlyReplay; // 書戻を行わない同じ実体・文法の結果の前方宣言

	using ReplayKey = std::pair<FileIdentity, const GrammarProfile *>; // 実体と入口文法の組
	using InspectionCache = std::map<ReplayKey, FileIO::InputInspection>; // 同じ実体・入口文法の受理計数と確認済の状態
	using ReplayCache = std::map<ReplayKey, ReadOnlyReplay>; // 差分表示の呼出間で保持する結果
	using ReplaySnapshot = std::unique_ptr<std::FILE, decltype(&std::fclose)>; // 原文と候補を持つ一時ストリームの所有権
	using NativeNameView = std::basic_string_view<Filesystem::path::value_type>; // 名前を判定する処理系のコードユニット列

	enum class PathRejection : uint8_t { None, LangConflict, NotRegularFile, UnknownLang }; // 明示パスの不受理理由（理由毎に案内を分ける為の分類） // 対象に出来た物 // `--lang` が確定した拡張子と食い違う状態 // FIFO・端末・装置等 // 言語が決まらない通常ファイル

	// 差分表示は原文・候補を一時ストリームへ退避し，入力量に比例する常駐領域を持たない
	struct ReadOnlyReplay {
		size_t Remaining = 0; // 未報告の同じ実体・文法のパス数
		FormatResult Format; // パスに依存しない分類・警告・診断（本文はストリームが持つ）
		std::optional<long> Offset; // 共通ストリーム中の原文，完成候補の開始位置
		size_t OriginalBytes = 0; // 原文の長さ
		size_t OutputBytes = 0; // 完成候補の長さ
		bool HasTextChange = false; // 原文と完成候補が異なるか
	};

	// 別名数に依らず１つのストリームを所有し，退避量を入力１件の上限以内に抑える
	struct ReplayStore {
		ReplayCache Entries; // 実体・文法毎の記録
		ReplaySnapshot Snapshot { nullptr, &std::fclose }; // 全記録で共有する原文と候補の退避先
		size_t Bytes = 0; // 失敗した書出も含む予約済の退避量
		size_t Stored = 0; // 退避を参照する未完了の記録数
		bool HasOpened = false; // 作成失敗時は再試行せず再整形する為の記録
	};

	struct PathEncodingFailure {
		Filesystem::path Path; // エンコーディングの変換を行わずに保持する起動引数の名前
	};

	// 対象ファイルと其の言語（`--lang` の上書は明示指定した単一ファイルにのみ及ぶ為，収集時に確定させて持ち回る）
	struct TargetFile {
		std::string Path;
		::Lang Lang;
		std::string Display; // 報告に使うパス（空なら `Path`）
		std::vector<FileIdentity> Identity; // 収集時の親成分と対象の実体
		bool HasUnknownIdentity = false; // 非対応の識別 API を実体共有の根拠から外すか
		std::error_code ReadError = std::error_code(); // 正規化の失敗も対象毎の読込失敗として保持する
	};

	// コマンドライン指定構造体
	struct Options {
		bool ShouldExit = false; // 入力を処理せず共通の終了処理へ戻るか
		int ExitCode = 0; // 解析中の終了要求の状態
		bool ShouldWrite = false;
		bool ShouldCheck = false;
		bool ShouldShowDiff = false;
		bool ShouldReadStdin = false;
		bool ShouldSkipLint = false;
		bool ShouldFailOnSkip = false;
		size_t MaxChars = 128; // 行最大文字数（`0` は無制限）
		::Lang OverrideLang = Lang::Get(Lang::Unknown); // `--lang` 指定値（`Lang::Unknown` は未指定）
		std::vector<std::string> Paths; // 指定された入力パス列
	};

	// ディレクトリ走査で読まなかったパスの数（読まれて居ない事を要約へ出し，検査工程が「全て検査した」と読まない様にする）
	struct ScanExclusions {
		size_t ByName = 0; // 名前で除いたパスの数
		size_t Links = 0; // 未読リンク数（明示パスと走査中の対象ファイル・ディレクトリ）
		size_t NamedLinks = 0; // 明示リンク数（`--fail-on-skip` では未検査の失敗扱い）
		size_t ReadFailures = 0; // 名指し又は走査中に報告した読込失敗数
	};

	// 対象ファイルパスは `Files` と添字で対応（パス文字列の複製保持を避ける）
	struct FileJobResult {
		FormatResult Format;
		bool IsCancelled = true; // 未割当の対象は中断時に未処理として集計する
		bool IsReadOk = false;
		bool IsWriteFailed = false;
		std::error_code ReadError; // 読込失敗の原因
		std::error_code WriteError; // 書込に失敗した処理系の理由（競合・照合の失敗では空）
		bool HasTextChange = false; // 整形結果のテキストが読込時の原文と異なるか
		uint64_t LinkCount = 0; // 分離前の同じ実体を指す名前の数
		bool IsHardLinkBroken = false; // 書戻で別名との実体共有が切り離されたか
		std::string Original; // 差分表示用の原文
		std::string Text; // 整形対象及び結果
	};

	// 整列用のファイル情報構造体（大きさで並べつつ走査順を保持する）
	struct SizedFile {
		uintmax_t Size;
		size_t Order; // 走査順の添字
		TargetFile Target;
	};

	static int FlushStdout(); // 標準出力の確定関数
	static void PrintUsage(const bool ShouldUseStderr); // 使用方法表示関数
	static std::string ErrorText(const std::error_code &Error); // 処理系の失敗理由の表示テキストの取得関数
	static void PrintPathError(const Filesystem::path &Path = Filesystem::path()); // パスの変換に失敗した入口の共通の通知関数

	#if defined(_WIN32)

	static std::vector<std::string> WindowsArgs(const int Argc, wchar_t *const Argv[]); // 起動引数の UTF-8 への変換関数

	#endif

	template<typename Char> static bool IsEqualIgnoringAsciiCase(
		const std::basic_string_view<Char> Name,
		const std::string_view Target
	); // ASCII の大小を無視した名前の一致の判定関数

	static bool IsLink(const Filesystem::path &Path, const Filesystem::file_status Status, std::error_code &Error); // 再解析点も含むリンクの判定関数
	static void PrintNamedLink(const std::string &Path, const Filesystem::path &Component); // 名指したリンクと実体の通知関数

	template<typename Char> static bool HasSuffixIgnoringAsciiCase(
		const std::basic_string_view<Char> Name,
		const std::string_view Suffix
	); // ASCII の大小を無視した末尾の一致の判定関数

	static bool IsWidthParsed(const std::string_view Value, size_t &Parsed, bool &IsOverflow); // 非負整数の行幅指定の読取関数
	static bool IsOptionName(const std::string_view Name); // 値の位置に来た既知の指定名の判定関数
	static void PrintValueError(const std::string_view Option, const std::string_view Value, const bool IsOverflow); // 値のエラーと対応する対処の共通の報告関数
	static void PrintReadError(const std::string_view Path, const std::error_code &Error, ScanExclusions *const Excluded = nullptr); // 読込失敗と対処の共通の報告関数
	static void PrintScanReadError(const Filesystem::path &Path, const std::error_code &Error, ScanExclusions &Excluded); // 変換出来ない名前も安全に表示する走査の読込失敗の報告関数
	static Options ParseArgs(const int Argc, char *const Argv[]); // コマンドライン引数解析関数
	static void PrintException(const std::exception &Error, const char *const Label = nullptr); // 確保せずに例外の失敗理由と対処を表示する関数
	static void PrintUnformatted(const FormatResult &Result, const std::string_view Label); // 見送・失敗した整形の通知関数（標準エラーへ）
	static void PrintEncodingNotices(const FormatResult &Result, const char *const PathLabel); // エンコーディング・改行様式の告知関数（規約が求める UTF-8（BOM 無）・LF との差を標準エラーへ）
	static void PrintDiagnostics(const FormatResult &Result, const std::string_view Label); // 付加診断の位置付表示関数
	static void PrintWarnings(const std::vector<LintWarning> &Warnings, const char *const FileLabel); // 規約違反警告出力関数（`[warn] file:line:col: <メッセージ>` 形式で標準エラーへ）

	static bool IsFileCollectionSuccessful(
		const std::string &Path,
		const ::Lang OverrideLang,
		std::vector<TargetFile> &Files,
		PathRejection &Rejection,
		ScanExclusions &Excluded
	); // 整形対象ファイル収集関数

	static int PrintSummary(const FormattingSummary &Summary); // 通常の出力を確定して共通の要約を書き出す関数
	static void RegisterInterruption(const int Signal); // POSIX の継承した無視を保持して中断を登録する関数

	static std::vector<FileJobResult> ProcessFiles(
		const Formatter &Shaver,
		const std::vector<TargetFile> &Files,
		const bool ShouldWrite,
		const bool IsKeepsOriginal,
		const bool IsSkipsLint,
		InspectionCache &Counted,
		ReplayStore *const Replays = nullptr
	); // ファイル一覧整形関数（結果を入力順に格納）

	static void CountEmergencyResult(const FileJobResult &Result); // 完了した結果を非常用の要約へ計上する関数
	static void PrintDiffLine(const std::string_view Text, const char Prefix); // 行末と表示用の置換を保った差分行の出力関数
	static void PrintDiff(const FileJobResult &Result, const char *const PathLabel); // 前後３行の文脈を含む統一差分の出力関数
	static int Execute(const int Argc, char *const Argv[]); // リソース不足を送出する実行の本体
	static int FinishStreams(const int Code); // ストリームの書出失敗を終了コードへ反映する関数

public:

	static int Run(const int Argc, ArgChar *const Argv[]); // 実行関数（引数の解釈から報告迄）

	Main() = delete; // コンストラクタ（禁止）
};

/**
 * 処理系の失敗理由を安全に表示するテキストの取得関数
 * 計算量：理由の長さ L に対し O(L)
 * @param Error 処理系の失敗理由
 * @return UTF-8 の安全な表示テキスト
 * @throws std::bad_alloc 理由の領域を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::string Main::ErrorText(const std::error_code &Error) {
	// 処理系の失敗理由を安全に表示するテキストの取得
	#if defined(_WIN32)
	// Windows の Unicode の理由の返戻
	return FileIO::GetNativeErrorText(Error);
	#else
	// UTF-8 の理由の安全な表示の返戻
	return FileIO::ForDisplay(Error.message());
	#endif
}

/**
 * パスの変換に失敗した起動引数と走査の共通の通知関数
 * 計算量：パス長 P に対し O(P)
 * @param Path 処理系のパス（空なら対象名を持たない入口の失敗）
 */
void Main::PrintPathError(const Filesystem::path &Path) {
	static constexpr char Failure[] =
	"Error: cannot read filesystem path\n[notice] verify the path encoding and access permissions\n";
	try {
		#if defined(_WIN32)
		const std::string Label = FileIO::GetPathDisplay(Path);
		#else
		const std::string Label = FileIO::ForDisplay(FileIO::GetPathText(Path));
		#endif
		if(Label.empty()) std::fputs(Failure, stderr);
		else {
			std::fprintf(stderr, "Error: cannot read filesystem path: %s\n", Label.c_str());
			std::fprintf(stderr, "[notice] %s: verify the path encoding and access permissions\n", Label.c_str());
		}
	} catch(const std::exception &) {
		// 例外の捕捉中に表示名を作れない場合も，新しい例外を送出せず既存の通知を残す
		std::fputs(Failure, stderr);
	}
	// 終了
	return;
}

#if defined(_WIN32)

/**
 * Windows の Unicode の起動引数を UTF-8 へ移す関数
 * 計算量：引数の総長 A に対し O(A)，追加領域 O(A)
 * @param Argc 起動引数の数
 * @param Argv Unicode の起動引数
 * @return ANSI 符号ページを経由しない起動引数
 * @throws std::bad_alloc 引数の領域を確保出来ない場合
 * @throws PathEncodingFailure 起動引数を UTF-8 へ変換出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::vector<std::string> Main::WindowsArgs(const int Argc, wchar_t *const Argv[]) {
	// エンコードした起動引数の収集準備
	std::vector<std::string> Args;
	Args.reserve(static_cast<size_t>(Argc));
	for(int Idx = 0; Idx < Argc; ++Idx) {
		const int Size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Argv[Idx], -1, nullptr, 0, nullptr, nullptr);
		if(!Size) throw PathEncodingFailure{ Filesystem::path(Argv[Idx]) };
		std::string Text(static_cast<size_t>(Size), '\0');
		if(!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Argv[Idx], -1, Text.data(), Size, nullptr, nullptr)) {
			throw PathEncodingFailure{ Filesystem::path(Argv[Idx]) };
		}
		Text.pop_back();
		Args.push_back(std::move(Text));
	}
	// UTF-8 の起動引数の返戻
	return Args;
}

#endif

/**
 * ASCII の大小を無視した名前の一致の判定関数
 * 計算量：名前の長さ N に対し O(N)
 * @tparam Char 名前に使うコードユニットの型
 * @param Name 検査する名前
 * @param Target 比べる名前
 * @return 大小を無視して一致すれば true
 */
template<typename Char> bool Main::IsEqualIgnoringAsciiCase(
	const std::basic_string_view<Char> Name,
	const std::string_view Target
) {
	// 長さの違う名前の返戻
	if(Name.size() != Target.size()) return false;
	for(size_t Idx = 0; Idx < Name.size(); ++Idx) {
		// 左側の小文字形
		// 右側の小文字形との照合
		// 一致しない字が有る事の返戻
		if(
			const Char Left = static_cast<Char>(Name[Idx] >= 'A' && Name[Idx] <= 'Z' ? Name[Idx] + 'a' - 'A' : Name[Idx]),
			Right = static_cast<Char>(Target[Idx] >= 'A' && Target[Idx] <= 'Z' ? Target[Idx] + 'a' - 'A' : Target[Idx]);
			Left != Right
		) return false;
	}
	// 全ての字が一致した事の返戻
	return true;
}

/**
 * 使用方法表示関数
 * 計算量：O(1)
 * @param ShouldUseStderr エラーに伴う表示なら true（エラーの手掛りが `> /dev/null` で消えない様に標準エラーへ出す）
 */
void Main::PrintUsage(const bool ShouldUseStderr) {
	// 使用法を出す流れ
	std::FILE *const Output = ShouldUseStderr ? stderr : stdout;
	std::fprintf(Output, "shavefmt %.*s\n", static_cast<int>(BuildInfo::Version.size()), BuildInfo::Version.data());
	std::fputs(
		"\n"
		"Usage: shavefmt [OPTIONS] <FILE|DIR>...\n"
		"       shavefmt --stdin --lang ID [OPTIONS]   (read stdin, write stdout)\n"
		"\n"
		"Options:\n"
		"  -w, --write      Write changes to files (default: dry-run; not with -c)\n"
		"  -c, --check      Check only (exit code 1 if changes needed)\n"
		"  --diff           Show unified diff\n"
		"  -l, --lang ID    Language of stdin or a named file (see \"Language IDs\")\n"
		"                   (a known file extension must agree)\n"
		"  -m, --chars N    Maximum characters per line (default: 128; 0 = unlimited)\n"
		"  --stdin          Read stdin and write the result to stdout (requires --lang)\n"
		"  --no-lint        Skip lint warnings (faster format-only mode)\n"
		"  --fail-on-skip   Treat skipped files and named paths through symbolic links\n"
		"                   as failures (exit code 1)\n"
		"  -h, --help       Show this help and exit (other arguments are ignored)\n"
		"  -v, --version    Show version and exit (other arguments are ignored)\n"
		"  --               Treat every later argument as a path\n"
		"                   (for names starting with -)\n"
		"\n"
		"Language IDs (case-insensitive; extensions such as ts, py, rb, hh also work):\n"
		"  c, cpp, csharp, java, go, rust, kotlin, swift, php, javascript,\n"
		"  javascriptreact, typescript, typescriptreact, ruby, python, json, jsonc,\n"
		"  html, css, scss\n"
		"  (h: C or C++ by content, as for .h files)\n"
		"\n"
		"Exit codes:\n"
		"  0  success (with -c: no file needs formatting)\n"
		"  1  formatting needed or no supported file found (-c),\n"
		"     a named path was missing or had an unsupported file type,\n"
		"     a file or directory could not be read or processed,\n"
		"     formatting was reverted by output validation or by a formatter defect,\n"
		"     or a file or named symbolic link was skipped with --fail-on-skip\n"
		"  2  usage error (unknown option, missing or invalid value,\n"
		"     --lang conflicting with a known extension, conflicting flags,\n"
		"     or no path)\n",
		Output
	);
	// 終了
	return;
}

/**
 * 標準出力の確定関数
 * 計算量：O(1)
 * @return 出力成功時０，失敗時１
 */
int Main::FlushStdout() {
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
 * 名前を転送する再解析点も含むリンクの判定関数
 * 計算量：パス長 P に対し O(P)
 * @param Path 状態を取得済のパス
 * @param Status リンクを辿らない状態
 * @param Error Windows の属性取得失敗の格納先
 * @return シンボリックリンク又は Windows の名前を転送する再解析点なら true
 */
bool Main::IsLink(
	[[maybe_unused]] const Filesystem::path &Path,
	[[maybe_unused]] const Filesystem::file_status Status,
	[[maybe_unused]] std::error_code &Error
) {
	// 名前を転送する再解析点も含むリンクの判定
	#if defined(_WIN32)
	const HANDLE Handle = CreateFileW(
		FileIO::GetNativePath(Path).c_str(),
		FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr
	);
	if(Handle == INVALID_HANDLE_VALUE) {
		Error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
		// 実体を取得出来ない事の返戻
		return false;
	}
	const bool IsLinked = FileIO::IsNameSurrogate(Handle, &Error);
	if(!CloseHandle(Handle)) std::fprintf(stderr, "[notice] failed to close file handle (system error %lu)\n", GetLastError());
	// 名前を転送する再解析点かの返戻
	return IsLinked;
	#else
	// POSIX のシンボリックリンクの判定の返戻
	return Filesystem::is_symlink(Status);
	#endif
}

/**
 * 名指したリンクと実体の通知関数
 * 計算量：パスの総長 P に対し O(P)
 * @param Path 指定パス
 * @param Component リンクだった成分
 * @throws std::bad_alloc パス又は表示名の領域を確保出来ない場合
 * @throws std::filesystem::filesystem_error パスのエンコーディングを変換出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::system_error パスのエンコーディングの変換に失敗した場合
 */
void Main::PrintNamedLink(const std::string &Path, const Filesystem::path &Component) {
	// パスの解決の失敗理由の格納準備
	std::error_code ResolveError;
	const Filesystem::path Resolved = Filesystem::canonical(FileIO::PathOf(Path), ResolveError);
	const std::string SafePath = FileIO::ForDisplay(Path), SafeComponent = FileIO::ForDisplay(FileIO::GetPathText(Component));
	if(ResolveError) {
		std::fprintf(
			stderr,
			"[skip] %s: link component %s is not followed\n[notice] %s: specify an existing path without link components\n",
			SafePath.c_str(),
			SafeComponent.c_str(),
			SafePath.c_str()
		);
	} else {
		std::fprintf(
			stderr,
			"[skip] %s: link component %s is not followed\n[notice] %s: use resolved path %s\n",
			SafePath.c_str(),
			SafeComponent.c_str(),
			SafePath.c_str(),
			FileIO::ForDisplay(FileIO::GetPathText(Resolved)).c_str()
		);
	}
	// 終了
	return;
}

/**
 * ASCII の大小を無視した末尾の一致の判定関数
 * 計算量：接尾辞長 L に対し O(L)
 * @tparam Char 名前に使うコードユニットの型
 * @param Name 検査する名前
 * @param Suffix 比べる末尾
 * @return 大小を無視して末尾が一致すれば true
 */
template<typename Char> bool Main::HasSuffixIgnoringAsciiCase(
	const std::basic_string_view<Char> Name,
	const std::string_view Suffix
) {
	// 末尾の一致の返戻
	return Name.size() >= Suffix.size() && IsEqualIgnoringAsciiCase(Name.substr(Name.size() - Suffix.size()), Suffix);
}

/**
 * 非負整数の行幅指定の読取関数
 * 計算量：指定値の長さ L に対し O(L)
 * @param Value １０進の指定値
 * @param Parsed 読み取った幅の格納先（成功時だけ使用する）
 * @param IsOverflow オーバーフローを検出したかの格納先
 * @return 空でない全桁の整数を上限以内で読めれば true
 */
bool Main::IsWidthParsed(const std::string_view Value, size_t &Parsed, bool &IsOverflow) {
	// 幅の読取の初期化
	Parsed = 0;
	IsOverflow = false;
	size_t Idx = 0;
	for(; Idx < Value.size() && Value[Idx] >= '0' && Value[Idx] <= '9'; ++Idx) {
		const size_t Digit = static_cast<size_t>(Value[Idx] - '0');
		// 小さな幅へのオーバーフロー防止
		if(Parsed > (std::numeric_limits<size_t>::max() - Digit) / 10) IsOverflow = true;
		Parsed = 10 * Parsed + Digit;
	}
	// 空入力・非数字・オーバーフローが無い事の返戻
	return Idx && Idx == Value.size() && !IsOverflow;
}

/**
 * 値の位置に来た既知の指定名の判定関数
 * 計算量：指定名長 L と既知の指定数 K に対し O(K L)
 * @param Name 値として渡された綴り
 * @return 既知の指定名其の物なら true
 */
bool Main::IsOptionName(const std::string_view Name) {
	// 値を取る指定も含む，完全一致だけの照合
	static constexpr std::string_view Names[] = {
		"--",
		"--chars",
		"--check",
		"--diff",
		"--fail-on-skip",
		"--help",
		"--lang",
		"--no-lint",
		"--stdin",
		"--version",
		"--write",
		"-c",
		"-h",
		"-l",
		"-m",
		"-v",
		"-w"
	};
	// 指定名の一致の返戻
	return std::ranges::find(Names, Name) != std::end(Names);
}

/**
 * 分けた値と連結した値のエラーを，同じ原因と対処で報告する関数
 * 計算量：指定名と値の総長 L に対し O(L)
 * @param Option 値を取る指定名
 * @param Value 拒否した値
 * @param IsOverflow 行幅の上限を超えたか
 * @throws std::bad_alloc 表示用のテキストを確保出来ない場合
 * @throws std::length_error 表示用のテキストの上限を超える場合
 */
void Main::PrintValueError(const std::string_view Option, const std::string_view Value, const bool IsOverflow) {
	const std::string SafeValue = FileIO::ForDisplay(Value);
	// 言語の既存の理由と対応する対処
	if(Option == "--lang" || Option == "-l") {
		std::fprintf(stderr, "Error: --lang unsupported language ID '%s'\n[notice] run --help for the list\n", SafeValue.c_str());
	} else {
		const std::string SafeOption = FileIO::ForDisplay(Option);
		const char *const Reason = IsOverflow ? "is out of range" : "requires a non-negative integer";
		std::fprintf(stderr, "Error: %s %s, got '%s'\n", SafeOption.c_str(), Reason, SafeValue.c_str());
	}
	// 終了
	return;
}

/**
 * 読込失敗と対応する対処の共通の報告関数
 * 計算量：表示名と理由の総長 L に対し O(L)
 * @param Path 表示前の UTF-8 の対象パス
 * @param Error 処理系の失敗理由
 * @param Excluded 収集時の読込失敗の格納先（処理結果の失敗は nullptr）
 * @throws std::bad_alloc 表示用のテキストを確保出来ない場合
 * @throws std::length_error 表示用のテキストの上限を超える場合
 */
void Main::PrintReadError(const std::string_view Path, const std::error_code &Error, ScanExclusions *const Excluded) {
	const std::string Label = FileIO::ForDisplay(Path);
	// 原因と対応する対処は別行にし，同じ表示名を使う
	std::fprintf(
		stderr,
		"Error reading: %s: %s\n",
		Label.c_str(),
		(Error ? ErrorText(Error) : "memory allocation failed while reading").c_str()
	);
	std::fprintf(stderr, "[notice] %s: verify read access and retry after concurrent changes stop\n", Label.c_str());
	// 収集時の失敗は名指しと走査で同じ報告の単位に揃える
	if(Excluded) ++Excluded->ReadFailures;
	// 終了
	return;
}

/**
 * 走査中の読込失敗を，処理系の名前から安全な表示へ移して報告する関数
 * 計算量：表示名と理由の総長 L に対し O(L)
 * @param Path 利用者の綴りを起点とした表示用のネイティブなパス
 * @param Error 読込又は名前の変換に失敗した理由
 * @param Excluded 収集時の読込失敗の格納先
 * @throws std::bad_alloc 表示用のテキストを確保出来ない場合
 * @throws std::length_error 表示用のテキストの上限を超える場合
 */
void Main::PrintScanReadError(const Filesystem::path &Path, const std::error_code &Error, ScanExclusions &Excluded) {
	#if defined(_WIN32)
	const std::string Label = FileIO::GetPathDisplay(Path);
	#else
	const std::string Label = FileIO::GetPathText(Path);
	#endif
	// 変換不能なコードユニットも表示用だけにエスケープし，同じ読込の理由と対処へ合流
	PrintReadError(Label, Error, &Excluded);
	// 終了
	return;
}

/**
 * コマンドライン引数解析関数
 * 計算量：引数数 A と引数の総長 L に対し O(A + L)
 * @param Argc 引数の数
 * @param Argv 引数の配列
 * @return 解析結果の `Options` 構造体
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
Main::Options Main::ParseArgs(const int Argc, char *const Argv[]) {
	// 解析中の設定
	Options Settings;
	const char *CheckArg = nullptr, *WriteArg = nullptr, *DiffArg = nullptr; // 実際に指定したオプション名
	// 値を要する引数の検査関数
	const auto HasRequiredVal = [Argc, &Argv, &Settings](const int Idx) -> bool {
		// 必須値の有無の検査
		if(Idx + 1 == Argc || IsOptionName(Argv[Idx + 1])) {
			std::fprintf(stderr, "Error: %s requires a value\n", FileIO::ForDisplay(Argv[Idx]).c_str());
			Settings.ShouldExit = true;
			Settings.ExitCode = UsageErrorCode;
			// 引数エラーを呼出元へ返す為の失敗の返戻
			return false;
		}
		// 必須値が在る事の返戻
		return true;
	};
	// 誤った引数の報告より先に行う早期終了オプションの走査
	for(int Idx = 1; Idx < Argc && std::strcmp(Argv[Idx], "--"); ++Idx) {
		// ヘルプ指定の場合
		if(!std::strcmp(Argv[Idx], "-h") || !std::strcmp(Argv[Idx], "--help")) {
			PrintUsage(false);
			Settings.ShouldExit = true;
			Settings.ExitCode = FlushStdout();
			// 表示だけを行った解析結果の返戻
			return Settings;
		}
		// バージョン表示指定の場合
		if(!std::strcmp(Argv[Idx], "-v") || !std::strcmp(Argv[Idx], "--version")) {
			// 不具合報告と検査工程向の機械可読なバージョンの単独行出力
			std::printf("shavefmt %.*s\n", static_cast<int>(BuildInfo::Version.size()), BuildInfo::Version.data());
			Settings.ShouldExit = true;
			Settings.ExitCode = FlushStdout();
			// 表示だけを行った解析結果の返戻
			return Settings;
		}
	}
	// 通常指定の左からの反映と値を取る指定での走査位置更新
	for(int Idx = 1; Idx < Argc; ++Idx) {
		// 値を取る指定の共通照合（等号付の誤指定の案内にも使う）
		const std::string_view Arg(Argv[Idx]), Name = Arg.substr(0, Arg.find('='));
		const bool TakesLang = Name == "--lang" || Name == "-l", TakesChars = Name == "--chars" || Name == "-m";
		if(!std::strcmp(Argv[Idx], "-w") || !std::strcmp(Argv[Idx], "--write")) {
			Settings.ShouldWrite = true;
			WriteArg = Argv[Idx];
		} else if(!std::strcmp(Argv[Idx], "-c") || !std::strcmp(Argv[Idx], "--check")) {
			Settings.ShouldCheck = true;
			CheckArg = Argv[Idx];
		} else if(!std::strcmp(Argv[Idx], "--diff")) {
			Settings.ShouldShowDiff = true;
			DiffArg = Argv[Idx];
		// 言語指定の読取
		} else if(Name.size() == Arg.size() && TakesLang) {
			// 必須値の欠落時は入力へ進まない事の返戻
			if(!HasRequiredVal(Idx)) return Settings;
			const char *const Val = Argv[++Idx];
			const ::Lang Parsed = Lang::FromName(Val);
			// 未対応の言語識別子
			if(Parsed == Lang::Unknown) {
				PrintValueError(Argv[Idx - 1], Val, false);
				Settings.ShouldExit = true;
				Settings.ExitCode = UsageErrorCode;
				// 使用法のエラーの解析結果の返戻
				return Settings;
			}
			// 指定言語を保存
			Settings.OverrideLang = Parsed;
		// 行幅指定の読取
		} else if(Name.size() == Arg.size() && TakesChars) {
			// １０進指定値の読取
			// 必須値の欠落時は入力へ進まない事の返戻
			if(!HasRequiredVal(Idx)) return Settings;
			const char *const Val = Argv[++Idx];
			size_t Parsed;
			bool IsOverflow;
			// 空入力・非数字・オーバーフローの拒否
			if(!IsWidthParsed(Val, Parsed, IsOverflow)) {
				PrintValueError(Argv[Idx - 1], Val, IsOverflow);
				Settings.ShouldExit = true;
				Settings.ExitCode = UsageErrorCode;
				// 使用法のエラーの解析結果の返戻
				return Settings;
			}
			Settings.MaxChars = Parsed;
		} else if(!std::strcmp(Argv[Idx], "--stdin")) Settings.ShouldReadStdin = true;
		else if(!std::strcmp(Argv[Idx], "--no-lint")) Settings.ShouldSkipLint = true;
		else if(!std::strcmp(Argv[Idx], "--fail-on-skip")) Settings.ShouldFailOnSkip = true;
		// オプション終端後をパスとして収集
		else if(!std::strcmp(Argv[Idx], "--")) for(++Idx; Idx < Argc; ++Idx) Settings.Paths.emplace_back(Argv[Idx]);
		// 未知のオプションの場合
		else if(Argv[Idx][0] == '-' && Argv[Idx][1]) {
			// 値を取る既知の指定だけに，引数の区切方を案内する
			size_t Parsed;
			bool IsOverflow = false;
			const bool IsShortLang = Arg.starts_with("-l") && Arg.size() > 2 && Lang::FromName(Arg.substr(2)) != Lang::Unknown;
			const bool IsShortChars = Arg.starts_with("-m") && Arg.size() > 2 && IsWidthParsed(Arg.substr(2), Parsed, IsOverflow);
			const bool HasEqualsValue = Name.size() < Arg.size() && (TakesLang || TakesChars);
			const bool HasJoinedValue = HasEqualsValue || IsShortLang || IsShortChars;
			// 既知の指定は利用者が書いた名前と値を分けた形で案内する
			if(HasJoinedValue) {
				const std::string_view OptionName = HasEqualsValue ? Name : Arg.substr(0, 2);
				const std::string_view RawValue = Arg.substr(HasEqualsValue ? Name.size() + 1 : 2);
				const std::string Option = FileIO::ForDisplay(OptionName), Value = FileIO::ForDisplay(RawValue);
				if(RawValue.empty()) std::fprintf(stderr, "Error: %s requires a value\n", Option.c_str());
				else {
					const bool IsValid =
					TakesLang || IsShortLang ? Lang::FromName(RawValue) != Lang::Unknown : IsWidthParsed(RawValue, Parsed, IsOverflow);
					std::fprintf(stderr, "Error: %s requires a separate value, got '%s'\n", Option.c_str(), Value.c_str());
					if(IsValid) std::fprintf(stderr, "[notice] use separate arguments: %s %s; see --help\n", Option.c_str(), Value.c_str());
					else PrintValueError(OptionName, RawValue, IsOverflow);
				}
			// 綴エラーの指定をパス扱にしない報告
			} else std::fprintf(stderr, "Error: unknown option '%s'\n[notice] see --help\n", FileIO::ForDisplay(Argv[Idx]).c_str());
			Settings.ShouldExit = true;
			Settings.ExitCode = UsageErrorCode;
			// 使用法のエラーの解析結果の返戻
			return Settings;
		// 通常の入力パスを追加
		} else Settings.Paths.emplace_back(Argv[Idx]);
	}
	// 検査時の誤書込を防ぐ `--check` と `--write` の同時指定拒否
	if(Settings.ShouldCheck && Settings.ShouldWrite) {
		std::fprintf(stderr, "Error: %s and %s are mutually exclusive\n", CheckArg, WriteArg);
		Settings.ShouldExit = true;
		Settings.ExitCode = UsageErrorCode;
		// 使用法のエラーの解析結果の返戻
		return Settings;
	}
	// `--stdin` で作用しない書戻・検査・差分表示指定の拒否
	if(Settings.ShouldReadStdin && (Settings.ShouldCheck || Settings.ShouldWrite || Settings.ShouldShowDiff)) {
		std::fprintf(stderr, "Error: --stdin cannot be combined with %s\n", CheckArg ? CheckArg : WriteArg ? WriteArg : DiffArg);
		Settings.ShouldExit = true;
		Settings.ExitCode = UsageErrorCode;
		// 使用法のエラーの解析結果の返戻
		return Settings;
	}
	// `--stdin` と併記したパスの拒否
	if(Settings.ShouldReadStdin && !Settings.Paths.empty()) {
		std::fprintf(
			stderr,
			"Error: --stdin does not take file arguments (got '%s')\n",
			FileIO::ForDisplay(Settings.Paths.front()).c_str()
		);
		Settings.ShouldExit = true;
		Settings.ExitCode = UsageErrorCode;
		// 使用法のエラーの解析結果の返戻
		return Settings;
	}
	// 確定したオプションの返戻
	return Settings;
}

/**
 * 確保せずに例外の失敗理由と対処を表示する関数
 * 計算量：表示する固定本文とラベルの長さ L に対し O(L)
 * @param Error 捕捉した例外
 * @param Label 原文を保持した対象の表示名（対象が無ければ nullptr）
 */
void Main::PrintException(const std::exception &Error, const char *const Label) {
	const FailureReason Reason = FailureReason::FromException(Error);
	const std::string_view Text = Reason.GetText();
	// 失敗の原因に対応する診断
	if(Reason == FailureReason::FormatterDefect) std::fputs("Error: formatter defect", stderr);
	else std::fprintf(stderr, "Error: formatting could not be completed (%.*s)", static_cast<int>(Text.size()), Text.data());
	if(Label) std::fprintf(stderr, ", left unchanged: %s", Label);
	if(Reason == FailureReason::FormatterDefect) std::fprintf(stderr, ": %.*s", static_cast<int>(Text.size()), Text.data());
	std::fputc('\n', stderr);
	const std::string_view Remedy = Reason.GetRemedy();
	std::fputs("[notice] ", stderr);
	if(Label) std::fprintf(stderr, "%s: ", Label);
	std::fprintf(stderr, "%.*s\n", static_cast<int>(Remedy.size()), Remedy.data());
	// 終了
	return;
}

/**
 * 見送・失敗した整形の通知関数（標準エラーへ，整形仕様 8.3，9.1）
 * 見送とフォーマッターの不具合は其の理由を示す
 * 検証の差戻は `Error: formatting would` に続けて，構文・意味・構造・空行の破壊とディレクティブコメント・ドキュメントの説明対象の保持違反を区別する
 * 其の他の完了出来なかった整形は判る理由（理由を持たない物はリソース枯渇又は内部のエラー）を示す
 * 計算量：パスと診断の総長 L に対し O(L)
 * @param Result 見送又は失敗した整形の結果
 * @param Label 通知に示す入力の名前
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void Main::PrintUnformatted(const FormatResult &Result, const std::string_view Label) {
	// 整形結果の分類別通知
	const std::string_view Reason = Result.Reason.GetText();
	const std::string LocatedLabel = std::string(Label) + (
		Result.ReasonPoint ?
		":" + std::to_string(Result.ReasonPoint->Row + 1) + ":" + std::to_string(Result.ReasonPoint->Column + 1) :
		""
	);
	const int LabelSize = static_cast<int>(LocatedLabel.size()), ReasonSize = static_cast<int>(Reason.size());
	if(Result.Classification == FormatClassification::Skipped) {
		std::fprintf(stderr, "[skip] %.*s: %.*s\n", LabelSize, LocatedLabel.data(), ReasonSize, Reason.data());
	} else if(Result.Reason == FailureReason::FormatterDefect) {
		std::fprintf(
			stderr,
			"Error: formatter defect, left unchanged: %.*s: %.*s\n",
			LabelSize,
			LocatedLabel.data(),
			ReasonSize,
			Reason.data()
		);
	} else if(Result.Reason.IsValidation()) {
		std::fprintf(
			stderr,
			"Error: formatting would %.*s, left unchanged: %.*s\n",
			ReasonSize,
			Reason.data(),
			LabelSize,
			LocatedLabel.data()
		);
	} else {
		std::fprintf(
			stderr,
			"Error: formatting could not be completed (%.*s), left unchanged: %.*s\n",
			ReasonSize,
			Reason.data(),
			LabelSize,
			LocatedLabel.data()
		);
	}
	const std::string_view Remedy = Result.Reason.GetRemedy();
	std::fprintf(stderr, "[notice] %.*s: %.*s\n", LabelSize, LocatedLabel.data(), static_cast<int>(Remedy.size()), Remedy.data());
	// 終了
	return;
}

/**
 * エンコーディング・改行様式の告知関数（規約が求める UTF-8（BOM 無）・LF との差を標準エラーへ）
 * 計算量：パスの表示長 P に対し O(P)
 * @param Result 告知の材料を持つ整形結果
 * @param PathLabel ファイル識別ラベル（行頭表示用）
 */
void Main::PrintEncodingNotices(const FormatResult &Result, const char *const PathLabel) {
	// UTF-8 BOM が有る場合
	if(Result.Normalization.HasRemovedBom) {
		std::fprintf(stderr, "[notice] %s: Input has a UTF-8 BOM; successful output omits it\n", PathLabel);
	}
	// CRLF 原本の場合
	if(Result.Normalization.HasFoldedCrLf && !Result.Normalization.HasMixedNewlines) {
		std::fprintf(stderr, "[notice] %s: Input uses CRLF line endings; generated output uses LF\n", PathLabel);
	}
	// CR のみの改行の原本の場合
	if(Result.Normalization.HasNormalizedCrOnly) {
		std::fprintf(stderr, "[notice] %s: File uses CR-only line endings; they become LF\n", PathLabel);
	}
	// 混在改行を LF へ畳んだ場合
	if(Result.Normalization.HasFoldedCrLf && Result.Normalization.HasMixedNewlines) {
		std::fprintf(stderr, "[notice] %s: File mixes CRLF and LF line endings; they become LF outside literals\n", PathLabel);
	}
	if(Result.Normalization.HasNormalizedUnicodeSeparator) {
		std::fprintf(stderr, "[notice] %s: Unicode line separators outside literals become LF\n", PathLabel);
	}
	// 終了
	return;
}

/**
 * 付加診断の位置付表示関数
 * 計算量：表示の総長 B に対し O(B)
 * @param Result 中核と検証が返した付加診断
 * @param Label 安全な表示へ変換済の入力名
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void Main::PrintDiagnostics(const FormatResult &Result, const std::string_view Label) {
	// 対象範囲の走査
	for(const FormatDiagnostic &Diagnostic : Result.Diagnostics) {
		const std::string Located = std::string(Label) +
		(Diagnostic.Point ? ":" + std::to_string(Diagnostic.Point->Row + 1) + ":" + std::to_string(Diagnostic.Point->Column + 1) : "");
		std::fprintf(stderr, "[notice] %s: %s\n", Located.c_str(), Diagnostic.Text.c_str());
	}
	// 終了
	return;
}

/**
 * 規約違反警告出力関数（`[warn] file:line:col: <メッセージ>` 形式で標準エラーへ）
 * 計算量：警告数 W とパス・警告の総長 L に対し O(W + L)
 * @param Warnings 警告メッセージ列
 * @param FileLabel ファイル識別ラベル（行頭表示用）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void Main::PrintWarnings(const std::vector<LintWarning> &Warnings, const char *const FileLabel) {
	// 安全な表示名に依る警告列の出力
	for(const LintWarning &Warning : Warnings) {
		std::fprintf(
			stderr,
			"[warn] %s:%u:%u: %s\n",
			FileLabel,
			Warning.Row + 1,
			Warning.Column + 1,
			FileIO::ForDisplay(Warning.Msg).c_str()
		);
	}
	// 終了
	return;
}

/**
 * 整形対象ファイル収集関数
 * 計算量：項目数 E に対し O(E log E)
 * @param Path ファイル又はディレクトリのパス
 * @param OverrideLang `--lang` の指定値（`Lang::Unknown` は未指定）
 * @param Files 収集先のファイル一覧
 * @param Rejection 明示指定したパスを対象に出来なかった理由の設定先
 * @param Excluded 走査で読まなかったパスの数の加算先
 * @return 指定パスが存在し走査を全う出来れば true（存在しないパス・走査の打切は呼出側がエラーとして扱う）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::filesystem::filesystem_error パスの変換又は対象の列挙に失敗した場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::system_error パスのエンコーディングの変換に失敗した場合
 */
bool Main::IsFileCollectionSuccessful(
	const std::string &Path,
	const ::Lang OverrideLang,
	std::vector<TargetFile> &Files,
	PathRejection &Rejection,
	ScanExclusions &Excluded
) {
	// 現在地の実体を基準に指定の全成分を検査し，リンク経由の書込を拒否して未読件数へ加算（行先の無いリンクも辿らないリンクとして数える為，存在の確認より先に行う）
	std::error_code AbsoluteError;
	const Filesystem::path Absolute = FileIO::GetAbsolutePath(Path, AbsoluteError);
	// 絶対パスの取得失敗
	if(AbsoluteError) {
		PrintReadError(Path, AbsoluteError, &Excluded);
		// 絶対パス取得失敗時の返戻
		return false;
	}
	// パス各階層のリンク検査
	Filesystem::path Prefix = Absolute.root_path();
	for(const Filesystem::path &Part : Absolute.relative_path()) {
		ParserAllocator::CheckInterruption();
		Prefix /= Part;
		std::error_code LinkError;
		const Filesystem::file_status LinkStatus = Filesystem::symlink_status(FileIO::GetNativePath(Prefix), LinkError);
		// 其の成分が無いパスの後の存在の確認への委任
		if(LinkStatus.type() == Filesystem::file_type::not_found) break;
		const bool IsLinked = IsLink(Prefix, LinkStatus, LinkError);
		// 階層情報の取得失敗
		if(LinkError) {
			PrintReadError(Path, LinkError, &Excluded);
			// 階層情報取得失敗時の返戻
			return false;
		}
		// 名前付パスがシンボリックリンクの場合
		if(IsLinked) {
			PrintNamedLink(Path, Prefix);
			++Excluded.Links;
			++Excluded.NamedLinks;
			// 名前付リンク除外時の返戻
			return true;
		}
	}
	// リンクを含まないパスの状態の１回取得とエラーコードに依る処理
	std::error_code StatusError;
	const Filesystem::file_status Status = Filesystem::status(FileIO::GetNativePath(Absolute), StatusError);
	if(StatusError && Status.type() != Filesystem::file_type::not_found) {
		PrintReadError(Path, StatusError, &Excluded);
		// 状態を読めない名指しの対象の返戻
		return false;
	}
	// 不在パスの対応外言語と区別した失敗の返戻（失敗の内容は原因を知る此処で報告する）
	if(StatusError || !Filesystem::exists(Status)) {
		std::fprintf(stderr, "Error: path not found: %s\n", FileIO::ForDisplay(Path).c_str());
		// 不在パスの返戻
		return false;
	}
	// 通常ファイルの場合
	if(Filesystem::is_regular_file(Status)) {
		// 単一の明示ファイルの言語判定
		const ::Lang ByPath = Lang::Detect(Path);
		// 確定した拡張子への別言語指定の拒否
		if(
			OverrideLang != Lang::Unknown && ByPath != Lang::Unknown && !Lang::IsAmbiguousCandidate(Path, OverrideLang) && (
				ByPath != OverrideLang.Id || ByPath.IsJsonc && !OverrideLang.IsJsonc || ByPath.IsJsx != OverrideLang.IsJsx ||
				ByPath.IsScss != OverrideLang.IsScss ||
				ByPath.Goal != Lang::SrcGoal::Undetermined && OverrideLang.Goal != Lang::SrcGoal::Undetermined &&
				ByPath.Goal != OverrideLang.Goal
			)
		) {
			Rejection = PathRejection::LangConflict;
			// 言語指定競合時の返戻
			return true;
		}
		// 明示指定を反映した適用言語の確定
		if(::Lang Detected = OverrideLang != Lang::Unknown ? OverrideLang : ByPath; Detected != Lang::Unknown) {
			// 共有する拡張子は明示指定が曖昧でない時だけ確定し，其れ以外は指定側の曖昧さを継ぐ
			Detected.IsAmbiguous = ByPath.IsAmbiguous ?
			OverrideLang == Lang::Unknown || OverrideLang.IsAmbiguous :
			OverrideLang.IsAmbiguous && ByPath == Lang::Unknown;
			if(Detected.Goal == Lang::SrcGoal::Undetermined) Detected.Goal = ByPath.Goal;
			const FileIO Input(Path);
			std::vector<FileIdentity> Identity = Input.GetPathIdentity();
			const bool HasUnknownIdentity = Input.GetReadError() == std::errc::operation_not_supported;
			Files.push_back({ Path, Detected, {}, std::move(Identity), HasUnknownIdentity });
		// 未対応の通常ファイルを記録
		} else Rejection = PathRejection::UnknownLang;
	// ディレクトリの場合
	} else if(Filesystem::is_directory(Status)) {
		if(OverrideLang != Lang::Unknown) {
			std::fprintf(
				stderr,
				"[notice] %s: --lang is ignored for directories; each file uses its extension\n",
				FileIO::ForDisplay(Path).c_str()
			);
		}
		std::error_code ScanError;
		// 走査はリンクを解決しない指定名を使い，根の実体の収集した子の親成分と照合
		const Filesystem::path NormalizedAbsolute = Absolute.lexically_normal();
		const Filesystem::path NamedRoot = NormalizedAbsolute.has_filename() ? NormalizedAbsolute : NormalizedAbsolute.parent_path();
		const FileIO RootInput(FileIO::GetPathText(NamedRoot));
		std::vector<FileIdentity> RootIdentity = RootInput.GetPathIdentity();
		if(RootIdentity.empty()) {
			PrintReadError(
				Path,
				RootInput.GetReadError() ? RootInput.GetReadError() : std::make_error_code(std::errc::state_not_recoverable),
				&Excluded
			);
			// 実体根を確定出来ない事の返戻
			return false;
		}
		#if defined(_WIN32)
		// 拡張パスへ移す前に，短い通常パスで解決した実際の綴りを使う
		const std::optional<std::string> RootName = RootInput.GetNormalizedPath();
		if(!RootName) {
			const std::error_code Error = RootInput.GetReadError();
			PrintReadError(Path, Error, &Excluded);
			// 走査根の実際の名前を取得出来ない失敗の返戻
			return false;
		}
		const Filesystem::path ScanRoot = FileIO::PathOf(*RootName);
		const bool HasSharedRoot = ScanRoot.root_name().native().starts_with(L"\\\\") && ScanRoot.parent_path().relative_path().empty();
		#else
		const Filesystem::path &ScanRoot = NamedRoot;
		const bool HasSharedRoot = false;
		#endif
		// 実体根と UNC の共有根は，既に親成分の最後として記録済
		if(!ScanRoot.has_filename() || HasSharedRoot) RootIdentity.pop_back();
		// 表示名はネイティブな相対部分から作り，変換出来ない名前を判定の前に変換しない
		const Filesystem::path NativeScanRoot = FileIO::GetNativePath(ScanRoot, true), DisplayRoot = FileIO::PathOf(Path);
		// 走査した対象を，利用者が書いた起点からの表示パスへ移す関数
		// 計算量：パス長 P に対し O(P)
		// @param Entry 表示するネイティブなパス
		// @return 起点の綴りを保ったネイティブな表示パス
		// @throws std::bad_alloc 表示パスの領域を確保出来ない場合
		// @throws std::length_error 表示パスの上限を超える場合
		const auto GetDisplayPath = [&NativeScanRoot, &DisplayRoot](const Filesystem::path &Entry) -> Filesystem::path {
			// 起点其の物には相対成分の点を付けず，枝とファイルには同じ相対部分を使う返戻
			return Entry == NativeScanRoot ? DisplayRoot : DisplayRoot / Entry.lexically_relative(NativeScanRoot);
		};
		// ディレクトリ毎に開いた反復子を保持し，読めない枝でも親の兄弟へ戻る
		std::vector<Filesystem::directory_iterator> ScanStack;
		ScanStack.emplace_back(NativeScanRoot, ScanError);
		const Filesystem::directory_iterator IterEnd;
		if(ScanError) {
			PrintReadError(Path, ScanError, &Excluded);
			// 起点を開けない失敗の返戻
			return false;
		}
		while(!ScanStack.empty()) {
			ParserAllocator::CheckInterruption();
			if(ScanStack.back() == IterEnd) {
				ScanStack.pop_back();
				continue;
			}
			const bool IsTopLevel = ScanStack.size() == 1;
			const Filesystem::directory_entry Entry = *ScanStack.back();
			// 親を先に読み進め，子を開けない場合にも兄弟の位置を失わない
			ScanStack.back().increment(ScanError);
			if(ScanError) {
				PrintScanReadError(GetDisplayPath(Entry.path().parent_path()), ScanError, Excluded);
				ScanStack.pop_back();
				ScanError.clear();
			}
			// 処理系の名前で除外と拡張子を判定し，対象だけを UTF-8 へ移す
			const Filesystem::path EntryName = Entry.path().filename();
			const NativeNameView FileName(EntryName.native());
			// シンボリックリンクの非追跡（名前の除外より先に数え，リンクを名前の除外に数えない）
			std::error_code EntryError;
			const Filesystem::file_status EntryStatus = Entry.symlink_status(EntryError);
			const bool IsLinked = IsLink(Entry.path(), EntryStatus, EntryError);
			// エントリ状態の取得失敗
			if(EntryError) {
				PrintScanReadError(GetDisplayPath(Entry.path()), EntryError, Excluded);
				continue;
			}
			const ::Lang Detected =
			IsLinked || Filesystem::is_regular_file(EntryStatus) ? Lang::Detect(EntryName.native()) : Lang::Get(Lang::Unknown);
			// 未追跡リンクの対応対象だけの計数
			if(IsLinked) {
				std::error_code TargetError;
				if(Filesystem::is_directory(Entry.status(TargetError)) || Detected != Lang::Unknown) ++Excluded.Links;
				continue;
			}
			// 再生成される領域と点始りのディレクトリの枝刈
			if(Filesystem::is_directory(EntryStatus)) {
				// 依存先・ツール専用の名前は全ての深さ，ソースにも使う生成物の名前は起点の直下だけで除外
				static constexpr std::string_view AnyDepthDirNames[] =
				{ "Carthage", "Pods", "bower_components", "node_modules", "vendor", "venv" };
				static constexpr std::string_view TopLevelDirNames[] = { "build", "dist", "obj", "out", "target" };
				const NativeNameView DirName = FileName;
				const auto IsListed = [&DirName](const std::span<const std::string_view> Names) -> bool {
					// 名前の一覧の何れかと大小を問わず一致するかの返戻
					return std::ranges::any_of(
						Names,
						[&DirName](const std::string_view Name) -> bool {
							// 名前の一致の返戻
							return IsEqualIgnoringAsciiCase(DirName, Name);
						}
					);
				};
				const bool IsExcludedDir =
				!DirName.empty() && DirName.front() == '.' || IsListed(AnyDepthDirNames) || IsTopLevel && IsListed(TopLevelDirNames);
				// 除外ディレクトリの場合
				if(IsExcludedDir) ++Excluded.ByName;
				else {
					Filesystem::directory_iterator Child(FileIO::GetNativePath(Entry.path(), true), ScanError);
					if(ScanError) {
						PrintScanReadError(GetDisplayPath(Entry.path()), ScanError, Excluded);
						ScanError.clear();
					} else ScanStack.push_back(std::move(Child));
				}
				continue;
			}
			// 通常ファイル以外を除外
			if(!Filesystem::is_regular_file(EntryStatus)) continue;
			// 圧縮済ファイル名と再生成される依存固定ファイル名かを判定（`.lock` で終わる名前と `package-lock.json`）
			// 除外ファイルの場合
			if(
				const bool IsExcludedFile =
				HasSuffixIgnoringAsciiCase(FileName, ".min.js") || HasSuffixIgnoringAsciiCase(FileName, ".min.css") ||
				HasSuffixIgnoringAsciiCase(FileName, ".lock") || IsEqualIgnoringAsciiCase(FileName, "package-lock.json");
				IsExcludedFile
			) {
				// 名前除外数の対応対象だけの加算（未追跡リンクの計数と揃え，元から整形しない名前で数を膨らませない）
				if(Detected != Lang::Unknown) ++Excluded.ByName;
				// 次のエントリへ移動
				continue;
			}
			// 走査中の拡張子に依る言語決定
			if(Detected != Lang::Unknown) {
				std::string EntryPath, Display;
				#if defined(_WIN32)
				try {
					EntryPath = FileIO::GetPathText(Entry.path());
					Display = FileIO::GetPathText(GetDisplayPath(Entry.path()));
				} catch(const std::system_error &E) {
					if(E.code().value() != ERROR_NO_UNICODE_TRANSLATION && E.code() != std::errc::illegal_byte_sequence) throw;
					PrintScanReadError(GetDisplayPath(Entry.path()), E.code(), Excluded);
					continue;
				}
				#else
				EntryPath = FileIO::GetPathText(Entry.path());
				Display = FileIO::GetPathText(GetDisplayPath(Entry.path()));
				#endif
				const FileIO Input(EntryPath);
				std::vector<FileIdentity> Identity = Input.GetPathIdentity();
				bool HasUnknownIdentity = Input.GetReadError() == std::errc::operation_not_supported;
				if(Identity.size() < RootIdentity.size() || !std::equal(RootIdentity.begin(), RootIdentity.end(), Identity.begin())) {
					Identity.clear();
					HasUnknownIdentity = false;
				}
				Files.push_back({ EntryPath, Detected, std::move(Display), std::move(Identity), HasUnknownIdentity });
			}
		}
	// 明示した FIFO・端末・装置等を対象０件の成功として扱わない
	} else Rejection = PathRejection::NotRegularFile;
	// 収集成功の返戻
	return true;
}

/**
 * 通常の標準出力を確定して，確保を伴わない共通の要約を使う関数
 * 計算量：件数の桁数 D に対し O(D)
 * @param Summary 完了数と入口の条件
 * @return 書出成功なら０，失敗なら１
 */
int Main::PrintSummary(const FormattingSummary &Summary) {
	// バッファ内の対象毎の報告を，直接の書出より先に確定
	if(FlushStdout()) return 1;
	if(!ParserAllocator::WriteSummary(Summary)) {
		if(Summary.Kind != SummaryKind::NoSupportedCheck) std::fputs("Error: failed to write stdout\n", stderr);
		// 要約を書けなかった失敗の返戻
		return 1;
	}
	// 要約の書出成功の返戻
	return 0;
}

/**
 * POSIX の継承した無視を保持して，書戻の中断を登録する関数
 * 計算量：O(1)
 * @param Signal 協調して扱う信号
 */
void Main::RegisterInterruption(const int Signal) {
	#if !defined(_WIN32)
	struct sigaction Current {};
	// 現在の動作を読めない場合も，継承した動作を変更しない終了
	if(::sigaction(Signal, nullptr, &Current) || Current.sa_handler == SIG_IGN) return;
	#endif
	// Windows の CRT は従来の登録を保持
	std::signal(Signal, RecordInterruption);
	// 終了
	return;
}

/**
 * 完了した結果を非常用の要約へ計上する関数
 * 計算量：O(1)
 * @param Result 原文取得と書戻も含めた確定済の結果
 */
void Main::CountEmergencyResult(const FileJobResult &Result) {
	ResultCounts Counts;
	// 未完了又は失敗の対象は非常用の全対象数から未成功として数える終了
	if(Result.IsCancelled || !Result.IsReadOk || Result.IsWriteFailed) return;
	switch(Result.Format.Classification) {
	case FormatClassification::Unchanged:
		Counts.Unchanged = 1;
		break;
	case FormatClassification::Changed:
		Counts.Changed = 1;
		break;
	case FormatClassification::Partial:
		Counts.Partial = 1;
		break;
	case FormatClassification::Skipped:
		Counts.Skipped = 1;
		break;
	case FormatClassification::Failed:
		// 失敗を完了した検査成功へ数えない終了
		return;
	}
	if(Counts.Unchanged || Counts.Changed || Counts.Partial) Counts.WriteNeeded = Result.HasTextChange;
	ParserAllocator::RecordResult(Counts);
	// 終了
	return;
}

/**
 * ファイル一覧整形関数（結果を入力順に格納）
 * 計算量：パス総長 P，読込・整形・書込の総費用 Q，対象件数 F に対し O(P + Q + F log F)
 * @param Shaver `LineBreaker` を持つフォーマッター
 * @param Files 対象ファイル一覧（言語は収集時に確定済）
 * @param ShouldWrite true で整形結果をファイルへ書き戻す
 * @param ShouldKeepOriginal true で整形前のテキストを `Original` に保持（差分表示用）
 * @param IsSkipsLint true で規約検査を省略
 * @param Counted 呼出間で共有する実体・入口文法毎の受理計数（状態が変われば更新）
 * @param Replays 差分表示の呼出間で共有する同じ実体・入口文法の結果（無ければ此の呼出内だけで共有する）
 * @return 各ファイルの処理結果（`Files` と添字で対応）
 * @throws std::bad_alloc 対象グループ又は結果の領域を確保出来ない場合
 * @throws std::filesystem::filesystem_error パスのエンコーディングを変換出来ない場合
 * @throws std::system_error 並列実行を開始出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::vector<Main::FileJobResult> Main::ProcessFiles(
	const Formatter &Shaver,
	const std::vector<TargetFile> &Files,
	const bool ShouldWrite,
	const bool ShouldKeepOriginal,
	const bool IsSkipsLint,
	InspectionCache &Counted,
	ReplayStore *const Replays
) {
	// 同じ実体の別名を同じスレッドへ纏め，先の書戻後に次の別名を読み直す
	std::map<FileIdentity, size_t> IdentityGroups;
	std::vector<std::vector<size_t>> Groups;
	for(size_t Idx = 0; Idx < Files.size(); ++Idx) {
		const std::optional<FileIdentity> Identity = Files[Idx].Identity.empty() || Files[Idx].HasUnknownIdentity ?
		std::nullopt :
		std::optional<FileIdentity>(Files[Idx].Identity.back());
		size_t Group = Groups.size();
		if(Identity) Group = IdentityGroups.emplace(*Identity, Group).first->second;
		if(Group == Groups.size()) Groups.emplace_back();
		Groups[Group].push_back(Idx);
	}
	// 結果領域と並列実行の構成
	std::vector<FileJobResult> Results(Files.size());
	std::vector<std::optional<FileIO::InputInspection>> Inspections(Files.size());
	#if defined(ShaveFormatInterruptionTestHooks)
	// 検査入口だけで，最初の公開と残りの読込を親の信号送信に同期する
	const char *const GateText = std::getenv("SHAVEFMT_TEST_WRITE_GATE"),
	*const NotifyText = std::getenv("SHAVEFMT_TEST_WRITE_NOTIFY");
	const int Gate = GateText ? std::atoi(GateText) : -1, Notify = NotifyText ? std::atoi(NotifyText) : -1;
	#endif
	// 同じ実体と入口条件の受理計数を共有し，原文は待機列へ保持しない
	for(const std::vector<size_t> &Group : Groups) {
		if(ParserAllocator::IsStopping()) break;
		std::optional<std::array<uint64_t, 10>> GroupMetadata;
		for(const size_t Idx : Group) {
			if(ParserAllocator::IsStopping()) break;
			FileJobResult &Result = Results[Idx];
			if(Files[Idx].ReadError || Files[Idx].Identity.empty()) {
				Result.IsCancelled = false;
				Result.ReadError = Files[Idx].ReadError ? Files[Idx].ReadError : std::make_error_code(std::errc::state_not_recoverable);
				continue;
			}
			try {
				const GrammarProfile &Profile = GrammarProfile::Resolve(Files[Idx].Lang);
				const std::optional<ReplayKey> Key = Files[Idx].Identity.empty() || Files[Idx].HasUnknownIdentity ?
				std::nullopt :
				std::optional<ReplayKey>(ReplayKey{ Files[Idx].Identity.back(), &Profile });
				const InspectionCache::iterator Found = Key ? Counted.find(*Key) : Counted.end();
				const FileIO File(Files[Idx].Path, Files[Idx].Identity);
				Inspections[Idx] = File.InspectInput(
					[&](const size_t Size, const FileIO::InputReader &Read) -> size_t {
						// 元の入口条件の受理見積の返戻
						return EstimateWorkingBytes(Profile, Size, Read);
					},
					Found == Counted.end() ? nullptr : &Found->second
				);
				if(!Inspections[Idx]) {
					Result.IsCancelled = false;
					Result.ReadError = File.GetReadError();
					if(Key) Counted.erase(*Key);
				} else if(GroupMetadata && *GroupMetadata != Inspections[Idx]->Metadata) {
					Result.IsCancelled = false;
					Result.ReadError = std::make_error_code(std::errc::state_not_recoverable);
					Inspections[Idx].reset();
					if(Key) Counted.erase(*Key);
				} else {
					if(!GroupMetadata) GroupMetadata = Inspections[Idx]->Metadata;
					if(Key && Found == Counted.end()) Counted.emplace(*Key, *Inspections[Idx]);
					else if(Found != Counted.end() && Found->second.Metadata != Inspections[Idx]->Metadata) Found->second = *Inspections[Idx];
				}
			} catch(const ProcessingInterrupted &) {
				break;
			} catch(const std::exception &E) {
				Result.IsCancelled = false;
				Inspections[Idx].reset();
				if(!Files[Idx].Identity.empty()) Counted.erase({ Files[Idx].Identity.back(), &GrammarProfile::Resolve(Files[Idx].Lang) });
				Result.IsReadOk = true;
				Result.Format.Classification = FormatClassification::Failed;
				Result.Format.Reason = FailureReason::FromException(E);
				continue;
			}
			if(!Inspections[Idx]) continue;
			const FailureReason Reason = AcceptInput(
				Inspections[Idx]->Bytes,
				[&]() -> size_t {
					// 予約前に済ませた受理走査の計数の返戻
					return Inspections[Idx]->WorkingBytes;
				}
			);
			if(Reason != FailureReason::None) {
				Result.IsCancelled = false;
				Result.IsReadOk = true;
				Result.Format.Classification =
				Reason == FailureReason::WorkingMemoryExceeded ? FormatClassification::Failed : FormatClassification::Skipped;
				Result.Format.Reason = Reason;
				Inspections[Idx].reset();
			}
		}
	}
	for(size_t Idx = 0; Idx < Results.size(); ++Idx) if(!Inspections[Idx] && !Results[Idx].IsCancelled) {
		CountEmergencyResult(Results[Idx]);
	}
	// １件ずつ同時に処理する実体グループの予約量（別名の処理中は同じ原文と最大の作業域を共有する）
	struct Reservation {
		size_t Group; // 実体グループ
		size_t Input; // 同じ原文の大きさ
		size_t Work; // 直列に処理する別名の最大の作業域
	};
	std::list<Reservation> Pending;
	for(size_t Group = 0; Group < Groups.size(); ++Group) {
		size_t Input = 0, Work = 0;
		bool HasInput = false;
		for(const size_t Idx : Groups[Group]) if(Inspections[Idx]) {
			Input = std::max(Input, Inspections[Idx]->Bytes);
			Work = std::max(Work, Inspections[Idx]->WorkingBytes);
			HasInput = true;
		}
		if(HasInput) Pending.push_back({ Group, Input, Work });
	}
	const size_t ThreadCount = Parallel::DecideThreads(Groups.size(), 12, 2); // 並列数上限と起動対象数の閾値
	std::mutex BudgetLock;
	std::condition_variable BudgetFreed;
	uintmax_t InFlight = 0, InputBytes = 0;
	size_t FreedGeneration = 0; // 通知を取り逃した場合も確認する予約解放の世代
	// ファイル処理の並列実行
	Parallel::ForChunks(
		ThreadCount,
		ThreadCount,
		[&](size_t, size_t, size_t) -> void {
			// 未処理ファイルの取得ループ
			while(true) {
				// 全終了経路での保有枠返却と待機者の起動
				struct BudgetRelease {
					uintmax_t Weight; // 此のファイルの見積作業域
					uintmax_t InputWeight; // 読込原文の寸法
					std::mutex &Lock; // 総量の排他
					std::condition_variable &Freed; // 空きを待つ者への通知
					uintmax_t &Total; // 処理中の見積作業域の和
					uintmax_t &Inputs; // 保持する入力の大きさの和
					size_t &Generation; // 同じロック内で更新する予約解放の世代

					/**
					 * デストラクタ（抱えた分を総量から返し，空きを待つ者を起こす）
					 * 計算量：O(1)
					 */
					~BudgetRelease() {
						// 予約した両枠の返却
						{
							// 総量更新の排他
							const std::lock_guard<std::mutex> Guard(Lock);
							Total -= Weight;
							Inputs -= InputWeight;
							++Generation;
						}
						Freed.notify_all();
						// 終了
						return;
					}
				} Release { 0, 0, BudgetLock, BudgetFreed, InFlight, InputBytes, FreedGeneration };
				Reservation Selected {};
				{
					std::unique_lock<std::mutex> Lock(BudgetLock);
					std::list<Reservation>::iterator Choice = Pending.end();
					const auto CanSelect = [&]() -> bool {
						// 中断要求に依る待機解除の返戻
						if(ParserAllocator::IsStopping()) return true;
						for(std::list<Reservation>::iterator it = Pending.begin(); it != Pending.end(); ++it) {
							if(const Reservation &Job = *it; Job.Input <= MaxInputBytes - InputBytes && Job.Work <= MaxWorkingBytes - InFlight) {
								Choice = it;
								// 大きい物から実行可能なグループを選んだ事の返戻
								return true;
							}
						}
						// 未処理のグループが無い事の返戻
						return Pending.empty();
					};
					while(!CanSelect()) {
						const size_t Generation = FreedGeneration;
						// 時間切だけでは予約量を再照会せず，中断又は取り逃した解放の通知を確認する
						while(
							BudgetFreed.wait_for(Lock, std::chrono::milliseconds(50)) == std::cv_status::timeout && !ParserAllocator::IsStopping() &&
							FreedGeneration == Generation
						);
					}
					if(Pending.empty() || ParserAllocator::IsStopping()) break;
					Selected = *Choice;
					Pending.erase(Choice);
					InputBytes += Selected.Input;
					InFlight += Selected.Work;
					Release.InputWeight = Selected.Input;
					Release.Weight = Selected.Work;
				}
				std::optional<std::string> GroupOriginal;
				std::map<const GrammarProfile *, size_t> Formatted;
				for(const size_t LocalIdx : Groups[Selected.Group]) {
					if(ParserAllocator::IsStopping()) break;
					if(!Inspections[LocalIdx]) continue;
					// 此の対象専用の結果フィールド
					FileJobResult &Result = Results[LocalIdx];
					Result.IsCancelled = false;
					struct CompletionGuard {
						const FileJobResult &Result; // 完了時に計上する対象の結果
						std::optional<ParserAllocator::WriteGuard> Publication; // 書戻後の計上迄を非常終了から保全

						/**
						 * 途中の continue と例外も含めた完了数の計上
						 * 計算量：O(1)
						 */
						~CompletionGuard() {
							CountEmergencyResult(Result);
							// 終了
							return;
						}
					};
					CompletionGuard Completed { Result, std::nullopt };
					// 例外が書込失敗に当たる段階の記録
					bool IsWriting = false;
					try {
						// 原本の同一性を監視する入出力口
						const std::string SafeName =
						FileIO::ForDisplay(Files[LocalIdx].Display.empty() ? Files[LocalIdx].Path : Files[LocalIdx].Display);
						const ParserAllocator::DisplayGuard Display(SafeName);
						#if defined(ShaveFormatInterruptionTestHooks)
						if(LocalIdx && Gate > -1) {
							char Released;
							ssize_t Count;
							do Count = ::read(Gate, &Released, 1);
							while(Count < 0 && errno == EINTR);
							if(Count != 1) throw std::system_error(Count < 0 ? errno : EIO, std::generic_category());
							while(!ParserAllocator::IsInterrupted()) std::this_thread::yield();
							ParserAllocator::CheckInterruption();
						}
						#endif
						const FileIO File(Files[LocalIdx].Path, Files[LocalIdx].Identity, GroupOriginal ? std::nullopt : Inspections[LocalIdx]);
						// 原本の読込と整形
						if(!File.IsFileReadSuccessful(Result.Text, ShouldWrite)) {
							Result.ReadError = File.GetReadError();
							continue;
						}
						if(GroupOriginal && Result.Text != *GroupOriginal) {
							std::string Empty;
							Result.Text.swap(Empty);
							Result.ReadError = std::make_error_code(std::errc::state_not_recoverable);
							continue;
						}
						if(!GroupOriginal && Groups[Selected.Group].size() > 1) GroupOriginal = Result.Text;
						Result.IsReadOk = true;
						if(ShouldKeepOriginal) Result.Original = Result.Text;
						const GrammarProfile *const Profile = &GrammarProfile::Resolve(Files[LocalIdx].Lang);
						ReadOnlyReplay *Replay = nullptr;
						if(!ShouldWrite && Replays && !Files[LocalIdx].Identity.empty() && !Files[LocalIdx].HasUnknownIdentity) {
							const ReplayCache::iterator Entry = Replays->Entries.find({ Files[LocalIdx].Identity.back(), Profile });
							if(Entry != Replays->Entries.end()) Replay = &Entry->second;
						}
						bool HasReplay = false;
						if(Replay && Replay->Offset && !std::fseek(Replays->Snapshot.get(), *Replay->Offset, SEEK_SET)) {
							// 別名毎の安定した読取の後に，原文とのバイト単位の一致を確かめる
							std::array<char, 0X10000> Buffer;
							bool IsSame = Result.Text.size() == Replay->OriginalBytes;
							for(size_t Pos = 0; IsSame && Pos < Replay->OriginalBytes;) {
								const size_t Len = std::min(Buffer.size(), Replay->OriginalBytes - Pos);
								IsSame = std::fread(Buffer.data(), 1, Len, Replays->Snapshot.get()) == Len &&
								std::string_view(Result.Text).substr(Pos, Len) == std::string_view(Buffer.data(), Len);
								Pos += Len;
							}
							if(IsSame) {
								std::string Candidate(Replay->OutputBytes, '\0');
								if(std::fread(Candidate.data(), 1, Candidate.size(), Replays->Snapshot.get()) == Candidate.size()) {
									Result.Format = Replay->Format;
									Result.HasTextChange = Replay->HasTextChange;
									Result.Text = std::move(Candidate);
									HasReplay = true;
								}
							}
						}
						if(!HasReplay) {
							if(
								const std::map<const GrammarProfile *, size_t>::const_iterator Previous = Formatted.find(Profile);
								!ShouldWrite && Previous != Formatted.end()
							) {
								const FileJobResult &First = Results[Previous->second];
								Result.Format = First.Format;
								Result.HasTextChange = First.HasTextChange;
								if(ShouldKeepOriginal) Result.Text = First.Text;
							} else {
								Result.Format = Shaver.FormatAccepted(Result.Text, Files[LocalIdx].Lang, !IsSkipsLint, true);
								Result.HasTextChange = Result.Format.Text != Result.Text;
								Result.Text = std::move(Result.Format.Text);
								const bool CanReplay =
								!ShouldWrite && (Groups[Selected.Group].size() > 1 || Replay) && !Result.Format.Reason.IsResourceLimit() &&
								Result.Format.Reason != FailureReason::ResourceFailure;
								if(CanReplay && Groups[Selected.Group].size() > 1) Formatted.emplace(Profile, LocalIdx);
								if(
									CanReplay && Replay && Replay->Remaining > 1 && !Replay->Offset && Result.Original.size() <= MaxInputBytes - Replays->Bytes &&
									Result.Text.size() <= MaxInputBytes - Replays->Bytes - Result.Original.size()
								) {
									// 退避出来ない組も通常の整形結果を報告し，以後の別名は再整形する
									if(!Replays->HasOpened) {
										Replays->HasOpened = true;
										Replays->Snapshot.reset(FileIO::OpenTemporaryStream());
									}
									static_assert(MaxInputBytes <= static_cast<size_t>(std::numeric_limits<long>::max()));
									const long Offset = static_cast<long>(Replays->Bytes);
									// 部分書出に失敗しても，次の記録は予約した範囲の外へだけ追記する
									Replays->Bytes += Result.Original.size() + Result.Text.size();
									std::FILE *const Snapshot = Replays->Snapshot.get();
									if(Snapshot) std::clearerr(Snapshot);
									if(
										Snapshot && !std::fseek(Snapshot, Offset, SEEK_SET) &&
										std::fwrite(Result.Original.data(), 1, Result.Original.size(), Snapshot) == Result.Original.size() &&
										std::fwrite(Result.Text.data(), 1, Result.Text.size(), Snapshot) == Result.Text.size() && !std::fflush(Snapshot)
									) {
										Replay->Format = Result.Format;
										Replay->OriginalBytes = Result.Original.size();
										Replay->OutputBytes = Result.Text.size();
										Replay->HasTextChange = Result.HasTextChange;
										Replay->Offset = Offset;
										++Replays->Stored;
									}
								}
							}
						}
						// BOM 無・LF の整形結果の書戻（整形仕様 10.3）
						if(ShouldWrite && Result.HasTextChange) {
							ParserAllocator::CheckInterruption();
							Result.LinkCount = File.GetLinkCount();
							Completed.Publication.emplace();
							if(!Completed.Publication->CanWrite()) {
								Result.IsCancelled = true;
								continue;
							}
							IsWriting = true;
							Result.IsWriteFailed = !File.IsFileWriteSuccessful(Result.Text, &Result.IsHardLinkBroken, &Result.WriteError);
							#if defined(ShaveFormatInterruptionTestHooks)
							if(!LocalIdx && Notify > -1 && !Result.IsWriteFailed) if(::write(Notify, "p", 1) != 1) {
								throw std::system_error(errno, std::generic_category());
							}
							#endif
						}
					} catch(const ProcessingInterrupted &) {
						Result.IsCancelled = true;
					} catch(const std::exception &E) {
						// 読込を終えた後の例外だけをフォーマッターのリソース・内部の失敗とし，読込中の例外は読込の失敗の儘とする
						Result.Format.Classification = FormatClassification::Failed;
						Result.Format.Reason = FailureReason::FromException(E);
						Result.IsWriteFailed = IsWriting;
						Result.Format.Warnings.clear();
					}
					// 差分非表示時は次のファイルの前に整形結果の領域を解放（全対象量に比例する常駐量を抑制）
					if(!ShouldKeepOriginal) {
						std::string Empty;
						Result.Text.swap(Empty);
					}
				}
			}
			// 終了
			return;
		}
	);
	// 書戻の試行後は成功・競合・例外の何れでも，並列処理の終了後に元の状態の計数を失効させる
	if(ShouldWrite) for(size_t Idx = 0; Idx < Files.size(); ++Idx) if(Results[Idx].HasTextChange && !Files[Idx].Identity.empty()) {
		Counted.erase({ Files[Idx].Identity.back(), &GrammarProfile::Resolve(Files[Idx].Lang) });
	}
	// 各ファイルの処理結果の返戻
	return Results;
}

/**
 * 行末を本文から分けた差分行の出力関数
 * 計算量：行のバイト数 B に対し O(B)
 * @param Text LF を含む行
 * @param Prefix 文脈・削除・挿入を示す文字
 * @throws std::bad_alloc 表示用の本文を確保出来ない場合
 * @throws std::length_error 表示用の本文の上限を超える場合
 */
void Main::PrintDiffLine(const std::string_view Text, const char Prefix) {
	// CRLF は行末として保持し，本文中の制御文字は安全な表示へ変換
	const bool HasNewline = Text.ends_with('\n'), HasCrLf = Text.ends_with("\r\n");
	const size_t Ending = HasCrLf ? 2 : HasNewline ? 1 : 0;
	const std::string Body = FileIO::ForDisplay(Text.substr(0, Text.size() - Ending), true);
	std::printf("%c%s%s\n", Prefix, Body.c_str(), HasCrLf ? "\r" : "");
	if(!HasNewline) std::puts("\\ No newline at end of file");
	// 終了
	return;
}

/**
 * 既存の編集列へ文脈を加えた統一差分の出力関数
 * 計算量：差分探索の他，両本文のバイト数 B と行数 L に対し O(B + L)，追加領域 O(L)
 * @param Result 原文と完成候補
 * @param PathLabel 表示用の置換を済ませたパス
 * @throws std::bad_alloc 差分行又は表示用の本文を確保出来ない場合
 * @throws std::length_error 差分行又は本文の上限を超える場合
 */
void Main::PrintDiff(const FileJobResult &Result, const char *const PathLabel) {
	// 各行の直前の原文・候補の行数を保持する表示列
	struct DiffLine {
		char Prefix;
		std::string_view Text;
		size_t OldLine;
		size_t NewLine;
	};
	std::vector<DiffLine> Lines;
	size_t OldLine = 0, NewLine = 0, Offset = 0;
	const auto Append = [&](const std::string_view Text, const char Prefix) -> void {
		Lines.push_back({ Prefix, Text, OldLine, NewLine });
		if(Prefix != '+') {
			++OldLine;
			Offset += Text.size();
		}
		if(Prefix != '-') ++NewLine;
		// 終了
		return;
	};
	const auto AppendContext = [&]() -> void {
		const size_t Break = Result.Original.find('\n', Offset), End = Break == std::string::npos ? Result.Original.size() : Break + 1;
		Append(std::string_view(Result.Original).substr(Offset, End - Offset), ' ');
		// 終了
		return;
	};
	for(const LineChange &Change : CompareLines(Result.Original, Result.Text)) {
		while((Change.IsInsertion ? NewLine : OldLine) < Change.Line) AppendContext();
		Append(Change.Text, Change.IsInsertion ? '+' : '-');
	}
	while(Offset < Result.Original.size()) AppendContext();
	#if defined(_WIN32)
	// 統一差分の見出しは区切を `/` で書く（適用処理は Windows の `\` を区切と読まない）
	std::string Header(PathLabel);
	std::replace(Header.begin(), Header.end(), '\\', '/');
	std::printf("--- a/%s\n+++ b/%s\n", Header.c_str(), Header.c_str());
	#else
	std::printf("--- a/%s\n+++ b/%s\n", PathLabel, PathLabel);
	#endif
	// 文脈が接する変更を同じ領域へ結合し，離れた領域を順に出力
	for(size_t Pos = 0; Pos < Lines.size();) {
		if(Lines[Pos].Prefix == ' ') {
			++Pos;
			continue;
		}
		const size_t Begin = Pos > 3 ? Pos - 3 : 0;
		size_t LastChange = Pos, Next = Pos + 1;
		for(; Next < Lines.size(); ++Next) {
			if(Lines[Next].Prefix != ' ') LastChange = Next;
			else if(Next - LastChange > 6) break;
		}
		const size_t End = LastChange + std::min(size_t(4), Lines.size() - LastChange);
		const size_t OldEnd = End < Lines.size() ? Lines[End].OldLine : OldLine;
		const size_t NewEnd = End < Lines.size() ? Lines[End].NewLine : NewLine, OldCount = OldEnd - Lines[Begin].OldLine;
		const size_t NewCount = NewEnd - Lines[Begin].NewLine;
		std::printf(
			"@@ -%zu,%zu +%zu,%zu @@\n",
			OldCount ? Lines[Begin].OldLine + 1 : Lines[Begin].OldLine,
			OldCount,
			NewCount ? Lines[Begin].NewLine + 1 : Lines[Begin].NewLine,
			NewCount
		);
		for(size_t Row = Begin; Row < End; ++Row) PrintDiffLine(Lines[Row].Text, Lines[Row].Prefix);
		Pos = Next;
	}
	// 終了
	return;
}

/**
 * 実行本体（指定ファイル／ディレクトリの整形，`-w` 書込／`-c` チェック／`--diff` 差分表示）
 * 計算量：入力収集・整形・入出力の総費用 Q に対し O(Q)
 * @param Argc コマンドライン引数の数
 * @param Argv コマンドライン引数の配列
 * @return 正常終了時 `0`，処理失敗又は要修正時 `1`，引数不正時 `2`
 * @throws std::bad_alloc 実行データの確保に失敗した場合
 * @throws std::filesystem::filesystem_error パスの変換に失敗した場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
int Main::Execute(const int Argc, char *const Argv[]) {
	// 矛盾を検査済の実行設定
	const Options Settings = ParseArgs(Argc, Argv);
	// 解析中の終了要求をストリームの共通検査へ返す
	if(Settings.ShouldExit) return Settings.ExitCode;
	// 書戻の公開と後始末を保全する実行だけ，中断を協調して扱う
	if(Settings.ShouldWrite) {
		RegisterInterruption(SIGINT);
		RegisterInterruption(SIGTERM);
	}
	// 行幅を受け取るのは `LineBreaker` だけで，他の工程へは値を渡さない（整形仕様 3 の設計要件）
	const char *const Observation = std::getenv("SHAVEFMT_VERIFY_COMMENT_SEPARATION");
	const Formatter Shaver {
		Core::LineBreaker(Settings.MaxChars),
		Observation && *Observation,
		static_cast<bool>(std::getenv("SHAVEFMT_WARNINGS")),
		static_cast<bool>(std::getenv("SHAVEFMT_STRUCT_DIAG"))
	};
	// 標準入力モードでは VSCode 拡張等の埋込利用向に `--lang` 必須
	if(Settings.ShouldReadStdin) {
		if(Settings.OverrideLang == Lang::Unknown) {
			std::fprintf(stderr, "Error: --stdin requires --lang ID\n");
			// `--stdin` で `--lang` 指定不在の使用法違反としての返戻
			return UsageErrorCode;
		}
		// 標準入力の一括読込
		std::string Src;
		// 64 KiB
		char Chunk[0X10000]; // 標準入力の読込単位（64 KiB）
		while(const size_t ReadLen = std::fread(Chunk, 1, sizeof(Chunk), stdin)) {
			// 上限を超える入力はファイルのパスと同じく見送，読んだ分と残りを其の儘コピーする（エディターの `:%!` 連携で本文を消さない）
			if(ReadLen > MaxInputBytes - Src.size()) {
				std::fputs("[skip] <stdin>: input exceeds size limit\n", stderr);
				const std::string_view Remedy = FailureReason(FailureReason::InputSizeExceeded).GetRemedy();
				std::fprintf(stderr, "[notice] <stdin>: %.*s\n", static_cast<int>(Remedy.size()), Remedy.data());
				bool IsWritten =
				std::fwrite(Src.data(), 1, Src.size(), stdout) == Src.size() && std::fwrite(Chunk, 1, ReadLen, stdout) == ReadLen;
				for(size_t Rest = 0; IsWritten && (Rest = std::fread(Chunk, 1, sizeof(Chunk), stdin));) {
					IsWritten = std::fwrite(Chunk, 1, Rest, stdout) == Rest;
				}
				if(!IsWritten || std::ferror(stdin) || std::fflush(stdout)) {
					std::fputs("Error: failed to copy stdin to stdout\n", stderr);
					// 原文をコピー切れなかった事の返戻
					return 1;
				}
				// 見送の返戻（指定時だけ非零）
				return Settings.ShouldFailOnSkip ? 1 : 0;
			}
			try {
				Src.append(Chunk, ReadLen);
			} catch(const std::bad_alloc &E) {
				PrintException(E, "<stdin>");
				bool IsWritten = std::fwrite(Src.data(), 1, Src.size(), stdout) == Src.size();
				IsWritten = std::fwrite(Chunk, 1, ReadLen, stdout) == ReadLen && IsWritten;
				while(const size_t Remaining = std::fread(Chunk, 1, sizeof(Chunk), stdin)) {
					IsWritten = std::fwrite(Chunk, 1, Remaining, stdout) == Remaining && IsWritten;
				}
				if(!IsWritten || std::ferror(stdin) || std::fflush(stdout)) std::fputs("Error: failed to copy stdin to stdout\n", stderr);
				// メモリ不足の失敗の返戻
				return 1;
			}
		}
		// 読込途中で失敗した内容を整形すると，切り詰められたドキュメントを「整形結果」として返す事への変化
		if(std::ferror(stdin)) {
			std::fputs("Error: failed to read stdin\n", stderr);
			// 入力が欠けた事の返戻
			return 1;
		}
		FormatResult Result;
		const ParserAllocator::DisplayGuard Display("<stdin>");
		ParserAllocator::SetStdin(Src);
		std::string_view Output = Src; // 診断の準備を終える迄，失敗時に返す原文を保持する
		try {
			Result = Shaver.Format(Src, Settings.OverrideLang, !Settings.ShouldSkipLint);
			// 整形未実施経路の明示
			switch(Result.Classification) {
			case FormatClassification::Changed:
			case FormatClassification::Unchanged:
			case FormatClassification::Partial:
				PrintEncodingNotices(Result, "<stdin>");
				break;
			case FormatClassification::Skipped:
			case FormatClassification::Failed:
				PrintUnformatted(Result, "<stdin>");
				break;
			}
			PrintDiagnostics(Result, "<stdin>");
			std::fputs(Result.Observed.c_str(), stderr);
			PrintWarnings(Result.Warnings, "<stdin>");
			Output = Result.Text; // 確保を伴う通知を終えた結果だけを出力へ渡す
		} catch(const std::exception &E) {
			// 整形又は診断の確保例外も原文を保持し，回復経路では動的確保を行わない
			Result.Classification = FormatClassification::Failed;
			PrintException(E, "<stdin>");
		}
		ParserAllocator::SetStdin(std::string_view());
		// 部分書込と書出失敗での非零終了に依るドキュメント切詰防止
		if(std::fwrite(Output.data(), 1, Output.size(), stdout) != Output.size() || std::fflush(stdout)) {
			std::fputs("Error: failed to write stdout\n", stderr);
			// 出力が欠けた事の返戻
			return 1;
		}
		// 原文を失わない様に標準出力へ結果を渡し，差戻又は指定時の見送だけを非零とする終了状態の返戻
		return Result.Classification == FormatClassification::Failed ||
		Settings.ShouldFailOnSkip && Result.Classification == FormatClassification::Skipped ? 1 : 0;
	}
	// パス入力の有無の検査
	if(Settings.Paths.empty()) {
		PrintUsage(true);
		// 入力対象の無い使用法の誤りとしての返戻
		return UsageErrorCode;
	}
	// 重複除去前の整形対象
	std::vector<TargetFile> Files;
	// パス指定のエラー
	bool IsFailedPath = false, IsUnsupportedPath = false, IsConflictingPath = false;
	// 走査で読まなかったパス数
	ScanExclusions Excluded;
	for(const std::string &Path : Settings.Paths) {
		// 此の明示パスの不適合
		// 失敗の内容は原因を知る収集側が報告済の為，此処では非零終了のマーカーだけを立てる
		if(
			PathRejection Rejection = PathRejection::None;
			!IsFileCollectionSuccessful(Path, Settings.OverrideLang, Files, Rejection, Excluded)
		) IsFailedPath = true;
		else switch(Rejection) {
		case PathRejection::LangConflict:
			std::fprintf(stderr, "Error: --lang conflicts with the file extension: %s\n", FileIO::ForDisplay(Path).c_str());
			IsConflictingPath = true;
			break;
		case PathRejection::UnknownLang:
			std::fprintf(
				stderr,
				"Error: unsupported file type: %s\n[notice] %s: use --lang ID to format it anyway\n",
				FileIO::ForDisplay(Path).c_str(),
				FileIO::ForDisplay(Path).c_str()
			);
			IsUnsupportedPath = true;
			break;
		case PathRejection::NotRegularFile:
			// `--lang` では解決しない為，拡張子の案内を出さない
			std::fprintf(stderr, "Error: not a regular file: %s\n", FileIO::ForDisplay(Path).c_str());
			IsUnsupportedPath = true;
			break;
		}
	}
	// 不在・対応外・言語不一致を非零終了として偽合格を防止（不一致はツールの誤用），言語不一致時の使用法エラーの返戻
	if(IsConflictingPath) return UsageErrorCode;
	// 名指したパスの不備は，読取専用の残りの対象の処理後も失敗とする
	const bool HasPathError = IsFailedPath || IsUnsupportedPath;
	// 対象の無い場合の報告
	if(Files.empty() && !HasPathError && !Excluded.ReadFailures) {
		FormattingSummary Summary;
		Summary.ExcludedByName = Excluded.ByName;
		Summary.UnfollowedLinks = Excluded.Links;
		Summary.Kind = Settings.ShouldCheck ? SummaryKind::NoSupportedCheck : SummaryKind::NoSupportedFiles;
		// 対応対象無の通知結果と，検査又は名指した未追跡リンクの不合格の返戻
		return PrintSummary(Summary) || Settings.ShouldCheck || Settings.ShouldFailOnSkip && Excluded.NamedLinks;
	}
	// 全ての元のパスのリンク検査後に，実際の名前解決が返す絶対パスを収集順のキーとする
	using TargetMap = std::map<std::string, TargetFile>;
	TargetMap Targets;
	for(TargetFile &File : Files) {
		ParserAllocator::CheckInterruption();
		const FileIO Input(File.Path, File.Identity);
		std::optional<std::string> Key = Input.GetNormalizedPath();
		// 実体を開けない対象も，リンク検査済の絶対パスで重複と収集順を確定する
		if(!Key) {
			File.ReadError = Input.GetReadError();
			std::error_code Error;
			const Filesystem::path Absolute = FileIO::GetAbsolutePath(File.Path, Error);
			// 基準の名前空間を取得出来ない場合は，異なる対象を空のキーへ纏めない
			if(Error) throw std::system_error(Error);
			Key = FileIO::GetPathText(Absolute.lexically_normal());
		}
		// 最初の元のパスと実体の記録を保持し，重複した明示ファイルの言語だけを反映
		const std::pair<TargetMap::iterator, bool> Added = Targets.try_emplace(*Key, std::move(File));
		if(!Added.second && File.Display.empty()) Added.first->second.Lang = File.Lang;
	}
	Files.clear();
	// 表示用の変換前の UTF-8 のキーの辞書順で，重複を除いた対象を取り出す
	for(std::pair<const std::string, TargetFile> &Entry : Targets) Files.push_back(std::move(Entry.second));
	Targets.clear();
	// 書戻は名指したパスの不備が１件でも有れば開始せず，重複を除いた未処理数を報告する
	if(HasPathError && Settings.ShouldWrite) {
		FormattingSummary Summary;
		Summary.Total = Files.size();
		Summary.NotWritten = Files.size();
		Summary.ReadFailures = Excluded.ReadFailures;
		Summary.ExcludedByName = Excluded.ByName;
		Summary.UnfollowedLinks = Excluded.Links;
		Summary.ShouldWrite = true;
		PrintSummary(Summary);
		// 原本を全て保持した失敗の返戻
		return 1;
	}
	// 寸法の１回取得と収集順を添えた対象列の生成
	std::vector<SizedFile> SizedFiles;
	SizedFiles.reserve(Files.size());
	for(size_t Idx = 0; Idx < Files.size(); ++Idx) {
		ParserAllocator::CheckInterruption();
		// 寸法取得の失敗理由
		std::error_code ErrorCode;
		const Filesystem::path Absolute = FileIO::GetAbsolutePath(Files[Idx].Path, ErrorCode);
		const uintmax_t Size =
		ErrorCode ? std::numeric_limits<uintmax_t>::max() : Filesystem::file_size(FileIO::GetNativePath(Absolute), ErrorCode);
		SizedFiles.push_back({ Size, Idx, std::move(Files[Idx]) });
	}
	std::sort(
		SizedFiles.begin(),
		SizedFiles.end(),
		[](const SizedFile &FileA, const SizedFile &FileB) -> bool {
			// サイズ降順比較で A の方が大きければ true の返戻
			return FileA.Size != FileB.Size ? FileA.Size > FileB.Size : FileA.Order < FileB.Order;
		}
	);
	// 収集順から処理順へのマッピング
	std::vector<size_t> ReportOrder(SizedFiles.size());
	for(size_t Idx = 0; Idx < SizedFiles.size(); ++Idx) {
		ReportOrder[SizedFiles[Idx].Order] = Idx;
		Files[Idx] = std::move(SizedFiles[Idx].Target);
	}
	// 差分表示は報告順に１件ずつ処理し，本文を次の対象へ持ち越さない
	std::vector<FileJobResult> Results;
	InspectionCache Counted;
	ReplayStore Replays;
	if(Settings.ShouldShowDiff && !Settings.ShouldWrite) {
		for(const TargetFile &File : Files) if(!File.Identity.empty() && !File.HasUnknownIdentity) {
			++Replays.Entries[{ File.Identity.back(), &GrammarProfile::Resolve(File.Lang) }].Remaining;
		}
		std::erase_if(
			Replays.Entries,
			[](const ReplayCache::value_type &Entry) -> bool {
				// 別名が無く共有を要しない対象かの返戻
				return Entry.second.Remaining < 2;
			}
		);
	}
	ParserAllocator::SetSummary(
		Files.size(),
		Settings.ShouldCheck,
		Settings.ShouldWrite,
		Excluded.ByName,
		Excluded.Links,
		Excluded.ReadFailures
	);
	std::map<FileIdentity, std::pair<uint64_t, size_t>> LinkGroups;
	std::map<FileIdentity, std::string> LinkLabels;
	if(!Settings.ShouldShowDiff) {
		Results = ProcessFiles(Shaver, Files, Settings.ShouldWrite, false, Settings.ShouldSkipLint, Counted);
	}
	// 要約の分類別件数
	size_t UnchangedCount = 0, ChangedCount = 0, PartialCount = 0, WriteNeededCount = 0, FailureCount = 0, SkippedCount = 0;
	size_t WarningCount = 0;
	// 収集順での処理結果の報告
	for(const size_t ResultIdx : ReportOrder) {
		// 収集順で報告する対象結果
		std::vector<FileJobResult> CurResults;
		if(Settings.ShouldShowDiff) {
			CurResults = ProcessFiles(Shaver, { Files[ResultIdx] }, Settings.ShouldWrite, true, Settings.ShouldSkipLint, Counted, &Replays);
			if(!Files[ResultIdx].Identity.empty() && !Files[ResultIdx].HasUnknownIdentity) {
				const ReplayCache::iterator Entry =
				Replays.Entries.find({ Files[ResultIdx].Identity.back(), &GrammarProfile::Resolve(Files[ResultIdx].Lang) });
				if(Entry != Replays.Entries.end() && !--Entry->second.Remaining) {
					if(Entry->second.Offset && !--Replays.Stored) {
						// 最後の参照を終えた退避先を破棄し，次の組に空の領域を再使用する
						Replays.Snapshot.reset();
						Replays.Bytes = 0;
						Replays.HasOpened = false;
					}
					Replays.Entries.erase(Entry);
				}
			}
		}
		const FileJobResult &Result = Settings.ShouldShowDiff ? CurResults.front() : Results[ResultIdx];
		// `Files` の添字に対応する表示パスの作成（利用者が仕込める端末制御列を１度だけ無害化）
		const std::string SafePath =
		FileIO::ForDisplay(Files[ResultIdx].Display.empty() ? Files[ResultIdx].Path : Files[ResultIdx].Display);
		// 出力中だけ有効な表示名
		const char *const PathLabel = SafePath.c_str();
		// 標準エラーの前に標準出力を掃出し，`2>&1` の通知順を保持
		std::fflush(stdout);
		if(Result.IsCancelled) {
			std::fprintf(
				stderr,
				"Error: formatting could not be completed (%s), left unchanged: %s\n",
				ErrorText(std::make_error_code(std::errc::operation_canceled)).c_str(),
				PathLabel
			);
			++FailureCount;
			continue;
		}
		// 読取失敗ファイルでの偽の合格を防ぐ非零終了
		if(!Result.IsReadOk) {
			PrintReadError(Files[ResultIdx].Display.empty() ? Files[ResultIdx].Path : Files[ResultIdx].Display, Result.ReadError);
			++FailureCount;
			continue;
		}
		// 書込失敗ファイルの整形済件数からの除外と終了コード通知
		if(Result.IsWriteFailed) {
			if(Result.Format.Classification == FormatClassification::Failed) PrintUnformatted(Result.Format, PathLabel);
			else {
				// 権限・容量・競合を区別出来る書込失敗理由の表示
				const std::string Reason =
				Result.WriteError ? ErrorText(Result.WriteError) : "the file changed on disk or could not be verified";
				std::fprintf(stderr, "Error writing: %s: %s\n", PathLabel, Reason.c_str());
				std::fprintf(stderr, "[notice] %s: verify write access, free space, and concurrent changes before retrying\n", PathLabel);
			}
			++FailureCount;
			continue;
		}
		switch(Result.Format.Classification) {
		case FormatClassification::Failed:
			PrintUnformatted(Result.Format, PathLabel);
			++FailureCount;
			continue;
		case FormatClassification::Skipped:
			PrintUnformatted(Result.Format, PathLabel);
			++SkippedCount;
			continue;
		case FormatClassification::Changed:
		case FormatClassification::Unchanged:
		case FormatClassification::Partial:
			break;
		}
		// 原本のエンコーディングと改行様式保持時の規約との差の警告
		PrintEncodingNotices(Result.Format, PathLabel);
		// 差替で実体共有が切れる別名ファイルの通知
		if(Settings.ShouldWrite && Result.HasTextChange && !Files[ResultIdx].Identity.empty()) {
			const FileIdentity Identity = Files[ResultIdx].Identity.back();
			std::pair<uint64_t, size_t> &Group = LinkGroups[Identity];
			Group.first = std::max(Group.first, Result.LinkCount);
			++Group.second;
			if(Result.IsHardLinkBroken) LinkLabels.try_emplace(Identity, SafePath);
		}
		PrintDiagnostics(Result.Format, PathLabel);
		std::fputs(Result.Format.Observed.c_str(), stderr);
		PrintWarnings(Result.Format.Warnings, PathLabel);
		WarningCount += Result.Format.Warnings.size();
		ParserAllocator::RecordResult({ .Warnings = Result.Format.Warnings.size() });
		// 整形仕様 8.3 の分類別の計数（部分整形は完全整形と別に数え，書戻の要否はテキストの変化で持つ）
		switch(Result.Format.Classification) {
		case FormatClassification::Partial:
			++PartialCount;
			break;
		case FormatClassification::Changed:
			++ChangedCount;
			break;
		default:
			++UnchangedCount;
			break;
		}
		if(Result.HasTextChange) {
			++WriteNeededCount;
			if(Settings.ShouldShowDiff) PrintDiff(Result, PathLabel);
			if(Settings.ShouldWrite) std::printf("Formatted: %s\n", PathLabel);
			// 差分表示外でも要修正のパスを示し，検査記録へ対象の保持
			else if(!Settings.ShouldShowDiff) std::printf("Would format: %s\n", PathLabel);
		}
		// 各対象の差分報告を非常終了より先に確定した書出の返戻
		if(Settings.ShouldShowDiff && FlushStdout()) return 1;
	}
	for(const std::pair<const FileIdentity, std::string> &Entry : LinkLabels) {
		if(const std::pair<uint64_t, size_t> &Group = LinkGroups.at(Entry.first); Group.first > Group.second) {
			std::fprintf(stderr, "[notice] %s: hard link was broken; the other names keep the old content\n", Entry.second.c_str());
		}
	}
	// 通常と非常の要約は同じ名前付の件数と書出を使う
	FormattingSummary Summary;
	Summary.Counts = {
		.Unchanged = UnchangedCount,
		.Changed = ChangedCount,
		.Partial = PartialCount,
		.Skipped = SkippedCount,
		.Warnings = WarningCount,
		.WriteNeeded = WriteNeededCount
	};
	Summary.Total = Files.size();
	Summary.Failed = FailureCount;
	Summary.ReadFailures = Excluded.ReadFailures;
	Summary.ExcludedByName = Excluded.ByName;
	Summary.UnfollowedLinks = Excluded.Links;
	Summary.Kind = Settings.ShouldCheck ? SummaryKind::Check : SummaryKind::Done;
	Summary.ShouldWrite = Settings.ShouldWrite;
	// 要約の書出失敗・処理失敗又は検査不合格の返戻
	if(
		PrintSummary(Summary) || ParserAllocator::IsStopping() || HasPathError || Excluded.ReadFailures || FailureCount ||
		Settings.ShouldFailOnSkip && (SkippedCount || Excluded.NamedLinks) || Settings.ShouldCheck && WriteNeededCount
	) return 1;
	// 全パス処理成功の返戻
	return 0;
}

/**
 * ストリームの書出失敗を終了コードへ反映する関数
 * 標準出力を掃き出し，標準出力又は標準エラーの書出が失敗して居れば読み書きの失敗として１を返す（整形仕様 10.4）
 * 計算量：O(1)
 * @param Code 処理の終了コード
 * @return 書出が全て成功なら Code，失敗なら１
 */
int Main::FinishStreams(const int Code) {
	// 両方の通知を確定してから，POSIX では最初に受けた信号の既定の動作へ戻す
	const int OutputStatus = std::fflush(stdout), ErrorStatus = std::fflush(stderr);
	#if !defined(_WIN32)
	if(const int Signal = InterruptedSignal.load(); Signal) {
		std::signal(Signal, SIG_DFL);
		std::raise(Signal);
		// 信号の再送に失敗しても，中断を正常終了にしない返戻
		return 1;
	}
	#endif
	// 書出の失敗の返戻
	if(OutputStatus || ErrorStatus || std::ferror(stdout) || std::ferror(stderr) || ParserAllocator::IsStopping()) return 1;
	// 処理の終了コードの返戻
	return Code;
}

/**
 * コマンドライン全体の例外を終了状態へ変換する実行関数
 * 計算量：入力収集・整形・入出力の総費用 Q に対し O(Q)
 * @param Argc コマンドライン引数の数
 * @param Argv コマンドライン引数の配列
 * @return 正常時０，処理失敗時１，引数不正時２
 */
int Main::Run(const int Argc, ArgChar *const Argv[]) {
	// 失敗の通知を伴う処理の実行
	try {
		// 確保器と中断信号の登録
		ParserAllocator::Install();
		#if !defined(_WIN32)
		// 読取側の閉じたパイプへの書込を SIGPIPE で終了させず，書出の失敗として扱う（整形仕様 10.4）
		std::signal(SIGPIPE, SIG_IGN);
		#endif
		#if defined(_WIN32)
		// 改行の二重展開と制御文字での読込打切を防ぎ，入出力のバイト列を保持する（起動引数の変換の失敗の通知を含め，標準エラーも LF の儘出し，拡張機能が行を分ける）
		if(
			_setmode(_fileno(stdin), _O_BINARY) < 0 || _setmode(_fileno(stdout), _O_BINARY) < 0 || _setmode(_fileno(stderr), _O_BINARY) < 0
		) {
			std::fputs("Error: failed to configure binary standard streams\n", stderr);
			// 入出力の完全性を保証出来ない場合の失敗の返戻
			return FinishStreams(1);
		}
		std::vector<std::string> Args = WindowsArgs(Argc, Argv);
		std::vector<char *> Ptrs;
		Ptrs.reserve(Args.size() + 1);
		for(std::string &Arg : Args) Ptrs.push_back(Arg.data());
		Ptrs.push_back(nullptr);
		// Unicode の起動引数で実行した終了状態の返戻
		return FinishStreams(Execute(static_cast<int>(Args.size()), Ptrs.data()));
		#else
		// 実行の終了状態の返戻
		return FinishStreams(Execute(Argc, Argv));
		#endif
	} catch(const ProcessingInterrupted &) {
		// 書戻開始前の中断は原本を保持し，既存の終了コードで返す
		return FinishStreams(1);
	} catch(const PathEncodingFailure &E) {
		PrintPathError(E.Path);
	} catch(const Filesystem::filesystem_error &E) {
		PrintPathError(E.path1());
	} catch(const std::system_error &E) {
		#if defined(_WIN32)
		if(E.code().value() == ERROR_NO_UNICODE_TRANSLATION) {
			PrintPathError();
			// 起動引数のパスのエンコーディングを受理出来ない失敗の返戻
			return 1;
		}
		#endif
		PrintException(E);
	} catch(const std::exception &E) {
		PrintException(E);
	}
	// 処理失敗の返戻
	return FinishStreams(1);
}

#if defined(_WIN32)

/**
 * Unicode の起動引数を受け取る起動関数
 * 計算量：実行する課題の総処理量 Q に対し O(Q)
 * @param Argc 引数の数
 * @param Argv Unicode の引数の配列
 * @return 終了コード
 */
int wmain(const int Argc, wchar_t *const Argv[]) {
	// 実行結果の返戻
	return Main::Run(Argc, Argv);
}

#else

/**
 * 起動関数
 * 計算量：実行する課題の総処理量 Q に対し O(Q)
 * @param Argc 引数の数
 * @param Argv 引数の配列
 * @return 終了コード
 */
int main(const int Argc, char *const Argv[]) {
	// 実行結果の返戻
	return Main::Run(Argc, Argv);
}

#endif
