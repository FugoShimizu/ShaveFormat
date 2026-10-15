#include "Util/FileIO.hpp"
#include "Util/FormatLimitExceeded.hpp"
#include "Util/WorkingMemory.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <new>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)

#include <windows.h>

#else

#include <sys/stat.h>
#include <sys/xattr.h>

#if defined(__linux__)

#include <fcntl.h>
#include <unistd.h>
#include <linux/fs.h>
#include <sys/ioctl.h>

#endif

#endif

// ファイルシステム名前空間の別名
namespace Filesystem = std::filesystem;

// 公開競合テストの鉤状態
static Filesystem::path HookTarget;
static Filesystem::path HookReplacement;
static Filesystem::path HookHeld;
static constexpr char AttributeVal[] = "latest attribute";

/**
 * テスト条件の検査関数
 * 計算量：O(1)
 * @param IsSatisfied 成功条件
 * @param Msg 失敗箇所の説明
 * @throws std::runtime_error 条件が成り立たない場合
 */
static void Check(const bool IsSatisfied, const char *const Msg) {
	// 最初の不一致でのテスト停止と例外文面への失敗位置保存
	if(!IsSatisfied) throw std::runtime_error(Msg);
	// 終了
	return;
}

// テスト専用の排他的な一時領域
class TestDir {
public:

	Filesystem::path Path; // 此のテストだけの一時領域

	TestDir &operator=(const TestDir &) = delete; // コピー代入演算子（禁止）
	TestDir(const TestDir &) = delete; // コピーコンストラクタ（禁止）

	/**
	 * テスト領域の生成コンストラクタ
	 * 計算量：テストケースのテキストとパスの総長 B，列挙するファイル数 F に対し O(B + F)
	 * @throws std::runtime_error テスト用のディレクトリを作れない場合
	 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
	 */
	TestDir() {
		// 一時領域の正規化と無作為名に依る生成
		std::random_device Random;
		const Filesystem::path Root = Filesystem::canonical(Filesystem::temp_directory_path());
		// 無作為名と既存リソースの衝突時だけの再試行
		for(uint32_t Attempt = 0; Attempt < 100; ++Attempt) { // 一時名生成の最大試行回数
			Path = Root / ("shavefmt-fileio-" + std::to_string(Random()));
			// 自分が生成出来た領域だけを取得しての返戻
			if(Filesystem::create_directory(Path)) return;
		}
		throw std::runtime_error("cannot create test directory");
	}

	/**
	 * テスト領域の解放デストラクタ
	 * 計算量：テストケースのテキストとパスの総長 B，列挙するファイル数 F に対し O(B + F)
	 */
	~TestDir() {
		// 例外送出中も継続する一時領域の解放
		std::error_code Error;
		Filesystem::remove_all(Path, Error);
		if(Error) std::fprintf(stderr, "[warn] test directory left behind: %s\n", Path.string().c_str());
		// 終了
		return;
	}
};

/**
 * 差替保存の再現関数
 * 計算量：２つのパスの総長 P に対し O(P)
 * @param From 移動元
 * @param To 差替先
 * @throws std::runtime_error 呼出先の検査又は入出力が失敗した場合
 */
static void Replace(const Filesystem::path &From, const Filesystem::path &To) {
	// 各環境のアトミック置換手段に依る外部保存の再現
	#if defined(_WIN32)
	// 保存中の口が削除を共有して開いた儘の差替先も，名前を直ちに外す削除の後に置き換える
	const bool IsReplaced = DeleteFileW(To.c_str()) && MoveFileExW(From.c_str(), To.c_str(), 0);
	Check(IsReplaced, ("fixture replacement failed: " + To.filename().string() + " " + std::to_string(GetLastError())).c_str());
	#else
	Filesystem::rename(From, To);
	#endif
	// 終了
	return;
}

/**
 * 公開直前の原本差替再現関数
 * 計算量：テストケースのテキストとパスの総長 B，列挙するファイル数 F に対し O(B + F)
 * @throws std::runtime_error 呼出先の検査又は入出力が失敗した場合
 */
static void ReplaceTargetAtPublish() {
	// 公開直前の鉤で読込後に別の実体へ差替
	Replace(HookReplacement, HookTarget);
	// 終了
	return;
}

/**
 * 公開後の領域確保失敗の注入関数
 * 計算量：O(1)
 * @throws std::bad_alloc 公開後の照合の確保失敗の再現
 */
static void FailPublishedAllocation() {
	// 公開後の領域確保失敗の注入
	throw std::bad_alloc();
}

/**
 * 外部保存の再現関数
 * 計算量：保存するテキスト長 B に対し O(B)
 * @param Path 保存先
 * @param Bytes 保存する生バイト
 * @throws std::runtime_error 呼出先の検査又は入出力が失敗した場合
 */
static void Save(const Filesystem::path &Path, const std::string &Bytes) {
	// 改行変換を伴わないテストケースの生バイト置換
	std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
	Output.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
	Output.close();
	Check(!Output.fail(), "fixture write failed");
	// 終了
	return;
}

/**
 * 外部読込の再現関数
 * 計算量：保存したテキスト長 B に対し O(B)
 * @param Path 読込元
 * @return 生バイトの全内容
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::runtime_error 呼出先の検査又は入出力が失敗した場合
 */
static std::string Load(const Filesystem::path &Path) {
	// 保存後実体の改行変換を伴わない終端迄の読込
	std::ifstream Input(Path, std::ios::binary);
	Check(Input.is_open(), "fixture open failed");
	const std::string Bytes { std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>() };
	Check(!Input.bad(), "fixture read failed");
	// 原本の返戻
	return Bytes;
}

/**
 * 外部保存と公開後の領域確保失敗の注入関数
 * 計算量：固定した名前変更の費用 Q に対し O(Q)
 * @throws std::filesystem::filesystem_error 外部保存の名前変更に失敗した場合
 * @throws std::bad_alloc 外部保存の後の照合の確保失敗の再現
 * @throws std::runtime_error 呼出先の検査又は入出力が失敗した場合
 */
static void ReplaceTargetAndFailPublishedAllocation() {
	// 外部保存と公開後の領域確保失敗の注入
	ReplaceTargetAtPublish();
	FailPublishedAllocation();
	// 終了
	return;
}

/**
 * 公開直後に上限を超える外部保存へ差し替える再現関数
 * 計算量：テストケースのテキストとパスの総長 B，列挙するファイル数 F に対し O(B + F)
 * @throws std::runtime_error 呼出先の検査又は入出力が失敗した場合
 */
static void ReplaceOversizedTargetAtPublish() {
	// 外部の新しい実体を読込上限を超える寸法にし，公開名を差し替える
	Save(HookReplacement, "external after publication");
	Filesystem::resize_file(HookReplacement, MaxInputBytes + 1);
	Replace(HookReplacement, HookTarget);
	// 終了
	return;
}

/**
 * 公開直前の拡張属性更新再現関数
 * 計算量：テストケースのテキストとパスの総長 B，列挙するファイル数 F に対し O(B + F)
 * @throws std::runtime_error 呼出先の検査又は入出力が失敗した場合
 */
static void ChangeAttributeAtPublish() {
	// 本文を変えない拡張属性更新に依るメタデータ競合の生成
	#if defined(__APPLE__)
	Check(
		!::setxattr(HookTarget.c_str(), "user.shavefmt-test", AttributeVal, sizeof(AttributeVal), 0, 0),
		"attribute update failed"
	);
	#elif defined(__linux__)
	Check(!::setxattr(HookTarget.c_str(), "user.shavefmt-test", AttributeVal, sizeof(AttributeVal), 0), "attribute update failed");
	#endif
	// 終了
	return;
}

#if !defined(_WIN32)

/**
 * 公開直前の本文変更再現関数
 * 計算量：テストケースのテキストとパスの総長 B，列挙するファイル数 F に対し O(B + F)
 * @throws std::runtime_error テストケースの保存に失敗した場合
 */
static void ChangeBytesAtPublish() {
	// 元の実体を保持した本文だけの外部更新
	Save(HookTarget, "changed body");
	// 終了
	return;
}

/**
 * 公開直前の日時変更再現関数
 * 計算量：テストケースのテキストとパスの総長 B，列挙するファイル数 F に対し O(B + F)
 * @throws std::filesystem::filesystem_error 更新日時を変更出来ない場合
 */
static void ChangeTimestampAtPublish() {
	// 本文の不変な原本の更新日時を２秒前へ変更する
	Filesystem::last_write_time(HookTarget, Filesystem::last_write_time(HookTarget) - std::chrono::seconds(2));
	// 終了
	return;
}

/**
 * 復旧中の公開名差替再現関数
 * 計算量：テストケースのテキストとパスの総長 B，列挙するファイル数 F に対し O(B + F)
 * @throws std::filesystem::filesystem_error 実体の移動に失敗した場合
 * @throws std::runtime_error 呼出先の検査又は入出力が失敗した場合
 */
