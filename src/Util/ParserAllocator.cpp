#include "ParserAllocator.hpp"
#include "FormatLimitExceeded.hpp"
#include <tree_sitter/api.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <thread>

#if defined(_WIN32)

#include <windows.h>

#else

#include <unistd.h>

#endif

thread_local size_t ParserAllocator::WriteDepth = 0;
thread_local std::string_view ParserAllocator::DisplayName;
std::atomic_flag ParserAllocator::WasInterrupted = ATOMIC_FLAG_INIT;
// 非常終了の選出と公開中の書戻の完了監視（非常経路ではロックを使わない）
std::atomic<bool> ParserAllocator::HasFailed(false);
std::atomic<size_t> ParserAllocator::ActiveWrites(0);
CompletionCounts<std::atomic<size_t>> ParserAllocator::Completed;
size_t ParserAllocator::ReadFailures = 0;
size_t ParserAllocator::TotalFiles = 0;
size_t ParserAllocator::ExcludedByName = 0;
size_t ParserAllocator::UnfollowedLinks = 0;
bool ParserAllocator::ShouldCheck = false;
bool ParserAllocator::ShouldWrite = false;
std::string_view ParserAllocator::StdinText;

static_assert(std::atomic<bool>::is_always_lock_free && std::atomic<size_t>::is_always_lock_free);

#if defined(ShaveFormatParserAllocationTestHooks)

// 確保の注入は他のスレッドの書戻を妨げない
thread_local size_t ParserAllocator::FailureCountdown = std::numeric_limits<size_t>::max();

/**
 * 確保失敗の注入関数
 * 計算量：O(1)
 * @return 此の確保を失敗させるなら true
 */
bool ParserAllocator::ShouldFailAllocation() {
	// 注入を無効にした場合の返戻
	if(FailureCountdown == std::numeric_limits<size_t>::max()) return false;
	// 注入位置に達した事の返戻
	if(!FailureCountdown) return true;
	--FailureCountdown;
	// 次の確保位置迄継続する事の返戻
	return false;
}

/**
 * 確保失敗迄の回数の設定関数
 * 計算量：O(1)
 * @param Count 失敗前に成功させる確保回数
 */
void ParserAllocator::SetFailureCountdown(const size_t Count) {
	FailureCountdown = Count;
	// 終了
	return;
}

/**
 * 非常経路の開始の照合関数
 * 計算量：O(1)
 * @return 非常終了を開始して居れば true
 */
bool ParserAllocator::IsFailing() {
	return HasFailed.load();
}

#endif

/**
 * 動的確保を行わない非常書出関数
 * 計算量：本文長 B に対し O(B)
 * @param Text 書き出す保持済の本文
 * @param Descriptor 標準出力又は標準エラーの記述子
 * @return 全バイトを書ければ true
 */
bool ParserAllocator::IsEmergencyWriteSuccessful(const std::string_view Text, const int Descriptor) {
	// 短い書込と割込を扱う固定領域からの書出
	for(size_t Pos = 0; Pos < Text.size();) {
		#if defined(_WIN32)
		DWORD Count = 0;
		// CRT の遅延確保を避け，処理系の記述子へ直接バイト列を渡す
		if(
			!WriteFile(
				GetStdHandle(Descriptor == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE),
				Text.data() + Pos,
				static_cast<DWORD>(std::min(Text.size() - Pos, size_t{ 0X10000 })),
				&Count,
				nullptr
			)
		// 非常書出に失敗した事の返戻
		) return false;
		#else
		const ssize_t Count = ::write(Descriptor, Text.data() + Pos, Text.size() - Pos);
		if(Count < 0 && errno == EINTR) continue;
		#endif
		// 書出が失敗又は進行しない場合の返戻
		if(Count < 1) return false;
		Pos += static_cast<size_t>(Count);
	}
	// 全バイト書出成功の返戻
	return true;
}

/**
 * 件数を固定長の領域から書き出す関数
 * 計算量：Count の桁数 D に対し O(D)
 * @param Count 書き出す件数
 * @param Descriptor 標準ストリームの記述子
 * @return 全桁を書ければ true
 */
