#include "Util/FileIO.hpp"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <random>
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

#include <cstring>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <sys/xattr.h>

#endif

#endif

// ファイルシステム名前空間の別名
namespace Filesystem = std::filesystem;

#if defined(_WIN32)

/**
 * 既存のエラーを保ち，ハンドル解放の失敗を記録する関数
 * @param Handle 解放対象のハンドル
 */
static void CloseFile(void *const Handle) {
	// 解放処理が呼出元の失敗理由を上書きしない様に両エラー状態を退避する
	const int SavedErrno = errno;
	const DWORD SavedError = GetLastError();
	// ハンドル解放失敗の警告
	if(!CloseHandle(Handle)) std::fprintf(stderr, "[warn] failed to close file handle (system error %lu)\n", GetLastError());
	errno = SavedErrno;
	// SavedError の SetLastError 設定
	SetLastError(SavedError);
	// 終了
	return;
}

#else

/**
 * 既存のエラーを保ち，記述子解放の失敗を記録する関数
 * @param Descriptor 解放対象の記述子
 */
static void CloseFile(const int Descriptor) {
	// 呼出前のエラー状態の保存
	const int SavedError = errno;
	// 中断時も再試行せず，再利用された記述子の二重解放を防ぐ
	if(::close(Descriptor)) std::fprintf(stderr, "[warn] failed to close file descriptor (errno %d)\n", errno);
	errno = SavedError;
	// 終了
	return;
}

#endif

#if defined(ShaveFormatFileIoTestHooks)

/**
 * 保存競合再現用フック設定関数
 * @param Hook 公開直前に呼ぶ関数
 */
void FileIO::SetTestHook(void (*const Hook)()) {
	// 次の保存１回だけが公開直前の競合を注入出来る様に差替える
	TestHook = Hook;
	// 終了
	return;
}

/**
 * 公開後競合再現用フック設定関数
 * @param Hook 公開直後に呼ぶ関数
 */
void FileIO::SetAfterTestHook(void (*const Hook)()) {
	// 公開後の検証経路へだけ競合を注入するフックを保持する
	AfterTestHook = Hook;
	// 終了
	return;
}

#endif

/**
 * 行配列分割関数（末尾の空行は除去）
 * @param Source 分割対象の文字列
 * @return 行の配列
 */
std::vector<std::string> FileIO::SplitLines(const std::string &Source) {
	// 行数の事前カウントに依る１度の reserve（push_back の償却型再確保を回避）
	std::vector<std::string> Lines;
	Lines.reserve(static_cast<size_t>(std::count(Source.begin(), Source.end(), '\n')) + 1);
	size_t Prev = 0;
	// find で行末を引き，CRLF の CR を除いて直接構築（SIMD 走査と substr の中間複写を省略）
	for(size_t Idx = Source.find('\n'); Idx != std::string::npos; Idx = Source.find('\n', Prev)) {
		Lines.emplace_back(Source.data() + Prev, Idx - Prev - (Idx > Prev && Source[Idx - 1] == '\r'));
		Prev = Idx + 1;
	}
	// 改行で終わらない最後の断片だけを末尾行として残す
	if(Prev < Source.size()) Lines.emplace_back(Source.data() + Prev, Source.size() - Prev);
	// Lines からの末尾要素除去
	while(!Lines.empty() && Lines.back().empty()) Lines.pop_back();
	// 行配列の返戻
	return Lines;
}

/**
 * CRLF の LF への畳込関数（其の場で詰める）
 * @param Source 畳む対象の文字列（インプレース編集）
 * @param From 畳み始める位置（其れより前は触らない）
 * @param IsLoneCrNewline 単独の CR も LF へ揃えるか（偽は行の終わりに読まない単独の CR を値の儘残す）
 */
void FileIO::FoldCrLf(std::string &Source, const size_t From, const bool IsLoneCrNewline) {
	// CR 不在時の正規化走査の省略
	if(Source.find('\r', From) == std::string::npos) return;
	size_t Kept = From;
	// 改行表現の正規化範囲
	for(size_t Idx = From; Idx < Source.size(); ++Idx) {
		// CRLF の CR は落として後の LF を残し，単独の CR は揃えるか値の儘写す
		if(Source[Idx] != '\r') Source[Kept++] = Source[Idx];
		else if(Idx + 1 >= Source.size() || Source[Idx + 1] != '\n') Source[Kept++] = IsLoneCrNewline ? '\n' : '\r';
	}
	Source.resize(Kept);
	// 終了
	return;
}

/**
 * 改行コードと末尾改行の正規化関数（空入力と改行のみの入力は空に）
 * @param Source 正規化対象の文字列（インプレース編集）
 * @param IsLoneCrNewline 単独の CR も行の終わりに読むか（偽の言語は単独の CR を空白や出力の文字に読む為，其の儘残す）
 */
void FileIO::Normalize(std::string &Source, const bool IsLoneCrNewline) {
	// 改行コードの LF 正規化（CRLF を LF へ）後段全パスは LF を行区切として動作する
	FoldCrLf(Source, 0, IsLoneCrNewline);
	// 末尾の連続改行の剥がしと非空時の改行１個の付直（行配列を作らず行分割→再結合の往復と等価）
	while(!Source.empty() && Source.back() == '\n') Source.pop_back();
	if(!Source.empty()) Source += '\n';
	// 終了
	return;
}

/**
 * 改行様式混在判定関数（LF 統一でも CRLF 統一でも無い入力の検出）
 * @param Source 判定対象の原本テキスト（LF 正規化前）
 * @return 改行が LF 統一でも CRLF 統一でも無ければ true
 */
bool FileIO::IsNewlineMixed(const std::string &Source) {
	// CRLF が無ければ書戻で改行は変わらない為，混在無の返戻（生 CR の保存・旧 CR 改行の変換は Normalize に委譲）
	if(Source.find("\r\n") == std::string::npos) return false;
	// 裸の LF の全位置走査
	// 最初の裸の LF で混在確定（裸の CR はリテラル内容で有り得，書戻でも触れない為に判定から除外）
	for(size_t Idx = Source.find('\n'); Idx != std::string::npos; Idx = Source.find('\n', Idx + 1)) {
		// 裸の LF が有る事の返戻
		if(!Idx || Source[Idx - 1] != '\r') return true;
	}
	// CRLF 統一である事の返戻
	return false;
}

/**
 * CR 単独改行の判定関数（LF を持たず CR で行を区切る旧形式）
 * @param Source 判定対象の原本テキスト（LF 正規化前）
 * @return CR 単独で行を区切って居れば true
 */
bool FileIO::IsCrOnlyNewline(const std::string &Source) {
	// LF を持たない儘 CR が有る形は，正規化で全行の改行が LF へ置き換わる事の返戻
	return Source.find('\n') == std::string::npos && Source.find('\r') != std::string::npos;
}