static void ReplaceTargetDuringRecovery() {
	// 差替前の実体も別名に残し，第３の実体の公開名へ移動
	Replace(HookTarget, HookHeld);
	Replace(HookReplacement, HookTarget);
	// 終了
	return;
}

/**
 * 属性競合と復旧直前の再競合の予約関数
 * 計算量：テストケースのテキストとパスの総長 B，列挙するファイル数 F に対し O(B + F)
 * @throws std::runtime_error 属性を変更出来ない場合
 */
static void ChangeAttributeAndScheduleRecoveryConflict() {
	// 属性競合と復旧直前の再競合の予約
	ChangeAttributeAtPublish();
	FileIO::SetTestHook(ReplaceTargetDuringRecovery);
	// 終了
	return;
}

/**
 * 初回交換の後で復旧直後の再競合を予約する関数
 * 計算量：テストケースのテキストとパスの総長 B，列挙するファイル数 F に対し O(B + F)
 */
static void ScheduleConflictAfterRecovery() {
	// 既存の１度限りの鉤を，次の名前変更へ設定する
	FileIO::SetAfterTestHook(ReplaceTargetDuringRecovery);
	// 終了
	return;
}

#if defined(__APPLE__)

/**
 * 公開直前の macOS のフラグ変更再現関数
 * 計算量：テストケースのテキストとパスの総長 B，列挙するファイル数 F に対し O(B + F)
 * @throws std::runtime_error フラグを取得又は変更出来ない場合
 */
static void ChangeFlagsAtPublish() {
	// 更新前のファイルの付帯属性の格納準備
	struct stat Metadata {};
	Check(!::stat(HookTarget.c_str(), &Metadata), "publication flag read failed");
	Check(!::chflags(HookTarget.c_str(), Metadata.st_flags ^ UF_NODUMP), "publication flag update failed");
	// 終了
	return;
}

#endif

/**
 * 拡張属性取得関数
 * 計算量：属性の値長 B に対し O(B)
 * @param Path 対象ファイル
 * @param Name 属性名
 * @return 属性値
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::runtime_error 呼出先の検査又は入出力が失敗した場合
 */
static std::string LoadAttribute(const Filesystem::path &Path, const char *const Name) {
	// 属性値の必要量取得と受取領域の確保
	#if defined(__APPLE__)
	const ssize_t Size = ::getxattr(Path.c_str(), Name, nullptr, 0, 0, 0);
	#else
	const ssize_t Size = ::getxattr(Path.c_str(), Name, nullptr, 0);
	#endif
	Check(Size > -1, "attribute size read failed");
	std::string Val(static_cast<size_t>(Size), '\0');
	// 読込量との一致に依る取得間寸法変化の失敗化
	#if defined(__APPLE__)
	const ssize_t Read = ::getxattr(Path.c_str(), Name, Val.data(), Val.size(), 0, 0);
	#else
	const ssize_t Read = ::getxattr(Path.c_str(), Name, Val.data(), Val.size());
	#endif
	Check(Read == Size, "attribute value read failed");
	// 属性値の返戻
	return Val;
}

#endif

#if defined(__linux__)

/**
 * リトルエンディアン１６ビット値追加関数
 * 計算量：O(1)
 * @param Val 値
 * @param Bytes 格納先
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
static void AppendLe16(const uint16_t Val, std::string &Bytes) {
	// ACL 外部表現に合わせた下位バイトからの格納
	Bytes.push_back(static_cast<char>(Val));
	Bytes.push_back(static_cast<char>(Val >> 8)); // １バイトのビット数
	// 終了
	return;
}

/**
 * リトルエンディアン３２ビット値追加関数
 * 計算量：O(1)
 * @param Val 値
 * @param Bytes 格納先
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
static void AppendLe32(const uint32_t Val, std::string &Bytes) {
	// ホストのエンディアンに依存しない４バイトシリアライズ
	for(uint32_t Shift = 0; Shift < 32; Shift += 8) Bytes.push_back(static_cast<char>(Val >> Shift)); // ３２ビットを８ビット単位でシリアライズ
	// 終了
	return;
}

/**
 * POSIX ACL 項目追加関数
 * 計算量：O(1)
 * @param Tag 項目種別
 * @param Permission 権限
 * @param Qualifier 利用者又は集団識別子
 * @param Bytes 格納先
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
static void AppendAclEntry(const uint16_t Tag, const uint16_t Permission, const uint32_t Qualifier, std::string &Bytes) {
	// カーネルの項目順に従う種別・権限・識別子の連結
	AppendLe16(Tag, Bytes);
	AppendLe16(Permission, Bytes);
	AppendLe32(Qualifier, Bytes);
	// 終了
	return;
}

/**
 * 既定 ACL と競合用アクセス ACL の外部表現の生成関数
 * 計算量：固定の５項目に対し O(1)
 * @return 所有者・指定利用者・集団・マスク・其の他のアクセス制御表
 * @throws std::bad_alloc 外部表現の領域を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
static std::string MakeAcl() {
	// テストケースのアクセス制御情報の構築準備
	std::string Acl;
	AppendLe32(2, Acl); // Linux のアクセス制御表バージョン
	// 所有者・指定利用者・集団・マスク・其の他の最小構成生成
	AppendAclEntry(0X01, 7, UINT32_MAX, Acl); // 所有者の全権限
	AppendAclEntry(0X02, 4, 65534, Acl); // 指定利用者の読込権限
	AppendAclEntry(0X04, 0, UINT32_MAX, Acl); // 所有集団の権限無
	AppendAclEntry(0X10, 4, UINT32_MAX, Acl); // 指定項目へ掛ける読込マスク
	AppendAclEntry(0X20, 0, UINT32_MAX, Acl); // 其の他の権限無
	// アクセス制御表の返戻
	return Acl;
}

/**
 * 既定 ACL 設定関数
 * 計算量：テストケースのテキストとパスの総長 B，列挙するファイル数 F に対し O(B + F)
 * @param Path 対象ディレクトリ
 * @throws std::runtime_error 既定 ACL を設定出来ない場合
 * @throws std::bad_alloc 外部表現の領域を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
static void SetDefaultAcl(const Filesystem::path &Path) {
	const std::string Acl = MakeAcl();
	Check(!::setxattr(Path.c_str(), "system.posix_acl_default", Acl.data(), Acl.size(), 0), "default ACL setup failed");
	// 終了
	return;
}

/**
 * 公開直前のアクセス ACL 変更再現関数
 * 計算量：テストケースのテキストとパスの総長 B，列挙するファイル数 F に対し O(B + F)
 * @throws std::runtime_error アクセス ACL を設定出来ない場合
 * @throws std::bad_alloc 外部表現の領域を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
static void ChangeAclAtPublish() {
	const std::string Acl = MakeAcl();
	Check(!::setxattr(HookTarget.c_str(), "system.posix_acl_access", Acl.data(), Acl.size(), 0), "access ACL update failed");
	// 終了
	return;
}

/**
 * 公開直前の Linux のフラグ変更再現関数
 * 計算量：テストケースのテキストとパスの総長 B，列挙するファイル数 F に対し O(B + F)
 * @throws std::runtime_error フラグを変更出来ない場合
 */
static void ChangeFlagsAtPublish() {
	// テストケースのファイルの記述子の取得
	const int Descriptor = ::open(HookTarget.c_str(), O_RDONLY | O_CLOEXEC);
	Check(Descriptor > -1, "cannot open publication flag fixture");
	int Flags = 0;
	const bool IsRead = !::ioctl(Descriptor, FS_IOC_GETFLAGS, &Flags);
	Flags ^= FS_NODUMP_FL;
	const bool HasChanged = IsRead && !::ioctl(Descriptor, FS_IOC_SETFLAGS, &Flags);
	Check(!::close(Descriptor) && HasChanged, "publication flag update failed");
	// 終了
	return;
}

#endif

/**
 * 公開直前の一時名差替再現関数
 * 計算量：保存先ディレクトリの項目数 N に対し O(N)
 * @param IsOversized 上限超過の一時実体へ差し替える場合は true
 * @param ForeignContent 差替後の外部実体の本文
 * @throws std::runtime_error 公開前の一時ファイルの名前が見付からない場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
static void ReplaceTemp(const bool IsOversized, const std::string &ForeignContent) {
	// 保存先と同じディレクトリに在る公開待ち一時名の探索
	for(const Filesystem::directory_entry &Entry : Filesystem::directory_iterator(HookTarget.parent_path())) {
		if(const std::string Name = Entry.path().filename().string(); !Name.starts_with(".shavefmt-") || !Name.ends_with(".tmp")) {
			continue;
		}
		// 整形済実体の退避と外部実体への差替及び必要時の寸法拡張
		Replace(Entry.path(), HookHeld);
		Save(Entry.path(), ForeignContent);
		if(IsOversized) Filesystem::resize_file(Entry.path(), MaxInputBytes + 1);
		// 一時名を差し替えた場合の返戻
		return;
	}
	// 一時名探索失敗の送出
	throw std::runtime_error("temporary publication name not found");
}

/**
 * 公開直前の一時名差替再現関数
 * 計算量：テストケースのテキストとパスの総長 B，列挙するファイル数 F に対し O(B + F)
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::runtime_error 呼出先の検査又は入出力が失敗した場合
 */