bool ParserAllocator::WriteCount(const size_t Count, const int Descriptor) {
	std::array<char, std::numeric_limits<size_t>::digits10 + 1> Buffer;
	size_t Pos = Buffer.size(), Rest = Count;
	do {
		Buffer[--Pos] = static_cast<char>('0' + Rest % 10);
		Rest /= 10;
	} while(Rest);
	// 全桁の書出結果の返戻
	return IsEmergencyWriteSuccessful(std::string_view(Buffer.data() + Pos, Buffer.size() - Pos), Descriptor);
}

/**
 * 通常時と非常時に，同じ件数・書式で要約を書き出す関数
 * メモリとロックを使わず，呼出側が通常の標準出力のバッファを先に書き出す
 * 計算量：件数の桁数 D に対し O(D)
 * @param Summary 完了数・失敗数と入口・除外の条件
 * @return 全本文を書ければ true
 */
bool ParserAllocator::WriteSummary(const FormattingSummary &Summary) {
	// 入口の出力先と，数値も同じ記述子へ出す条件
	const int Descriptor = Summary.Kind == SummaryKind::NoSupportedCheck ? 2 : 1;
	bool IsSuccessful = false;
	if(Summary.Kind == SummaryKind::NoSupportedCheck) {
		IsSuccessful = IsEmergencyWriteSuccessful("Error: no supported files found\n", Descriptor);
	} else if(Summary.Kind == SummaryKind::NoSupportedFiles) {
		IsSuccessful = IsEmergencyWriteSuccessful("No supported files found", Descriptor);
	} else if(Summary.Kind == SummaryKind::Check) {
		IsSuccessful =
		IsEmergencyWriteSuccessful("Formatting check: ", Descriptor) && WriteCount(Summary.Counts.Unchanged, Descriptor) &&
		IsEmergencyWriteSuccessful(" unchanged, ", Descriptor) && WriteCount(Summary.Counts.Changed, Descriptor) &&
		IsEmergencyWriteSuccessful(" would change, ", Descriptor) && WriteCount(Summary.Counts.Partial, Descriptor) &&
		IsEmergencyWriteSuccessful(" partial, ", Descriptor) && WriteCount(Summary.Counts.Skipped, Descriptor) &&
		IsEmergencyWriteSuccessful(" skipped (not checked), ", Descriptor) && WriteCount(Summary.Failed, Descriptor) &&
		IsEmergencyWriteSuccessful(" failed, ", Descriptor) && WriteCount(Summary.Counts.Warnings, Descriptor) &&
		IsEmergencyWriteSuccessful(" warning(s)", Descriptor);
	} else {
		IsSuccessful = IsEmergencyWriteSuccessful("Done. ", Descriptor) && WriteCount(Summary.Counts.WriteNeeded, Descriptor) &&
		IsEmergencyWriteSuccessful("/", Descriptor) && WriteCount(Summary.Total, Descriptor) &&
		IsEmergencyWriteSuccessful(Summary.ShouldWrite ? " file(s) formatted" : " file(s) would be formatted", Descriptor);
		if(Summary.NotWritten) {
			IsSuccessful = IsSuccessful && IsEmergencyWriteSuccessful(", ", Descriptor) && WriteCount(Summary.NotWritten, Descriptor) &&
			IsEmergencyWriteSuccessful(" file(s) not written because a named path could not be used", Descriptor);
		}
		if(Summary.Failed) {
			IsSuccessful = IsSuccessful && IsEmergencyWriteSuccessful(", ", Descriptor) && WriteCount(Summary.Failed, Descriptor) &&
			IsEmergencyWriteSuccessful(" file(s) could not be processed", Descriptor);
		}
		if(Summary.ReadFailures) {
			IsSuccessful = IsSuccessful && IsEmergencyWriteSuccessful(", ", Descriptor) && WriteCount(Summary.ReadFailures, Descriptor) &&
			IsEmergencyWriteSuccessful(" path(s) could not be read", Descriptor);
		}
		if(Summary.Counts.Skipped) {
			IsSuccessful = IsSuccessful && IsEmergencyWriteSuccessful(", ", Descriptor) && WriteCount(Summary.Counts.Skipped, Descriptor) &&
			IsEmergencyWriteSuccessful(" file(s) skipped", Descriptor);
		}
		if(Summary.Counts.Warnings) {
			IsSuccessful =
			IsSuccessful && IsEmergencyWriteSuccessful(", ", Descriptor) && WriteCount(Summary.Counts.Warnings, Descriptor) &&
			IsEmergencyWriteSuccessful(" warning(s)", Descriptor);
		}
	}
	// 検査対象無だけは，各除外件数を標準エラーの独立行へ出す
	const bool IsSeparate = Summary.Kind == SummaryKind::NoSupportedCheck;
	if(Summary.ExcludedByName) {
		IsSuccessful = IsSuccessful && IsEmergencyWriteSuccessful(IsSeparate ? "[notice] " : ", ", Descriptor) &&
		WriteCount(Summary.ExcludedByName, Descriptor) && IsEmergencyWriteSuccessful(" path(s) excluded by name", Descriptor) &&
		(!IsSeparate || IsEmergencyWriteSuccessful("\n", Descriptor));
	}
	if(Summary.UnfollowedLinks) {
		IsSuccessful = IsSuccessful && IsEmergencyWriteSuccessful(IsSeparate ? "[notice] " : ", ", Descriptor) &&
		WriteCount(Summary.UnfollowedLinks, Descriptor) && IsEmergencyWriteSuccessful(" symbolic link(s) not followed", Descriptor) &&
		(!IsSeparate || IsEmergencyWriteSuccessful("\n", Descriptor));
	}
	// 全本文と，通常の要約だけに付く終端の書出結果の返戻
	return IsSuccessful && (IsSeparate || IsEmergencyWriteSuccessful(".\n", Descriptor));
}

