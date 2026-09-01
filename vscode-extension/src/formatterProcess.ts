import * as childProcess from "child_process";
import type { CancellationToken, OutputChannel } from "vscode";

// 実行結果と，結果を適用しない終了の区別
type ProcessResult = { kind: "completed", code: number | null, signal: NodeJS.Signals | null, stdout: Buffer, stderr: string } |
{ kind: "busy" | "cancelled" | "overflow" | "spawnError" };

export const MAX_INPUT_BYTES = 0X1000000; // 整形器の入力上限（16 MiB）

// 待機処理
interface PendingProcess { bytes: number, isQueued: boolean, start(): boolean } // 待機処理の起動関数

// 文書別取消と同時実行・待機順制御の状態
const running = new Map<string, () => void>();
const MAX_PROCESSES = 2; // 子処理の同時実行上限
const MAX_PENDING = 8; // 待機処理の件数上限
const MAX_PENDING_BYTES = MAX_INPUT_BYTES << 1; // 待機入力の総量上限
const FORMAT_TIMEOUT_MS = 30_000; // 子処理の実行上限３０秒
const TERMINATION_GRACE_MS = 1_000; // 子処理の終了猶予１秒
const MAX_RETAINED_HEAD = 63; // 待機列を詰めずに保持する消費済先頭数
const pendingProcesses: PendingProcess[] = [];
let activeProcesses = 0;
let pendingBytes = 0;
let pendingCount = 0;
let pendingHead = 0;

/**
 * 空いた実行枠へ待機中の処理を移す
 * 計算量：取消済の待機数を Q として O(Q)
 */
function startPending(): void {
	// 待機列の先頭からの走査
	while(pendingHead < pendingProcesses.length) {
		const pending = pendingProcesses[pendingHead];
		// 取消済の場合
		if(!pending.isQueued) {
			++pendingHead;
			continue;
		}
		// 実行枠が無い場合の走査打切
		if(activeProcesses >= MAX_PROCESSES) break;
		// 待機状態と計数の更新
		++pendingHead;
		pending.isQueued = false;
		--pendingCount;
		pendingBytes -= pending.bytes;
		if(pending.start()) ++activeProcesses;
	}
	// 消費済先頭の一括除去に依る配列移動の償却線形化
	if(pendingHead == pendingProcesses.length) {
		pendingProcesses.length = 0;
		pendingHead = 0;
	} else if(
		pendingProcesses.length - pendingHead > (MAX_PENDING << 1) ||
		pendingHead > MAX_RETAINED_HEAD && (pendingHead << 1) >= pendingProcesses.length
	) {
		const queued = pendingProcesses.slice(pendingHead).filter((pending: PendingProcess): boolean => pending.isQueued);
		// 有効な待機処理だけの再配置
		pendingProcesses.length = 0;
		pendingProcesses.push(...queued);
		pendingHead = 0;
	}
	// 終了
	return;
}

// 検査専用環境変数の子処理への非継承
const PASS_THROUGH =
new Set(["home", "lang", "lc_all", "lc_ctype", "systemroot", "temp", "tmp", "tmpdir", "userprofile", "windir"]);

/**
 * 子プロセスの起動・受信・取消・後始末を完結させる
 * 計算量：入力 N バイトと受信出力 M バイトに対し送受信・結合は O(N+M)，子プロセス内の整形処理は含まない
 * @param binPath 実行ファイルの絶対パス
 * @param key 文書を識別する鍵
 * @param input 改行を正規化した入力
 * @param language 言語の識別子
 * @param maxChars 行幅
 * @param token 取消の合図
 * @param channel 起動と受信の問題を記録する出力欄
 * @returns 完了結果又は適用しない終了の理由
 */
