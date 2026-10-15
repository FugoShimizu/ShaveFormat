// 実際のフォーマッターを起動し，拡張機能の受信・取消・要求の差替を検査する
let forDisplay;
let MAX_INPUT_BYTES;
let runFormatter; // 実行する拡張機能の操作
const assert = require("node:assert/strict");
const childProcess = require("node:child_process");
const { EventEmitter } = require("node:events");
const fileSystem = require("node:fs");
const moduleApi = require("node:module");
const path = require("node:path");
const vm = require("node:vm");

/**
 * 生成物を作らず，型を除いたソースを同じモジュール文脈で実行する
 * 計算量：モジュールの本文長 N に対して O(N)
 * @param context 実行文脈
 * @param filename 対象ソース
 * @param dependencies 登録境界の代替操作
 * @return リンク済のモジュール
 */
async function sourceModule(context, filename, dependencies) {
	const source = moduleApi.stripTypeScriptTypes(fileSystem.readFileSync(filename, "utf8"));
	const loaded = new vm.SourceTextModule(source, { context, identifier: filename });
	await loaded.link(
		async name => {
			if(name.startsWith(".") && !dependencies) {
				// 相対参照のソースの返戻
				return sourceModule(context, path.resolve(path.dirname(filename), name + ".ts"), null);
			}
			const dependency = dependencies ? dependencies[name] : require(name), names = Object.keys(dependency);
			// 子プロセスの模擬操作への差替を呼出時に参照する
			return new vm.SyntheticModule(
				names,
				function() {
					for(const key of names) {
						this.setExport(
							key,
							!dependencies && typeof dependency[key] === "function" ? (...args) => dependency[key](...args) : dependency[key]
						);
					}
				},
				{ context }
			);
		}
	);
	// リンクしたモジュールの返戻
	return loaded;
}

/**
 * 生成済の拡張機能，又は型消去だけで実行するソースを読み込む
 * @param filename 生成済のモジュールの位置
 * @param dependencies 登録境界の代替操作
 * @return 公開する操作
 */
async function loadTestModule(filename, dependencies = null) {
	if(!process.argv.includes("--source")) {
		// 生成済の実モジュールの返戻
		if(!dependencies) return require(filename);
		const exported = {};
		vm.runInNewContext(
			fileSystem.readFileSync(filename, "utf8"),
			{
				exports: exported,
				require: name => dependencies[name],
				process: dependencies.process ?? process,
				Buffer,
				setTimeout,
				clearTimeout
			},
			{ filename }
		);
		// 模擬文脈で公開した操作の返戻
		return exported;
	}
	const context = vm.createContext(
		{
			process: dependencies?.process ?? process,
			Buffer,
			setTimeout: (...args) => setTimeout(...args),
			clearTimeout: (...args) => clearTimeout(...args),
			queueMicrotask
		}
	);
	const source = filename.replace(/[/\\]out[/\\]/, "/src/").replace(/\.js$/, ".ts");
	const loaded = await sourceModule(context, source, dependencies);
	await loaded.evaluate();
	// 実行済の公開操作の返戻
	return loaded.namespace;
}

/**
 * 取消の発行元を作る
 * @return 購読と解除を扱う合図及び発行関数
 */
function cancellation() {
	// 取消の購読者の集合
	const callbacks = new Set;
	// 子の処理へ渡す合図と，テストから取消を発行する関数の返戻
	return {
		token: {
			/**
			 * 取消の受取を登録する
			 * @param callback 取消時に呼び出す関数
			 * @return 購読の解除手段
			 */
			onCancellationRequested(callback) {
				// 取消時に呼び出す購読の登録
				callbacks.add(callback);
				// 取消の購読解除手段の返戻
				return { dispose: () => callbacks.delete(callback) };
			}
		},
		// 登録した購読へ取消を通知する発行操作
		cancel: () => callbacks.forEach(callback => callback())
	};
}

/**
 * 起動失敗の購読・コードの保持と，終了後の枠・予約量の返却を検査する
 * @param channel 記録先
 * @return 検査の完了
 * @throws AssertionError 起動失敗又は後続要求の受理が契約と異なる場合
 */
async function verifySpawnFailures(channel) {
	// 標準入出力の欠落・同期例外・待機からの起動の組合せ
	const originalSpawn = childProcess.spawn, src = Buffer.from("int x;\n"), large = Buffer.alloc(MAX_INPUT_BYTES - 512);
	try {
		for(
			const [errorCode, hasStreams, isThrown, isQueued] of [
				["EAGAIN", true, false, false],
				["EMFILE", false, false, false],
				["EMFILE", false, false, true],
				["ENFILE", false, false, false],
				["ENOENT", true, false, false],
				["ENOMEM", true, true, false],
				[undefined, false, false, false]
			]
		) {
			const children = [];
			let isFailing = !isQueued;
			childProcess.spawn = () => {
				// 同期起動例外の発行
				if(isFailing && isThrown) throw Object.assign(new Error("mock spawn failure"), { code: errorCode });
				// 起動した子と，標準入出力の準備
				const child = new EventEmitter;
				if(!isFailing || hasStreams) {
					child.stdin = new EventEmitter;
					child.stdin.end = () => {};
					child.stdout = new EventEmitter;
					child.stderr = new EventEmitter;
				}
				children.push(child);
				// 模擬子プロセスの返戻
				return child;
			};
			// 実行中の正常な要求が有る場合の待機からの起動
			const blocker = isQueued ? runFormatter("mock", src, "spawn-blocker", "c", 128, cancellation().token, channel) : undefined;
			isFailing = true;
			const failed = runFormatter("mock", src, "spawn-failure", "c", 128, cancellation().token, channel);
			void failed.catch(() => {});
			if(blocker) {
				children[0].emit("close", 0, null);
				assert.equal((await blocker).kind, "completed");
			}
			// ストリームの有無に依らない終了購読と，起動失敗の発行
			if(!isThrown) {
				const child = children.at(-1);
				assert.equal(child.listenerCount("error"), 1);
				assert.equal(child.listenerCount("close"), 1);
				if(errorCode) child.emit("error", Object.assign(new Error("mock spawn failure"), { code: errorCode }));
				child.emit("close", -1, null);
			}
			const result = await failed;
			assert.equal(result.kind, "spawnError");
			assert.equal(result.errorCode, errorCode);
			// 最低予約量さえ残れば拒否される大きさで，返却と正常経路を検査する
			isFailing = false;
			const count = children.length, next = runFormatter("mock", large, "spawn-recovered", "c", 128, cancellation().token, channel);
			assert.equal(children.length, count + 1);
			children.at(-1).emit("close", 0, null);
			assert.equal((await next).kind, "completed");
		}
	} finally {
		childProcess.spawn = originalSpawn;
	}
	// 終了
	return;
}

/**
 * 原因から分けた対処が，主診断と同じドキュメント・範囲へ付く事を検査する
 * @param doc 対象ドキュメント
 * @param diagnostic 主診断
 * @param remedy 手で定めた対処
 * @throws AssertionError 対処の本文又は位置が契約と異なる場合
 */
function verifyRemedy(doc, diagnostic, remedy) {
	// 関連情報の本文と，主診断との位置の一致の照合
	const related = diagnostic.relatedInformation;
	assert.equal(related.length, 1);
	assert.equal(related[0].message, remedy);
	assert.strictEqual(related[0].location.uri, doc.uri);
	assert.strictEqual(related[0].location.range, diagnostic.range);
	// 終了
	return;
}

/**
 * 子プロセスの直列起動と，待機中の取消が枠を消費しない事を検査する
 * 計算量：要求数 N に対し O(N)（同じドキュメントの差替には固定長のキーを使う）
 * @param channel 記録先
 * @return 検査の完了
 * @throws AssertionError 起動数・取消・差替又は待機上限が契約と異なる場合
 */
