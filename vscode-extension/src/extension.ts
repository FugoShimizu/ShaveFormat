import * as vscode from "vscode";
import * as fileSystem from "fs";
import * as path from "path";
import { MAX_INPUT_BYTES, runFormatter } from "./formatterProcess";

// 整形処理の既定値と制御文字
const DEFAULT_MAX_CHARS = 128; // 既定の最大行幅
const DIAGNOSTIC_MAX = 1000; // 診断件数上限
const DIAGNOSTIC_WAIT_MS = 5_000; // 編集適用待機の上限５秒
const EXECUTABLE_MODE = 0O755; // 所有者の書込を含む実行可能権限
const CR = 0X0D; // 復帰文字のバイト値
const LF = 0X0A; // 改行文字のバイト値

// `--lang` へ直接渡す shavefmt 対応 VS Code 言語識別子
const SUPPORTED_LANGUAGE_IDS: string[] = [
	"c",
	"cpp",
	"csharp",
	"java",
	"go",
	"rust",
	"kotlin",
	"swift",
	"php",
	"javascript",
	"javascriptreact",
	"typescript",
	"typescriptreact",
	"ruby",
	"python",
	"json",
	"jsonc",
	"html",
	"css",
	"scss"
];

// 通知済文書の URI 集合
const notifiedSkips = new Set<string>();
// 整形編集の適用を待つ文書毎の診断更新
const pendingDiagnostics = new Map<string, { watcher: vscode.Disposable, timer: ReturnType<typeof setTimeout> }>();

// 規約警告の位置と文面
type LintWarning = { row: number, col: number, message: string };

/**
 * 診断反映待機の解除関数
 * @param key 文書を一意に表す URI
 */
function clearPendingDiagnostics(key: string): void {
	const pending = pendingDiagnostics.get(key);
	// 待機が無い場合の終了
	if(!pending) return;
	// 文書変更購読と時間切れ処理の解除及び待機情報の除去
	pending.watcher.dispose();
	clearTimeout(pending.timer);
	pendingDiagnostics.delete(key);
	// 終了
	return;
}

/**
 * 出力欄への導線を添えた通知関数（出力欄は既定で不可視の為，詳細は出力欄へ書いて通知から開ける様にする）
 * 通知には固定文だけを示し，詳細は出力欄へ記録する
 * @param show 通知の表示関数
 * @param message 通知の本文
 * @param channel 出力欄
 */
function notifyWithOutput(
	show: (message: string, ...items: string[]) => Thenable<string | undefined>,
	message: string,
	channel: vscode.OutputChannel
): void {
	// 出力欄を開く選択肢
	const open = "Show Output";
	void Promise.resolve(show(message, open)).then(
		(choice: string | undefined): void => {
			// 選択結果の反映
			if(choice === open) channel.show(true);
			// 終了
			return;
		}
	).catch(
		(error: unknown): void => channel.appendLine(`notification failed: ${error instanceof Error ? error.message : String(error)}`)
	);
	// 終了
	return;
}

/**
 * 見送の通知関数（保存の度に出さない様に同じ文書では１度だけ知らせる）
 * @param doc 見送った文書
 * @param reason 見送の理由
 * @param channel 出力欄
 */
function notifySkip(doc: vscode.TextDocument, reason: string, channel: vscode.OutputChannel): void {
	// 見送理由と文書識別子の取得
	channel.appendLine(`[skip] ${reason}`);
	const key = doc.uri.toString();
	// 文書毎の初回通知判定
	if(!notifiedSkips.has(key)) {
		notifiedSkips.add(key);
		const message = /^[A-Za-z0-9 .-]+$/.test(reason) ?
		`Shave Format: Formatting was skipped (${reason}).` :
		"Shave Format: Formatting was skipped.";
		notifyWithOutput(vscode.window.showInformationMessage, message, channel);
	}
	// 終了
	return;
}

/**
 * Buffer 内の CR (0X0D) 除去に依る CRLF→LF 正規化関数
 * 計算量：入力バイト数 N に対し O(N)
 * @param buf 入力 Buffer (UTF-8)
 * @returns CR 除去後の Buffer（CR 不在なら入力を其のまま返戻）
 */
function stripCR(buf: Buffer): Buffer {
	// 改行としての CR だけを除く為の最初の CR 位置
	const first = buf.indexOf(CR);
	// CR が無ければ入力を其のまま返戻
	if(first < 0) return buf;
	const out = Buffer.allocUnsafe(buf.length);
	// 最初の CR より前の一括複製と出力量の初期化
	buf.copy(out, 0, 0, first);
	let written = first;
	// 最初の CR 以後の走査
	for(let i = first; i < buf.length; ++i) {
		const byte = buf[i], next = buf[i + 1];
		if(byte != CR || next === undefined || next != LF) out[written++] = byte;
	}
	// CR 除去後の Buffer を返戻
	return out.subarray(0, written);
}

