import {
	forDisplay,
	MAX_INPUT_BYTES,
	type ProcessResult,
	QUEUED_REQUEST_CAUSE,
	QUEUED_REQUEST_REMEDY,
	runFormatter
} from "./formatterProcess";

import * as vscode from "vscode";
import * as fileSystem from "fs";
import * as path from "path";

// 整形処理の既定値と制御文字
const DEFAULT_MAX_CHARS: number = 128; // 既定の最大行幅
const DIAGNOSTIC_MAX: number = 1000; // 診断件数上限
const DIAGNOSTIC_WAIT_MS: number = 5_000; // 編集適用待機の上限５秒
const RESOURCE_ERROR_CODES: Set<string> = new Set(["EAGAIN", "EMFILE", "ENFILE", "ENOMEM"]); // 一時的な起動リソースの不足

const RESOURCE_REMEDY: string =
"Retry after releasing memory or execution resources; report persistent failures with the formatter version.";

const SMALLER_INPUT_REMEDY: string = "Format smaller independently valid inputs.";
const SIZE_LIMIT_REMEDY: string = `Provide independently valid input within ${MAX_INPUT_BYTES / 0X100000} MiB.`;

// `--lang` へ直接渡す shavefmt 対応 VS Code 言語識別子
const SUPPORTED_LANG_IDS: string[] = [
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

// 通知済ドキュメントの URI 集合
const notifiedSessionReasons: Set<string> = new Set<string>(); // セッション内の通知済の理由
const notifiedSettings: Set<string> = new Set<string>(); // 通知済の設定名
const notifiedReasons: Map<string, Set<string>> = new Map<string, Set<string>>(); // ドキュメント毎の通知済の理由

type OutputLog = Pick<vscode.OutputChannel, "appendLine"|"show">&{ name: string|undefined }; // ドキュメント別の記録と表示（起動時の名前は未指定）
type NotificationAction = { title: string, run: () => Thenable<unknown> }; // 通知から実行する対処
// 整形編集の適用を待つドキュメント毎の診断更新
type PendingDiagnostics = { watcher: vscode.Disposable, timer: ReturnType<typeof setTimeout> }; // 診断の反映待機

const pendingDiagnostics: Map<string, PendingDiagnostics> = new Map<string, PendingDiagnostics>();

// 規約警告の位置と文面
type LintRecord = { row: number, col: number, msg: string, isNotice: boolean, isSkip: boolean, isLocated: boolean };
type SkipReason = { cause: string, remedy: string|undefined }; // 見送の原因と直後の対処（対処が無ければ未指定）
type ParsedDiagnostics = { diagnostics: vscode.Diagnostic[], skip: SkipReason|undefined }; // 診断と最初の見送（見送が無ければ未指定）

type FailureReason =
{ cause: string, remedy: string, scope: "document"|"session"|"transient", canReport: boolean, isInstallation: boolean }; // document は診断を残し，session は全体で重複の通知を抑え，transient はドキュメント毎の通知だけ // 原因と対処及び診断・通知・報告・導入の区分

/**
 * ドキュメントの識別と時刻を各記録へ添える
 * 計算量：表示するパスと記録の総長 L に対して O(L)
 * @param doc 対象ドキュメント（起動時は未指定）
 * @param channel 出力パネル
 * @return 記録と表示の操作
 */
function documentOutput(doc: vscode.TextDocument|undefined, channel: vscode.OutputChannel): OutputLog {
	const relative: string = doc ? vscode.workspace.asRelativePath(doc.uri) : "extension";
	const name: string = forDisplay(
		path.isAbsolute(relative) || /^[A-Za-z]:[\\/]/.test(relative) ? relative.split(/[\\/]/).at(-1) ?? "document" : relative
	);
	// 記録時点の時刻を付ける操作の返戻
	return {
		name: doc ? name : undefined,
		appendLine: (message: string): void => {
			for(const line of message.split(/\r?\n/)) {
				channel.appendLine(`[${new Date().toLocaleTimeString()}] ${name}: ${forDisplay(line)}`);
			}
			// 終了
			return;
		},
		show: (): void => channel.show(true)
	};
}

/**
 * 診断反映待機の解除関数
 * 計算量：O(1)
 * @param key ドキュメントを一意に表す URI
 */
function clearPendingDiagnostics(key: string): void {
	// ドキュメントの待機中の診断の取得
	const pending: PendingDiagnostics|undefined = pendingDiagnostics.get(key);
	// 待機が無い場合の終了
	if(!pending) return;
	// ドキュメント変更購読と時間切処理の解除及び待機情報の除去
	pending.watcher.dispose();
	clearTimeout(pending.timer);
	pendingDiagnostics.delete(key);
	// 終了
	return;
}

/**
 * 出力パネルへの導線を添えた通知関数（出力パネルは既定で不可視の為，詳細は出力パネルへ書いて通知から開ける様にする）
 * 通知には安全化したドキュメント名と固定文を示し，詳細は出力パネルへ記録する
 * 計算量：通知のテキスト長 L に対し O(L)
 * @param msg 通知の本文
 * @param show 通知の表示関数
 * @param channel 出力パネル
 * @param actions 追加の対処
 */
function notifyWithOutput(
	msg: string,
	show: (msg: string, ...items: string[]) => Thenable<string|undefined>,
	channel: OutputLog,
	...actions: NotificationAction[]
): void {
	// 出力パネルを開く選択肢
	const open: string = "Show Output", titles: string[] = actions.map((action: NotificationAction): string => action.title);
	const hiddenAt: number = titles.indexOf("Don't Show Again");
	titles.splice(hiddenAt < 0 ? titles.length : hiddenAt, 0, open);
	void Promise.resolve(show(msg, ...titles)).then(
		(choice: string|undefined): unknown => {
			// 選択結果の反映
			if(choice === open) channel.show(true);
			// 選択した対処の実行結果の返戻
			else return actions.find((action: NotificationAction): boolean => choice === action.title)?.run();
			// 終了
			return;
		}
	).catch(
		(error: unknown): void =>
		channel.appendLine(forDisplay(`notification failed: ${error instanceof Error ? error.message : String(error)}`))
	);
	// 終了
	return;
}

/**
 * 同じドキュメント，又はセッションの同じ理由は１度だけ通知する
 * 計算量：理由と URI の総長 L に対して O(L)
 * @param doc 対象ドキュメント（セッション単位なら未指定）
 * @param reason 固定文の理由
 * @param show 通知の表示関数
 * @param channel 記録先
 * @param actions 追加の対処
 */
function notifyReason(
	doc: vscode.TextDocument|undefined,
	reason: string,
	show: (msg: string, ...items: string[]) => Thenable<string|undefined>,
	channel: OutputLog,
	...actions: NotificationAction[]
): void {
	const key: string|undefined = doc?.uri.toString();
	const reasons: Set<string> = key === undefined ? notifiedSessionReasons : notifiedReasons.get(key) ?? new Set<string>();
	channel.appendLine(reason);
	// 通知済の理由の場合の終了
	if(reasons.has(reason)) return;
	reasons.add(reason);
	if(key !== undefined) notifiedReasons.set(key, reasons);
	notifyWithOutput(`Shave Format: ${channel.name === undefined ? "" : channel.name + ": "}${reason}`, show, channel, ...actions);
	// 終了
	return;
}

/**
 * CLI の対処を通知と Problems の英文として表示する
 * 計算量：本文長 L に対して O(L)
 * @param text 表示する対処
 * @return 安全化し，先頭と文末を揃えた対処
 */
function displayRemedy(text: string): string {
	const message: string = forDisplay(text).replace(/^[a-z]/, (initial: string): string => initial.toUpperCase());
	return message.endsWith(".") ? message : message + ".";
}

/**
 * 適用しない理由を先頭行の診断へ反映する
 * 計算量：先頭行と理由の総長 L に対して O(L)
 * @param doc 対象ドキュメント
 * @param reason 固定文の原因
 * @param remedy 利用者が実行出来る対処
 * @param severity 失敗の重大度
 * @param diagnostics 診断の反映先
 */
function setFailureDiagnostic(
	doc: vscode.TextDocument,
	reason: string,
	remedy: string,
	severity: vscode.DiagnosticSeverity,
	diagnostics: vscode.DiagnosticCollection
): void {
	const range: vscode.Range = new vscode.Range(new vscode.Position(0, 0), new vscode.Position(0, doc.lineAt(0).text.length));
	const diagnostic: vscode.Diagnostic = new vscode.Diagnostic(range, reason, severity);
	diagnostic.source = "shavefmt";
	diagnostic.relatedInformation =
	[new vscode.DiagnosticRelatedInformation(new vscode.Location(doc.uri, range), displayRemedy(remedy))];
	diagnostics.set(doc.uri, [diagnostic]);
	// 終了
	return;
}

/**
 * 改行変更の情報通知へ設定から戻せる非表示の選択を添える
 * 計算量：O(1)
 * @param channel 記録先
 */
function notifyLineEndings(channel: OutputLog): void {
	// 宣言済の設定と，成功して適用した改行変更の情報
	const setting: vscode.WorkspaceConfiguration = vscode.workspace.getConfiguration("shavefmt");
	const reason: string = "Line endings changed from CRLF to LF.";
	channel.appendLine(reason);
	if(setting.get<boolean>("notifyLineEndingChanges", true)) {
		notifyWithOutput(
			`Shave Format: ${channel.name}: ${reason}`,
			vscode.window.showInformationMessage,
			channel,
			{
				title: "Don't Show Again",
				run: (): Thenable<void> => setting.update("notifyLineEndingChanges", false, vscode.ConfigurationTarget.Global)
			}
		);
	}
	// 終了
	return;
}

/**
 * CLI の失敗から通知を許す固定文だけを取り出す
 * 計算量：標準エラー長 N に対して O(N)
 * @param stderr 子プロセスの診断
 * @return 通知に使う固定文の理由
 */
function failureReason(stderr: string): FailureReason {
	const validation: RegExpMatchArray|null = stderr.match(
		/^Error: formatting would (break syntax|change the meaning of the code|change a directive comment|change a documentation comment|change blank lines|change code structure|remove a required block body), left unchanged: /m
	);
	// 検証の拒否理由の照合
	if(validation) {
		// 検証の拒否理由の返戻
		return {
			cause: `output would ${validation[1]}`,
			remedy: "Report the issue with the formatter version.",
			scope: "document",
			canReport: true,
			isInstallation: false
		};
	}
	// リソース不足の固定理由の取得
	const resource: RegExpMatchArray|null = stderr.match(
		/^Error: formatting could not be completed \((estimated working memory exceeds resource limit|out of memory or execution resources)\)(?:, left unchanged: [^\r\n]+)?\r?$/m
	);
	// リソース不足の理由と対処の選択
	if(resource) {
		// リソース不足の理由と対処の返戻
		return {
			cause: resource[1],
			remedy: resource[1] === "estimated working memory exceeds resource limit" ? SMALLER_INPUT_REMEDY : RESOURCE_REMEDY,
			scope: "document",
			canReport: resource[1] === "out of memory or execution resources",
			isInstallation: false
		};
	}
	// パーサーの初期化失敗の照合
	if(
		/^Error: formatting could not be completed \(parser initialization failed\)(?:, left unchanged: [^\r\n]+)?\r?$/m.test(stderr)
	) {
		// パーサーの初期化失敗の固定理由の返戻
		return {
			cause: "parser initialization failed",
			remedy: installationRemedy(),
			scope: "document",
			canReport: true,
			isInstallation: true
		};
	}
	// 不具合に依る停止理由の照合
	if(/^Error: formatter defect, left unchanged: /m.test(stderr)) {
		// 不具合に依る停止理由の返戻
		return {
			cause: "formatter defect",
			remedy: "Report the issue with the formatter version.",
			scope: "document",
			canReport: true,
			isInstallation: false
		};
	}
	// 未知の本文を通知へ転記しない固定文の返戻
	return {
		cause: "formatter process failed",
		remedy: "See Output for details.",
		scope: "document",
		canReport: false,
		isInstallation: false
	};
}

/**
 * 実行環境に合う拡張機能の配布対象を案内する
 * @return 必要なパッケージを含む固定文
 */
function installationRemedy(): string {
	return `Install the ${process.platform}-${process.arch} package of Shave Format again.`;
}

/**
 * 原因と対処を通知し，不具合の報告には原因だけを渡す
 * 計算量：ドキュメント名と原因及び対処の総長 L に対して O(L)
 * @param extensionId 問題の報告先の拡張機能識別子
 * @param doc 対象ドキュメント
 * @param reason 原因と対処及び診断・通知・失敗の区分
 * @param channel 記録先
 * @param diagnostics 診断の反映先
 */
function notifyFailure(
	extensionId: string,
	doc: vscode.TextDocument,
	reason: FailureReason,
	channel: OutputLog,
	diagnostics: vscode.DiagnosticCollection
): void {
	const title: string = `Formatting was not applied: ${reason.cause}.`;
	const message: string = `${title} ${displayRemedy(reason.remedy)}`, actions: NotificationAction[] = [];
	// 報告には原因だけを渡し，原文・パス・対処の指示を混ぜない
	if(reason.canReport) {
		actions.push(
			{
				title: "Report Issue",
				run: (): Thenable<unknown> =>
				vscode.commands.executeCommand("workbench.action.openIssueReporter", { extensionId, issueTitle: title })
			}
		);
	}
	const severity: vscode.DiagnosticSeverity =
	reason.isInstallation ? vscode.DiagnosticSeverity.Error : vscode.DiagnosticSeverity.Warning;
	if(reason.scope === "document") setFailureDiagnostic(doc, title, reason.remedy, severity, diagnostics);
	notifyReason(
		reason.scope === "session" ? undefined : doc,
		message,
		reason.isInstallation ? vscode.window.showErrorMessage : vscode.window.showWarningMessage,
		channel,
		...actions
	);
	// 終了
	return;
}

/**
 * 見送の通知関数（同じドキュメントと理由の組毎に成功迄１度だけ知らせる）
 * 計算量：ドキュメント名と理由の総長 L に対し O(L)
 * @param doc 見送ったドキュメント
 * @param reason 見送の原因と CLI の直後の対処
 * @param channel 出力パネル
 */
function notifySkip(doc: vscode.TextDocument, reason: SkipReason, channel: OutputLog): void {
	// 診断と同じ接頭辞の文面で，ドキュメントと理由を通知する
	const text: string = /^[A-Za-z0-9 .-]+$/.test(reason.cause) ? reason.cause : "unsupported input";
	const remedy: string = reason.remedy ? ` ${displayRemedy(reason.remedy)}` : "";
	notifyReason(doc, `Formatting was skipped: ${text}.${remedy}`, vscode.window.showInformationMessage, channel);
	// 終了
	return;
}

/**
 * shavefmt の標準エラーの規約警告から VS Code 診断への変換関数
 * 計算量：標準エラー長 S と警告数 W に対し O(S+W log W)
 * @param doc 対象ドキュメント（範囲の上限調整に行数を使用）
 * @param stderr stderr に出力された全テキスト
 * @param channel 打切を書く出力パネル
 * @return 主診断と関連情報及び通知する最初の見送（該当行が無ければ空・未指定）
 */
function parseLintDiagnostics(doc: vscode.TextDocument, stderr: string, channel: OutputLog): ParsedDiagnostics {
	// 位置付とドキュメント全体の警告形式及び診断上限
	const locatedPattern: RegExp = /^\[(notice|skip|warn)\] <stdin>(?::(\d+):(\d+))?: (.+)$/;
	const lastRow: number = Math.max(0, doc.lineCount - 1), warnings: LintRecord[] = [];
	let matched: number = 0, previous: RegExpExecArray|null = null, firstSkip: RegExpExecArray|undefined, remedy: string|undefined;
	// LF で完結した標準エラーの行だけの走査
	for(const line of stderr.slice(0, stderr.lastIndexOf("\n") + 1).split(/\r?\n/)) {
		if(line.startsWith("[notice] ")) channel.appendLine(forDisplay(line));
		// 位置付警告の照合と上限迄の保存
		const located: RegExpExecArray|null = locatedPattern.exec(line);
		if(!located) {
			previous = null;
			continue;
		}
		const isRemedy: boolean = located[1] === "notice" && previous?.[1] === "skip" &&
		(!located[2] || located[2] === previous[2] && located[3] === previous[3]);
		if(located[1] === "skip" && !firstSkip) firstSkip = located;
		else if(isRemedy && previous === firstSkip) remedy = located[4];
		const position: RegExpExecArray = isRemedy && !located[2] && previous ? previous : located;
		previous = located;
		if(located[1] !== "skip" && !located[2] && !isRemedy || ++matched > DIAGNOSTIC_MAX) continue;
		warnings.push(
			{
				row: Math.max(parseInt(position[2] ?? "1", 10) - 1, 0),
				col: Math.max(0, parseInt(position[3] ?? "1", 10) - 1),
				msg: located[4],
				isNotice: located[1] === "notice",
				isSkip: located[1] === "skip",
				isLocated: !!position[2]
			}
		);
	}
	// 診断打切時の総件数の通知
	if(matched > DIAGNOSTIC_MAX) channel.appendLine(`diagnostics truncated at ${DIAGNOSTIC_MAX} of ${matched}`);
	// 位置順への整列と換算状態の初期化
	warnings.sort(
		(lhs: LintRecord, rhs: LintRecord): number =>
		lhs.row - rhs.row || lhs.col - rhs.col || Number(lhs.isLocated) - Number(rhs.isLocated)
	);
	const diagnostics: vscode.Diagnostic[] = [];
	let row: number = -1, text: string = "", bytes: Buffer = Buffer.alloc(0), byteAt: number = 0, charAt: number = 0;
	// 位置順の警告走査
	for(let idx: number = 0; idx < warnings.length;) {
		const warning: LintRecord = warnings[idx], boundedRow: number = Math.min(warning.row, lastRow);
		if(boundedRow != row) {
			// 現在行の本文と換算状態の更新
			row = boundedRow;
			text = doc.lineAt(row).text;
			bytes = Buffer.from(text, "utf8");
			byteAt = charAt = 0;
		}
		const target: number = Math.min(warning.col, bytes.length);
		// UTF-16 位置への増分換算と換算済位置の更新
		charAt += bytes.subarray(byteAt, target).toString("utf8").length;
		byteAt = target;
		// 位置付の診断は語又は１文字，位置の無い見送は先頭行全体を範囲にする
		const position: vscode.Position = new vscode.Position(row, Math.min(charAt, text.length));
		const end: number = Math.min(position.character + ((text.codePointAt(position.character) ?? 0) > 0XFFFF ? 2 : 1), text.length);
		const range: vscode.Range = warning.isLocated ?
		doc.getWordRangeAtPosition(position) ?? new vscode.Range(position, new vscode.Position(row, end)) :
		new vscode.Range(new vscode.Position(0, 0), new vscode.Position(0, text.length));
		const primary: vscode.Diagnostic[] = [], notices: vscode.DiagnosticRelatedInformation[] = [];
		// 同じ原位置の主診断と対処を１度だけ纏める
		do {
			const record: LintRecord = warnings[idx++];
			if(record.isNotice) {
				notices.push(new vscode.DiagnosticRelatedInformation(new vscode.Location(doc.uri, range), displayRemedy(record.msg)));
			} else {
				const diagnostic: vscode.Diagnostic = new vscode.Diagnostic(
					range,
					forDisplay(record.isSkip ? `Formatting was skipped: ${record.msg}.` : record.msg),
					record.isSkip || record.msg.startsWith("Review:") ? vscode.DiagnosticSeverity.Information : vscode.DiagnosticSeverity.Warning
				);
				diagnostic.source = "shavefmt";
				primary.push(diagnostic);
			}
		} while(
			idx < warnings.length && warnings[idx].row === warning.row && warnings[idx].col === warning.col &&
			warnings[idx].isLocated === warning.isLocated
		);
		// 対処の配列は主診断で共有し，主が無い対処だけ情報の診断にする
		if(primary.length) for(const diagnostic of primary) {
			if(notices.length) diagnostic.relatedInformation = notices;
			diagnostics.push(diagnostic);
		} else for(const notice of notices) {
			const diagnostic: vscode.Diagnostic = new vscode.Diagnostic(range, notice.message, vscode.DiagnosticSeverity.Information);
			diagnostic.source = "shavefmt";
			diagnostics.push(diagnostic);
		}
	}
	// 診断と最初の見送の原因・対処の返戻
	return { diagnostics, skip: firstSkip ? { cause: firstSkip[4], remedy } : undefined };
}

/**
 * UTF-8 へ損失無で変換出来る UTF-16 の本文かを確認する
 * 計算量：本文のコードユニット数 N に対して O(N)，追加の領域は O(1)
 * @param text 対象の本文
 * @return 対を成さないサロゲートを含まない場合は true
 */
function isWellFormedUtf16(text: string): boolean {
	for(let idx: number = 0; idx < text.length; ++idx) {
		const unit: number = text.charCodeAt(idx);
		if(unit > 0XD7FF && unit < 0XDC00) {
			const next: number = text.charCodeAt(idx + 1);
			// 上位に対応する下位サロゲートが無い場合の返戻
			if(!(next > 0XDBFF && next < 0XE000)) return false;
			++idx;
		// 対応する上位の無い下位サロゲートの返戻
		} else if(unit > 0XDBFF && unit < 0XE000) return false;
	}
	// 本文全体を損失無で変換出来る場合の返戻
	return true;
}

/**
 * shavefmt バイナリの起動に依るドキュメント整形関数
 * 計算量：入力のテキスト長 B，警告数 W，警告のテキスト長 L，子プロセスの費用 Q に対し O(B + W log(W + 1) + L + Q)
 * @param extensionId 問題の報告先の拡張機能識別子
 * @param doc 整形対象の VS Code ドキュメント
 * @param binaryPath shavefmt バイナリの絶対パス
 * @param token 整形の取消の合図（保存の繰返等で古い要求を取り消す）
 * @param channel 失敗時のログ出力先
 * @param diagnostics 規約警告を反映する DiagnosticCollection
 * @return 整形結果の TextEdit 配列（変更無・失敗・取消の時は空配列）
 */
async function format(
	extensionId: string,
	doc: vscode.TextDocument,
	binaryPath: string,
	token: vscode.CancellationToken,
	channel: OutputLog,
	diagnostics: vscode.DiagnosticCollection
): Promise<vscode.TextEdit[]> {
	// ドキュメント別の古い診断反映待機の解除と原文状態の取得
	const key: string = doc.uri.toString();
	clearPendingDiagnostics(key);
	const configured: unknown = vscode.workspace.getConfiguration("shavefmt", doc).get<unknown>("chars", DEFAULT_MAX_CHARS);
	const original: string = doc.getText(), originalVersion: number = doc.version;
	// 行幅の検証と入力構築（改行様式を変えずに渡し，BOM 無・LF の出力を其の儘使う）
	const isValidChars: boolean = Number.isSafeInteger(configured) && configured as number > -1;
	const maxChars: number = isValidChars ? configured as number : DEFAULT_MAX_CHARS;
	if(!isValidChars) {
		const setting: string = "shavefmt.chars";
		channel.appendLine("shavefmt.chars must be a non-negative integer; using 128");
		if(!notifiedSettings.has(setting)) {
			notifiedSettings.add(setting);
			notifyWithOutput(
				`Shave Format: ${channel.name}: shavefmt.chars must be a non-negative integer; using 128.`,
				vscode.window.showWarningMessage,
				channel,
				{
					title: "Open Settings",
					run: (): Thenable<unknown> => vscode.commands.executeCommand("workbench.action.openSettings", setting)
				}
			);
		}
	}
	// UTF-8 への変換で原文の値を失う本文は，子プロセスへ渡さない
	if(!isWellFormedUtf16(original)) {
		const parsed: ParsedDiagnostics = parseLintDiagnostics(
			doc,
			"[skip] <stdin>: unpaired UTF-16 surrogate\n" +
			"[notice] <stdin>: Replace unpaired UTF-16 surrogates with the intended Unicode characters and retry.\n",
			channel
		);
		diagnostics.set(doc.uri, parsed.diagnostics);
		if(parsed.skip) notifySkip(doc, parsed.skip, channel);
		// 原文のコードユニットを保持し，編集無の返戻
		return [];
	}
	const inputBuf: Buffer = Buffer.from(original, "utf8");
	// 子プロセスへ渡せない大きさの入力見送
	if(inputBuf.length > MAX_INPUT_BYTES) {
		const parsed: ParsedDiagnostics =
		parseLintDiagnostics(doc, `[skip] <stdin>: input exceeds size limit\n[notice] <stdin>: ${SIZE_LIMIT_REMEDY}\n`, channel);
		diagnostics.set(doc.uri, parsed.diagnostics);
		if(parsed.skip) notifySkip(doc, parsed.skip, channel);
		// 上限超過ドキュメントへの編集無の返戻
		return [];
	}
	// 共有ヘッダ拡張子の判定と子プロセスの実行
	const ext: string = path.extname(doc.fileName).toLowerCase();
	const isHeader: boolean = (doc.languageId === "c" || doc.languageId === "cpp") && ext === ".h";
	const lang: string = isHeader ? "h" : doc.languageId === "javascript" && ext === ".mjs" ?
	"mjs" :
	doc.languageId === "javascript" && ext === ".cjs" ? "cjs" : doc.languageId;
	const result: ProcessResult = await runFormatter(binaryPath, inputBuf, key, lang, maxChars, token, channel);
	// 取消要求の通知無の返戻
	if(result.kind === "cancelled") return [];
	// 起動又は受信失敗時の通知と診断
	if(result.kind !== "completed") {
		const reason: FailureReason = result.kind === "busy" ?
		{ cause: QUEUED_REQUEST_CAUSE, remedy: QUEUED_REQUEST_REMEDY, scope: "session", canReport: false, isInstallation: false } :
		result.kind === "overflow" ?
		{
			cause: "formatter returned more output than its limit",
			remedy: "Report the issue with the formatter version.",
			scope: "document",
			canReport: true,
			isInstallation: false
		} :
		result.kind === "spawnError" && RESOURCE_ERROR_CODES.has(result.errorCode ?? "") ?
		{
			cause: "out of memory or execution resources",
			remedy: RESOURCE_REMEDY,
			scope: "transient",
			canReport: true,
			isInstallation: false
		} :
		{
			cause: "formatter could not start",
			remedy: installationRemedy(),
			scope: "transient",
			canReport: false,
			isInstallation: true
		};
		notifyFailure(extensionId, doc, reason, channel, diagnostics);
		// 起動又は受信失敗時の編集無の返戻
		return [];
	}
	// 異常終了時の理由の記録と診断
	if(result.code ?? 1) {
		const stopped: string = result.signal ? `terminated by signal ${result.signal}` : "";
		const msg: string = result.stderr.trim() || stopped;
		channel.appendLine(`shavefmt exited with code ${result.code}${msg ? ": " + msg : ""}`);
		const reason: FailureReason = result.signal ?
		{
			cause: "formatter crashed",
			remedy: "Report the issue with the formatter version.",
			scope: "document",
			canReport: true,
			isInstallation: false
		} :
		failureReason(result.stderr);
		notifyFailure(extensionId, doc, reason, channel, diagnostics);
		// 整形失敗時の編集無の返戻
		return [];
	}
	// 子プロセス中にドキュメントが変わった場合の旧診断解除
	if(doc.version != originalVersion || doc.getText() !== original) {
		diagnostics.delete(doc.uri);
		// 変更後のドキュメントへの編集無の返戻
		return [];
	}
	// 規約警告と改行様式の通知（フォーマッターが出す）及び整形結果
	const stderrText: string = result.stderr, formattedBuf: Buffer = result.stdout;
	// 原文と同一なら編集を行わない（VS Code 側での再描画・履歴更新を避ける為）
	if(formattedBuf.equals(inputBuf)) {
		// 見送理由の通知と整形済ドキュメントへの診断反映
		const parsed: ParsedDiagnostics = parseLintDiagnostics(doc, stderrText, channel);
		if(parsed.skip) notifySkip(doc, parsed.skip, channel);
		else notifiedReasons.delete(key);
		diagnostics.set(doc.uri, parsed.diagnostics);
		// 原文と同一の場合の返戻
		return [];
	}
	// 整形後の本文と編集適用後の診断更新購読
	const formatted: string = formattedBuf.toString("utf8");
	// VS Code が１つの改行様式で表せる本文だけを適用する
	let outputEol: vscode.EndOfLine|undefined;
	for(const ending of formatted.matchAll(/\r\n|\r|\n/g)) {
		const nextEol: vscode.EndOfLine = ending[0] === "\r\n" ? vscode.EndOfLine.CRLF : vscode.EndOfLine.LF;
		if(ending[0] === "\r" || outputEol !== undefined && outputEol != nextEol) {
			const reason: string = ending[0] === "\r" ?
			"Formatting was not applied: VS Code cannot preserve lone CR inside literals." :
			"Formatting was not applied: preserving CR in multiline literals would mix LF and CRLF.";
			const remedy: string = "Change the document to LF and retry; this also changes CR characters inside literals.";
			setFailureDiagnostic(doc, reason, remedy, vscode.DiagnosticSeverity.Warning, diagnostics);
			notifyReason(
				doc,
				`${reason} ${remedy}`,
				vscode.window.showWarningMessage,
				channel,
				{
					title: "Change End of Line Sequence",
					run: async (): Promise<unknown> => {
						await vscode.window.showTextDocument(doc);
						// 対象ドキュメントの改行選択の返戻
						return vscode.commands.executeCommand("workbench.action.editor.changeEOL");
					}
				}
			);
			// 検証済の逐語内容を保持出来ない本文への編集無の返戻
			return [];
		}
		outputEol = nextEol;
	}
	// 改行の無い本文は現在の様式を保つ
	outputEol ??= doc.eol;
	const isEolChanging: boolean = outputEol != doc.eol;
	const isChangingToLf: boolean = doc.eol === vscode.EndOfLine.CRLF && outputEol === vscode.EndOfLine.LF;
	const applyWatcher: vscode.Disposable = vscode.workspace.onDidChangeTextDocument(
		(event: vscode.TextDocumentChangeEvent): void => {
			// 対象外ドキュメントの変更時の返戻
			if(event.document.uri.toString() !== key) return;
			// 整形結果を適用済か
			const isApplied: boolean = event.document.getText() === formatted;
			// 改行様式の切替と本文の置換を別のバージョンで適用するエディターの途中のバージョンを待つ場合の終了
			if(!isApplied && isEolChanging && event.document.version == originalVersion + 1) return;
			// 対象ドキュメントの待機解除と変更内容に応じた診断更新
			clearPendingDiagnostics(key);
			if(!isApplied || event.document.version > originalVersion + (isEolChanging ? 2 : 1)) {
				diagnostics.delete(event.document.uri);
				// 整形結果と異なるバージョンの診断を消した場合の終了
				return;
			}
			// 適用を確認した改行様式の変更だけを通知
			if(isChangingToLf) notifyLineEndings(channel);
			// 整形結果への診断反映
			notifiedReasons.delete(key);
			diagnostics.set(event.document.uri, parseLintDiagnostics(event.document, stderrText, channel).diagnostics);
			// 終了
			return;
		}
	);
	// 編集適用待機の時間切とドキュメント毎の待機情報の登録
	const timer: ReturnType<typeof setTimeout> = setTimeout((): void => clearPendingDiagnostics(key), DIAGNOSTIC_WAIT_MS);
	pendingDiagnostics.set(key, { watcher: applyWatcher, timer });
	// ドキュメント全体の置換範囲
	const fullRange: vscode.Range = new vscode.Range(new vscode.Position(0, 0), doc.positionAt(original.length));
	// 整形後のドキュメントへの置換と，検証済の本文に一致する改行様式への切替
	const edits: vscode.TextEdit[] = [vscode.TextEdit.replace(fullRange, formatted)];
	if(isEolChanging) edits.push(vscode.TextEdit.setEndOfLine(outputEol));
	// 置換の編集の返戻
	return edits;
}

/**
 * 実行 OS とアーキテクチャに対応する shavefmt バイナリのパスを求める
 * 計算量：パス長 P に対し O(P)
 * @param extensionPath 拡張機能のインストールルート
 * @return バイナリの絶対パス
 */
function resolveBinPath(extensionPath: string): string {
	const arch: string = process.platform === "win32" && process.arch === "arm64" ? "x64" : process.arch;
	// 配布済の実行ファイルのパスの返戻
	return path.join(extensionPath, "bin", `shavefmt-${process.platform}-${arch}${process.platform === "win32" ? ".exe" : ""}`);
}

/**
 * 拡張機能の起動処理関数（全対応言語への整形プロバイダ登録）
 * 計算量：登録する言語数 L に対し O(L)
 * @param ctx 拡張機能のコンテキスト
 */
export function activate(ctx: vscode.ExtensionContext): void {
	// 出力パネル・診断集合・実行バイナリの準備
	const channel: vscode.OutputChannel = vscode.window.createOutputChannel("Shave Format");
	ctx.subscriptions.push(channel);
	const diagnostics: vscode.DiagnosticCollection = vscode.languages.createDiagnosticCollection("shaveformat");
	ctx.subscriptions.push(diagnostics);
	const startupLog: OutputLog = documentOutput(undefined, channel), binaryPath: string = resolveBinPath(ctx.extensionPath);
	const missingBinary: string =
	`Bundled formatter for ${process.platform}-${process.arch} was not found. ${installationRemedy()}`;
	if(!fileSystem.existsSync(binaryPath)) {
		startupLog.appendLine(`shavefmt binary not found: ${binaryPath}`);
		notifyReason(undefined, missingBinary, vscode.window.showErrorMessage, startupLog);
	}
	// ドキュメント終了時の状態解放登録
	ctx.subscriptions.push(
		vscode.workspace.onDidCloseTextDocument(
			(doc: vscode.TextDocument): void => {
				// ドキュメント固有状態の解放
				const key: string = doc.uri.toString();
				notifiedReasons.delete(key);
				clearPendingDiagnostics(key);
				// 診断情報の破棄
				diagnostics.delete(doc.uri);
				// 終了
				return;
			}
		)
	);
	// 拡張機能終了時の残存待機解放
	ctx.subscriptions.push(
		{
			/**
			 * 残存する診断反映待機を全て解除する
			 * 計算量：保留する診断数 D に対し O(D)
			 */
			dispose(): void {
				// 全文書の残存待機解除
				for(const key of [...pendingDiagnostics.keys()]) clearPendingDiagnostics(key);
				notifiedReasons.clear();
				notifiedSettings.clear();
				notifiedSessionReasons.clear();
				// 終了
				return;
			}
		}
	);
	// 全対応言語で共有する整形プロバイダーの構築と登録
	const provider: vscode.DocumentFormattingEditProvider = {
		/**
		 * ドキュメント整形結果の編集列を提供する
		 * 計算量：入力のテキスト長 B，警告数 W，警告のテキスト長 L，子プロセスの費用 Q に対し O(B + W log(W + 1) + L + Q)
		 * @param doc 整形対象ドキュメント
		 * @param _settings エディターの整形設定
		 * @param token 取消の合図
		 * @return ドキュメントへ適用する編集列
		 */
		provideDocumentFormattingEdits(
			doc: vscode.TextDocument,
			_settings: vscode.FormattingOptions,
			token: vscode.CancellationToken
		): Promise<vscode.TextEdit[]> {
			const log: OutputLog = documentOutput(doc, channel);
			if(!fileSystem.existsSync(binaryPath)) {
				clearPendingDiagnostics(doc.uri.toString());
				log.appendLine(`shavefmt binary not found: ${binaryPath}`);
				notifyReason(undefined, missingBinary, vscode.window.showErrorMessage, log);
				// 実行出来ない場合の編集無の返戻
				return Promise.resolve([]);
			}
			// 整形の編集の返戻
			return format(ctx.extension.id, doc, binaryPath, token, log, diagnostics);
		}
	};
	ctx.subscriptions.push(
		vscode.languages.registerDocumentFormattingEditProvider(
			SUPPORTED_LANG_IDS.flatMap(
				(language: string): vscode.DocumentFilter[] =>
				["file", "untitled"].map((scheme: string): vscode.DocumentFilter => ({ language, scheme }))
			),
			provider
		)
	);
	// 終了
	return;
}