async function verifyConcurrency(channel) {
	// 直列起動を観測する模擬子プロセスの準備
	const originalSpawn = childProcess.spawn, children = [];
	let active = 0, max = 0;
	// 起動回数と終了通知をテスト側で管理する模擬起動操作
	childProcess.spawn = () => {
		// 標準入出力と終了通知の発行元の準備
		const child = new EventEmitter;
		child.stdout = new EventEmitter;
		child.stderr = new EventEmitter;
		child.stdin = new EventEmitter;
		child.stdin.end = () => {};
		// 停止要求を受けて終了通知を発行する模擬操作
		child.kill = () => {
			// 要求に対応する終了の発行
			child.finish(null, "SIGTERM");
			// 停止要求を受理した結果の返戻
			return true;
		};
		// 重複を抑止して閉鎖を通知する終了操作
		child.finish = (code, signal) => {
			// 閉済の子プロセスの場合の終了
			if(child.isClosed) return;
			child.isClosed = true;
			--active;
			child.emit("close", code, signal);
		};
		++active;
		max = Math.max(max, active);
		children.push(child);
		// 起動済の模擬子プロセスの返戻
		return child;
	};
	try {
		const src = Buffer.from("int x;\n"), first = runFormatter("mock", src, "first", "c", 128, cancellation().token, channel);
		const second = runFormatter("mock", src, "second", "c", 128, cancellation().token, channel), cancelled = cancellation();
		const waiting = runFormatter("mock", src, "waiting", "c", 128, cancelled.token, channel);
		const fourth = runFormatter("mock", src, "fourth", "c", 128, cancellation().token, channel);
		cancelled.cancel();
		assert.equal((await waiting).kind, "cancelled");
		assert.equal(active, 1);
		assert.equal(max, 1);
		assert.equal(children.length, 1);
		children[0].finish(0, null);
		await new Promise(resolve => setImmediate(resolve));
		assert.equal(active, 1);
		assert.equal(max, 1);
		assert.equal(children.length, 2);
		children[1].finish(0, null);
		children[2].finish(0, null);
		for(const result of await Promise.all([first, second, fourth])) {
			assert.equal(result.kind, "completed");
			assert.equal(result.code, 0);
		}
		// 先頭を待機させた儘後続を差替え，取消済入力が列へ蓄積しない事を検査する
		const childAt = children.length, blocker = runFormatter("mock", src, "blocker", "c", 128, cancellation().token, channel);
		const fixed = runFormatter("mock", src, "fixed", "c", 128, cancellation().token, channel), replaced = [];
		let latest = runFormatter("mock", src, "repeated", "c", 128, cancellation().token, channel);
		for(let idx = 0; idx < 256; ++idx) {
			replaced.push(latest);
			latest = runFormatter("mock", src, "repeated", "c", 128, cancellation().token, channel);
		}
		for(const result of await Promise.all(replaced)) assert.equal(result.kind, "cancelled");
		assert.equal(active, 1);
		assert.equal(children.length, childAt + 1);
		// Save All 相当の１２件を待機させ，総量内の要求を件数で落とさない
		const queued = [];
		for(let idx = 0; idx < 12; ++idx) {
			queued.push(runFormatter("mock", src, `queued-${idx}`, "c", 128, cancellation().token, channel));
		}
		const reserved = Buffer.alloc(MAX_INPUT_BYTES - 15 * 1024);
		queued.push(runFormatter("mock", reserved, "large", "c", 128, cancellation().token, channel));
		const excess = await runFormatter("mock", Buffer.alloc(1), "excess", "c", 128, cancellation().token, channel);
		assert.equal(excess.kind, "busy");
		assert.equal(active, 1);
		assert.equal(max, 1);
		for(let idx = childAt; idx < children.length; ++idx) children[idx].finish(0, null);
		for(const result of await Promise.all([blocker, fixed, latest, ...queued])) {
			assert.equal(result.kind, "completed");
			assert.equal(result.code, 0);
		}
	} finally {
		for(let idx = 0; idx < children.length; ++idx) children[idx].finish(0, null);
		childProcess.spawn = originalSpawn;
	}
	// 終了
	return;
}

/**
 * 取消・出力超過と，終了確認後だけの実行枠の返却を検査する
 * 計算量：受信出力の総バイト数 B と要求数 N に対し O(B + N)
 * @param channel 記録先
 * @return 検査の完了
 * @throws AssertionError 停止・終了確認・上限又は失敗の受渡が契約と異なる場合
 */
async function verifyForcedTermination(channel) {
	// 停止要求と終了猶予を観測する模擬子プロセスの準備
	const originalSpawn = childProcess.spawn, originalSetTimeout = global.setTimeout, originalClearTimeout = global.clearTimeout;
	const children = [], timers = [];
	// 起動回数と終了通知をテスト側で管理する模擬起動操作
	childProcess.spawn = () => {
		// 標準入出力と終了通知の発行元の準備
		const child = new EventEmitter;
		child.stdout = new EventEmitter;
		child.stderr = new EventEmitter;
		child.stdin = new EventEmitter;
		child.stdin.end = () => {};
		child.signals = [];
		child.isKillSuccessful = true;
		// 終了通知と独立して停止要求を記録する模擬操作
		child.kill = signal => {
			// 受け取った停止要求の記録
			child.signals.push(signal ?? "SIGTERM");
			// 停止要求を送信出来たかの返戻（終了通知は別に発行する）
			return child.isKillSuccessful;
		};
		// 重複を抑止して閉鎖を通知する終了操作
		child.finish = (code, signal) => {
			// 閉済の子プロセスの場合の終了
			if(child.isClosed) return;
			child.isClosed = true;
			child.emit("close", code, signal);
		};
		children.push(child);
		// 起動済の模擬子プロセスの返戻
		return child;
	};
	// 時計を進めずに終了猶予を保持する予約操作
	global.setTimeout = (callback, delay) => {
		// 解除状態を持つ予約の保持
		const timer = { callback, delay, isCleared: false };
		timers.push(timer);
		// 制御可能な終了猶予の返戻
		return timer;
	};
	// 予約の解除を記録する模擬操作
	global.clearTimeout = timer => {
		// 終了猶予の解除記録
		timer.isCleared = true;
	};
	try {
		const src = Buffer.from("int x;\n");
		assert.equal(MAX_INPUT_BYTES, 0X1000000);
		// 作業域の受理は CLI へ委ね，其の失敗理由と終了状態を其の儘伝える（整形仕様 9.1）
		const rejected = runFormatter("mock", src, "memory", "c", 128, cancellation().token, channel);
		const memoryError =
		"Error: formatting could not be completed (estimated working memory exceeds resource limit), left unchanged: <stdin>\n";
		children[0].stdout.emit("data", Buffer.from("unaccepted candidate"));
		children[0].stderr.emit("data", Buffer.from(memoryError));
		children[0].finish(1, null);
		const rejection = await rejected;
		assert.equal(rejection.kind, "completed");
		assert.equal(rejection.code, 1);
		assert.equal(rejection.stdout.length, 0);
		assert.equal(rejection.stderr, memoryError);
		assert.equal(timers.length, 0, "extension added a whole-format timeout");
		// 標準エラー上限は受信片に依らず，完全な行だけを残す（整形仕様 8.4）
		const stderrMax = 0X400000, completeLine = Buffer.from("[warn] <stdin>:1:1: Violation: Replace `NULL` with `nullptr`\n");
		const partialLine = Buffer.from("[warn] <stdin>:1:1: Review: Replace `NULL` with ");
		// 各受信片の並びは到着順を表す為，内側の順を保持
		const stderrCases = [
			{ chunks: [Buffer.alloc(stderrMax, 10), Buffer.from("[notice] extra\n")], expected: stderrMax },
			{
				chunks: [Buffer.alloc(stderrMax - partialLine.length, 10), partialLine, Buffer.from("x")],
				expected: stderrMax - partialLine.length
			},
			{ chunks: [completeLine, Buffer.alloc(stderrMax, 120), Buffer.from("\n")], expected: completeLine.length },
			{ chunks: [completeLine.subarray(0, 7), completeLine.subarray(7)], expected: completeLine.length }
		];
		for(const fixture of stderrCases) {
			const childAt = children.length, logs = [];
			const pending = runFormatter("mock", src, "stderr", "c", 128, cancellation().token, { appendLine: msg => logs.push(msg) });
			for(const chunk of fixture.chunks) children[childAt].stderr.emit("data", chunk);
			children[childAt].finish(0, null);
			const result = await pending;
			assert.equal(result.kind, "completed");
			assert.equal(Buffer.byteLength(result.stderr, "utf8"), fixture.expected);
			assert.equal(result.stderr.endsWith("\n"), true);
			assert.equal(logs.length, fixture.chunks.reduce((total, chunk) => total + chunk.length, 0) > stderrMax ? 1 : 0);
		}
		// 不正な UTF-8 のバイトだけを置換し，診断の改行と正常な U+FFFD は保つ
		{
			const childAt = children.length;
			const pending = runFormatter("mock", src, "invalid-diagnostic", "c", 128, cancellation().token, channel);
			children[childAt].stderr.emit("data", Buffer.from([0XC0, 0X80, 0XED, 0XA0, 0X80, 0XEF, 0XBF, 0XBD, 0X0A]));
			children[childAt].finish(0, null);
			assert.equal((await pending).stderr, "?????�\n");
		}
		// 停止要求の成否に依らず，遅い close 迄枠を返さない
		for(const isKillSuccessful of [false, true]) {
			const childAt = children.length, timerAt = timers.length, cancelled = cancellation();
			const stopped = runFormatter("mock", src, "stopped", "c", 128, cancelled.token, channel);
			children[childAt].isKillSuccessful = isKillSuccessful;
			const waiting = runFormatter("mock", src, "waiting-after-cancellation", "c", 128, cancellation().token, channel);
			assert.equal(children.length, childAt + 1);
			assert.equal(timers.length, timerAt, "running a formatter added a deadline");
			cancelled.cancel();
			assert.deepEqual(children[childAt].signals, ["SIGTERM"]);
			assert.equal(timers.length, timerAt + 1);
			assert.equal(timers[timerAt].delay, 1000);
			timers[timerAt].callback();
			assert.deepEqual(children[childAt].signals, ["SIGTERM", "SIGKILL"]);
			assert.equal((await stopped).kind, "cancelled");
			assert.equal(children.length, childAt + 1, "slot was returned before actual child exit");
			children[childAt].finish(null, "SIGKILL");
			assert.equal(children.length, childAt + 2, "confirmed exit did not return the slot");
			children[childAt + 1].finish(0, null);
			const completed = await waiting;
			assert.equal(completed.kind, "completed");
			assert.equal(completed.code, 0);
		}
		// 出力は入力の８倍へ広げず，16 MiB の境界と其の直後を検査する（整形仕様 9.1）
		const childAt = children.length, timerAt = timers.length;
		const output = runFormatter("mock", Buffer.alloc(0X400000, 97), "output", "json", 128, cancellation().token, channel);
		const waiting = runFormatter("mock", src, "waiting-after-overflow", "c", 128, cancellation().token, channel);
		children[childAt].stdout.emit("data", Buffer.alloc(MAX_INPUT_BYTES));
		assert.equal(timers.length, timerAt);
		assert.deepEqual(children[childAt].signals, []);
		children[childAt].stdout.emit("data", Buffer.from("x"));
		assert.deepEqual(children[childAt].signals, ["SIGTERM"]);
		assert.equal(timers.length, timerAt + 1);
		assert.equal(timers[timerAt].delay, 1000);
		assert.equal(children.length, childAt + 1);
		children[childAt].finish(null, "SIGTERM");
		assert.equal((await output).kind, "overflow");
		assert.equal(timers[timerAt].isCleared, true);
		assert.equal(children.length, childAt + 2);
		children[childAt + 1].finish(0, null);
		assert.equal((await waiting).kind, "completed");
	} finally {
		for(let idx = 0; idx < children.length; ++idx) children[idx].finish(0, null);
		childProcess.spawn = originalSpawn;
		global.setTimeout = originalSetTimeout;
		global.clearTimeout = originalClearTimeout;
	}
	// 終了
	return;
}

