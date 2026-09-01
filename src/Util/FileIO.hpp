#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <string_view>
#include <utility>
#include <vector>

// ファイル入出力クラス
class FileIO {
private: // 記述子と保存時状態の内部管理

	#if defined(_WIN32) // Windows の実体記述子

	using NativeFile = void *; // 読込対象の記述子（Windows の HANDLE）

	#else // POSIX の実体記述子

	using NativeFile = int; // 読込対象の記述子

	#endif

	struct ReadState {
		std::string Bytes; // BOM を含む読込時の原本
		std::array<uint64_t, 8> Metadata {}; // 同一性・寸法・更新情報（読取日時は除外）
	}; // 保存時の競合検査に使う読込原本の状態

	#if defined(ShaveFormatFileIoTestHooks)

	static inline void (*TestHook)() = nullptr; // 公開直前に呼ぶ保存競合再現用フック
	static inline void (*AfterTestHook)() = nullptr; // 公開直後に呼ぶ公開後競合再現用フック

	#endif

	mutable std::optional<ReadState> LastRead; // 成功した読込の控え（保存後に消費）
	std::string TargetPath; // 対象の絶対経路（最終ファイルのリンクは辿らない）
	bool IsReady = false; // 親ディレクトリを固定出来たか

	#if defined(_WIN32)

	std::vector<void *> DirectoryHandles; // 削除共有を禁じた親ディレクトリ群

	#else

	int ParentDescriptor = -1; // 書換の間保持する親ディレクトリ記述子

	#endif

	static bool ReadMetadata(const NativeFile Handle, std::array<uint64_t, 8> &Metadata); // 原本の同一性と更新情報の取得関数

	static bool ReadStableFile(
		const NativeFile Handle,
		std::string &Bytes,
		std::array<uint64_t, 8> &Metadata,
		const bool ShouldCompare,
		const size_t MaxSize
	); // 読込中の更新を検査する原本取得・照合関数

	static unsigned long GetProcessId(); // 処理番号取得関数
	static std::string RandomSuffix(); // 一時ファイル名の乱数部生成関数

	#if !defined(_WIN32)

	#if defined(__APPLE__)

	static bool HasNoExtendedAcl(const int Descriptor); // 拡張 ACL 空判定関数

	#endif

	static bool CopyExtendedAttributes(const int SourceDescriptor, const int DestinationDescriptor, std::error_code &Error); // 拡張属性の複写関数
	static bool ReadExtendedAttributes(const int Descriptor, std::vector<std::pair<std::string, std::string>> &Attributes); // 拡張属性一覧取得関数
	static bool HasSameExtendedAttributes(const int Original, const int Formatted); // 公開前後の付帯属性一致判定関数
	static bool ExchangeNames(const int Directory, const std::filesystem::path &Left, const std::filesystem::path &Right); // ２つの名前を原子的に交換する関数
	static bool HasSameFileState(const std::array<uint64_t, 8> &Left, const std::array<uint64_t, 8> &Right); // 名前交換後の同一実体・状態判定関数
	static bool SyncDirectory(const int Directory); // 親ディレクトリ同期関数

	#endif

public: // 呼出側へ公開する入出力操作

	static constexpr size_t MaxInputBytes = 0X1000000; // 百倍余りの作業域と３２ビット位置を見込み，16 MiB に抑える入力上限
	static std::vector<std::string> SplitLines(const std::string &Source); // 行配列分割関数（末尾の空行は除去）
	static void FoldCrLf(std::string &Source, const size_t From, const bool IsLoneCrNewline); // 指定位置以降の CRLF 畳込関数
	static void Normalize(std::string &Source, const bool IsLoneCrNewline); // 改行コードと末尾改行の正規化関数
	static bool IsNewlineMixed(const std::string &Source); // LF と CRLF の混在判定関数
	static bool IsCrOnlyNewline(const std::string &Source); // CR 単独改行の判定関数
	static void ExpandLf(std::string &Source); // LF の CRLF への展開関数
	static size_t StripBom(std::string &Source); // 先頭 BOM の除去関数
	static std::string ForDisplay(const std::string_view Text, const bool KeepsTab = false); // 端末制御列の無害化関数

	#if defined(ShaveFormatFileIoTestHooks)

	static void SetTestHook(void (*const Hook)()); // 保存競合再現用フック設定関数
	static void SetAfterTestHook(void (*const Hook)()); // 公開後競合再現用フック設定関数

	#endif

	bool ReadFile(std::string &Source, size_t *const BomCount = nullptr) const; // 固定した親経路上でのファイル読込関数

	bool WriteFile(
		const std::string &Source,
		bool *const IsHardLinkBroken = nullptr,
		std::error_code *const OutError = nullptr
	) const; // 原子的書込関数（再保存には再読込が必要）

	FileIO &operator=(const FileIO &) = delete; // 記述子の代入禁止

	explicit FileIO(const std::string &Path); // 親ディレクトリを固定するコンストラクタ

	FileIO(const FileIO &) = delete; // 記述子の複写禁止

	~FileIO(); // 親ディレクトリの固定を解除するデストラクタ
};
