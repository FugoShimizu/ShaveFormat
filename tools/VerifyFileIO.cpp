#include "Util/FileIO.hpp"
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

#if defined(_WIN32)

#include <windows.h>

#else

#include <sys/stat.h>
#include <sys/xattr.h>

#endif

// ファイルシステム名前空間の別名
namespace Filesystem = std::filesystem;

// 公開競合試験の鉤状態
static Filesystem::path HookTarget;
static Filesystem::path HookReplacement;
static Filesystem::path HookHeld;
static constexpr char AttributeValue[] = "latest attribute";

/**
 * 試験条件の検査関数
 * @param IsSatisfied 成功条件
 * @param Message 失敗箇所の説明
 */
static void Check(const bool IsSatisfied, const char *const Message) {
	// 最初の不一致での試験停止と例外文面への失敗位置保存
	if(!IsSatisfied) throw std::runtime_error(Message);
	// 終了
	return;
}

// 試験専用の排他的な一時領域
class TestDirectory {
public:

	Filesystem::path Path; // 此の試験だけの一時領域

	TestDirectory &operator=(const TestDirectory &) = delete; // コピー代入演算子（禁止）
	TestDirectory(const TestDirectory &) = delete; // コピーコンストラクタ（禁止）

	/**
	 * 試験領域の生成コンストラクタ
	 */
	TestDirectory() {
		// 一時領域の正規化と無作為名による生成
		std::random_device Random;
		const Filesystem::path Root = Filesystem::canonical(Filesystem::temp_directory_path());
		// 無作為名と既存資源の衝突時だけの再試行
		for(uint32_t Attempt = 0; Attempt < 100; ++Attempt) { // 一時名生成の最大試行回数
			Path = Root / ("shavefmt-fileio-" + std::to_string(Random()));
			// 自分が生成出来た領域だけを取得しての返戻
			if(Filesystem::create_directory(Path)) return;
		}
		throw std::runtime_error("cannot create test directory");
	}

	/**
	 * 試験領域の解放デストラクタ
	 */
	~TestDirectory() {
		// 例外送出中も継続する一時領域の解放
		std::error_code Error;
		Filesystem::remove_all(Path, Error);
		if(Error) std::fprintf(stderr, "[warn] test directory left behind: %s\n", Path.string().c_str());
		// 終了
		return;
	}
};

/**
 * 外部保存の再現関数
 * @param Path 保存先
 * @param Bytes 保存する生バイト
 */
static void Save(const Filesystem::path &Path, const std::string &Bytes) {
	// 改行変換を伴わない検体の生バイト置換
	std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
	Output.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
	Output.close();
	Check(!Output.fail(), "fixture write failed");
	// 終了
	return;
}

/**
 * 外部読込の再現関数
 * @param Path 読込元
 * @return 生バイトの全内容
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
 * 差替保存の再現関数
 * @param From 移動元
 * @param To 差替先
 */
static void Replace(const Filesystem::path &From, const Filesystem::path &To) {
	// 各環境の原子的置換手段による外部保存の再現
	#if defined(_WIN32)
	Check(MoveFileExW(From.c_str(), To.c_str(), MOVEFILE_REPLACE_EXISTING), "fixture replacement failed");
	#else
	Filesystem::rename(From, To);
	#endif
	// 終了
	return;
}

/**
 * 公開直前の原本差替再現関数
 */
static void ReplaceTargetAtPublish() {
	// 公開直前の鉤で読込後に別の実体へ差替
	Replace(HookReplacement, HookTarget);
	// 終了
	return;
}

/**
 * 公開直前の拡張属性更新再現関数
 */
static void ChangeAttributeAtPublish() {
	// 本文を変えない拡張属性更新によるメタデータ競合の生成
	#if defined(__APPLE__)
	Check(
		!::setxattr(HookTarget.c_str(), "user.shavefmt-test", AttributeValue, sizeof(AttributeValue), 0, 0),
		"attribute update failed"
	);
	#elif defined(__linux__)
	Check(
		!::setxattr(HookTarget.c_str(), "user.shavefmt-test", AttributeValue, sizeof(AttributeValue), 0),
		"attribute update failed"
	);
	#endif
	// 終了
	return;
}