/**
 * ドキュメント変更・言語の受渡・告知と失敗の診断を検査する
 * 計算量：テストケースの本文と診断の総バイト数 B に対し O(B)
 * @param languageIds 拡張機能の宣言の言語
 * @param channel 記録先
 * @return 検査の完了
 * @throws AssertionError 編集・言語又は診断が契約と異なる場合
 * @throws Error 生成済の拡張機能を読み込めない場合，又は初期化・購読解除が失敗する場合
 * @throws SyntaxError 読み込んだ拡張機能の構文が対象の実行環境で読めない場合
 */
async function verifyChangedDocument(languageIds, channel) {
	// 実際の整形プロバイダーへ渡すドキュメントと，旧診断を持つ診断集合の準備
	const uri = { toString: () => "file:///changed.c" }, stored = new Map;
	let text = "int x;\n", provider, complete, changed, closed, configured = 128, binaryExists = true;
	const langArgs = [], notices = [], notifications = [], commands = [], settings = [], selectors = [], binaries = [], widths = [];
	let selection, lastInput, relativeName = "folder/日本語\u202E.c";
	const doc = {
		uri,
		fileName: "changed.c",
		languageId: "c",
		eol: 1,
		version: 1,
		lineCount: 2,
		getText: () => text,
		positionAt: at => at,
		lineAt: row => ({ text: text.split("\n")[row] }),
		getWordRangeAtPosition: position => {
			for(const match of text.split("\n")[position.line].matchAll(/[\p{L}_][\p{L}\p{N}_]*/gu)) {
				if(match.index <= position.character && position.character < match.index + match[0].length) {
					// 対象位置を含む語の範囲の返戻
					return new vscode.Range(
						new vscode.Position(position.line, match.index),
						new vscode.Position(position.line, match.index + match[0].length)
					);
				}
			}
			// 語でない位置の返戻
			return undefined;
		}
	};
	const disposable = { dispose: () => {} };
	const diagnostics =
	{ delete: key => stored.delete(key.toString()), set: (key, val) => stored.set(key.toString(), val), dispose: () => {} };
	// VS Code の登録境界の模擬とプロバイダーの取得
	const preferences = new Map;
	const vscode = {
		Location: function(uri, range) {
			this.uri = uri;
			this.range = range;
			// 終了
			return;
		},
		DiagnosticRelatedInformation: function(location, message) {
			this.location = location;
			this.message = message;
			// 終了
			return;
		},
		ConfigurationTarget: { Global: 1 },
		EndOfLine: { LF: 1, CRLF: 2 },
		DiagnosticSeverity: { Error: 0, Warning: 1, Information: 2 },
		// 警告本文と対象範囲を保持する模擬診断の構築操作
		Diagnostic: function(range, message, severity) {
			this.range = range;
			this.message = message;
			this.severity = severity;
			// 終了
			return;
		},
		// ドキュメントの行・桁を保持する模擬位置の構築操作
		Position: function(line, character) {
			// 位置の保持
			this.line = line;
			this.character = character;
			// 終了
			return;
		},
		// 編集の開始・終了位置を保持する模擬範囲の構築操作
		Range: function(start, end) {
			// 範囲の保持
			this.start = start;
			this.end = end;
			// 終了
			return;
		},
		TextEdit: { replace: (range, newText) => ({ range, newText }), setEndOfLine: newEol => ({ newEol }) },
		window: {
			createOutputChannel: () => ({ ...channel, appendLine: msg => notices.push(msg), dispose: () => {} }),
			showWarningMessage: (msg, ...items) => {
				notifications.push({ level: "warning", msg, items });
				return Promise.resolve(selection);
			},
			showInformationMessage: (msg, ...items) => {
				notifications.push({ level: "information", msg, items });
				return Promise.resolve(selection);
			},
			showErrorMessage: (msg, ...items) => {
				notifications.push({ level: "error", msg, items });
				return Promise.resolve(selection);
			},
			showTextDocument: document => {
				assert.equal(document, doc);
				return Promise.resolve();
			}
		},
		commands: {
			executeCommand: (...args) => {
				commands.push(args);
				return Promise.resolve();
			}
		},
		languages: {
			createDiagnosticCollection: () => diagnostics,
			// 登録されたドキュメントの整形プロバイダーを取得する模擬操作
			registerDocumentFormattingEditProvider: (_languages, registered) => {
				// プロバイダーの保持
				provider = registered;
				selectors.push(..._languages);
				// 登録解除手段の返戻
				return disposable;
			}
		},
		workspace: {
			getConfiguration: (name, scope) => {
				settings.push([name, scope]);
				// 設定の読取と，復元出来る通知設定の更新手段の返戻
				return {
					get: (key, fallback) => key === "chars" ? configured : preferences.get(key) ?? fallback,
					update: (key, value, target) => {
						assert.equal(target, 1);
						preferences.set(key, value);
						// 設定更新完了の返戻
						return Promise.resolve();
					}
				};
			},
			asRelativePath: uri => uri.toString() === "untitled:other.c" ? "other.c" : relativeName,
			onDidCloseTextDocument: listener => {
				closed = listener;
				return disposable;
			},
			// 本文の変更を通知する購読を保持する模擬操作
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
		fs: {
			existsSync: binary => {
				binaries.push(binary);
				return binaryExists;
			},
			chmodSync: () => assert.fail("activation changed executable permissions")
		},
		path,
		"./formatterProcess": {
			QUEUED_REQUEST_CAUSE: "too many formatting requests are queued (16 MiB total)",
			QUEUED_REQUEST_REMEDY: "Format again after current requests finish.",
			forDisplay,
			MAX_INPUT_BYTES,
			// 言語の受渡を記録し，完了を手動で発行する模擬整形操作
			runFormatter: (_binary, _input, _key, lang, width, _token, output) => {
				lastInput = _input;
				widths.push(width);
				output.appendLine("process record");
				// 指定された言語の記録
				langArgs.push(lang);
				// 制御可能な子プロセス結果の返戻
				return new Promise(
					resolve => {
						// 子プロセスの完了操作の保持
						complete = resolve;
						// 終了
						return;
					}
				);
			}
		}
	};
	// 生成済拡張機能の独立文脈での読込と公開入口からの登録
	const filename = path.resolve(__dirname, "../vscode-extension/out/extension.js");
	const exported = await loadTestModule(filename, dependencies);
	const context = { extension: { id: "foogoo.shavefmt" }, extensionPath: path.dirname(filename), subscriptions: [] };
	exported.activate(context);
	try {
		// リソース不足の固定文はパスの有無に依らず同じ失敗診断へ対応付ける
		for(const suffix of ["", ", left unchanged: <stdin>"]) {
			const pending = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			complete(
				{
					kind: "completed",
					code: 1,
					stdout: Buffer.alloc(0),
					stderr: `Error: formatting could not be completed (out of memory or execution resources)${suffix}\n`
				}
			);
			assert.equal((await pending).length, 0);
			assert.equal(
				notifications.at(-1).msg,
				"Shave Format: folder/日本語?.c: Formatting was not applied: out of memory or execution resources. Retry after releasing memory or execution resources; report persistent failures with the formatter version."
			);
			assert(stored.has(uri.toString()), "failure diagnostic missing");
			const failure = stored.get(uri.toString())[0];
			assert.equal(failure.message, "Formatting was not applied: out of memory or execution resources.");
			verifyRemedy(
				doc,
				failure,
				"Retry after releasing memory or execution resources; report persistent failures with the formatter version."
			);
			assert.equal(failure.severity, 1);
			assert.equal(failure.source, "shavefmt");
			assert(notifications.at(-1).items.includes("Report Issue"));
			assert.equal(failure.range.start.line, 0);
			assert.equal(failure.range.start.character, 0);
			assert.equal(failure.range.end.line, 0);
			assert.equal(failure.range.end.character, doc.lineAt(0).text.length);
		}
		assert.equal(notifications.length, 1);
		// 登録は通常のファイルと未保存のドキュメントに限る
		assert(selectors.length);
		assert.deepEqual(Array.from(new Set(selectors.map(item => item.language))), Array.from(languageIds));
		assert(selectors.every(item => ["file", "untitled"].includes(item.scheme)));
		for(const language of new Set(selectors.map(item => item.language))) {
			assert.deepEqual(selectors.filter(item => item.language === language).map(item => item.scheme), ["file", "untitled"]);
		}
		// 設定はドキュメントの言語別設定を読み，不正値は１度だけ知らせる
		selection = "Open Settings";
		for(const value of [-1, "40", 40, 0]) {
			configured = value;
			const pending = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			complete({ kind: "completed", code: 0, stdout: Buffer.from(text), stderr: "" });
			assert.equal((await pending).length, 0);
			assert.equal(settings.at(-1)[1], doc);
			assert.equal(widths.at(-1), typeof value === "number" && value > -1 ? value : 128);
		}
		assert.equal(notifications.filter(item => item.msg.includes("shavefmt.chars")).length, 1);
		assert(commands.some(args => args[0] === "workbench.action.openSettings" && args[1] === "shavefmt.chars"));
		configured = 128;
		selection = undefined;
		// 全対応言語の共通入口と，UTF-16 の端・連続する不正な対を確認する
		for(
			const [language, original] of [
				...Array.from(languageIds, language => [language, "const  x = \"\uD800\";\n"]),
				...[
					"\uD800",
					"\uDBFF",
					"\uDC00",
					"\uDFFF",
					"x\uD800",
					"\uDC00x",
					"\uD800\uD800",
					"\uDC00\uDC00",
					"\uDC00\uD800",
					"\uD800x\uDC00"
				].map(value => ["javascript", value])
			]
		) {
			text = original;
			doc.languageId = language;
			const before = widths.length, pending = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			assert.equal(widths.length, before, "unpaired surrogate was sent to the formatter");
			assert.equal((await pending).length, 0);
			assert.equal(doc.getText(), original);
			const diagnostic = stored.get(uri.toString())[0];
			assert.equal(diagnostic.message, "Formatting was skipped: unpaired UTF-16 surrogate.");
			assert.equal(diagnostic.severity, 2);
			verifyRemedy(doc, diagnostic, "Replace unpaired UTF-16 surrogates with the intended Unicode characters and retry.");
			assert(
				notifications.at(-1).msg.endsWith(
					"Formatting was skipped: unpaired UTF-16 surrogate. Replace unpaired UTF-16 surrogates with the intended Unicode characters and retry."
				)
			);
		}
		// 正常な対・境界の BMP 文字・元からの置換文字・ASCII のエスケープは送信出来る
		for(const original of ["\uD800\uDC00", "\uDBFF\uDFFF", "\uD7FF\uE000", "\uFFFD", "\\uD800\\uDC00", ""]) {
			text = original;
			const before = widths.length, pending = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			assert.equal(widths.length, before + 1);
			assert.equal(lastInput.toString("utf8"), original);
			complete({ kind: "completed", code: 0, stdout: Buffer.from(original, "utf8"), stderr: "" });
			assert.equal((await pending).length, 0);
			assert.equal(stored.get(uri.toString()).length, 0);
		}
		text = "int x;\n";
		doc.languageId = "c";
		// バージョン又は本文の変更及び正常完了の個別照合
		// バージョンと本文の変更状態を累積してから変更無を確かめる為に此の順を保つ
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
		selection = "Don't Show Again";
		// CRLF のドキュメントは整形結果の LF の本文と改行様式の切替を返し，CRLF へ戻さない
		stored.set(uri.toString(), [{ message: "old diagnostic" }]);
		text = "int  x;\r\n";
		doc.eol = 2;
		const version = doc.version, pending = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
		complete({ kind: "completed", code: 0, stdout: Buffer.from("int x;\n"), stderr: "" });
		const edits = await pending;
		assert.equal(edits.length, 2);
		assert.equal(edits[0].newText, "int x;\n");
		assert.equal(edits[1].newEol, 1);
		// 改行様式だけを切り替えた途中のバージョンでは診断を消さず，本文の置換後に更新する
		changed({ document: { ...doc, version: version + 1, getText: () => "int  x;\n" } });
		assert.equal(stored.get(uri.toString()).length, 1);
		changed({ document: { ...doc, version: version + 2, getText: () => "int x;\n" } });
		assert.equal(stored.get(uri.toString()).length, 0);
		assert.equal(notifications.filter(item => item.msg.includes("Line endings changed from CRLF to LF")).length, 1);
		await new Promise(resolve => setImmediate(resolve));
		assert.equal(preferences.get("notifyLineEndingChanges"), false);
		assert.equal(notifications.at(-1).items.at(-1), "Don't Show Again");
		selection = "Change End of Line Sequence";
		// 混在する改行と単独の CR は VS Code が逐語内容を保てない為，原文と診断の状態を検査する
		for(const formatted of ["const char *s = R\"(a\r\nb)\";\n", "const char *s = R\"(a\rb)\";\n"]) {
			text = "const char *s=R\"(a\r\nb)\";\r\n";
			doc.eol = 2;
			stored.set(uri.toString(), [{ message: "old diagnostic" }]);
			const original = text, noticeAt = notices.length, watcher = changed;
			const pending = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			complete({ kind: "completed", code: 0, stdout: Buffer.from(formatted), stderr: "" });
			assert.equal((await pending).length, 0, "unrepresentable line endings were applied");
			assert.equal(text, original);
			assert.equal(doc.eol, 2);
			assert.equal(stored.get(uri.toString())[0].severity, 1);
			assert.equal(
				stored.get(uri.toString())[0].message,
				formatted.includes("a\rb") ?
				"Formatting was not applied: VS Code cannot preserve lone CR inside literals." :
				"Formatting was not applied: preserving CR in multiline literals would mix LF and CRLF."
			);
			verifyRemedy(
				doc,
				stored.get(uri.toString())[0],
				"Change the document to LF and retry; this also changes CR characters inside literals."
			);
			assert.equal(changed, watcher, "rejected output installed a diagnostic watcher");
			assert(notices.slice(noticeAt).some(msg => msg.includes("CR characters inside literals")));
		}
		await Promise.resolve();
		const mixed = notifications.filter(item => item.msg.includes("preserving CR in multiline literals"));
		assert.equal(mixed.length, 1);
		assert(mixed[0].msg.includes("CR characters inside literals"));
		assert(!mixed[0].items.includes("Don't Show Again"));
		assert(commands.some(args => args[0] === "workbench.action.editor.changeEOL"));
		selection = undefined;
		// 設定で通知を無効にした後の LF への切替は通知しない
		text = "int  x;\r\n";
		doc.eol = 2;
		const again = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
		complete({ kind: "completed", code: 0, stdout: Buffer.from("int x;\n"), stderr: "" });
		await again;
		changed({ document: { ...doc, version: doc.version + 2, getText: () => "int x;\n" } });
		assert.equal(notifications.filter(item => item.msg.includes("Line endings changed from CRLF to LF")).length, 1);
		// 単一様式の CRLF と改行無の本文は，其の儘表せる様式で編集する
		for(const [eol, formatted, expectedEol] of [[1, "a\r\nb\r\n", 2], [2, "a\r\nb\r\n", 2], [2, "value", 2]]) {
			text = "unformatted\n";
			doc.eol = eol;
			const version = doc.version, pending = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			complete({ kind: "completed", code: 0, stdout: Buffer.from(formatted), stderr: "" });
			const edits = await pending;
			assert.equal(edits[0].newText, formatted);
			assert.equal(edits.length, eol === expectedEol ? 1 : 2);
			if(eol !== expectedEol) assert.equal(edits[1].newEol, expectedEol);
			changed({ document: { ...doc, version: version + (eol === expectedEol ? 1 : 2), getText: () => formatted } });
			assert.equal(stored.get(uri.toString()).length, 0);
		}
		// 拡張子で確定した読みを CLI へ渡し，BOM の告知を問題へ混ぜない
		text = "const x = 1;\n";
		doc.eol = 1;
		const languageAt = langArgs.length;
		for(const ext of ["cjs", "mjs"]) {
			doc.fileName = "/source." + ext;
			doc.languageId = "javascript";
			const pending = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			complete(
				{ kind: "completed", code: 0, signal: null, stdout: Buffer.from(text), stderr: "[notice] <stdin>: Input has a UTF-8 BOM\n" }
			);
			assert.equal((await pending).length, 0);
			assert.equal(stored.get(uri.toString()).length, 0, "normalization notice became a Problem");
		}
		assert.deepEqual(langArgs.slice(languageAt), ["cjs", "mjs"]);
		assert.equal(notices.filter(msg => msg.includes("UTF-8 BOM")).length, 2);
		// 完全な固定本文の行だけを問題として扱い，終端を失った警告は受理しない
		const warning = "[warn] <stdin>:1:1: Violation: Replace `NULL` with `nullptr`";
		for(const stderr of [warning, warning + "\n" + warning]) {
			const pending = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			complete({ kind: "completed", code: 0, signal: null, stdout: Buffer.from(text), stderr });
			assert.equal((await pending).length, 0);
			assert.equal(stored.get(uri.toString()).length, stderr.includes("\n") ? 1 : 0);
		}
		// 通知・診断・失敗理由の表示は安全化し，整形本文を保持
		{
			const pending = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			complete(
				{
					kind: "completed",
					code: 0,
					signal: null,
					stdout: Buffer.from(text),
					stderr: "[notice] 日本語\u202E\n[warn] <stdin>:1:1: Review: 日本語\x1B\u200B\n"
				}
			);
			assert.equal((await pending).length, 0);
			assert.equal(stored.get(uri.toString())[0].message, "Review: 日本語??");
			assert.ok(notices.some(msg => msg.endsWith("[notice] 日本語?")));
			const failed = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			complete({ kind: "completed", code: 1, signal: null, stdout: Buffer.alloc(0), stderr: "日本語\n\x1B\u200B" });
			assert.equal((await failed).length, 0);
			assert.ok(notices.some(msg => msg.endsWith("shavefmt exited with code 1: 日本語")));
			assert.ok(notices.some(msg => msg.endsWith(": ??")));
		}
		// UTF-8 の位置を UTF-16 の語又は１文字へ換算し，見送と助言は情報として示す
		text = "日本語 + 😀 tail\n";
		const report = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
		complete(
			{
				kind: "completed",
				code: 0,
				stdout: Buffer.from(text),
				stderr: "[notice] <stdin>:1:11: input syntax error; correct the input syntax before retrying\n" +
				"[warn] <stdin>:1:1: Review: first\n[warn] <stdin>:1:11: Violation: second\n" + "[warn] <stdin>:1:11: Second finding\n" +
				"[skip] <stdin>:1:13: unsupported construct\n[notice] <stdin>: format smaller independently valid inputs\n" +
				"[notice] <stdin>:1:13: parser recognition failed; verify the source syntax and target language version\n" +
				"[notice] <stdin>:1:18: standalone remedy\n"
			}
		);
		assert.equal((await report).length, 0);
		const records = stored.get(uri.toString());
		assert.equal(records.length, 5);
		assert.deepEqual(
			Array.from(records, item => [item.severity, item.range.start.character, item.range.end.character]),
			[[2, 0, 3], [1, 4, 5], [1, 4, 5], [2, 6, 8], [2, 9, 13]]
		);
		assert.equal(records[1].message, "Violation: second");
		assert.equal(records[2].message, "Second finding");
		assert.strictEqual(records[1].relatedInformation, records[2].relatedInformation);
		assert.equal(records[3].message, "Formatting was skipped: unsupported construct.");
		assert.equal(records[4].message, "Standalone remedy.");
		for(const idx of [1, 3]) {
			const related = records[idx].relatedInformation;
			assert.equal(related.length, idx === 3 ? 2 : 1);
			for(const notice of related) {
				assert.strictEqual(notice.location.uri, uri);
				assert.strictEqual(notice.location.range, records[idx].range);
			}
		}
		assert.equal(records[1].relatedInformation[0].message, "Input syntax error; correct the input syntax before retrying.");
		assert.equal(
			records[3].relatedInformation[1].message,
			"Parser recognition failed; verify the source syntax and target language version."
		);
		assert.equal(records[3].relatedInformation[0].message, "Format smaller independently valid inputs.");
		assert(
			notifications.at(-1).msg.endsWith("Formatting was skipped: unsupported construct. Format smaller independently valid inputs.")
		);
		assert(!notifications.at(-1).msg.includes("parser recognition failed"));
		assert(notices.some(msg => msg.endsWith("[notice] <stdin>: format smaller independently valid inputs")));
		const unlocated = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
		complete(
			{
				kind: "completed",
				code: 0,
				stdout: Buffer.from(text),
				stderr:
				"[skip] <stdin>: Objective-C header\n[notice] <stdin>: exclude Objective-C sources from the formatting selection\n[notice] <stdin>: Input has a UTF-8 BOM\n[warn] <stdin>: invalid warning\n[warn] <stdin>:1:1: Positioned warning\n"
			}
		);
		await unlocated;
		const unlocatedRecords = stored.get(uri.toString());
		assert.equal(unlocatedRecords.length, 2);
		assert.equal(unlocatedRecords[0].range.start.character, 0);
		assert.equal(unlocatedRecords[0].range.end.character, "日本語 + 😀 tail".length);
		assert.equal(unlocatedRecords[0].severity, 2);
		assert.equal(unlocatedRecords[0].message, "Formatting was skipped: Objective-C header.");
		assert.equal(unlocatedRecords[0].relatedInformation.length, 1);
		assert.equal(unlocatedRecords[0].relatedInformation[0].message, "Exclude Objective-C sources from the formatting selection.");
		assert.strictEqual(unlocatedRecords[0].relatedInformation[0].location.uri, uri);
		assert.strictEqual(unlocatedRecords[0].relatedInformation[0].location.range, unlocatedRecords[0].range);
		assert.equal(unlocatedRecords[1].relatedInformation, undefined);
		assert(
			notifications.at(-1).msg.endsWith(
				"Formatting was skipped: Objective-C header. Exclude Objective-C sources from the formatting selection."
			)
		);
		assert(!notifications.at(-1).msg.includes("UTF-8 BOM"));
		// 対処は直後且つ同じ対象だけを通知に添え，別位置・別ファイル・後続の告知を混ぜない
		for(
			const notice of [
				"[notice] <stdin>:1:11: different position\n",
				"[notice] other.c:1:13: other file\n",
				"\n[notice] <stdin>: delayed notice\n",
				"unmatched line\n[notice] <stdin>: delayed notice\n",
				"[notice] <stdin>: first remedy\n[notice] <stdin>: second notice\n"
			]
		) {
			const success = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			complete({ kind: "completed", code: 0, stdout: Buffer.from(text), stderr: "" });
			await success;
			const pending = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			complete(
				{ kind: "completed", code: 0, stdout: Buffer.from(text), stderr: "[skip] <stdin>:1:13: unsupported construct\n" + notice }
			);
			await pending;
			const skipped = stored.get(uri.toString()).find(item => item.message === "Formatting was skipped: unsupported construct.");
			assert.equal(skipped.relatedInformation?.[0]?.message, notice.includes("first remedy") ? "First remedy." : undefined);
			assert(
				notifications.at(-1).msg.endsWith(
					"Formatting was skipped: unsupported construct." + (notice.includes("first remedy") ? " First remedy." : "")
				)
			);
		}
		// 対処も上限の件数へ含め，保存を打ち切った後も照合した総数を数える
		const limitedDiagnostics = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
		complete(
			{
				kind: "completed",
				code: 0,
				stdout: Buffer.from(text),
				stderr: "[skip] <stdin>:1:1: diagnostic limit\n[notice] <stdin>: first remedy\n" +
				"[warn] <stdin>:1:1: Review: same position\n".repeat(999)
			}
		);
		await limitedDiagnostics;
		assert.equal(stored.get(uri.toString()).length, 999);
		assert(notices.some(line => line.endsWith("diagnostics truncated at 1000 of 1001")));
		// 見送はドキュメントと理由の組で管理し，成功後は同じ理由も知らせる
		const skippedAt = notifications.length;
		for(const reason of ["parser could not read the source", "parser could not read the source", "input exceeds size limit"]) {
			const pending = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			complete({ kind: "completed", code: 0, stdout: Buffer.from(text), stderr: `[skip] <stdin>: ${reason}\n` });
			await pending;
			assert.equal(stored.get(uri.toString())[0].message, `Formatting was skipped: ${reason}.`);
		}
		assert.equal(notifications.length, skippedAt + 2);
		const recoveredSkip = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
		complete({ kind: "completed", code: 0, stdout: Buffer.from(text), stderr: "" });
		await recoveredSkip;
		const repeatedSkip = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
		complete(
			{ kind: "completed", code: 0, stdout: Buffer.from(text), stderr: "[skip] <stdin>: parser could not read the source\n" }
		);
		await repeatedSkip;
		assert.equal(notifications.length, skippedAt + 3);
		// 固定の失敗理由だけを通知し，同じ理由の繰返と別の理由を区別する
		selection = "Report Issue";
		for(
			const reason of [
				"break syntax",
				"change the meaning of the code",
				"change a directive comment",
				"change a documentation comment",
				"change blank lines",
				"change code structure",
				"remove a required block body"
			]
		) {
			const before = notifications.length, existing = [{ message: "existing violation", severity: 1 }];
			stored.set(uri.toString(), existing);
			for(let repeat = 0; repeat < 2; ++repeat) {
				const pending = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
				complete(
					{ kind: "completed", code: 1, stdout: Buffer.alloc(0), stderr: `Error: formatting would ${reason}, left unchanged: <stdin>\n` }
				);
				assert.equal((await pending).length, 0);
				assert.equal(stored.get(uri.toString())[0].message, `Formatting was not applied: output would ${reason}.`);
				verifyRemedy(doc, stored.get(uri.toString())[0], "Report the issue with the formatter version.");
			}
			assert.equal(notifications.length, before + 1);
			assert(notifications.at(-1).msg.includes(reason));
			assert(notifications.at(-1).items.includes("Report Issue"));
		}
		await Promise.resolve();
		const reports = commands.filter(args => args[0] === "workbench.action.openIssueReporter");
		assert(reports.length);
		for(const args of reports) {
			assert.equal(args.length, 2);
			assert.equal(args[1].extensionId, "foogoo.shavefmt");
			assert(args[1].issueTitle.startsWith("Formatting was not applied: output would "));
			assert(!/Report|Install|retry|folder|日本語/.test(args[1].issueTitle));
			assert.deepEqual(Object.keys(args[1]).sort(), ["extensionId", "issueTitle"]);
		}
		selection = undefined;
		// 成功後の再発は改めて通知し，未知の本文と不具合の固定理由を区別する
		for(
			const [stderr, cause, remedy, reportable] of [
				[
					"Error: formatter defect, left unchanged: <stdin>: internal detail\n",
					"formatter defect",
					"Report the issue with the formatter version.",
					true
				],
				["Error: unknown failure\n", "formatter process failed", "See Output for details.", false],
				[
					"Error: formatting could not be completed (parser initialization failed)\n",
					"parser initialization failed",
					`Install the ${process.platform}-${process.arch} package of Shave Format again.`,
					true
				],
				[
					"Error: formatting could not be completed (out of memory or execution resources EXTRA)\n",
					"formatter process failed",
					"See Output for details.",
					false
				]
			]
		) {
			const success = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			complete({ kind: "completed", code: 0, stdout: Buffer.from(text), stderr: "" });
			await success;
			assert.equal(stored.get(uri.toString()).length, 0);
			const before = notifications.length, pending = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			complete({ kind: "completed", code: 1, stdout: Buffer.alloc(0), stderr });
			await pending;
			assert.equal(notifications.length, before + 1);
			assert.equal(stored.get(uri.toString())[0].message, `Formatting was not applied: ${cause}.`);
			verifyRemedy(doc, stored.get(uri.toString())[0], remedy);
			assert(notifications.at(-1).msg.endsWith(`Formatting was not applied: ${cause}. ${remedy}`));
			assert.equal(notifications.at(-1).items.includes("Report Issue"), reportable);
			assert.equal(notifications.at(-1).level, stderr.includes("parser initialization failed") ? "error" : "warning");
			assert.equal(stored.get(uri.toString())[0].severity, stderr.includes("parser initialization failed") ? 0 : 1);
			// 診断を残す失敗は，同じ原因でも別のドキュメントを個別に通知する
			if(stderr === "Error: unknown failure\n") {
				const otherFailure = { ...doc, uri: { toString: () => "untitled:failure-other.c" } };
				const pending = provider.provideDocumentFormattingEdits(otherFailure, {}, cancellation().token);
				complete({ kind: "completed", code: 1, stdout: Buffer.alloc(0), stderr });
				assert.equal((await pending).length, 0);
				assert.equal(notifications.length, before + 2);
				assert.equal(stored.get(otherFailure.uri.toString())[0].message, "Formatting was not applied: formatter process failed.");
				verifyRemedy(otherFailure, stored.get(otherFailure.uri.toString())[0], "See Output for details.");
			}
		}
		// 信号の名前や子の詳細を通知・報告の本文へ混ぜない
		selection = "Report Issue";
		const crashed = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
		complete({ kind: "completed", code: null, signal: "SIGSEGV", stdout: Buffer.alloc(0), stderr: "private source detail" });
		await crashed;
		await Promise.resolve();
		assert.equal(
			notifications.at(-1).msg,
			"Shave Format: folder/日本語?.c: Formatting was not applied: formatter crashed. Report the issue with the formatter version."
		);
		assert.equal(commands.at(-1)[1].extensionId, "foogoo.shavefmt");
		assert.equal(commands.at(-1)[1].issueTitle, "Formatting was not applied: formatter crashed.");
		assert.equal(stored.get(uri.toString())[0].message, "Formatting was not applied: formatter crashed.");
		verifyRemedy(doc, stored.get(uri.toString())[0], "Report the issue with the formatter version.");
		assert.deepEqual(Object.keys(commands.at(-1)[1]).sort(), ["extensionId", "issueTitle"]);
		selection = undefined;
		for(
			const [kind, reason, remedy] of [
				["busy", "too many formatting requests are queued (16 MiB total)", "Format again after current requests finish."],
				["overflow", "formatter returned more output than its limit", "Report the issue with the formatter version."],
				["spawnError", "could not start", `Install the ${process.platform}-${process.arch} package of Shave Format again.`]
			]
		) {
			const before = notifications.length, logAt = notices.length, existing = [{ message: "existing violation", severity: 1 }];
			stored.set(uri.toString(), existing);
			for(let repeat = 0; repeat < 2; ++repeat) {
				const pending = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
				complete({ kind });
				assert.equal((await pending).length, 0);
			}
			assert.equal(notifications.length, before + 1);
			assert(notifications.at(-1).msg.includes(reason));
			assert(notifications.at(-1).msg.includes(remedy));
			assert.equal(notifications.at(-1).level, kind === "spawnError" ? "error" : "warning");
			assert.equal(notifications.at(-1).items.includes("Report Issue"), kind === "overflow");
			if(kind === "overflow") {
				assert.equal(stored.get(uri.toString())[0].severity, 1);
				verifyRemedy(doc, stored.get(uri.toString())[0], remedy);
			} else assert.strictEqual(stored.get(uri.toString()), existing);
			if(kind === "busy") {
				assert.equal(notices.slice(logAt).filter(line => line.includes("too many formatting requests are queued")).length, 2);
			}
			// 通知だけの起動失敗もドキュメント毎に知らせ，診断は作らない
			if(kind === "spawnError") {
				const otherFailure = { ...doc, uri: { toString: () => "untitled:failure-other.c" } };
				const otherExisting = stored.get(otherFailure.uri.toString());
				const pending = provider.provideDocumentFormattingEdits(otherFailure, {}, cancellation().token);
				complete({ kind });
				assert.equal((await pending).length, 0);
				assert.equal(notifications.length, before + 2);
				assert.equal(notifications.at(-1).level, "error");
				assert.strictEqual(stored.get(otherFailure.uri.toString()), otherExisting);
			}
		}
		// リソース不足の起動失敗は入れ直しの Error と区別し，既存診断を保持する
		for(const errorCode of ["EAGAIN", "EMFILE", "ENFILE", "ENOMEM"]) {
			const success = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			complete({ kind: "completed", code: 0, stdout: Buffer.from(text), stderr: "" });
			await success;
			const existing = [{ message: "existing violation", severity: 1 }], before = notifications.length;
			stored.set(uri.toString(), existing);
			for(let repeat = 0; repeat < 2; ++repeat) {
				const pending = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
				complete({ kind: "spawnError", errorCode });
				assert.equal((await pending).length, 0);
			}
			assert.equal(notifications.length, before + 1);
			assert.equal(notifications.at(-1).level, "warning");
			assert(
				notifications.at(-1).msg.endsWith(
					"Formatting was not applied: out of memory or execution resources. Retry after releasing memory or execution resources; report persistent failures with the formatter version."
				)
			);
			assert(!notifications.at(-1).msg.includes("Install"));
			assert(notifications.at(-1).items.includes("Report Issue"));
			assert.strictEqual(stored.get(uri.toString()), existing);
		}
		// 導入の不備と未分類の起動失敗は，必要なパッケージを Error で案内する
		for(const errorCode of ["EACCES", "ENOENT", "ENOEXEC", undefined]) {
			const success = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			complete({ kind: "completed", code: 0, stdout: Buffer.from(text), stderr: "" });
			await success;
			const existing = [{ message: "existing violation", severity: 1 }];
			stored.set(uri.toString(), existing);
			const pending = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			complete({ kind: "spawnError", errorCode });
			await pending;
			assert.equal(notifications.at(-1).level, "error");
			assert(
				notifications.at(-1).msg.endsWith(
					`Formatting was not applied: formatter could not start. Install the ${process.platform}-${
						process.arch
					} package of Shave Format again.`
				)
			);
			assert.strictEqual(stored.get(uri.toString()), existing);
		}
		// 拡張機能自身の入力上限も，原因だけの先頭行診断と対処付の通知を返す
		const oversized = {
			...doc,
			uri: { toString: () => "untitled:oversized.c" },
			getText: () => "x".repeat(MAX_INPUT_BYTES + 1),
			lineAt: () => ({ text: "x" })
		};
		const widthAt = widths.length;
		assert.equal((await provider.provideDocumentFormattingEdits(oversized, {}, cancellation().token)).length, 0);
		assert.equal(widths.length, widthAt);
		const sizeDiagnostic = stored.get(oversized.uri.toString())[0];
		assert.equal(sizeDiagnostic.message, "Formatting was skipped: input exceeds size limit.");
		assert.equal(sizeDiagnostic.severity, 2);
		assert.equal(sizeDiagnostic.range.start.character, 0);
		assert.equal(sizeDiagnostic.range.end.character, 1);
		verifyRemedy(oversized, sizeDiagnostic, "Provide independently valid input within 16 MiB.");
		assert(
			notifications.at(-1).msg.endsWith(
				"Formatting was skipped: input exceeds size limit. Provide independently valid input within 16 MiB."
			)
		);
		// 待機上限は診断を残さず，通知はセッション内で増やさない
		const other = { ...doc, uri: { toString: () => "untitled:other.c" } }, busyAt = notifications.length;
		const busy = provider.provideDocumentFormattingEdits(other, {}, cancellation().token);
		complete({ kind: "busy" });
		await busy;
		assert.equal(notifications.length, busyAt);
		assert.equal(stored.has(other.uri.toString()), false);
		assert(notifications.every(item => !/[\x00-\x1F\x7F\u202E]/u.test(item.msg)));
		assert(notifications.some(item => item.msg.startsWith("Shave Format: folder/日本語?.c: Formatting was skipped:")));
		assert(notifications.some(item => item.msg.startsWith("Shave Format: folder/日本語?.c: Line endings changed")));
		assert(notices.some(line => /^\[[^\]]+\] folder\/日本語\?\.c: process record$/.test(line)));
		assert(notices.every(line => !/[\x00-\x1F\x7F\u202E]/u.test(line)));
		// CLI の見積失敗でドキュメントを置換せず，理由を出力パネルへ伝える
		stored.set(uri.toString(), [{ message: "old diagnostic" }]);
		const rejected = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
		complete(
			{
				kind: "completed",
				code: 1,
				signal: null,
				stdout: Buffer.alloc(0),
				stderr: "Error: formatting could not be completed (estimated working memory exceeds resource limit), left unchanged: <stdin>\n"
			}
		);
		assert.equal((await rejected).length, 0);
		assert.equal(
			stored.get(uri.toString())[0].message,
			"Formatting was not applied: estimated working memory exceeds resource limit."
		);
		verifyRemedy(doc, stored.get(uri.toString())[0], "Format smaller independently valid inputs.");
		assert.ok(notices.some(msg => msg.includes("estimated working memory exceeds resource limit")));
		assert(notifications.at(-1).msg.includes("estimated working memory exceeds resource limit"));
		assert(!notifications.at(-1).items.includes("Report Issue"));
		// バイナリが後で無く為った場合も通知し，復旧後は同じプロバイダーで再実行する
		binaryExists = false;
		const missingAt = notifications.length;
		for(let repeat = 0; repeat < 2; ++repeat) {
			assert.equal((await provider.provideDocumentFormattingEdits(doc, {}, cancellation().token)).length, 0);
		}
		assert.equal(notifications.length, missingAt + 1);
		assert(notifications.at(-1).msg.includes(`${process.platform}-${process.arch}`));
		closed(doc);
		await provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
		assert.equal(notifications.length, missingAt + 1);
		binaryExists = true;
		const recovered = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
		complete({ kind: "completed", code: 0, stdout: Buffer.from(text), stderr: "" });
		assert.equal((await recovered).length, 0);
		assert.equal(stored.get(uri.toString()).length, 0);
		// 別のドキュメントでも LF への切替は通知を増やさない
		text = "int  x;\r\n";
		const otherLf = { ...other, eol: 2 }, lf = provider.provideDocumentFormattingEdits(otherLf, {}, cancellation().token);
		complete({ kind: "completed", code: 0, stdout: Buffer.from("int x;\n"), stderr: "" });
		await lf;
		changed({ document: { ...otherLf, version: otherLf.version + 2, getText: () => "int x;\n" } });
		assert.equal(notifications.filter(item => item.msg.includes("Line endings changed from CRLF to LF")).length, 1);
		// 設定を戻すと情報通知が再開し，警告には非表示のボタンを置かない
		preferences.set("notifyLineEndingChanges", true);
		text = "int  x;\r\n";
		doc.eol = 2;
		const resumed = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
		complete({ kind: "completed", code: 0, stdout: Buffer.from("int x;\n"), stderr: "" });
		await resumed;
		changed({ document: { ...doc, version: doc.version + 2, getText: () => "int x;\n" } });
		assert.equal(notifications.filter(item => item.msg.includes("Line endings changed from CRLF to LF")).length, 2);
		assert(notifications.filter(item => item.level !== "information").every(item => !item.items.includes("Don't Show Again")));
		preferences.set("notifyLineEndingChanges", false);
		selection = undefined;
	} finally {
		// 模擬拡張機能の購読解除
		for(const subscription of context.subscriptions) subscription.dispose();
	}
	// Windows ARM64 でも配布済の x64 を選び，不在でも登録を保持する
	binaryExists = false;
	provider = undefined;
	dependencies.process = { platform: "win32", arch: "arm64" };
	const windows = await loadTestModule(filename, dependencies), windowsContext = { ...context, subscriptions: [] };
	windows.activate(windowsContext);
	try {
		assert(provider);
		assert(binaries.at(-1).endsWith("shavefmt-win32-x64.exe"));
		assert(notifications.at(-1).msg.includes("win32-arm64 package of Shave Format"));
		assert.equal(notifications.at(-1).level, "error");
		assert(!notifications.at(-1).msg.includes("win32-x64"));
		const initial = notifications.at(-1).msg, initialCount = notifications.length, previous = stored.get(uri.toString());
		assert(initial.startsWith("Shave Format: Bundled formatter "));
		assert(!initial.includes("Formatting was not applied"));
		await provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
		assert.equal(notifications.at(-1).msg, initial);
		assert.equal(notifications.length, initialCount);
		assert.strictEqual(stored.get(uri.toString()), previous);
		// 再起動後も警告は通知し，設定で情報だけを非表示にする
		binaryExists = true;
		const mutedAt = notifications.length, muted = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
		complete({ kind: "completed", code: 0, stdout: Buffer.from("const char *s = R\"(a\r\nb)\";\n"), stderr: "" });
		await muted;
		assert.equal(notifications.length, mutedAt + 1);
		assert(!notifications.at(-1).items.includes("Don't Show Again"));
		assert.equal(stored.get(uri.toString())[0].severity, 1);
		const switched = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
		complete({ kind: "completed", code: 0, stdout: Buffer.from("int x;\n"), stderr: "" });
		await switched;
		changed({ document: { ...doc, version: doc.version + 2, getText: () => "int x;\n" } });
		assert.equal(notifications.length, mutedAt + 1);
		assert.equal(stored.get(uri.toString()).length, 0);
	} finally {
		for(const subscription of windowsContext.subscriptions) subscription.dispose();
	}
	// 各配布対象の導入失敗には対象名を示し，原因だけを報告する
	for(
		const [platform, arch] of [
			["darwin", "arm64"],
			["darwin", "x64"],
			["linux", "arm64"],
			["linux", "x64"],
			["win32", "arm64"],
			["win32", "x64"]
		]
	) {
		binaryExists = false;
		dependencies.process = { platform, arch };
		const extension = await loadTestModule(filename, dependencies), targetContext = { ...context, subscriptions: [] };
		extension.activate(targetContext);
		try {
			assert.equal(notifications.at(-1).level, "error");
			assert(notifications.at(-1).msg.includes(`Install the ${platform}-${arch} package of Shave Format again.`));
			binaryExists = true;
			relativeName = "extension";
			const startup = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			complete({ kind: "spawnError" });
			await startup;
			assert.equal(notifications.at(-1).level, "error");
			assert(notifications.at(-1).msg.startsWith("Shave Format: extension: Formatting was not applied:"));
			relativeName = "folder/日本語\u202E.c";
			assert(notifications.at(-1).msg.includes(`Install the ${platform}-${arch} package of Shave Format again.`));
			selection = "Report Issue";
			const initialized = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			complete(
				{
					kind: "completed",
					code: 1,
					stdout: Buffer.alloc(0),
					stderr: "Error: formatting could not be completed (parser initialization failed)\n"
				}
			);
			await initialized;
			await Promise.resolve();
			assert.equal(notifications.at(-1).level, "error");
			assert.equal(stored.get(uri.toString())[0].severity, 0);
			assert(notifications.at(-1).msg.includes(`Install the ${platform}-${arch} package of Shave Format again.`));
			assert.equal(commands.at(-1)[1].issueTitle, "Formatting was not applied: parser initialization failed.");
			assert.deepEqual(Object.keys(commands.at(-1)[1]).sort(), ["extensionId", "issueTitle"]);
			// ワークスペース外の絶対パスは通知へ転記せず，安全化したファイル名だけを使う
			relativeName = "/outside/private-folder/日本語\u202E.c";
			const excess = provider.provideDocumentFormattingEdits(doc, {}, cancellation().token);
			complete({ kind: "overflow" });
			await excess;
			await Promise.resolve();
			assert(notifications.at(-1).msg.startsWith("Shave Format: 日本語?.c: Formatting was not applied:"));
			assert(!notifications.at(-1).msg.includes("private-folder"));
			assert.equal(commands.at(-1)[1].issueTitle, "Formatting was not applied: formatter returned more output than its limit.");
			assert.deepEqual(Object.keys(commands.at(-1)[1]).sort(), ["extensionId", "issueTitle"]);
		} finally {
			for(const subscription of targetContext.subscriptions) subscription.dispose();
			relativeName = "folder/日本語\u202E.c";
			selection = undefined;
		}
	}
	// 終了
	return;
}

