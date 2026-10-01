// 実際の整形器を起動し，拡張機能の受信・取消・要求の差替を検査する
const assert = require("node:assert/strict");
const childProcess = require("node:child_process");
const { EventEmitter } = require("node:events");
const fileSystem = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const { runFormatter } = require("../vscode-extension/out/formatterProcess.js");

/**
 * 取消の発行元を作る
 * @return 購読と解除を扱う合図及び発行関数
 */
function cancellation() {
	const callbacks = new Set;
	// 子の処理へ渡す合図と，試験から取消を発行する関数の返戻
	return {
		token: {
			/**
			 * 取消の受取を登録する
			 * @param callback 取消時に呼び出す関数
			 * @return 購読の解除手段
			 */
			onCancellationRequested(callback) {
				callbacks.add(callback);
				// 取消の購読解除手段の返戻
				return { dispose: () => callbacks.delete(callback) };
			}
		},
		cancel: () => callbacks.forEach(callback => callback())
	};
}

/**
 * 子処理が同時に２件だけ起動し，待機中の取消が枠を消費しない事を検査する
 * @param channel 記録先
 * @return 検査の完了
 */
async function verifyConcurrency(channel) {
	const originalSpawn = childProcess.spawn, children = [];
	let active = 0, maximum = 0;
	childProcess.spawn = () => {
		const child = new EventEmitter;
		child.stdout = new EventEmitter;
		child.stderr = new EventEmitter;
		child.stdin = new EventEmitter;
		child.stdin.end = () => {};
		child.kill = () => {
			child.finish(null, "SIGTERM");
			// 停止要求を受理した結果の返戻
			return true;
		};
		child.finish = (code, signal) => {
			// 閉じ済の子処理の場合の終了
			if(child.closed) return;
			child.closed = true;
			--active;
			child.emit("close", code, signal);
		};
		++active;
		maximum = Math.max(maximum, active);
		children.push(child);
		// 起動済の模擬子処理の返戻
		return child;
	};
	try {
		const source = Buffer.from("int x;\n"), first = runFormatter("mock", "first", source, "c", 128, cancellation().token, channel);
		const second = runFormatter("mock", "second", source, "c", 128, cancellation().token, channel), cancelled = cancellation();
		const waiting = runFormatter("mock", "waiting", source, "c", 128, cancelled.token, channel);
		const fourth = runFormatter("mock", "fourth", source, "c", 128, cancellation().token, channel);
		cancelled.cancel();
		assert.equal((await waiting).kind, "cancelled");
		assert.equal(active, 2);
		assert.equal(maximum, 2);
		assert.equal(children.length, 2);
		children[0].finish(0, null);
		await new Promise(resolve => setImmediate(resolve));
		assert.equal(active, 2);
		assert.equal(maximum, 2);
		assert.equal(children.length, 3);
		children[1].finish(0, null);
		children[2].finish(0, null);
		for(const result of await Promise.all([first, second, fourth])) {
			assert.equal(result.kind, "completed");
			assert.equal(result.code, 0);
		}
		// 先頭を待機させた儘後続を差替え，取消済入力が列へ蓄積しない事を検査する
		const childAt = children.length, blockerA = runFormatter("mock", "blocker-a", source, "c", 128, cancellation().token, channel);
		const blockerB = runFormatter("mock", "blocker-b", source, "c", 128, cancellation().token, channel);
		const fixed = runFormatter("mock", "fixed", source, "c", 128, cancellation().token, channel), replaced = [];
		let latest = runFormatter("mock", "repeated", source, "c", 128, cancellation().token, channel);
		for(let index = 0; index < 32; ++index) {
			replaced.push(latest);
			latest = runFormatter("mock", "repeated", source, "c", 128, cancellation().token, channel);
		}
		for(const result of await Promise.all(replaced)) assert.equal(result.kind, "cancelled");
		assert.equal(active, 2);
		assert.equal(children.length, childAt + 2);
		// 待機上限を満たす６件に続く要求は子処理を起動せずに拒否する
		const queued = [];
		for(let index = 0; index < 6; ++index) {
			queued.push(runFormatter("mock", `queued-${index}`, source, "c", 128, cancellation().token, channel));
		}
		const excess = await runFormatter("mock", "excess", source, "c", 128, cancellation().token, channel);
		assert.equal(excess.kind, "busy");
		assert.equal(active, 2);
		assert.equal(maximum, 2);
		for(let index = childAt; index < children.length; ++index) children[index].finish(0, null);
		for(const result of await Promise.all([blockerA, blockerB, fixed, latest, ...queued])) {
			assert.equal(result.kind, "completed");
			assert.equal(result.code, 0);
		}
	} finally {
		for(let index = 0; index < children.length; ++index) children[index].finish(0, null);
		childProcess.spawn = originalSpawn;
	}
	// 終了
	return;
}