#if !defined(_WIN32)

/**
 * 拡張属性取得関数
 * @param Path 対象ファイル
 * @param Name 属性名
 * @return 属性値
 */
static std::string LoadAttribute(const Filesystem::path &Path, const char *const Name) {
	// 属性値の必要量取得と受取領域の確保
	#if defined(__APPLE__)
	const ssize_t Size = ::getxattr(Path.c_str(), Name, nullptr, 0, 0, 0);
	#else
	const ssize_t Size = ::getxattr(Path.c_str(), Name, nullptr, 0);
	#endif
	Check(Size > -1, "attribute size read failed");
	std::string Value(static_cast<size_t>(Size), '\0');
	// 読込量との一致による取得間寸法変化の失敗化
	#if defined(__APPLE__)
	const ssize_t Read = ::getxattr(Path.c_str(), Name, Value.data(), Value.size(), 0, 0);
	#else
	const ssize_t Read = ::getxattr(Path.c_str(), Name, Value.data(), Value.size());
	#endif
	Check(Read == Size, "attribute value read failed");
	// 属性値の返戻
	return Value;
}

#endif

#if defined(__linux__)

/**
 * リトルエンディアン１６ビット値追加関数
 * @param Bytes 格納先
 * @param Value 値
 */
static void AppendLe16(std::string &Bytes, const uint16_t Value) {
	// ACL 外部表現に合わせた下位バイトからの格納
	Bytes.push_back(static_cast<char>(Value));
	Bytes.push_back(static_cast<char>(Value >> 8)); // １バイトのビット数
	// 終了
	return;
}

/**
 * リトルエンディアン３２ビット値追加関数
 * @param Bytes 格納先
 * @param Value 値
 */
static void AppendLe32(std::string &Bytes, const uint32_t Value) {
	// ホストのエンディアンに依存しない４バイト直列化
	for(uint32_t Shift = 0; Shift < 32; Shift += 8) Bytes.push_back(static_cast<char>(Value >> Shift)); // ３２ビットを８ビット単位で直列化
	// 終了
	return;
}

/**
 * POSIX ACL 項目追加関数
 * @param Bytes 格納先
 * @param Tag 項目種別
 * @param Permission 権限
 * @param Qualifier 利用者又は集団識別子
 */
static void AppendAclEntry(std::string &Bytes, const uint16_t Tag, const uint16_t Permission, const uint32_t Qualifier) {
	// カーネルの項目順に従う種別・権限・識別子の連結
	AppendLe16(Bytes, Tag);
	AppendLe16(Bytes, Permission);
	AppendLe32(Bytes, Qualifier);
	// 終了
	return;
}

/**
 * 既定 ACL 設定関数
 * @param Path 対象ディレクトリ
 */
static void SetDefaultAcl(const Filesystem::path &Path) {
	// 既定 ACL の外部表現
	std::string Acl;
	AppendLe32(Acl, 2); // Linux のアクセス制御表版
	// 所有者・指定利用者・集団・マスク・其の他の最小構成生成
	AppendAclEntry(Acl, 0X01, 7, UINT32_MAX); // 所有者の全権限
	AppendAclEntry(Acl, 0X02, 4, 65534); // 指定利用者の読込権限
	AppendAclEntry(Acl, 0X04, 0, UINT32_MAX); // 所有集団の権限無
	AppendAclEntry(Acl, 0X10, 4, UINT32_MAX); // 指定項目へ掛ける読込マスク
	AppendAclEntry(Acl, 0X20, 0, UINT32_MAX); // 其の他の権限無
	Check(!::setxattr(Path.c_str(), "system.posix_acl_default", Acl.data(), Acl.size(), 0), "default ACL setup failed");
	// 終了
	return;
}

#endif

/**
 * 公開直前の一時名差替再現関数
 * 計算量：保存先ディレクトリの項目数 N に対し O(N)
 * @param IsOversized 上限超過の一時実体へ差し替える場合は true
 */