/**
 * LF の CRLF への展開関数（原本が CRLF だったファイルの書戻で改行様式を復元する）
 * @param Source 展開対象の文字列（インプレース編集）
 */
void FileIO::ExpandLf(std::string &Source) {
	// 正規化後に残る CR はリテラルの値の為，LF 前にも CR を補って原本の CRLF を復元
	std::string Out;
	Out.reserve(Source.size() + (Source.size() >> 5) + 16);
	// Source の Char 走査
	for(const char Char : Source) {
		if(Char == '\n') Out += '\r';
		Out += Char;
	}
	// 中間バッファとの交換で原稿を１度だけ更新する
	Source = std::move(Out);
	// 終了
	return;
}

/**
 * 親ディレクトリを固定するコンストラクタ
 * 経路成分のシンボリックリンクを辿らず差替競合を拒む
 * @param Path 対象経路
 */
FileIO::FileIO(const std::string &Path) {
	// 対象経路の絶対名への固定
	std::error_code Error;
	const Filesystem::path Target = Filesystem::absolute(Path, Error);
	// 経路を解決出来ない場合の返戻
	if(Error) return;
	// 後続の成分検査と差替で同じ絶対名を使う
	TargetPath = Target.string();
	#if defined(_WIN32)
	// Windows の親経路ハンドル列の構築
	Filesystem::path Parent = Target.root_path();
	// 親経路の各成分走査
	for(const Filesystem::path &Part : Target.parent_path().relative_path()) {
		Parent /= Part;
		const HANDLE Handle = CreateFileW(
			Parent.c_str(),
			FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
			nullptr
		);
		// 対象を開けない場合の返戻
		if(Handle == INVALID_HANDLE_VALUE) return;
		// 固定した親ディレクトリハンドルの保持
		DirectoryHandles.push_back(Handle);
		BY_HANDLE_FILE_INFORMATION Info {};
		// 親の実体を保証出来ない場合の返戻
		if(!GetFileInformationByHandle(Handle, &Info) || Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return;
	}
	#else
	// POSIX の親ディレクトリ記述子の構築
	#if defined(__APPLE__)
	constexpr int DirectoryAccess = O_SEARCH;
	#elif defined(__linux__)
	constexpr int DirectoryAccess = O_PATH;
	#else
	constexpr int DirectoryAccess = O_RDONLY;
	#endif
	ParentDescriptor = ::open(Target.root_path().c_str(), DirectoryAccess | O_DIRECTORY | O_CLOEXEC);
	// 根を開けない場合の返戻
	if(ParentDescriptor < 0) return;
	// 親迄の各成分を記述子から相対的に開き，名前解決の差替を防ぐ
	for(const Filesystem::path &Part : Target.parent_path().relative_path()) {
		// 成分毎にリンクを拒否し，確認と次の成分への移動を同じ記述子上で行う
		const int Next = ::openat(ParentDescriptor, Part.c_str(), DirectoryAccess | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		// 親成分を開けない場合の返戻
		if(Next < 0) return;
		CloseFile(ParentDescriptor);
		ParentDescriptor = Next;
	}
	#endif
	IsReady = true;
	// 終了
	return;
}

/**
 * 親ディレクトリの固定を解除するデストラクタ
 */
FileIO::~FileIO() {
	// 構築途中で保持した親資源の全解放
	#if defined(_WIN32)
	for(void *const Handle : DirectoryHandles) CloseFile(Handle);
	#else
	if(ParentDescriptor > -1) CloseFile(ParentDescriptor);
	#endif
	// 終了
	return;
}

/**
 * 原本の同一性と更新情報の取得関数
 * @param Handle 読込対象の記述子
 * @param Metadata 比較情報の格納先
 * @return 通常ファイルの情報を取得出来れば true
 */
bool FileIO::ReadMetadata(const NativeFile Handle, std::array<uint64_t, 8> &Metadata) {
	// 実行環境毎の原本情報の取得
	#if defined(_WIN32)
	BY_HANDLE_FILE_INFORMATION Info {};
	FILE_BASIC_INFO Basic {};
	// 通常ファイル以外・情報取得失敗の返戻
	if(
		!GetFileInformationByHandle(Handle, &Info) || !GetFileInformationByHandleEx(Handle, FileBasicInfo, &Basic, sizeof(Basic)) ||
		Info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)
		// 取得失敗の返戻
	) return false;
	// 差替検出用の実体識別子と全更新時刻の退避
	// ファイル状態の領域・実体・寸法・各日時・属性・別名数
	Metadata = {
		Info.dwVolumeSerialNumber,
		static_cast<uint64_t>(Info.nFileIndexHigh) << 32 | Info.nFileIndexLow,
		static_cast<uint64_t>(Info.nFileSizeHigh) << 32 | Info.nFileSizeLow,
		static_cast<uint64_t>(Basic.LastWriteTime.QuadPart),
		static_cast<uint64_t>(Basic.ChangeTime.QuadPart),
		static_cast<uint64_t>(Basic.CreationTime.QuadPart),
		Info.dwFileAttributes,
		Info.nNumberOfLinks
	};
	#else
	struct stat Info {};
	// 通常ファイル以外・情報取得失敗の返戻
	if(::fstat(Handle, &Info) || !S_ISREG(Info.st_mode) || Info.st_size < 0) return false;
	#if defined(__APPLE__)
	const timespec Modified = Info.st_mtimespec, Changed = Info.st_ctimespec;
	#else
	const timespec Modified = Info.st_mtim, Changed = Info.st_ctim;
	#endif
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
		static_cast<uint64_t>(Info.st_mode)
	};
	#endif
	// 取得成功の返戻
	return true;
}

/**
 * 読込中の更新を検査する原本取得・照合関数
 * @param Handle 読込対象の記述子
 * @param Bytes 原本の格納先又は照合元
 * @param Metadata 比較情報の格納先又は照合元
 * @param ShouldCompare 取得済の控えと照合する場合は true
 * @param MaxSize 読込を許す最大寸法
 * @return 読込前後で更新が無く，照合時は内容も一致すれば true
 */
