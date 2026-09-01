# Shave Format

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](https://github.com/FugoShimizu/ShaveFormat/blob/main/LICENSE)

Shave Format is an opinionated formatter and linter for C / C++ / C# / Java / Go / Rust / Kotlin / Swift / PHP / JavaScript (JSX) / TypeScript (TSX) / Ruby / Python / JSON (JSONC) / HTML / CSS (SCSS). It bundles the formatter binary, exposes one formatting option, and reports coding-standard issues in the Problems panel.

## Installation

Install **ShaveFormat** from the VS Code Extensions view, or enter this in Quick Open:

```text
ext install foogoo.shavefmt
```

The extension runs in trusted workspaces and supports local and virtual documents. Bundled binaries support macOS x64/arm64, Windows x64, and Linux x64/arm64.

## Quick start

Run **Format Document** from the Command Palette or press `Shift+Option+F` on macOS, `Shift+Alt+F` on Windows, or `Ctrl+Shift+I` on Linux.

To use Shave Format on save, add this to `settings.json`:

```jsonc
{
	"editor.defaultFormatter": "foogoo.shavefmt",
	"editor.formatOnSave": true
}
```

VS Code also accepts the same settings inside a language block when another formatter is the global default.

## Supported languages

C / C++ / C# / Java / Go / Rust / Kotlin / Swift / PHP / JavaScript (JSX) / TypeScript (TSX) / Ruby / Python / JSON (JSONC) / HTML / CSS (SCSS).

## Settings

| Setting | Default | Description |
|---|---:|---|
| `shavefmt.chars` | `128` | Maximum characters per line. Use `0` for unlimited width. CSS, SCSS, and HTML ignore this limit. |

## Diagnostics and skipped files

Formatting warnings appear in the Problems panel. More detail is available under **View → Output → Shave Format**.

The document remains unchanged when formatting fails, exceeds the 30-second limit, is rejected by the parser, or exceeds 16 MiB. Two processes run at once; requests beyond eight queued documents or 32 MiB of queued input are skipped. Reinstall the extension if the output channel reports that the formatter binary for the current platform is missing.

JSONC trailing commas are removed. Documents rejected by the parser remain unchanged; tolerated parser gaps can be partially formatted and produce a warning. A comment inside a compact object can move to the preceding line.