/**
 * Buffer 内の LF (0X0A) の CRLF への展開関数
 * 計算量：入力バイト数 N に対し O(N)
 * @param buf 入力 Buffer (UTF-8)
 * @returns CRLF 展開後の Buffer（LF 不在なら入力を其のまま返戻）
 */
function expandLF(buf: Buffer): Buffer {
	// 最初の LF 位置
	const first = buf.indexOf(LF);
	// LF が無ければ入力を其のまま返戻
	if(first < 0) return buf;
	const out = Buffer.allocUnsafe(buf.length << 1);
	// 最初の LF より前の一括複製と出力量の初期化
	buf.copy(out, 0, 0, first);
	let written = first;
	// 最初の LF 以後の走査
	for(let i = first; i < buf.length; ++i) {
		if(buf[i] == LF) out[written++] = CR;
		out[written++] = buf[i];
	}
	// CRLF 展開後の Buffer を返戻
	return out.subarray(0, written);
}

/**
 * shavefmt の標準エラーの規約警告から VS Code 診断への変換関数
 * 計算量：標準エラー長 S と警告数 W に対し O(S+W log W)
 * @param stderr stderr に出力された全テキスト
 * @param doc 対象ドキュメント（範囲の上限調整に行数を使用）
 * @param channel 打切を書く出力欄
 * @returns Diagnostic 配列（警告行が無ければ空）
 */
function parseLintDiagnostics(stderr: string, doc: vscode.TextDocument, channel: vscode.OutputChannel): vscode.Diagnostic[] {
	// 位置付と文書全体の警告形式及び診断上限
	const locatedPattern = /^\[warn\] <stdin>:(\d+):(\d+): (.+)$/, documentPattern = /^\[warn\] <stdin>: (.+)$/;
	const lastRow = Math.max(0, doc.lineCount - 1), warnings: LintWarning[] = [];
	let matched = 0;
	// 標準エラーの行走査
	for(const line of stderr.split(/\r?\n/)) {
		// 位置付又は文書警告の照合と上限迄の保存
		const located = locatedPattern.exec(line), document = documentPattern.exec(line);
		if(!located && !document || ++matched > DIAGNOSTIC_MAX) continue;
		warnings.push(
			located ?
			{
				row: Math.min(Math.max(parseInt(located[1], 10) - 1, 0), lastRow),
				col: Math.max(0, parseInt(located[2], 10) - 1),
				message: located[3]
			} :
			{ row: 0, col: 0, message: document?.[1] ?? "" }
		);
	}
	// 診断打切時の総件数の通知
	if(matched > DIAGNOSTIC_MAX) channel.appendLine(`diagnostics truncated at ${DIAGNOSTIC_MAX} of ${matched}`);
	// 位置順への整列と換算状態の初期化
	warnings.sort((lhs: LintWarning, rhs: LintWarning): number => lhs.row - rhs.row || lhs.col - rhs.col);
	const diagnostics: vscode.Diagnostic[] = [];
	let row = -1, text = "", bytes = Buffer.alloc(0), byteAt = 0, charAt = 0;
	// 位置順の警告走査
	for(const warning of warnings) {
		if(warning.row != row) {
			// 現在行の本文と換算状態の更新
			row = warning.row;
			text = doc.lineAt(row).text;
			bytes = Buffer.from(text, "utf8");
			byteAt = charAt = 0;
		}
		const target = Math.min(warning.col, bytes.length);
		// UTF-16 位置への増分換算と換算済位置の更新
		charAt += bytes.subarray(byteAt, target).toString("utf8").length;
		byteAt = target;
		// 該当行のテキスト末尾迄を範囲指定（波線が見える様にする為）
		const diag =
		new vscode.Diagnostic(new vscode.Range(row, charAt, row, text.length), warning.message, vscode.DiagnosticSeverity.Warning);
		diag.source = "shavefmt";
		diagnostics.push(diag);
	}
	// 診断配列の返戻
	return diagnostics;
}