bool FileIO::ReadStableFile(
	const NativeFile Handle,
	std::string &Bytes,
	std::array<uint64_t, 8> &Metadata,
	const bool ShouldCompare,
	const size_t MaxSize
) {
	// 読込前後の状態格納域の準備
	std::array<uint64_t, 8> Before {}, After {};
	// 記述子情報の取得失敗・原本変化・入力又は照合上限の超過時は，信頼出来ない寸法で記憶域を確保せず失敗返戻
	if(!ReadMetadata(Handle, Before) || ShouldCompare && Before != Metadata || Before[2] > MaxSize) return false;
	const size_t Size = static_cast<size_t>(Before[2]);
	// 控えと寸法が異なる場合の失敗返戻
	if(ShouldCompare && Bytes.size() != Size) return false;
	// 初回取得だけ確定寸法へ伸ばし，照合時は控えの領域を変更しない
	if(!ShouldCompare) Bytes.resize(Size);
	#if defined(_WIN32)
	const LARGE_INTEGER Start {};
	// 同一記述子による再照合に向けた読取位置の復帰
	// Windows の読込位置の先頭への復帰
	if(!SetFilePointerEx(Handle, Start, nullptr, FILE_BEGIN)) return false;
	#endif
	// 原本を追加複写せず，固定長の領域で分割照合する
	std::array<char, 65536> Buffer {};
	for(size_t Done = 0;;) {
		// 読み終えた後も１バイト試し，寸法取得直後の追記を検出する
		const size_t Requested = Done < Size ? std::min(Buffer.size(), Size - Done) : 1;
		#if defined(_WIN32)
		DWORD Count = 0;
		// 読取失敗の返戻
		// Windows の部分読込
		if(!::ReadFile(Handle, Buffer.data(), static_cast<DWORD>(Requested), &Count, nullptr)) return false;
		#else
		const ssize_t Count = ::pread(Handle, Buffer.data(), Requested, static_cast<off_t>(Done));
		// 割込による POSIX 読込の再試行
		if(Count < 0 && errno == EINTR) continue;
		// 読取失敗の返戻
		// POSIX 読込失敗の返戻
		if(Count < 0) return false;
		#endif
		// 予定寸法読込後の追記検査
		if(Done == Size) {
			// 寸法確認後に追記された場合の失敗返戻
			if(Count) return false;
			break;
		}
		// 想定より前に終端した場合の失敗返戻
		if(!Count) return false;
		const size_t ReadSize = static_cast<size_t>(Count);
		// 原本の内容が変化した場合の失敗返戻
		// 読込済範囲と基準バイト列の照合
		if(ShouldCompare && !std::equal(Buffer.data(), Buffer.data() + ReadSize, Bytes.data() + Done)) return false;
		// 初回読込バイト列への転写
		else if(!ShouldCompare) std::copy_n(Buffer.data(), ReadSize, Bytes.data() + Done);
		// 部分読込の分だけ次の照合位置を進める
		Done += ReadSize;
	}
	// 読込中に原本が更新された場合の失敗返戻
	if(!ReadMetadata(Handle, After) || Before != After) return false;
	Metadata = Before;
	// 原本取得・照合成功の返戻
	return true;
}

/**
 * 先頭 BOM 除去関数（UTF-8 BOM (EF BB BF) のみ）
 * @param Source BOM 除去対象のソース文字列（インプレース編集）
 * @return 先頭から除いた BOM の個数
 */
size_t FileIO::StripBom(std::string &Source) {
	// 先頭 BOM の全個数を控えて除去し書出時に復元（リテラル・コメント内の BOM は保持）
	size_t Count = 0;
	// Count の次位置
	while(3 * Count + 3 <= Source.size() && !std::memcmp(Source.data() + 3 * Count, "\xEF\xBB\xBF", 3)) ++Count;
	Source.erase(0, 3 * Count);
	// 除いた BOM の個数の返戻
	return Count;
}

/**
 * ファイル読込関数
 * @param Source 読み込んだ内容の格納先
 * @param BomCount 先頭に在った BOM の個数の格納先
 * @return 読込成功時 true
 */
