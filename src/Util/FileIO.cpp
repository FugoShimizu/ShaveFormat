#include "Util/FileIO.hpp"
#include "Util/FormatLimitExceeded.hpp"
#include "Util/HiddenDisplay.hpp"
#include "Util/ParserAllocator.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <sys/stat.h>

#if defined(_WIN32)

#include <io.h>
#include <process.h>
#include <windows.h>

#else

#include <unistd.h>

#if defined(__APPLE__)

#include <copyfile.h>
#include <sys/acl.h>
#include <sys/stdio.h>
#include <sys/xattr.h>

#elif defined(__linux__)

#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/xattr.h>

#endif

#endif

// ファイルシステム名前空間の別名
namespace Filesystem = std::filesystem;

/**
 * パスの UTF-8 のテキストの取得関数
 * 計算量：パス長 P に対し O(P)
 * @param Path 処理系のパス
 * @return ANSI 符号ページを経由しない UTF-8 のパス
 * @throws std::bad_alloc テキストの領域を確保出来ない場合
 * @throws std::system_error パスのエンコーディングの変換に失敗した場合
 */
std::string FileIO::GetPathText(const std::filesystem::path &Path) {
	// 対象テキストの取得
	const std::u8string Text = Path.u8string();
	std::u8string_view View(Text);
	#if defined(_WIN32)
	// 処理系専用の接頭辞を，表示・収集順に使う通常の絶対名へ統一する
	if(Path.native().starts_with(ExtendedUncPrefix)) {
		const std::u8string Unc = u8"\\\\" + std::u8string(View.substr(ExtendedUncPrefix.size()));
		// 通常の UNC 名の返戻
		return std::string(Unc.begin(), Unc.end());
	}
	if(Path.native().starts_with(ExtendedPathPrefix)) View.remove_prefix(ExtendedPathPrefix.size());
	#endif
	// コードユニット毎の整数変換で UTF-8 のバイト列を保持する
	// UTF-8 のパスの返戻
	return std::string(View.begin(), View.end());
}

#if defined(_WIN32)

#if defined(ShaveFormatFileIoTestHooks)

/**
 * 識別 API の非対応を再現するフックの設定関数
 * 計算量：O(1)
 * @param IsUnsupported 非対応を再現するか
 */
void FileIO::SetUnsupportedIdentity(const bool IsUnsupported) {
	HasUnsupportedIdentity = IsUnsupported;
	// 終了
	return;
}

#endif

/**
 * UTF-16 のコードユニットから安全な表示名を作る関数
 * 計算量：パス長 P に対し O(P)
 * @param Path 表示だけに使う処理系のパス
 * @return 有効な文字は UTF-8，変換不能なコードユニットはエスケープした表示名
 * @throws std::bad_alloc 表示名の領域を確保出来ない場合
 * @throws std::length_error 表示名の上限を超える場合
 */
std::string FileIO::GetPathDisplay(const Filesystem::path &Path) {
	std::wstring_view Text(Path.native());
	std::string Display;
	// 有効な名前の表示と同じ綴りへ統一してから，変換不能なコードユニットを表示する
	if(Text.starts_with(ExtendedUncPrefix)) {
		Display = "\\\\";
		Text.remove_prefix(ExtendedUncPrefix.size());
	} else if(Text.starts_with(ExtendedPathPrefix)) Text.remove_prefix(ExtendedPathPrefix.size());
	for(size_t Pos = 0; Pos < Text.size();) {
		const bool HasPair =
		Text[Pos] > 0XD7FF && Text[Pos] < 0XDC00 && Pos + 1 < Text.size() && Text[Pos + 1] > 0XDBFF && Text[Pos + 1] < 0XE000;
		const int Units = HasPair ? 2 : 1;
		std::array<char, 8> Buffer {};
		if(
			const int Bytes =
			WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Text.data() + Pos, Units, Buffer.data(), 4, nullptr, nullptr)
		) Display.append(Buffer.data(), static_cast<size_t>(Bytes));
		else {
			std::snprintf(Buffer.data(), Buffer.size(), "\\u%04X", static_cast<unsigned int>(Text[Pos]));
			Display.append(Buffer.data());
		}
		Pos += static_cast<size_t>(Units);
	}
	// 表示だけに制御文字の無害化を適用した返戻
	return ForDisplay(Display);
}

/**
 * 領域番号と１２８ビットの実体番号の取得関数
 * 計算量：O(1)
 * @param Handle 識別対象のハンドル
 * @param Identity 取得した識別子の格納先
 * @param Error 取得失敗の格納先
 * @return 識別子を取得出来れば true
 */
bool FileIO::IsIdentityReadSuccessful(const NativeFile Handle, FileIdentity &Identity, std::error_code *const Error) {
	#if defined(ShaveFormatFileIoTestHooks)
	if(HasUnsupportedIdentity) {
		if(Error) *Error = std::error_code(ERROR_NOT_SUPPORTED, std::system_category());
		// 非対応の API の再現の返戻
		return false;
	}
	#endif
	// ファイルシステムが保証する全桁の識別子の取得
	FILE_ID_INFO Info {};
	if(!GetFileInformationByHandleEx(Handle, FileIdInfo, &Info, sizeof(Info))) {
		RecordNativeError(Error);
		// 識別子を取得出来ない場合の返戻
		return false;
	}
	Identity[0] = Info.VolumeSerialNumber;
	std::memcpy(&Identity[1], Info.FileId.Identifier, sizeof(uint64_t));
	std::memcpy(&Identity[2], Info.FileId.Identifier + sizeof(uint64_t), sizeof(uint64_t));
	// 全桁の識別子を取得した事の返戻
	return true;
}

/**
 * 読取専用で非対応の識別子を未知として扱う関数
 * 計算量：O(1)
 * @param Handle 識別対象のハンドル
 * @param Identity 取得した識別子（非対応では０）
 * @param Error 取得失敗の格納先
 * @return 識別子を取得出来たか，API が非対応なら true
 */
bool FileIO::IsReadIdentitySuccessful(const NativeFile Handle, FileIdentity &Identity, std::error_code *const Error) {
	std::error_code Failure;
	// 全桁の識別子を取得した読取の成功の返戻
	if(IsIdentityReadSuccessful(Handle, Identity, &Failure)) return true;
	if(
		Failure.value() != ERROR_INVALID_FUNCTION && Failure.value() != ERROR_INVALID_PARAMETER &&
		Failure.value() != ERROR_NOT_SUPPORTED
	) {
		if(Error) *Error = Failure;
		// 非対応以外の失敗の返戻
		return false;
	}
	Identity = {};
	// 実体共有の根拠にしない未知の識別子の返戻
	return true;
}

/**
 * 既存のエラーを保ち，ハンドル解放の失敗を記録する関数
 * 計算量：O(1)
 * @param Handle 解放対象のハンドル
 */
void FileIO::CloseFile(void *const Handle) {
	// 解放処理が呼出元の失敗理由を上書きしない様に両エラー状態を退避する
	const int SavedErrno = errno;
	const DWORD SavedError = GetLastError();
	// ハンドル解放失敗の警告
	if(!CloseHandle(Handle)) std::fprintf(stderr, "[notice] failed to close file handle (system error %lu)\n", GetLastError());
	errno = SavedErrno;
	// SavedError の SetLastError 設定
	SetLastError(SavedError);
	// 終了
	return;
}

#endif

/**
 * 既存のエラーを保ち，記述子解放の失敗を記録する関数
 * 計算量：O(1)
 * @param Descriptor 解放対象の記述子
 */
void FileIO::CloseFile(const int Descriptor) {
	// 呼出前のエラー状態の保存
	const int SavedError = errno;
	// 中断時も再試行せず，再利用された記述子の二重解放を防ぐ
	#if defined(_WIN32)
	const int Status = ::_close(Descriptor);
	#else
	const int Status = ::close(Descriptor);
	#endif
	if(Status) std::fprintf(stderr, "[notice] failed to close file descriptor (errno %d)\n", errno);
	errno = SavedError;
	// 終了
	return;
}

// 領域確保を伴わない終了処理の所有者
// 呼出可能な型は捕捉を持つ無名関数の型から推論される
template<class Action> struct ScopeExit {
	const Action &Cleanup; // 終了時に呼ぶ処理

	/**
	 * デストラクタ（所有するリソースの後始末）
	 * 計算量：後始末の費用 Q に対し O(Q)
	 */
	~ScopeExit() {
		// 終了時の後始末の実行
		Cleanup();
		// 終了
		return;
	}
};

// 読込・照合用の実体記述子の例外時も含む所有者
struct FileIO::NativeFileGuard {
	#if defined(_WIN32)

	void *Handle; // Windows の実体

	#else

	int Handle; // POSIX の実体

	#endif

	bool HasClosed = false; // 明示的に解放済か
	void Close(); // 所有する実体記述子の解放関数

	~NativeFileGuard(); // 例外時を含めて記述子を解放するデストラクタ
};

/**
 * 所有する実体記述子の解放関数
 * 計算量：O(1)
 */
void FileIO::NativeFileGuard::Close() {
	// 所有する実体記述子の解放
	#if defined(_WIN32)
	const bool IsValid = Handle != INVALID_HANDLE_VALUE;
	#else
	const bool IsValid = Handle > -1;
	#endif
	if(!HasClosed && IsValid) CloseFile(Handle);
	HasClosed = true;
	// 終了
	return;
}

/**
 * デストラクタ（例外時を含めて記述子を解放する）
 * 計算量：O(1)
 */
FileIO::NativeFileGuard::~NativeFileGuard() {
	// 所有する記述子の解放
	Close();
	// 終了
	return;
}

/**
 * 表示でテキストを持たないコードポイントの判定関数
 * 幅を持たないコードポイントは端末で名前を別の名前に見せる為，表示から落とす
 * 計算量：O(1)
 * @param Point 判定するコードポイント
 * @return 表示から落とすかの真偽
 */
bool FileIO::IsHiddenPoint(const char32_t Point) {
	using DisplaySafety::HiddenRange;
	// 範囲に当たった事の返戻
	for(const HiddenRange &Range : DisplaySafety::Hidden) if(Point > Range.Below && Point < Range.Above) return true;
	// 何れの範囲にも当たらない事の返戻
	return false;
}

/**
 * 前の字の見た目だけを選ぶコードポイントの判定関数
 * 異体字セレクタは字を隠さず並べ替えもしない為，ソースの本文を見せる時は原文の儘残す
 * 計算量：O(1)
 * @param Point 判定するコードポイント
 * @return 異体字セレクタなら true
 */
bool FileIO::IsVariationSelector(const char32_t Point) {
	// 基本多言語面の１６個と，漢字の字体差に使う補助の２４０個かの返戻
	return Point > 0XFDFF && Point < 0XFE10 || Point > 0XE00FF && Point < 0XE01F0;
}

/**
 * 端末制御列の無害化関数（パス名や表示する本文に紛れ込んだ制御文字を落とす）
 * 利用者入力の制御列と，テキストを持たず名前を化けさせるコードポイントを無害化する
 * ソースの本文では水平タブと異体字セレクタを残す（名前の取違を防ぐ必要が無く，落とすと絵文字や漢字の字体が化けて見える）
 * 計算量：テキスト長 L に対し O(L)
 * @param Text 表示対象の文字列
 * @param IsBody ソースの本文の表示か（`--diff` の行，パス名・警告の文面では偽）
 * @return 制御文字 (C0 / DEL) と `IsHiddenPoint` が落とすコードポイントと不正な UTF-8 のバイトを `?` へ置き換えた文字列
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::string FileIO::ForDisplay(const std::string_view Text, const bool IsBody) {
	// 端末で安全に表示するテキストの構築準備
	std::string Safe;
	Safe.reserve(Text.size());
	// ASCII と多バイト列の分離と無効列の１バイト読飛
	// 表示用文字列のバイト走査
	for(size_t Pos = 0; Pos < Text.size();) {
		// 符号の先頭バイト
		const unsigned char Lead = static_cast<unsigned char>(Text[Pos]);
		// ASCII バイトの無害化
		if(Lead < 0X80) {
			Safe += Lead < 0X20 && !(IsBody && Lead == '\t') || Lead == 0X7F ? '?' : static_cast<char>(Lead);
			++Pos;
			continue;
		}
		// 整形式の UTF-8 の並び（Unicode 表 3-7：冗長なエンコーディング・サロゲート・範囲外は第２バイトの範囲で除く）だけの複写
		const size_t Len = Lead > 0XC1 && Lead < 0XE0 ? 2 : Lead > 0XDF && Lead < 0XF0 ? 3 : Lead > 0XEF && Lead < 0XF5 ? 4 : 0;
		bool IsValidRead = Len && Pos + Len <= Text.size();
		char32_t Point = Lead & (Len == 2 ? 0X1F : Len == 3 ? 0XF : 0X7);
		for(size_t Idx = 1; IsValidRead && Idx < Len; ++Idx) {
			const unsigned char Follow = static_cast<unsigned char>(Text[Pos + Idx]);
			const unsigned char Low = Idx > 1 ? 0X7F : Lead == 0XE0 ? 0X9F : Lead == 0XF0 ? 0X8F : 0X7F;
			const unsigned char High = Idx > 1 ? 0XC0 : Lead == 0XED ? 0XA0 : Lead == 0XF4 ? 0X90 : 0XC0;
			IsValidRead = Follow > Low && Follow < High;
			Point = Point << 6 | Follow & 0X3F;
		}
		// テキストを持たないコードポイントの `?` への置換（ソースの本文では異体字セレクタだけ原文の儘残す）
		if(!IsValidRead || IsHiddenPoint(Point) && !(IsBody && IsVariationSelector(Point))) {
			Safe += '?';
			Pos += IsValidRead ? Len : 1;
			continue;
		}
		Safe.append(Text.data() + Pos, Len);
		// 妥当なコードポイントだけ元の UTF-8 バイト列を保つ
		Pos += Len;
	}
	// 無害化した文字列の返戻
	return Safe;
}

#if defined(_WIN32)

/**
 * 処理系の失敗理由の UTF-8 の取得関数
 * 計算量：理由の長さ L に対し O(L)
 * @param Error 処理系の失敗理由
 * @return Unicode の理由（取得出来ない場合は安全に表示した処理系のテキスト）
 * @throws std::bad_alloc 理由の領域を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::string FileIO::GetNativeErrorText(const std::error_code &Error) {
	wchar_t Text[2048];
	DWORD Len = Error.category() == std::system_category() ?
	FormatMessageW(
		FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr,
		static_cast<DWORD>(Error.value()),
		0,
		Text,
		static_cast<DWORD>(std::size(Text)),
		nullptr
	) :
	0;
	// 表示用変換前の末尾の改行・空白の除去
	while(Len && std::iswspace(Text[Len - 1])) --Len;
	const int Bytes = Len ? WideCharToMultiByte(CP_UTF8, 0, Text, static_cast<int>(Len), nullptr, 0, nullptr, nullptr) : 0;
	if(!Bytes) {
		// 処理系の代替テキストも，末尾の空白を除いてから表示用へ渡す
		const std::string Message = Error.message();
		const size_t End = Message.find_last_not_of(" \t\r\n\v\f");
		// Unicode の理由を取得出来ない場合の返戻
		return ForDisplay(End == std::string::npos ? std::string_view() : std::string_view(Message).substr(0, End + 1));
	}
	std::string Result(static_cast<size_t>(Bytes), '\0');
	WideCharToMultiByte(CP_UTF8, 0, Text, static_cast<int>(Len), Result.data(), Bytes, nullptr, nullptr);
	// 安全な UTF-8 の理由の返戻
	return ForDisplay(Result);
}

/**
 * 名前を転送する再解析点かの判定関数
 * 計算量：O(1)
 * @param Handle リンクを辿らずに開いた実体
 * @param Error 属性取得失敗の格納先（失敗も拒否する）
 * @return シンボリックリンク又は junction の様に名前を転送するタグか，不明なら true
 */