static void ReplaceTempAtPublish() {
	// 通常寸法の外部実体への交換
	ReplaceTemp(false, "foreign temporary content");
	// 終了
	return;
}

/**
 * 公開直前の同寸法一時名差替再現関数
 * 計算量：保存先ディレクトリの項目数 N に対し O(N)
 * @throws std::runtime_error 公開前の一時名が見付からない場合
 * @throws std::filesystem::filesystem_error 一時実体の移動に失敗した場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
static void ReplaceSameSizeTempAtPublish() {
	// 候補と同じ寸法の別実体への交換
	ReplaceTemp(false, "foreign result!!");
	// 終了
	return;
}

/**
 * 公開直前の上限超過一時名差替再現関数
 * 計算量：テストケースのテキストとパスの総長 B，列挙するファイル数 F に対し O(B + F)
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::runtime_error 呼出先の検査又は入出力が失敗した場合
 */
static void ReplaceOversizedTempAtPublish() {
	// 上限超過の外部実体への交換
	ReplaceTemp(true, "");
	// 終了
	return;
}

#if !defined(_WIN32)

/**
 * 復旧直後の一時名差替再現関数
 * 計算量：保存先ディレクトリの項目数 N に対し O(N)
 * @throws std::runtime_error 一時名の探索又はテストケースの保存に失敗した場合
 * @throws std::filesystem::filesystem_error 実体の移動に失敗した場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
static void ReplaceTempDuringRecovery() {
	// 復旧直後の一時名差替再現
	ReplaceTemp(false, "recovery temporary external");
	// 終了
	return;
}

/**
 * 属性競合と復旧直前の一時名の再競合の予約関数
 * 計算量：テストケースのテキストとパスの総長 B，列挙するファイル数 F に対し O(B + F)
 * @throws std::runtime_error 属性を変更出来ない場合
 */
static void ChangeAttributeAndScheduleTempConflict() {
	// 属性競合と復旧直前の一時名の再競合の予約
	ChangeAttributeAtPublish();
	FileIO::SetTestHook(ReplaceTempDuringRecovery);
	// 終了
	return;
}

/**
 * 初回交換の後で復旧直後の一時名の再競合を予約する関数
 * 計算量：テストケースのテキストとパスの総長 B，列挙するファイル数 F に対し O(B + F)
 */
static void ScheduleTempConflictAfterRecovery() {
	// 初回交換の後で復旧直後の一時名の再競合を予約する
	FileIO::SetAfterTestHook(ReplaceTempDuringRecovery);
	// 終了
	return;
}

#endif

/**
 * 競合時に保全された一時リソースの除去関数
 * 計算量：保存先ディレクトリの項目数 N に対し O(N)
 * @param Path 対象ファイル
 * @param Expected 保全された内容
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::runtime_error 呼出先の検査又は入出力が失敗した場合
 */
static void RemovePreservedFiles(const Filesystem::path &Path, const std::string &Expected) {
	// 保全リソースの探索・検査・除去数
	size_t Removed = 0;
	for(const Filesystem::directory_entry &Entry : Filesystem::directory_iterator(Path.parent_path())) {
		if(!Entry.path().filename().string().starts_with(".shavefmt-")) continue;
		Check(Load(Entry.path()) == Expected, "preserved file bytes differ");
		Check(Filesystem::remove(Entry.path()), "preserved file cleanup failed");
		++Removed;
	}
	// 個数に依る保全リソースの欠落・重複検出
	Check(Removed == 1, "preserved file count differs");
	// 終了
	return;
}

/**
 * 確認付の復旧後の一時リソースの不在検査関数
 * 計算量：保存先ディレクトリの項目数 N に対し O(N)
 * @param Path 対象ファイル
 * @throws std::runtime_error フォーマッターの一時リソースが残って居る場合
 * @throws std::filesystem::filesystem_error 項目の列挙に失敗した場合
 * @throws std::bad_alloc 項目の名前の領域を確保出来ない場合
 */
static void CheckNoPreservedFiles(const Filesystem::path &Path) {
	// 他のテストケースへ残留の持ち越さず，此の復旧の直後に確認
	for(const Filesystem::directory_entry &Entry : Filesystem::directory_iterator(Path.parent_path())) {
		Check(!Entry.path().filename().string().starts_with(".shavefmt-"), "confirmed recovery left a temporary file");
	}
	// 終了
	return;
}

/**
 * 上限超過で保全された一時リソースの除去関数
 * 計算量：保存先ディレクトリの項目数 N に対し O(N)
 * @param Path 対象ファイル
 * @throws std::runtime_error 呼出先の検査又は入出力が失敗した場合
 */
static void RemoveOversizedPreservedFile(const Filesystem::path &Path) {
	// 上限超過リソースの探索・検査・除去数
	size_t Removed = 0;
	for(const Filesystem::directory_entry &Entry : Filesystem::directory_iterator(Path.parent_path())) {
		if(!Entry.path().filename().string().starts_with(".shavefmt-")) continue;
		Check(Filesystem::file_size(Entry.path()) == MaxInputBytes + 1, "oversized preserved file size differs");
		Check(Filesystem::remove(Entry.path()), "oversized preserved file cleanup failed");
		++Removed;
	}
	// 単一保全の確認
	Check(Removed == 1, "oversized preserved file count differs");
	// 終了
	return;
}

/**
 * 外部保存後の競合検査関数
 * 計算量：原本と候補の総長 B，パス長 P に対し O(B + P)
 * @param Path 保存先
 * @param Original 読込時の内容
 * @param Updated 外部保存の内容
 * @param IsRestoreTime 元の更新日時に戻す場合は true
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::filesystem::filesystem_error パスの変換又は対象の列挙に失敗した場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::runtime_error 呼出先の検査又は入出力が失敗した場合
 */
static void CheckConflict(
	const Filesystem::path &Path,
	const std::string &Original,
	const std::string &Updated,
	const bool IsRestoreTime
) {
	// 読込時の実体と状態の記録
	Save(Path, Original);
	const Filesystem::file_time_type OriginalTime = Filesystem::last_write_time(Path);
	const FileIO File(Path.string());
	std::string Src;
	Check(File.IsFileReadSuccessful(Src), "read before external save failed");
	// 外部保存と必要時の更新日時復元に依る競合生成
	Save(Path, Updated);
	if(IsRestoreTime) Filesystem::last_write_time(Path, OriginalTime);
	// 古い整形結果の拒否と外部保存内容の保全の個別確認
	Check(!File.IsFileWriteSuccessful("stale result"), "external save was overwritten");
	Check(Load(Path) == Updated, "external bytes changed");
	// 再読込後の新しいスナップショットへの更新と通常保存への復帰
	Check(File.IsFileReadSuccessful(Src) && File.IsFileWriteSuccessful("fresh result"), "reread after conflict did not recover");
	Check(Load(Path) == "fresh result", "fresh save bytes differ");
	// 終了
	return;
}

#if defined(__linux__)