/**
 * 正常結果と結果を適用しない経路を検査する
 * 計算量：要求数 N，テストケースと受信の総バイト数 B，子プロセスの総費用 F に対し O(N + B + F)
 * @return 全検査の完了
 * @throws AssertionError 検査結果が契約と異なる場合
 * @throws Error 呼出先で生成済の拡張機能の読込・初期化又は購読解除が失敗する場合
 * @throws SyntaxError 呼出先で読み込んだ拡張機能が有効な構文でない場合
 */
async function main() {
	(
		{ forDisplay, MAX_INPUT_BYTES, runFormatter } =
		await loadTestModule(path.resolve(__dirname, "../vscode-extension/out/formatterProcess.js"))
	);
	await verifySpawnFailures({ appendLine: () => {} });
	// 言語別設定を許す公開設定の照合
	const manifest = JSON.parse(fileSystem.readFileSync(path.resolve(__dirname, "../vscode-extension/package.json"), "utf8"));
	assert.equal(manifest.contributes.configuration.properties["shavefmt.chars"].scope, "language-overridable");
	assert.equal(manifest.contributes.configuration.properties["shavefmt.notifyLineEndingChanges"].default, true);
	// 検査対象のフォーマッターと要求の準備
	const binary = process.argv[2] ?
	path.resolve(process.argv[2]) :
	path.resolve(__dirname, `../build/shavefmt${process.platform === "win32" ? ".exe" : ""}`);
	const msgs = [], channel = { appendLine: msg => msgs.push(msg) }, src = Buffer.from("int  x;\n");
	const completed = await runFormatter(binary, src, "normal", "c", 128, cancellation().token, channel);
	assert.equal(completed.kind, "completed");
	assert.equal(completed.code, 0);
	assert.equal(completed.stdout.toString(), "int x;\n");
	const rejected = await runFormatter(binary, src, "invalid", "not-a-language", 128, cancellation().token, channel);
	assert.equal(rejected.kind, "completed");
	assert.notEqual(rejected.code, 0);
	assert.ok(rejected.stderr.length);
	const missing = await runFormatter(binary + ".missing", src, "missing", "c", 128, cancellation().token, channel);
	assert.equal(missing.kind, "spawnError");
	assert.equal(missing.errorCode, "ENOENT");
	assert.ok(msgs.some(msg => msg.startsWith("spawn failed:")));
	// 表示だけを変換し，起動失敗の区分と通常の日本語・補助面の文字を保つ（整形仕様 10.4）
	const hidden = "\0\t\n\r\x1B\x7F\u0080\u00AD\u034F\u061C\u115F\u17B4\u180B\u200B\u2028\u2060\u3164" +
	"\uFE00\uFEFF\uFFA0\uFFF0\u{1BCA0}\u{1D173}\u{E0000}\uD800";
	assert.equal(forDisplay(hidden), "?".repeat(25));
	assert.equal(forDisplay("日本語😀�"), "日本語😀�");
	const logs = [], unsafePath = binary + ".改行\n含む\u202E.missing";
	const unsafe =
	await runFormatter(unsafePath, src, "unsafe-path", "c", 128, cancellation().token, { appendLine: msg => logs.push(msg) });
	assert.equal(unsafe.kind, "spawnError");
	assert.ok(logs.some(msg => msg.includes(".改行?含む?.missing")));
	assert.ok(logs.every(msg => !/[\x00-\x1F\x7F\u202E]/u.test(msg)));
	const cancelled = cancellation(), pending = runFormatter(binary, src, "cancelled", "c", 128, cancelled.token, channel);
	cancelled.cancel();
	assert.equal((await pending).kind, "cancelled");
	// 同じドキュメントへの後の要求だけが結果を返す
	const first = runFormatter(binary, src, "replaced", "c", 128, cancellation().token, channel);
	const second = runFormatter(binary, src, "replaced", "c", 128, cancellation().token, channel);
	const [old, latest] = await Promise.all([first, second]);
	assert.equal(old.kind, "cancelled");
	assert.equal(latest.kind, "completed");
	assert.equal(latest.code, 0);
	assert.equal(latest.stdout.toString(), "int x;\n");
	await verifyConcurrency(channel);
	assert(!msgs.some(msg => msg.includes("too many formatting requests are queued")));
	await verifyForcedTermination(channel);
	await verifyChangedDocument(manifest.activationEvents.map(event => event.slice("onLanguage:".length)), channel);
	console.log(
		"extension process: normal, failure, cancellation, replacement, serial scheduling, close, limits and diagnostics OK"
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