/**
 * shavefmt バイナリの起動に依るドキュメント整形関数
 * @param binPath shavefmt バイナリの絶対パス
 * @param doc 整形対象の VSCode ドキュメント
 * @param channel 失敗時のログ出力先
 * @param diagnostics 規約警告を反映する DiagnosticCollection
 * @param token 整形の取消の合図（保存の繰返等で古い要求を取り消す）
 * @returns 整形結果の TextEdit 配列（変更無・失敗・取消の時は空配列）
 */
async function format(
	binPath: string,
	doc: vscode.TextDocument,
	channel: vscode.OutputChannel,
	diagnostics: vscode.DiagnosticCollection,
	token: vscode.CancellationToken
): Promise<vscode.TextEdit[]> {
	// 文書別の古い診断反映待機の解除と原文状態の取得
	const key = doc.uri.toString();
	clearPendingDiagnostics(key);
	const configured = vscode.workspace.getConfiguration("shavefmt").get<unknown>("chars", DEFAULT_MAX_CHARS);
	const original = doc.getText(), originalVersion = doc.version;
	// 行幅と改行様式の検証及び入力構築
	const maxChars = Number.isSafeInteger(configured) && configured as number > -1 ? configured as number : DEFAULT_MAX_CHARS;
	if(maxChars !== configured) channel.appendLine("shavefmt.chars must be a non-negative integer; using 128");
	const isCRLF = doc.eol == vscode.EndOfLine.CRLF, originalBuf = Buffer.from(original, "utf8");
	const inputBuf = isCRLF ? stripCR(originalBuf) : originalBuf;
	// 子処理へ渡せない大きさの入力見送
	if(inputBuf.length > MAX_INPUT_BYTES) {
		notifySkip(doc, "input exceeds size limit", channel);
		// 上限超過文書への編集無の返戻
		return [];
	}
	// 共有ヘッダ拡張子の判定と子処理の実行
	const isHeader = (doc.languageId === "c" || doc.languageId === "cpp") && path.extname(doc.fileName).toLowerCase() === ".h";
	const result = await runFormatter(binPath, key, inputBuf, isHeader ? "h" : doc.languageId, maxChars, token, channel);
	// 取消要求の通知無の返戻
	if(result.kind === "cancelled") return [];
	// 起動又は受信失敗時の通知と診断解除
	if(result.kind !== "completed") {
		diagnostics.delete(doc.uri);
		notifyWithOutput(vscode.window.showWarningMessage, "Shave Format: Formatting was not applied.", channel);
		// 起動又は受信失敗時の編集無の返戻
		return [];
	}
	// 異常終了時の停止理由の記録と診断解除
	if(result.code ?? 1) {
		const stopped =
		result.signal === "SIGTERM" ? "timed out after 30 seconds" : result.signal ? `terminated by signal ${result.signal}` : "";
		const msg = result.stderr.trim() || stopped;
		channel.appendLine(`shavefmt exited with code ${result.code}${msg ? ": " + msg : ""}`);
		notifyWithOutput(vscode.window.showWarningMessage, "Shave Format: Formatting was not applied.", channel);
		diagnostics.delete(doc.uri);
		// 整形失敗時の編集無の返戻
		return [];
	}
	// 子処理中に文書が変わった場合の旧診断解除
	if(doc.version !== originalVersion || doc.getText() !== original) {
		diagnostics.delete(doc.uri);
		// 変更後の文書への編集無の返戻
		return [];
	}
	// 規約警告と原文改行様式の通知集約及び出力復元
	const stderrRaw = result.stderr;
	const stderrText = stderrRaw +
	(isCRLF ? `${stderrRaw && !stderrRaw.endsWith("\n") ? "\n" : ""}[warn] <stdin>: File uses CRLF line endings\n` : "");
	const formattedBuf = result.stdout, restoredBuf = isCRLF ? expandLF(formattedBuf) : formattedBuf;
	// 原文と同一なら編集を行わない（VSCode 側での再描画・履歴更新を避ける為）
	if(restoredBuf.equals(originalBuf)) {
		// 見送理由の通知と整形済文書への診断反映
		const skipLine = stderrText.split(/\r?\n/).find((line: string): boolean => line.startsWith("[skip] "));
		if(skipLine) notifySkip(doc, skipLine.slice("[skip] ".length).replace(/^<stdin>: /, ""), channel);
		diagnostics.set(doc.uri, parseLintDiagnostics(stderrText, doc, channel));
		// 原文と同一の場合の返戻
		return [];
	}
	// 整形後の本文と編集適用後の診断更新購読
	const restored = restoredBuf.toString("utf8");
	const applyWatcher = vscode.workspace.onDidChangeTextDocument(
		(event: vscode.TextDocumentChangeEvent): void => {
			// 対象外文書の変更時の返戻
			if(event.document.uri.toString() !== key) return;
			// 対象文書の待機解除と変更内容に応じた診断更新
			clearPendingDiagnostics(key);
			if(event.document.version !== originalVersion + 1 || event.document.getText() !== restored) {
				diagnostics.delete(event.document.uri);
				return;
			}
			// 整形結果への診断反映
			diagnostics.set(event.document.uri, parseLintDiagnostics(stderrText, event.document, channel));
			// 終了
			return;
		}
	);
	// 編集適用待機の時間切れと文書毎の待機情報の登録
	const timer = setTimeout((): void => clearPendingDiagnostics(key), DIAGNOSTIC_WAIT_MS);
	pendingDiagnostics.set(key, { watcher: applyWatcher, timer });
	// 文書全体の置換範囲
	const fullRange = new vscode.Range(new vscode.Position(0, 0), doc.positionAt(original.length));
	// 整形後の文書への置換の返戻
	return [vscode.TextEdit.replace(fullRange, restored)];
}

