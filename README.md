# Shave Format

[![Release](https://github.com/FugoShimizu/ShaveFormat/actions/workflows/release.yml/badge.svg)](https://github.com/FugoShimizu/ShaveFormat/actions/workflows/release.yml) [![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Shave Format is an opinionated formatter and linter with a native C++ command-line tool and a VS Code extension.
Line width is its only formatting preference.

Supported languages are C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python, JSON, HTML, and CSS, including JSX, TSX, JSONC, and SCSS.
See the [formatter specification](docs/formatter_spec.md#第２章-対象言語) for extensions, dialects, and language selection.

## Formatting behavior

The [coding standards](CODING_STANDARDS.md) define the style; the [formatter specification](docs/formatter_spec.md) defines the transformation and validation contract.

- **Syntax-driven layout:** normalize tabs, spacing, blank lines, parentheses, braces, and terminators without using the input's nonsemantic whitespace as a preference.
- **Consistent transformations:** merge compatible local declarations, split them when required by width, and sort supported named import specifiers without changing module evaluation order.
- **Comment handling:** normalize explanatory text and synchronize supported documentation with function signatures without inventing descriptions.
- **Idempotence:** formatting the result again with the same language and settings produces no further changes.

Line width defaults to 128 Unicode code points, excluding indentation, separately handled comments and their separators, and Unicode `Cc` control characters.
Source-content regions, including HTML comments and runtime documentation values, are counted.
Use `--chars 0` to disable width-based wrapping, not mandatory structural line breaks.
Python compound statements and nonempty declaration bodies retain mandatory expansion.
Indivisible tokens and protected content may exceed the limit.

Comments never determine code wrapping, indentation, or blank lines.
Ordinary comments keep their standalone, line-end, or mid-line kind where safe; comments with no valid placement may be removed.
Explanatory sentences use separate logical lines without sentence-final periods, including in English.
Documentation may absorb adjacent or signature comments under the specification's ownership rules; protected documentation and registered language or official-toolchain directives are not subject to ordinary-comment removal.

A transformation is applied only when its required semantic conditions are established.
Otherwise, the construct is retained and an applicable lint warning or review candidate is reported.
Formatting does not prove compliance with every coding rule.

## Installation

### VS Code

Download a platform-specific `.vsix` from [Releases](https://github.com/FugoShimizu/ShaveFormat/releases) and use **Extensions: Install from VSIX...** in VS Code.
Package targets are `darwin-x64`, `darwin-arm64`, `linux-x64`, `linux-arm64`, and `win32-x64`; check the release assets for availability.
The extension ID is `foogoo.shavefmt`, and execution requires a trusted workspace.

Enable format on save in your settings:

```json
{
	"editor.formatOnSave": true,
	"editor.defaultFormatter": "foogoo.shavefmt"
}
```

The `shavefmt.chars` setting controls line width and defaults to 128.
Its native binary is bundled; build dependencies are unnecessary for installation.

### Build from source

Requirements are Git, a C17 compiler and a C++20 compiler, CMake 3.20 or later, Node.js, and tree-sitter CLI 0.27.0 on `PATH`.
Use tree-sitter runtime 0.27.0 with language ABI 15 support, pinned below to the same commit as the [release workflow](.github/workflows/release.yml).
The workflow uses Node.js 22; its [CLI installer](.github/actions/tree-sitter-cli/action.yml) records the release assets and checksums.

The commands below use Bash and run from the repository root after cloning.
On Windows, use a Bash environment with the native compiler toolchain available; executable paths end in `.exe`.

```bash
git clone --recursive https://github.com/FugoShimizu/ShaveFormat.git
cd ShaveFormat
```

Build the pinned runtime into a project-local directory:

```bash
(
	set -eu
	runtime_source="$(mktemp -d "${TMPDIR:-/tmp}/shaveformat-tree-sitter.XXXXXX")"
	trap 'rm -rf -- "$runtime_source"' EXIT
	git -C "$runtime_source" init -q
	runtime_commit=6070dbfefd326bd735e5683eb128cc1b57dad0c0
	git -C "$runtime_source" fetch -q --depth 1 https://github.com/tree-sitter/tree-sitter.git "$runtime_commit"
	git -C "$runtime_source" checkout -q FETCH_HEAD
	configure_args=(
		-S "$runtime_source"
		-B "$runtime_source/build"
		-DCMAKE_BUILD_TYPE=Release
		-DBUILD_SHARED_LIBS=OFF
		-DCMAKE_INSTALL_LIBDIR=lib
		-DCMAKE_POLICY_DEFAULT_CMP0091=NEW
		-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
	)
	cmake "${configure_args[@]}"
	cmake --build "$runtime_source/build" --config Release --parallel
	cmake --install "$runtime_source/build" --config Release --prefix "$PWD/.tree-sitter-runtime"
)
```

Generate the Swift parser, then build the formatter; CMake generates the SCSS parser during the build:

```bash
(
	set -eu
	(cd vendor/tree-sitter-swift && tree-sitter generate)
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTREE_SITTER_ROOT="$PWD/.tree-sitter-runtime"
	cmake --build build --config Release --parallel
)
```

The executable is `build/shavefmt` (`build/shavefmt.exe` on Windows).
CMake also copies it to `vscode-extension/bin/shavefmt-<platform>-<arch>[.exe]`.

## Usage

Write formatted files, read standard input, or check a directory without writing:

```bash
./build/shavefmt --write src/
./build/shavefmt --stdin --lang cpp < src/Main.cpp
./build/shavefmt --check --fail-on-skip src/
```

The examples are independent; review changes before committing them.

### Options

| Option | Description |
|---|---|
| `-w`, `--write` | Write validated changes to files; without this option, files are not modified. |
| `-c`, `--check` | Check without writing; report required changes through the exit status. |
| `--diff` | Show formatting differences; combine with `--check` to fail on required changes. |
| `-l`, `--lang ID` | Select the language for standard input or an explicitly named file. |
| `-m`, `--chars N` | Set nonnegative line width; the default is 128, and 0 disables width-based wrapping. |
| `--stdin` | Read standard input and write the result to standard output; requires `--lang`. |
| `--no-lint` | Disable lint analysis, not formatting, output validation, or input/output diagnostics. |
| `--fail-on-skip` | Fail on skipped inputs and explicitly named paths through symbolic links. |
| `-h`, `--help` | Show command help. |
| `-v`, `--version` | Show the version. |
| `--` | Treat subsequent arguments as paths. |

Separate option values with a space: `--chars 100`, not `--chars=100`.
Language IDs and extension aliases are case-insensitive; a conflicting known extension is rejected.
`--stdin` cannot be combined with `--check`, `--write`, `--diff`, or file paths.

### File selection

Directories are scanned recursively, excluding dependency directories, build output, hidden directories, lockfiles, and minified JavaScript/CSS according to the [selection rules](docs/formatter_spec.md#102-経路の収集と安全な書戻).
Explicitly naming an otherwise excluded path bypasses its name exclusion, but not exclusions encountered below it.
Symbolic links are never followed, including links in an explicitly named path.

### Exit status and diagnostics

| Status | Meaning |
|---|---|
| 0 | Success, a permitted skip, or no supported input outside check mode. |
| 1 | Required changes or no supported input in check mode; a path, access, formatting, or resource failure; or a skip rejected by `--fail-on-skip`. |
| 2 | Invalid command-line usage, including missing values, conflicting options, or a language/extension mismatch. |

Diagnostics go to standard error: `[warn]` for lint findings and notices, `[skip]` for inputs left unformatted, and errors for failed operations or rejected candidates.
Warnings alone do not change the exit status, and status 0 does not certify that every input was fully formatted.
Warning coordinates refer to formatted text, which can differ from the file on disk under `--check`.

## Safety and limitations

Only validated candidates are returned as successful formatting results or written to files.
Skipped or failed formatting returns the original input bytes, including its BOM and original line endings.
Validation covers syntax, semantic distinctions, protected content, registered directives, documentation targets, and blank-line invariants; it is not a proof of arbitrary program equivalence.

Partial formatting requires identifiable error regions and trustworthy surrounding syntax and boundaries.
Those regions remain unchanged, and the rest must pass validation.
If their preservation cannot be verified, the candidate is rejected rather than reported as successful partial formatting.
Validated partial formatting emits a warning and is not rejected merely by `--fail-on-skip`.

Input and output are each limited to 16 MiB; parsing, nesting, and formatting work have additional [resource limits](docs/formatter_spec.md#第９章-資源管理と並列実行).
Oversized standard input is copied unchanged to standard output and reported as skipped.
Successful output uses UTF-8 without a BOM and LF for generated lines; protected literal, document, and data content keeps the bytes required by its preservation contract.
Concurrent file changes or file attributes that cannot be preserved prevent writing; replacing a hard-linked file reports the loss of shared identity with its other names.

For an unexpected result, include a minimal reproducible input, the command, version, diagnostics, and expected behavior in an [issue](https://github.com/FugoShimizu/ShaveFormat/issues), after removing sensitive information.

## Development

### Tests

Build the native targets first and run the commands below from the repository root.
Use Python and npm in addition to the build requirements; the release workflow uses Python 3.13 and Node.js 22.
Language-specific checks need the corresponding toolchains, and unavailable checks are not evidence of coverage.

Install extension dependencies and build the JavaScript required by its process-integration test:

```bash
(
	set -eu
	npm ci --ignore-scripts --prefix vscode-extension
	npm run build --prefix vscode-extension
)
```

Run the main checks, stopping at the first failed command:

```bash
(
	set -eu
	ctest --test-dir build -C Release --output-on-failure --no-tests=error
	bash tools/golden_test.sh
	python3 tools/verify_golden_syntax.py
	python3 tools/verify_runtime.py ./build/shavefmt
	node tools/verifyExtension.cjs ./build/shavefmt
	python3 tools/verify_semantic_preservation.py
	bash tools/verify_local_corpus.sh
	python3 tools/verify_space_random_ast.py
)
```

On Windows, use `./build/shavefmt.exe` and ensure `python3` resolves to the intended interpreter.
CTest runs the CMake-registered checks; `golden_test.sh` runs expected-output regressions and its associated invariant checks.
The [acceptance criteria](docs/formatter_spec.md#111-不変条件の検査) define what each check must establish; passing an available subset does not establish complete coverage.

### Extension packaging

After building the native binary and installing extension dependencies, select a VSIX target matching that binary:

```bash
(
	set -eu
	cd vscode-extension
	npx --no-install vsce package --target "<platform>"
)
```

Replace `<platform>` with a target listed under [VS Code](#vs-code).
The package command runs the configured prepublish build; packaging for another target requires its matching binary first.
See [extension development](vscode-extension/DEVELOPMENT.md) for further details.

## Documentation

- [Coding standards](CODING_STANDARDS.md): shared coding and documentation rules, including language-specific conditions.
- [Formatter specification](docs/formatter_spec.md): transformations, responsibility boundaries, validation, and external contracts.
- [Line-breaking specification](docs/line_breaking_spec.md): break groups, evaluation order, and width decisions.
- [Token-spacing specification](docs/token_spacing_spec.md): spacing determined by syntactic role.

## License

[MIT](LICENSE) © 2026 Fugo Shimizu