/**
 * 原文返戻と公開済の後始末を保全する非常終了関数
 * C のパーサーと外部スキャナーの途中状態を巻き戻さず，１つの失敗スレッドだけが終了を担当する
 * 計算量：進行中の書戻の費用 Q と保持済原文長 B に対し O(Q + B)
 */
[[noreturn]] void ParserAllocator::Fail() {
	// 新しい書戻の遮断と，非常終了するスレッドの選出
	if(HasFailed.exchange(true)) while(true) std::this_thread::yield();
	// 一時名を作った書戻自身に，公開・復旧・後始末を完了させる
	while(ActiveWrites.load()) std::this_thread::yield();
	const FailureReason Reason(FailureReason::ResourceFailure);
	IsEmergencyWriteSuccessful("Error: formatting could not be completed (", 2);
	IsEmergencyWriteSuccessful(Reason.GetText(), 2);
	IsEmergencyWriteSuccessful(")", 2);
	if(!DisplayName.empty()) {
		IsEmergencyWriteSuccessful(", left unchanged: ", 2);
		IsEmergencyWriteSuccessful(DisplayName, 2);
	}
	IsEmergencyWriteSuccessful("\n[notice] ", 2);
	if(!DisplayName.empty()) {
		IsEmergencyWriteSuccessful(DisplayName, 2);
		IsEmergencyWriteSuccessful(": ", 2);
	}
	IsEmergencyWriteSuccessful(Reason.GetRemedy(), 2);
	IsEmergencyWriteSuccessful("\n", 2);
	if(!IsEmergencyWriteSuccessful(StdinText, 1)) IsEmergencyWriteSuccessful("Error: failed to write stdout\n", 2);
	if(TotalFiles) {
		// 未完了は成功へ含めず，保持済の分類だけを同じ要約へ渡す
		FormattingSummary Summary;
		Summary.Counts.Unchanged = Completed.Unchanged.load();
		Summary.Counts.Changed = Completed.Changed.load();
		Summary.Counts.Partial = Completed.Partial.load();
		Summary.Counts.Skipped = Completed.Skipped.load();
		Summary.Counts.Warnings = Completed.Warnings.load();
		Summary.Counts.WriteNeeded = Completed.WriteNeeded.load();
		Summary.Total = TotalFiles;
		Summary.Failed = TotalFiles -
		std::min(TotalFiles, Summary.Counts.Unchanged + Summary.Counts.Changed + Summary.Counts.Partial + Summary.Counts.Skipped);
		Summary.ReadFailures = ReadFailures;
		Summary.ExcludedByName = ExcludedByName;
		Summary.UnfollowedLinks = UnfollowedLinks;
		Summary.Kind = ShouldCheck ? SummaryKind::Check : SummaryKind::Done;
		Summary.ShouldWrite = ShouldWrite;
		if(!WriteSummary(Summary)) IsEmergencyWriteSuccessful("Error: failed to write stdout\n", 2);
	}
	std::_Exit(1);
}