bool FileIO::IsNameSurrogate(void *const Handle, std::error_code *const Error) {
	// 名前を転送する再解析点かの判定
	FILE_ATTRIBUTE_TAG_INFO Info {};
	if(!GetFileInformationByHandleEx(Handle, FileAttributeTagInfo, &Info, sizeof(Info))) {
		if(Error) *Error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
		// タグを確定出来ない実体の拒否の返戻
		return true;
	}
	// 名前を転送する再解析点かの返戻
	return Info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT && IsReparseTagNameSurrogate(Info.ReparseTag);
}

#endif

#if defined(ShaveFormatFileIoTestHooks)

/**
 * 保存競合再現用フック設定関数
 * 計算量：O(1)
 * @param Hook 次の名前変更の直前に１回呼ぶ関数
 */
void FileIO::SetTestHook(void (*const Hook)()) {
	// 次の公開又は復旧の直前へ１回だけ競合を注入する関数を保持する
	TestHook = Hook;
	// 終了
	return;
}

/**
 * 公開後競合再現用フック設定関数
 * 計算量：O(1)
 * @param Hook 次の名前変更の直後に１回呼ぶ関数
 */
void FileIO::SetAfterTestHook(void (*const Hook)()) {
	// 次の公開又は復旧の直後へ１回だけ競合を注入する関数を保持する
	AfterTestHook = Hook;
	// 終了
	return;
}

#endif

/**
 * UTF-8 のパスのテキストの処理系のパスへの変換関数
 * 計算量：テキスト長 P に対し O(P)
 * @param Text UTF-8 のパスのテキスト
 * @return ANSI 符号ページを経由しない処理系のパス
 * @throws std::bad_alloc パスの領域を確保出来ない場合
 * @throws std::system_error パスのエンコーディングの変換に失敗した場合
 */
Filesystem::path FileIO::PathOf(const std::string_view Text) {
	// UTF-8 のバイト列を文字８型のコードユニットとして渡し，処理系の符号ページへの変換を避ける
	// UTF-8 として読んだパスの返戻
	return Filesystem::path(std::u8string_view(reinterpret_cast<const char8_t *>(Text.data()), Text.size()));
}

/**
 * 失敗した入出力の処理系の原因の記録関数
 * 計算量：O(1)
 * @param Error 原因を必要とする場合の格納先
 */
void FileIO::RecordNativeError(std::error_code *const Error) {
	// 失敗した入出力の処理系の原因の記録
	if(Error) {
		#if defined(_WIN32)
		*Error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
		#else
		*Error = std::error_code(errno, std::generic_category());
		#endif
	}
	// 終了
	return;
}

/**
 * 元のパスの成分を畳まない絶対パスの取得関数
 * 相対パスの基準だけを解決し，リンクの後の `..` を全成分の非追跡の検査より先に隠さない（整形仕様 10.2）
 * 計算量：パス長 P に対し O(P)，追加領域 O(P)
 * @param Text 元のパスの UTF-8 のテキスト
 * @param Error 現在地又はパスの根を取得出来ない場合の原因
 * @return 元のパスの成分を保持する絶対パス（取得出来ない場合は空）
 * @throws std::bad_alloc パスの領域を確保出来ない場合
 * @throws std::filesystem::filesystem_error パスのエンコーディングを変換出来ない場合
 * @throws std::length_error パスの格納先の上限を超える場合
 * @throws std::system_error パスのエンコーディングを変換出来ない場合
 */
Filesystem::path FileIO::GetAbsolutePath(const std::string_view Text, std::error_code &Error) {
	Error.clear();
	#if defined(_WIN32)
	const Filesystem::path Path = PathOf(GetPathText(PathOf(Text)));
	#else
	const Filesystem::path Path = PathOf(Text);
	#endif
	// 元から絶対パスの場合の返戻
	if(Path.is_absolute()) return Path;
	// 現在地，指定したドライブの現在地，又はドライブ内の根だけの解決
	const Filesystem::path Root =
	Path.has_root_path() ? Filesystem::absolute(Path.root_path(), Error) : Filesystem::current_path(Error);
	// 基準の取得に失敗した場合の返戻
	if(Error) return {};
	// 元のパスの各成分を畳まず付けた絶対パスの返戻
	return Root / Path.relative_path();
}

/**
 * 成分検査済の絶対パスを長いパス対応の API へ渡す関数
 * 元の成分列は呼出側に残し，リンクを検査した前置範囲の `.`・`..` だけを API 用に畳む
 * 計算量：パス長 P に対し O(P)
 * @param Path 根から順に成分を検査した絶対パス（既存の拡張パスも可）
 * @param ShouldExtend 短い走査根でも拡張パスを使うなら true
 * @return 長い Windows パスは拡張パス，其の他は元のパス
 * @throws std::bad_alloc API 用パスの領域を確保出来ない場合
 * @throws std::filesystem::filesystem_error パスのエンコーディングを変換出来ない場合
 */
Filesystem::path FileIO::GetNativePath(const Filesystem::path &Path, [[maybe_unused]] const bool ShouldExtend) {
	#if defined(_WIN32)
	// 強制しない短いパスと既存の拡張パスの其の儘の返戻
	if(!ShouldExtend && Path.native().size() < MAX_PATH || Path.native().starts_with(ExtendedPathPrefix)) return Path;
	// 拡張パスでは処理系が展開しない区切・相対成分の解決
	const std::wstring Text = Path.lexically_normal().make_preferred().native();
	// UNC の共有名を保つ拡張パスの返戻
	if(Text.starts_with(L"\\\\")) return Filesystem::path(std::wstring(ExtendedUncPrefix) + Text.substr(2));
	// ドライブの絶対パスの返戻
	return Filesystem::path(std::wstring(ExtendedPathPrefix) + Text);
	#else
	// 処理系専用の変換が不要なパスの返戻
	return Path;
	#endif
}

/**
 * 原本の同一性と更新情報の取得関数
 * 計算量：O(1)
 * @param Handle 読込対象の記述子
 * @param Metadata 比較情報の格納先
 * @param FileState POSIX の付帯情報も必要な場合の格納先
 * @param ReadFailure 原因を必要とする場合の格納先
 * @param ShouldVerifyIdentity 非対応の識別 API を書戻の照合で拒否するか
 * @return 通常ファイルの情報を取得出来れば true
 */
bool FileIO::IsMetadataReadSuccessful(
	const NativeFile Handle,
	std::array<uint64_t, 10> &Metadata,
	[[maybe_unused]] struct stat *const FileState,
	std::error_code *const ReadFailure,
	[[maybe_unused]] const bool ShouldVerifyIdentity
) {
	// 実行環境毎の原本情報の取得
	#if defined(_WIN32)
	BY_HANDLE_FILE_INFORMATION Info {};
	FILE_BASIC_INFO Basic {};
	FileIdentity Identity {};
	// 書戻の全桁照合と，読取専用の非対応の区別
	const bool HasIdentity = ShouldVerifyIdentity ?
	IsIdentityReadSuccessful(Handle, Identity, ReadFailure) :
	IsReadIdentitySuccessful(Handle, Identity, ReadFailure);
	// 必要な識別を取得出来ない場合の返戻
	if(!HasIdentity) return false;
	if(!GetFileInformationByHandle(Handle, &Info) || !GetFileInformationByHandleEx(Handle, FileBasicInfo, &Basic, sizeof(Basic))) {
		RecordNativeError(ReadFailure);
		// 情報取得失敗の返戻
		return false;
	}
	if(
		Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ||
		Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT && IsNameSurrogate(Handle, ReadFailure)
	) {
		if(ReadFailure && !*ReadFailure) *ReadFailure = std::make_error_code(std::errc::operation_not_supported);
		// 通常ファイル以外の失敗の返戻
		return false;
	}
	// 差替検出用の実体識別子と全更新時刻の退避
	// ファイル状態の領域・実体・寸法・各日時・属性・別名数
	Metadata = {
		Identity[0],
		Identity[1],
		static_cast<uint64_t>(Info.nFileSizeHigh) << 32 | Info.nFileSizeLow,
		static_cast<uint64_t>(Basic.LastWriteTime.QuadPart),
		static_cast<uint64_t>(Basic.ChangeTime.QuadPart),
		static_cast<uint64_t>(Basic.CreationTime.QuadPart),
		Info.dwFileAttributes,
		Info.nNumberOfLinks,
		0,
		Identity[2]
	};
	#else
	struct stat Info {};
	if(::fstat(Handle, &Info)) {
		RecordNativeError(ReadFailure);
		// 情報取得失敗の返戻
		return false;
	}
	if(!S_ISREG(Info.st_mode) || Info.st_size < 0) {
		if(ReadFailure) *ReadFailure = std::make_error_code(std::errc::operation_not_supported);
		// 通常ファイル以外の失敗の返戻
		return false;
	}
	#if defined(__APPLE__)
	const timespec Modified = Info.st_mtimespec, Changed = Info.st_ctimespec;
	#else
	const timespec Modified = Info.st_mtim, Changed = Info.st_ctim;
	#endif
	uint32_t Flags = 0;
	#if defined(__APPLE__)
	Flags = Info.st_flags;
	#elif defined(__linux__)
	int InodeFlags = 0;
	if(::ioctl(Handle, FS_IOC_GETFLAGS, &InodeFlags) && errno != ENOTTY && errno != EOPNOTSUPP) {
		RecordNativeError(ReadFailure);
		// フラグを持たないファイルシステム以外の取得失敗の返戻
		return false;
	}
	Flags = static_cast<uint32_t>(InodeFlags);
	#endif
	if(FileState) *FileState = Info;
	// 秒とナノ秒の個別保持に依る更新見逃しの防止
	// ファイル状態の領域・実体・寸法・各日時・モード
	Metadata = {
		static_cast<uint64_t>(Info.st_dev),
		static_cast<uint64_t>(Info.st_ino),
		static_cast<uint64_t>(Info.st_size),
		static_cast<uint64_t>(Modified.tv_sec),
		static_cast<uint64_t>(Modified.tv_nsec),
		static_cast<uint64_t>(Changed.tv_sec),
		static_cast<uint64_t>(Changed.tv_nsec),
		static_cast<uint64_t>(Info.st_mode),
		Flags
	};
	#endif
	// 取得成功の返戻
	return true;
}

/**
 * 親ディレクトリを固定するコンストラクタ
 * パス成分のシンボリックリンクを辿らず差替競合を拒む
 * 計算量：成分数 C とパス長 P に対し O(CP)，追加領域 O(C + P)
 * @param Path 対象パス
 * @param Expected 収集時の親成分と対象の実体（収集の呼出では空）
 * @param Accepted 原文の保持前に求めた受理計数と原文の状態
 * @throws std::bad_alloc パス又は実体の識別子の領域を確保出来ない場合
 * @throws std::filesystem::filesystem_error パスのエンコーディングを変換出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::system_error パスのエンコーディングの変換に失敗した場合
 */
