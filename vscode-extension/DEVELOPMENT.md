# Shave Format VS Code 拡張機能（開発者向）

## ローカル開発

必要な道具と tree-sitter の実行時・CLI の準備は，[本体 README](../README.md#build-from-source) に従う．以下は最上位から Bash で実行する．Windows の実行ファイルには `.exe` を付ける．

```bash
(
	set -eu
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTREE_SITTER_ROOT="$PWD/.tree-sitter-runtime"
	cmake --build build --config Release --parallel
	npm ci --ignore-scripts --prefix vscode-extension
	cd vscode-extension
	target="$(node -p "process.platform + '-' + process.arch")"
	version="$(node -p "require('./package.json').version")"
	vsix="shavefmt-$target-$version.vsix"
	npx --no-install vsce package --target "$target" --out "$vsix"
	node ../tools/verifyExtension.cjs
	code --install-extension "$vsix" --force
)
```

CMake が `vscode-extension/bin/shavefmt-<platform>-<arch>[.exe]` へ本体を配置し，`vsce package` が `vscode:prepublish` で拡張機能を構築する．連携試験に成功した VSIX だけをインストールする．別環境向の構築では本体と `--target` を一致させる．

## 通知の文言

文書の変換・診断・通知は `src/extension.ts`，子プロセスの起動・受信・取消・後始末は `src/formatterProcess.ts` が担う．通知は英語の固定文面とし，Markdown のリンクと誤解釈され得る任意の出力・設定値は出力チャネルへ書く．見送理由は，英数字・空白・`-`・`.` だけの所定の文面に限って本文へ示す．

## 実行記録に基付く最適化 (PGO)

Clang / GCC では `PGO_MODE=generate` で記録を取り，`PGO_MODE=use` で記録を使う構築を行う．既定は `off` である．同じソース・処理系・対象環境・構築ディレクトリを使い，過去の記録を混ぜない．GCC の記録名は構築時の経路に依存する為，途中でディレクトリを変えない．効果は通常構築との比較で測定する．

前節の準備後，未使用の `build_pgo` で計測する．例の `src/` 以外も，用途を代表する言語・規模の対応ファイルを使う．書戻指定は付けず，失敗・見送を計測の成功に数えない．

```bash
(
	set -eu
	test ! -e build_pgo
	cmake -S . -B build_pgo -DCMAKE_BUILD_TYPE=Release -DTREE_SITTER_ROOT="$PWD/.tree-sitter-runtime" -DPGO_MODE=generate
	cmake --build build_pgo --config Release --parallel
	./build_pgo/shavefmt --fail-on-skip src/
)
```

Clang の場合だけ，使用した処理系に対応する `llvm-profdata` で記録を統合する．macOS では `xcrun llvm-profdata`，Linux では `llvm-profdata` を使う．GCC では此の工程を行わない．

```bash
(
	set -eu
	if [ "$(uname -s)" = Darwin ]; then
		xcrun llvm-profdata merge -output=build_pgo/pgo-data/default.profdata build_pgo/pgo-data/*.profraw
	else
		llvm-profdata merge -output=build_pgo/pgo-data/default.profdata build_pgo/pgo-data/*.profraw
	fi
)
```

同じディレクトリで最終の本体を構築する．配布には検査済の最終実行ファイルを使い，計測用の物を混ぜない．

```bash
(
	set -eu
	cmake -S . -B build_pgo -DCMAKE_BUILD_TYPE=Release -DTREE_SITTER_ROOT="$PWD/.tree-sitter-runtime" -DPGO_MODE=use -DPGO_DATA="$PWD/build_pgo/pgo-data"
	cmake --build build_pgo --config Release --parallel
)
```

## VS Code マーケットプレースへの配布

### 事前準備

[公式の公開手順](https://code.visualstudio.com/api/working-with-extensions/publishing-extension) に従い，Azure DevOps で全組織を対象とする `Marketplace → Manage` の権限を持つ PAT を作成し，発行元を登録する．`package.json` の `publisher` を発行元の ID と一致させ，次のログイン時に PAT を入力する．認証値を文書・コマンド履歴へ書かない．CI 用の PAT は正式リポジトリの秘密情報 `VSCE_PAT` へ登録し，仮リポジトリへ複製しない．

```bash
(
	set -eu
	cd vscode-extension
	npx --no-install vsce login "$(node -p "require('./package.json').publisher")"
)
```

### 環境別の配布物

対象は `darwin-arm64`, `darwin-x64`, `linux-arm64`, `linux-x64`, `win32-x64` とする．各環境で本体を構築し，対応する実行ファイル１個を `bin/` に含め，`vsce package --target` で環境を指定する．`engines.vscode` と対象環境に合う VSIX が配信される．

### CI に依る自動配布

[release.yml](../.github/workflows/release.yml) は `v*` のタグ送信時に５環境で構築・検査し，成功した環境の VSIX を保存する．`main` の更新又はプルリクエストだけでは起動しない．正式リポジトリだけで GitHub Release への添付とマーケットプレースへの公開を行い，環境の欠落は成功分の配布後に失敗として通知する．

`VSCE_PAT` が未設定なら公開を試みずに失敗とし，同じ環境の配布物が複数在る場合は配布前に停止する．公開済の同じ版・環境は `--skip-duplicate` で飛ばし，其の他の公開失敗も全件を試みた上で警告に留める．公開の成否は，工程の結果でなく公開工程の記録とマーケットプレースの掲載で確かめる．

公開前に `CMakeLists.txt` と `vscode-extension/package.json` の版を揃え，変更をコミットする．次の検査では終了コードだけでなく，整形差分・見送・失敗・警告が全て０件である事を確認する．警告だけでは整形器の終了コードは変わらない．PGO の配布物を使う場合は，本体の経路を最終構築の物へ合わせる．

```bash
(
	set -eu
	bash tools/golden_test.sh
	python3 tools/verify_golden_syntax.py
	./build/shavefmt --check --fail-on-skip src/ tools/ vscode-extension/src/
)
```

正式公開前の全環境検査は，仮の公開リポジトリで行う．`CHECK_REPO` に新しい仮リポジトリの `所有者/名前` を設定する．公開して良いコミット履歴・ソースだけを送り，秘密情報を含めない．

```bash
(
	set -eu
	: "${CHECK_REPO:?Set CHECK_REPO to the temporary owner/repository}"
	version="$(node -p "require('./vscode-extension/package.json').version")"
	git tag "v$version"
	gh repo create "$CHECK_REPO" --public
	git push "https://github.com/$CHECK_REPO.git" HEAD:refs/heads/main "refs/tags/v$version"
	gh run list --repo "$CHECK_REPO" --workflow release.yml --branch "v$version"
)
```

送信したタグのコミットに対応する実行 ID を `RUN_ID` に設定する．終了後は成否に関わらず仮リポジトリを非公開へ戻し，削除せず残す．途中で手動中断した場合も非公開化を確認する．全環境の検査成功を確認する迄，正式リポジトリへタグを送らない．

```bash
(
	set -eu
	: "${CHECK_REPO:?Set CHECK_REPO to the temporary owner/repository}"
	: "${RUN_ID:?Set RUN_ID to the matching workflow run}"
	result=0
	gh run watch "$RUN_ID" --exit-status --repo "$CHECK_REPO" || result=$?
	gh repo edit "$CHECK_REPO" --visibility private --accept-visibility-change-consequences
	exit "$result"
)
```

仮リポジトリの検査は公開の成功を保証しない．検査済のコミットと版を正式リポジトリへ送る．内容を変更する場合は版を上げ，同じ版の再実行は同じコミットの未公開環境の補完に限る．公開済のタグは強制的に付け替えない．

```bash
(
	set -eu
	version="$(node -p "require('./vscode-extension/package.json').version")"
	git push origin main "refs/tags/v$version"
)
```

### 手動の配布

ローカル開発で検査した１環境の VSIX を公開する．再構築せず，生成済の１個を明示する．

```bash
(
	set -eu
	cd vscode-extension
	target="$(node -p "process.platform + '-' + process.arch")"
	version="$(node -p "require('./package.json').version")"
	vsix="shavefmt-$target-$version.vsix"
	test -f "$vsix"
	npx --no-install vsce publish --packagePath "$vsix"
)
```