/**
 * tree-sitter の確保関数
 * 計算量：libc の確保費用 Q に対し O(Q)
 * @param Size 要求バイト数
 * @return libc と互換な領域（失敗は非常終了）
 */
void *ParserAllocator::Allocate(const size_t Size) {
	#if defined(ShaveFormatParserAllocationTestHooks)
	if(ShouldFailAllocation()) Fail();
	#endif
	void *const Ptr = std::malloc(Size);
	if(!Ptr && Size) Fail();
	// 確保した領域の返戻
	return Ptr;
}

/**
 * tree-sitter の零初期化付の確保関数
 * 計算量：libc の確保費用 Q に対し O(Q)
 * @param Count 要素数
 * @param Size 要素のバイト数
 * @return libc と互換な領域（失敗は非常終了）
 */
void *ParserAllocator::AllocateZeroed(const size_t Count, const size_t Size) {
	#if defined(ShaveFormatParserAllocationTestHooks)
	if(ShouldFailAllocation()) Fail();
	#endif
	void *const Ptr = std::calloc(Count, Size);
	if(!Ptr && Count && Size) Fail();
	// 零初期化済の領域の返戻
	return Ptr;
}

/**
 * tree-sitter の再確保関数
 * 計算量：libc の再確保費用 Q に対し O(Q)
 * @param Ptr 変更する既存領域
 * @param Size 要求バイト数
 * @return libc と互換な領域（失敗は非常終了）
 */
void *ParserAllocator::Reallocate(void *const Ptr, const size_t Size) {
	#if defined(ShaveFormatParserAllocationTestHooks)
	if(ShouldFailAllocation()) Fail();
	#endif
	void *const Result = std::realloc(Ptr, Size);
	if(!Result && Size) Fail();
	// 再確保した領域の返戻
	return Result;
}

/**
 * 確保器を最初のパーサー生成前に登録する関数
 * 既存領域も libc で解放出来る為，登録前の領域との互換を保つ
 * 計算量：O(1)
 */
void ParserAllocator::Install() {
	// C++ の静的ローカル初期化で，並列のパーサー生成より先に１度だけ登録する
	static const bool IsInstalled = []() -> bool {
		ts_set_allocator(Allocate, AllocateZeroed, Reallocate, std::free);
		return true;
	}();
	static_cast<void>(IsInstalled);
	// 終了
	return;
}

/**
 * 非常返戻する標準入力の原文の登録関数
 * 計算量：O(1)
 * @param Text 解析前に全バイトを保持した原文（登録解除迄寿命を保つ）
 */
void ParserAllocator::SetStdin(const std::string_view Text) {
	StdinText = Text;
	// 終了
	return;
}

/**
 * 此のスレッドの書戻の範囲の判定関数
 * 計算量：O(1)
 * @return 公開と後始末を保全する範囲を保持中なら true
 */
bool ParserAllocator::IsWriting() {
	return static_cast<bool>(WriteDepth);
}

/**
 * 中断要求の照合関数
 * 計算量：O(1)
 * @return 中断を要求済なら true
 */
bool ParserAllocator::IsInterrupted() {
	return WasInterrupted.test();
}

/**
 * 新しい処理の遮断の判定関数
 * 計算量：O(1)
 * @return 中断又は非常終了を開始済なら true
 */
bool ParserAllocator::IsStopping() {
	return IsInterrupted() || HasFailed.load();
}

/**
 * 非常終了との競合を照合して書戻区間へ入る関数
 * 外側の区間を保持して居れば継続し，新しい区間だけ遮断と照合する
 * 計算量：O(1)
 * @return 非常終了前に開始出来たなら true
 */
bool ParserAllocator::CanEnterWrite() {
	// 遮断との競合を含む書戻区間への進入
	ActiveWrites.fetch_add(1);
	const bool IsEntered = IsWriting() || !HasFailed.load() && !IsInterrupted();
	if(!IsEntered) ActiveWrites.fetch_sub(1);
	// 非常終了前に開始出来たかの返戻
	return IsEntered;
}

