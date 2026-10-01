# Shave Format

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](https://github.com/FugoShimizu/ShaveFormat/blob/main/LICENSE)

Shave Format is an opinionated formatter and linter for VS Code.
It bundles a native formatter with one formatting preference: line width.

## Installation

Install **Shave Format** from the Extensions view, or enter this in Quick Open:

```text
ext install foogoo.shavefmt
```

The extension runs in trusted workspaces and supports local and editable virtual documents.
Bundled binaries target macOS x64/arm64, Linux x64/arm64, and Windows x64.

## Quick start

Run **Format Document** from the Command Palette, or use the default shortcut: `Shift+Option+F` on macOS, `Shift+Alt+F` on Windows, or `Ctrl+Shift+I` on Linux.

Set the default formatter and enable format on save in `settings.json`:

```json
{ "editor.formatOnSave": true, "editor.defaultFormatter": "foogoo.shavefmt" }
```

Place these editor settings inside a language block to apply them only to that language.

## Supported languages

C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript (JSX), TypeScript (TSX), Ruby, Python, JSON (JSONC), HTML, and CSS (SCSS).

## Settings

| Setting | Default | Description |
|---|---:|---|
| `shavefmt.chars` | `128` | Nonnegative line width; `0` disables width-based wrapping, not mandatory line breaks. |

Line width measures code length rather than visual display width; indentation and separately handled comments do not count toward the limit.
Long strings and other indivisible content may exceed it.

## Formatting

Code spacing and line breaks follow a fixed style while preserving whitespace that affects meaning.
JSONC trailing commas are removed.

## Diagnostics and limits

Warnings appear in **Problems** at positions in the formatted document after edits are applied.
For skip reasons and errors, open **View → Output** and select **Shave Format**.
Diagnostics do not certify compliance with every coding rule.

When only part of a document can be formatted safely, that part may be formatted while unreadable regions remain unchanged; a warning reports partial formatting.
Failed, skipped, cancelled, or outdated results are not applied.
Successful formatting uses LF for generated lines while preserving protected content.

Each UTF-8 input is limited to 16 MiB, and each formatter process has a 30-second timeout.
Requests that time out or exceed input or waiting limits are not applied.

## Help

If the output channel reports a missing binary, reinstall the package for the operating system and architecture running the extension.
Report unexpected behavior with the version, diagnostics, and a minimal reproducible example in an [issue](https://github.com/FugoShimizu/ShaveFormat/issues), after removing sensitive information.