bool FileIO::ReadFile(std::string &Source, size_t *const BomCount) const {
	// 前回読込の基準は新しい取得が完了する迄無効にする
	// 古い読込基準の破棄
	LastRead.reset();
	if(BomCount) *BomCount = 0;
	// 親を固定出来ない場合の失敗返戻
	// 親経路未固定時の失敗返戻
	if(!IsReady) return false;
	// 固定した親からの対象ファイルの取得
	#if defined(_WIN32)
	const NativeFile Handle = CreateFileW(
		Filesystem::path(TargetPath).c_str(),
		GENERIC_READ,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr
	);
	// 対象を開けない場合の返戻
	if(Handle == INVALID_HANDLE_VALUE) return false;
	#else
	const NativeFile Handle =
	::openat(ParentDescriptor, Filesystem::path(TargetPath).filename().c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
	// 記述子を取得出来ない場合の失敗返戻
	// POSIX ファイルの開放失敗返戻
	if(Handle < 0) return false;
	#endif
	// 更新を検出する安定読込と記述子の解放
	ReadState State;
	const bool IsReadOk = ReadStableFile(Handle, State.Bytes, State.Metadata, false, MaxInputBytes);
	CloseFile(Handle);
	// 読込失敗の返戻
	// 原本読込失敗時の返戻
	if(!IsReadOk) return false;
	// 解析用原稿と保存基準の分離
	LastRead = std::move(State);
	Source = LastRead->Bytes;
	// 解析用原稿からだけ BOM を外し，保存用の基準内容は原形を保つ
	// 除去した BOM 寸法の格納
	if(const size_t Stripped = StripBom(Source); BomCount) *BomCount = Stripped;
	// 読込成功の返戻
	return true;
}

/**
 * 処理番号取得関数（一時ファイル名の一意化に用いる）
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
 * 一時ファイル名の乱数部生成関数（予測可能な名前を狙った先回り作成を成立させない為）
 * @return １６桁の１６進文字列
 */
std::string FileIO::RandomSuffix() {
	// 名前毎に OS から乱数６４ビットを取得し，既知の名前から次を予測する先回り作成を防止
	static std::random_device Device;
	static std::mutex Lock;
	uint64_t Value = 0;
	{
		// random_device の同時利用を直列化し，一つの接尾辞を一括取得する
		const std::lock_guard<std::mutex> Guard(Lock);
		Value = static_cast<uint64_t>(Device()) << 32 | Device();
	}
	std::string Text(16, '0');
	// 乱数値の下位桁から１６進化
	for(int Idx = 15; Idx > -1; --Idx, Value >>= 4) Text[static_cast<size_t>(Idx)] = "0123456789ABCDEF"[Value & 0XF];
	// 乱数部の返戻
	return Text;
}

/**
 * 端末制御列の無害化関数（経路名や表示する本文に紛れ込んだ制御文字を落とす）
 * 利用者入力の制御列と双方向文字を無害化する
 * @param Text 表示対象の文字列
 * @param KeepsTab 水平タブを残すか（本文の表示）
 * @return 制御文字（C0 / DEL / C1・双方向の制御文字）と不正な UTF-8 のバイトを `?` へ置き換えた文字列
 */
std::string FileIO::ForDisplay(const std::string_view Text, const bool KeepsTab) {
	std::string Safe;
	Safe.reserve(Text.size());
	// ASCII と多バイト列の分離と無効列の１バイト読飛し
	// 表示用文字列のバイト走査
	for(size_t Pos = 0; Pos < Text.size();) {
		// Lead の初期化
		const unsigned char Lead = static_cast<unsigned char>(Text[Pos]);
		// ASCII バイトの無害化
		if(Lead < 0X80) {
			Safe += Lead < 0X20 && !(KeepsTab && Lead == '\t') || Lead == 0X7F ? '?' : static_cast<char>(Lead);
			++Pos;
			continue;
		}
		// 整形式の UTF-8 の並び（Unicode 表 3-7：冗長な符号化・サロゲート・範囲外は第２バイトの範囲で除く）だけの複写
		const size_t Length = Lead > 0XC1 && Lead < 0XE0 ? 2 : Lead > 0XDF && Lead < 0XF0 ? 3 : Lead > 0XEF && Lead < 0XF5 ? 4 : 0;
		bool IsValidRead = Length && Pos + Length <= Text.size();
		uint32_t Point = Lead & (Length == 2 ? 0X1F : Length == 3 ? 0XF : 0X7);
		for(size_t Index = 1; IsValidRead && Index < Length; ++Index) {
			const unsigned char Follow = static_cast<unsigned char>(Text[Pos + Index]);
			const unsigned char Low = Index > 1 ? 0X80 : Lead == 0XE0 ? 0XA0 : Lead == 0XF0 ? 0X90 : 0X80;
			const unsigned char High = Index > 1 ? 0XBF : Lead == 0XED ? 0X9F : Lead == 0XF4 ? 0X8F : 0XBF;
			IsValidRead = Follow >= Low && Follow <= High;
			Point = Point << 6 | Follow & 0X3F;
		}
		// C1 制御文字と双方向制御文字の `?` への置換
		if(
			!IsValidRead || Point < 0XA0 || Point == 0X61C || Point == 0X200E || Point == 0X200F || Point > 0X2027 && Point < 0X202F ||
			Point > 0X2065 && Point < 0X206A
		) {
			Safe += '?';
			Pos += IsValidRead ? Length : 1;
			continue;
		}
		Safe.append(Text.data() + Pos, Length);
		// 妥当な符号点だけ元の UTF-8 バイト列を保つ
		Pos += Length;
	}
	// 無害化した文字列の返戻
	return Safe;
}

#if !defined(_WIN32)

#if defined(__APPLE__)

/**
 * 拡張 ACL 空判定関数
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
	if(::acl_free(Access)) std::fprintf(stderr, "[warn] failed to release ACL (errno %d)\n", errno);
	if(!EntryResult) {
		errno = 0;
		// 登録項目が在る場合の返戻
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
 * @param SourceDescriptor 原本の記述子
 * @param DestinationDescriptor 複写先の記述子
 * @param Error 系呼出が失敗した場合の理由（属性の競合なら空）
 * @return 属性を全て保持出来れば true
 */
bool FileIO::CopyExtendedAttributes(const int SourceDescriptor, const int DestinationDescriptor, std::error_code &Error) {
	// 複写結果格納域の初期化
	Error.clear();
	#if defined(__APPLE__)
	// 全属性を引き継げた場合の返戻
	if(!::fcopyfile(SourceDescriptor, DestinationDescriptor, nullptr, COPYFILE_XATTR)) return true;
	Error = std::error_code(errno, std::generic_category());
	// 複写失敗の返戻
	return false;
	#elif defined(__linux__)
	// 属性名列の必要寸法照会
	const ssize_t NamesLen = ::flistxattr(SourceDescriptor, nullptr, 0);
	if(NamesLen < 0) {
		// 拡張属性に未対応のファイルシステムは属性無と看做す返戻
		if(errno == ENOTSUP) return true;
		Error = std::error_code(errno, std::generic_category());
		// 属性名の寸法取得失敗の返戻
		return false;
	}
	if(NamesLen) {
		// 属性名一覧の固定長読込
		// 最初に確定した長さを使い，属性名一覧の途中変更を後で照合する
		std::string Names(static_cast<size_t>(NamesLen), '\0');
		// 属性名列の確定
		const ssize_t ReadNamesLen = ::flistxattr(SourceDescriptor, Names.data(), Names.size());
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
			const ssize_t ValueLen = ::fgetxattr(SourceDescriptor, Name, nullptr, 0);
			if(ValueLen < 0) {
				Error = std::error_code(errno, std::generic_category());
				// 属性値の寸法取得失敗の返戻
				return false;
			}
			std::string Value(static_cast<size_t>(ValueLen), '\0');
			// 属性値の固定長読込と複写
			// 値の長さも再読して属性の同時更新を拒む
			const ssize_t ReadValueLen = ::fgetxattr(SourceDescriptor, Name, Value.data(), Value.size());
			if(ReadValueLen < 0) {
				Error = std::error_code(errno, std::generic_category());
				// 属性値の取得失敗の返戻
				return false;
			}
			// 読込中に属性値の寸法が変化した場合の返戻
			if(ReadValueLen != ValueLen) return false;
			// 拡張属性値の複写失敗処理
			if(::fsetxattr(DestinationDescriptor, Name, Value.data(), Value.size(), 0)) {
				Error = std::error_code(errno, std::generic_category());
				// 属性値の複写失敗の返戻
				return false;
			}
		}
	}
	// 全属性を引き継げたかの返戻
	return true;
	#else
	static_cast<void>(SourceDescriptor);
	static_cast<void>(DestinationDescriptor);
	static_cast<void>(Error);
	// 拡張属性を持たない環境での正常返戻
	return true;
	#endif
}

/**
 * 拡張属性一覧取得関数
 * @param Descriptor 対象記述子
 * @param Attributes 属性名と値の格納先
 * @return 全属性を取得出来れば true
 */
bool FileIO::ReadExtendedAttributes(const int Descriptor, std::vector<std::pair<std::string, std::string>> &Attributes) {
	// 属性名一覧の必要寸法取得
	// 実行環境毎の拡張属性の取得
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
		// 次の属性名と値寸法の取得
		// NUL 区切りの属性名を順に値との組へ変換する
		const std::string Name(Names.data() + Pos);
		Pos += Name.size() + 1;
		#if defined(__APPLE__)
		const ssize_t ValueLen = ::fgetxattr(Descriptor, Name.c_str(), nullptr, 0, 0, 0);
		#else
		const ssize_t ValueLen = ::fgetxattr(Descriptor, Name.c_str(), nullptr, 0);
		#endif
		// 属性値の大きさの取得に失敗した事の返戻
		if(ValueLen < 0) return false;
		std::string Value(static_cast<size_t>(ValueLen), '\0');
		#if defined(__APPLE__)
		// 属性値の読取に失敗した事の返戻
		if(::fgetxattr(Descriptor, Name.c_str(), Value.data(), Value.size(), 0, 0) != ValueLen) return false;
		#else
		// 属性値の読取に失敗した事の返戻
		if(::fgetxattr(Descriptor, Name.c_str(), Value.data(), Value.size()) != ValueLen) return false;
		#endif
		Attributes.emplace_back(Name, std::move(Value));
	}
	// 公開前後の比較に向けた属性の名前順整列
	std::sort(Attributes.begin(), Attributes.end());
	// 取得成功の返戻
	return true;
}

