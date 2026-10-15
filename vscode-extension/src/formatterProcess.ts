import { HIDDEN_DISPLAY_RANGES } from "./hiddenDisplay";
import type { CancellationToken, OutputChannel } from "vscode";
import * as childProcess from "child_process";

// 実行結果と，結果を適用しない終了の区別
export type ProcessResult =
{ kind: "completed", code: number|null, signal: NodeJS.Signals|null, stdout: Buffer, stderr: string }|
{ kind: "busy"|"cancelled"|"overflow" }|{ kind: "spawnError", errorCode: string|undefined }; // 起動失敗のコード（取得出来なければ未指定）

export const MAX_INPUT_BYTES: number = 0X1000000; // フォーマッターの入力上限 (16 MiB)

// 待機処理
interface PendingProcess { isQueued: boolean, tryStart(): boolean } // 待機処理の起動関数

// ドキュメント別取消と待機順の状態（独立したコマンドラインは直列に呼び，作業域の受理をコマンドラインへ集約する）
const running: Map<string, () => void> = new Map<string, () => void>();
const MAX_PENDING_BYTES: number = MAX_INPUT_BYTES; // 実行・待機で保持する入力と待機情報の見積総量上限

export const QUEUED_REQUEST_CAUSE: string =
`too many formatting requests are queued (${MAX_PENDING_BYTES / 0X100000} MiB total)`; // 待機上限の原因

export const QUEUED_REQUEST_REMEDY: string = "Format again after current requests finish."; // 待機上限の対処

const MIN_REQUEST_BYTES: number = 1024; // 空の入力でも待機情報の費用を計上する最低量
const TERMINATION_GRACE_MS: number = 1_000; // 子プロセスの終了猶予１秒
const MAX_RETAINED_HEAD: number = 63; // 待機列を詰めずに保持する消費済先頭数
const pendingProcesses: PendingProcess[] = [];
let activeProcesses: number = 0;
let pendingBytes: number = 0;
let discardedPending: number = 0;
let pendingHead: number = 0;
// 不正な UTF-16 と ASCII の制御文字は受信側の条件として共通の範囲へ加える
const HIDDEN_DISPLAY: RegExp = new RegExp("[\\u0000-\\u001F\\u007F\\uD800-\\uDFFF" + HIDDEN_DISPLAY_RANGES + "]", "gu");

/**
 * パス・診断の表示に制御文字や不可視文字を混ぜない
 * CLI の `FileIO::ForDisplay` と同じ範囲を `?` へ置き換える
 * 計算量：テキスト長 L に対し O(L)
 * @param text 表示用のテキスト
 * @return 通常の文字を保持した安全な表示
 */
export function forDisplay(text: string): string {
	// 制御文字・不可視文字・不正な UTF-16 の置換結果の返戻
	return text.replace(HIDDEN_DISPLAY, "?");
}

/**
 * 診断バイト列の不正な UTF-8 を１バイト毎に `?` へ置き換える
 * 行境界は診断の読取迄保ち，表示時の制御文字の置換は `forDisplay` に委ねる
 * 計算量：バイト数 B に対し O(B)
 * @param bytes 受信済の診断バイト列
 * @return 不正なバイトを安全な文字へ置き換えた診断
 */
function decodeDiagnostics(bytes: Buffer): string {
	const pieces: string[] = [];
	let start: number = 0;
	for(let pos: number = 0; pos < bytes.length;) {
		const lead: number = bytes[pos];
		if(lead < 0X80) {
			++pos;
			continue;
		}
		const len: number = lead > 0XC1 && lead < 0XF5 ? lead < 0XE0 ? 2 : lead < 0XF0 ? 3 : 4 : 0;
		let isValid: boolean = !!len && pos + len <= bytes.length;
		for(let idx: number = 1; isValid && idx < len; ++idx) {
			const byte: number = bytes[pos + idx], lo: number = idx > 1 ? 0X7F : lead == 0XE0 ? 0X9F : lead == 0XF0 ? 0X8F : 0X7F;
			const hi: number = idx > 1 ? 0XC0 : lead == 0XED ? 0XA0 : lead == 0XF4 ? 0X90 : 0XC0;
			isValid = byte > lo && byte < hi;
		}
		if(isValid) pos += len;
		else {
			pieces.push(bytes.subarray(start, pos).toString("utf8"), "?");
			start = ++pos;
		}
	}
	pieces.push(bytes.subarray(start).toString("utf8"));
	// 妥当な文字と行境界を保持した診断の返戻
	return pieces.join("");
}