static void ReplaceTemporary(const bool IsOversized) {
	// 保存先と同じディレクトリに在る公開待ち一時名の探索
	for(const Filesystem::directory_entry &Entry : Filesystem::directory_iterator(HookTarget.parent_path())) {
		if(const std::string Name = Entry.path().filename().string(); !Name.starts_with(".shavefmt-") || !Name.ends_with(".tmp")) {
			continue;
		}
		// 整形済実体の退避と外部実体への差替及び必要時の寸法拡張
		Replace(Entry.path(), HookHeld);
		Save(Entry.path(), IsOversized ? "" : "foreign temporary content");
		if(IsOversized) Filesystem::resize_file(Entry.path(), FileIO::MaxInputBytes + 1);
		// 終了
		return;
	}
	// 一時名探索失敗の送出
	throw std::runtime_error("temporary publication name not found");
}

/**
 * 公開直前の一時名差替再現関数
 */
static void ReplaceTemporaryAtPublish() {
	// 通常寸法の外部実体への交換
	ReplaceTemporary(false);
	// 終了
	return;
}

/**
 * 公開直前の上限超過一時名差替再現関数
 */
static void ReplaceOversizedTemporaryAtPublish() {
	// 上限超過の外部実体への交換
	ReplaceTemporary(true);
	// 終了
	return;
}

/**
 * 競合時に保全された一時資源の除去関数
 * 計算量：保存先ディレクトリの項目数 N に対し O(N)
 * @param Path 対象ファイル
 * @param Expected 保全された内容
 */
static void RemovePreservedFiles(const Filesystem::path &Path, const std::string &Expected) {
	// 保全資源の探索・検査・除去数
	size_t Removed = 0;
	for(const Filesystem::directory_entry &Entry : Filesystem::directory_iterator(Path.parent_path())) {
		if(!Entry.path().filename().string().starts_with(".shavefmt-")) continue;
		Check(Load(Entry.path()) == Expected, "preserved file bytes differ");
		Check(Filesystem::remove(Entry.path()), "preserved file cleanup failed");
		++Removed;
	}
	// 個数による保全資源の欠落・重複検出
	Check(Removed == 1, "preserved file count differs");
	// 終了
	return;
}

/**
 * 上限超過で保全された一時資源の除去関数
 * 計算量：保存先ディレクトリの項目数 N に対し O(N)
 * @param Path 対象ファイル
 */
