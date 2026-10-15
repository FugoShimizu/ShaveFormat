#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <sys/stat.h>

using FileIdentity = std::array<uint64_t, 3>; // 領域番号と１２８ビットの実体番号（POSIX は末尾０）

// ファイル入出力クラス
class FileIO {
private: // 記述子と保存時状態の内部管理

	#if defined(_WIN32) // Windows の実体記述子

	using NativeFile = void *; // 読込対象の記述子（Windows の HANDLE）

	#else // POSIX の実体記述子

	using NativeFile = int; // 読込対象の記述子

	#endif

	struct NativeFileGuard; // 読込・照合用の実体記述子の例外時も含む所有者

	// 保存時の競合検査に使う読込原本の状態
	struct ReadState {
		std::string Bytes; // BOM を含む読込時の原本
		std::array<uint64_t, 10> Metadata {}; // 同一性・寸法・更新情報（読取日時は除外）
	};

	#if defined(_WIN32)

	static constexpr std::wstring_view ExtendedPathPrefix = L"\\\\?\\"; // 拡張パスの接頭辞
	static constexpr std::wstring_view ExtendedUncPrefix = L"\\\\?\\UNC\\"; // 拡張 UNC パスの接頭辞

	#endif

	#if defined(ShaveFormatFileIoTestHooks)

	#if defined(_WIN32)

	static inline bool HasUnsupportedIdentity = false; // 識別 API が非対応の読取経路の再現

	#endif

	static inline void (*TestHook)() = nullptr; // 名前変更の直前に１回呼ぶ保存競合再現用フック
	static inline void (*AfterTestHook)() = nullptr; // 名前変更の直後に１回呼ぶ競合再現用フック

	#endif

	mutable std::error_code ReadError; // 読込の原因の Main へ受渡
	mutable std::optional<ReadState> LastRead; // 成功した読込の控え（保存後に消費）
	std::vector<FileIdentity> ParentIdentities; // 収集から読込迄を照合する親成分
	std::optional<std::array<uint64_t, 10>> InspectionMetadata; // 受理計数した原文の状態
	std::optional<FileIdentity> ExpectedIdentity; // 収集時の対象の実体
	std::string TargetPath; // 対象の絶対パス（最終ファイルのリンクは辿らない）
	bool IsReady = false; // 親ディレクトリを固定出来たか

	#if defined(_WIN32)

	std::vector<void *> DirHandles; // 削除共有を禁じた親ディレクトリグループ

	#else

	int ParentDescriptor = -1; // 書換の間保持する親ディレクトリ記述子

	#endif

	#if defined(_WIN32)

	static void CloseFile(void *const Handle); // 既存のエラーを保つハンドル解放関数

	#endif

	static void CloseFile(const int Descriptor); // 既存のエラーを保つ記述子解放関数
	static void RecordNativeError(std::error_code *const Error); // 失敗した入出力の処理系の原因の記録関数

	#if defined(_WIN32)

	static bool IsIdentityReadSuccessful(const NativeFile Handle, FileIdentity &Identity, std::error_code *const Error = nullptr); // 領域番号と１２８ビットの実体番号の取得関数

	#endif

	#if defined(_WIN32)

	static bool IsReadIdentitySuccessful(const NativeFile Handle, FileIdentity &Identity, std::error_code *const Error = nullptr); // 読取では非対応の識別子だけを未知として扱う関数

	#endif

	static bool IsMetadataReadSuccessful(
		const NativeFile Handle,
		std::array<uint64_t, 10> &Metadata,
		struct stat *const FileState = nullptr,
		std::error_code *const ReadFailure = nullptr,
		const bool ShouldVerifyIdentity = false
	); // 原本の同一性と更新情報の取得関数

	static bool IsStableFileReadSuccessful(
		const NativeFile Handle,
		const bool ShouldCompare,
		const size_t MaxSize,
		std::string &Bytes,
		std::array<uint64_t, 10> &Metadata,
		std::error_code *const ReadFailure = nullptr,
		const FileIdentity *const Expected = nullptr,
		const std::array<uint64_t, 10> *const ExpectedMetadata = nullptr
	); // 読込中の更新を検査する原本取得・照合関数

	static bool IsHiddenPoint(const char32_t Point); // 表示を隠す制御文字の判定関数
	static bool IsVariationSelector(const char32_t Point); // 異体字セレクタの判定関数
	static bool IsVerificationReadSuccessful(const NativeFile Handle, const size_t MaxSize, ReadState &State); // 書戻の照合の読込関数
	static unsigned long GetProcessId(); // 処理番号取得関数
	static std::string RandomSuffix(); // 一時ファイル名の乱数部生成関数

	#if !defined(_WIN32)

	#if defined(__APPLE__)

	static bool HasNoExtendedAcl(const int Descriptor); // 拡張 ACL 空判定関数

	#endif