/**
 * 実行 OS とアーキテクチャに対応する shavefmt バイナリのパス解決関数
 * @param extensionPath 拡張機能のインストールルート
 * @param channel 失敗時のログ出力先
 * @returns バイナリの絶対パス（無ければ null）
 */
function resolveBinPath(extensionPath: string, channel: vscode.OutputChannel): string | null {
	// 実行環境に対応する経路の構築
	const binary =
	path.join(extensionPath, "bin", `shavefmt-${process.platform}-${process.arch}${process.platform === "win32" ? ".exe" : ""}`);
	// バイナリ存在時の返戻
	if(fileSystem.existsSync(binary)) return binary;
	// バイナリ不在の記録
	channel.appendLine(`shavefmt binary not found: ${binary}`);
	// 解決失敗時の null の返戻
	return null;
}

/**
 * 拡張機能の起動処理関数（全対応言語への整形プロバイダ登録）
 * @param context 拡張機能のコンテキスト
 */
export function activate(context: vscode.ExtensionContext): void {
	// 出力欄・診断集合・実行バイナリの準備
	const channel = vscode.window.createOutputChannel("Shave Format");
	context.subscriptions.push(channel);
	const diagnostics = vscode.languages.createDiagnosticCollection("shaveformat");
	context.subscriptions.push(diagnostics);
	const binPath = resolveBinPath(context.extensionPath, channel);
	if(!binPath) {
		notifyWithOutput(
			vscode.window.showErrorMessage,
			"Shave Format: The formatter binary for this platform was not found; reinstall the extension.",
			channel
		);
		// 終了
		return;
	}
	// Linux / macOS は実行権限を保証する（vsix からの展開で失われる場合の保険）
	if(process.platform !== "win32") try {
		fileSystem.chmodSync(binPath, EXECUTABLE_MODE);
	} catch(e) {
		channel.appendLine(`chmod failed: ${(e as Error).message}`);
	}
	// 文書終了時の状態解放登録
	context.subscriptions.push(
		vscode.workspace.onDidCloseTextDocument(
			(doc: vscode.TextDocument): void => {
				// 文書固有状態の解放
				const key = doc.uri.toString();
				notifiedSkips.delete(key);
				clearPendingDiagnostics(key);
				// 診断情報の破棄
				diagnostics.delete(doc.uri);
				// 終了
				return;
			}
		)
	);
	// 拡張機能終了時の残存待機解放
	context.subscriptions.push(
		{
			/** 残存する診断反映待機を全て解除する */
			dispose(): void {
				// 全文書の残存待機解除
				for(const key of [...pendingDiagnostics.keys()]) clearPendingDiagnostics(key);
				// 終了
				return;
			}
		}
	);
	// 全対応言語で共有する整形提供器の構築と登録
	const provider: vscode.DocumentFormattingEditProvider = {
		/**
		 * 文書整形結果の編集列を提供する
		 * @param doc 整形対象文書
		 * @param _options 編集器の整形設定
		 * @param token 取消の合図
		 * @returns 文書へ適用する編集列
		 */
		provideDocumentFormattingEdits(
			doc: vscode.TextDocument,
			_options: vscode.FormattingOptions,
			token: vscode.CancellationToken
		): Promise<vscode.TextEdit[]> {
			// 整形の編集の返戻
			return format(binPath, doc, channel, diagnostics, token);
		}
	};
	context.subscriptions.push(vscode.languages.registerDocumentFormattingEditProvider(SUPPORTED_LANGUAGE_IDS, provider));
	// 終了
	return;
}
