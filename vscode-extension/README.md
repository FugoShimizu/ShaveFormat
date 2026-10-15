# Shave Format

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](https://github.com/FugoShimizu/ShaveFormat/blob/main/LICENSE)

Shave Format is an opinionated formatter and linter for VS Code. It bundles a native formatter with one formatting preference: line width.

## Installation

Install **Shave Format** from the Extensions view, or enter this in Quick Open:

```text
ext install foogoo.shavefmt
```

The extension runs in trusted workspaces and supports `file` and `untitled` documents. Bundled binaries target macOS x64/arm64, Linux x64/arm64, and Windows x64. Windows ARM64 uses the same x64 binary through x64 emulation (Windows 11 on Arm).

## Quick start

Run **Format Document** from the Command Palette, or use the default shortcut: `Shift+Option+F` on macOS, `Shift+Alt+F` on Windows, or `Ctrl+Shift+I` on Linux.

Set the default formatter and enable format on save in `settings.json`:

```json
{ "editor.formatOnSave": true, "editor.defaultFormatter": "foogoo.shavefmt" }
```

Only whole documents are formatted; set `editor.formatOnSaveMode` to `file`.

Place these editor settings inside a language block to apply them only to that language.

## Supported languages

C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript (JSX), TypeScript (TSX), Ruby, Python, JSON (JSONC), HTML, and CSS (SCSS).

## Settings

| Setting | Default | Description |
|---|---:|---|
| `shavefmt.chars` | `128` | Non-negative line width; `0` disables width-based wrapping, not mandatory line breaks. |
| `shavefmt.notifyLineEndingChanges` | `true` | Notify when successful formatting changes CRLF to LF. |

`shavefmt.chars` can also be set in a language-specific settings block.

Line width measures code length rather than visual display width; indentation and separately handled comments do not count toward the limit. Long strings and other indivisible content may exceed it.

## Formatting

Formatting is not limited to whitespace: it can also rewrite eligible syntax, merge or split declarations, sort supported named imports, and synchronize documentation with function signatures. Transformations require their semantic conditions to be established. Review the resulting changes before committing them.

## Diagnostics and limits

Review and skip diagnostics use Information severity; violations and document-specific formatting failures use Warning severity, except parser initialization failures (Error). Installation failures are Error notifications; temporary startup resource shortages are Warning notifications. When too many formatting requests are queued, or startup fails, details appear only in notifications and Output.

Warnings appear in **Problems** at positions in the formatted document after edits are applied. Open **View → Output** and select **Shave Format** for notices, skip reasons, and errors. Positioned notices appear as related information in Problems, or as Information when no matching diagnostic exists. A document-wide skip and its following remedy share the first-line range. Diagnostics do not certify compliance with every coding rule.

Partial formatting leaves unreadable regions unchanged and reports a warning. Failed, skipped, cancelled, or outdated results are not applied. Successful formatting uses LF for generated lines while preserving protected content.

CRLF documents switch to LF on successful formatting; output that mixes LF with preserved CR inside literals is not applied.

Each UTF-8 input is limited to 16 MiB and to 512 MiB of estimated working memory; the estimate does not guarantee resident memory usage. Each parse has the native formatter's time limit. One request runs at a time; formatting requests are queued in order within a 16 MiB total input and memory estimate. If too many formatting requests are queued, they are not applied; format again after current requests finish. Requests that exceed parsing, input, or working-memory limits are also not applied.

## Help

**Formatting was not applied: output would change code structure.** means output validation rejected the candidate. The document is left unchanged; this does not diagnose an input style violation. Failures appear as a first-line diagnostic in **Problems**; skips say **Formatting was skipped: &lt;reason&gt;.** Problems shows the cause, with remedies in related information; notifications include both.

Use **Show Output** for details. **Report Issue** opens a report for Shave Format with only the fixed reason as its title; it does not insert source text. Review the report before sending it. Installation errors name the required Shave Format package (for example, `darwin-arm64` or `win32-arm64`); install that package again. For estimated memory limits, format smaller independently valid inputs; for resource shortages, release memory or execution resources and retry. Output validation failures, formatter defects, parser initialization failures, crashes, and output-limit violations offer **Report Issue**. Document-specific notifications include a safely displayed workspace-relative path, or the file name outside the workspace; report titles contain only the cause.

**Don't Show Again** is available only on the CRLF-to-LF information notice. Set `shavefmt.notifyLineEndingChanges` back to `true` to restore it; warnings about formatting that was not applied remain visible.