	static bool IsExtendedAttributeCopySuccessful(const int SrcDescriptor, const int DstDescriptor, std::error_code &Error); // 拡張属性の複写関数

	static bool IsExtendedAttributeReadSuccessful(
		const int Descriptor,
		std::vector<std::pair<std::string, std::string>> &Attributes
	); // 拡張属性一覧取得関数

	static bool HasSameExtendedAttributes(const int Original, const int Formatted); // 公開前後の付帯属性一致判定関数
	static bool IsNameExchangeSuccessful(const int Dir, const std::filesystem::path &Left, const std::filesystem::path &Right); // ２つの名前をアトミックに交換する関数
	static bool HasSameFileState(const std::array<uint64_t, 10> &Left, const std::array<uint64_t, 10> &Right); // 名前交換後の同一実体・状態判定関数
	static bool IsDirSyncSuccessful(const int Dir); // 親ディレクトリ同期関数

	#endif

	bool IsNamedFile(const NativeFile Handle, const std::filesystem::path &Name) const; // 保持中の実体と現在の名前の照合関数

public: // 呼出側へ公開する入出力操作

	using InputReader = std::function<size_t(size_t, std::span<char>)>; // 固定領域へ原文を読む関数
	using InputEstimator = std::function<size_t(size_t, const InputReader &)>; // 入口条件に従う受理見積の関数

	// 解析前の受理計数と，其の原文の実体・更新状態
	struct InputInspection {
		size_t Bytes = 0; // 既知の原文長
		size_t WorkingBytes = 0; // 原文の受理見積
		std::array<uint64_t, 10> Metadata {}; // 計数した原文の状態
	};

	static std::string ForDisplay(const std::string_view Text, const bool IsBody = false); // 端末制御列の無害化関数
	static std::string GetPathText(const std::filesystem::path &Path); // パスの UTF-8 のテキストの取得関数
	static std::filesystem::path PathOf(const std::string_view Text); // UTF-8 のパスのテキストの処理系のパスへの変換関数
	static std::filesystem::path GetNativePath(const std::filesystem::path &Path, const bool ShouldExtend = false); // 成分検査済の絶対パスを長いパス対応の API へ渡す関数
	static std::filesystem::path GetAbsolutePath(const std::string_view Text, std::error_code &Error); // 元のパスの成分を畳まない絶対パスの取得関数

	#if defined(ShaveFormatFileIoTestHooks)

	#if defined(_WIN32)

	static void SetUnsupportedIdentity(const bool IsUnsupported); // 識別 API の非対応を再現するフックの設定関数

	#endif

	static void SetTestHook(void (*const Hook)()); // 保存競合再現用フック設定関数
	static void SetAfterTestHook(void (*const Hook)()); // 公開後競合再現用フック設定関数

	#endif

	#if defined(_WIN32)

	static std::string GetPathDisplay(const std::filesystem::path &Path); // 不正な UTF-16 のコードユニットも識別出来る表示用パスの取得関数
	static std::string GetNativeErrorText(const std::error_code &Error); // 処理系の失敗理由の UTF-8 の取得関数
	static bool IsNameSurrogate(void *const Handle, std::error_code *const Error = nullptr); // 名前を転送する再解析点かの判定関数

	#endif

	static std::FILE *OpenTemporaryStream(); // 排他的な退避先の作成関数（呼出側が fclose で閉じる）
	std::optional<std::string> GetNormalizedPath() const; // 実際の名前解決に従う絶対パスの取得関数
	std::optional<FileIdentity> GetIdentity() const; // 固定したパスの実体識別子の取得関数

	std::optional<InputInspection> InspectInput(
		const InputEstimator &Estimate,
		const InputInspection *const Counted = nullptr
	) const; // 原文を保持せず解析前の受理計数を求める関数

	std::vector<FileIdentity> GetPathIdentity() const; // 親成分と対象の収集時の実体の取得関数
	uint64_t GetLinkCount() const; // 書戻前の別名の数の取得関数
	std::error_code GetReadError() const; // 読込失敗の原因の取得関数
	bool IsFileReadSuccessful(std::string &Src, const bool ShouldKeepWriteBaseline = true) const; // 固定した親パス上でのファイル読込関数

	bool IsFileWriteSuccessful(
		const std::string &Src,
		bool *const IsHardLinkBroken = nullptr,
		std::error_code *const OutputError = nullptr
	) const; // アトミック書込関数（再保存には再読込が必要）

	FileIO &operator=(const FileIO &) = delete; // 記述子の代入禁止

	explicit FileIO(
		const std::string &Path,
		const std::span<const FileIdentity> Expected = std::span<const FileIdentity>(),
		const std::optional<InputInspection> Accepted = std::nullopt
	); // 親ディレクトリを固定するコンストラクタ

	FileIO(const FileIO &) = delete; // 記述子の複写禁止

	~FileIO(); // 親ディレクトリの固定を解除するデストラクタ
};