/**
 * 停止要求に応答しない子処理を強制停止して待機処理を再開する事を検査する
 * @param channel 記録先
 * @return 検査の完了
 */
async function verifyForcedTermination(channel) {
	const originalSpawn = childProcess.spawn, originalSetTimeout = global.setTimeout, originalClearTimeout = global.clearTimeout;
	const children = [], timers = [];
	childProcess.spawn = () => {
		const child = new EventEmitter;
		child.stdout = new EventEmitter;
		child.stderr = new EventEmitter;
		child.stdin = new EventEmitter;
		child.stdin.end = () => {};
		child.signals = [];
		child.kill = signal => {
			child.signals.push(signal ?? "SIGTERM");
			// 停止要求を無視した結果の返戻
			return true;
		};
		child.finish = (code, signal) => {
			// 閉じ済の子処理の場合の終了
			if(child.closed) return;
			child.closed = true;
			child.emit("close", code, signal);
		};
		children.push(child);
		// 起動済の模擬子処理の返戻
		return child;
	};
	global.setTimeout = callback => {
		const timer = { callback, isCleared: false };
		timers.push(timer);
		// 制御可能な模擬時間制限の返戻
		return timer;
	};
	global.clearTimeout = timer => {
		// 模擬時間制限の解除記録
		timer.isCleared = true;
	};
	try {
		const source = Buffer.from("int x;\n");
		const timedOut = runFormatter("mock", "timed-out", source, "c", 128, cancellation().token, channel);
		const blocker = runFormatter("mock", "blocker", source, "c", 128, cancellation().token, channel);
		const waiting = runFormatter("mock", "waiting-after-timeout", source, "c", 128, cancellation().token, channel);
		assert.equal(children.length, 2);
		assert.equal(timers.length, 2);
		// 実行上限と終了猶予の満了
		timers[0].callback();
		assert.deepEqual(children[0].signals, ["SIGTERM"]);
		assert.equal(timers.length, 3);
		timers[2].callback();
		assert.deepEqual(children[0].signals, ["SIGTERM", "SIGKILL"]);
		assert.equal(children.length, 3);
		const result = await timedOut;
		assert.equal(result.kind, "completed");
		assert.equal(result.code, null);
		assert.equal(result.signal, "SIGKILL");
		// 解放された実行枠で開始した待機処理の完了
		children[1].finish(0, null);
		children[2].finish(0, null);
		for(const completed of await Promise.all([blocker, waiting])) {
			assert.equal(completed.kind, "completed");
			assert.equal(completed.code, 0);
		}
	} finally {
		for(let index = 0; index < children.length; ++index) children[index].finish(0, null);
		childProcess.spawn = originalSpawn;
		global.setTimeout = originalSetTimeout;
		global.clearTimeout = originalClearTimeout;
	}
	// 終了
	return;
}

/**
 * 子処理中の文書変更で旧診断を消し，変更が無ければ診断を更新する事を検査する
 * @param channel 記録先
 * @return 検査の完了
 */