FileIO::FileIO(
	const std::string &Path,
	const std::span<const FileIdentity> Expected,
	const std::optional<InputInspection> Accepted
) : InspectionMetadata(Accepted ? std::optional<std::array<uint64_t, 10>>(Accepted->Metadata) : std::nullopt) {
	// 未移管の記述子と一時名の後始末
	const auto Cleanup = [&]() -> void {
		// 対象の状態の更新
		if(std::uncaught_exceptions()) {
			#if defined(_WIN32)
			for(void *const Handle : DirHandles) CloseFile(Handle);
			#else
			if(ParentDescriptor > -1) CloseFile(ParentDescriptor);
			#endif
		}
		// 終了
		return;
	};
	const ScopeExit<decltype(Cleanup)> CleanupGuard { Cleanup };
	// 対象パスの絶対名への固定
	std::error_code Error;
	const Filesystem::path Target = GetAbsolutePath(Path, Error);
	if(Error) {
		ReadError = Error;
		// パス解決失敗時の終了
		return;
	}
	// 後続の成分検査と差替で同じ絶対名を使う
	TargetPath = GetPathText(Target);
	#if defined(_WIN32)
	// Windows の親パスハンドル列の構築
	Filesystem::path Parent = Target.root_path();
	const Filesystem::path Relative = Target.parent_path().relative_path();
	Filesystem::path::iterator Part = Relative.begin();
	// UNC のサーバー名だけを開かず，共有名迄を最初の親に含める
	if(Target.root_name().native().starts_with(L"\\\\")) {
		const Filesystem::path Components = Target.relative_path();
		if(Components.empty()) {
			ReadError = std::make_error_code(std::errc::invalid_argument);
			// 共有名の無い UNC の終了
			return;
		}
		Parent /= *Components.begin();
		if(Part != Relative.end()) ++Part;
	}
	// 根から親パスの各成分迄の走査
	while(true) {
		if(!ParserAllocator::IsWriting()) ParserAllocator::CheckInterruption();
		const HANDLE Handle = CreateFileW(
			GetNativePath(Parent).c_str(),
			FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
			nullptr
		);
		if(Handle == INVALID_HANDLE_VALUE) {
			ReadError = std::error_code(static_cast<int>(GetLastError()), std::system_category());
			// 親パスを開けない場合の終了
			return;
		}
		// 固定した親ディレクトリハンドルの保持
		NativeFileGuard HandleGuard { Handle };
		DirHandles.push_back(Handle);
		HandleGuard.HasClosed = true;
		BY_HANDLE_FILE_INFORMATION Info {};
		if(!GetFileInformationByHandle(Handle, &Info)) {
			ReadError = std::error_code(static_cast<int>(GetLastError()), std::system_category());
			// 親の実体を取得出来ない場合の終了
			return;
		}
		if(Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT && IsNameSurrogate(Handle, &ReadError)) {
			if(!ReadError) ReadError = std::make_error_code(std::errc::too_many_symbolic_link_levels);
			// 名前を転送する又はタグ不明の親パスでの終了
			return;
		}
		FileIdentity Identity {};
		// 親の全桁の識別子を取得出来ない場合の終了
		if(!IsReadIdentitySuccessful(Handle, Identity, &ReadError)) return;
		ParentIdentities.push_back(Identity);
		if(Part == Relative.end()) break;
		Parent /= *Part;
		++Part;
	}
	#else
	// POSIX の親ディレクトリ記述子の構築
	#if defined(__APPLE__)
	constexpr int DirAccess = O_SEARCH;
	#elif defined(__linux__)
	constexpr int DirAccess = O_PATH;
	#else
	constexpr int DirAccess = O_RDONLY;
	#endif
	ParentDescriptor = ::open(Target.root_path().c_str(), DirAccess | O_DIRECTORY | O_CLOEXEC);
	if(ParentDescriptor < 0) {
		ReadError = std::error_code(errno, std::generic_category());
		// 根を開けない場合の終了
		return;
	}
	struct stat RootInfo {};
	if(::fstat(ParentDescriptor, &RootInfo)) {
		ReadError = std::error_code(errno, std::generic_category());
		// 根の実体を確認出来ない場合の終了
		return;
	}
	ParentIdentities.push_back({ static_cast<uint64_t>(RootInfo.st_dev), static_cast<uint64_t>(RootInfo.st_ino), 0 });
	// 親迄の各成分を記述子から相対的に開き，名前解決の差替を防ぐ
	for(const Filesystem::path &Part : Target.parent_path().relative_path()) {
		if(!ParserAllocator::IsWriting()) ParserAllocator::CheckInterruption();
		// 成分毎にリンクを拒否し，確認と次の成分への移動を同じ記述子上で行う
		const int Next = ::openat(ParentDescriptor, Part.c_str(), DirAccess | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if(Next < 0) {
			ReadError = std::error_code(errno, std::generic_category());
			// 親成分を開けない場合の終了
			return;
		}
		NativeFileGuard NextGuard { Next };
		struct stat Info {};
		if(::fstat(Next, &Info)) {
			ReadError = std::error_code(errno, std::generic_category());
			// 親成分の実体を確認出来ない場合の終了
			return;
		}
		ParentIdentities.push_back({ static_cast<uint64_t>(Info.st_dev), static_cast<uint64_t>(Info.st_ino), 0 });
		CloseFile(ParentDescriptor);
		ParentDescriptor = Next;
		NextGuard.HasClosed = true;
	}
	#endif
	if(!Expected.empty()) {
		if(!std::ranges::equal(ParentIdentities, Expected.first(Expected.size() - 1))) {
			ReadError = std::make_error_code(std::errc::state_not_recoverable);
			// 収集後の親成分の差替での終了
			return;
		}
		ExpectedIdentity = Expected.back();
	}
	IsReady = true;
	// 終了
	return;
}

/**
 * 固定したパスの実体識別子の取得関数
 * 計算量：パス長 P に対し O(P)
 * @return 実体の領域と番号（取得出来ない場合は空）
 * @throws std::bad_alloc パスの領域を確保出来ない場合
 * @throws std::filesystem::filesystem_error パスのエンコーディングを変換出来ない場合
 */
std::optional<FileIdentity> FileIO::GetIdentity() const {
	// 固定出来ないパスの返戻
	if(!IsReady) return std::nullopt;
	#if defined(_WIN32)
	const HANDLE Handle = CreateFileW(
		GetNativePath(PathOf(TargetPath)).c_str(),
		FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr
	);
	// 開けない実体の返戻
	if(Handle == INVALID_HANDLE_VALUE) {
		RecordNativeError(&ReadError);
		// 実体を開けない事の返戻
		return std::nullopt;
	}
	BY_HANDLE_FILE_INFORMATION Info {};
	FileIdentity Identity {};
	const bool HasInfo = GetFileInformationByHandle(Handle, &Info) &&
	(!(Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !IsNameSurrogate(Handle)) &&
	IsReadIdentitySuccessful(Handle, Identity, &ReadError);
	CloseFile(Handle);
	// 不明な実体の返戻
	if(!HasInfo) return std::nullopt;
	// Windows の領域とファイル番号の返戻
	return Identity;
	#else
	struct stat Info {};
	if(
		(
			PathOf(TargetPath).has_filename() ?
			::fstatat(ParentDescriptor, PathOf(TargetPath).filename().c_str(), &Info, AT_SYMLINK_NOFOLLOW) :
			::fstat(ParentDescriptor, &Info)
		) || !S_ISREG(Info.st_mode) && !S_ISDIR(Info.st_mode)
	// 不明な実体の返戻
	) return std::nullopt;
	// POSIX の領域と inode の返戻
	return FileIdentity{ static_cast<uint64_t>(Info.st_dev), static_cast<uint64_t>(Info.st_ino) };
	#endif
}

/**
 * 実際の名前解決に従う絶対パスの取得関数
 * 大小文字・字の同一視は実体記述子のパスへ反映された結果を用い，別名の実体番号だけでは纏めない（整形仕様 9.4, 10.2）
 * 元のパスの各成分は構築時にリンクを辿らず固定し，終端もリンクを辿らず開く
 * 計算量：パス長 P に対し O(P)，追加領域 O(P)
 * @return ファイルシステムが返す名前の UTF-8 の絶対パス（取得出来ない場合は空）
 * @throws std::bad_alloc パスの領域を確保出来ない場合
 * @throws std::filesystem::filesystem_error パスのエンコーディングを変換出来ない場合
 * @throws std::length_error パスの格納先の上限を超える場合
 * @throws std::system_error パスのエンコーディングを変換出来ない場合
 */
std::optional<std::string> FileIO::GetNormalizedPath() const {
	// 固定出来ないパスの返戻
	if(!IsReady) return std::nullopt;
	#if defined(_WIN32)
	const HANDLE Handle = CreateFileW(
		GetNativePath(PathOf(TargetPath)).c_str(),
		FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr
	);
	if(Handle == INVALID_HANDLE_VALUE) {
		RecordNativeError(&ReadError);
		// 終端を開けない場合の返戻
		return std::nullopt;
	}
	const NativeFileGuard Guard { Handle };
	BY_HANDLE_FILE_INFORMATION Info {};
	if(!GetFileInformationByHandle(Handle, &Info)) {
		RecordNativeError(&ReadError);
		// 実体を識別出来ない場合の返戻
		return std::nullopt;
	}
	if(Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT && IsNameSurrogate(Handle, &ReadError)) {
		if(!ReadError) ReadError = std::make_error_code(std::errc::too_many_symbolic_link_levels);
		// 名前を転送する終端の返戻
		return std::nullopt;
	}
	FileIdentity Identity {};
	// 全桁の識別子を取得出来ない場合の返戻
	if(!IsReadIdentitySuccessful(Handle, Identity, &ReadError)) return std::nullopt;
	#else
	#if defined(__APPLE__)
	constexpr int Access = O_EVTONLY;
	#elif defined(__linux__)
	constexpr int Access = O_PATH;
	#else
	constexpr int Access = O_RDONLY;
	#endif
	const int Handle =
	::openat(ParentDescriptor, PathOf(TargetPath).filename().c_str(), Access | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
	if(Handle < 0) {
		RecordNativeError(&ReadError);
		// 終端を開けない場合の返戻
		return std::nullopt;
	}
	const NativeFileGuard Guard { Handle };
	struct stat Info {};
	if(::fstat(Handle, &Info)) {
		RecordNativeError(&ReadError);
		// 実体を識別出来ない場合の返戻
		return std::nullopt;
	}
	if(!S_ISREG(Info.st_mode)) {
		ReadError = std::make_error_code(std::errc::operation_not_supported);
		// 通常ファイル以外の終端の返戻
		return std::nullopt;
	}
	const FileIdentity Identity { static_cast<uint64_t>(Info.st_dev), static_cast<uint64_t>(Info.st_ino) };
	#endif
	if(ExpectedIdentity && *ExpectedIdentity != Identity) {
		ReadError = std::make_error_code(std::errc::state_not_recoverable);
		// 収集した終端の差替の返戻
		return std::nullopt;
	}
	#if defined(_WIN32)
	// ファイルシステムの正規名の寸法取得と転写
	const DWORD Size = GetFinalPathNameByHandleW(Handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	if(!Size) {
		RecordNativeError(&ReadError);
		// パスの寸法を取得出来ない場合の返戻
		return std::nullopt;
	}
	std::wstring Path(Size, L'\0');
	const DWORD Len = GetFinalPathNameByHandleW(Handle, Path.data(), Size, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	if(!Len || Len > Size - 1) {
		if(!Len) RecordNativeError(&ReadError);
		else ReadError = std::make_error_code(std::errc::filename_too_long);
		// パスを転写出来ない場合の返戻
		return std::nullopt;
	}
	Path.resize(Len);
	// 実際の名前解決が返した絶対パスの返戻
	return GetPathText(Filesystem::path(Path));
	#else
	std::array<char, PATH_MAX> Path {};
	#if defined(__APPLE__)
	if(::fcntl(Handle, F_GETPATH, Path.data())) {
		RecordNativeError(&ReadError);
		// パスを取得出来ない場合の返戻
		return std::nullopt;
	}
	// 実際の名前解決が返した絶対パスの返戻
	return std::string(Path.data());
	#elif defined(__linux__)
	// 記述子が指す名前の取得（別のハードリンクの名前へ置き換えない）
	const ssize_t Len = ::readlink(("/proc/self/fd/" + std::to_string(Handle)).c_str(), Path.data(), Path.size());
	if(Len < 0 || static_cast<size_t>(Len) == Path.size()) {
		if(Len < 0) RecordNativeError(&ReadError);
		else ReadError = std::make_error_code(std::errc::filename_too_long);
		// パスを取得出来ない場合の返戻
		return std::nullopt;
	}
	// 実際の名前解決が返した絶対パスの返戻
	return std::string(Path.data(), static_cast<size_t>(Len));
	#else
	ReadError = std::make_error_code(std::errc::operation_not_supported);
	// 実体からのパス取得を持たない処理系の返戻
	return std::nullopt;
	#endif
	#endif
}

/**
 * 読込中の更新を検査する原本取得・照合関数
 * 計算量：O(B)（B は原本のバイト数）
 * @param Handle 読込対象の記述子
 * @param ShouldCompare 取得済の控えと照合する場合は true
 * @param MaxSize 読込を許す最大寸法
 * @param Bytes 原本の格納先又は照合元
 * @param Metadata 比較情報の格納先又は照合元
 * @param ReadFailure 原因を必要とする場合の格納先
 * @param Expected 収集時の実体識別（状態全体の期待が無い場合だけ照合する）
 * @param ExpectedMetadata 受理計数時の原文の状態（指定時は実体識別より優先する）
 * @return 読込前後で更新が無く，照合時は内容も一致すれば true
 * @throws std::bad_alloc 初回取得で確定寸法の領域を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool FileIO::IsStableFileReadSuccessful(
	const NativeFile Handle,
	const bool ShouldCompare,
	const size_t MaxSize,
	std::string &Bytes,
	std::array<uint64_t, 10> &Metadata,
	std::error_code *const ReadFailure,
	const FileIdentity *const Expected,
	const std::array<uint64_t, 10> *const ExpectedMetadata
) {
	// 読込前後の状態格納域の準備
	std::array<uint64_t, 10> Before {}, After {};
	// 記述子情報を取得出来ない場合の失敗の返戻
	if(!IsMetadataReadSuccessful(Handle, Before, nullptr, ReadFailure)) return false;
	if(ExpectedMetadata ? Before != *ExpectedMetadata : Expected && FileIdentity{ Before[0], Before[1], Before[9] } != *Expected) {
		if(ReadFailure) *ReadFailure = std::make_error_code(std::errc::state_not_recoverable);
		// 収集後の対象の差替の拒否の返戻
		return false;
	}
	if(ShouldCompare ? Before != Metadata : Before[2] > MaxSize) {
		if(ReadFailure) {
			*ReadFailure = std::make_error_code(ShouldCompare ? std::errc::state_not_recoverable : std::errc::file_too_large);
		}
		// 原本変化又は入力上限の超過時の失敗の返戻
		return false;
	}
	const size_t Size = static_cast<size_t>(Before[2]);
	// 初回取得だけ確定寸法へ伸ばし，照合時は控えの領域を変更しない
	if(!ShouldCompare) Bytes.resize(Size);
	#if defined(_WIN32)
	// Windows の読込位置を先頭へ戻せない場合の失敗の返戻（同一記述子で再照合する為）
	if(!SetFilePointerEx(Handle, LARGE_INTEGER{}, nullptr, FILE_BEGIN)) {
		RecordNativeError(ReadFailure);
		// 読込位置を戻せない場合の失敗の返戻
		return false;
	}
	#endif
	// 原本の追加複写せず，固定長の領域で分割照合
	std::array<char, 65536> Buf {};
	for(size_t Done = 0;;) {
		// 読み終えた後も１バイト試し，寸法取得直後の追記の検出
		const size_t Requested = Done < Size ? std::min(Buf.size(), Size - Done) : 1;
		#if defined(_WIN32)
		DWORD Count = 0;
		if(!::ReadFile(Handle, Buf.data(), static_cast<DWORD>(Requested), &Count, nullptr)) {
			RecordNativeError(ReadFailure);
			// Windows の読込失敗の返戻
			return false;
		}
		#else
		const ssize_t Count = ::pread(Handle, Buf.data(), Requested, static_cast<off_t>(Done));
		// 割込に因る POSIX 読込の再試行
		if(Count < 0 && errno == EINTR) continue;
		if(Count < 0) {
			RecordNativeError(ReadFailure);
			// POSIX の読込失敗の返戻
			return false;
		}
		#endif
		// 予定寸法読込後の追記検査
		if(Done == Size) {
			if(Count) {
				if(ReadFailure) *ReadFailure = std::make_error_code(std::errc::state_not_recoverable);
				// 寸法確認後に追記された場合の失敗の返戻
				return false;
			}
			break;
		}
		if(!Count) {
			if(ReadFailure) *ReadFailure = std::make_error_code(std::errc::state_not_recoverable);
			// 予定寸法より前に終端した場合の失敗の返戻
			return false;
		}
		const size_t ReadSize = static_cast<size_t>(Count);
		if(ShouldCompare && !std::equal(Buf.data(), Buf.data() + ReadSize, Bytes.data() + Done)) {
			if(ReadFailure) *ReadFailure = std::make_error_code(std::errc::state_not_recoverable);
			// 原本の内容が変化した場合の失敗の返戻
			return false;
		// 初回読込バイト列への転写
		} else if(!ShouldCompare) std::copy_n(Buf.data(), ReadSize, Bytes.data() + Done);
		// 部分読込の分だけ次の照合位置を進める
		Done += ReadSize;
	}
	// 読込後の情報を取得出来ない場合の失敗の返戻
	if(!IsMetadataReadSuccessful(Handle, After, nullptr, ReadFailure)) return false;
	if(Before != After) {
		if(ReadFailure) *ReadFailure = std::make_error_code(std::errc::state_not_recoverable);
		// 読込中に原本が更新された場合の失敗の返戻
		return false;
	}
	Metadata = Before;
	// 原本取得・照合成功の返戻
	return true;
}

/**
 * 親ディレクトリの固定を解除するデストラクタ
 * 計算量：保持する親の記述子数 D に対し O(D)
 */
FileIO::~FileIO() {
	// 構築途中で保持した親リソースの全解放
	#if defined(_WIN32)
	for(void *const Handle : DirHandles) CloseFile(Handle);
	#else
	if(ParentDescriptor > -1) CloseFile(ParentDescriptor);
	#endif
	// 終了
	return;
}

/**
 * 原文を保持せず解析前の受理計数を求める関数
 * 計算量：パス長 P に対し計数の共有は O(P)，其の他は見積関数の費用 Q に対し O(P + Q)，読込は固定長の領域を使う
 * @param Estimate 元の入口条件で受理見積を求める関数
 * @param Counted 同じ入口条件で確認済の受理計数（無ければ計数する）
 * @return 原文の寸法・見積・状態（入力上限超過は寸法だけ，読込又は収集後の同一性の照合に失敗すれば空）
 * @throws std::bad_alloc 見積関数の領域を確保出来ない場合
 * @throws std::exception 見積関数が読込以外の失敗を送出した場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
std::optional<FileIO::InputInspection> FileIO::InspectInput(
	const InputEstimator &Estimate,
	const InputInspection *const Counted
) const {
	// 親を固定出来ない原文の返戻
	if(!IsReady) return std::nullopt;
	#if defined(_WIN32)
	const NativeFile Handle = CreateFileW(
		GetNativePath(PathOf(TargetPath)).c_str(),
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr
	);
	const bool IsOpen = Handle != INVALID_HANDLE_VALUE;
	#else
	const NativeFile Handle =
	::openat(ParentDescriptor, PathOf(TargetPath).filename().c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
	const bool IsOpen = Handle > -1;
	#endif
	const NativeFileGuard Guard { Handle };
	if(!IsOpen) {
		RecordNativeError(&ReadError);
		// 読込口を取得出来ない事の返戻
		return std::nullopt;
	}
	InputInspection Result;
	// 原文の状態を取得出来ない事の返戻
	if(!IsMetadataReadSuccessful(Handle, Result.Metadata, nullptr, &ReadError)) return std::nullopt;
	if(ExpectedIdentity && FileIdentity{ Result.Metadata[0], Result.Metadata[1], Result.Metadata[9] } != *ExpectedIdentity) {
		ReadError = std::make_error_code(std::errc::state_not_recoverable);
		// 収集後に対象の実体が変わった事の返戻
		return std::nullopt;
	}
	// 別名の実体と更新状態を確かめた後だけの受理計数の共有
	if(Counted && Counted->Metadata == Result.Metadata) return *Counted;
	Result.Bytes = Result.Metadata[2] > MaxInputBytes ? MaxInputBytes + 1 : static_cast<size_t>(Result.Metadata[2]);
	// １件の上限を超す原文を読まずに返す受理データの返戻
	if(Result.Bytes > MaxInputBytes) return Result;
	// 記述子上の原文の読取
	const auto Read = [&](const size_t Offset, const std::span<char> Buf) -> size_t {
		#if defined(_WIN32)
		LARGE_INTEGER Pos {};
		Pos.QuadPart = static_cast<LONGLONG>(Offset);
		DWORD Count = 0;
		const bool IsSuccessful = SetFilePointerEx(Handle, Pos, nullptr, FILE_BEGIN) &&
		::ReadFile(Handle, Buf.data(), static_cast<DWORD>(Buf.size()), &Count, nullptr);
		if(!IsSuccessful) throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
		#else
		ssize_t Count = 0;
		do Count = ::pread(Handle, Buf.data(), Buf.size(), static_cast<off_t>(Offset));
		while(Count < 0 && errno == EINTR);
		if(Count < 0) throw std::system_error(errno, std::generic_category());
		#endif
		// 記述子上で読んだ量の返戻
		return static_cast<size_t>(Count);
	};
	try {
		Result.WorkingBytes = Estimate(Result.Bytes, Read);
	} catch(const std::system_error &E) {
		ReadError = E.code();
		// 原文を読み切れない事の返戻
		return std::nullopt;
	}
	std::array<uint64_t, 10> After {};
	// 受理走査後の状態を取得出来ない事の返戻
	if(!IsMetadataReadSuccessful(Handle, After, nullptr, &ReadError)) return std::nullopt;
	if(After != Result.Metadata) {
		ReadError = std::make_error_code(std::errc::state_not_recoverable);
		// 受理走査中に原文が変わった事の返戻
		return std::nullopt;
	}
	// 原文の受理計数と状態の返戻
	return Result;
}

/**
 * 親成分と対象の収集時の実体の取得関数
 * 計算量：親成分数 D に対し O(D)，追加領域 O(D)
 * @return 親成分と対象の識別子（対象を取得出来なければ空）
 * @throws std::bad_alloc 識別子の領域を確保出来ない場合
 * @throws std::filesystem::filesystem_error パスの変換又は対象の列挙に失敗した場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::vector<FileIdentity> FileIO::GetPathIdentity() const {
	// 保持する原本の実体の取得
	const std::optional<FileIdentity> Identity = GetIdentity();
	// 収集時の対象を開けない事の返戻
	if(!Identity) return {};
	if(*Identity == FileIdentity() || std::ranges::find(ParentIdentities, FileIdentity()) != ParentIdentities.end()) {
		ReadError = std::make_error_code(std::errc::operation_not_supported);
	}
	std::vector<FileIdentity> Result = ParentIdentities;
	Result.push_back(*Identity);
	// 取得済の成分は照合し，未知の番号を実体共有へ使わない成分列の返戻
	return Result;
}

/**
 * 書戻前の別名の数の取得関数
 * 計算量：O(1)
 * @return 固定した親上の別名の数（Windows では読込時の数，取得出来なければ０）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 */
uint64_t FileIO::GetLinkCount() const {
	// 書戻前の別名の数の取得
	#if defined(_WIN32)
	// 読込時の別名数の返戻
	return LastRead ? LastRead->Metadata[7] : 0;
	#else
	struct stat Info {};
	// 固定した親上の別名数の返戻
	return !::fstatat(ParentDescriptor, PathOf(TargetPath).filename().c_str(), &Info, AT_SYMLINK_NOFOLLOW) ?
	static_cast<uint64_t>(Info.st_nlink) :
	0;
	#endif
}

/**
 * 読込失敗の原因の取得関数
 * 計算量：O(1)
 * @return 記録した読込失敗の原因
 */
std::error_code FileIO::GetReadError() const {
	// 読込失敗の原因の返戻
	return ReadError;
}

/**
 * 書戻の照合の読込関数
 * メモリを確保出来ない読込も照合出来なかった物とし，公開名の交換の前後で例外で抜けずに復旧・警告のパスへ進める
 * 計算量：読込上限 B に対し O(B)
 * @param Handle 読込対象の記述子
 * @param MaxSize 読込を許す最大寸法
 * @param State 内容と比較情報の格納先
 * @return 読込前後で更新が無く読めれば true
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool FileIO::IsVerificationReadSuccessful(const NativeFile Handle, const size_t MaxSize, ReadState &State) {
	// 失敗の通知を伴う処理の実行
	try {
		// 読込の成否の返戻
		return IsStableFileReadSuccessful(Handle, false, MaxSize, State.Bytes, State.Metadata);
	} catch(const std::bad_alloc &) {
		// メモリを確保出来ない読込の失敗の返戻
		return false;
	}
}

/**
 * ファイル読込関数
 * 計算量：原文長 B と保持する親パスの成分数 D に対し O(B + D)
 * @param Src 読み込んだ内容の格納先（BOM を含む原本の儘）
 * @param ShouldKeepWriteBaseline 書戻の照合の為に原本を保持するか
 * @return 読込成功時 true
 * @throws std::bad_alloc 読込領域を確保出来ない場合
 */
bool FileIO::IsFileReadSuccessful(std::string &Src, const bool ShouldKeepWriteBaseline) const {
	// 前回読込の基準は新しい取得が完了する迄無効にする
	LastRead.reset();
	// 親パスを固定出来ない読込の失敗の返戻
	if(!IsReady) return false;
	ReadError.clear();
	// 固定した親からの対象ファイルの取得
	#if defined(_WIN32)
	const NativeFile Handle = CreateFileW(
		GetNativePath(PathOf(TargetPath)).c_str(),
		GENERIC_READ,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr
	);
	if(Handle == INVALID_HANDLE_VALUE) {
		ReadError = std::error_code(static_cast<int>(GetLastError()), std::system_category());
		// Windows の開込失敗の返戻
		return false;
	}
	#else
	const NativeFile Handle =
	::openat(ParentDescriptor, PathOf(TargetPath).filename().c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
	if(Handle < 0) {
		ReadError = std::error_code(errno, std::generic_category());
		// POSIX の開込失敗の返戻
		return false;
	}
	#endif
	// 更新を検出する安定読込と記述子の解放
	ReadState State;
	bool IsReadOk = false;
	try {
		IsReadOk = IsStableFileReadSuccessful(
			Handle,
			false,
			MaxInputBytes,
			State.Bytes,
			State.Metadata,
			&ReadError,
			ExpectedIdentity ? &*ExpectedIdentity : nullptr,
			InspectionMetadata ? &*InspectionMetadata : nullptr
		);
	} catch(...) {
		CloseFile(Handle);
		throw;
	}
	CloseFile(Handle);
	// 読込中の変更又は取得を検証出来ない場合の返戻
	if(!IsReadOk) return false;
	// 書戻時だけ原本を控え，其の他の入口は読込領域を渡す
	if(ShouldKeepWriteBaseline) {
		LastRead = std::move(State);
		Src = LastRead->Bytes;
	} else Src = std::move(State.Bytes);
	// 読込成功の返戻
	return true;
}

/**
 * 処理番号取得関数（一時ファイル名の一意化に用いる）
 * 計算量：O(1)
 * @return 現在の処理番号
 */
unsigned long FileIO::GetProcessId() {
	// 実行環境毎の処理番号の取得
	#if defined(_WIN32)
	const int Raw = _getpid();
	#else
	const pid_t Raw = getpid();
	#endif
	// 処理番号の返戻
	return static_cast<unsigned long>(Raw);
}

/**
 * 一時ファイル名の乱数部生成関数（予測可能な名前を狙った先回作成を成立させない為）
 * 計算量：O(1)
 * @return １６桁の１６進文字列
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::string FileIO::RandomSuffix() {
	// 名前毎に OS から乱数６４ビットを取得し，既知の名前から次を予測する先回作成を防止
	static std::random_device Device;
	static std::mutex Lock;
	uint64_t Val = 0;
	{
		// random_device の同時利用をシリアライズし，１つの接尾辞の一括取得
		const std::lock_guard<std::mutex> Guard(Lock);
		Val = static_cast<uint64_t>(Device()) << 32 | Device();
	}
	std::string Text(16, '0');
	// 乱数値の下位桁から１６進化
	for(int Idx = 15; Idx > -1; --Idx, Val >>= 4) Text[static_cast<size_t>(Idx)] = "0123456789ABCDEF"[Val & 0XF];
	// 乱数部の返戻
	return Text;
}

/**
 * 終了時に削除する排他的な一時ストリームの作成関数
 * 計算量：一時パス長 P に対し O(P)
 * @return 読書用のバイナリストリーム，作成出来なければ nullptr（成功時は呼出側が fclose で閉じる）
 * @throws std::bad_alloc 一時パスを確保出来ない場合
 * @throws std::length_error 一時パスが文字列の上限を超える場合
 * @throws std::system_error 一時パスの変換又は乱数の取得に失敗した場合
 */
std::FILE *FileIO::OpenTemporaryStream() {
	#if defined(_WIN32)
	// 新しい API は動的に解決し，未対応の Windows でも既存の一時パス API を使う
	using TempPathFunction = DWORD(WINAPI *)(DWORD, LPWSTR); // 一時ディレクトリの取得 API の型
	const HMODULE Kernel = GetModuleHandleW(L"kernel32.dll");
	const FARPROC Address = Kernel ? GetProcAddress(Kernel, "GetTempPath2W") : nullptr;
	static_assert(sizeof(TempPathFunction) == sizeof(Address));
	TempPathFunction ReadTempPath = &GetTempPathW;
	if(Address) std::memcpy(&ReadTempPath, &Address, sizeof(ReadTempPath));
	std::array<wchar_t, MAX_PATH + 2> Buffer;
	const DWORD Len = ReadTempPath(static_cast<DWORD>(Buffer.size()), Buffer.data());
	// 利用出来る一時ディレクトリを取得出来ない場合の返戻
	if(!Len || Len >= Buffer.size()) return nullptr;
	const Filesystem::path Directory(std::wstring_view(Buffer.data(), Len));
	for(size_t Attempt = 0; Attempt < 16; ++Attempt) {
		const Filesystem::path Path = GetNativePath(Directory / ("shavefmt-replay-" + RandomSuffix() + ".tmp"), true);
		NativeFileGuard File {
			CreateFileW(
				Path.c_str(),
				GENERIC_READ | GENERIC_WRITE,
				0,
				nullptr,
				CREATE_NEW,
				FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
				nullptr
			)
		};
		if(File.Handle == INVALID_HANDLE_VALUE) {
			const DWORD Error = GetLastError();
			if(Error == ERROR_FILE_EXISTS || Error == ERROR_ALREADY_EXISTS) continue;
			// 作成先・共有・アクセス権等で退避出来ない場合の返戻
			return nullptr;
		}
		const int Descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(File.Handle), _O_BINARY | _O_NOINHERIT | _O_RDWR);
		// CRT へ所有権を渡せない場合の返戻（ガードがハンドルを閉じて削除する）
		if(Descriptor < 0) return nullptr;
		File.HasClosed = true;
		std::FILE *const Stream = _fdopen(Descriptor, "w+b");
		if(!Stream) CloseFile(Descriptor);
		// 成功時は fclose が記述子とハンドルを閉じ，退避先を削除するストリームの返戻
		return Stream;
	}
	// 名前の衝突が続き作成出来ない場合の返戻
	return nullptr;
	#else
	// 排他的に作成され，閉じる時に削除される標準の一時ストリームの返戻
	return std::tmpfile();
	#endif
}

#if !defined(_WIN32)

#if defined(__APPLE__)

/**
 * 拡張 ACL 空判定関数
 * 計算量：属性の項目数 A に対し O(A)
 * @param Descriptor 対象記述子
 * @return 拡張 ACL が空なら true
 */
bool FileIO::HasNoExtendedAcl(const int Descriptor) {
	// 拡張 ACL 問合せ前のエラー状態の初期化
	errno = 0;
	// ACL 本体の取得と不在・取得失敗の区別
	const acl_t Access = ::acl_get_fd_np(Descriptor, ACL_TYPE_EXTENDED);
	// 拡張 ACL 不在時だけの空扱いの返戻
	if(!Access) return errno == ENOENT;
	// ACL 先頭項目の問合せ
	acl_entry_t Entry;
	errno = 0;
	const int EntryResult = ::acl_get_entry(Access, ACL_FIRST_ENTRY, &Entry), AccessError = errno;
	// 判定後の取得済 ACL の解放
	if(::acl_free(Access)) std::fprintf(stderr, "[notice] failed to release ACL (errno %d)\n", errno);
	if(!EntryResult) {
		errno = 0;
		// 登録項目が有る場合の返戻
		return false;
	}
	// 空一覧と問合せ失敗の識別
	if(AccessError == EINVAL) {
		errno = 0;
		// 登録項目が無い場合の返戻
		return true;
	}
	errno = AccessError;
	// 取得失敗の返戻
	return false;
}

#endif

/**
 * 拡張属性の複写関数（原本の付帯情報を差替先へ引き継ぐ）
 * 計算量：属性名・値の総長 B と属性数 A に対し O(B + A)
 * @param SrcDescriptor 原本の記述子
 * @param DstDescriptor 複写先の記述子
 * @param Error 系呼出が失敗した場合の理由（属性の競合なら空）
 * @return 属性を全て保持出来れば true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool FileIO::IsExtendedAttributeCopySuccessful(const int SrcDescriptor, const int DstDescriptor, std::error_code &Error) {
	// 複写結果格納域の初期化
	Error.clear();
	#if defined(__APPLE__)
	// 全属性を引き継げた場合の返戻
	if(!::fcopyfile(SrcDescriptor, DstDescriptor, nullptr, COPYFILE_XATTR)) return true;
	Error = std::error_code(errno, std::generic_category());
	// 複写失敗の返戻
	return false;
	#elif defined(__linux__)
	// 属性名列の必要寸法照会
	const ssize_t NamesLen = ::flistxattr(SrcDescriptor, nullptr, 0);
	if(NamesLen < 0) {
		// 拡張属性に未対応のファイルシステムを属性無と看做す事の返戻
		if(errno == ENOTSUP) return true;
		Error = std::error_code(errno, std::generic_category());
		// 属性名の寸法取得失敗の返戻
		return false;
	}
	if(NamesLen) {
		// 属性名一覧の固定長読込
		// 最初に確定した長さを使い，属性名一覧の途中変更の後で照合
		std::string Names(static_cast<size_t>(NamesLen), '\0');
		// 属性名列の確定
		const ssize_t ReadNamesLen = ::flistxattr(SrcDescriptor, Names.data(), Names.size());
		if(ReadNamesLen < 0) {
			Error = std::error_code(errno, std::generic_category());
			// 属性名の取得失敗の返戻
			return false;
		}
		// 読込中に属性名の寸法が変化した場合の返戻
		if(ReadNamesLen != NamesLen) return false;
		// 属性毎の値取得と複写
		for(size_t Pos = 0; Pos < Names.size();) {
			// 次の属性名と値寸法の取得
			const char *const Name = Names.data() + Pos;
			Pos += std::strlen(Name) + 1;
			// 属性値の必要寸法照会
			const ssize_t ValLen = ::fgetxattr(SrcDescriptor, Name, nullptr, 0);
			if(ValLen < 0) {
				Error = std::error_code(errno, std::generic_category());
				// 属性値の寸法取得失敗の返戻
				return false;
			}
			std::string Val(static_cast<size_t>(ValLen), '\0');
			// 属性値の固定長読込と複写
			// 値の長さも再読して属性の同時更新を拒む
			const ssize_t ReadValLen = ::fgetxattr(SrcDescriptor, Name, Val.data(), Val.size());
			if(ReadValLen < 0) {
				Error = std::error_code(errno, std::generic_category());
				// 属性値の取得失敗の返戻
				return false;
			}
			// 読込中に属性値の寸法が変化した場合の返戻
			if(ReadValLen != ValLen) return false;
			// 拡張属性値の複写失敗処理
			if(::fsetxattr(DstDescriptor, Name, Val.data(), Val.size(), 0)) {
				Error = std::error_code(errno, std::generic_category());
				// 属性値の複写失敗の返戻
				return false;
			}
		}
	}
	// 全属性を引き継げたかの返戻
	return true;
	#else
	static_cast<void>(SrcDescriptor);
	static_cast<void>(DstDescriptor);
	static_cast<void>(Error);
	// 拡張属性を持たない環境での正常の返戻
	return true;
	#endif
}

/**
 * 拡張属性一覧取得関数
 * 計算量：属性名・値の総長 B と属性数 A に対し O(B + A)
 * @param Descriptor 対象記述子
 * @param Attributes 属性名と値の格納先
 * @return 全属性を取得出来れば true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool FileIO::IsExtendedAttributeReadSuccessful(
	const int Descriptor,
	std::vector<std::pair<std::string, std::string>> &Attributes
) {
	// 実行環境毎の属性名一覧の必要寸法の取得
	#if defined(__APPLE__)
	const ssize_t NamesLen = ::flistxattr(Descriptor, nullptr, 0, 0);
	#else
	const ssize_t NamesLen = ::flistxattr(Descriptor, nullptr, 0);
	#endif
	// 拡張属性に未対応のファイルシステムは属性無と看做す結果の返戻
	if(NamesLen < 0) return errno == ENOTSUP;
	// 属性名列の読取
	std::string Names(static_cast<size_t>(NamesLen), '\0');
	#if defined(__APPLE__)
	// 属性名の読取に失敗した事の返戻
	if(NamesLen && ::flistxattr(Descriptor, Names.data(), Names.size(), 0) != NamesLen) return false;
	#else
	// 属性名の読取に失敗した事の返戻
	if(NamesLen && ::flistxattr(Descriptor, Names.data(), Names.size()) != NamesLen) return false;
	#endif
	// 属性毎の値取得
	for(size_t Pos = 0; Pos < Names.size();) {
		// NUL 区切の属性名を順に値との組へ変換する
		const std::string Name(Names.data() + Pos);
		Pos += Name.size() + 1;
		#if defined(__APPLE__)
		const ssize_t ValLen = ::fgetxattr(Descriptor, Name.c_str(), nullptr, 0, 0, 0);
		#else
		const ssize_t ValLen = ::fgetxattr(Descriptor, Name.c_str(), nullptr, 0);
		#endif
		// 属性値の大きさの取得に失敗した事の返戻
		if(ValLen < 0) return false;
		std::string Val(static_cast<size_t>(ValLen), '\0');
		#if defined(__APPLE__)
		// 属性値の読取に失敗した事の返戻
		if(::fgetxattr(Descriptor, Name.c_str(), Val.data(), Val.size(), 0, 0) != ValLen) return false;
		#else
		// 属性値の読取に失敗した事の返戻
		if(::fgetxattr(Descriptor, Name.c_str(), Val.data(), Val.size()) != ValLen) return false;
		#endif
		Attributes.emplace_back(Name, std::move(Val));
	}
	// 公開前後の比較に向けた属性の名前順整列
	std::sort(Attributes.begin(), Attributes.end());
	// 取得成功の返戻
	return true;
}

/**
 * 公開前後の付帯属性一致判定関数
 * 計算量：属性数 A と属性名・値の総長 B に対し O(A log(A + 1) + B)
 * @param Original 交換された原本記述子
 * @param Formatted 公開した整形結果記述子
 * @return 保持対象の属性が一致すれば true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool FileIO::HasSameExtendedAttributes(const int Original, const int Formatted) {
	// 所有者・グループ・モードの拡張属性と分けた直接比較
	struct stat OriginalState {}, FormattedState {};
	if(
		::fstat(Original, &OriginalState) || ::fstat(Formatted, &FormattedState) || OriginalState.st_uid != FormattedState.st_uid ||
		OriginalState.st_gid != FormattedState.st_gid || (OriginalState.st_mode & 07777) != (FormattedState.st_mode & 07777)
	// 属性が一致しない事の返戻
	) return false;
	#if defined(__APPLE__)
	if(OriginalState.st_flags != FormattedState.st_flags || !HasNoExtendedAcl(Original) || !HasNoExtendedAcl(Formatted)) {
		// ファイルのフラグ・拡張 ACL の不一致の返戻
		return false;
	}
	#endif
	// 拡張属性列の取得と比較
	std::vector<std::pair<std::string, std::string>> OriginalAttributes, FormattedAttributes;
	// 属性一致の返戻
	return IsExtendedAttributeReadSuccessful(Original, OriginalAttributes) &&
	IsExtendedAttributeReadSuccessful(Formatted, FormattedAttributes) && OriginalAttributes == FormattedAttributes;
}

#endif

#if !defined(_WIN32)

/**
 * ２つの名前をアトミックに交換する関数
 * 計算量：２つの名前の総長 P に対し O(P)
 * @param Dir 親ディレクトリ記述子
 * @param Left 一方の名前
 * @param Right 他方の名前
 * @return 交換成功時 true
 */
bool FileIO::IsNameExchangeSuccessful(const int Dir, const Filesystem::path &Left, const Filesystem::path &Right) {
	// 実行環境毎の名前交換
	#if defined(__APPLE__)
	// 交換結果の返戻
	return !::renameatx_np(Dir, Left.c_str(), Dir, Right.c_str(), RENAME_SWAP);
	#elif defined(__linux__)
	// 交換結果の返戻
	return !::syscall(SYS_renameat2, Dir, Left.c_str(), Dir, Right.c_str(), RENAME_EXCHANGE);
	#else
	// 名前のアトミックな交換を持たない環境では失敗として扱う
	static_cast<void>(Dir);
	static_cast<void>(Left);
	static_cast<void>(Right);
	errno = ENOTSUP;
	// 交換出来ない事の返戻
	return false;
	#endif
}

/**
 * 名前交換後の同一実体・状態判定関数
 * 計算量：O(1)
 * @param Left 一方の情報
 * @param Right 他方の情報
 * @return 名前変更で変わり得る属性変更日時を除いて一致すれば true
 */
bool FileIO::HasSameFileState(const std::array<uint64_t, 10> &Left, const std::array<uint64_t, 10> &Right) {
	// 状態一致の返戻
	return Left[0] == Right[0] && Left[1] == Right[1] && Left[2] == Right[2] && Left[3] == Right[3] && Left[4] == Right[4] &&
	Left[7] == Right[7] && Left[8] == Right[8];
}

/**
 * 親ディレクトリ同期関数
 * 計算量：O(1)
 * @param Dir 親ディレクトリ記述子
 * @return 同期成功時 true
 */
bool FileIO::IsDirSyncSuccessful(const int Dir) {
	// 同期対象ディレクトリの再取得
	// 名前交換の永続化対象に為る親自身を別記述子で開く
	const int Handle = ::openat(Dir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	// ディレクトリを開けない事の返戻
	if(Handle < 0) return false;
	// 親ディレクトリの同期と解放
	int Result;
	while((Result = ::fsync(Handle)) && errno == EINTR);
	const int SyncError = Result ? errno : 0, CloseResult = ::close(Handle);
	if(SyncError) errno = SyncError;
	// 同期結果の返戻
	return !Result && !CloseResult;
}

#endif

/**
 * 保持中の実体と現在の名前の照合関数
 * 計算量：名前の長さ P に対し O(P)
 * @param Handle 復旧の判断が終わる迄保持する実体記述子
 * @param Name Windows では絶対パス，POSIX では固定した親内の名前
 * @return 名前が保持中の実体を指せば true（取得不能も不一致として扱う）
 */
bool FileIO::IsNamedFile(const NativeFile Handle, const Filesystem::path &Name) const {
	// 保持中の実体と現在の名前の照合
	#if defined(_WIN32)
	const NativeFile Named = CreateFileW(
		Name.c_str(),
		FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr
	);
	const NativeFileGuard NamedGuard { Named };
	FileIdentity HeldInfo {}, NamedInfo {};
	// 保持中のハンドルと名前から取得した実体の一致の返戻
	return Named != INVALID_HANDLE_VALUE && IsIdentityReadSuccessful(Handle, HeldInfo) &&
	IsIdentityReadSuccessful(Named, NamedInfo) && HeldInfo == NamedInfo;
	#else
	struct stat HeldInfo {}, NamedInfo {};
	// リンクを辿らない名前と保持中の記述子の一致の返戻
	return !::fstat(Handle, &HeldInfo) && !::fstatat(ParentDescriptor, Name.c_str(), &NamedInfo, AT_SYMLINK_NOFOLLOW) &&
	HeldInfo.st_dev == NamedInfo.st_dev && HeldInfo.st_ino == NamedInfo.st_ino;
	#endif
}

/**
 * ファイル書込関数
 * 計算量：原文・候補・付帯属性の総長 B，パス長 P と成分数 C に対し O(B + CP)
 * @param Src 書き出す内容
 * @param IsHardLinkBroken 原本が別名と実体を共有して居た場合に true を設定
 * @param OutputError 処理系の失敗の理由を設定（外部の保存との競合・内容の照合の失敗では空の儘とし，errno を当推量で載せない）
 * @return 書込成功時 true（公開後の確保・照合失敗は復旧又は実体保全を行い false）
 * @throws std::bad_alloc 公開前に内容・属性・パスの確保に失敗した場合
 * @throws std::filesystem::filesystem_error パスのエンコーディングを変換出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool FileIO::IsFileWriteSuccessful(
	const std::string &Src,
	bool *const IsHardLinkBroken,
	std::error_code *const OutputError
) const {
	// 出力状態と失敗理由の初期化
	if(IsHardLinkBroken) *IsHardLinkBroken = false;
	if(OutputError) OutputError->clear();
	// パーサーの非常終了前に開始した書戻だけの保全
	const ParserAllocator::WriteGuard Publication;
	if(!Publication.CanWrite()) {
		if(OutputError) {
			*OutputError =
			std::make_error_code(ParserAllocator::IsInterrupted() ? std::errc::operation_canceled : std::errc::not_enough_memory);
		}
		// 非常終了開始後の新しい書戻の拒否の返戻
		return false;
	}
	// 親を固定出来ないか，対応する読込が無い場合の失敗の返戻
	if(!LastRead) return false;
	std::error_code ErrorCode, WriteError;
	const FileIO CurPath(TargetPath);
	if(!CurPath.IsReady || CurPath.ParentIdentities != ParentIdentities) {
		if(OutputError) *OutputError = std::make_error_code(std::errc::state_not_recoverable);
		// 読込後の親成分の差替の拒否の返戻
		return false;
	}
	const Filesystem::path Target = GetNativePath(PathOf(TargetPath));
	#if !defined(_WIN32)
	// POSIX 原本の記述子と状態の確保
	// 原本の状態を控える（権限・所有・別名の共有の有無は差替で失われる為，書出前に取る）
	struct stat OriginalState {};
	const Filesystem::path TargetName = Target.filename();
	const int OriginalDescriptor = ::openat(ParentDescriptor, TargetName.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
	if(OriginalDescriptor < 0) {
		if(OutputError) *OutputError = std::error_code(errno, std::generic_category());
		// 原本を開けなかった事の返戻
		return false;
	}
	// 書戻の間保持する原本記述子の所有者
	struct OriginalGuard {
		int Descriptor;

		/**
		 * 原本の記述子を解放するデストラクタ
		 * 計算量：O(1)
		 */
		~OriginalGuard() {
			// 原本記述子の解放
			if(Descriptor > -1) CloseFile(Descriptor);
			// 終了
			return;
		}
	};
	const OriginalGuard Guard { OriginalDescriptor };
	// 原本の同一性・種別・書込権限の検証
	std::array<uint64_t, 10> OriginalMetadata {};
	// 原本が通常ファイルでないか書込不可の場合の失敗の返戻（一時ファイル差替でも利用者の書込禁止を尊重）
	errno = 0;
	if(!IsMetadataReadSuccessful(OriginalDescriptor, OriginalMetadata, &OriginalState)) {
		if(OutputError && errno) *OutputError = std::error_code(errno, std::generic_category());
		// 原本の種別又は情報を検証出来ない場合の返戻
		return false;
	}
	#if defined(__linux__)
	if(OriginalMetadata[8] & (FS_APPEND_FL | FS_IMMUTABLE_FL)) {
		if(OutputError) *OutputError = std::make_error_code(std::errc::operation_not_permitted);
		// 追記専用又は変更禁止の原本を置換しない事の返戻
		return false;
	}
	#endif
	if(::faccessat(ParentDescriptor, TargetName.c_str(), W_OK, 0)) {
		if(OutputError) *OutputError = std::error_code(errno, std::generic_category());
		// 原本を書き換えれない場合の返戻
		return false;
	}
	// 別名と実体を共有して居たファイルは差替で切り離され，別名は旧内容の儘取り残される為，呼出側へ知らせる
	if(IsHardLinkBroken) *IsHardLinkBroken = OriginalState.st_nlink > 1;
	#else
	// Windows 原本ハンドルの排他的な確保
	// 書込共有を禁じ，差替が完了する迄原本の実体の保持
	const NativeFile OriginalDescriptor = CreateFileW(
		Target.c_str(),
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr
	);
	if(OriginalDescriptor == INVALID_HANDLE_VALUE) {
		RecordNativeError(OutputError);
		// 原本を開けない場合の失敗の返戻
		return false;
	}
	// 公開前の競合の照合迄保持する原本記述子の所有者
	struct OriginalGuard {
		NativeFile Handle;

		/**
		 * 原本の記述子を解放するデストラクタ
		 * 計算量：O(1)
		 */
		~OriginalGuard() {
			CloseFile(Handle);
			// 終了
			return;
		}
	};
	const OriginalGuard Guard { OriginalDescriptor };
	FileIdentity WriteIdentity {};
	// 全桁の識別子を保証出来ない書戻の拒否の返戻
	if(!IsIdentityReadSuccessful(OriginalDescriptor, WriteIdentity, OutputError)) return false;
	if(std::ranges::find(ParentIdentities, FileIdentity()) != ParentIdentities.end()) {
		if(OutputError) *OutputError = std::make_error_code(std::errc::operation_not_supported);
		// 全ての親の同一性を検証出来ない書戻の拒否の返戻
		return false;
	}
	if(IsHardLinkBroken) *IsHardLinkBroken = LastRead->Metadata[7] > 1;
	#endif
	// 読込時基準と現在の原本の再照合
	ReadState &Baseline = *LastRead;
	if(!IsStableFileReadSuccessful(OriginalDescriptor, true, Baseline.Bytes.size(), Baseline.Bytes, Baseline.Metadata)) {
		// 読込済の原本が消えた場合や，原本が読込時から変化した場合の失敗の返戻
		return false;
	}
	#if !defined(_WIN32)
	// 複写する権限・所有の取得時点と基準確定時点で状態が異なる場合の失敗の返戻
	if(OriginalMetadata != Baseline.Metadata) return false;
	#endif
	// 同一ディレクトリへの排他・リンク追随無の一時書出
	// 一時ファイル名の生成
	static std::atomic<unsigned long> TempSeq(0);
	const Filesystem::path TempPath = GetNativePath(
		Target.parent_path() / (
			".shavefmt-" + std::to_string(GetProcessId()) + "-" + std::to_string(TempSeq.fetch_add(1, std::memory_order_relaxed)) + "-" +
			RandomSuffix() + ".tmp"
		)
	);
	const std::string SafeTemp = ForDisplay(GetPathText(TempPath));
	std::string SafeBackup;
	const char *PreservationName = SafeTemp.c_str();
	bool HasPublished = false, ShouldKeepTemp = false;
	#if defined(_WIN32)
	// 原本アクセス制御の取得
	// 内容を書き始める前に原本のアクセス制御を適用し，公開権限の一時ファイルを作らない
	constexpr SECURITY_INFORMATION SecurityParts =
	OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
	DWORD SecurityBytes = 0;
	if(!GetKernelObjectSecurity(OriginalDescriptor, SecurityParts, nullptr, 0, &SecurityBytes)) {
		if(const DWORD SecurityError = GetLastError(); SecurityError != ERROR_INSUFFICIENT_BUFFER) {
			if(OutputError) *OutputError = std::error_code(static_cast<int>(SecurityError), std::system_category());
			// 寸法照会以外の失敗の返戻
			return false;
		}
	}
	std::vector<unsigned char> Security(SecurityBytes);
	if(
		!SecurityBytes || !GetKernelObjectSecurity(OriginalDescriptor, SecurityParts, Security.data(), SecurityBytes, &SecurityBytes)
	) {
		if(OutputError) *OutputError = std::error_code(static_cast<int>(GetLastError()), std::system_category());
		// アクセス制御を取得出来ない場合の失敗の返戻
		return false;
	}
	SECURITY_ATTRIBUTES Attributes { sizeof(SECURITY_ATTRIBUTES), Security.data(), FALSE };
	// 原本と同じアクセス制御を持つ一時ファイルの生成
	const HANDLE TempHandle = CreateFileW(
		TempPath.c_str(),
		GENERIC_WRITE,
		0,
		&Attributes,
		CREATE_NEW,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr
	);
	// 一時ファイルを作成出来ない場合（書込不可のディレクトリ・共有違反等）は，其の場の理由を返す
	if(TempHandle == INVALID_HANDLE_VALUE) {
		if(OutputError) *OutputError = std::error_code(static_cast<int>(GetLastError()), std::system_category());
		// 一時ファイル作成失敗の返戻
		return false;
	}
	// Windows の所有ハンドルを，実行時の記述子の生成 API が受ける整数表現へ渡す
	const int Descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(TempHandle), _O_WRONLY | _O_BINARY);
	// 一時ハンドルの記述子への移管
	if(Descriptor < 0) {
		if(OutputError) *OutputError = std::error_code(errno, std::generic_category());
		CloseFile(TempHandle);
		if(!DeleteFileW(TempPath.c_str())) std::fprintf(stderr, "[notice] %s: temporary file left behind\n", SafeTemp.c_str());
		// 記述子へ移管出来ない場合の失敗の返戻
		return false;
	}
	#else
	// POSIX 一時ファイルの排他的な生成
	const Filesystem::path TempName = TempPath.filename();
	const int Descriptor =
	::openat(ParentDescriptor, TempName.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
	#endif
	// 一時ファイル作成失敗（書込不可・同名の先回作成等）の返戻（容量不足・中断で原本を壊す直接書込への退避は禁止）
	if(Descriptor < 0) {
		if(OutputError) *OutputError = std::error_code(errno, std::generic_category());
		// 一時ファイルを作れなかった事の返戻
		return false;
	}
	// 失敗時に用いる一時ファイル削除処理の準備
	const auto IsTempRemovalSuccessful = [&]() -> bool {
		// 一時ファイルの削除
		#if defined(_WIN32)
		// 一時ファイルを消せたか，既に存在しない事の返戻
		return DeleteFileW(TempPath.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
		#else
		// 一時ファイルを消せたか，既に存在しない事の返戻
		return !::unlinkat(ParentDescriptor, TempName.c_str(), 0) || errno == ENOENT;
		#endif
	};
	// 一時記述子は名前の公開前後とも同じ所有者が解放する
	bool HasClosedTemp = false;
	// 終了時の一時ファイルと記述子の後始末
	const auto Cleanup = [&]() -> void {
		if(!HasClosedTemp) CloseFile(Descriptor);
		if(std::uncaught_exceptions()) {
			if(HasPublished || ShouldKeepTemp) {
				std::fprintf(stderr, "[notice] %s: replacement was preserved after an interrupted verification\n", PreservationName);
			} else if(!IsTempRemovalSuccessful()) std::fprintf(stderr, "[notice] %s: temporary file left behind\n", PreservationName);
		}
		// 終了
		return;
	};
	const ScopeExit<decltype(Cleanup)> CleanupGuard { Cleanup };
	// 残量が尽きる迄の書出の反復
	bool IsWriteOk = true;
	#if defined(__linux__)
	// ファイルシステムの内部形式のビットを継承せず，利用者が指定する通常ファイルの属性だけを継承
	constexpr uint32_t PreservedFlags =
	FS_APPEND_FL | FS_COMPR_FL | FS_DAX_FL | FS_IMMUTABLE_FL | FS_JOURNAL_DATA_FL | FS_NOATIME_FL | FS_NOCOMP_FL | FS_NOCOW_FL |
	FS_NODUMP_FL | FS_NOTAIL_FL | FS_SECRM_FL | FS_SYNC_FL | FS_UNRM_FL;
	int TempFlags = 0;
	if(::ioctl(Descriptor, FS_IOC_GETFLAGS, &TempFlags)) {
		if(errno != ENOTTY && errno != EOPNOTSUPP || OriginalMetadata[8] & PreservedFlags) {
			IsWriteOk = false;
			WriteError = std::error_code(errno, std::generic_category());
		}
	} else {
		const int DesiredFlags =
		static_cast<int>(static_cast<uint32_t>(TempFlags) & ~PreservedFlags | OriginalMetadata[8] & PreservedFlags);
		if(DesiredFlags != TempFlags && ::ioctl(Descriptor, FS_IOC_SETFLAGS, &DesiredFlags)) {
			IsWriteOk = false;
			WriteError = std::error_code(errno, std::generic_category());
		}
	}
	#endif
	#if defined(__APPLE__)
	// 一時ファイルの公開前アクセス制御の制限
	// 親から継承した ACL がモード `0600` を越えて読込を許す場合が有る為，内容を書く前に空の ACL へ固定する
	const acl_t TempAccess = ::acl_init(0);
	IsWriteOk = TempAccess && !::acl_set_fd_np(Descriptor, TempAccess, ACL_TYPE_EXTENDED);
	if(!IsWriteOk) WriteError = std::error_code(errno, std::generic_category());
	if(TempAccess) {
		const int SavedError = errno;
		if(::acl_free(TempAccess)) std::fprintf(stderr, "[notice] failed to release ACL (errno %d)\n", errno);
		errno = SavedError;
	}
	#elif defined(__linux__)
	// 親から継承した既定 ACL の除去
	// 親の既定 ACL を消してから原本の拡張属性だけを複写し，原本に無い権限を公開しない
	if(::fremovexattr(Descriptor, "system.posix_acl_access") && errno != ENODATA && errno != ENOTSUP) {
		IsWriteOk = false;
		WriteError = std::error_code(errno, std::generic_category());
	}
	#endif
	// 整形済内容の全量書出
	for(size_t Done = 0; IsWriteOk && Done < Src.size();) {
		#if defined(_WIN32)
		const int Wrote = _write(Descriptor, Src.data() + Done, static_cast<unsigned int>(Src.size() - Done));
		#else
		const ssize_t Wrote = ::write(Descriptor, Src.data() + Done, Src.size() - Done);
		#endif
		// 信号に依る中断は失敗ではない為，其の儘書き続ける（一律の失敗扱は整形結果を黙って捨てる）
		if(Wrote < 0 && errno == EINTR) continue;
		if(Wrote < 1) {
			IsWriteOk = false;
			WriteError = std::error_code(Wrote < 0 ? errno : EIO, std::generic_category());
			break;
		}
		Done += static_cast<size_t>(Wrote);
	}
	#if !defined(_WIN32)
	// POSIX 付帯情報の引継と永続化
	if(IsWriteOk) {
		// 所有・権限・属性の継承失敗時の原本保持
		if(::fchown(Descriptor, OriginalState.st_uid, OriginalState.st_gid)) {
			IsWriteOk = false;
			WriteError = std::error_code(errno, std::generic_category());
		}
		#if defined(__APPLE__)
		// 削除拒否等のアクセス制御を複写前に検査し，後始末不能と引継不能な権限の破棄を防止
		if(IsWriteOk && !HasNoExtendedAcl(OriginalDescriptor)) {
			IsWriteOk = false;
			// 問合せ自体の失敗は其の理由を，拡張 ACL が有る（書戻を扱わない）場合は非対応を理由に返す（空だと競合と報告される）
			WriteError = errno ? std::error_code(errno, std::generic_category()) : std::make_error_code(std::errc::not_supported);
		}
		#endif
		std::error_code AttributeError;
		// 拡張属性とファイルモードの引継
		// 拡張属性複写失敗の保存結果への反映
		if(IsWriteOk && !IsExtendedAttributeCopySuccessful(OriginalDescriptor, Descriptor, AttributeError)) {
			IsWriteOk = false;
			WriteError = AttributeError;
		}
		if(IsWriteOk && ::fchmod(Descriptor, OriginalState.st_mode & 07777)) {
			IsWriteOk = false;
			WriteError = std::error_code(errno, std::generic_category());
		}
		#if defined(__APPLE__)
		// macOS ファイルフラグの引継
		if(IsWriteOk && ::fchflags(Descriptor, OriginalState.st_flags)) {
			IsWriteOk = false;
			WriteError = std::error_code(errno, std::generic_category());
		}
		#endif
		if(IsWriteOk) {
			// 一時ファイル内容の永続化
			// 同期失敗時の原本差替の抑止
			int SyncResult;
			while((SyncResult = ::fsync(Descriptor)) && errno == EINTR);
			IsWriteOk = !SyncResult;
			if(!IsWriteOk) WriteError = std::error_code(errno, std::generic_category());
		}
	}
	#endif
	// 公開前照合に用いる一時ファイル状態の取得
	std::array<uint64_t, 10> TempMetadata {};
	#if defined(_WIN32)
	// Windows 一時ファイルの確定と解放
	// 記述子が所有する整数表現を，同じ Windows のハンドルとして借りる
	if(const NativeFile NativeTemp = reinterpret_cast<NativeFile>(_get_osfhandle(Descriptor)); IsWriteOk) {
		IsWriteOk =
		NativeTemp != INVALID_HANDLE_VALUE && IsMetadataReadSuccessful(NativeTemp, TempMetadata, nullptr, &WriteError, true);
	}
	if(IsWriteOk && _commit(Descriptor)) {
		IsWriteOk = false;
		WriteError = std::error_code(errno, std::generic_category());
	}
	HasClosedTemp = true;
	if(_close(Descriptor)) {
		// 先行する書込失敗の理由を保ち，重なる解放失敗は別に記録する
		if(IsWriteOk) WriteError = std::error_code(errno, std::generic_category());
		else std::fprintf(stderr, "[notice] failed to close temporary output (errno %d)\n", errno);
		IsWriteOk = false;
	}
	#else
	if(IsWriteOk) IsWriteOk = IsMetadataReadSuccessful(Descriptor, TempMetadata);
	#endif
	#if defined(__linux__)
	if(IsWriteOk && (OriginalMetadata[8] & PreservedFlags) != (TempMetadata[8] & PreservedFlags)) {
		IsWriteOk = false;
		WriteError = std::make_error_code(std::errc::operation_not_supported);
	}
	#endif
	if(IsWriteOk) {
		// 公開直前の原本不変性の再確認
		// 同じ記述子の内容だけでは，別実体へ差し替えられた保存を検出出来ない
		IsWriteOk = IsStableFileReadSuccessful(OriginalDescriptor, true, Baseline.Bytes.size(), Baseline.Bytes, Baseline.Metadata);
		#if defined(_WIN32)
		const NativeFile Cur = CreateFileW(
			Target.c_str(),
			FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_OPEN_REPARSE_POINT,
			nullptr
		);
		NativeFileGuard CurGuard { Cur };
		const bool IsOpened = Cur != INVALID_HANDLE_VALUE;
		#else
		const NativeFile Cur = ::openat(ParentDescriptor, TargetName.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
		NativeFileGuard CurGuard { Cur };
		const bool IsOpened = Cur > -1;
		#endif
		std::array<uint64_t, 10> CurMetadata {};
		// 公開名が読込時と同じ実体を指す事の照合
		IsWriteOk = IsWriteOk && IsOpened && IsMetadataReadSuccessful(Cur, CurMetadata, nullptr, &WriteError, true) &&
		CurMetadata == Baseline.Metadata;
		CurGuard.Close();
	}
	if(!IsWriteOk) {
		// 未公開一時ファイルの後始末
		// 系呼出の失敗だけ其の場で控えた理由を返し，照合不一致へ残存 errno を当てない
		if(OutputError) *OutputError = WriteError;
		#if !defined(_WIN32)
		HasClosedTemp = true;
		CloseFile(Descriptor);
		#endif
		// 一時ファイルの削除失敗は UTF-8 の安全なパスで報告する
		if(!IsTempRemovalSuccessful()) std::fprintf(stderr, "[notice] %s: temporary file left behind\n", SafeTemp.c_str());
		// 書出に失敗した一時ファイルを片付けての失敗の返戻
		return false;
	}
	#if defined(ShaveFormatFileIoTestHooks)
	// 公開直前競合を再現するテストフックの実行
	if(TestHook) {
		// １度だけ実行する競合再現フックの取得
		void (*const Hook)() = TestHook;
		TestHook = nullptr;
		Hook();
	}
	#endif
	// 公開する名前が，書き出して同期した候補自身を指す事の照合
	#if defined(_WIN32)
	const NativeFile Candidate = CreateFileW(
		TempPath.c_str(),
		FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr
	);
	const bool IsCandidateOpened = Candidate != INVALID_HANDLE_VALUE;
	#else
	const NativeFile Candidate = ::openat(ParentDescriptor, TempName.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
	const bool IsCandidateOpened = Candidate > -1;
	#endif
	NativeFileGuard CandidateGuard { Candidate };
	std::array<uint64_t, 10> CandidateMetadata {};
	std::error_code CandidateError;
	if(!IsCandidateOpened) RecordNativeError(&CandidateError);
	const bool IsOwnCandidate =
	IsCandidateOpened && IsMetadataReadSuccessful(Candidate, CandidateMetadata, nullptr, &CandidateError, true) &&
	CandidateMetadata[0] == TempMetadata[0] && CandidateMetadata[1] == TempMetadata[1] && CandidateMetadata[9] == TempMetadata[9];
	#if !defined(_WIN32)
	CandidateGuard.Close();
	#endif
	if(!IsOwnCandidate) {
		// 別実体又は実体を確かめれない一時名は削除も公開もせず，原本を其の儘保つ
		ShouldKeepTemp = true;
		if(OutputError) *OutputError = CandidateError;
		std::fprintf(stderr, "[notice] %s: temporary file left behind\n", SafeTemp.c_str());
		// 候補の同一性を確認出来ない事の返戻
		return false;
	}
	#if !defined(_WIN32)
	const std::string SafeTarget = ForDisplay(TargetPath);
	#endif
	// 検証本文の領域は公開前に準備し，公開後の付帯属性・パスの確保失敗は復旧へ合流させる
	ReadState OutputState, ReplacedState;
	OutputState.Bytes.reserve(Src.size());
	ReplacedState.Bytes.reserve(Baseline.Bytes.size());
	#if defined(_WIN32)
	// 競合実体の退避パス
	// 置換候補と復旧用パスの準備
	const Filesystem::path BackupPath = GetNativePath(Filesystem::path(TempPath).concat(".backup"));
	SafeBackup = ForDisplay(GetPathText(BackupPath));
	PreservationName = SafeBackup.c_str();
	std::vector<unsigned char> ReplacedSecurity(Security.size());
	const bool HasReplaced = ReplaceFileW(Target.c_str(), TempPath.c_str(), BackupPath.c_str(), 0, nullptr, nullptr);
	// アトミック置換結果と退避状態の記録
	const DWORD ReplacementError = HasReplaced ? ERROR_SUCCESS : GetLastError();
	HasPublished = HasReplaced;
	bool IsPublished = HasReplaced, ShouldKeepBackup = !HasReplaced;
	if(IsPublished) {
		NativeFileGuard BackupGuard { INVALID_HANDLE_VALUE }, CurGuard { INVALID_HANDLE_VALUE };
		try {
			// 公開名と退避原本の交換後照合
			#if defined(ShaveFormatFileIoTestHooks)
			if(AfterTestHook) {
				void (*const Hook)() = AfterTestHook;
				AfterTestHook = nullptr;
				Hook();
			}
			#endif
			const FileIO PublishedPath(TargetPath);
			// 公開先と退避先の読込
			BackupGuard.Handle = CreateFileW(
				BackupPath.c_str(),
				GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_DELETE,
				nullptr,
				OPEN_EXISTING,
				FILE_FLAG_OPEN_REPARSE_POINT,
				nullptr
			);
			CurGuard.Handle = CreateFileW(
				Target.c_str(),
				GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_DELETE,
				nullptr,
				OPEN_EXISTING,
				FILE_FLAG_OPEN_REPARSE_POINT,
				nullptr
			);
			const bool IsOwnOutput =
			CurGuard.Handle != INVALID_HANDLE_VALUE && IsVerificationReadSuccessful(CurGuard.Handle, Src.size(), OutputState) &&
			OutputState.Bytes == Src && OutputState.Metadata[0] == TempMetadata[0] && OutputState.Metadata[1] == TempMetadata[1] &&
			OutputState.Metadata[9] == TempMetadata[9];
			DWORD ReplacedSecurityBytes = 0;
			const bool HasOriginalSecurity = BackupGuard.Handle != INVALID_HANDLE_VALUE && GetKernelObjectSecurity(
				BackupGuard.Handle,
				SecurityParts,
				ReplacedSecurity.data(),
				static_cast<DWORD>(ReplacedSecurity.size()),
				&ReplacedSecurityBytes
			) && ReplacedSecurityBytes == Security.size() && ReplacedSecurity == Security;
			IsPublished =
			PublishedPath.IsReady && PublishedPath.ParentIdentities == ParentIdentities && HasOriginalSecurity && IsOwnOutput &&
			IsVerificationReadSuccessful(BackupGuard.Handle, Baseline.Bytes.size(), ReplacedState) &&
			ReplacedState.Bytes == Baseline.Bytes && ReplacedState.Metadata[0] == Baseline.Metadata[0] &&
			ReplacedState.Metadata[1] == Baseline.Metadata[1] && ReplacedState.Metadata[9] == Baseline.Metadata[9] &&
			ReplacedState.Metadata[3] == Baseline.Metadata[3] && ReplacedState.Metadata[5] == Baseline.Metadata[5] &&
			ReplacedState.Metadata[6] == Baseline.Metadata[6] && ReplacedState.Metadata[7] == Baseline.Metadata[7];
		} catch(const std::bad_alloc &) {
			IsPublished = false;
			ErrorCode = std::make_error_code(std::errc::not_enough_memory);
		} catch(const std::system_error &E) {
			IsPublished = false;
			ErrorCode = E.code();
		}
		if(!IsPublished) {
			ShouldKeepBackup = true;
			ShouldKeepTemp = true;
			// 読めない退避でも，元の原本記述子で実体を確認出来れば復旧する
			// 親ハンドルの保持下で候補と退避原本の実体を確かめ，照合の確保失敗でも復旧する
			if(
				const NativeFile RestoreSrc = BackupGuard.Handle != INVALID_HANDLE_VALUE ? BackupGuard.Handle : OriginalDescriptor;
				IsNamedFile(Candidate, Target) && IsNamedFile(RestoreSrc, BackupPath)
			) {
				#if defined(ShaveFormatFileIoTestHooks)
				if(TestHook) {
					void (*const Hook)() = TestHook;
					TestHook = nullptr;
					Hook();
				}
				#endif
				// 両移動に置換のフラグを付けず，再出現した名前を上書きしない
				if(!MoveFileExW(Target.c_str(), TempPath.c_str(), MOVEFILE_WRITE_THROUGH)) RecordNativeError(&ErrorCode);
				else if(IsNamedFile(Candidate, TempPath) && IsNamedFile(RestoreSrc, BackupPath)) {
					if(MoveFileExW(BackupPath.c_str(), Target.c_str(), MOVEFILE_WRITE_THROUGH)) {
						ShouldKeepBackup = false;
						PreservationName = SafeTemp.c_str();
						#if defined(ShaveFormatFileIoTestHooks)
						if(AfterTestHook) {
							void (*const Hook)() = AfterTestHook;
							AfterTestHook = nullptr;
							Hook();
						}
						#endif
						// 保持中の候補だけを後始末し，再競合の別実体は残す
						ShouldKeepTemp = !IsNamedFile(Candidate, TempPath);
					} else RecordNativeError(&ErrorCode);
				}
			}
			// 再競合又は復旧不能の実体を残す場合だけ残留先を通知する
			if(ShouldKeepTemp) {
				std::fprintf(
					stderr,
					"[notice] %s: publication conflict; replacement files left behind\n",
					ShouldKeepBackup ? SafeBackup.c_str() : SafeTemp.c_str()
				);
			}
		}
		BackupGuard.Close();
		CurGuard.Close();
	}
	// 置換失敗時に残った退避原本の復旧（再競合した対象名を置換しない）
	if(!HasReplaced) {
		const DWORD BackupAttributes = GetFileAttributesW(BackupPath.c_str());
		const DWORD BackupError = BackupAttributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
		if(BackupAttributes != INVALID_FILE_ATTRIBUTES) {
			ShouldKeepTemp = true;
			if(
				const DWORD TargetError = GetFileAttributesW(Target.c_str()) == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
				TargetError == ERROR_FILE_NOT_FOUND && IsNamedFile(OriginalDescriptor, BackupPath) &&
				MoveFileExW(BackupPath.c_str(), Target.c_str(), MOVEFILE_WRITE_THROUGH)
			) {
				ShouldKeepBackup = false;
				PreservationName = SafeTemp.c_str();
				ShouldKeepTemp = !IsNamedFile(Candidate, TempPath);
				if(ShouldKeepTemp) {
					std::fprintf(stderr, "[notice] %s: replacement recovery conflicted; temporary file left behind\n", SafeTemp.c_str());
				}
			} else std::fprintf(stderr, "[notice] %s: failed replacement was preserved as a backup\n", SafeBackup.c_str());
		} else if(BackupError != ERROR_FILE_NOT_FOUND) {
			ShouldKeepTemp = true;
			std::fprintf(stderr, "[notice] %s: could not inspect replacement backup\n", SafeBackup.c_str());
		}
	}
	if(!IsPublished && ReplacementError) ErrorCode = std::error_code(static_cast<int>(ReplacementError), std::system_category());
	// 確認済の公開の後でだけ，退避した旧原本を除く
	if(IsPublished && !ShouldKeepBackup && !DeleteFileW(BackupPath.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
		std::fprintf(stderr, "[notice] %s: temporary backup left behind\n", SafeBackup.c_str());
	}
	#else
	// 検証済の候補と公開名のアトミックな交換
	int ExchangeFailure = 0;
	bool IsPublished = IsNameExchangeSuccessful(ParentDescriptor, TempName, TargetName);
	HasPublished = IsPublished;
	if(!IsPublished) ExchangeFailure = errno;
	// 名前の交換の失敗の理由は其の場でしか確定しない（後続の照合・復旧・解放が errno を書き換える）
	// 交換後の公開結果と退避原本の照合
	if(IsPublished) {
		NativeFileGuard CurGuard { -1 }, ReplacedGuard { -1 };
		try {
			#if defined(ShaveFormatFileIoTestHooks)
			if(AfterTestHook) {
				void (*const Hook)() = AfterTestHook;
				AfterTestHook = nullptr;
				Hook();
			}
			#endif
			const FileIO PublishedPath(TargetPath);
			CurGuard.Handle = ::openat(ParentDescriptor, TargetName.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
			ReplacedGuard.Handle = ::openat(ParentDescriptor, TempName.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
			// 読めたか否かは復旧の判断に要る為，両実体の再読込の可否を個別に保つ
			const bool IsCurRead = CurGuard.Handle > -1 && IsVerificationReadSuccessful(CurGuard.Handle, Src.size(), OutputState);
			const bool IsReplacedRead =
			ReplacedGuard.Handle > -1 && IsVerificationReadSuccessful(ReplacedGuard.Handle, Baseline.Bytes.size(), ReplacedState);
			// 両実体の内容・状態・付帯属性の一致確認
			IsPublished = PublishedPath.IsReady && PublishedPath.ParentIdentities == ParentIdentities && IsCurRead && IsReplacedRead &&
			HasSameFileState(OutputState.Metadata, TempMetadata) && OutputState.Bytes == Src &&
			HasSameFileState(ReplacedState.Metadata, Baseline.Metadata) && ReplacedState.Bytes == Baseline.Bytes &&
			HasSameExtendedAttributes(ReplacedGuard.Handle, CurGuard.Handle) && IsDirSyncSuccessful(ParentDescriptor);
		} catch(const std::bad_alloc &) {
			IsPublished = false;
			ErrorCode = std::make_error_code(std::errc::not_enough_memory);
		} catch(const std::system_error &E) {
			IsPublished = false;
			ErrorCode = E.code();
		}
		if(!IsPublished) {
			ShouldKeepTemp = true;
			// 固定した親記述子内の両名を保持中の実体と照合し，読取・パスの確保失敗でも復旧する
			if(
				IsNamedFile(Descriptor, TargetName) &&
				IsNamedFile(ReplacedGuard.Handle > -1 ? ReplacedGuard.Handle : OriginalDescriptor, TempName)
			) {
				#if defined(ShaveFormatFileIoTestHooks)
				if(TestHook) {
					void (*const Hook)() = TestHook;
					TestHook = nullptr;
					Hook();
				}
				#endif
				if(IsNameExchangeSuccessful(ParentDescriptor, TempName, TargetName)) {
					#if defined(ShaveFormatFileIoTestHooks)
					if(AfterTestHook) {
						void (*const Hook)() = AfterTestHook;
						AfterTestHook = nullptr;
						Hook();
					}
					#endif
					// 退避名が保持中の候補を指す場合だけ後始末する
					ShouldKeepTemp = !IsNamedFile(Descriptor, TempName);
				} else ErrorCode = std::error_code(errno, std::generic_category());
			}
			// 別実体を残す場合は再交換も削除もせず，残留先を通知する
			if(ShouldKeepTemp) std::fprintf(stderr, "[notice] %s: publication conflict; temporary file left behind\n", SafeTemp.c_str());
		}
		CurGuard.Close();
		ReplacedGuard.Close();
	}
	// 名前の後始末迄作成記述子を保持し，通常の返戻で１度だけ解放する
	const auto CloseTemp = [&]() -> void {
		HasClosedTemp = true;
		if(::close(Descriptor)) {
			const int CloseError = errno;
			std::fprintf(
				stderr,
				"[notice] %s: %s (errno %d)\n",
				SafeTarget.c_str(),
				IsPublished ? "formatted output was published but closing it failed" : "failed to close temporary output",
				CloseError
			);
		}
		// 終了
		return;
	};
	// 名前の交換の失敗だけが処理系の理由を持つ（内容の照合の不一致は errno を持たず，此処へ来る迄に close 等が上書きして居る）
	if(!IsPublished && ExchangeFailure) ErrorCode = std::error_code(ExchangeFailure, std::generic_category());
	#endif
	// 公開失敗時の保存物とエラー状態の整理
	if(!IsPublished) {
		if(!ShouldKeepTemp && !IsTempRemovalSuccessful()) {
			std::fprintf(stderr, "[notice] %s: temporary file left behind\n", SafeTemp.c_str());
		}
		#if !defined(_WIN32)
		// 復旧と候補の除去の名前変更を共に永続化する
		if(HasPublished && !ShouldKeepTemp && !IsDirSyncSuccessful(ParentDescriptor)) {
			ErrorCode = std::error_code(errno, std::generic_category());
		}
		CloseTemp();
		#endif
		// 系呼出の失敗だけ理由を返し，競合・照合の不一致は理由を持たない失敗として区別する
		if(OutputError && ErrorCode) *OutputError = ErrorCode;
		// 差替又は公開後の照合に失敗した事の返戻（復旧不能時の残留先は上で報告）
		return false;
	}
	#if !defined(_WIN32)
	// 交換済原本の一時名からの除去
	if(!IsTempRemovalSuccessful()) std::fprintf(stderr, "[notice] %s: replaced original left behind\n", SafeTemp.c_str());
	CloseTemp();
	#endif
	// 公開後の読込基準の破棄
	LastRead.reset();
	// 書込成功の返戻
	return true;
}