/**
 * 空いた実行枠へ待機中の処理を移す
 * 計算量：走査・再配置する待機数 Q と起動処理の総費用 S に対し O(Q + S)
 */
function startPending(): void {
	// 待機列の先頭からの走査
	while(pendingHead < pendingProcesses.length) {
		const pending: PendingProcess = pendingProcesses[pendingHead];
		// 取消済の場合
		if(!pending.isQueued) {
			++pendingHead;
			--discardedPending;
			continue;
		}
		// 実行枠が無い場合の走査打切
		if(activeProcesses) break;
		// 待機状態と計数の更新
		++pendingHead;
		pending.isQueued = false;
		if(pending.tryStart()) ++activeProcesses;
	}
	// 消費済先頭の一括除去に依る配列移動の償却線形化
	if(pendingHead == pendingProcesses.length) {
		pendingProcesses.length = 0;
		pendingHead = 0;
		discardedPending = 0;
	} else if(
		pendingHead + discardedPending > MAX_RETAINED_HEAD && pendingHead + discardedPending << 1 > pendingProcesses.length - 1
	) {
		const queued: PendingProcess[] =
		pendingProcesses.slice(pendingHead).filter((pending: PendingProcess): boolean => pending.isQueued);
		// 有効な待機処理だけの再配置
		pendingProcesses.length = 0;
		pendingProcesses.push(...queued);
		pendingHead = 0;
		discardedPending = 0;
	}
	// 終了
	return;
}

// 検査専用環境変数の子プロセスへの非継承
const PASS_THROUGH: Set<string> =
new Set(["HOME", "LANG", "LC_ALL", "LC_CTYPE", "SYSTEMROOT", "TEMP", "TMP", "TMPDIR", "USERPROFILE", "WINDIR"]);

/**
 * 子プロセスの起動・受信・取消・後始末を完結させる
 * 計算量：入力長 N，キー長 K，受信出力長 M と要求数 Q に対し合計 O(N + K + M + Q)，子プロセス内の整形処理は含まない
 * @param binaryPath 実行ファイルの絶対パス
 * @param input 改行様式を保持した UTF-8 の入力
 * @param key ドキュメントを識別するキー
 * @param lang 言語の識別子
 * @param maxChars 行幅
 * @param token 取消の合図
 * @param channel 起動と受信の問題を記録する出力パネル
 * @return 完了結果又は適用しない終了の理由
 */
