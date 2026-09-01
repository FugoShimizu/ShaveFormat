# Shave Format

[![Release](https://github.com/FugoShimizu/ShaveFormat/actions/workflows/release.yml/badge.svg)](https://github.com/FugoShimizu/ShaveFormat/actions/workflows/release.yml) [![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Shave Format is an opinionated formatter and linter for C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python, JSON, HTML, CSS, and their supported variants. It provides one style across all languages with one setting: the maximum line width. Supported variants include JSX, TSX, JSONC, and SCSS.

## Principles

- **Deterministic output:** layout is derived from syntax, independent of the input's indentation, line breaks, and comment positions. HTML comments remain document nodes because moving them can change the DOM.
- **Idempotence:** formatting an already formatted file produces no further changes.
- **Meaning preservation:** a syntax guard restores the original bytes when its checks detect structural damage or a protected language-binding change.
- **Conservative failure:** input that cannot be handled safely is left unchanged and reported instead of being partly guessed.
- **Unified tooling:** one native C++ binary formats supported files, reads standard input, processes files in parallel, and reports coding-standard violations.

The detailed pipeline and language-specific behavior are documented in [`doc/processing_spec.md`](doc/processing_spec.md).

## Formatting

- Normalize indentation, blank lines, and token spacing.
- Add, remove, or expand control-structure braces when syntax and scope are preserved.
- Merge or split declarations and sort supported named imports.
- Remove redundant parentheses and normalize operators, separators, and terminators.
- Wrap long lines in the order defined by [`doc/line_split_order.md`](doc/line_split_order.md).
- Preserve comments while normalizing their placement and text; reconcile supported function documentation.
- Apply safe language-specific canonical forms, including JSONC trailing-comma removal and required trailing separators.

Rules that require type, binding, macro, or intent information are reported as lint warnings instead of being rewritten.

## Build

Requirements:

- a C++20 compiler with C17 support
- CMake 3.20 or later
- Node.js
- tree-sitter runtime v0.26.8 with language ABI 15
- tree-sitter CLI v0.26.8 for Swift and SCSS parser generation

Clone the repository with its grammar submodules:

```bash
git clone --recursive https://github.com/FugoShimizu/ShaveFormat.git
cd ShaveFormat
```

Build and install the pinned tree-sitter runtime. Package managers often provide an incompatible version.

```bash
tree_sitter_src="$(mktemp -d "${TMPDIR:-/tmp}/shaveformat-tree-sitter.XXXXXX")"
git -C "$tree_sitter_src" init -q
git -C "$tree_sitter_src" fetch -q --depth 1 \
	https://github.com/tree-sitter/tree-sitter.git \
	cd5b087cd9f45ca6d93ab1954f6b7c8534f324d2
git -C "$tree_sitter_src" checkout -q FETCH_HEAD
cmake -S "$tree_sitter_src" \
	-B "$tree_sitter_src/build" \
	-DCMAKE_BUILD_TYPE=Release \
	-DBUILD_SHARED_LIBS=OFF \
	-DCMAKE_INSTALL_LIBDIR=lib \
	-DCMAKE_POLICY_DEFAULT_CMP0091=NEW \
	-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
cmake --build "$tree_sitter_src/build" --config Release
cmake --install "$tree_sitter_src/build" \
	--config Release \
	--prefix "$PWD/.tree-sitter-runtime"
rm -rf -- "$tree_sitter_src"
```

Place tree-sitter CLI v0.26.8 on `PATH`, generate the Swift parser, and build Shave Format:

```bash
(cd vendor/tree-sitter-swift && tree-sitter generate)
cmake -S . -B build \
	-DCMAKE_BUILD_TYPE=Release \
	-DTREE_SITTER_ROOT="$PWD/.tree-sitter-runtime"
cmake --build build --config Release --parallel
```

CI downloads the CLI from the [tree-sitter v0.26.8 release](https://github.com/tree-sitter/tree-sitter/releases/tag/v0.26.8) and verifies the checksums pinned in [`.github/actions/tree-sitter-cli/action.yml`](.github/actions/tree-sitter-cli/action.yml). The formatter is written to `build/shavefmt` and copied to `vscode-extension/bin/shavefmt-<platform>-<arch>[.exe]`.

## Usage

```bash
# Format files in place. The default line width is 128 characters.
./build/shavefmt --write src/main.cpp src/lib.cpp

# Read standard input and write formatted text to standard output.
cat src/main.cpp | ./build/shavefmt --stdin --lang cpp

# Exit with status 1 when formatting is required.
./build/shavefmt --check src/

# Treat skipped inputs as failures in continuous integration.
./build/shavefmt --check --fail-on-skip src/

# Print a positional line diff.
./build/shavefmt --diff src/main.cpp

# Set the line width. Zero disables the limit.
./build/shavefmt --write --chars 100 src/
```

### Options

| Flag | Description |
|---|---|
| `-w`, `--write` | Write changes to files. The default mode is a dry run. |
| `-c`, `--check` | Check only; exit with status 1 when changes are required. |
| `--diff` | Print a positional line diff. Add `--check` when differences must fail the command. |
| `-l`, `--lang ID` | Set the language for standard input or an explicitly named file. A conflicting known extension is rejected. |
| `-m`, `--chars N` | Set the maximum line width. The default is 128; 0 means unlimited. CSS, SCSS, and HTML do not use this limit. |
| `--stdin` | Read standard input and write standard output. Requires `--lang`. |
| `--no-lint` | Run formatting without lint rules. File-integrity notices are still reported. |
| `--fail-on-skip` | Count skipped inputs and explicitly named paths through symbolic links as failures. |
| `-h`, `--help` | Show command help. |
| `-v`, `--version` | Print the version. |
| `--` | Treat every following argument as a path. |

Exit status 0 means success. Status 1 means that `--check` found required changes, an operation failed, or `--fail-on-skip` rejected a skipped input. Status 2 means invalid command-line usage. Run `shavefmt --help` for the complete descriptions.

Directory scans ignore common dependency and build directories, lockfiles, and minified JavaScript or CSS. Explicitly named regular files are still processed. Symbolic links are never followed. Input is limited to 16 MiB, and output growth, parser time, nesting, and formatting work are bounded. Use `--fail-on-skip` when rejected inputs and explicitly named symbolic-link paths must fail.

## Safety and diagnostics

Shave Format keeps the original bytes when an input guard or the syntax guard rejects a change. It reports the reason on standard error:

- `[skip] <path>: <reason>` means the input was not formatted.
- `Error: formatting would break syntax, left unchanged: <path>` means a formatter invariant failed.
- `[warn]` reports preserved file properties and write-side effects such as a UTF-8 BOM, CRLF line endings, partial formatting, or a hard link broken by atomic replacement.

Successful file writes preserve a leading UTF-8 BOM and consistent CRLF line endings. Literal contents, heredocs, PHP text outside code tags, and other language-defined verbatim regions keep their bytes. See the [processing specification](doc/processing_spec.md) for exact guards, limits, and exceptions.

## VS Code extension

The extension runs in trusted workspaces. Download the platform-specific `.vsix` from [Releases](https://github.com/FugoShimizu/ShaveFormat/releases), or build it locally:

```bash
cd vscode-extension
npm ci --ignore-scripts
npm run build
npx --no-install vsce package --target <platform>
code --install-extension shavefmt-<platform>-<version>.vsix --force
```

Supported targets are `darwin-x64`, `darwin-arm64`, `win32-x64`, `linux-x64`, and `linux-arm64`. The marketplace extension ID is `foogoo.shavefmt`. The package target must match the host binary produced by CMake unless the matching target binary is placed in `vscode-extension/bin` first.

Enable format on save in VS Code:

```jsonc
{
	"editor.formatOnSave": true,
	"editor.defaultFormatter": "foogoo.shavefmt"
}
```

The extension setting `shavefmt.chars` controls the maximum line width and defaults to 128.

## Tests

```bash
# Golden outputs and expected warnings.
./tools/golden_test.sh

# Syntax validation with installed language toolchains.
python3 ./tools/verify_golden_syntax.py

# CMake-registered unit and integration tests.
ctest --test-dir build -C Release --output-on-failure --no-tests=error

# Runtime behavior and byte preservation.
npm ci --ignore-scripts --prefix vscode-extension
python3 ./tools/verify_runtime.py ./build/shavefmt

# VS Code extension build and process integration.
npm run build --prefix vscode-extension
node ./tools/verifyExtension.cjs ./build/shavefmt

# Semantic-preservation checks.
python3 ./tools/verify_semantic_preservation.py
python3 ./tools/verify_semantic_preservation.py --self-test
python3 ./tools/verify_semantic_preservation.py --corpus

# Local grammar corpora and randomized whitespace checks.
./tools/verify_local_corpus.sh
python3 ./tools/verify_space_random_ast.py
```

## Documentation

- [`CODING_STANDARDS.md`](CODING_STANDARDS.md) defines the language-independent coding rules.
- [`doc/processing_spec.md`](doc/processing_spec.md) specifies the formatter pipeline, safety guards, and pass responsibilities.
- [`doc/line_split_order.md`](doc/line_split_order.md) specifies line-split ordering and width decisions.
- [`doc/unnamed_token_rules.md`](doc/unnamed_token_rules.md) specifies per-language spacing around unnamed syntax tokens.
- [`vscode-extension/DEVELOPMENT.md`](vscode-extension/DEVELOPMENT.md) describes extension development and packaging.

## License

[MIT](LICENSE) © 2026 Fugo Shimizu