static void RemoveOversizedPreservedFile(const Filesystem::path &Path) {
	// 上限超過資源の探索・検査・除去数
	size_t Removed = 0;
	for(const Filesystem::directory_entry &Entry : Filesystem::directory_iterator(Path.parent_path())) {
		if(!Entry.path().filename().string().starts_with(".shavefmt-")) continue;
		Check(Filesystem::file_size(Entry.path()) == FileIO::MaxInputBytes + 1, "oversized preserved file size differs");
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
 * @param Path 保存先
 * @param Original 読込時の内容
 * @param Updated 外部保存の内容
 * @param RestoreTime 元の更新日時に戻す場合は true
 */
static void CheckConflict(
	const Filesystem::path &Path,
	const std::string &Original,
	const std::string &Updated,
	const bool RestoreTime
) {
	// 読込時の実体と状態の記録
	Save(Path, Original);
	const Filesystem::file_time_type OriginalTime = Filesystem::last_write_time(Path);
	const FileIO File(Path.string());
	std::string Source;
	Check(File.ReadFile(Source), "read before external save failed");
	// 外部保存と必要時の更新日時復元による競合生成
	Save(Path, Updated);
	if(RestoreTime) Filesystem::last_write_time(Path, OriginalTime);
	// 古い整形結果の拒否と外部保存内容の保全の個別確認
	Check(!File.WriteFile("stale result"), "external save was overwritten");
	Check(Load(Path) == Updated, "external bytes changed");
	// 再読込後の新しいスナップショットへの更新と通常保存への復帰
	Check(File.ReadFile(Source) && File.WriteFile("fresh result"), "reread after conflict did not recover");
	Check(Load(Path) == "fresh result", "fresh save bytes differ");
	// 終了
	return;
}

/**
 * ファイル保存の回帰検査関数
 * @return 全検査成功時は０，失敗時は１
 */
int main() {
	// 全検査の例外境界
	try {
		{
			// CRLF に揃う原本は正規化と展開の往復で，行の終わりに読まない単独の CR も含め原文へ戻る
			// 単独 CR を含む CRLF 原本
			static constexpr std::string_view Original = "a\r\r\nb\r\n";
			// 正規化と復元の作業領域
			std::string Source(Original);
			// 内部 LF 表現への正規化
			FileIO::Normalize(Source, false);
			// 正規化後改行の原本様式への復元による可逆性検査
			FileIO::ExpandLf(Source);
			Check(Source == Original, "CRLF round trip changed bytes");
		}
		// 全検体を隔離する一時領域
		const TestDirectory Directory;
		// 全経路での保存先再利用による読込状態の消費・再取得検査
		const Filesystem::path Path = Directory.Path / "source.cpp", Replacement = Directory.Path / "replacement.cpp";
		// 空入力・分割読込境界・NUL・BOM・CRLF の通常経路
		struct ReadCase {
			std::string Original; // 保存する生バイト
			std::string Expected; // BOM 除去後の本文
			size_t BomCount; // 先頭 BOM の期待個数
		};
		const ReadCase ReadCases[] = {
			{ "", "", 0 }, // 零バイト入力と読込失敗の区別
			{ std::string(65537, '\0'), std::string(65537, '\0'), 0 }, // 内部読込塊の境界越しの NUL
			{ "\xEF\xBB\xBF\xEF\xBB\xBF" "line\r\n", "line\r\n", 2 } // 先頭 BOM の期待個数
		};
		for(const ReadCase &Case : ReadCases) {
			// 各検体を同じ経路へ新規保存し，前の検体の状態を残さない
			const std::string &Original = Case.Original;
			Save(Path, Original);
			const FileIO File(Path.string());
			std::string Source;
			size_t BomCount = 0;
			Check(File.ReadFile(Source, &BomCount), "normal read failed");
			Check(Source == Case.Expected && BomCount == Case.BomCount, "BOM read contract failed");
			// 読込と同じ原本の書戻による無変更時の生バイト保持
			Check(File.WriteFile(Original), "normal write failed");
			Check(Load(Path) == Original, "normal write changed bytes");
			// 成功した保存の読込権の消費と二重公開の拒否
			Check(!File.WriteFile("stale result"), "consumed read was reused");
			Check(Load(Path) == Original, "consumed read changed bytes");
			// 明示的な再読込に依る次の保存権取得検査
			Check(File.ReadFile(Source) && File.WriteFile(Original), "reread after save failed");
		}
		{
			// 整形で増えた出力を除く入力上限の読込保護限定
			Save(Path, "large output source");
			const FileIO File(Path.string());
			std::string Source;
			// 入力上限を越える出力
			const std::string LargeResult(FileIO::MaxInputBytes + 1, 'x');
			Check(File.ReadFile(Source), "read before large output failed");
			// 上限を１バイト越える出力の公開境界検査
			Check(File.WriteFile(LargeResult), "large permitted output save failed");
			Check(Load(Path) == LargeResult, "large permitted output bytes differ");
		}
		// 空への外部切詰を含む通常更新の競合検出
		CheckConflict(Path, "original", "", false);
		// 更新日時復元後の内容差による競合検出
		CheckConflict(Path, "original", "external", true);
		// BOM 除去後に本文が一致する原本バイト差の検出
		CheckConflict(Path, "\xEF\xBB\xBF" "original", "original", false);
		// 比較塊境界より後だけが異なる外部更新の検出
		CheckConflict(Path, std::string(65536, 'x') + "a", std::string(65536, 'x') + "b", true); // 内部比較塊の境界
		// 書戻中の差替・削除・リンクの再現は名前を交換出来る POSIX に限定
		#if !defined(_WIN32)
		{
			// 公開直前に保存先の実体を交換し，古い結果の上書きを拒ませる
			Save(Path, "original");
			Save(Replacement, "external at publication");
			const FileIO File(Path.string());
			std::string Source;
			Check(File.ReadFile(Source), "read before publication conflict failed");
			HookTarget = Path;
			HookReplacement = Replacement;
			// 一時資源準備後且つ公開前への外部差替の注入
			FileIO::SetTestHook(ReplaceTargetAtPublish);
			// 無関係なシステム誤りの受取先
			std::error_code WriteError;
			Check(!File.WriteFile("stale result", nullptr, &WriteError), "publication conflict was overwritten");
			Check(!WriteError, "publication conflict reported an unrelated system error");
			Check(Load(Path) == "external at publication", "publication conflict bytes changed");
			// 公開出来なかった整形結果の調査用保全の確認
			RemovePreservedFiles(Path, "stale result");
		}
		{
			// 公開待ち一時名の外部実体への交換による名前再利用の検出
			Save(Path, "original");
			const FileIO File(Path.string());
			std::string Source;
			Check(File.ReadFile(Source), "read before temporary replacement failed");
			HookTarget = Path;
			HookHeld = Directory.Path / "held-result.tmp";
			// 公開直前の同名交換
			FileIO::SetTestHook(ReplaceTemporaryAtPublish);
			// 無関係なシステム誤りの受取先
			std::error_code WriteError;
			Check(!File.WriteFile("formatted result", nullptr, &WriteError), "foreign temporary file was published");
			Check(!WriteError, "temporary replacement conflict reported an unrelated system error");
			Check(Load(Path) == "original", "temporary replacement changed original");
			// 元の一時実体に残る整形結果の識別検査
			Check(Load(HookHeld) == "formatted result", "prepared output identity changed");
			Check(Filesystem::remove(HookHeld), "held result cleanup failed");
			RemovePreservedFiles(Path, "foreign temporary content");
		}
		{
			// 入力上限を越える差替後一時実体の公開前検査
			Save(Path, "original");
			const FileIO File(Path.string());
			std::string Source;
			Check(File.ReadFile(Source), "read before oversized temporary replacement failed");
			HookTarget = Path;
			HookHeld = Directory.Path / "held-oversized-result.tmp";
			// 上限超過実体への交換
			FileIO::SetTestHook(ReplaceOversizedTemporaryAtPublish);
			Check(!File.WriteFile("oversized formatted"), "oversized foreign temporary file was published");
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
			std::string Source;
			Check(File.ReadFile(Source), "read before post-publication conflict failed");
			HookTarget = Path;
			HookReplacement = Replacement;
			// 公開後照合の直前への外部差替の注入
			FileIO::SetAfterTestHook(ReplaceTargetAtPublish);
			// 無関係なシステム誤りの受取先
			std::error_code WriteError;
			Check(!File.WriteFile("formatted before later save", nullptr, &WriteError), "post-publication conflict succeeded");
			Check(!WriteError, "post-publication conflict reported an unrelated system error");
			Check(Load(Path) == "later external save", "later external save was overwritten");
			// 公開前の原本が別名に保全され，復旧可能である事を探す
			bool IsPreserved = false;
			for(const Filesystem::directory_entry &Entry : Filesystem::directory_iterator(Directory.Path)) {
				if(Entry.path() == Path || Entry.path() == Replacement || Load(Entry.path()) != "original before later save") continue;
				// 原本の別名保全を確認
				IsPreserved = true;
				Check(Filesystem::remove(Entry.path()), "post-publication backup cleanup failed");
			}
			Check(IsPreserved, "original backup was not preserved");
		}
		{
			// 本文・日時一致時の拡張属性更新の外部変更扱い
			Save(Path, "attribute original");
			const FileIO File(Path.string());
			std::string Source;
			Check(File.ReadFile(Source), "read before attribute conflict failed");
			HookTarget = Path;
			// 公開直前の属性更新
			FileIO::SetTestHook(ChangeAttributeAtPublish);
			Check(!File.WriteFile("attribute formatted"), "attribute conflict succeeded");
			Check(Load(Path) == "attribute original", "attribute conflict changed bytes");
			Check(
				LoadAttribute(Path, "user.shavefmt-test") == std::string(AttributeValue, sizeof(AttributeValue)),
				"attribute conflict lost latest value"
			);
			// 拒否された整形結果の競合調査用一時資源への保全
			RemovePreservedFiles(Path, "attribute formatted");
		}
		#if defined(__linux__)
		{
			// 既定 ACL の新規実体への継承と既存値保全の個別検査
			const Filesystem::path AclDirectory = Directory.Path / "acl", AclPath = AclDirectory / "source.cpp";
			Check(Filesystem::create_directory(AclDirectory), "ACL fixture directory creation failed");
			Save(AclPath, "ACL original");
			SetDefaultAcl(AclDirectory);
			// 探査用ファイルでの既定 ACL の継承確認
			const Filesystem::path Probe = AclDirectory / "probe";
			Save(Probe, "probe");
			Check(::getxattr(Probe.c_str(), "system.posix_acl_access", nullptr, 0) > -1, "default ACL was not inherited");
			const std::string ProbeAcl = LoadAttribute(Probe, "system.posix_acl_access");
			// 通常保存後の既存アクセス制御表の値保持
			const FileIO ProbeFile(Probe.string());
			std::string ProbeSource;
			Check(ProbeFile.ReadFile(ProbeSource) && ProbeFile.WriteFile("formatted probe"), "existing ACL save failed");
			Check(LoadAttribute(Probe, "system.posix_acl_access") == ProbeAcl, "existing ACL value was not preserved");
			Check(Filesystem::remove(Probe), "ACL probe cleanup failed");
			// 差替保存の新実体への親の既定 ACL 漏出検査
			const FileIO File(AclPath.string());
			std::string Source;
			Check(File.ReadFile(Source) && File.WriteFile("ACL formatted"), "default ACL save failed");
			errno = 0;
			Check(
				::getxattr(AclPath.c_str(), "system.posix_acl_access", nullptr, 0) < 0 && errno == ENODATA,
				"inherited ACL leaked to output"
			);
			Check(Filesystem::remove(AclPath) && Filesystem::remove(AclDirectory), "ACL fixture cleanup failed");
		}
		#endif
		{
			// 本文が同じでも読込後の更新日時変更を競合として拒む
			Save(Path, "original");
			const FileIO File(Path.string());
			std::string Source;
			Check(File.ReadFile(Source), "read before metadata change failed");
			Filesystem::last_write_time(Path, Filesystem::last_write_time(Path) - std::chrono::seconds(2)); // 更新日時を戻す秒数
			Check(!File.WriteFile("stale result"), "metadata change was ignored");
			Check(Load(Path) == "original", "metadata change allowed overwrite");
		}
		{
			// 同内容・同日時の差替に於ける実体識別による検出
			Save(Path, "original");
			const FileIO File(Path.string());
			std::string Source;
			Check(File.ReadFile(Source), "read before replacement failed");
			Save(Replacement, "original");
			Filesystem::last_write_time(Replacement, Filesystem::last_write_time(Path));
			// 内容と日時の比較を通過する保存先実体交換の競合生成
			Replace(Replacement, Path);
			Check(!File.WriteFile("stale result"), "replacement was overwritten");
			Check(Load(Path) == "original", "replacement bytes changed");
		}
		{
			// 古い保存権による読込後削除・同名再作成の上書き拒否
			Save(Path, "original");
			const FileIO File(Path.string());
			std::string Source;
			Check(File.ReadFile(Source), "read before removal failed");
			Check(Filesystem::remove(Path), "fixture removal failed");
			Check(!File.WriteFile("stale result") && !Filesystem::exists(Path), "deleted file was resurrected");
			// 削除後に別の利用者が作った同名実体の新規実体としての保護
			Save(Path, "external");
			Check(!File.WriteFile("stale result"), "recreated file was overwritten");
			Check(Load(Path) == "external", "recreated bytes changed");
		}
		{
			// 再読込失敗時の旧スナップショットと BOM 個数の破棄
			const FileIO File(Path.string());
			std::string Source;
			Check(File.ReadFile(Source), "initial read failed");
			Check(Filesystem::remove(Path), "fixture removal failed");
			size_t BomCount = 1; // 失敗時の零初期化確認用
			Check(!File.ReadFile(Source, &BomCount) && !BomCount, "failed read retained BOM state");
			Save(Path, "external");
			Check(!File.WriteFile("stale result"), "failed read retained old snapshot");
			Check(Load(Path) == "external", "failed read allowed overwrite");
		}
		{
			// 対応する読込が無い保存要求は現在の実体へ触れず拒む
			const FileIO File(Path.string());
			const std::string Before = Load(Path);
			Check(!File.WriteFile("direct save"), "save without a matching read succeeded");
			Check(Load(Path) == Before, "save without a read changed bytes");
		}
		{
			// 読込無の不存在経路に対する新規作成の拒否
			Check(Filesystem::remove(Path), "fixture removal failed");
			const FileIO File(Path.string());
			Check(!File.WriteFile("new file") && !Filesystem::exists(Path), "save without an existing read created a file");
			Save(Path, "new file");
		}
		{
			// ハードリンク先への差替保存による別名の旧実体残留通知
			const Filesystem::path Alias = Directory.Path / "hard-link.cpp";
			Save(Path, "linked original");
			Filesystem::create_hard_link(Path, Alias);
			const FileIO File(Path.string());
			std::string Source;
			bool IsHardLinkBroken = false;
			Check(File.ReadFile(Source) && File.WriteFile("linked formatted", &IsHardLinkBroken), "hard-link save failed");
			Check(IsHardLinkBroken && Load(Path) == "linked formatted" && Load(Alias) == "linked original", "hard-link notice failed");
			Check(Filesystem::remove(Alias), "hard-link cleanup failed");
		}
		{
			// 特殊権限を含む全モードビットの差替後実体への移行
			Save(Path, "mode original");
			Check(!::chmod(Path.c_str(), 01700), "special mode setup failed"); // 特殊権限付の検体モード
			const FileIO File(Path.string());
			std::string Source;
			Check(File.ReadFile(Source) && File.WriteFile("mode formatted"), "special mode save failed");
			struct stat Info {};
			Check(!::stat(Path.c_str(), &Info) && (Info.st_mode & 07777) == 01700, "special mode was not preserved"); // 全モードビットの比較
		}
		{
			// 最終ファイルのリンクは辿らず，読書きとも失敗させて実体を書き換えない
			// 相対リンクによる保存先ディレクトリ基準解決時の拒否確認
			const Filesystem::path Link = Directory.Path / "alias.cpp";
			Filesystem::create_symlink(Path.filename(), Link);
			Save(Path, "link target");
			const FileIO Linked(Link.string());
			std::string Source;
			Check(!Linked.ReadFile(Source) && !Linked.WriteFile("stale result"), "symlink was followed");
			Check(Filesystem::is_symlink(Link) && Load(Path) == "link target", "symlink target was changed");
			Check(Filesystem::remove(Link), "symlink cleanup failed");
		}
		#endif
		// 試験領域の自動削除前に於ける一時ファイル・控えの残留検出
		for(const Filesystem::directory_entry &Entry : Filesystem::directory_iterator(Directory.Path)) {
			Check(Entry.path() == Path, "unexpected temporary file remains");
		}
		// 最後の原本の明示削除と試験領域の空状態検査
		Check(Filesystem::remove(Path) && Filesystem::is_empty(Directory.Path), "fixture cleanup failed");
		std::puts("FileIO normal saves and external update preservation passed");
	} catch(const std::exception &E) {
		std::fprintf(stderr, "FileIO verification failed: %s\n", E.what());
		// 検査失敗の返戻
		return 1;
	}
	// 全検査成功の返戻
	return 0;
}