export function runFormatter(
	binPath: string,
	key: string,
	input: Buffer,
	language: string,
	maxChars: number,
	token: CancellationToken,
	channel: Pick<OutputChannel, "appendLine">
): Promise<ProcessResult> {
	// 実行枠の待機と子処理終了後の購読・文書状態の解除
	return new Promise(
		(resolve: (value: ProcessResult | PromiseLike<ProcessResult>) => void): void => {
			// 標準出力と標準エラーの受信列・出力上限
			const stdoutChunks: Buffer[] = [], stderrChunks: Buffer[] = [], stdoutMax = Math.max(MAX_INPUT_BYTES, input.length << 3); // 入力の８倍又は入力上限の大きい方
			const STDERR_MAX = 0X400000; // 標準エラーの受信上限（4 MiB）
			let stdoutSize = 0, stderrSize = 0, isAborted = false, isOverflowed = false, isSettled = false, isStarted = false;
			let child: childProcess.ChildProcessWithoutNullStreams | undefined, timeout: NodeJS.Timeout | undefined;
			let terminationTimeout: NodeJS.Timeout | undefined, cancellation: { dispose(): unknown } | undefined;
			let pending: PendingProcess | undefined, queuedInput: Buffer | undefined = input;
			/**
			 * 結果確定時に資源を解放して次処理を起動する
			 * @param result 呼出元へ返す処理結果
			 */
			const finish = (result: ProcessResult): void => {
				// 条件成立時の返戻
				if(isSettled) return;
				// 時間制限・終了猶予と取消購読の解除
				isSettled = true;
				if(timeout) clearTimeout(timeout);
				if(terminationTimeout) clearTimeout(terminationTimeout);
				cancellation?.dispose();
				// 文書別取消登録の解除
				if(running.get(key) === abort) running.delete(key);
				// 起動済の場合
				if(isStarted) {
					isStarted = false;
					--activeProcesses;
				}
				// 呼出元への結果確定と次処理の起動
				resolve(result);
				startPending();
				// 終了
				return;
			};
			/** 応答しない子処理を終了猶予後に強制停止する */
			const stopChild = (): void => {
				// 重複停止要求の除外
				if(!child || terminationTimeout) return;
				// 通常の停止要求
				try {
					child.kill();
				} catch(error) {
					if(error instanceof Error) channel.appendLine(`formatter stop failed: ${error.message}`);
				}
				// 同期終了時の返戻
				if(isSettled) return;
				// 終了猶予後の強制停止と結果確定
				terminationTimeout = setTimeout(
					(): void => {
						try {
							child?.kill("SIGKILL");
						} catch(error) {
							if(error instanceof Error) channel.appendLine(`formatter force stop failed: ${error.message}`);
						}
						if(isAborted) finish({ kind: isOverflowed ? "overflow" : "cancelled" });
						else {
							finish(
								{
									kind: "completed",
									code: null,
									signal: "SIGKILL",
									stdout: Buffer.alloc(0),
									stderr: Buffer.concat(stderrChunks).toString("utf8")
								}
							);
						}
						// 終了
						return;
					},
					TERMINATION_GRACE_MS
				);
				// 終了
				return;
			};
			/** 待機又は実行中の処理を取り消す */
			const abort = (): void => {
				// 条件成立時の返戻
				if(isSettled) return;
				// 取消状態の記録と起動済子処理の停止
				isAborted = true;
				if(child) stopChild();
				else {
					// 待機列内の状態と計数の更新
					if(pending?.isQueued) {
						pending.isQueued = false;
						--pendingCount;
						pendingBytes -= pending.bytes;
					}
					// 保持入力の解放と取消結果の確定
					queuedInput = undefined;
					finish({ kind: "cancelled" });
				}
				// 終了
				return;
			};
			/**
			 * 待機処理を起動する
			 * @returns 実行枠を取得した場合は true
			 */
			const start = (): boolean => {
				// 条件成立時の返戻
				if(isSettled) return false;
				// 起動状態の記録と入力所有権の移動及び解放済時の取消確定
				isStarted = true;
				const processInput = queuedInput;
				queuedInput = undefined;
				if(!processInput) {
					queueMicrotask((): void => finish({ kind: "cancelled" }));
					// 非同期完了処理に依る実行枠の解放
					return true;
				}
				// 子環境構築と整形器起動及び時間制限設定
				const childEnv = Object.fromEntries(
					Object.entries(process.env).filter(([name]: [string, string | undefined]): boolean => PASS_THROUGH.has(name.toLowerCase()))
				);
				try {
					child = childProcess.spawn(binPath, ["--stdin", "--chars", String(maxChars), "--lang", language], { env: childEnv });
				} catch(error) {
					// 起動失敗の説明と記録
					const message = error instanceof Error ? error.message : String(error);
					channel.appendLine(`spawn failed: ${message}`);
					queueMicrotask((): void => finish({ kind: isAborted ? "cancelled" : "spawnError" }));
					// 非同期完了処理に依る実行枠の解放
					return true;
				}
				timeout = setTimeout(
					(): void => {
						// 制限時間超過時の子処理停止
						stopChild();
						// 終了
						return;
					},
					FORMAT_TIMEOUT_MS
				);
				// 子処理の出力・誤り・終了イベント購読
				child.stdout.on(
					"data",
					(chunk: Buffer): void => {
						// 標準出力の受信と上限検査
						stdoutSize += chunk.length;
						if(stdoutSize > stdoutMax) {
							if(!isOverflowed) channel.appendLine(`output exceeded ${stdoutMax} bytes; stopped`);
							isOverflowed = true;
							abort();
							// 終了
							return;
						}
						stdoutChunks.push(chunk);
						// 終了
						return;
					}
				);
				child.stderr.on(
					"data",
					(chunk: Buffer): void => {
						// 標準エラー受信上限到達後の返戻
						if(stderrSize > STDERR_MAX) return;
						// 標準エラー受信量の加算
						stderrSize += chunk.length;
						// 受信上限に応じた保存範囲の選択
						if(stderrSize > STDERR_MAX) {
							// 上限超過時は最後の完全な行終端で切断し，不完全な警告を診断から除外
							const tail = chunk.lastIndexOf(0X0A); // LF のバイト値
							if(tail > -1) stderrChunks.push(chunk.subarray(0, tail + 1));
							channel.appendLine(`warnings exceeded ${STDERR_MAX} bytes; the rest is dropped`);
						} else stderrChunks.push(chunk);
						// 終了
						return;
					}
				);
				child.on(
					"error",
					(error: Error): void => {
						// 起動失敗後の終了イベントを含む結果の一度だけの確定
						channel.appendLine(`spawn failed: ${error.message}`);
						finish({ kind: isAborted ? "cancelled" : "spawnError" });
						// 終了
						return;
					}
				);
				child.on(
					"close",
					(code: number | null, signal: NodeJS.Signals | null): void => {
						// 条件成立時の返戻
						if(isSettled) return;
						// 取消又は正常終了の振分
						if(isAborted) finish({ kind: isOverflowed ? "overflow" : "cancelled" });
						else {
							// 正常終了結果の構築
							finish(
								{
									kind: "completed",
									code,
									signal,
									stdout: !(code ?? 1) ? Buffer.concat(stdoutChunks, stdoutSize) : Buffer.alloc(0), // 終了コード無は失敗扱
									stderr: Buffer.concat(stderrChunks).toString("utf8")
								}
							);
						}
						// 終了
						return;
					}
				);
				// 標準入力の誤り購読と送信
				child.stdin.on("error", (error: Error): void => channel.appendLine(`shavefmt stdin error: ${error.message}`));
				child.stdin.end(processInput);
				// 起動した処理の実行数への加算
				return true;
			};
			// 同じ文書の直前処理の取消
			running.get(key)?.();
			// 待機上限の検査
			if(pendingCount >= MAX_PENDING || pendingBytes + input.length > MAX_PENDING_BYTES) {
				// 上限超過入力の解放と混雑結果の確定
				queuedInput = undefined;
				channel.appendLine("formatter queue limit reached; skipped");
				finish({ kind: "busy" });
				// 終了
				return;
			}
			// 待機情報と取消関数の登録
			pending = { bytes: input.length, isQueued: true, start };
			running.set(key, abort);
			// 待機列と計数の更新
			pendingProcesses.push(pending);
			++pendingCount;
			pendingBytes += pending.bytes;
			// 取消購読と後始末用参照の登録
			const subscription = token.onCancellationRequested(abort);
			cancellation = subscription;
			// 同期確定時の購読解除
			if(isSettled) subscription.dispose();
			// 実行枠が有る場合の起動
			startPending();
			// 終了
			return;
		}
	);
}