/**
 * 公開前後の付帯属性一致判定関数
 * @param Original 交換された原本記述子
 * @param Formatted 公開した整形結果記述子
 * @return 保持対象の属性が一致すれば true
 */
bool FileIO::HasSameExtendedAttributes(const int Original, const int Formatted) {
	// 所有者・群・モードの拡張属性と分けた直接比較
	struct stat OriginalStat {}, FormattedStat {};
	if(
		::fstat(Original, &OriginalStat) || ::fstat(Formatted, &FormattedStat) || OriginalStat.st_uid != FormattedStat.st_uid ||
		OriginalStat.st_gid != FormattedStat.st_gid || (OriginalStat.st_mode & 07777) != (FormattedStat.st_mode & 07777)
		// 属性不一致の返戻
	) return false;
	#if defined(__APPLE__)
	// ファイルの旗・拡張 ACL の不一致の返戻
	if(OriginalStat.st_flags != FormattedStat.st_flags || !HasNoExtendedAcl(Original) || !HasNoExtendedAcl(Formatted)) return false;
	#endif
	// 拡張属性列の取得と比較
	std::vector<std::pair<std::string, std::string>> OriginalAttributes, FormattedAttributes;
	// 属性一致の返戻
	return ReadExtendedAttributes(Original, OriginalAttributes) && ReadExtendedAttributes(Formatted, FormattedAttributes) &&
	OriginalAttributes == FormattedAttributes;
}

#endif

#if !defined(_WIN32)

/**
 * ２つの名前を原子的に交換する関数
 * @param Directory 親ディレクトリ記述子
 * @param Left 一方の名前
 * @param Right 他方の名前
 * @return 交換成功時 true
 */
bool FileIO::ExchangeNames(const int Directory, const Filesystem::path &Left, const Filesystem::path &Right) {
	// 実行環境毎の名前交換
	#if defined(__APPLE__)
	// 交換結果の返戻
	return !::renameatx_np(Directory, Left.c_str(), Directory, Right.c_str(), RENAME_SWAP);
	#elif defined(__linux__)
	// 交換結果の返戻
	return !::syscall(SYS_renameat2, Directory, Left.c_str(), Directory, Right.c_str(), RENAME_EXCHANGE);
	#else
	// 名前の原子的な交換を持たない環境では失敗として扱う
	static_cast<void>(Directory);
	static_cast<void>(Left);
	static_cast<void>(Right);
	errno = ENOTSUP;
	// 交換出来ない事の返戻
	return false;
	#endif
}

/**
 * 名前交換後の同一実体・状態判定関数
 * @param Left 一方の情報
 * @param Right 他方の情報
 * @return 名前変更で変わり得る属性変更日時を除いて一致すれば true
 */
bool FileIO::HasSameFileState(const std::array<uint64_t, 8> &Left, const std::array<uint64_t, 8> &Right) {
	// 状態一致の返戻
	return Left[0] == Right[0] && Left[1] == Right[1] && Left[2] == Right[2] && Left[3] == Right[3] && Left[4] == Right[4] &&
	Left[7] == Right[7];
}

/**
 * 親ディレクトリ同期関数
 * @param Directory 親ディレクトリ記述子
 * @return 同期成功時 true
 */
