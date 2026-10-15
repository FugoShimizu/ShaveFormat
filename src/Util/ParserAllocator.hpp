#pragma once

#include <atomic>
#include <cstddef>
#include <string_view>

// 書戻区間以外の処理を中断する合図
struct ProcessingInterrupted {};

// 完了した分類・表示済の警告・本文の差分の件数（通常値とアトミックな値で同じ名前を使う）
template<typename Counter> struct CompletionCounts {
	Counter Unchanged { 0 }; // 変更の無い対象数
	Counter Changed { 0 }; // 変更の在る対象数
	Counter Partial { 0 }; // 部分整形の対象数
	Counter Skipped { 0 }; // 未検査の見送数
	Counter Warnings { 0 }; // 表示済の警告数
	Counter WriteNeeded { 0 }; // 本文に差分が在る対象数
};

using ResultCounts = CompletionCounts<size_t>; // 完了した１件又は全対象の通常の計数

enum class SummaryKind : unsigned char { Check, Done, NoSupportedCheck, NoSupportedFiles }; // 要約の入口 // 検査 // 通常又は書戻 // 検査対象無のエラー // 通常の対応対象無

struct FormattingSummary {
	ResultCounts Counts; // 完了した対象と表示した警告の数
	size_t Total = 0; // 重複を除いた全対象数
	size_t Failed = 0; // 未完了を含む失敗数
	size_t NotWritten = 0; // 名指したパスの不備で書戻を開始しなかった数
	size_t ReadFailures = 0; // 名指し又は走査中に報告した読込失敗数
	size_t ExcludedByName = 0; // 名前で除いたパス数
	size_t UnfollowedLinks = 0; // 辿らなかったリンク数
	SummaryKind Kind = SummaryKind::Done; // 入口に従う書式と出力先
	bool ShouldWrite = false; // 書戻の実行か
}; // 通常時と非常時で同じ書出へ渡す，確保を伴わない要約

// パーサーの確保失敗時に原文と書戻を保全する非常経路
class ParserAllocator {
private:

	#if defined(ShaveFormatParserAllocationTestHooks)

	static thread_local size_t FailureCountdown; // 此のスレッドで注入位置迄に残る確保回数

	#endif

	static thread_local size_t WriteDepth; // 此のスレッドで保持中の書戻の範囲の深さ
	static thread_local std::string_view DisplayName; // 解析前に保持した此のスレッドの表示名
	static std::atomic_flag WasInterrupted; // 書戻以外の処理への中断要求
	static std::atomic<bool> HasFailed; // 非常終了を担当するスレッドの選出
	static std::atomic<size_t> ActiveWrites; // 公開・後始末を完了して居ない書戻数
	static CompletionCounts<std::atomic<size_t>> Completed; // 完了した分類と表示済の警告・変更件数
	static size_t ReadFailures; // 事前に確定した収集時の読込失敗数
	static size_t TotalFiles; // 事前に確定した対象数
	static size_t ExcludedByName; // 名前で除いたパス数
	static size_t UnfollowedLinks; // 辿らなかったリンク数
	static bool ShouldCheck; // 非常時に検査の要約を出すか
	static bool ShouldWrite; // 書戻の要約を出すか
	static std::string_view StdinText; // 非常返戻する保持済の原文

	#if defined(ShaveFormatParserAllocationTestHooks)

	static bool ShouldFailAllocation(); // 此の確保を失敗させるかの判定関数

	#endif

	static void SetDisplayName(const std::string_view Name); // 解析前に保持した表示名の登録関数
	static bool IsEmergencyWriteSuccessful(const std::string_view Text, const int Descriptor); // 確保を伴わない標準ストリームへの書出関数
	static bool WriteCount(const size_t Count, const int Descriptor); // 固定長領域から件数を書き出す関数
	static bool CanEnterWrite(); // 非常終了との競合を照合して書戻区間へ入る関数
	[[noreturn]] static void Fail(); // 原文返戻と進行中の書戻の完了待を行う非常終了関数
	static void *Allocate(const size_t Size); // tree-sitter の確保関数
	static void *AllocateZeroed(const size_t Count, const size_t Size); // tree-sitter の零初期化付の確保関数
	static void *Reallocate(void *const Ptr, const size_t Size); // tree-sitter の再確保関数

public:

	// 非常終了との競合下でも，公開と後始末の全区間を保持する所有者
	class WriteGuard {
	private:

		const bool IsEntered; // 書戻区間へ入れたか

	public:

		bool CanWrite() const; // 非常終了前に開始済の書戻かの判定関数

		WriteGuard &operator=(const WriteGuard &) = delete; // 書戻区間の代入禁止
		WriteGuard(const WriteGuard &) = delete; // 書戻区間の複写禁止

		WriteGuard(); // 書戻区間への進入を試みるコンストラクタ
		~WriteGuard(); // 書戻区間の完了を通知するデストラクタ
	};

	class DisplayGuard {
	public:

		DisplayGuard &operator=(const DisplayGuard &) = delete; // 表示名の所有者の代入禁止

		explicit DisplayGuard(const std::string_view Name); // 保持済の表示名を登録するコンストラクタ

		DisplayGuard(const DisplayGuard &) = delete; // 表示名の所有者の複写禁止

		~DisplayGuard(); // 参照先の破棄前に表示名を解除するデストラクタ
	}; // 此のスレッドの非常用表示名の寿命を保持する所有者

	static void SetSummary(
		const size_t Count,
		const bool IsCheck,
		const bool IsWrite,
		const size_t ByName = 0,
		const size_t Links = 0,
		const size_t ReadErrors = 0
	); // 解析前に非常用の要約の条件を設定する関数

	static bool WriteSummary(const FormattingSummary &Summary); // 通常時と非常時に確保せず同じ要約を書き出す関数
	static void RecordResult(const ResultCounts &Counts); // 完了した結果の分類を非常用の計数へ足す関数
	static void Install(); // libc と互換な確保器を１度だけ登録する関数
	static void RequestInterruption(); // 確保とロックを伴わず中断を要求する関数
	static bool IsWriting(); // 此のスレッドで書戻の区間を保持中かの判定関数
	static bool IsInterrupted(); // 中断要求の照合関数
	static bool IsStopping(); // 中断又は非常終了に依る新しい処理の遮断の判定関数
	static void CheckInterruption(); // 書戻区間以外で中断を送出する関数
	static void SetStdin(const std::string_view Text); // 非常返戻する保持済の原文の登録関数（標準入力の主スレッド専用）

	#if defined(ShaveFormatParserAllocationTestHooks)

	static void SetFailureCountdown(const size_t Count); // 次の確保失敗迄の回数を此のスレッドだけへ注入する関数
	static bool IsFailing(); // 非常経路の開始を照合する関数

	#endif

	ParserAllocator() = delete; // インスタンスの生成禁止
};