/**
 * 非常終了前に開始した書戻区間を保持するコンストラクタ
 * 計算量：O(1)
 */
ParserAllocator::WriteGuard::WriteGuard() : IsEntered(CanEnterWrite()) {
	if(IsEntered) ++WriteDepth;
	// 終了
	return;
}

/**
 * 非常終了前に開始済の書戻かの判定関数
 * 計算量：O(1)
 * @return 非常終了前に区間へ入れたなら true
 */
bool ParserAllocator::WriteGuard::CanWrite() const {
	return IsEntered;
}

/**
 * 公開・復旧・後始末の完了を通知するデストラクタ
 * 計算量：O(1)
 */
ParserAllocator::WriteGuard::~WriteGuard() {
	// 開始した書戻区間の完了通知
	if(IsEntered) {
		--WriteDepth;
		ActiveWrites.fetch_sub(1);
	}
	// 終了
	return;
}

/**
 * 中断要求の登録関数
 * 計算量：O(1)
 */
void ParserAllocator::RequestInterruption() {
	WasInterrupted.test_and_set();
	// 終了
	return;
}

/**
 * 書戻区間以外の処理の中断関数
 * 計算量：通常 O(1)，非常終了後は其の完了迄待機
 * @throws ProcessingInterrupted 中断を要求済の場合
 */
void ParserAllocator::CheckInterruption() {
	// 確保失敗後には例外の領域も求めず，非常終了を担当するスレッドを待つ
	while(HasFailed.load()) std::this_thread::yield();
	if(IsInterrupted()) throw ProcessingInterrupted();
	// 終了
	return;
}

/**
 * 非常経路へ渡す表示名の登録関数
 * 計算量：O(1)
 * @param Name 此のスレッドの解析が完了する迄寿命を保つ無害化済の表示名
 */
void ParserAllocator::SetDisplayName(const std::string_view Name) {
	DisplayName = Name;
	// 終了
	return;
}

/**
 * 解析前に非常用の要約を初期化する関数
 * 計算量：O(1)
 * @param Count 全対象の数
 * @param IsCheck 検査の要約を使うか
 * @param IsWrite 書戻の要約を使うか
 * @param ByName 名前で除いたパス数
 * @param Links 辿らなかったリンク数
 * @param ReadErrors 名指し又は走査中に報告した読込失敗数
 */
void ParserAllocator::SetSummary(
	const size_t Count,
	const bool IsCheck,
	const bool IsWrite,
	const size_t ByName,
	const size_t Links,
	const size_t ReadErrors
) {
	TotalFiles = Count;
	ReadFailures = ReadErrors;
	ExcludedByName = ByName;
	UnfollowedLinks = Links;
	ShouldCheck = IsCheck;
	ShouldWrite = IsWrite;
	Completed.Unchanged.store(0);
	Completed.Changed.store(0);
	Completed.Partial.store(0);
	Completed.Skipped.store(0);
	Completed.Warnings.store(0);
	Completed.WriteNeeded.store(0);
	// 終了
	return;
}

/**
 * 非常用の分類別の完了数へ足す関数
 * 計算量：O(1)
 * @param Counts 完了した分類・表示済の警告・差分の件数
 */
void ParserAllocator::RecordResult(const ResultCounts &Counts) {
	Completed.Unchanged.fetch_add(Counts.Unchanged);
	Completed.Changed.fetch_add(Counts.Changed);
	Completed.Partial.fetch_add(Counts.Partial);
	Completed.Skipped.fetch_add(Counts.Skipped);
	Completed.Warnings.fetch_add(Counts.Warnings);
	Completed.WriteNeeded.fetch_add(Counts.WriteNeeded);
	// 終了
	return;
}

/**
 * 表示名を登録するコンストラクタ
 * 計算量：O(1)
 * @param Name 所有者より長く生きる無害化済の表示名
 */
ParserAllocator::DisplayGuard::DisplayGuard(const std::string_view Name) {
	SetDisplayName(Name);
	// 終了
	return;
}

/**
 * 表示名の参照を解除するデストラクタ
 * 計算量：O(1)
 */
ParserAllocator::DisplayGuard::~DisplayGuard() {
	SetDisplayName(std::string_view());
	// 終了
	return;
}