bool FileIO::SyncDirectory(const int Directory) {
	// 同期対象ディレクトリの再取得
	// 名前交換の永続化対象となる親自身を別記述子で開く
	const int Handle = ::openat(Directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
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
 * ファイル書込関数
 * @param Source 書き出す内容
 * @param IsHardLinkBroken 原本が別名と実体を共有して居た場合に true を設定
 * @param OutError 処理系の失敗の理由を設定（外部の保存との競合・内容の照合の失敗では空の儘とし，errno を当て推量で載せない）
 * @return 書込成功時 true
 */
bool FileIO::WriteFile(const std::string &Source, bool *const IsHardLinkBroken, std::error_code *const OutError) const {
	// 出力状態と失敗理由の初期化
	if(IsHardLinkBroken) *IsHardLinkBroken = false;
	if(OutError) OutError->clear();
	// 親を固定出来ないか，対応する読込が無い場合の失敗返戻
	if(!IsReady || !LastRead) return false;
	std::error_code ErrorCode, WriteError;
	const Filesystem::path Target(TargetPath);
	#if !defined(_WIN32)
	// POSIX 原本の記述子と状態の確保
	// 原本の状態を控える（権限・所有・別名の共有の有無は差替で失われる為，書出前に取る）
	struct stat OrigStat {};
	const Filesystem::path TargetName = Target.filename();
	const int OrigFd = ::openat(ParentDescriptor, TargetName.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
	// 原本確認失敗時の解放・報告前の errno 保存と失敗の返戻
	if(OrigFd < 0) {
		if(OutError) *OutError = std::error_code(errno, std::generic_category());
		// 原本を開けなかった事の返戻
		return false;
	}
	struct OriginalGuard {
		int Descriptor;

		/**
		 * 原本の記述子を解放するデストラクタ
		 */
		~OriginalGuard() {
			// 一時ファイル記述子の解放
			if(Descriptor > -1) CloseFile(Descriptor);
			// 終了
			return;
		}
	} Guard { OrigFd };
	// 原本の同一性・種別・書込権限の検証
	std::array<uint64_t, 8> OriginalMetadata {};
	// 原本が通常ファイルでないか書込不可なら失敗返戻（一時ファイル差替でも利用者の書込禁止を尊重）
	errno = 0;
	if(!ReadMetadata(OrigFd, OriginalMetadata)) {
		if(OutError && errno) *OutError = std::error_code(errno, std::generic_category());
		// 原本の種別又は情報を検証出来ない場合の返戻
		return false;
	}
	if(::fstat(OrigFd, &OrigStat)) {
		if(OutError) *OutError = std::error_code(errno, std::generic_category());
		// 原本の情報を取得出来ない場合の返戻
		return false;
	}
	if(::faccessat(ParentDescriptor, TargetName.c_str(), W_OK, 0)) {
		if(OutError) *OutError = std::error_code(errno, std::generic_category());
		// 原本を書き換えれない場合の返戻
		return false;
	}
	// 別名と実体を共有して居たファイルは差替で切り離され，別名は旧内容の儘取り残される為，呼出側へ知らせる
	if(IsHardLinkBroken) *IsHardLinkBroken = OrigStat.st_nlink > 1;
	#else
	// Windows 原本ハンドルの排他的な確保
	// 書込共有を禁じ，差替が完了する迄原本の実体を保持する
	const NativeFile OrigFd = CreateFileW(
		Target.c_str(),
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr
	);
	// 原本を開けない場合の失敗返戻
	if(OrigFd == INVALID_HANDLE_VALUE) return false;
	struct OriginalGuard {
		NativeFile Handle;

		/**
		 * 原本の記述子を解放するデストラクタ
		 */
		~OriginalGuard() {
			// 原本記述子の解放
			CloseFile(Handle);
			// 終了
			return;
		}
	} Guard { OrigFd };
	if(IsHardLinkBroken) *IsHardLinkBroken = LastRead->Metadata[7] > 1;
	#endif
	// 読込時基準と現在の原本の再照合
	ReadState &Baseline = *LastRead;
	// 読込済の原本が消えた場合や，原本が読込時から変化した場合の失敗返戻
	if(!ReadStableFile(OrigFd, Baseline.Bytes, Baseline.Metadata, true, Baseline.Bytes.size())) return false;
	#if !defined(_WIN32)
	// 複写する権限・所有の取得時点と基準確定時点を揃える
	if(OriginalMetadata != Baseline.Metadata) return false;
	#endif
	// 同一ディレクトリへの排他・リンク追随無の一時書出
	// 一時ファイル名の生成
	static std::atomic<unsigned long> TempSeq(0);
	const Filesystem::path TempPath = Target.parent_path() / (
		".shavefmt-" + std::to_string(GetProcessId()) + "-" + std::to_string(TempSeq.fetch_add(1, std::memory_order_relaxed)) + "-" +
		RandomSuffix() + ".tmp"
	);
	#if defined(_WIN32)
	// 原本アクセス制御の取得
	// 内容を書き始める前に原本のアクセス制御を適用し，公開権限の一時ファイルを作らない
	constexpr SECURITY_INFORMATION SecurityParts =
	OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
	DWORD SecurityBytes = 0;
	if(!GetKernelObjectSecurity(OrigFd, SecurityParts, nullptr, 0, &SecurityBytes)) {
		const DWORD SecurityError = GetLastError();
		// セキュリティ記述子寸法照会以外の失敗返戻
		if(SecurityError != ERROR_INSUFFICIENT_BUFFER) {
			if(OutError) *OutError = std::error_code(static_cast<int>(SecurityError), std::system_category());
			// 寸法照会以外の失敗の返戻
			return false;
		}
	}
	std::vector<unsigned char> Security(SecurityBytes);
	// セキュリティ記述子取得失敗の返戻
	if(!SecurityBytes || !GetKernelObjectSecurity(OrigFd, SecurityParts, Security.data(), SecurityBytes, &SecurityBytes)) {
		// アクセス制御を取得出来ない場合の失敗返戻
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
	// 一時ファイルを作成出来ない場合の失敗返戻
	// 一時ファイル作成失敗の返戻
	if(TempHandle == INVALID_HANDLE_VALUE) return false;
	const int Descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(TempHandle), _O_WRONLY | _O_BINARY);
	// 一時ハンドルの記述子への移管
	if(Descriptor < 0) {
		if(OutError) *OutError = std::error_code(errno, std::generic_category());
		CloseFile(TempHandle);
		if(!Filesystem::remove(TempPath, ErrorCode)) {
			std::fprintf(stderr, "[warn] %s: temporary file left behind\n", ForDisplay(TempPath.string()).c_str());
		}
		// 記述子へ移管出来ない場合の失敗返戻
		return false;
	}
	#else
	// POSIX 一時ファイルの排他的な生成
	const Filesystem::path TempName = TempPath.filename();
	const int Descriptor =
	::openat(ParentDescriptor, TempName.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
	#endif
	// 一時ファイル作成失敗（書込不可・同名の先回り作成等）の返戻（容量不足・中断で原本を壊す直接書込への退避は禁止）
	if(Descriptor < 0) {
		if(OutError) *OutError = std::error_code(errno, std::generic_category());
		// 一時ファイルを作れなかった事の返戻
		return false;
	}
	// 失敗時に用いる一時ファイル削除処理の準備
	const auto RemoveTemp = [&]() -> bool {
		// 遅延処理の実行
		#if defined(_WIN32)
		// 一時ファイルを消せたか，既に存在しない事の返戻
		return Filesystem::remove(TempPath, ErrorCode) || !ErrorCode;
		#else
		// 一時ファイルを消せたか，既に存在しない事の返戻
		return !::unlinkat(ParentDescriptor, TempName.c_str(), 0) || errno == ENOENT;
		#endif
	};
	// 残量が尽きる迄の書出の反復
	bool IsWriteOk = true;
	#if defined(__APPLE__)
	// 一時ファイルの公開前アクセス制御の制限
	// 親から継承した ACL がモード 0600 を越えて読込を許す場合が有る為，内容を書く前に空の ACL へ固定する
	const acl_t TemporaryAccess = ::acl_init(0);
	IsWriteOk = TemporaryAccess && !::acl_set_fd_np(Descriptor, TemporaryAccess, ACL_TYPE_EXTENDED);
	if(!IsWriteOk) WriteError = std::error_code(errno, std::generic_category());
	if(TemporaryAccess) {
		const int SavedError = errno;
		if(::acl_free(TemporaryAccess)) std::fprintf(stderr, "[warn] failed to release ACL (errno %d)\n", errno);
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
	for(size_t Done = 0; IsWriteOk && Done < Source.size();) {
		#if defined(_WIN32)
		const int Wrote = _write(Descriptor, Source.data() + Done, static_cast<unsigned int>(Source.size() - Done));
		#else
		const ssize_t Wrote = ::write(Descriptor, Source.data() + Done, Source.size() - Done);
		#endif
		// 信号に依る中断は失敗ではない為，其の儘書き続ける（一律の失敗扱いは整形結果を黙って捨てる）
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
		if(::fchown(Descriptor, OrigStat.st_uid, OrigStat.st_gid)) {
			IsWriteOk = false;
			WriteError = std::error_code(errno, std::generic_category());
		}
		#if defined(__APPLE__)
		// 削除拒否等のアクセス制御を複写前に検査し，後始末不能と引継不能な権限の破棄を防止
		if(IsWriteOk && !HasNoExtendedAcl(OrigFd)) {
			IsWriteOk = false;
			if(errno) WriteError = std::error_code(errno, std::generic_category());
		}
		#endif
		std::error_code AttributeError;
		// 拡張属性とファイルモードの引継
		// 拡張属性複写失敗の保存結果への反映
		if(IsWriteOk && !CopyExtendedAttributes(OrigFd, Descriptor, AttributeError)) {
			IsWriteOk = false;
			WriteError = AttributeError;
		}
		if(IsWriteOk && ::fchmod(Descriptor, OrigStat.st_mode & 07777)) {
			IsWriteOk = false;
			WriteError = std::error_code(errno, std::generic_category());
		}
		#if defined(__APPLE__)
		// macOS ファイル旗の引継
		if(IsWriteOk && ::fchflags(Descriptor, OrigStat.st_flags)) {
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
	std::array<uint64_t, 8> TempMetadata {};
	#if defined(_WIN32)
	// Windows 一時ファイルの確定と解放
	if(const NativeFile NativeTemp = reinterpret_cast<NativeFile>(_get_osfhandle(Descriptor)); IsWriteOk) {
		IsWriteOk = NativeTemp != INVALID_HANDLE_VALUE && ReadMetadata(NativeTemp, TempMetadata);
	}
	if(IsWriteOk && _commit(Descriptor)) {
		IsWriteOk = false;
		WriteError = std::error_code(errno, std::generic_category());
	}
	if(_close(Descriptor)) {
		// 先行する書込失敗の理由を保ち，重なる解放失敗は別に記録する
		if(IsWriteOk) WriteError = std::error_code(errno, std::generic_category());
		else std::fprintf(stderr, "[warn] failed to close temporary output (errno %d)\n", errno);
		IsWriteOk = false;
	}
	#else
	if(IsWriteOk) IsWriteOk = ReadMetadata(Descriptor, TempMetadata);
	#endif
	if(IsWriteOk) {
		// 公開直前の原本不変性の再確認
		// 同じ記述子の内容だけでは，別実体へ差し替えられた保存を検出出来ない
		IsWriteOk = ReadStableFile(OrigFd, Baseline.Bytes, Baseline.Metadata, true, Baseline.Bytes.size());
		#if defined(_WIN32)
		const NativeFile Current = CreateFileW(
			Target.c_str(),
			FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_OPEN_REPARSE_POINT,
			nullptr
		);
		const bool IsOpened = Current != INVALID_HANDLE_VALUE;
		#else
		const NativeFile Current = ::openat(ParentDescriptor, TargetName.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
		const bool IsOpened = Current > -1;
		#endif
		std::array<uint64_t, 8> CurrentMetadata {};
		// 公開名が読込時と同じ実体を指す事の照合
		IsWriteOk = IsWriteOk && IsOpened && ReadMetadata(Current, CurrentMetadata) && CurrentMetadata == Baseline.Metadata;
		if(IsOpened) CloseFile(Current);
	}
	if(!IsWriteOk) {
		// 未公開一時ファイルの後始末
		// 系呼出の失敗だけ其の場で控えた理由を返し，照合不一致へ残存 errno を当てない
		if(OutError) *OutError = WriteError;
		#if !defined(_WIN32)
		CloseFile(Descriptor);
		#endif
		// 一時ファイルの削除失敗を報告（残留を隠さず，環境依存の `Filesystem::path::c_str` は `string()` で `%s` 用に変換）
		if(!RemoveTemp()) std::fprintf(stderr, "[warn] %s: temporary file left behind\n", ForDisplay(TempPath.string()).c_str());
		// 書出に失敗した一時ファイルを片付けての失敗の返戻
		return false;
	}
	#if defined(ShaveFormatFileIoTestHooks)
	// 公開直前競合を再現する試験フックの実行
	if(TestHook) {
		// 一度だけ実行する競合再現フックの取得
		void (*const Hook)() = TestHook;
		TestHook = nullptr;
		Hook();
	}
	#endif
	bool KeepsTemporary = false;
	#if defined(_WIN32)
	// 競合実体の退避経路
	// 置換候補と復旧用経路の準備
	const Filesystem::path BackupPath = Filesystem::path(TempPath).concat(".backup"),
	RecoveryPath = Filesystem::path(TempPath).concat(".recovery");
	ReadState CandidateState;
	const NativeFile Candidate =
	CreateFileW(TempPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	const bool CandidateRead = Candidate != INVALID_HANDLE_VALUE &&
	ReadStableFile(Candidate, CandidateState.Bytes, CandidateState.Metadata, false, Source.size());
	// 置換候補内容の確定と読込資源の解放
	if(Candidate != INVALID_HANDLE_VALUE) CloseFile(Candidate);
	if(!CandidateRead) {
		// 読込不能な候補の一時ファイル保持
		KeepsTemporary = true;
		std::fprintf(stderr, "[warn] %s: unverified temporary replacement was preserved\n", ForDisplay(TempPath.string()).c_str());
	}
	const bool HasReplaced =
	CandidateRead && ReplaceFileW(Target.c_str(), TempPath.c_str(), BackupPath.c_str(), 0, nullptr, nullptr);
	// 原子的置換結果と退避状態の記録
	const DWORD ReplacementError = CandidateRead && !HasReplaced ? GetLastError() : ERROR_SUCCESS;
	bool IsPublished = HasReplaced, KeepsBackup = !HasReplaced;
	if(IsPublished) {
		// 公開名と退避原本の交換後照合
		#if defined(ShaveFormatFileIoTestHooks)
		if(AfterTestHook) {
			void (*const Hook)() = AfterTestHook;
			AfterTestHook = nullptr;
			Hook();
		}
		#endif
		ReadState Output, Replaced;
		// 公開先と退避先の読込
		const NativeFile Backup =
		CreateFileW(BackupPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		const NativeFile Current =
		CreateFileW(Target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		const bool IsPublishedNameUnchanged =
		Current != INVALID_HANDLE_VALUE && ReadStableFile(Current, Output.Bytes, Output.Metadata, false, Source.size()) &&
		Output.Bytes == CandidateState.Bytes && Output.Metadata[0] == CandidateState.Metadata[0] &&
		Output.Metadata[1] == CandidateState.Metadata[1];
		const bool IsOwnOutput = IsPublishedNameUnchanged && Output.Bytes == Source && Output.Metadata[0] == TempMetadata[0] &&
		Output.Metadata[1] == TempMetadata[1];
		IsPublished = Backup != INVALID_HANDLE_VALUE && IsOwnOutput &&
		ReadStableFile(Backup, Replaced.Bytes, Replaced.Metadata, false, Baseline.Bytes.size()) && Replaced.Bytes == Baseline.Bytes &&
		Replaced.Metadata[0] == Baseline.Metadata[0] && Replaced.Metadata[1] == Baseline.Metadata[1];
		if(Backup != INVALID_HANDLE_VALUE) CloseFile(Backup);
		if(Current != INVALID_HANDLE_VALUE) CloseFile(Current);
		// 公開直前の競合又は一時名の差替を検出した場合は，退避した外部保存を戻す
		if(!IsPublished) {
			KeepsBackup =
			!(IsPublishedNameUnchanged && ReplaceFileW(Target.c_str(), BackupPath.c_str(), RecoveryPath.c_str(), 0, nullptr, nullptr));
			if(KeepsBackup) {
				std::fprintf(stderr, "[warn] %s: concurrent replacement was preserved as a backup\n", ForDisplay(BackupPath.string()).c_str());
			}
		}
	}
	// 置換失敗時に残った退避原本の復旧
	std::error_code StateError;
	if(!HasReplaced) {
		const bool HasBackup = Filesystem::exists(BackupPath, StateError);
		if(StateError) {
			std::fprintf(stderr, "[warn] %s: could not inspect replacement backup\n", ForDisplay(BackupPath.string()).c_str());
		} else if(HasBackup) {
			const bool HasTarget = Filesystem::exists(Target, StateError);
			if(StateError) std::fprintf(stderr, "[warn] %s: could not inspect replacement target\n", ForDisplay(TargetPath).c_str());
			else if(!HasTarget) {
				if(MoveFileExW(BackupPath.c_str(), Target.c_str(), MOVEFILE_WRITE_THROUGH)) KeepsBackup = false;
				else std::fprintf(stderr, "[warn] %s: replacement backup could not be restored\n", ForDisplay(BackupPath.string()).c_str());
			}
			if(KeepsBackup) {
				std::fprintf(stderr, "[warn] %s: failed replacement was preserved as a backup\n", ForDisplay(BackupPath.string()).c_str());
			}
		}
	}
	if(!IsPublished && ReplacementError) ErrorCode = std::error_code(static_cast<int>(ReplacementError), std::system_category());
	// 復旧後に不要となった退避ファイルの削除
	std::error_code RemoveError;
	if(!KeepsBackup && !Filesystem::remove(BackupPath, RemoveError) && RemoveError) {
		std::fprintf(stderr, "[warn] %s: temporary backup left behind\n", ForDisplay(BackupPath.string()).c_str());
	}
	// 復旧用退避が残った場合の利用者への通知
	std::error_code RecoveryError;
	const bool HasRecovery = Filesystem::exists(RecoveryPath, RecoveryError);
	if(RecoveryError) {
		std::fprintf(stderr, "[warn] %s: could not inspect replacement recovery\n", ForDisplay(RecoveryPath.string()).c_str());
	} else if(HasRecovery) {
		std::fprintf(stderr, "[warn] %s: replacement recovery was preserved\n", ForDisplay(RecoveryPath.string()).c_str());
	}
	#else
	// POSIX 置換候補の再読込と内容確定
	const NativeFile Candidate = ::openat(ParentDescriptor, TempName.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
	ReadState CandidateState;
	const bool CandidateRead =
	Candidate > -1 && ReadStableFile(Candidate, CandidateState.Bytes, CandidateState.Metadata, false, Source.size());
	if(Candidate > -1) CloseFile(Candidate);
	if(!CandidateRead) {
		KeepsTemporary = true;
		std::fprintf(stderr, "[warn] %s: unverified temporary replacement was preserved\n", ForDisplay(TempPath.string()).c_str());
	}
	// 原本と一時名の原子的交換と両実体の照合
	bool IsPublished = false;
	int ExchangeFailure = 0;
	if(CandidateRead) {
		// 検証済候補と公開名の交換
		IsPublished = ExchangeNames(ParentDescriptor, TempName, TargetName);
		if(!IsPublished) ExchangeFailure = errno;
	}
	// 名前の交換の失敗の理由は其の場でしか確定しない（後続の照合・復旧・解放が errno を書き換える）
	// 交換後の公開結果と退避原本の照合
	if(IsPublished) {
		#if defined(ShaveFormatFileIoTestHooks)
		if(AfterTestHook) {
			void (*const Hook)() = AfterTestHook;
			AfterTestHook = nullptr;
			Hook();
		}
		#endif
		const NativeFile Current = ::openat(ParentDescriptor, TargetName.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
		const NativeFile Replaced = ::openat(ParentDescriptor, TempName.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
		ReadState CurrentState, ReplacedState;
		// 両実体の内容・状態・付帯属性の一致確認
		IsPublished =
		Current > -1 && Replaced > -1 && ReadStableFile(Current, CurrentState.Bytes, CurrentState.Metadata, false, Source.size()) &&
		HasSameFileState(CurrentState.Metadata, TempMetadata) && CurrentState.Bytes == Source &&
		ReadStableFile(Replaced, ReplacedState.Bytes, ReplacedState.Metadata, false, Baseline.Bytes.size()) &&
		HasSameFileState(ReplacedState.Metadata, Baseline.Metadata) && ReplacedState.Bytes == Baseline.Bytes &&
		HasSameExtendedAttributes(Replaced, Current) && SyncDirectory(ParentDescriptor);
		if(Current > -1) CloseFile(Current);
		if(Replaced > -1) CloseFile(Replaced);
		if(!IsPublished) {
			// 競合検出時の公開名復旧
			const bool IsPublishedNameUnchanged =
			CurrentState.Bytes == CandidateState.Bytes && HasSameFileState(CurrentState.Metadata, CandidateState.Metadata);
			KeepsTemporary = true;
			// 対象名が交換した実体を指す場合だけ，検出した競合を元の名前へ戻す
			// 競合後の公開名復旧失敗の警告
			if(!IsPublishedNameUnchanged || !ExchangeNames(ParentDescriptor, TempName, TargetName)) {
				std::fprintf(stderr, "[warn] %s: concurrent replacement could not be restored\n", ForDisplay(TempPath.string()).c_str());
			} else {
				std::fprintf(stderr, "[warn] %s: replaced output was preserved after a conflict\n", ForDisplay(TempPath.string()).c_str());
			}
			if(!SyncDirectory(ParentDescriptor)) {
				std::fprintf(stderr, "[warn] %s: could not sync replacement recovery\n", ForDisplay(TargetPath).c_str());
			}
		}
	}
	// 公開後の解放失敗は記録し，公開済の結果を原本保持の失敗へ変えない
	// 一時ファイル記述子の最終解放
	if(::close(Descriptor)) {
		// 公開後の記述子解放失敗の保存結果への反映
		const int CloseError = errno;
		std::fprintf(
			stderr,
			"[warn] %s: %s (errno %d)\n",
			ForDisplay(TargetPath).c_str(),
			IsPublished ? "formatted output was published but closing it failed" : "failed to close temporary output",
			CloseError
		);
	}
	// 名前の交換の失敗だけが処理系の理由を持つ（内容の照合の不一致は errno を持たず，此処へ来る迄に close 等が上書きして居る）
	if(!IsPublished && ExchangeFailure) ErrorCode = std::error_code(ExchangeFailure, std::generic_category());
	#endif
	// 公開失敗時の保存物とエラー状態の整理
	if(!IsPublished) {
		// 系呼出の失敗だけ理由を返し，競合・照合の不一致は理由を持たない失敗として区別する
		if(OutError && ErrorCode) *OutError = ErrorCode;
		if(!KeepsTemporary && !RemoveTemp()) {
			std::fprintf(stderr, "[warn] %s: temporary file left behind\n", ForDisplay(TempPath.string()).c_str());
		}
		// 差替又は公開後の照合に失敗した事の返戻（復旧不能時の残留先は上で報告）
		return false;
	}
	#if !defined(_WIN32)
	// 交換済原本の一時名からの除去
	if(!RemoveTemp()) std::fprintf(stderr, "[warn] %s: replaced original left behind\n", ForDisplay(TempPath.string()).c_str());
	#endif
	LastRead.reset();
	// 書込成功の返戻
	return true;
}