async function verifyChangedDocument(channel) {
	// 実際の整形提供器へ渡す文書と，旧診断を持つ診断集合の準備
	const uri = { toString: () => "file:///changed.c" }, stored = new Map;
	let text = "int x;\n", provider, complete, changed;
	const doc =
	{ uri, fileName: "changed.c", languageId: "c", eol: 1, version: 1, lineCount: 2, getText: () => text, positionAt: at => at };
	const disposable = { dispose: () => {} };
	const diagnostics =
	{ delete: key => stored.delete(key.toString()), set: (key, value) => stored.set(key.toString(), value), dispose: () => {} };
	// VS Code の登録境界の模擬と提供器の取得
	const vscode = {
		EndOfLine: { LF: 1, CRLF: 2 },
		Position: function(line, character) {
			// 位置の保持
			this.line = line;
			this.character = character;
			// 終了
			return;
		},
		Range: function(start, end) {
			// 範囲の保持
			this.start = start;
			this.end = end;
			// 終了
			return;
		},
		TextEdit: { replace: (range, newText) => ({ range, newText }), setEndOfLine: newEol => ({ newEol }) },
		window: { createOutputChannel: () => ({ ...channel, dispose: () => {} }) },
		languages: {
			createDiagnosticCollection: () => diagnostics,
			registerDocumentFormattingEditProvider: (_languages, registered) => {
				// 提供器の保持
				provider = registered;
				// 登録解除手段の返戻
				return disposable;
			}
		},
		workspace: {
			getConfiguration: () => ({ get: () => 128 }),
			onDidCloseTextDocument: () => disposable,
			onDidChangeTextDocument: listener => {
				// 編集の適用を知らせる購読の保持
				changed = listener;
				// 登録解除手段の返戻
				return disposable;
			}
		}
	};
	const dependencies = {
		vscode,
		fs: { existsSync: () => true, chmodSync: () => {} },
		path,
		"./formatterProcess": {
			MAX_INPUT_BYTES: 0X10000000,
			runFormatter: () => new Promise(
				resolve => {
					// 子処理の完了操作の保持
					complete = resolve;
					// 終了
					return;
				}
			)
		}
	};
	// 生成済拡張機能の独立文脈での読込と公開入口からの登録
	const filename = path.resolve(__dirname, "../vscode-extension/out/extension.js"), exported = {};
	vm.runInNewContext(
		fileSystem.readFileSync(filename, "utf8"),
		{ exports: exported, require: name => dependencies[name], process, Buffer, setTimeout, clearTimeout },
		{ filename }
	);
	const context = { extensionPath: path.dirname(filename), subscriptions: [] };
	exported.activate(context);
	try {
		// 版又は本文の変更及び正常完了の個別照合
		// 版と本文の変更状態を累積してから変更無しを確かめる為に此の順を保つ
		for(const change of ["version", "text", "none"]) {
			stored.set(uri.toString(), [{ message: "old diagnostic" }]);
			const original = text, pending = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			if(change === "version") ++doc.version;
			if(change === "text") text = "int y;\n";
			complete({ kind: "completed", code: 0, stdout: Buffer.from(original), stderr: "" });
			assert.equal((await pending).length, 0);
			if(change === "none") assert.equal(stored.get(uri.toString()).length, 0);
			else assert.equal(stored.has(uri.toString()), false, `stale diagnostics after ${change} change`);
		}
		// CRLF の文書は整形結果の LF の本文と改行様式の切替を返し，CRLF へ戻さない（整形仕様 10.3）
		stored.set(uri.toString(), [{ message: "old diagnostic" }]);
		text = "int  x;\r\n";
		doc.eol = 2;
		const version = doc.version, pending = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
		complete({ kind: "completed", code: 0, stdout: Buffer.from("int x;\n"), stderr: "" });
		const edits = await pending;
		assert.equal(edits.length, 2);
		assert.equal(edits[0].newText, "int x;\n");
		assert.equal(edits[1].newEol, 1);
		// 改行様式だけを切り替えた途中の版では診断を消さず，本文の置換後に更新する
		changed({ document: { ...doc, version: version + 1, getText: () => "int  x;\n" } });
		assert.equal(stored.get(uri.toString()).length, 1);
		changed({ document: { ...doc, version: version + 2, getText: () => "int x;\n" } });
		assert.equal(stored.get(uri.toString()).length, 0);
	} finally {
		// 模擬拡張機能の購読解除
		for(const subscription of context.subscriptions) subscription.dispose();
	}
	// 終了
	return;
}

/**
 * 正常結果と結果を適用しない経路を検査する
 * @return 全検査の完了
 */
async function main() {
	const binary = process.argv[2] ?
	path.resolve(process.argv[2]) :
	path.resolve(__dirname, `../build/shavefmt${process.platform === "win32" ? ".exe" : ""}`);
	const messages = [], channel = { appendLine: message => messages.push(message) }, source = Buffer.from("int  x;\n");
	const completed = await runFormatter(binary, "normal", source, "c", 128, cancellation().token, channel);
	assert.equal(completed.kind, "completed");
	assert.equal(completed.code, 0);
	assert.equal(completed.stdout.toString(), "int x;\n");
	const rejected = await runFormatter(binary, "invalid", source, "not-a-language", 128, cancellation().token, channel);
	assert.equal(rejected.kind, "completed");
	assert.notEqual(rejected.code, 0);
	assert.ok(rejected.stderr.length);
	const missing = await runFormatter(binary + ".missing", "missing", source, "c", 128, cancellation().token, channel);
	assert.equal(missing.kind, "spawnError");
	assert.ok(messages.some(message => message.startsWith("spawn failed:")));
	const cancelled = cancellation(), pending = runFormatter(binary, "cancelled", source, "c", 128, cancelled.token, channel);
	cancelled.cancel();
	assert.equal((await pending).kind, "cancelled");
	// 同じ文書への後の要求だけが結果を返す
	const first = runFormatter(binary, "replaced", source, "c", 128, cancellation().token, channel);
	const second = runFormatter(binary, "replaced", source, "c", 128, cancellation().token, channel);
	const [old, latest] = await Promise.all([first, second]);
	assert.equal(old.kind, "cancelled");
	assert.equal(latest.kind, "completed");
	assert.equal(latest.code, 0);
	assert.equal(latest.stdout.toString(), "int x;\n");
	await verifyConcurrency(channel);
	await verifyForcedTermination(channel);
	await verifyChangedDocument(channel);
	console.log(
		"extension process: normal, failure, cancellation, replacement, concurrency, forced termination and document changes OK"
	);
	// 終了
	return;
}

main().catch(
	error => {
		console.error(error);
		process.exitCode = 1;
		// 終了
		return;
	}
);
