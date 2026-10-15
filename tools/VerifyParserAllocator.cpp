#include "Util/FileIO.hpp"
#include "Util/Lang.hpp"
#include "Util/ParserAllocator.hpp"
#include <tree_sitter/api.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#if defined(_WIN32)

#include <fcntl.h>
#include <io.h>

#endif

// 書戻が公開の直前又は直後へ到達した事の，注入スレッドへの通知
static std::atomic_flag IsAtPublication = ATOMIC_FLAG_INIT;

/**
 * 公開区間で他スレッドの確保失敗を待つテスト用フック
 * 計算量：非常経路開始迄の待機回数 Q に対し O(Q)
 */
static void AwaitAllocationFailure() {
	// 書戻区間への到達通知と，確保失敗の注入待
	IsAtPublication.test_and_set();
	while(!ParserAllocator::IsFailing()) std::this_thread::yield();
	// 終了
	return;
}

/**
 * 確保失敗の注入を受ける子プロセスの入口
 * 計算量：原文長 B と解析又は書戻の費用 Q に対し O(B + Q)
 * @param Argc モード・確保回数・言語・書戻先を含む引数の数
 * @param Argv 子プロセスの引数
 * @return 注入されなかった正常経路は０，入力又はテスト条件の不正は２（確保失敗は非常経路で１）
 */
int main(const int Argc, char *const Argv[]) {
	// 原文のバイト列を変えない標準入出力の設定
	#if defined(_WIN32)
	// バイナリ入出力を設定出来ない場合の返戻
	if(_setmode(_fileno(stdin), _O_BINARY) < 0 || _setmode(_fileno(stdout), _O_BINARY) < 0) return 2;
	#endif
	// 必須引数が無い場合の返戻
	if(Argc < 4) return 2;
	const std::string_view Mode(Argv[1]);
	// 注入する前に，全原文を保持する
	std::string Original;
	char Chunk[4096];
	while(const size_t Count = std::fread(Chunk, 1, sizeof(Chunk), stdin)) Original.append(Chunk, Count);
	// 原文の読込が失敗した場合の返戻
	if(std::ferror(stdin)) return 2;
	ParserAllocator::Install();
	if(Mode == "cancel-before" || Mode == "cancel-after" || Mode == "cancel-nested") {
		// 書戻先が無いテスト条件の不正の返戻
		if(Argc != 5) return 2;
		const FileIO File(Argv[4]);
		std::string Src;
		// 中断前の読込に失敗したテスト条件の不正の返戻
		if(!File.IsFileReadSuccessful(Src)) return 2;
		if(Mode == "cancel-nested") {
			const ParserAllocator::WriteGuard Completion;
			// 外側の区間を開始出来なかった検査失敗の返戻
			if(!Completion.CanWrite()) return 2;
			ParserAllocator::RequestInterruption();
			// 内側で中断を再照合して公開を拒否した検査失敗の返戻
			if(!File.IsFileWriteSuccessful("int X;\n")) return 2;
		} else {
			if(Mode == "cancel-before") FileIO::SetTestHook(ParserAllocator::RequestInterruption);
			else FileIO::SetAfterTestHook(ParserAllocator::RequestInterruption);
			// 開始済の公開を完了出来なかった検査失敗の返戻
			if(!File.IsFileWriteSuccessful("int X;\n")) return 2;
		}
		std::error_code Error;
		// 中断後の新しい書戻を拒否出来なかった検査失敗の返戻
		if(File.IsFileWriteSuccessful("int Y;\n", nullptr, &Error) || Error != std::errc::operation_canceled) return 2;
		// 完了した公開を保持し，新しい書戻を拒否した検査成功の返戻
		return ParserAllocator::IsInterrupted() ? 0 : 2;
	}
	if(Mode == "write-before" || Mode == "write-after") {
		// 書戻先が指定されて居ない場合の返戻
		if(Argc != 5) return 2;
		const std::string FailedPath = std::string(Argv[4]) + ".parsing.cpp", SafeName = FileIO::ForDisplay(FailedPath);
		const FileIO FailedFile(FailedPath);
		std::string FailedSource;
		// 解析する原本を読み出せないテスト条件の不正の返戻
		if(!FailedFile.IsFileReadSuccessful(FailedSource)) return 2;
		const ParserAllocator::DisplayGuard Display(SafeName);
		ParserAllocator::SetSummary(2, Mode == "write-before", true, 3, 4, 2);
		const FileIO File(Argv[4]);
		std::string Src;
		// 書戻の読込基準を取得出来ない場合の返戻
		if(!File.IsFileReadSuccessful(Src)) return 2;
		if(Mode == "write-before") FileIO::SetTestHook(AwaitAllocationFailure);
		else FileIO::SetAfterTestHook(AwaitAllocationFailure);
		std::thread Writer(
			[&]() -> void {
				// 公開と後始末，完了数の計上は此の書戻自身に完了させる
				const ParserAllocator::WriteGuard Completion;
				if(!Completion.CanWrite() || !File.IsFileWriteSuccessful("int X;\n")) std::_Exit(2);
				ParserAllocator::RecordResult({ .Changed = 1, .WriteNeeded = 1 });
				// 終了
				return;
			}
		);
		while(!IsAtPublication.test()) std::this_thread::yield();
		ParserAllocator::SetFailureCountdown(0);
		ts_parser_delete(ts_parser_new());
		Writer.join();
		// 非常終了へ入らなかった異常の返戻
		return 2;
	}
	const ParserAllocator::DisplayGuard Display("<stdin>");
	ParserAllocator::SetStdin(Original);
	const TSLanguage *const Grammar = Lang::FromName(Argv[3]).GetTreeSitterLang();
	// 対象文法が無い場合の返戻
	if(!Grammar) return 2;
	const size_t Countdown = static_cast<size_t>(std::strtoull(Argv[2], nullptr, 10));
	// パーサー生成と外部スキャナー生成・解析の双方へ，異なる確保位置を注入する
	if(Mode == "parser") ParserAllocator::SetFailureCountdown(Countdown);
	TSParser *const Parser = ts_parser_new();
	if(Mode == "parse") ParserAllocator::SetFailureCountdown(Countdown);
	// 文法を設定出来ない場合の返戻
	if(!ts_parser_set_language(Parser, Grammar)) return 2;
	TSTree *const Tree = ts_parser_parse_string(Parser, nullptr, Original.data(), static_cast<uint32_t>(Original.size()));
	// 解析が完了しない場合の返戻
	if(!Tree) return 2;
	ts_tree_delete(Tree);
	ts_parser_delete(Parser);
	ParserAllocator::SetStdin(std::string_view());
	// 注入されなかった経路も，手記述の原文を其の儘返す
	return std::fwrite(Original.data(), 1, Original.size(), stdout) == Original.size() && !std::fflush(stdout) ? 0 : 2;
}