export function runFormatter(
	binaryPath: string,
	input: Buffer,
	key: string,
	lang: string,
	maxChars: number,
	token: CancellationToken,
	channel: Pick<OutputChannel, "appendLine">
): Promise<ProcessResult> {
	// 実行枠の待機と子プロセス終了後の購読・ドキュメント状態の解除を行う処理結果の約束の返戻
	return new Promise(
		(resolve: (val: ProcessResult|PromiseLike<ProcessResult>) => void): void => {
			// 標準出力と標準エラーの受信列・出力上限
			const stdoutChunks: Buffer[] = [], stderrChunks: Buffer[] = [], STDOUT_MAX: number = MAX_INPUT_BYTES; // コマンドラインの出力上限
			const STDERR_MAX: number = 0X400000; // 標準エラーの受信上限 (4 MiB)
			let stdoutSize: number = 0, stderrSize: number = 0, stderrEnd: number = 0, isStderrCut: boolean = false;
			let isAborted: boolean = false, isOverflowed: boolean = false, isSettled: boolean = false, isStarted: boolean = false;
			let hasExited: boolean = false, hasStandardStreams: boolean = false, child: childProcess.ChildProcess|undefined;
			let terminationTimeout: NodeJS.Timeout|undefined, cancellation: { dispose(): unknown }|undefined;
			let pending: PendingProcess|undefined, queuedInput: Buffer|undefined = input, reservedBytes: number = 0;
			// 結果を呼出元へ返し，子プロセスの終了確認後に実行枠を解放する
			const finish: (result: ProcessResult) => void = (result: ProcessResult): void => {
				// 実際に終了した子プロセスの実行枠の返却
				const hasReleasedSlot: boolean = isStarted && (hasExited || !child);
				if(hasReleasedSlot) {
					isStarted = false;
					--activeProcesses;
				}
				// 起動して居ない取消又は実際の終了で予約した入力枠を返す
				if((hasReleasedSlot || !isStarted) && reservedBytes) {
					pendingBytes -= reservedBytes;
					reservedBytes = 0;
				}
				if(isSettled) {
					if(hasReleasedSlot) startPending();
					// 確定済の場合の終了
					return;
				}
				// 終了猶予と取消購読の解除
				isSettled = true;
				if(terminationTimeout) clearTimeout(terminationTimeout);
				cancellation?.dispose();
				// ドキュメント別取消登録の解除
				if(running.get(key) === abort) running.delete(key);
				// 呼出元への結果確定と次処理の起動
				resolve(result);
				startPending();
				// 終了
				return;
			};
			// 応答しない子プロセスを終了猶予後に強制停止する
			const stopChild: () => void = (): void => {
				// 子プロセスが無いか停止を要求済の場合の終了
				if(!child || terminationTimeout) return;
				// 通常の停止要求
				try {
					child.kill();
				} catch(e: unknown) {
					channel.appendLine(forDisplay(`formatter stop failed: ${e instanceof Error ? e.message : String(e)}`));
				}
				// 同期終了時の返戻
				if(isSettled) return;
				// 終了猶予後の強制停止と結果確定
				terminationTimeout = setTimeout(
					(): void => {
						// 失敗の通知を伴う処理の実行
						try {
							child?.kill("SIGKILL");
						} catch(e: unknown) {
							channel.appendLine(forDisplay(`formatter force stop failed: ${e instanceof Error ? e.message : String(e)}`));
						}
						finish({ kind: isOverflowed ? "overflow" : "cancelled" });
						// 終了
						return;
					},
					TERMINATION_GRACE_MS
				);
				// 終了
				return;
			};
			// 待機又は実行中の処理を取り消す
			const abort: () => void = (): void => {
				// 確定済の場合の終了
				if(isSettled) return;
				// 取消状態の記録と起動済子プロセスの停止
				isAborted = true;
				if(child) stopChild();
				else {
					// 待機列内の状態と計数の更新
					if(pending?.isQueued) {
						pending.isQueued = false;
						++discardedPending;
					}
					// 保持入力の解放と取消結果の確定
					queuedInput = undefined;
					finish({ kind: "cancelled" });
				}
				// 終了
				return;
			};
			// 待機処理を起動し，実行枠を取得したかを返す
			const tryStart: () => boolean = (): boolean => {
				// 確定済で実行枠を取得しない事の返戻
				if(isSettled) return false;
				// 起動状態の記録と入力所有権の移動及び解放済時の取消確定
				isStarted = true;
				const processInput: Buffer|undefined = queuedInput;
				queuedInput = undefined;
				if(!processInput) {
					queueMicrotask((): void => finish({ kind: "cancelled" }));
					// 実行枠を取得し，非同期の完了処理で解放する事の返戻
					return true;
				}
				// 子環境構築とフォーマッター起動（解析期限はコマンドラインの各解析へ委ねる）
				const childEnvironment: NodeJS.ProcessEnv = Object.fromEntries(
					Object.entries(process.env).filter(
						// Windows の環境変数名は大文字・小文字を区別しない
						([name]: [string, string|undefined]): boolean => PASS_THROUGH.has(process.platform === "win32" ? name.toUpperCase() : name)
					)
				);
				try {
					child = childProcess.spawn(binaryPath, ["--stdin", "--chars", String(maxChars), "--lang", lang], { env: childEnvironment });
				} catch(e: unknown) {
					// 起動失敗の説明と記録
					const msg: string = e instanceof Error ? e.message : String(e);
					channel.appendLine(forDisplay(`spawn failed: ${msg}`));
					const errorCode: string|undefined =
					typeof e === "object" && e !== null && "code" in e && typeof e.code === "string" ? e.code : undefined;
					queueMicrotask((): void => finish(isAborted ? { kind: "cancelled" } : { kind: "spawnError", errorCode }));
					// 実行枠を取得し，非同期の完了処理で解放する事の返戻
					return true;
				}
				// 起動直後のエラーと終了を，標準入出力へアクセスする前に購読する
				child.on(
					"error",
					(error: NodeJS.ErrnoException): void => {
						// 起動失敗後の終了イベントを含む結果の１度だけの確定
						channel.appendLine(forDisplay(`spawn failed: ${error.message}`));
						finish(isAborted ? { kind: "cancelled" } : { kind: "spawnError", errorCode: error.code });
						// 終了
						return;
					}
				);
				child.on(
					"close",
					(code: number|null, signal: NodeJS.Signals|null): void => {
						// 対象の状態の更新
						hasExited = true;
						if(isSettled) {
							finish({ kind: "cancelled" });
							// 確定済の結果を保っての終了
							return;
						}
						// 取消又は正常終了の振分
						if(isAborted) finish({ kind: isOverflowed ? "overflow" : "cancelled" });
						else if(!hasStandardStreams) finish({ kind: "spawnError", errorCode: undefined });
						else {
							// 正常終了結果の構築
							finish(
								{
									kind: "completed",
									code,
									signal,
									stdout: !(code ?? 1) ? Buffer.concat(stdoutChunks, stdoutSize) : Buffer.alloc(0), // 終了コード無は失敗扱
									stderr: decodeDiagnostics(Buffer.concat(stderrChunks, stderrSize).subarray(0, isStderrCut ? stderrEnd : stderrSize))
								}
							);
						}
						// 終了
						return;
					}
				);
				// 起動に失敗してパイプが無い子は，終了イベントで枠と予約量を返す
				if(!child.stdin || !child.stdout || !child.stderr) return true;
				hasStandardStreams = true;
				// 子プロセスの出力イベント購読
				child.stdout.on(
					"data",
					(chunk: Buffer): void => {
						// 標準出力の受信と上限検査
						stdoutSize += chunk.length;
						if(stdoutSize > STDOUT_MAX) {
							if(!isOverflowed) channel.appendLine(`output exceeded ${STDOUT_MAX} bytes; stopped`);
							isOverflowed = true;
							abort();
							// 出力上限を超過した場合の終了
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
						// 標準エラー受信上限到達後の終了
						if(isStderrCut) return;
						// 上限以内の受信範囲と，受信片を跨ぐ最後の完全な行の終端
						const kept: Buffer = chunk.subarray(0, STDERR_MAX - stderrSize), tail: number = kept.lastIndexOf(0X0A); // LF のバイト値
						if(tail > -1) stderrEnd = stderrSize + tail + 1;
						stderrSize += kept.length;
						if(kept.length) stderrChunks.push(kept);
						if(kept.length < chunk.length) {
							// 未終端の末尾行は保存済の受信片も含めて除き，以後の受信を保存しない
							isStderrCut = true;
							channel.appendLine(`warnings exceeded ${STDERR_MAX} bytes; the rest is dropped`);
						}
						// 終了
						return;
					}
				);
				// 標準入力のエラー購読と送信
				child.stdin.on("error", (error: Error): void => channel.appendLine(forDisplay(`shavefmt stdin error: ${error.message}`)));
				child.stdin.end(processInput);
				// 実行枠を取得して処理を起動した事の返戻
				return true;
			};
			// 同じドキュメントの直前処理の取消
			running.get(key)?.();
			// 待機上限の検査
			const charge: number = Math.max(MIN_REQUEST_BYTES, input.length);
			if(pendingBytes + charge > MAX_PENDING_BYTES) {
				// 上限超過入力の解放と混雑結果の確定
				queuedInput = undefined;
				finish({ kind: "busy" });
				// 待機上限を超過した場合の終了
				return;
			}
			// 待機情報と取消関数の登録
			reservedBytes = charge;
			pending = { isQueued: true, tryStart };
			running.set(key, abort);
			// 待機列と計数の更新
			pendingProcesses.push(pending);
			pendingBytes += charge;
			// 取消購読と後始末用参照の登録
			const subscription: { dispose(): unknown } = token.onCancellationRequested(abort);
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