/**
 * Linux の付帯情報を保てない書戻の拒否の検査関数
 * 計算量：テストケース長 B に対し O(B)
 * @param Path 通常ファイルのテストケース
 * @throws std::runtime_error nodump が失われるか，原文が書き換わる場合
 * @throws std::bad_alloc 読込又はパスの領域を確保出来ない場合
 * @throws std::filesystem::filesystem_error パスを操作出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
static void CheckLinuxFlags(const Filesystem::path &Path) {
	// Linux の付帯情報を保てない書戻の拒否の検査
	Save(Path, "original flags");
	int Descriptor = ::open(Path.c_str(), O_RDONLY | O_CLOEXEC);
	Check(Descriptor > -1, "cannot open flag fixture");
	// テストケースのファイルのフラグの復旧
	struct FlagGuard {
		bool HasChanged = false;
		int Descriptor;
		int Original = 0;

		/**
		 * 付けたフラグを戻し記述子を閉じるデストラクタ
		 * 計算量：テストケースのテキストとパスの総長 B，列挙するファイル数 F に対し O(B + F)
		 */
		~FlagGuard() {
			// 付けたフラグを戻し記述子を閉じるデストラクタ
			if(HasChanged) ::ioctl(Descriptor, FS_IOC_SETFLAGS, &Original);
			::close(Descriptor);
			// 終了
			return;
		}
	} Guard { .Descriptor = Descriptor };
	if(::ioctl(Descriptor, FS_IOC_GETFLAGS, &Guard.Original)) {
		Check(errno == ENOTTY || errno == EOPNOTSUPP, "cannot read inode flags");
		std::puts("Linux inode flag fixture skipped: filesystem has no inode flags");
		// フラグを持たないファイルシステムのテストケースの終了
		return;
	}
	int Flags = Guard.Original | FS_NODUMP_FL;
	if(::ioctl(Descriptor, FS_IOC_SETFLAGS, &Flags)) {
		Check(errno == ENOTTY || errno == EOPNOTSUPP, "cannot set nodump fixture");
		std::puts("Linux inode flag fixture skipped: filesystem cannot set inode flags");
		// フラグを変更出来ないファイルシステムのテストケースの終了
		return;
	}
	Guard.HasChanged = true;
	const FileIO File(Path.string());
	std::string Src;
	Check(File.IsFileReadSuccessful(Src), "nodump file was not readable");
	std::error_code Error;
	Check(File.IsFileWriteSuccessful("formatted flags", nullptr, &Error), "nodump replacement was rejected");
	Check(!Error && Load(Path) == "formatted flags", "flag preservation changed content or reason");
	const int Published = ::open(Path.c_str(), O_RDONLY | O_CLOEXEC);
	Check(Published > -1, "cannot open published flag fixture");
	Check(!::ioctl(Published, FS_IOC_GETFLAGS, &Flags) && Flags & FS_NODUMP_FL, "replacement cleared nodump");
	const int PublishedOriginal = Flags & ~FS_NODUMP_FL | Guard.Original & FS_NODUMP_FL;
	Check(!::ioctl(Published, FS_IOC_SETFLAGS, &PublishedOriginal), "cannot restore published fixture flags");
	Check(!::ioctl(Descriptor, FS_IOC_SETFLAGS, &Guard.Original), "cannot restore fixture flags");
	Check(!::close(Descriptor), "cannot close replaced fixture handle");
	Descriptor = Published;
	Guard.Descriptor = Published;
	Guard.Original = PublishedOriginal;
	Guard.HasChanged = false;
	// 交換出来るフラグの公開直前更新は，交換後の照合から復旧する
	Save(Path, "original flags");
	const FileIO RecoverableFile(Path.string());
	Check(RecoverableFile.IsFileReadSuccessful(Src), "read before publication flag conflict failed");
	HookTarget = Path;
	FileIO::SetTestHook(ChangeFlagsAtPublish);
	Guard.HasChanged = true;
	Check(!RecoverableFile.IsFileWriteSuccessful("formatted flags", nullptr, &Error), "publication flag conflict succeeded");
	Check(!Error && Load(Path) == "original flags", "flag recovery changed original or reason");
	Check(
		!::ioctl(Descriptor, FS_IOC_GETFLAGS, &Flags) && Flags == (Guard.Original ^ FS_NODUMP_FL),
		"flag recovery lost external flags"
	);
	CheckNoPreservedFiles(Path);
	Check(!::ioctl(Descriptor, FS_IOC_SETFLAGS, &Guard.Original), "cannot restore recovered fixture flags");
	Guard.HasChanged = false;
	// 終了
	return;
}

#endif

/**
 * ファイル保存の回帰検査関数
 * 計算量：呼び出す処理・検査の総費用 Q に対し O(Q)
 * @return 全検査成功時は０，失敗時は１
 */
