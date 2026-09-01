# Shave Format VS Code 拡張 - 開発者向け

## ローカル開発

```sh
# 0. Swift の文法を生成（初回のみ．省くと parser.c が無くリンクに失敗）
# tree-sitter CLI 0.26.8 は CI と同じ配布物を SHA-256 で照合して PATH へ置く
(cd vendor/tree-sitter-swift && tree-sitter generate)

# 1. ネイティブバイナリをビルド（最上位で実行．必要物は README.md の Build を参照）
# POST_BUILD が vscode-extension/bin/shavefmt-<platform>-<arch>[.exe] へ配置
cmake -S . -B build \
	-DCMAKE_BUILD_TYPE=Release \
	-DTREE_SITTER_ROOT="$PWD/.tree-sitter-runtime";
cmake --build build --config Release --parallel;

# 2. 拡張機能の依存導入・ビルド・検査（package-lock.json の版を使用）
cd vscode-extension;
npm ci --ignore-scripts;
npm run build;
node ../tools/verifyExtension.cjs;

# 3. ローカル確認用の vsix を生成・インストール
npx --no-install vsce package;
code --install-extension shavefmt-*.vsix;
```

`process.platform` / `process.arch` で対応するバイナリが自動選択される（例：`bin/shavefmt-darwin-arm64`，`bin/shavefmt-linux-x64`，`bin/shavefmt-win32-x64.exe`）．未配置の場合は整形せず，探した経路を出力チャネルへ書いて通知する．

## 通知の文言

`src/formatterProcess.ts` は子プロセスの起動・受信・取消・後始末を担い，`src/extension.ts` は文書の変換・診断・通知を担う．

通知は英語で記述する．通知の本文は Markdown のリンクを描画する為，整形器の出力や設定の値を埋めず（見送の理由は英数字・空白・`-`・`.` だけの決まった文の時に限り示す），詳細は出力チャネルへ書く．

## Profile-Guided Optimization (PGO)

clang / gcc では，LTO に加えて実行記録を使う２段階のビルドを選べる．

```sh
# 1) 実行の記録を取る計測用のビルド
cmake -S . -B build_pgo \
	-DCMAKE_BUILD_TYPE=Release \
	-DTREE_SITTER_ROOT="$PWD/.tree-sitter-runtime" \
	-DPGO_MODE=generate;
cmake --build build_pgo --config Release --parallel;

# 2) 代表的な入力で実行して記録を集める（書き戻さない試行でも整形の全工程が走る為，-w で src/ と子モジュールを書き換えない）
./build_pgo/shavefmt src/;
./build_pgo/shavefmt vendor/tree-sitter-typescript/test/; # 大きな入力を推奨

# 3) 記録の統合（Clang は統合した default.profdata を読み，Linux は llvm-profdata を直接呼び，GCC は不要）
xcrun llvm-profdata merge -output=build_pgo/pgo-data/default.profdata build_pgo/pgo-data/*.profraw;

# 4) 記録を使う最終のビルド
cmake -S . -B build \
	-DCMAKE_BUILD_TYPE=Release \
	-DTREE_SITTER_ROOT="$PWD/.tree-sitter-runtime" \
	-DPGO_MODE=use \
	-DPGO_DATA="$PWD/build_pgo/pgo-data";
cmake --build build --config Release --parallel;
```

`PGO_MODE=off`（既定）は通常ビルド．記録には本番に近い大きなファイル群を使う．

## VSCode マーケットプレースへの配布

### 事前準備（１回のみ）

1. **Azure DevOps で Personal Access Token (PAT) を作成**
	- <https://dev.azure.com/> にサインイン
	- User Settings → Personal Access Tokens → New Token
	- Organization: **All accessible organizations**
	- Scopes: **Marketplace → Manage**
2. **発行元（Publisher）を登録**
	- <https://marketplace.visualstudio.com/manage> で発行元の ID を取得
	- `package.json` の `"publisher"` を登録した ID に合わせる
3. **vsce へログイン**
	```sh
	npx --no-install vsce login <publisher-id>;
	# 上記の PAT を貼り付ける
	```

### マーケットプレース配布

配布は **環境別の .vsix** で行う．各 OS・アーキテクチャの環境でネイティブバイナリをビルドし，対応する１個だけを `vscode-extension/bin/` に置いてから `vsce package --target <環境>` で包む．Marketplace は `engines.vscode` と環境の組合せで対応する .vsix を配信する．

対象の環境：`darwin-x64`，`darwin-arm64`，`linux-x64`，`linux-arm64`，`win32-x64`

### CI に依る自動配布（推奨）

GitHub Actions の [release.yml](../.github/workflows/release.yml) は **`v*` のタグ送信時だけ** 次を行う（main と PR では実行しない）：
1. ５環境の各々でビルドし，配布する実行ファイルに整形の回帰（golden・意味保存・実行時・配置の独立・書込の衝突）と CLI の経路の検査を掛け，通った環境の `.vsix` を成果物として保存（tree-sitter の実行時は全環境で開発環境と同じ v0.26.8 に固定して源から構築する）
2. 正式リポジトリだけで **GitHub Release への添付 → マーケットプレースへの公開**（`VSCE_PAT` が未設定，又は重複以外の公開に失敗した場合は失敗とする）

リリース前に使い捨ての非公開リポジトリで全環境を検査する．`<版>` は `package.json` の版とする．
```sh
# 1. 手元で golden と処理系の構文検査を通し，自身のソースが整形器の不動点で警告を出さない事も確かめ，版の札を付ける（同じ版で出し直す時は既存の札を `git tag -f v<版>` で付け替える）
bash tools/golden_test.sh;
python3 tools/verify_golden_syntax.py;
./build/shavefmt --check --fail-on-skip src/ tools/ vscode-extension/src/; # 整形の要否と警告が共に０件で有る事（CI でも確かめる）
git tag v<版>;
# 2. 使い捨ての非公開リポジトリへ送り，全環境の構築・検査を確かめてから消す（公開ジョブは正式リポジトリ以外で実行されない）
gh repo create <所有者>/shaveformat-ci-check --private;
git push https://github.com/<所有者>/shaveformat-ci-check.git main v<版>;
gh run list --repo <所有者>/shaveformat-ci-check;
gh auth refresh -h github.com -s delete_repo; # リポジトリの削除に要る権限を gh の認証へ加える（初回だけ）
gh repo delete <所有者>/shaveformat-ci-check --yes;
# 3. 本番のリポジトリへ送る（札を付け替えた時は `git push origin main` の後に `git push --force origin v<版>` で送る）
# 同じ版で出し直すと，Release の同名配布物を差し替え，マーケットプレースの公開済対象を飛ばして未公開対象だけを送る
git push origin main v<版>;
# → GitHub Actions が全環境の .vsix をビルドして公開する
```

リポジトリの秘密情報（Actions の secrets）に `VSCE_PAT`（Azure DevOps の Personal Access Token，スコープ：`Marketplace → Manage`）を設定する必要が有る．

### 手動の配布（１環境だけ試す時）

```sh
cmake --build build --config Release --parallel; # 自分の環境のバイナリを bin/ に置く
cd vscode-extension;
npx --no-install vsce package --target darwin-arm64; # 自分の環境を指定する
npx --no-install vsce publish --packagePath shavefmt-darwin-arm64-0.1.0.vsix;
```
