# Shave Format

[![Release](https://github.com/FugoShimizu/ShaveFormat/actions/workflows/release.yml/badge.svg)](https://github.com/FugoShimizu/ShaveFormat/actions/workflows/release.yml) [![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Shave Format is an opinionated formatter and linter with a native C++ command-line tool and a VS Code extension. Line width is its only formatting preference.

Supported languages are C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python, JSON, HTML, and CSS, including JSX, TSX, JSONC, and SCSS.

## Formatting

Shave Format normalizes layout, performs supported syntax transformations, and synchronizes documentation. It is not limited to whitespace changes: compatible declarations can be merged or split, and eligible returns made explicit. Transformations require established semantic conditions. The linter reports detected violations and review candidates.

Layout follows syntax rather than the input's nonsemantic whitespace. Comments do not determine code wrapping or blank lines. Line width defaults to 128 Unicode code points, excluding indentation and separately handled comments. Use `--chars 0` to disable width-based wrapping; required structural line breaks remain. Protected content and indivisible tokens may exceed the limit. Reformatting with the same settings must not change the result.

## Installation

### VS Code

Download the matching `.vsix` from [Releases](https://github.com/FugoShimizu/ShaveFormat/releases) and run **Extensions: Install from VSIX...** in VS Code. Targets are `darwin-x64`, `darwin-arm64`, `linux-x64`, `linux-arm64`, and `win32-x64`; check the release assets for availability. The extension bundles its native binary and requires a trusted workspace.

To format on save, add this to `settings.json`:

```json
{ "editor.formatOnSave": true, "editor.defaultFormatter": "foogoo.shavefmt" }
```

The `shavefmt.chars` setting controls line width and defaults to `128`; `0` disables width-based wrapping.

### Build from source

Requirements are Git, a C17 compiler and a C++20 compiler, CMake 3.20 or later, Node.js, and tree-sitter CLI 0.27.0 on `PATH`. Use tree-sitter runtime 0.27.0 with language ABI 15 support, pinned below to the same commit as the [release workflow](.github/workflows/release.yml). The workflow uses Node.js 22; its [CLI installer](.github/actions/tree-sitter-cli/action.yml) records the release assets and checksums.

The commands below use Bash and run from the repository root after cloning. On Windows, use a Bash environment with the native compiler toolchain available; executable paths end in `.exe`.

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

Build the formatter; CMake generates the extended HTML, SCSS, and Swift parsers during the build:

```bash
(
	set -eu
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTREE_SITTER_ROOT="$PWD/.tree-sitter-runtime"
	cmake --build build --config Release --parallel
)
```

The executable is `build/shavefmt` (`build/shavefmt.exe` on Windows). CMake also copies it to `vscode-extension/bin/shavefmt-<platform>-<arch>[.exe]`.

## Usage

From the repository root after building, write changes, format standard input, or check files without writing:

```bash
./build/shavefmt --write src/
./build/shavefmt --stdin --lang cpp < src/Main.cpp
./build/shavefmt --check --fail-on-skip src/
```

These are independent examples. Review changes before committing them.

### Options

| Option | Description |
|---|---|
| `-w`, `--write` | Write validated changes; otherwise files are not modified. |
| `-c`, `--check` | Check without writing and fail when changes are required. |
| `--diff` | Show differences; combine with `--check` to fail on required changes. |
| `-l`, `--lang ID` | Select the language for standard input or an explicitly named file. |
| `-m`, `--chars N` | Set nonnegative line width; default `128`, or `0` to disable width-based wrapping. |
| `--stdin` | Read standard input and write the result to standard output; requires `--lang`. |
| `--no-lint` | Disable lint analysis, not formatting, validation, or input/output diagnostics. |
| `--fail-on-skip` | Fail on skipped inputs and explicitly named paths through symbolic links. |
| `-h`, `--help` | Show command help. |
| `-v`, `--version` | Show the version. |
| `--` | Treat subsequent arguments as paths. |

Separate values with a space: `--chars 100`, not `--chars=100`. Language IDs and extension aliases are case-insensitive; a conflicting known extension is rejected. `--check` and `--write` are mutually exclusive. `--stdin` cannot be combined with `--check`, `--write`, `--diff`, or file paths.

Directory scans exclude dependency, build, and hidden directories, lockfiles, and minified JavaScript/CSS. Explicit paths bypass their own name exclusion, not exclusions below them. Symbolic links are never followed. For directories, `--lang` is ignored with a notice; each file uses its extension.

## Diagnostics and limitations

Diagnostics go to standard error: `[warn]` reports lint, partial-formatting, or unformatted-embedded-code warnings; `[notice]` provides notices and guidance; `[skip]` identifies unformatted inputs. Warnings alone do not fail the command. Warning positions on successful output refer to formatted text, which can differ from the file on disk under `--check`.

| Exit status | Meaning |
|---|---|
| `0` | Success, a permitted skip, or no supported input outside check mode. |
| `1` | Required changes or no supported input in check mode; an operation failure; or a skip rejected by `--fail-on-skip`. |
| `2` | Invalid command-line usage. |

Skipped or failed formatting retains the original input bytes. Only validated results are returned as successful formatting or written to files; validation is not a proof of arbitrary program equivalence or compliance with every coding rule. Partial formatting preserves unread regions, unsupported embedded content, or child code skipped for a preservable input reason. It validates the surrounding syntax and preservation boundaries and emits the corresponding warning; `--fail-on-skip` does not reject validated partial formatting.

`Error: formatting would change code structure, left unchanged: <path>` means the generated candidate failed structural validation, not that the input violates the coding standards. The original file is not rewritten. For an unexpected result, report the version, command, diagnostics, expected behavior, and a minimal input through [Issues](https://github.com/FugoShimizu/ShaveFormat/issues), after removing sensitive information.

Input and output are each limited to 16 MiB. Memory estimates, parsing time, nesting, and formatting work have additional limits. Successful output is UTF-8 without a BOM, using LF for generated lines; protected content retains its required bytes.

## Development

### Tests

Build the native targets first and run the commands below from the repository root. Use Python and npm in addition to the build requirements; the release workflow uses Python 3.13 and Node.js 22. Language-specific checks need the corresponding toolchains, and unavailable checks are not evidence of coverage.

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
	python3 tools/verify_node_type_names.py
	python3 tools/verify_runtime.py ./build/shavefmt
	node tools/verifyExtension.cjs ./build/shavefmt
	python3 tools/verify_semantic_preservation.py
	bash tools/verify_local_corpus.sh
	python3 tools/verify_space_random_ast.py
	MAX_CHARS=0 python3 tools/verify_space_random_ast.py
)
```

On Windows, use `./build/shavefmt.exe` and ensure `python3` resolves to the intended interpreter. The [acceptance criteria](docs/formatter_spec.md#111-不変条件の検査) define what each check must establish; passing an available subset does not establish complete coverage.

### Extension packaging

After building the native binary and installing extension dependencies, select a VSIX target matching that binary:

```bash
(
	set -eu
	cd vscode-extension
	npx --no-install vsce package --target "<platform>"
)
```

Replace `<platform>` with `darwin-x64`, `darwin-arm64`, `linux-x64`, `linux-arm64`, or `win32-x64`. The package command runs the configured prepublish build; packaging for another target requires its matching binary first. See [extension development](vscode-extension/DEVELOPMENT.md) for further details.

## Documentation

- [Coding standards](CODING_STANDARDS.md): formatting and coding rules.
- [Formatter specification](docs/formatter_spec.md): transformations, preservation, diagnostics, and resource limits.
- [Line-breaking specification](docs/line_breaking_spec.md): wrapping rules and examples.
- [Token-spacing specification](docs/token_spacing_spec.md): spacing by syntactic role.

## License

[MIT](LICENSE) © 2026 Fugo Shimizu