int main() {
	// 全検査の例外境界
	try {
		// 全テストケースを隔離する一時領域
		const TestDir Dir;
		// 全パスでの保存先再利用に依る読込状態の消費・再取得検査
		const Filesystem::path Path = Dir.Path / "source.cpp", Replacement = Dir.Path / "replacement.cpp";
		#if defined(__linux__)
		CheckLinuxFlags(Path);
		#endif
		{
			const FileIO Missing((Dir.Path / "missing.cpp").string());
			std::string Src;
			Check(!Missing.IsFileReadSuccessful(Src), "missing file was reported as read");
			Check(Missing.GetReadError() == std::errc::no_such_file_or_directory, "read failure lost its native cause");
		}
		#if defined(_WIN32)
		{
			// レジストリ設定を前提にせず，長いパスの読込・公開・後始末を通す
			FileIO::SetUnsupportedIdentity(true);
			{
				Save(Path, "readonly original");
				const FileIO Readonly(FileIO::GetPathText(Path));
				std::string Text;
				Check(Readonly.GetNormalizedPath().has_value(), "unsupported identity prevented path normalization");
				Check(
					!Readonly.GetPathIdentity().empty() && Readonly.GetReadError() == std::errc::operation_not_supported,
					"unknown identity was not marked"
				);
				Check(Readonly.IsFileReadSuccessful(Text) && Text == "readonly original", "unsupported identity prevented read");
				std::error_code Error;
				Check(!Readonly.IsFileWriteSuccessful("replacement", nullptr, &Error), "unsupported identity allowed write");
				Check(Error.value() == ERROR_NOT_SUPPORTED && Load(Path) == "readonly original", "identity rejection changed original");
				CheckNoPreservedFiles(Path);
			}
			FileIO::SetUnsupportedIdentity(false);
			const Filesystem::path InvalidName(std::wstring(1, static_cast<wchar_t>(0XD800)) + L".cpp");
			Check(FileIO::GetPathDisplay(InvalidName) == "\\uD800.cpp", "unpaired surrogate display was ambiguous");
			Filesystem::path Deep = Dir.Path;
			for(size_t Idx = 0; Idx < 4; ++Idx) Deep /= std::string(80, 'x');
			Check(Filesystem::create_directories(FileIO::GetNativePath(Deep)), "long directory could not be created");
			const Filesystem::path LongFile = Deep / "source.cpp";
			Save(FileIO::GetNativePath(LongFile), "long original");
			{
				const FileIO File(FileIO::GetPathText(LongFile));
				std::string Src;
				Check(File.IsFileReadSuccessful(Src) && Src == "long original", "long file could not be read");
				Check(File.IsFileWriteSuccessful("long formatted"), "long file could not be published");
				Check(Load(FileIO::GetNativePath(LongFile)) == "long formatted", "long file publication lost bytes");
			}
			size_t Found = 0;
			for(
				const Filesystem::directory_entry &Entry :
				Filesystem::recursive_directory_iterator(FileIO::GetNativePath(Dir.Path, true))
			) if(Entry.path() == FileIO::GetNativePath(LongFile, true)) ++Found;
			Check(Found == 1, "short scan root could not enumerate long descendants");
			Check(Filesystem::remove(FileIO::GetNativePath(LongFile)), "long file cleanup failed");
			while(Deep != Dir.Path) {
				Check(Filesystem::remove(FileIO::GetNativePath(Deep)), "long directory cleanup failed");
				Deep = Deep.parent_path();
			}
		}
		{
			// システムの通知末尾を，表示用の疑問符へ変える前に除く
			const std::string Text = FileIO::GetNativeErrorText(std::error_code(ERROR_ACCESS_DENIED, std::system_category()));
			Check(!Text.empty() && Text.back() != '?' && Text.back() != ' ', "native error kept its trailing line break or space");
		}
		{
			// 下位６４ビットが同じでも，上位６４ビットの異なる実体を拒む
			Save(Path, "identity original");
			std::vector<FileIdentity> Identity = FileIO(Path.string()).GetPathIdentity();
			Check(!Identity.empty(), "full identity could not be collected");
			Identity.back()[2] ^= 1;
			const FileIO File(Path.string(), Identity);
			std::string Src;
			Check(!File.IsFileReadSuccessful(Src), "different high file identity was accepted");
			Check(Load(Path) == "identity original", "identity rejection changed the original");
		}
		{
			// 読込後の共有違反を，競合の固定文で隠さず書込失敗理由へ渡す
			Save(Path, "locked original");
			const FileIO File(Path.string());
			std::string Src;
			Check(File.IsFileReadSuccessful(Src), "read before sharing conflict failed");
			const HANDLE Locked = CreateFileW(Path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
			Check(Locked != INVALID_HANDLE_VALUE, "sharing lock could not be acquired");
			std::error_code Error;
			const bool IsWritten = File.IsFileWriteSuccessful("unpublished candidate", nullptr, &Error), IsClosed = CloseHandle(Locked);
			Check(!IsWritten && IsClosed, "sharing conflict was overwritten or leaked a handle");
			Check(Error == std::error_code(ERROR_SHARING_VIOLATION, std::system_category()), "write failure lost its native cause");
			Check(Load(Path) == "locked original", "sharing failure changed original bytes");
		}
		#endif
		{
			// 収集した対象名を別の実体へ交換してから読む場合の拒否
			Save(Path, "collected original");
			const std::vector<FileIdentity> Identity = FileIO(Path.string()).GetPathIdentity();
			Check(!Identity.empty(), "collection identity was not recorded");
			Filesystem::rename(Path, Replacement);
			Save(Path, "replacement after collection");
			const FileIO Collected(Path.string(), Identity);
			std::string Src;
			Check(!Collected.IsFileReadSuccessful(Src), "replacement after collection was read");
			Check(Collected.GetReadError() == std::errc::state_not_recoverable, "collection replacement lost its cause");
			Check(Load(Path) == "replacement after collection", "collection replacement was modified");
			Check(Filesystem::remove(Replacement), "collected original cleanup failed");
		}
		{
			// 親の改名と同名の新しい親の作成の，収集時の親の実体と照合
			const Filesystem::path Parent = Dir.Path / "collected", Moved = Dir.Path / "moved";
			Check(Filesystem::create_directory(Parent), "collection parent creation failed");
			const Filesystem::path Child = Parent / "child.cpp", MovedChild = Moved / "child.cpp";
			Save(Child, "original in collected parent");
			const std::vector<FileIdentity> Identity = FileIO(Child.string()).GetPathIdentity();
			Filesystem::rename(Parent, Moved);
			Check(Filesystem::create_directory(Parent), "replacement parent creation failed");
			Save(Child, "foreign parent content");
			{
				// Windows の親の保持も検査の終了で解除する
				const FileIO Collected(Child.string(), Identity);
				std::string Src;
				Check(!Collected.IsFileReadSuccessful(Src), "replacement parent was followed");
			}
			Check(Load(Child) == "foreign parent content", "replacement parent content changed");
			Check(Load(MovedChild) == "original in collected parent", "moved parent content changed");
			// 検査した２つの実体と空にした親だけを此のテストケース内で除く
			Check(
				Filesystem::remove(Child) && Filesystem::remove(Parent) && Filesystem::remove(MovedChild) && Filesystem::remove(Moved),
				"parent replacement fixture cleanup failed"
			);
		}
		{
			// 保持せず読んだ受理データは，本文を取得する時の更新状態と共有する
			Save(Path, "inspected source");
			const FileIO Collected(Path.string());
			// 原文の作業域の見積
			const FileIO::InputEstimator Estimate = [](const size_t Size, const FileIO::InputReader &Read) -> size_t {
				// テストケースの読込領域の準備
				std::array<char, 7> Buf {};
				for(size_t Pos = 0; Pos < Size;) {
					const size_t Count = Read(Pos, std::span<char>(Buf).first(std::min(Buf.size(), Size - Pos)));
					Check(Count, "inspection reader stopped before EOF");
					Pos += Count;
				}
				// 此の入出力検査が読み取った寸法の返戻
				return Size;
			};
			const std::optional<FileIO::InputInspection> Inspection = Collected.InspectInput(Estimate);
			Check(Inspection && Inspection->Bytes == 16 && Inspection->WorkingBytes == 16, "inspection bytes differ");
			const FileIO Accepted(Path.string(), Collected.GetPathIdentity(), Inspection);
			std::string Src;
			Check(Accepted.IsFileReadSuccessful(Src, false) && Src == "inspected source", "accepted bytes were not shared");
			Save(Path, "changed after inspection");
			const FileIO Changed(Path.string(), Collected.GetPathIdentity(), Inspection);
			Check(!Changed.IsFileReadSuccessful(Src), "changed inspected bytes were accepted");
		}
		{
			// 別名毎の安定読取を保ちながら，同じ原文の受理計数を１度だけ共有する
			Save(Path, "linked inspection");
			std::vector<Filesystem::path> Aliases;
			for(size_t Idx = 0; Idx < 16; ++Idx) {
				const Filesystem::path Alias = Dir.Path / ("inspected-alias-" + std::to_string(Idx) + ".cpp");
				Filesystem::create_hard_link(Path, Alias);
				Aliases.push_back(Alias);
			}
			size_t Counted = 0;
			const FileIO First(Path.string());
			// 共有される見積の呼出数と読込の確認
			const FileIO::InputEstimator Estimate = [&](const size_t Size, const FileIO::InputReader &Read) -> size_t {
				++Counted;
				std::array<char, 32> Buffer;
				Check(Read(0, std::span<char>(Buffer).first(Size)) == Size, "linked inspection could not read the source");
				// 手で定めた原文の寸法の返戻
				return Size;
			};
			const std::optional<FileIO::InputInspection> Inspection = First.InspectInput(Estimate);
			Check(Inspection && Inspection->Bytes == 17, "linked inspection has the wrong source size");
			for(size_t Count = 2; Count < 17; Count *= 2) {
				for(const Filesystem::path &Alias : std::span<const Filesystem::path>(Aliases).first(Count)) {
					const FileIO Collected(Alias.string());
					Check(Collected.GetIdentity() == First.GetIdentity(), "hard-link identity changed between aliases");
					const std::optional<FileIO::InputInspection> Shared = Collected.InspectInput(Estimate, &*Inspection);
					Check(Shared && Shared->WorkingBytes == 17, "an alias lost the shared admission count");
					const FileIO Accepted(Alias.string(), Collected.GetPathIdentity(), Shared);
					std::string Src;
					Check(Accepted.IsFileReadSuccessful(Src, false) && Src == "linked inspection", "an alias lost the shared source");
				}
				Check(Counted == 1, "shared inspection was recalculated for each alias");
			}
			// 寸法と更新日時を戻しても，変更日時の異なる計数は再利用しない
			// 変更日時は時刻の刻み毎に進む為，刻みが変わる迄書換を繰返す
			const Filesystem::file_time_type Modified = Filesystem::last_write_time(Path);
			std::optional<FileIO::InputInspection> Revised;
			for(
				const std::chrono::steady_clock::time_point Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
				Counted == 1 && std::chrono::steady_clock::now() < Deadline;
			) {
				Save(Path, "external revision");
				Filesystem::last_write_time(Path, Modified);
				Revised = First.InspectInput(Estimate, &*Inspection);
			}
			Check(Revised && Revised->WorkingBytes == 17 && Counted == 2, "changed state did not recount admission");
			// 上限超過の計数も確認済の状態で共有し，拒否の判定を変えない
			const FileIO::InputEstimator Reject = [&](const size_t, const FileIO::InputReader &) -> size_t {
				++Counted;
				// 受理出来ない作業域の見積の返戻
				return MaxWorkingBytes + 1;
			};
			const std::optional<FileIO::InputInspection> Rejected = First.InspectInput(Reject);
			Check(Rejected && Counted == 3, "rejected input was not inspected once");
			for(const Filesystem::path &Alias : Aliases) {
				const FileIO Collected(Alias.string());
				const std::optional<FileIO::InputInspection> Shared = Collected.InspectInput(Reject, &*Rejected);
				Check(Shared && Shared->WorkingBytes == MaxWorkingBytes + 1 && Counted == 3, "rejected aliases repeated admission");
				const FileIO Changed(Alias.string(), Collected.GetPathIdentity(), Inspection);
				std::string Src;
				Check(!Changed.IsFileReadSuccessful(Src, false), "changed source reused an old admission result");
			}
			// 別名の削除で実体の状態が変われば，拒否の計数も失効する
			Check(Filesystem::remove(Aliases.front()), "linked inspection cleanup failed");
			const std::optional<FileIO::InputInspection> Unlinked = First.InspectInput(Reject, &*Rejected);
			Check(Unlinked && Unlinked->WorkingBytes == MaxWorkingBytes + 1 && Counted == 4, "removed alias retained admission");
			for(size_t Idx = 1; Idx < Aliases.size(); ++Idx) {
				const FileIO Collected(Aliases[Idx].string());
				const std::optional<FileIO::InputInspection> Shared = Collected.InspectInput(Reject, &*Unlinked);
				Check(Shared && Shared->WorkingBytes == MaxWorkingBytes + 1 && Counted == 4, "revised aliases repeated admission");
			}
			for(size_t Idx = 1; Idx < Aliases.size(); ++Idx) Check(Filesystem::remove(Aliases[Idx]), "linked inspection cleanup failed");
			// 同じ寸法・本文でも，別の実体の計数は共有しない
			Save(Replacement, "external revision");
			const FileIO Other(Replacement.string());
			const std::optional<FileIO::InputInspection> Separate = Other.InspectInput(Estimate, &*Revised);
			Check(Separate && Separate->WorkingBytes == 17 && Counted == 5, "different entities shared admission");
			Check(Filesystem::remove(Replacement), "separate inspection cleanup failed");
		}
		// 空入力・分割読込境界・NUL・BOM・CRLF の通常経路（読込は原本のバイト列を其の儘返し，BOM はフォーマッターの入力の正規化が除く）
		// 空入力から境界跨ぎを経て BOM 付入力へ進め，同じ保存先の読込状態を順に確かめる
		// 零バイト入力と読込失敗の区別
		// 内部読込ブロックの境界越しの NUL
		// 先頭の BOM と CRLF
		const std::string ReadCases[] = { "", std::string(65537, '\0'), "\xEF\xBB\xBF\xEF\xBB\xBF" "line\r\n" };
		for(const std::string &Original : ReadCases) {
			// 各テストケースを同じパスへ新規保存し，前のテストケースの状態を残さない
			Save(Path, Original);
			const FileIO File(Path.string());
			std::string Src;
			Check(File.IsFileReadSuccessful(Src), "normal read failed");
			Check(Src == Original, "raw read contract failed");
			// 読込と同じ原本の書戻に依る無変更時の生バイト保持
			Check(File.IsFileWriteSuccessful(Original), "normal write failed");
			Check(Load(Path) == Original, "normal write changed bytes");
			// 成功した保存の読込権の消費と二重公開の拒否
			Check(!File.IsFileWriteSuccessful("stale result"), "consumed read was reused");
			Check(Load(Path) == Original, "consumed read changed bytes");
			// 明示的な再読込に依る次の保存権取得検査
			Check(File.IsFileReadSuccessful(Src) && File.IsFileWriteSuccessful(Original), "reread after save failed");
		}
		{
			// 整形で増えた出力を除く入力上限の読込保護限定
			Save(Path, "large output source");
			const FileIO File(Path.string());
			std::string Src;
			// 入力上限を越える出力
			const std::string LargeResult(MaxInputBytes + 1, 'x');
			Check(File.IsFileReadSuccessful(Src), "read before large output failed");
			// 上限を１バイト越える出力の公開境界検査
			Check(File.IsFileWriteSuccessful(LargeResult), "large permitted output save failed");
			Check(Load(Path) == LargeResult, "large permitted output bytes differ");
			Check(!File.IsFileReadSuccessful(Src), "oversized input was read");
			Check(File.GetReadError() == std::errc::file_too_large, "oversized input lost its exact cause");
			const std::optional<FileIO::InputInspection> Oversized = File.InspectInput(
				[]([[maybe_unused]] const size_t Size, [[maybe_unused]] const FileIO::InputReader &Read) -> size_t {
					// 対象の状態の更新
					throw std::runtime_error("oversized input entered the admission reader");
				}
			);
			Check(Oversized && Oversized->Bytes == MaxInputBytes + 1, "oversized preflight lost its classification");
		}
		// 空への外部切詰を含む通常更新の競合検出
		CheckConflict(Path, "original", "", false);
		// 更新日時復元後の内容差に依る競合検出
		CheckConflict(Path, "original", "external", true);
		// BOM 除去後に本文が一致する原本バイト差の検出
		CheckConflict(Path, "\xEF\xBB\xBF" "original", "original", false);
		// 比較ブロック境界より後だけが異なる外部更新の検出
		CheckConflict(Path, std::string(65536, 'x') + "a", std::string(65536, 'x') + "b", true); // 内部比較ブロックの境界
		{
			// 公開直前に保存先の実体を交換し，古い結果の上書を拒ませる
			Save(Path, "original");
			Save(Replacement, "external at publication");
			const FileIO File(Path.string());
			std::string Src;
			Check(File.IsFileReadSuccessful(Src), "read before publication conflict failed");
			HookTarget = Path;
			HookReplacement = Replacement;
			// 一時リソース準備後且つ公開前への外部差替の注入
			FileIO::SetTestHook(ReplaceTargetAtPublish);
			// 無関係なシステムエラーの受取先
			std::error_code WriteError;
			Check(!File.IsFileWriteSuccessful("stale result", nullptr, &WriteError), "publication conflict was overwritten");
			Check(!WriteError, "publication conflict reported an unrelated system error");
			Check(Load(Path) == "external at publication", "verified recovery did not restore external bytes");
			// 実体を確認した競合原本を元へ戻し，候補を製品側で除く
			CheckNoPreservedFiles(Path);
		}
		for(const bool HasExternalSave : { false, true }) {
			// 公開後の確保失敗でも，確認した候補だけを戻し，別の保存と退避原本を保持する
			Save(Path, "allocation original");
			Save(Replacement, "allocation external");
			const FileIO File(Path.string());
			std::string Src;
			Check(File.IsFileReadSuccessful(Src), "read before published allocation failure failed");
			HookTarget = Path;
			HookReplacement = Replacement;
			FileIO::SetAfterTestHook(HasExternalSave ? ReplaceTargetAndFailPublishedAllocation : FailPublishedAllocation);
			std::error_code WriteError;
			Check(!File.IsFileWriteSuccessful("allocation formatted", nullptr, &WriteError), "published allocation failure succeeded");
			Check(WriteError == std::errc::not_enough_memory, "published allocation failure lost its cause");
			Check(
				Load(Path) == (HasExternalSave ? "allocation external" : "allocation original"),
				"published allocation failure changed confirmed public bytes"
			);
			if(HasExternalSave) RemovePreservedFiles(Path, "allocation original");
			else {
				CheckNoPreservedFiles(Path);
				Check(Filesystem::remove(Replacement), "allocation external fixture cleanup failed");
			}
		}
		// 以下の付帯属性と名前交換の競合注入は POSIX に限定
		#if !defined(_WIN32)
		{
			// 公開待ち一時名の外部実体への交換に依る名前再利用の検出
			Save(Path, "original");
			const FileIO File(Path.string());
			std::string Src;
			Check(File.IsFileReadSuccessful(Src), "read before temporary replacement failed");
			HookTarget = Path;
			HookHeld = Dir.Path / "held-result.tmp";
			// 公開直前の同名交換
			FileIO::SetTestHook(ReplaceTempAtPublish);
			std::error_code WriteError;
			Check(!File.IsFileWriteSuccessful("formatted result", nullptr, &WriteError), "foreign temporary file was published");
			Check(!WriteError, "temporary replacement conflict reported an unrelated system error");
			Check(Load(Path) == "original", "temporary replacement changed original");
			// 元の一時実体に残る整形結果の識別検査
			Check(Load(HookHeld) == "formatted result", "prepared output identity changed");
			Check(Filesystem::remove(HookHeld), "held result cleanup failed");
			RemovePreservedFiles(Path, "foreign temporary content");
		}
		{
			// 寸法だけでは区別出来ない一時名の別実体への交換
			Save(Path, "original");
			const FileIO File(Path.string());
			std::string Src;
			Check(File.IsFileReadSuccessful(Src), "read before same-size temporary replacement failed");
			HookTarget = Path;
			HookHeld = Dir.Path / "held-same-size-result.tmp";
			FileIO::SetTestHook(ReplaceSameSizeTempAtPublish);
			std::error_code WriteError;
			Check(!File.IsFileWriteSuccessful("formatted result", nullptr, &WriteError), "same-size foreign temporary file was published");
			Check(!WriteError, "same-size temporary replacement reported an unrelated system error");
			Check(Load(Path) == "original", "same-size temporary replacement changed original");
			Check(Load(HookHeld) == "formatted result", "same-size prepared output identity changed");
			Check(Filesystem::remove(HookHeld), "same-size held result cleanup failed");
			RemovePreservedFiles(Path, "foreign result!!");
		}
		{
			// 入力上限を越える差替後一時実体の公開前検査
			Save(Path, "original");
			const FileIO File(Path.string());
			std::string Src;
			Check(File.IsFileReadSuccessful(Src), "read before oversized temporary replacement failed");
			HookTarget = Path;
			HookHeld = Dir.Path / "held-oversized-result.tmp";
			// 上限超過実体への交換
			FileIO::SetTestHook(ReplaceOversizedTempAtPublish);
			Check(!File.IsFileWriteSuccessful("oversized formatted"), "oversized foreign temporary file was published");
			Check(Load(Path) == "original", "oversized temporary replacement changed original");
			Check(Load(HookHeld) == "oversized formatted", "oversized prepared output identity changed");
			Check(Filesystem::remove(HookHeld), "oversized held result cleanup failed");
			// 大きな外部実体を切り詰めない保全の寸法検査
			RemoveOversizedPreservedFile(Path);
		}
		{
			// 公開成功直後の外部保存を再現し，利用者の新しい実体を差し戻さない
			Save(Path, "original before later save");
			Save(Replacement, "later external save");
			const FileIO File(Path.string());
			std::string Src;
			Check(File.IsFileReadSuccessful(Src), "read before post-publication conflict failed");
			HookTarget = Path;
			HookReplacement = Replacement;
			// 公開後照合の直前への外部差替の注入
			FileIO::SetAfterTestHook(ReplaceTargetAtPublish);
			std::error_code WriteError;
			Check(!File.IsFileWriteSuccessful("formatted before later save", nullptr, &WriteError), "post-publication conflict succeeded");
			Check(!WriteError, "post-publication conflict reported an unrelated system error");
			Check(Load(Path) == "later external save", "later external save was overwritten");
			// 公開前の原本が別名に保全され，復旧可能である事を探す
			bool IsPreserved = false;
			for(const Filesystem::directory_entry &Entry : Filesystem::directory_iterator(Dir.Path)) {
				if(Entry.path() == Path || Entry.path() == Replacement || Load(Entry.path()) != "original before later save") continue;
				// 原本の別名保全を確認
				IsPreserved = true;
				Check(Filesystem::remove(Entry.path()), "post-publication backup cleanup failed");
			}
			Check(IsPreserved, "original backup was not preserved");
		}
		{
			// 公開後の外部保存が読込上限を超えても其の名前を奪わない
			Save(Path, "original before oversized save");
			const FileIO File(Path.string());
			std::string Src;
			Check(File.IsFileReadSuccessful(Src), "read before oversized post-publication save failed");
			HookTarget = Path;
			HookReplacement = Replacement;
			FileIO::SetAfterTestHook(ReplaceOversizedTargetAtPublish);
			std::error_code WriteError;
			Check(
				!File.IsFileWriteSuccessful("formatted before oversized save", nullptr, &WriteError),
				"oversized external save succeeded"
			);
			Check(!WriteError, "oversized external save reported an unrelated system error");
			Check(Filesystem::file_size(Path) == MaxInputBytes + 1, "oversized external save was displaced");
			Check(Load(Path).starts_with("external after publication"), "oversized external save bytes changed");
			RemovePreservedFiles(Path, "original before oversized save");
		}
		{
			// 本文・日時一致時の拡張属性更新の外部変更扱い
			Save(Path, "attribute original");
			const FileIO File(Path.string());
			std::string Src;
			Check(File.IsFileReadSuccessful(Src), "read before attribute conflict failed");
			HookTarget = Path;
			// 公開直前の属性更新
			FileIO::SetTestHook(ChangeAttributeAtPublish);
			Check(!File.IsFileWriteSuccessful("attribute formatted"), "attribute conflict succeeded");
			Check(Load(Path) == "attribute original", "attribute conflict changed bytes");
			Check(
				LoadAttribute(Path, "user.shavefmt-test") == std::string(AttributeVal, sizeof(AttributeVal)),
				"attribute conflict lost latest value"
			);
			// 復旧で退避した候補の製品側の後始末の検査
			CheckNoPreservedFiles(Path);
		}
		{
			// 最後の照合の後に更新された本文を元の公開名へ戻す
			Save(Path, "body original");
			const FileIO File(Path.string());
			std::string Src;
			Check(File.IsFileReadSuccessful(Src), "read before body publication conflict failed");
			HookTarget = Path;
			FileIO::SetTestHook(ChangeBytesAtPublish);
			std::error_code WriteError;
			Check(!File.IsFileWriteSuccessful("body formatted", nullptr, &WriteError), "body publication conflict succeeded");
			Check(!WriteError && Load(Path) == "changed body", "body recovery lost the external edit");
			CheckNoPreservedFiles(Path);
		}
		{
			// 本文の変えない日時更新も同じ復旧経路で保持
			Save(Path, "timestamp original");
			const Filesystem::file_time_type ChangedTime = Filesystem::last_write_time(Path) - std::chrono::seconds(2);
			const FileIO File(Path.string());
			std::string Src;
			Check(File.IsFileReadSuccessful(Src), "read before timestamp publication conflict failed");
			HookTarget = Path;
			FileIO::SetTestHook(ChangeTimestampAtPublish);
			std::error_code WriteError;
			Check(!File.IsFileWriteSuccessful("timestamp formatted", nullptr, &WriteError), "timestamp publication conflict succeeded");
			Check(!WriteError && Load(Path) == "timestamp original", "timestamp recovery changed original");
			Check(Filesystem::last_write_time(Path) == ChangedTime, "timestamp recovery lost external timestamp");
			CheckNoPreservedFiles(Path);
		}
		#if defined(__APPLE__)
		{
			// 交換を妨げないフラグの更新を，原本の実体と共に戻す
			Save(Path, "flags original");
			struct stat Metadata {};
			Check(!::stat(Path.c_str(), &Metadata), "flag fixture read failed");
			const FileIO File(Path.string());
			std::string Src;
			Check(File.IsFileReadSuccessful(Src), "read before flag publication conflict failed");
			HookTarget = Path;
			FileIO::SetTestHook(ChangeFlagsAtPublish);
			std::error_code WriteError;
			Check(!File.IsFileWriteSuccessful("flags formatted", nullptr, &WriteError), "flag publication conflict succeeded");
			Check(!WriteError && Load(Path) == "flags original", "flag recovery changed original or reason");
			struct stat Recovered {};
			Check(
				!::stat(Path.c_str(), &Recovered) && Recovered.st_flags == (Metadata.st_flags ^ UF_NODUMP),
				"flag recovery lost external flags"
			);
			CheckNoPreservedFiles(Path);
			Check(!::chflags(Path.c_str(), Metadata.st_flags), "flag fixture cleanup failed");
		}
		#endif
		const bool IsRecoveryPhases[] = { false, true };
		for(const bool IsAfterRecovery : IsRecoveryPhases) {
			// 復旧直前と直後に第３の実体を注入し，再交換で上書きしない事を確かめる
			// 前のテストケースの拡張属性を継承しない新規実体の生成
			Check(Filesystem::remove(Path), "recovery fixture reset failed");
			Save(Path, "recovery original");
			Save(Replacement, "recovery external");
			const FileIO File(Path.string());
			std::string Src;
			Check(File.IsFileReadSuccessful(Src), "read before recovery race failed");
			HookTarget = Path;
			HookReplacement = Replacement;
			HookHeld = Dir.Path / "held-recovery.tmp";
			if(IsAfterRecovery) {
				FileIO::SetTestHook(ChangeAttributeAtPublish);
				FileIO::SetAfterTestHook(ScheduleConflictAfterRecovery);
			} else FileIO::SetTestHook(ChangeAttributeAndScheduleRecoveryConflict);
			std::error_code WriteError;
			Check(!File.IsFileWriteSuccessful("recovery formatted", nullptr, &WriteError), "recovery race succeeded");
			Check(!WriteError, "recovery race reported an unrelated system error");
			Check(Load(Path) == (IsAfterRecovery ? "recovery external" : "recovery original"), "recovery race changed public bytes");
			Check(Load(HookHeld) == (IsAfterRecovery ? "recovery original" : "recovery formatted"), "recovery race lost held bytes");
			const Filesystem::path OriginalPath = IsAfterRecovery ? HookHeld : Path;
			Check(
				LoadAttribute(OriginalPath, "user.shavefmt-test") == std::string(AttributeVal, sizeof(AttributeVal)),
				"recovery race lost original attributes"
			);
			// 自身の候補は製品が除き，別実体だけテストケースが後始末する
			if(IsAfterRecovery) CheckNoPreservedFiles(Path);
			else RemovePreservedFiles(Path, "recovery external");
			Check(Filesystem::remove(HookHeld), "recovery held fixture cleanup failed");
		}
		for(const bool IsAfterRecovery : IsRecoveryPhases) {
			// 復旧直前と直後の退避名の差替で，候補と原本と外部実体の保全を確かめる
			Check(Filesystem::remove(Path), "temporary recovery fixture reset failed");
			Save(Path, "temporary recovery original");
			const FileIO File(Path.string());
			std::string Src;
			Check(File.IsFileReadSuccessful(Src), "read before temporary recovery race failed");
			HookTarget = Path;
			HookHeld = Dir.Path / "held-temporary-recovery.tmp";
			if(IsAfterRecovery) {
				FileIO::SetTestHook(ChangeAttributeAtPublish);
				FileIO::SetAfterTestHook(ScheduleTempConflictAfterRecovery);
			} else FileIO::SetTestHook(ChangeAttributeAndScheduleTempConflict);
			std::error_code WriteError;
			Check(!File.IsFileWriteSuccessful("temporary recovery formatted", nullptr, &WriteError), "temporary recovery race succeeded");
			Check(!WriteError, "temporary recovery race reported an unrelated system error");
			Check(
				Load(Path) == (IsAfterRecovery ? "temporary recovery original" : "recovery temporary external"),
				"temporary recovery race changed public bytes"
			);
			Check(
				Load(HookHeld) == (IsAfterRecovery ? "temporary recovery formatted" : "temporary recovery original"),
				"temporary recovery race lost held bytes"
			);
			const Filesystem::path OriginalPath = IsAfterRecovery ? Path : HookHeld;
			Check(
				LoadAttribute(OriginalPath, "user.shavefmt-test") == std::string(AttributeVal, sizeof(AttributeVal)),
				"temporary recovery race lost original attributes"
			);
			if(IsAfterRecovery) RemovePreservedFiles(Path, "recovery temporary external");
			else CheckNoPreservedFiles(Path);
			Check(Filesystem::remove(HookHeld), "temporary recovery held fixture cleanup failed");
		}
		#if defined(__linux__)
		{
			// 既定 ACL の新規実体への継承と既存値保全の個別検査
			const Filesystem::path AclDir = Dir.Path / "acl", AclPath = AclDir / "source.cpp";
			Check(Filesystem::create_directory(AclDir), "ACL fixture directory creation failed");
			Save(AclPath, "ACL original");
			SetDefaultAcl(AclDir);
			// 探査用ファイルでの既定 ACL の継承確認
			const Filesystem::path Probe = AclDir / "probe";
			Save(Probe, "probe");
			Check(::getxattr(Probe.c_str(), "system.posix_acl_access", nullptr, 0) > -1, "default ACL was not inherited");
			const std::string ProbeAcl = LoadAttribute(Probe, "system.posix_acl_access");
			// 通常保存後の既存アクセス制御表の値保持
			const FileIO ProbeFile(Probe.string());
			std::string ProbeSrc;
			Check(
				ProbeFile.IsFileReadSuccessful(ProbeSrc) && ProbeFile.IsFileWriteSuccessful("formatted probe"),
				"existing ACL save failed"
			);
			Check(LoadAttribute(Probe, "system.posix_acl_access") == ProbeAcl, "existing ACL value was not preserved");
			Check(Filesystem::remove(Probe), "ACL probe cleanup failed");
			// 差替保存の新実体への親の既定 ACL 漏出検査
			const FileIO File(AclPath.string());
			std::string Src;
			Check(File.IsFileReadSuccessful(Src) && File.IsFileWriteSuccessful("ACL formatted"), "default ACL save failed");
			errno = 0;
			Check(
				::getxattr(AclPath.c_str(), "system.posix_acl_access", nullptr, 0) < 0 && errno == ENODATA,
				"inherited ACL leaked to output"
			);
			// 最後の照合の後に付いた ACL も原本の実体と共に戻す
			const FileIO ConflictingFile(AclPath.string());
			Check(ConflictingFile.IsFileReadSuccessful(Src), "read before ACL publication conflict failed");
			HookTarget = AclPath;
			FileIO::SetTestHook(ChangeAclAtPublish);
			std::error_code WriteError;
			Check(!ConflictingFile.IsFileWriteSuccessful("ACL rejected", nullptr, &WriteError), "ACL publication conflict succeeded");
			Check(!WriteError && Load(AclPath) == "ACL formatted", "ACL recovery changed original or reason");
			Check(LoadAttribute(AclPath, "system.posix_acl_access") == MakeAcl(), "ACL recovery lost external access control");
			CheckNoPreservedFiles(AclPath);
			Check(Filesystem::remove(AclPath) && Filesystem::remove(AclDir), "ACL fixture cleanup failed");
		}
		#endif
		{
			// 本文が同じでも読込後の更新日時変更を競合として拒む
			Save(Path, "original");
			const FileIO File(Path.string());
			std::string Src;
			Check(File.IsFileReadSuccessful(Src), "read before metadata change failed");
			Filesystem::last_write_time(Path, Filesystem::last_write_time(Path) - std::chrono::seconds(2)); // 更新日時を戻す秒数
			Check(!File.IsFileWriteSuccessful("stale result"), "metadata change was ignored");
			Check(Load(Path) == "original", "metadata change allowed overwrite");
		}
		{
			// 同内容・同日時の差替に於ける実体識別に依る検出
			Save(Path, "original");
			const FileIO File(Path.string());
			std::string Src;
			Check(File.IsFileReadSuccessful(Src), "read before replacement failed");
			Save(Replacement, "original");
			Filesystem::last_write_time(Replacement, Filesystem::last_write_time(Path));
			// 内容と日時の比較を通過する保存先実体交換の競合生成
			Replace(Replacement, Path);
			Check(!File.IsFileWriteSuccessful("stale result"), "replacement was overwritten");
			Check(Load(Path) == "original", "replacement bytes changed");
		}
		{
			// 古い保存権に依る読込後削除・同名再作成の上書拒否
			Save(Path, "original");
			const FileIO File(Path.string());
			std::string Src;
			Check(File.IsFileReadSuccessful(Src), "read before removal failed");
			Check(Filesystem::remove(Path), "fixture removal failed");
			Check(!File.IsFileWriteSuccessful("stale result") && !Filesystem::exists(Path), "deleted file was resurrected");
			// 削除後に別の利用者が作った同名実体の新規実体としての保護
			Save(Path, "external");
			Check(!File.IsFileWriteSuccessful("stale result"), "recreated file was overwritten");
			Check(Load(Path) == "external", "recreated bytes changed");
		}
		{
			// 再読込失敗時の旧スナップショットの破棄
			const FileIO File(Path.string());
			std::string Src;
			Check(File.IsFileReadSuccessful(Src), "initial read failed");
			Check(Filesystem::remove(Path), "fixture removal failed");
			Check(!File.IsFileReadSuccessful(Src), "failed read succeeded");
			Save(Path, "external");
			Check(!File.IsFileWriteSuccessful("stale result"), "failed read retained old snapshot");
			Check(Load(Path) == "external", "failed read allowed overwrite");
		}
		{
			// 対応する読込が無い保存要求は現在の実体へ触れず拒む
			const FileIO File(Path.string());
			const std::string Before = Load(Path);
			Check(!File.IsFileWriteSuccessful("direct save"), "save without a matching read succeeded");
			Check(Load(Path) == Before, "save without a read changed bytes");
		}
		{
			// 読込無の不存在パスに対する新規作成の拒否
			Check(Filesystem::remove(Path), "fixture removal failed");
			const FileIO File(Path.string());
			Check(!File.IsFileWriteSuccessful("new file") && !Filesystem::exists(Path), "save without an existing read created a file");
			Save(Path, "new file");
		}
		{
			// ハードリンク先への差替保存に依る別名の旧実体残留通知
			const Filesystem::path Alias = Dir.Path / "hard-link.cpp";
			Save(Path, "linked original");
			Filesystem::create_hard_link(Path, Alias);
			const FileIO File(Path.string());
			std::string Src;
			bool IsHardLinkBroken = false;
			Check(
				File.IsFileReadSuccessful(Src) && File.IsFileWriteSuccessful("linked formatted", &IsHardLinkBroken),
				"hard-link save failed"
			);
			Check(IsHardLinkBroken && Load(Path) == "linked formatted" && Load(Alias) == "linked original", "hard-link notice failed");
			Check(Filesystem::remove(Alias), "hard-link cleanup failed");
		}
		{
			// 特殊権限を含む全モードビットの差替後実体への移行
			Save(Path, "mode original");
			Check(!::chmod(Path.c_str(), 01700), "special mode setup failed"); // 特殊権限付のテストケースモード
			const FileIO File(Path.string());
			std::string Src;
			Check(File.IsFileReadSuccessful(Src) && File.IsFileWriteSuccessful("mode formatted"), "special mode save failed");
			struct stat Info {};
			// 全モードビットの比較
			Check(!::stat(Path.c_str(), &Info) && (Info.st_mode & 07777) == 01700, "special mode was not preserved");
		}
		{
			// 最終ファイルのリンクは辿らず，読み書きとも失敗させて実体を書き換えない
			// 相対リンクに依る保存先ディレクトリ基準解決時の拒否確認
			const Filesystem::path Link = Dir.Path / "alias.cpp";
			Filesystem::create_symlink(Path.filename(), Link);
			Save(Path, "link target");
			const FileIO Linked(Link.string());
			std::string Src;
			Check(!Linked.IsFileReadSuccessful(Src) && !Linked.IsFileWriteSuccessful("stale result"), "symlink was followed");
			Check(Filesystem::is_symlink(Link) && Load(Path) == "link target", "symlink target was changed");
			Check(Filesystem::remove(Link), "symlink cleanup failed");
		}
		#endif
		// テスト領域の自動削除前に於ける一時ファイル・控えの残留検出
		for(const Filesystem::directory_entry &Entry : Filesystem::directory_iterator(Dir.Path)) {
			Check(Entry.path() == Path, "unexpected temporary file remains");
		}
		// 最後の原本の明示削除とテスト領域の空状態検査
		Check(Filesystem::remove(Path) && Filesystem::is_empty(Dir.Path), "fixture cleanup failed");
		std::puts("FileIO normal saves and external update preservation passed");
	} catch(const std::exception &E) {
		std::fprintf(stderr, "FileIO verification failed: %s\n", E.what());
		// 検査失敗の返戻
		return 1;
	}
	// 全検査成功の返戻
	return 0;
}
