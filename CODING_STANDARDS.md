# プロジェクト共通コーディング規約

---

# 目次

- **[第１章 基本方針](#第１章-基本方針)**
	- [1.1 目的](#11-目的)
	- [1.2 用語（強度定義）](#12-用語強度定義)
	- [1.3 基本原則](#13-基本原則)
	- [1.4 対象言語と適用範囲](#14-対象言語と適用範囲)
- **[第２章 フォーマット規約](#第２章-フォーマット規約)**
	- [2.1 文字コードと改行コード (MUST)](#21-文字コードと改行コード-must)
	- [2.2 インデント (MUST)](#22-インデント-must)
	- [2.3 行長と改行 (MUST)](#23-行長と改行-must)
	- [2.4 空行 (MUST)](#24-空行-must)
	- [2.5 波括弧の位置 (MUST)](#25-波括弧の位置-must)
	- [2.6 ラベルのインデント (MUST)](#26-ラベルのインデント-must)
	- [2.7 アクセス修飾子の順序 (MUST)](#27-アクセス修飾子の順序-must)
	- [2.8 数値リテラル (MUST)](#28-数値リテラル-must)
	- [2.9 演算子周りのスペース (MUST)](#29-演算子周りのスペース-must)
	- [2.10 const の位置 (MUST)](#210-const-の位置-must)
	- [2.11 参照・ポインタの記号位置 (MUST)](#211-参照ポインタの記号位置-must)
	- [2.12 sizeof の括弧 (MUST)](#212-sizeof-の括弧-must)
	- [2.13 文字列の引用符 (MUST)](#213-文字列の引用符-must)
	- [2.14 末尾カンマ (MUST)](#214-末尾カンマ-must)
	- [2.15 末尾セミコロン (MUST)](#215-末尾セミコロン-must)
	- [2.16 セミコロンとカンマの選択 (MUST)](#216-セミコロンとカンマの選択-must)
- **[第３章 制御構文](#第３章-制御構文)**
	- [3.1 括弧前スペース (MUST)](#31-括弧前スペース-must)
	- [3.2 波括弧 (MUST)](#32-波括弧-must)
	- [3.3 if-else のネスト回避 (SHOULD)](#33-if-else-のネスト回避-should)
	- [3.4 else / else if (MUST)](#34-else--else-if-must)
	- [3.5 switch (MUST)](#35-switch-must)
	- [3.6 for / while / do-while (MUST)](#36-for--while--do-while-must)
	- [3.7 goto・ラベル付 break (MUST)](#37-gotoラベル付-break-must)
	- [3.8 Ruby 制御構文 (MUST)](#38-ruby-制御構文-must)
- **[第４章 式・演算](#第４章-式演算)**
	- [4.1 順序 (MUST)](#41-順序-must)
	- [4.2 式の最適化 (SHOULD)](#42-式の最適化-should)
	- [4.3 シフト演算と逆数乗算 (MUST)](#43-シフト演算と逆数乗算-must)
	- [4.4 括弧 (MUST)](#44-括弧-must)
	- [4.5 クランプ (MUST)](#45-クランプ-must)
	- [4.6 0 判定 (MUST)](#46-0-判定-must)
	- [4.7 bool リテラル (MUST)](#47-bool-リテラル-must)
	- [4.8 bool 反転 (MUST)](#48-bool-反転-must)
	- [4.9 インクリメント・デクリメント (MUST)](#49-インクリメントデクリメント-must)
	- [4.10 三項演算子 (MUST)](#410-三項演算子-must)
	- [4.11 比較演算子の選択 (MUST)](#411-比較演算子の選択-must)
	- [4.12 比較演算子の使分 (MUST)](#412-比較演算子の使分-must)
- **[第５章 変数・型](#第５章-変数型)**
	- [5.1 変数化 (MUST)](#51-変数化-must)
	- [5.2 制御文での即時束縛 (MUST)](#52-制御文での即時束縛-must)
	- [5.3 同一型の一括宣言 (SHOULD)](#53-同一型の一括宣言-should)
	- [5.4 auto / var / any の使用制限 (MUST)](#54-auto--var--any-の使用制限-must)
	- [5.5 整数型の符号選択 (MUST)](#55-整数型の符号選択-must)
	- [5.6 引数無の void 省略 (MUST)](#56-引数無の-void-省略-must)
	- [5.7 return の明示 (MUST)](#57-return-の明示-must)
	- [5.8 const 優先 (MUST)](#58-const-優先-must)
	- [5.9 コンパイル時定数 (MUST)](#59-コンパイル時定数-must)
	- [5.10 遅延初期化・静的初期化 (SHOULD)](#510-遅延初期化静的初期化-should)
	- [5.11 クラス設計 (MUST)](#511-クラス設計-must)
	- [5.12 命名 (MUST)](#512-命名-must)
- **[第６章 ファイル構成・言語固有機能](#第６章-ファイル構成言語固有機能)**
	- [6.1 include / import の順序 (MUST)](#61-include--import-の順序-must)
	- [6.2 ヘッダガード (MUST)](#62-ヘッダガード-must)
	- [6.3 宣言と定義の分離 (MUST)](#63-宣言と定義の分離-must)
	- [6.4 キャスト (MUST)](#64-キャスト-must)
	- [6.5 マクロの使用制限 (MUST)](#65-マクロの使用制限-must)
	- [6.6 using namespace (MUST)](#66-using-namespace-must)
	- [6.7 NULL と nullptr (MUST)](#67-null-と-nullptr-must)
	- [6.8 エラー処理 (SHOULD)](#68-エラー処理-should)
	- [6.9 言語固有の必須事項 (MUST)](#69-言語固有の必須事項-must)
	- [6.10 型ヒント・null 安全 (MUST)](#610-型ヒントnull-安全-must)
	- [6.11 JSX 条件レンダリング (MUST)](#611-jsx-条件レンダリング-must)
	- [6.12 Ruby メソッドの１行定義 (MUST)](#612-ruby-メソッドの１行定義-must)
	- [6.13 内包表記 (SHOULD)](#613-内包表記-should)
	- [6.14 JSON (MUST)](#614-json-must)
	- [6.15 HTML 構造 (MUST)](#615-html-構造-must)
	- [6.16 CSS / SCSS (MUST)](#616-css--scss-must)
- **[第７章 コメント](#第７章-コメント)**
	- [7.1 基本 (MUST)](#71-基本-must)
	- [7.2 コメント形式 (MUST)](#72-コメント形式-must)
	- [7.3 禁止 (MUST)](#73-禁止-must)
	- [7.4 規約番号参照の禁止 (MUST)](#74-規約番号参照の禁止-must)
	- [7.5 宣言グループ (MUST)](#75-宣言グループ-must)
	- [7.6 ステップコメント (MUST)](#76-ステップコメント-must)
	- [7.7 インラインコメント (MUST)](#77-インラインコメント-must)
	- [7.8 ドキュメントコメント (MUST)](#78-ドキュメントコメント-must)
	- [7.9 セクション区切り (SHOULD)](#79-セクション区切り-should)
	- [7.10 return コメント (MUST)](#710-return-コメント-must)
	- [7.11 useEffect コメント (MUST)](#711-useeffect-コメント-must)
	- [7.12 JSX コメント (MUST)](#712-jsx-コメント-must)
- **[第８章 表記統一](#第８章-表記統一)**
	- [8.1 英単語の混入制限 (MUST)](#81-英単語の混入制限-must)
	- [8.2 漢字表記 (MUST)](#82-漢字表記-must)
	- [8.3 ら抜き言葉 (MUST)](#83-ら抜き言葉-must)
	- [8.4 複合名詞の送仮名省略 (MUST)](#84-複合名詞の送仮名省略-must)
	- [8.5 全角・半角の使分 (MUST)](#85-全角半角の使分-must)
	- [8.6 スペースの入れ方 (MUST)](#86-スペースの入れ方-must)
- **[第９章 例外規定](#第９章-例外規定)**
- **[第１０章 運用](#第１０章-運用)**
- **[第１１章 付録Ａ（典型アンチパターン）](#第１１章-付録ａ典型アンチパターン)**
- **[第１２章 付録Ｂ（競技プログラミング用テンプレート）](#第１２章-付録ｂ競技プログラミング用テンプレート)**

---

# 第１章 基本方針

## 1.1 目的

本規約は，コードの完全性・安全性・一貫性を始めとする，1.3 で定める優先順位に基付く品質の最大化を目的とする．又，大規模言語モデルに依るコード生成時にも本規約を厳守させる事で，人間と大規模言語モデルの間に於けるスタイルの乖離を防ぎ，統一された品質を担保する．

## 1.2 用語（強度定義）

- **MUST**: 必須．違反は認めない．
- **SHOULD**: 強く推奨．合理的な理由が有る場合のみ例外を認める．
- 本規約の例は当該節の規則のみを示す．

## 1.3 基本原則

以下の優先順位でコードの品質を追求する (MUST)：

1. **完全性** — 仕様を満たし，正しく動作する事を最優先とする
2. **安全性** — バグを未然に防ぎ，型安全性を担保する
3. **汎用性** — 特定の環境に依存せず，再利用可能にする
4. **効率性** — 冗長な処理を排除し，高速に動作させる
5. **一貫性** — 書き方を統一し，認知コストを下げる
6. **簡潔性** — 冗長な記述を排除し，最小限のコードで表現する
7. **保守性** — 変更や拡張が容易な構造にする
8. **可読性** — 第三者が理解し易いコードにする

上位項目と下位項目が矛盾する場合は上位を優先する．理由や背景（「何故」）はコード外で補完する (SHOULD)．

既存コードを変換する際は，値・型・評価順序と回数・副作用・例外・寿命・束縛・外部契約を保持する．数学的な等式や字面の形式だけで変換せず，意味の保存を確認出来ない場合は原形を保つ．

## 1.4 対象言語と適用範囲

本規約に於ける「全言語共通」の表記は，未列挙の言語も対象とする．個別節の言語指定は，其の節に限って適用する．対象言語に概念が存在して記法のみが異なる場合は，其の言語に於ける同等の記法を用いる．言語のバージョン及び実行対象はプロジェクトで固定する．

JavaScript は JSX，TypeScript は TSX，JSON は JSONC，CSS は SCSS をそれぞれ対象に含む．CSS に無い変数や制御式の記述は SCSS に適用する．埋込コードと，通常の属性値・本文・リテラルは区別する．

見本コード中の未定義名は外部から与える値・型・関数を表し，❌と✅は独立した対比を示す．例示の前提条件と全ての適用規約を満たした上で使用する．

---

# 第２章 フォーマット規約

## 2.1 文字コードと改行コード (MUST)

**対象**：全言語共通

- 文字コード：UTF-8（BOM 無）
- 改行コード：LF (`\n`)
- 行末の空白（スペース・タブ）は禁止
- ファイル末尾に改行を正確に１つ付与する（末尾の空行は禁止）

## 2.2 インデント (MUST)

**対象**：全言語共通

言語に依らず，インデントにはタブを使用する．

但し，YAML 等のインデントにタブを許可しない構文では，仕様に従って空白を用いる．本文やリテラル内の空白は，インデントとしては変更しない．

- C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, Python 等：表示幅４を推奨．ネストは最大３２段．
- JavaScript, TypeScript, Ruby, JSON, HTML, CSS（SCSS を含む）等：表示幅２を推奨．ネストは最大６４段．

**例外 (HTML)**：`<pre>`，`<textarea>`，及び `white-space` に依り空白を保持する要素では，内容の空白や改行が値又は表示に反映される為，本文のインデントを挿入・削除してはならない．開始タグ直後の改行も機械的に追加してはならない．`<script>` と `<style>` 内のコードは其の言語の規則に従うが，リテラルの内容は保持する．

**注意 (Python)**：構文上のインデントはタブに統一する．タブは構文解析時に８桁境界迄展開される為，推奨表示幅４とは異なる．タブと空白の混在に因ってブロックの解釈が変わる場合は `TabError` と為る．文字列内部の空白は，本インデント規約に依る変更の対象外とする．

## 2.3 行長と改行 (MUST)

**対象**：全言語共通

- １行は１２８文字以内とする（インデントとコメントを除く）．
- １２８文字を超える場合は，論理単位（引数の間，演算子の後等）で改行する．
- 改行時，二項演算子・三項演算子・代入演算子は改行前（前行の末尾）に配置する．
- メンバアクセス演算子 (`.` `?.` `->` `::`) と閉じ括弧 (`)` `]` `}`) は，改行後（次行の先頭）に配置する．
- 行継続（バックスラッシュ `\` + 改行）は禁止とする．改行が必要な場合は，括弧や演算子の位置で改行する．
- **１２８文字以内に収まる場合は改行してはならない．** 関数呼出の引数，import 文，変数宣言，単一文の `if` 等，１行にまとめて１２８文字以内に収まる物は必ず１行で記述する．

１行化に当たっては，本体・空行・閉じ波括弧の構造規則に従う．分割不能なトークンや，改行に依って構文・値・式結合が変わる箇所では，行長制限よりも意味の保持を優先する．尚，改行位置のみを示す短い対比例は，実際には行長制限を超過した場合に適用する．

**例外 (Go)**：メンバアクセスの `.` は行末に配置する．Go は識別子・`)`・`]`・`}` の直後の改行に対してセミコロンを自動挿入する為，行頭に置くと前行で文が終端して構文エラーと為る．閉じ括弧を行頭に置く場合は，直前の要素に末尾カンマが必要と為る（2.14 参照）．

**例外 (Python)**：`if cond: stmt` の様な複合文の１行化は禁止する．文の本体は行数に依らず，必ず次行へインデントして記述する．

**例外 (Python)**：複数行に跨がる式は丸括弧で囲む．括弧の外側では改行が文の終端と見做され，且つ行継続が禁止されている為，他に記述手段が無い．

**例外 (JSON)**：文字列値を分割する手段（連結・行継続）が無い為，長い文字列値は１２８文字制限の対象外とする．

**例外 (HTML)**：インライン要素の連結部及び属性値の内部では改行してはならない．何れも改行やインデントが空白文字として描画，又は属性値へ混入する為，１２８文字を超えても１行を維持する．

**例外 (CSS)**：`url()` は分割手段が無い為，１２８文字制限の対象外とする．長い文字列値は `\` ＋改行に依る継続を許可する．

```cpp
// ❌ 演算子が行頭
int Result = LongVariableA
+ LongVariableB;
bool Ok = ConditionA
&& ConditionB;

// ✅ 演算子が行末
int Result = LongVariableA +
LongVariableB;
bool Ok = ConditionA &&
ConditionB;

// ✅ 三項演算子
std::string Name = Condition ?
"valueA" :
"valueB";
```

```typescript
// ❌ メンバアクセスが行末（連鎖の起点が読み取れない）
const value = repository.
findAll().
filter(Predicate);

// ✅ メンバアクセスは行頭
const value = repository
.findAll()
.filter(Predicate);
```

```typescript
// ✅ １行（１２８文字以内）
const options = { willReadFrequently: true };

// ✅ 複数行（１２８文字を超える場合）
const offscreenRenderConfig = {
	canvasWidthInPhysicalDevicePixelsForOffscreenRenderTargetTexture: 32,
	canvasHeightInPhysicalDevicePixelsForOffscreenRenderTargetTexture: 16
};
```

## 2.4 空行 (MUST)

**対象**：全言語共通

### トップレベル宣言間

- 複数行に亘るトップレベル宣言（関数定義，複数行のクラス／構造体定義等）の間には，１行の空行を挿入する．
- 構造種別が異なる宣言間（宣言文 ↔ 関数定義 ↔ クラス／構造体定義 ↔ import / include 等）には，単一行同士であっても１行の空行を挿入する．
- 同一の構造種別であり，且つ単一行で完結する宣言が連続する場合は，空行を挿入しない．関数プロトタイプ・変数宣言・定数定義は文法上同一の**宣言文**として扱い，連続する場合は空行を挿入しない．

```cpp
// 同一の宣言文（関数プロトタイプ／変数宣言／定数定義）の連続は空行無
void Foo();
void Bar();
const int MaxCount = 100, MinCount = 0;

// 構造種別が変わる箇所には空行を入れる
class Status {};

// 複数行関数定義は常に空行で区切る
void Foo() {
	DoA();
}

void Bar() {
	DoB();
}
```

```typescript
import { A } from "a";
import { B } from "b";

const X = 1;

function Foo(): number {
	return 1;
}

function Bar(): number {
	return 2;
}
```

### プリプロセッサ (C, C++)

- 同じ種類のディレクティブ同士（`#include` 同士，`#define` 同士）の間には，空行を挿入しない．
- 異なる種類のディレクティブ間（`#pragma` → `#include`，`#include` → `#define` 等）には，１行の空行を挿入する．

```cpp
#pragma once

#include "Foo.hpp"
#include <string>
#include <vector>

#define MAX 100
#define MIN 0
```

### クラス・ブロック内

- クラス内のメンバ宣言間（メンバ関数定義，変数宣言，関数プロトタイプ等）の空行には，上記「トップレベル宣言間」と同一の規則を適用する．即ち，複数行に亘る宣言の前後には１行の空行を挿入し，同一の構造種別且つ単一行で完結する宣言が連続する場合は空行を挿入しない．
- アクセス修飾子 (`private:` `protected:` `public:`) の前後には，１行の空行を挿入する．但し，クラス本体の開始直後（`{` の直後）に在るアクセス修飾子の**前**には空行を挿入しない．

```cpp
class Foo {
private:

	int Count;
	int Total;

	void ProcessSourceWithIndices(
		const std::string &Source,
		const std::vector<int> &Indices,
		const std::map<std::string, int> &LookupTable,
		std::vector<int> &Output
	);

	void HelperShort();

public:

	void DoWork();
	void Reset();
};
```

### 例外

**例外 (C#, Java, Kotlin, Swift, PHP)**：アクセス修飾子がセクション構造を持たない為，修飾子前後の空行規則は適用しない（2.7 参照）．

**例外 (Python)**：デコレータと被修飾定義の間には空行を挿入しない．

**例外 (HTML)**：インライン要素間の空行や空白は空白１文字として描画される為，本節の空行規則の対象外とする．

### 禁止事項

- ２行以上の連続した空行は禁止とする．
- ファイルの先頭及び末尾への空行の挿入は禁止とする．

## 2.5 波括弧の位置 (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, JSON

開き波括弧 `{` の前で改行してはならない（K&R スタイル）．

複数行に展開した行頭の閉じ括弧 (`)` `]` `}`) の後に続く閉じ括弧は次行に配置し，其れ以外の字句（`,`／`;`／`else`／`from`／本体の `{`／メンバアクセス／演算子等）は同一行に続ける．構文・式の結合を変える改行は行わない．

関数呼出の引数として渡すオブジェクトリテラル (`{...}`) が複数行に亘る場合，`{` は独立した行に配置し，関数呼出と同一行にしてはならない．１行に収まる場合は 2.3 に従って１行で記述する．

**例外 (Go)**：複合リテラルは型名と `{` の間で改行出来ない（セミコロンが自動挿入される）為，`{` を型名と同一行に配置する．

**例外 (Kotlin)**：末尾ラムダ (`Items.forEach { ... }`) は，呼出式と同一行に `{` を置かないと呼出に結合せず構文エラーと為る為，同一行に配置する．

**例外 (Swift)**：末尾クロージャはオブジェクトリテラルではない為，本規則の対象外とする（Swift の辞書・配列リテラルは `[...]` を用いる）．

**本段落の対象**：上記に加え Ruby, Python（何れもハッシュ・辞書・集合のリテラルと内包表記が波括弧を用いる為）

波括弧の開始と終了が同一行に在る場合は，波括弧の内側にスペースを１つ挿入する．空の本体 `{}` はスペース不要で同一行に記述する（本体が空の関数は，5.7 に定める末尾の `return;` の対象外とする）．

**例外 (スペースの非挿入)**：以下の `{` `}` の内側にはスペースを挿入しない．
- JSX 内の式埋込：`{expression}`
- テンプレートリテラル・文字列補間：`` `${expression}` ``，`$"{expression}"` (C#)，`"{$expression}"` / `"${variable}"` (PHP)，`"#{expression}"` (Ruby) 等．PHP の複雑構文（複素構文）は `{` の直後が `$` である事が要件の為，スペースを挿入すると補間が無効化され，波括弧が文字として其の儘出力される．

```cpp
// ❌
if(VeryLongConditionExpressionToPreventCollapseFromHundredTwentyEightCharacterLimit)
{
	PerformLongerOperationCallWithReasonablyDescriptiveName();
}

// ✅
if(VeryLongConditionExpressionToPreventCollapseFromHundredTwentyEightCharacterLimit) {
	PerformLongerOperationCallWithReasonablyDescriptiveName();
}

// ❌
void Foo()
{
}

// ✅
void Foo() {}
```

```typescript
// ❌ 関数呼出と同じ行に { を配置
const screenStylesheetB = StyleSheet.create({
	mainContainerWithReasonablyLongDescriptiveIdentifierName: {
		flexGrowFactorForLayoutComputationEngineModule: 1
	}
});

// ✅ 関数引数の { を独立した行に配置
const screenStylesheetA = StyleSheet.create(
	{
		mainContainerWithReasonablyLongDescriptiveIdentifierName: {
			flexGrowFactorForLayoutComputationEngineModule: 1
		}
	}
);
```

```typescript
// ❌ スペース無
const options = {willReadFrequently: true};

// ✅ 同一行の波括弧：内側にスペース
const options = { willReadFrequently: true };
const pair = { key: "a", value: 1 };

// ✅ 空の本体はスペース不要
function noop(): void {}
```

```typescript
// ✅ 空の本体はスペース不要（catch の本体を空にする事は 6.8 が禁じる為，例には用いない）
const Noop = (): void => {};
```

```tsx
// ❌
<Text>{ item.name }</Text>

// ✅
<Text>{item.name}</Text>
const msg = `Hello ${name}`;
```

```ruby
greeting = "Hello #{name}"
```

## 2.6 ラベルのインデント (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby（`when` のみ）

`case` / `default`（Ruby は `when` / `else`），アクセス修飾子（`private:` / `protected:` / `public:`．C++ のみ），goto ラベル (C, C++, C#, Go, PHP)，ループラベル (Java, Go, Swift, JavaScript, TypeScript) は，内部の処理より１段浅くインデントする．即ち，ラベルは親ブロック（`switch`，クラス，関数の各 `{}`）と同一のインデントレベルに配置し，内部のコードはラベルから１段深くインデントする．

尚，Rust の `match` アーム及び Kotlin の `when` 分岐は，`switch` の `case` に相当するが本体をインデントで区切る為，ラベルとしては扱わない．又，C#, Java, Kotlin, Swift, PHP のアクセス修飾子はメンバ毎の修飾子であり，ラベルではない為本節の対象外とする（2.7 参照）．

**例外 (Rust, Kotlin)**：ラベルは `'outer: loop { }` (Rust) / `OuterLoop@ for(...)` (Kotlin) の形式で対象ループと同一行の先頭に配置する為，独立行への配置やインデント調整は行わない．

**例外 (Ruby)**：`case` / `when` では，`when` を `case` と同一のインデントレベルに配置する．

```cpp
// ❌ 悪い例（ラベルのインデントが深すぎる）
switch(Status) {
	case 1:
		return "active";
	default:
		return "unknown";
}

// ✅ 良い例（ラベルは周囲より１段浅く，中身は通常インデント）
switch(Status) {
case 1:
	return "active";
case 2:
	return "inactive";
default:
	return "unknown";
}

// ✅ goto ラベル（関数内）
void Foo() {
	for(int i = 0; i < h; ++i) for(int j = 0; j < w; ++j) if(Grid[i][j] == Target) goto Found;
	return;
Found:
	Process();
	return;
}
```

```ruby
case status when 200 then handle_successful_request_with_long_name(payload, headers)
when 404 then handle_resource_not_found_with_long_name(payload, headers)
else handle_unknown_status_code_with_long_name(payload, headers) end
```

## 2.7 アクセス修飾子の順序 (MUST)

**対象**：C++, C#, Java, Kotlin, Swift, PHP, TypeScript

クラス定義では，可視性を `private` → `protected` → `public` の順で記述する．不要なセクションは省略して良い．被呼出側（内部実装）を先頭に，公開インタフェースを末尾に配置する事で，定義順（5.11 参照）と一致させる．C++ の `class` は既定で `private` と為るが，`private` メンバが存在する場合は `private:` を省略せず明記する．

メンバの並替に依って，公開範囲・初期化順序・メモリ配置・外部契約を変更してはならない（5.11 参照）．

**例外 (C#, Java, Kotlin, Swift, PHP, TypeScript)**：修飾子をメンバ毎に付与する構文であり，セクション（`private:` 形式）が存在しない為，同一の順序で**メンバの並び順**を制御する事で本規則を満たす物とする．セクションが存在しない為，2.4 の修飾子前後の空行規則及び 2.6 の修飾子ラベルのインデント規則は適用しない．

**例外 (C#)**：可視性は `private` → `private protected` → `protected` → `internal` → `protected internal` → `public` の順とする．

**例外 (Java)**：可視性は `private` → パッケージ非公開（修飾子無）→ `protected` → `public` の順とする．パッケージ非公開と `protected` を混同してはならない．

**例外 (Kotlin)**：可視性は `private` → `protected` → `internal` → `public`（既定）の順とする．

**例外 (Swift)**：`protected` は存在しない．可視性は `private` → `fileprivate` → `internal`（既定）→ `package` → `public` → `open` の順とし，既定の `internal` も省略せず明記する．

```cpp
class Foo {
private:

	int Baz;

protected:

	void Qux();

public:

	void Bar();
};
```

## 2.8 数値リテラル (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python

### 浮動小数点

**対象**：C, C++, C#, Java, Kotlin

`float` 型リテラルには必ず `F` 接尾辞を付与する．`double` 型リテラルには接尾辞を付与しない．小数点の前後の `0` は省略してはならない．尚，C# の `decimal` 型には `M` 接尾辞を付与する．

```cpp
// ❌
float Pi = 3.14;
float Half = .5;
float One = 1.;

// ✅
float Pi = 3.14F;
float Half = 0.5F;
float One = 1.0F;
```

### 接頭辞・接尾辞・指数の英字は大文字

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python

数値リテラルに含まれる英字（接頭辞・接尾辞・指数記号・１６進数の桁）の内，大文字と小文字の両方が構文上有効な物は，全て大文字を使用する．

- 整数接尾辞：`L`，`U`，`UL`，`LL`，`ULL` 等を使用する（`l`，`u` は不可）．大文字・小文字の揺れが許容される接尾辞を持つのは C, C++, C#, Java, Kotlin のみである（Kotlin は `u` / `U` のみで `l` は不正．Go に整数接尾辞は無く，Rust の型接尾辞及び JavaScript / TypeScript の BigInt `n` は小文字のみ有効であり，下記の例外に該当する）．
- １６進接頭辞と桁：`0X`，`0XFF` を使用する（`0x`，`0xff` は不可）．
- ２進接頭辞：`0B` を使用する（`0b` は不可）．C では C23 以降でのみ利用出来る．
- ８進接頭辞：`0O` を使用する（`0o` は不可）．`0o` 形式を持つのは Go, Rust, Swift, PHP（8.1 以降），JavaScript, TypeScript, Ruby, Python のみである．C は `017` 表記を用い，C# と Kotlin に８進リテラルは存在せず，Java は先頭 `0` 表記を用いる．
- 指数表記：`1.5E2`，`1.0E-3` を使用する（`e` は不可）．

**例外**：大文字にすると構文エラーに為る文字（小文字のみ有効で大文字・小文字の選択肢が無い文字）は対象外とする．此れには Rust の型接尾辞（`u32` / `i64` / `f32` 等）や接頭辞 `0x` / `0b` / `0o`，Swift の接頭辞 `0x` / `0b` / `0o`（整数・１６進浮動小数点数の何れも小文字のみ有効），多倍長整数 (BigInt) の `n`，虚数単位の `i` (Ruby) が該当する．

```cpp
// ❌
long Big = 1234567890l;
unsigned int Mask = 0xdeadbeef;
unsigned long Flags = 0xffull;
int Bits = 0b1010;
float Exp = 1.5e2f;

// ✅
long Big = 1234567890L;
unsigned int Mask = 0XDEADBEEF;
unsigned long Flags = 0XFFULL;
int Bits = 0B1010;
float Exp = 1.5E2F;
```

## 2.9 演算子周りのスペース (MUST)

**対象**：全言語共通

CSS（SCSS を含む）は記号の多くが演算子ではなく字句の一部である為，A〜K ではなく末尾の「L. CSS の空白」に従う．JSON の区切文字 `:` と `,` は E 及び H に従う．HTML は属性の `=` (I) 及び属性間の空白を対象とし，通常の属性値は変更しない．既存の `style` 属性やイベントハンドラ属性等に埋め込まれたコードに限り，埋込構文及び JavaScript / CSS の意味を損なわない範囲で本節を適用する（新規記述の可否は 6.15 参照）．

### A. 前後にスペース ` op `

二項演算子の前後にはスペースを１つ挿入する．

| 分類 | 演算子 |
|------|--------|
| 算術 | `+` `-` `*` `/` `%` `**` `//`(Python) |
| 比較 | `==` `!=` `===` `!==` `<` `>` `<=` `>=` `<=>` `=~` `!~` |
| 論理 | `&&` `\|\|` `and` `or`（PHP, Ruby, Python の語演算子），`xor` (PHP) |
| ビット | `&` `\|` `^` `<<` `>>` `>>>`（Kotlin を除く．Kotlin は中置関数 `and` / `or` / `xor` / `shl` / `shr` / `ushr`） |
| 文字列連結 | `.` (PHP)．`.` を前後スペース無で書くと小数点・構文エラーに為る為，D のメンバアクセスとは扱いが異なる |
| 代入 | `=` `+=` `-=` `*=` `/=` `%=` `**=` `//=` `<<=` `>>=` `>>>=` `&=` `^=` `\|=` `??=` `&&=` `\|\|=` |
| 三項 | `?` `:` |
| アロー | `=>`（ラムダ・アロー関数，Rust の `match` アーム，PHP の配列，Ruby のハッシュと `rescue => e`），`->`（Java のラムダ，Rust / Swift / Python の戻り値型及び関数型，Kotlin の `when` の分岐とラムダの引数区切り） |
| Null 合体 | `??` `?:` |
| 型演算 | `as` `as?` `is` `!is` `in` `!in` `instanceof` |
| Go 固有 | `:=` `<-` |

```cpp
int x = a + b;
bool Ok = a && b;
int Sign = x > 0 ? 1 : -1;
```

### B. スペース無 `op`（前置単項）

前置単項演算子とオペランドの間にはスペースを入れない．

| 演算子 | 例 |
|--------|-----|
| 符号 | `-x` `+x` |
| 論理否定 | `!Flag`（Python は語演算子 `not x` で空白が要る） |
| ビット否定 | `~Mask`（Kotlin は `Mask.inv()`） |
| インクリメント・デクリメント | `++i` `--i`（Go は文の `i++` のみ．Rust, Swift, Ruby, Python に `++` / `--` は無い） |
| ポインタ間接参照 | `*Ptr`（C, C++, Go, Rust. C# は unsafe 文脈に限る） |
| アドレス取得・参照 | `&Var`（C, C++, Go, Rust, PHP. C# は unsafe 文脈に限る） |
| スプレッド | `...Arr` `*arr`(Kotlin) `*Arr`(Ruby / Python) |
| チャネル受信 | `<-Ch`(Go)．型記法 `chan<- int` / `<-chan int` も詰めて書く |

### C. スペース無 `op`（後置単項）

後置単項演算子とオペランドの間にはスペースを入れない．

| 演算子 | 例 |
|--------|-----|
| インクリメント・デクリメント | `i++` `i--` |
| 非 null 表明・強制アンラップ | `x!`(C# / Swift / TypeScript) `x!!`(Kotlin)．C# / Kotlin / Swift / TypeScript の何れも 6.10 で使用を禁止する |
| エラー伝播 | `expr?`(Rust) |

### D. スペース無（メンバアクセス・スコープ）

| 演算子 | 例 |
|--------|-----|
| ドット | `Obj.Member` |
| アロー（メンバ） | `Ptr->Member` (C, C++, PHP) |
| オプショナルチェーン | `Obj?.Member`(Kotlin / Swift / JavaScript / TypeScript) `$obj?->member`(PHP) |
| スコープ解決 | `std::cout`(C++) `mod::func`(Rust) `Foo::bar` / `self::` / `static::` / `parent::`(PHP) |
| 安全呼出 | `Obj&.Method`(Ruby) |
| 範囲 | `1..10`（Rust は排他，Kotlin と Ruby は包含）`1...10`（Swift は包含，Ruby は排他）`1..<10`（Kotlin 1.9 以降，Swift）`1..=10`(Rust) `1 until 10`(Kotlin) |

### E. 前スペース無，後スペース有 `x: T`

型注釈・フィールド定義・辞書リテラルのコロン．

| 文脈 | 例 |
|------|-----|
| 型注釈 | `x: i32`(Rust) `x: number`(TypeScript) `x: int`(Python) |
| フィールド定義 | `pub x: i32`(Rust) `val Name: String`(Kotlin) |
| 辞書・オブジェクト | `{ key: value }`(JavaScript / TypeScript / Ruby / Python) |

### F. 前後スペース有 ` : `

| 文脈 | 例 |
|------|-----|
| 三項演算子 | `x ? a : b` |
| 基底クラス (C++) | `class A : public B` |
| 継承 (C# / Kotlin / Swift) | `class A : B` |
| 範囲 for (C++ / Java) | `for(auto &V : Arr)`(C++) `for(X x : Arr)`(Java) |

### G. スペース無 `:`（ラベル・ブロック開始）

| 文脈 | 例 |
|------|-----|
| case / default | `case 1:` `default:` |
| goto ラベル | `Found:` |
| Python ブロック開始 | `if x:` `def f():` `class A:` |

### H. 前スペース無，後スペース有 `,` `;`

カンマの前にはスペースを入れない．同一行でカンマの後に次の要素が続く場合は，スペースを１つ挿入する．直後が閉じ括弧又は改行の場合はスペースを入れない．セミコロンも同様とする．

```cpp
Func(a, b, c);
for(int i = 0; i < n; ++i) Process(i);
```

### I. スペース無 `=`（特定文脈）

| 文脈 | 例 |
|------|-----|
| Python キーワード引数 | `func(key=value)` |
| Python 既定引数 | `def f(x=0):` |
| HTML 属性 | `class="foo"` |

### J. 括弧の内側スペース無

開き括弧 `(` `[` の直後及び閉じ括弧 `)` `]` の直前にはスペースを入れない．括弧の内部に現れる字句同士の空白は本規則の対象外とする（`{` `}` の規則は 2.5 を参照）．関数・メソッド名と呼出・宣言の `(` の間，及び添字対象と `[` の間にもスペースを入れない．

```cpp
Func( a, b ); // ❌
Func (a, b); // ❌
Func(a, b); // ✅
Arr[ i ]; // ❌
Arr[i]; // ✅
```

### K. 型パラメータ括弧 `<` `>`

ジェネリクス及びテンプレートの `<` `>` の内側にはスペースを入れない．

```cpp
std::vector<int> V;
```

```java
HashMap<String, List<Integer>> Map;
```

### L. CSS の空白

**対象**：CSS, SCSS

CSS では `+` `-` `%` `>` `~` が演算子ではなく字句の一部又は結合子である為，A〜K は適用しない．次の規則に従う．

- 識別子（プロパティ名・カスタムプロパティ名・関数名・キーワード）に含まれるハイフンは演算子ではない為，前後に空白を入れない (`margin-top`, `--main-color`, `hue-rotate`)．
- `%`，`unicode-range` の `U+`，及び `An+B` 記法 (`nth-child(-n+3)`) の符号は字句の一部である為，空白を入れない．
- `calc()` 内の二項演算子 `+` `-` は言語仕様に依り両側の空白が要求される為，必ず空白を入れる (`calc(100% - 10px)`)．前置単項演算子と見做して空白を削除してはならない．
- 空白を挿入するのは，子孫結合子（`.a .b` の空白．意味を持つ為削除しない），結合子 `>` `+` `~` の前後，カンマの後，プロパティのコロンの後，`{` の前，`!important` の前，`@media (min-width: 600px) and (...)` の `and` の前後，SCSS の演算子 (`$a + $b`) 及び `@if` / `@each` の後とする．
- `:where()` `:is()` `:not()` `:has()` の括弧内に現れる空白は子孫結合子であり構文上の意味を持つ為，削除してはならない（`:where(.a .b)` と `:where(.a.b)` は異なる）．

## 2.10 const の位置 (MUST)

**対象**：C, C++

`const` は型名の前に配置する (west const)．

```cpp
// ❌ (east const)
int const Value = 10;
int const &Ref = Value;

// ✅ (west const)
const int Value = 10;
const int &Ref = Value;
```

## 2.11 参照・ポインタの記号位置 (MUST)

**対象**：C, C++

ポインタ記号 `*` は変数名側に付与する．C++ では参照記号 `&` も同様とする（C に参照型は存在しない）．

```cpp
// ❌
int* Ptr;
const int& Ref = Value;
void Func(const std::string& Name);

// ✅
int *Ptr;
const int &Ref = Value;
void Func(const std::string &Name);
```

## 2.12 sizeof の括弧 (MUST)

**対象**：C, C++

`sizeof` には必ず括弧を付与する．式に対して括弧の記述が文法上任意であっても省略してはならない．

```cpp
// ❌
int n = sizeof LangTable / sizeof LangTable[0];

// ✅
int n = sizeof(LangTable) / sizeof(LangTable[0]);
```

## 2.13 文字列の引用符 (MUST)

**対象**：全言語共通

文字列リテラルにはダブルクォート `"` を使用する．

但し，文字列の区切文字が異なる言語では其の言語の記法に従う．SQL の文字列リテラルはシングルクォート `'...'` であり，識別子を表すダブルクォート `"..."` と混同してはならない．又，引用符の種類に依って変数補間やエスケープシーケンスの展開規則が変わる言語に於いても，意図した値を保持する引用符を使用する．

**例外 (C, C++, C#, Java, Go, Rust, Kotlin)**：単一引用符で囲む文字リテラル及び rune リテラル (`'a'`) は文字列型とは異なる型を持つ為，本節の対象外とする．二重引用符に変更すると型が変わり，多くは型不一致に為る．

**例外 (PHP)**：単一引用符と二重引用符では挙動が異なり，二重引用符のみが変数展開 (`$var`, `{$expr}`, `$arr[key]`, `$obj->prop`) やエスケープシーケンス（`\n` `\t` `\x..` 等）を解釈する．従って，変数展開又はエスケープを意図する場合に限り二重引用符を用い，其れ以外は単一引用符を用いる．引用符の機械的な統一や一括変換は禁止する．

**例外 (Ruby)**：単一引用符は非展開リテラルであり，`\\` と `\'` 以外のエスケープシーケンスを解釈しない．`#{` / `#@` / `#$` 又は `\` を含む単一引用符リテラルを二重引用符へ変換してはならない（補間やエスケープが処理されて値が変化する為）．

**例外 (HTML)**：属性値自体にダブルクォート `"` が含まれる場合は，シングルクォート `'` を用いる．二重引用符で囲むと属性値が途中で切詰められ，後続部分が別の属性として解釈される為である．

**注意 (Python)**：Docstring には三重二重引用符 `"""` を用いる（三重単一引用符 `'''` も構文上は有効だが統一する）．引用符を変更する際は，エスケープと文字列値を保持する事．

**注意**：生文字列 (raw string)，複数行文字列，テンプレートリテラル，正規表現リテラルの区切文字を通常の引用符として変換しない．接頭辞，補間構文，改行，エスケープ，タグ付テンプレート等に於ける生の値は全て保持する．

```typescript
// ❌
const name = 'hello';

// ✅
const name = "hello";
```

## 2.14 末尾カンマ (MUST)

**対象**：全言語共通

配列・オブジェクトリテラル・引数リスト等の末尾にはカンマを付与しない．

**例外**：以下の場合は，構文上又は意味の保持の為にカンマが必要と為る為付与する．
- Go の複数行リテラル及び引数リスト（言語仕様上必須．`,` を省略すると構文エラーと為る）．
- Rust の単一要素タプル `(x,)` 及びタプル型 `(T,)`（カンマを削除すると通常の括弧式又は括弧型と為り，型が変化する為）．
- Python の単一要素タプル `(x,)`（`(x)` は単なる括弧付の式として扱われ，タプルに為らない為）．
- CSS の `var()` の空フォールバック `var(--x,)`（末尾カンマがフォールバック値の一部であり，削除すると変数未定義時の挙動が変化する為）．
- TSX に於ける制約・既定型を伴わない単一型仮引数を持つアロー関数 `const identity = <T,>(value: T): T => value;`（カンマを削除すると JSX タグの開始と区別が付かなく為る為）．
- JavaScript / TypeScript の配列に於ける空要素 `[,]` / `[1,,]`（最後のカンマが空要素の数を決定する為，末尾区切文字として削除してはならない）．
- Go の型仮引数・型引数・パラメータリスト等に於いて，改行直前のカンマが自動セミコロン挿入を抑止する為に必要な場合は保持する．

```typescript
// ❌
const Colors = [
	"firstColorOptionWithReasonablyLongDescription",
	"secondColorOptionWithReasonablyLongDescription",
	"thirdColorOptionWithReasonablyLongDescription",
];

// ✅
const Colors = [
	"firstColorOptionWithReasonablyLongDescription",
	"secondColorOptionWithReasonablyLongDescription",
	"thirdColorOptionWithReasonablyLongDescription"
];
```

```go
// ✅ Go の複数行では末尾カンマ必須
items := []string{
	"first item with a reasonably long description",
	"second item with a reasonably long description",
	"third item with a reasonably long description",
}
```

```python
# ✅ 単一要素タプル（カンマ必須）
single = 42,
```

## 2.15 末尾セミコロン (MUST)

**対象**：全言語共通

文の末尾にはセミコロンを付与する．セミコロンの記述が任意である言語（JavaScript, TypeScript 等）に於いても省略せず，自動セミコロン挿入 (ASI) に依存してはならない．CSS に於いても，ブロック内の最後の宣言に必ずセミコロンを付与する（6.16 参照）．

**例外 (Kotlin, Swift, Ruby, Python)**：行末のセミコロンは不要な冗長記号である為記述しない（同一行に複数の文を記述する場合の区切文字としてのみ許可する）．

**例外 (JSON, HTML)**：文及びセミコロンの概念が存在しない為，本節の対象外とする（JSON にセミコロンを記述すると構文エラーと為る）．

**例外 (Go)**：構文規則に依り行末の改行がセミコロンとして解釈・自動挿入される為，行末には記述しない（同一行に複数の文を記述する場合の区切文字としてのみ許可する）．

**例外 (Rust)**：ブロック末尾の式に `;` を付与すると評価値が `()` と為り戻り値の型が変わる為（例: `fn f() -> i32 { a + b }` に `;` を付けると型不一致と為る），文の終端にのみ付与する．

```typescript
// ❌
const name = "hello"
const greet = () => "hi"

// ✅
const name = "hello";
const greet = (): string => "hi";
```

## 2.16 セミコロンとカンマの選択 (MUST)

**対象**：TypeScript

セミコロンとカンマの何方も使用可能な文脈（インタフェース本体，オブジェクト型等）では，カンマを使用する．

```typescript
// ❌
interface Props {
	firstReasonablyLongPropertyNameToPreventCollapse: string;
	secondReasonablyLongPropertyNameToPreventCollapse: number;
}

// ✅
interface Props {
	firstReasonablyLongPropertyNameToPreventCollapse: string,
	secondReasonablyLongPropertyNameToPreventCollapse: number
}
```

---

# 第３章 制御構文

## 3.1 括弧前スペース (MUST)

**対象**：C, C++, C#, Java, Kotlin, PHP, JavaScript, TypeScript

制御構文キーワード（`if`, `for`, `while`, `switch`, `catch` 等）と括弧の間にスペースを入れない（関数・メソッドの呼出及び添字の括弧は 2.9 J 参照）．

```cpp
// ❌
if (Condition) return;
for (int i = 0; i < n; ++i) Process(i);

// ✅
if(Condition) return;
for(int i = 0; i < n; ++i) Process(i);
```

## 3.2 波括弧 (MUST)

**対象**：全言語共通

### 原則

単一文では波括弧を使用せず，制御構文と同一行に記述する．行長制限（2.3: １２８文字）を超える場合に限り改行して次行に記述し，其の際は波括弧を付ける．

**例外 (Go, Rust, Swift)**：言語仕様上，本体の波括弧が必須で省略出来ない為，単一文でも波括弧を記述し，`if cond { return }` の形で同一行に収める．

**例外 (Ruby, Python)**：本体の範囲がインデント又は `end` で決まり波括弧を用いない．Ruby は修飾子形式（3.8 参照），Python は本体を次行へインデントする（2.3 の例外を参照）．

**例外 (JSON, HTML, CSS)**：波括弧は文の本体ではなくオブジェクトや規則集合の区切文字である為，本節の対象外とする（省略すると構文エラーに為る）．尚，HTML に波括弧を用いる構文は無い．

- Kotlin は波括弧を省略出来る為原則に従うが，条件の丸括弧は言語仕様上必須である（`if Cond { return }` は構文エラー）．

```cpp
// ❌
if(Cond) {
	return;
}

// ✅
if(Cond) return;
```

```cpp
// １２８文字を超える場合のみ許可
if(VeryLongConditionalExpressionExceedingHundredTwentyEightChars) {
	DoSomethingWithVeryLongArguments(Arg1, Arg2, Arg3, Arg4, Arg5);
}
```

### 波括弧が必須の条件

以下の場合は必ず波括弧を使用する：

- 本体が２文以上の場合．
- 本体が宣言の場合．
- `try` / `catch` 文の場合（C に例外機構は無い為対象外．Swift は `do-catch`，Ruby は `begin-rescue`）．
- 本体が単一文であっても，制御構文と同じ行（条件を折り返した場合は閉じ括弧の行）に続けると行長制限 (2.3) に収まらない場合，又は本体自身が複数行に渡る場合．
- 波括弧を外すと `else` の結合先，宣言のスコープ，リソースの寿命，又は言語固有の構文が変わる場合．

```cpp
// ❌ 危険（DoB() は常に実行される）
if(Condition)
	DoA();
	DoB();

// ✅
if(Condition) {
	DoA();
	DoB();
}
```

```cpp
// ❌（単一文だが引数の折返で複数行に渡る — 波括弧無だと範囲が誤読され易い）
if(SomeReasonablyLongConditionExpression) PerformOperationWithLongerDescriptiveName(
	FirstArgumentWithReasonablyLongName,
	SecondArgumentWithReasonablyLongName,
	ThirdArgumentWithReasonablyLongName
);

// ✅
if(SomeReasonablyLongConditionExpression) {
	PerformOperationWithLongerDescriptiveName(
		FirstArgumentWithReasonablyLongName,
		SecondArgumentWithReasonablyLongName,
		ThirdArgumentWithReasonablyLongName
	);
}
```

### if-else で片方のみ複数文の場合

単一文側は波括弧を省略し，複数文側のみ波括弧を付ける．

```cpp
// ❌（単一文側にも波括弧）
if(Condition) {
	DoA();
} else {
	DoB();
	DoC();
}

// ✅（単一文側は省略）
if(Condition) DoA();
else {
	DoB();
	DoC();
}
```

## 3.3 if-else のネスト回避 (SHOULD)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python

ネストが深く為るのを避ける為，`if-else` 及び三項演算子（三項演算子を持つ言語のみ．4.10 参照）では，`if ... else if ... else if ... else` の形で記述出来る様に条件を設定する．`if` の中に `if` をネストさせるのではなく，条件を反転させて `else` 側に `if` を配置し，`else if` チェーンにする．

本体が同一の `if` は，統合しても意味が変わらない限り条件を `||` で結合して１文へ統合する (MUST)．条件が重複する場合は，其の条件式が呼出・代入・増減を含まず再評価しても副作用が観測されない時に限り，重複を取り除く（副作用を持つ条件は２回目の評価も観測される為，同一の字面であっても残す）．

尚，下例は通常の整数値を前提とする．NaN や演算子の多重定義では，`!(x <= a)` と `x > a` が等価とは限らない．又，統合は短絡評価・破棄時点・スコープの束縛範囲を保持出来る場合に限る．多重定義された論理演算子や，プロパティ・添字・`volatile`・共有状態の読取を，同一の字面だけで統合・重複除去しない．

**例外**：条件が同時に成立し得る連続した `if` で本体に副作用が有る場合 (`if(a) Log(); if(b) Log();`) は，統合すると実行回数が変わる為統合しない．

```cpp
// ❌ if の中に if をネスト（深く為る）
if(x <= 10) {
	if(x <= 6) DoC();
	else DoB();
} else DoA();

// ✅ 条件を反転して else if チェーンにする
if(x > 10) DoA();
else if(x > 6) DoB();
else DoC();

// 三項演算子も同様

// ❌ ネストが深い
const int LongResultIdentifierForVeryDescriptiveStorageInLogicLayer = LongInputValue <= UpperThresholdValue ?
LongInputValue <= LowerThresholdValue ? LowResultValue : MidResultValue :
HighResultValue;

// ✅ 条件を反転して平坦に
const int LongResultIdentifierForVeryDescriptiveStorageInLogicLayer = LongInputValue > UpperThresholdValue ?
HighResultValue :
LongInputValue > LowerThresholdValue ? MidResultValue : LowResultValue;
```

```cpp
// ❌ 同じ本体の if が連続（先行の脱出で後続は実行されない）
if(!Ptr) return;
if(!Count) return;

// ✅ 条件を || で結合
if(!Ptr || !Count) return;

// ❌ else if で同じ本体
if(a) Reset();
else if(b) Reset();

// ✅ 条件を || で結合
if(a || b) Reset();
```

## 3.4 else / else if (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python

単一文の `else` / `else if` も波括弧を省略する．単一文の場合，`else` は直前の `if` の次行に配置する．波括弧を付ける場合は閉じ波括弧と同一行 (`} else {`) に配置する．

**例外 (Go, Rust, Swift)**：言語仕様上，本体の波括弧が必須で省略出来ない為（3.2 参照），`} else if cond {` / `} else {` の形とする．

**例外 (Ruby, Python)**：`elsif` / `elif` を用い，本体をインデント又は `end` で区切る（3.8 参照）．

```cpp
if(a) DoA();
else if(b) DoB();
else DoC();
```

## 3.5 switch (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python

同一変数に対する３分岐以上の値比較には `switch`（Rust は `match`，Kotlin は `when`，Ruby は `case`，Python 3.10 以降は `match`）を使用する．分岐への書換は，受理する型と比較の意味を保持出来る場合に限る．Python の捕捉パターン，Ruby の `===`，PHP の緩い比較を，単純な値比較と混同してはならない．

明示的な処理が必要な場合は `default` 句（Rust / Python は `_`，Kotlin / Ruby は `else`）を必ず記述する．処理が不要（何もせず素通りで良い）な場合は省略出来る．尚，Ruby の `case` の記述詳細は 3.8 に従う．

**例外（Go, Rust, Kotlin, Swift, PHP の `match`）**：暗黙のフォールスルーが無い為，複数値の併記は `case 2, 3:` (Go, Swift)，`2 | 3 =>` (Rust)，`2, 3 ->` (Kotlin)，`2, 3 => ...`（PHP の `match`）の様に列挙する．Go に於いて空の `case` を並べる書き方は，素通りせず何も実行しない為意味が変わる．

**例外 (Rust, Swift)**：分岐の網羅性が言語仕様上必須である．全ての値を列挙出来る場合は既定分岐を省略出来る．未処理の値が残る場合は Rust の `_ => {}`，Swift の `default: break` 等を用いる．値を返す式では各分岐の型も満たす必要が有る．

**例外 (C#)**：各 `case` 区画は `break` / `return` / `throw` / `continue` / `goto case` 等，制御が区画の末尾へフォールスルーしない形で終端させる事が必須である（暗黙のフォールスルーは CS0163 エラー）．中身の無い `case` ラベルを積み重ねる記述（`case 1: case 2: 処理; break;`）は可能である．

**注意 (C#, Java)**：全 case が同一変数への代入である場合は，下記の関数切出に代えて switch 式（C# 8 以降の `x switch { 1 => "A", _ => "X" }`，Java 14 以降の `case 1 -> "A";`）を用いて良い．

**注意 (PHP)**：`switch` の比較は緩い比較 (`==`) で行われる．厳密な同一性が必要な場合の書き方は 4.12 に定める．

```cpp
switch(Code) {
case 1:
	return "A";
case 2:
case 3:
	return "BC";
default:
	return "X";
}
```

### 関数切出

全 case が同一変数への代入のみの場合は，switch 文を関数に切り出して戻り値で返す．

```cpp
// ❌
std::string Message;
switch(Reason) {
case 1:
	Message = "見付かりません";
	break;
case 2:
	Message = "権限が有りません";
	break;
default:
	Message = "エラー";
	break;
}

// ✅
std::string GetMessage(int Reason) {
	switch(Reason) {
	case 1:
		return "見付かりません";
	case 2:
		return "権限が有りません";
	default:
		return "エラー";
	}
}
```

## 3.6 for / while / do-while (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python

### 使分

- **for**: 反復回数が既知の場合，又はイテレータで走査する場合に使用する．
- **while**: 終了条件のみが既知で，０回実行の可能性が有る場合に使用する．
- **do-while**: 本体を少なくとも１回は実行する場合に使用する．

各言語の対応構文は以下の通りとする．

- C 形式 `for(初期化; 条件; 更新)` を持つのは C, C++, C#, Java, Go, PHP, JavaScript, TypeScript のみである．Rust は `for i in 0..n`，Kotlin は `for(i in 0 until n)`，Swift は `for i in 0..<n`，Ruby は `n.times` 又は `each`，Python は `for i in range(n)` を用いる（Ruby の `for` は使用しない）．
- `while` を持たないのは Go のみであり，`for 条件 { }` が対応する．
- `do-while` を持つのは C, C++, C#, Java, Kotlin, PHP, JavaScript, TypeScript のみである．Swift は `repeat { } while 条件`，Ruby は `begin ... end while 条件` を用いる（`begin` を伴わない修飾子 `while` は０回実行に為り do-while に為らない）．Go / Rust / Python は `for { }` / `loop { }` / `while True:` の末尾で条件を判定して `break` する形で表す．

```cpp
// for: 回数既知
for(int i = 0; i < n; ++i) Process(i);

// for: 範囲走査
for(const Item &x : Items) Process(x);

// while: 条件充足迄の繰返
while(a && b) {
	const int g = Gcd(a, b);
	a -= g;
	b -= g;
}

// while: 単一文
while(!(n % 2)) n /= 2;

// do-while: １回以上の実行を保証
do {
	Increment(s);
	++Step;
} while(next_permutation(s.begin(), s.end()));
```

### 波括弧

単一文の `for` / `while` / `do-while` も波括弧を省略する．`do-while` で波括弧を使用する場合，`while` は閉じ波括弧と同一行 (`} while(...);`) に配置する．単一文の `do-while` の場合，`while` は `do <文>;` の次行に配置し，同一行にはしない．

波括弧の省略は 3.2 の原則に従う言語 (C, C++, C#, Java, Kotlin, PHP, JavaScript, TypeScript) でのみ行う．Go / Rust / Swift は波括弧が必須であり，Ruby / Python はインデント又は `end` で本体を区切る為，何れも波括弧の省略は行わない．

```cpp
for(int i = 0; i < n; ++i) Process(i);
while(Queue.front() != Target) Queue.pop();
do ++i;
while(i < n);
```

### 条件式への集約

**対象**：C, C++, C#, Java, Kotlin, PHP, JavaScript, TypeScript（空本体 `;` と前置デクリメントを共に持つ言語）

条件式自体に副作用（インクリメント，代入等）を含める事でループ本体が不要に為る場合は，本体を省略して条件式のみで記述する．ループ本体に相当する処理を条件式に集約出来る場合は，優先的に此のパターンを使用する．

条件式への処理の移動も 1.3 に従う．下例は `Bottom` が非負整数であり，全ての添字が `Mask` の範囲内である事を前提とする．又，C# / Java / Kotlin は整数を真偽値として扱えない為，`while(Bottom > 0 && !Mask[--Bottom]);` の様に明示的な比較として記述する．

```cpp
// ❌ 冗長（上記で記述可能）
while(Bottom > 0) {
	--Bottom;
	if(Mask[Bottom]) break;
}

// ✅ 条件式に副作用を集約（空本体）
while(Bottom && !Mask[--Bottom]);

// ✅ 条件式と単一文に依る簡潔な記述
while(!(n % 2)) n /= 2;
```

### 多重ループ・ループ内条件分岐の連鎖

**対象**：3.2 で波括弧の省略が出来る言語 (C, C++, C#, Java, Kotlin, PHP, JavaScript, TypeScript)

多重ループ，及びループと `if` / `switch` の組合せは，波括弧を使わず同一行に連鎖して記述する．最終的な本体が単一文であれば波括弧は不要である．本体が複数文の場合は本体のみに波括弧を付ける．

連鎖自体の折返が不要な場合，中間のループや条件分岐には波括弧を付けない．最内ブロックの本体のみが複数行であっても，途中の制御文は同一行に連鎖出来る．ヘッダの折返が必要な場合，又は結合関係やスコープが変わる場合は，3.2 に従い必要に応じて中間の本体にも波括弧を付ける．

```cpp
// 単一文：全て同一行
for(int i = 0; i < h; ++i) for(int j = 0; j < w; ++j) Process(i, j);

// ループ + if + 単一文
for(int i = 0; i < n; ++i) if(s[i] == '#') ++Count;

// 多重ループ + if + 単一文
for(int i = 0; i < h; ++i) for(int j = 0; j < w; ++j) if(Grid[i][j] == '#') ++Count;

// if + ループ + 単一文
if(CookieCnt < LastCookieCnt) for(int j = 0; j < w; ++j) if(s[i][j] != s[i - 1][j]) return j;

// 本体が複数文：本体のみに波括弧
for(int i = 0; i < h; ++i) for(int j = 0; j < w; ++j) if(s[i][j] == '#') {
	Search(s, h, w, i, j);
	++Ans;
}

// ループ + switch（3.5 参照）
for(int i = 1; i <= Size; ++i) for(int j = 1; j <= Size; ++j) switch(Paths[i][j]) {
case 1:
	BuildWall(i, j);
	break;
default:
	BuildPath(i, j);
	break;
}
```

```cpp
// ❌ 中間の for に波括弧
for(int i = 0; i < h; ++i) {
	for(int j = 0; j < w; ++j) Process(i, j);
}

// ✅
for(int i = 0; i < h; ++i) for(int j = 0; j < w; ++j) Process(i, j);
```

## 3.7 goto・ラベル付 break (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript

多重ループからの脱出には，`goto` (C, C++, C#, Go, PHP)，ラベル付 `break` / `continue` (Java, Go, Rust, Kotlin, Swift, JavaScript, TypeScript)，及び PHP の `break <段数>;` の使用を許可する．但し，可能な限りループ部分を関数に切り出して `return` で脱出する事に依り回避すべきである (SHOULD)．

各言語の記法は以下の通りとする．

- Java / Go / Swift / JavaScript / TypeScript: 独立行のラベル（Swift は `outerLoop:` の次行にループ）．
- Go: `goto` は変数宣言を飛び越せず，ブロック内へも飛び込めない為，多重ループ脱出にはラベル付 `break` を第一選択とする．
- Rust: `'outer: loop { ... break 'outer; }`．
- Kotlin: `OuterLoop@ for(...) { ... break@OuterLoop }`（ラベルはループと同一行に前置する）．
- PHP: ラベル付 `break` は無く `break 2;` の様に段数を指定する．PHP の `goto` はループや `switch` の内側へは飛び込めない．
- Java / Rust / Kotlin / Swift / JavaScript / TypeScript: `goto` は存在しない．

上記以外の用途（エラー処理・リソースの解放等）であっても，他の構文では資源漏洩・処理の重複を招く場合は使用を認める（全面禁止では無い）．ラベルのインデントは 2.6 に従う（周囲より１段浅く配置する）．

**例外 (C)**：C は自動的なリソース解放機構（デストラクタ・RAII・`defer`）を持たず，解放処理の重複記述かリソース漏洩の何れかを強いられる為，関数末尾に解放ラベルを置く goto クリーンアップを許可する．ラベルは獲得順の逆順に並べ，前方跳躍のみとする．

```cpp
// ✅ 関数化で対応（推奨）
bool Search(const Grid &Grid) {
	for(int i = 0; i < h; ++i) for(int j = 0; j < w; ++j) if(Grid[i][j] == Target) return true;
	return false;
}

// ✅ goto に依る多重ループ脱出（関数化が困難な場合のみ）
for(int i = 0; i < h; ++i) for(int j = 0; j < w; ++j) if(Grid[i][j] == Target) goto Found;

// 未発見時の処理
// ...
Found:
// 発見時の処理
```

```typescript
// ✅ ラベル付 break に依る多重ループ脱出（関数化が困難な場合のみ）
outerLoop:
	for(let i = 0; i < h; ++i) for(let j = 0; j < w; ++j) if(Grid[i][j] === target) break outerLoop;
```

## 3.8 Ruby 制御構文 (MUST)

**対象**：Ruby

Ruby の制御構文の書式を以下に定める．三項演算子は 4.10 に従う．

### 修飾子形式

`if`, `unless`, `while`, `until` の本体が単一文であり，且つ else 節を伴わない場合は修飾子形式（`<式> if <条件>`，`<式> unless <条件>`，`<式> while <条件>`，`<式> until <条件>`）を用いる．通常形式（`if <条件> ... end` 等）は冗長である為禁止する．

条件内の代入や正規表現の捕捉が本体のローカル変数認識に影響を与える場合，値文脈，及び後判定の `begin ... end while/until` は原形を保つ（例：`use(value) if (value = fetch)` は通常形式と解析時の束縛順が異なる）．

```ruby
# ❌ 単一文本体の冗長な通常形式
if cond
	return
end

# ✅ 修飾子（本体単一文，else 無）
return if cond
puts "warn" unless valid?
i += 1 while i < arr.size
process until done?
```

### １行統合形

else 節を伴う `if`, `unless` や `case` は，全節の本体が単一文であれば１行に統合して記述する．行長制限を超える場合の改行は 2.3 に従い，１行統合形式を維持した儘改行する．通常形式（`when`, `elsif` の前で改行した上で，本体を別行に展開する複数行形式）には展開しない．

```ruby
# ✅ 全節単一文 → １行統合
if x < 0 then -1 elsif x.zero? then 0 elsif x < 10 then 1 else 2 end

case x when 0 then "zero" when 1 then "one" else "other" end

# ✅ 行長超過時の改行 (2.3)
case status when :ok then perform_success_action_with_long_name(payload)
when :err then perform_error_action_with_long_name(payload) else perform_default_action_with_long_name(payload) end
```

### 単一文本体と複数文本体の混合

複数文本体の節のみ本体部を改行して展開する．単一文本体の節は次の節境界キーワードと同行の儘１行統合形式を維持する．`if` と `case` (`when`) の何れも同様とする．

`case ... in`（パターンマッチ）は，対象式と最初の `in` を同一行に配置すると `x in 1` が１行パターンマッチ式と解釈され，`case` の対象式を消費してしまう為，１行統合形式の対象外とする．`case <式>` を単独行とし，各節を `in <パターン> then <式>` の１行形式で並べる．

```ruby
# ✅ case/in は対象式の後で改行する
case x
in 1 then :a
in 2 then :b
else :c
end
```

```ruby
# ✅ if-elsif-else の混合（elsif 節のみ複数文）
if cond1 then a elsif cond2
	b1
	b2
else c end

# ✅ case-when の混合（when :b 節のみ複数文）
case x when :a then short_a when :b
	long_b_1
	long_b_2
else short_else end
```

---

# 第４章 式・演算

## 4.1 順序 (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python

以下の規則は記載順に優先度が高い．項の並替は 1.3 の意味保存を前提とし，減算・除算を交換可能として扱ってはならない．尚，以下のコード例は中間値も型の範囲内に収まる整数を前提とする．

1. 加算／減算では次数の高い（変数の数が多い）項を先に配置する．
2. 加算／減算では加算を先に配置する．
3. 乗算／除算の定数と変数では定数を先に配置する．
4. 乗算／除算では乗算を先に配置する．

**例外**：上記の規則に依って計算コストが増大する場合は此の限りではない（効率を優先する）．例えば，並替に依って短絡評価が崩れる場合，計算負荷の高い項が安価なガード条件（早期に偽と為る条件）より前に出る場合，共通部分式の再利用や強度低減が妨げられる場合等が該当する．

```cpp
// ❌
z = 1 - 3 * y + x * x;

// ✅
z = x * x - 3 * y + 1;
```

```cpp
// ✅ ルール通りなら -x + 1 だが，単項マイナス（符号反転）が増える為其の儘にする
y = 1 - x;
```

## 4.2 式の最適化 (SHOULD)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python

式は変数の出現回数や演算子の数が少なく為る様に最適化する．同じ変数が複数回出現する場合は，因数分解や共通項の括出を行う．因数分解を行う際も 1.3 の意味保存に従う．尚，以下のコード例は中間値も型の範囲内に収まる整数を前提とする．

```cpp
// ❌ 同じ変数が重複
z = a * x + x;
y = x * x - 2 * x + 1;

// ✅ 最適化
z = (a + 1) * x;
y = (x - 1) * (x - 1);
```

多項式は仕様上の許容誤差を確認した上でホーナー法（ネストした乗算）を優先する．乗算回数は削減出来るが，展開形と丸め結果が一致する事や精度の向上は保証されない．

```cpp
// ❌ 許容誤差を確認した上で削減出来る展開形（乗算６回）
float y = a * x * x * x + b * x * x + c * x + d;

// ✅ ホーナー法（乗算３回）
float y = x * (x * (x * a + b) + c) + d;
```

融合積和演算 (FMA) は，１回の丸めが仕様上許容され，且つ対象環境で効果を確認出来た場合に優先する．個別の乗算・加算とは計算結果が異なり，常に単一命令に為るとは限らず，高速化されるとも限らない．

```cpp
// ✅ FMA に依るホーナー法
const double y = std::fma(x, std::fma(x, std::fma(x, a, b), c), d);
```

## 4.3 シフト演算と逆数乗算 (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python

整数の２の冪乗に依る乗算・除算はビットシフトで置換える．但し，元の演算と動作が異なる場合はシフト演算に置換せず，`*` や `/` の儘残す．

- 除算 `/` → `>>`：ゼロ方向に丸める整数除算 (C / C++ / C# / Java / Go / Rust / Kotlin / Swift) では，負の値に於いて `>>`（負の無限大方向への丸め）と結果が異なる為（例：`-7 / 4 == -1` に対し `-7 >> 2 == -2`），unsigned 又は非負である事が保証されて居る場合にのみ置換する．切下除算を行う Ruby (`/`) と Python (`//`) は `>>` と結果が一致する為置換出来る．PHP / JavaScript / TypeScript の `/` は浮動小数点除算であり整数除算ではない為対象外とする（PHP の整数除算は `intdiv()` を使用する）．
- 乗算 `*` → `<<`：計算結果が型の範囲に収まる限り `× 2^n` と同値である（負値に対する `<<` が定義されて居るのは C++20 以降及び C 以外の対象言語であり，C 及び C++17 以前では未定義動作と為る）．固定幅の型でオーバーフローする可能性が有る場合は置換しない．C / C++ に於ける符号付乗算のオーバーフローは未定義動作である．PHP の整数演算は溢れると `float` へ昇格するが，`<<` はラップアラウンド（桁溢れで巡回）する．Rust はデバッグビルドに於いて `*` はオーバーフローでパニックし，`<<` はシフトするビット数が型幅以上の場合にのみパニックする．JavaScript の `<<` は結果を ３２ビット整数へ切詰める．

浮動小数の逆数乗算化は，対象型・値域に於いて丸め結果と例外発生が一致する場合に限る．１０進の有限小数であると言う理由だけでは置換しない．誤差を許容する最適化に就いては，仕様上の許容範囲と効果を別途検証する．

**例外 (Ruby, Python)**：被演算子の型が静的に定まらない為，両辺が整数リテラル又は整数と確定して居る変数の場合に限り置換する．`*` や `<<` が多重定義されて居る型（String, Array 等）には適用しない（Ruby に於いて `"ab" * 2` は `"abab"` と為り，`"ab" << 1` は文字コード 1 の追加と為る）．

**例外 (Java)**：符号無整数型が存在しない為，符号無としての右シフトが必要な場合は `>>>` を用いる．

**例外 (Kotlin)**：ビット演算子の記号が無く，中置関数 `shl` / `shr` / `ushr` / `and` / `or` / `xor` 及び `inv()` を用いる．此等の中置関数は全て同一の優先順位であり，乗除・加減の演算子よりも低い為，`a and b shl 2` は `(a and b) shl 2` と解釈される．意図する結合を括弧で明示する必要が有り，此の括弧は 4.4 の冗長な括弧には該当しない．

```cpp
// ❌ (unsigned)
unsigned a = x * 8;
unsigned b = y / 4;

// ✅ (unsigned)
unsigned a = x << 3; // ×8
unsigned b = y >> 2; // ÷4

// ✅ 符号付除算で負を取り得る場合はシフト置換しない（/ と >> で丸め方向が異なる）
int c = z / 4;
```

```cpp
// ❌ ２進浮動小数で結果同値を確認済の場合
float a = x / 2.0F;

// ✅
float a = 0.5F * x; // 1/2 は２進でも正確
float b = y / 5.0F; // 1/5 は２進で正確に表せず，0.2F 乗算へ置換しない

// ※ 1/3 = 0.333... は無限小数の為，分数の儘残す
float c = 1.0F / 3.0F * x;
```

## 4.4 括弧 (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python

冗長な括弧は付けてはならない．演算子の優先順位に従い，必要な場合にのみ括弧を使用する．但し，以下に示す括弧は冗長ではない為，除去してはならない．

### A. 言語仕様上必須（括弧が無いと構文誤りに為る）

- **Go, Rust**: `if` / `for` / `switch`（Rust は `if` / `while` / `for` / `match`）のヘッダ部に現れる複合リテラル・構造体リテラル `T{...}`．
- **Rust**: 単一要素タプル `(x,)` 及びタプル型 `(T,)`（2.14 参照）．
- **PHP**: 三項演算子のネスト（PHP 8 以降，括弧無のネストは解析時の致命的エラーと為る．4.10 参照）．
- **JavaScript, TypeScript**: 即時実行関数式 `(() => { ... })()` の外側の括弧．
- **JavaScript, TypeScript**: `??` と `||` / `&&` を混在させる式．`a ?? (b || c)` / `(a || b) ?? c` の何れも括弧無は構文エラーと為る．
- **JavaScript, TypeScript**: 単項演算子を伴う冪乗 `(-a) ** b`（`-a ** b` は構文エラーと為る）．
- **JavaScript, TypeScript**: 文の先頭に置くオブジェクト分割代入 `({ a } = Obj);`（括弧が無いと波括弧がブロックと解釈され，構文エラーに為る）．
- **JavaScript, TypeScript**: 要素が２つ以上有るか，又は識別子以外のキーを持つオブジェクトリテラルを返すアロー関数 `() => ({ a: 1, b: 2 })` / `() => ({ "a": 1 })`（括弧が無いと波括弧がブロックと解釈され，`,` 又は `"a"` の位置で構文エラーに為る）．
- **Ruby**: 引数リストに置く `and` / `or`（`g(true and false)` は構文エラーと為る．`not` は引数リストでも括弧無で書ける為本項に含めない）．
- **Ruby**: 条件節に置く修飾子 `rescue`（`elsif(t.zone rescue false)` の括弧を外すと構文エラーに為る．代入の右辺では `=` より強く結合する為括弧は不要である〈`x = f() rescue -1`〉．6.8 参照）．
- **Python**: 行を跨ぐ式を囲む括弧（2.3 参照）．
- **Python**: 式文として記述する代入式（ウォルラス演算子）を囲む括弧 `(x := f())`（括弧の無い単独の `x := f()` は式文として構文エラーに為る．`if x := f():` の様に条件式単独で置く場合は括弧は不要である．5.2 参照）．

尚，`sizeof` の括弧 (2.12) は式に対しては文法上任意であるが，本規約で省略を禁止して居る為冗長な括弧には該当しない．

### B. 括弧を外すと意味が変わる

- **C, C++**: ポインタのメンバアクセス `(*Ptr).Member`（`*Ptr.Member` は `*(Ptr.Member)` と解釈される．C++ では `->` を用いる事で本項を回避出来る）．
- **C#, Java, Kotlin, TypeScript**: キャスト・型表明の結果に対してメンバアクセスする際の括弧 `((String)Obj).Length` (C#)・`((String)Obj).length()` (Java)・`(x as Foo).bar` (Kotlin, TypeScript)（括弧が無いとキャストが呼出結果に掛かる．Kotlin と TypeScript の `x as Foo.bar` は `Foo.bar` が修飾型名と解釈される）．
- **C#, PHP**: `??` と `||` / `&&` を混在させる式．`??` の方が優先順位が低い為，括弧が必要なのは `(a ?? b) || c` の側である．
- **Go**: ポインタ型・チャネル型への型変換 `(*T)(p)` / `(<-chan int)(ch)`（括弧の無い `*T(p)` は `*(T(p))` と解釈されて型変換にならず，型検査エラーに為る．`<-chan int(ch)` は `<-(chan int(ch))` と解釈され，型変換ではなく受信式に為って型が要素型へと変化する）．
- **Go, Swift**: シフト演算と加減算を混在させる式．`<<` / `>>` が `+` / `-` より優先順位が高く，`a << (b + c)` の括弧を外すと `(a << b) + c` と解釈される（`1 << 2 + 3` は 7）．
- **Rust**: 範囲式に対してメソッドを呼び出す際の括弧 `(1..5).map(...)`（括弧が無いと `1..(5.map(...))` と解釈される）．
- **Kotlin**: 中置関数（`shl` / `and` 等）を混在させる式．中置関数は全て同一の優先順位であり加減算より低い（4.3 参照）．
- **Kotlin**: エルビス演算子 `?:` と `||` / `&&` を混在させる式．`?:` の方が優先順位が高く，`a ?: (b || c)` の括弧を外すと `(a ?: b) || c` と解釈される．
- **Swift**: `??` と `||` / `&&` を混在させる式．`??` の方が優先順位が高く，`a ?? (b || c)` の括弧を外すと `(a ?? b) || c` と解釈される．
- **PHP**: `and` / `or` / `xor` を囲む括弧．何れも代入演算子より優先順位が低く，`$x = ($a and $b)` の括弧を外すと `($x = $a) and $b` と解釈される．論理演算には `&&` / `||` を用いる事で本項を回避出来る（6.9 参照）．
- **JavaScript, TypeScript**: 空，又は要素が１つで識別子キーを持つオブジェクトリテラルを返すアロー関数 `() => ({})` / `() => ({ a: 1 })`（括弧の無い `() => {}` は空ブロックと為り，`() => { a: 1 }` は `a:` をラベル付文とする妥当な構文と為る為，何れも構文エラーにはならず戻り値が `undefined` に変化する．プロパティ短縮記法 `() => { a }` も同様である）．
- **Ruby**: `and` / `or` / `not` 及び修飾子 `if` / `unless` / `while` / `until` を囲む括弧．何れも代入演算子より優先順位が低く，`x = (a and b)` の括弧を外すと `(x = a) and b` と解釈される．

## 4.5 クランプ (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python

言語にクランプ関数が用意されて居る場合は其れを使用する（C++ の `std::clamp` 等）．クランプ関数が無い場合は `min(max(値, 下限), 上限)` の順で記述する．`max(上限, min(値, 下限))` 等の逆順での記述は禁止する．

上下限の指定順序，比較対象の型，NaN の扱いを確認し，下限が上限以下である事等の前提条件を満たす必要が有る．`std::clamp` の返却参照を保持する場合は，参照先オブジェクトの寿命も確保する．前提条件が異なる `min` / `max` の組合せを機械的に置換してはならない．

```typescript
Math.min(Math.max(value, lower), upper);
```

## 4.6 0 判定 (MUST)

**対象**：全言語共通

元の比較と真偽判定が同値である場合，0 との比較は論理演算子を使用する．`== 0` / `!= 0`，及び非負整数型 (`size_t` / `uint*_t` / `unsigned`) や `signed` でも論理的に非負である事が明らかな変数の `> 0` 等，明示的な 0 比較は禁止する．

**例外**：以下の場合は明示的な比較を許可する．

- 0 と `null` / `undefined` / `nil` の区別が必要な場合の `== 0` / `!= 0`
- `-1` / `0` / `+1` を返す関数等，`-1` を番兵値として使用する符号付変数の `> 0`

**例外 (C#, Java, Go, Rust, Kotlin, Swift)**：整数型から真偽値型への暗黙の型変換が無く，条件式は真偽値のみを受理する為，`== 0` / `!= 0` が唯一の記法である．真偽値変数其の物の判定にのみ `if(Flag)` / `if(!Flag)` を用いる．

**例外 (CSS, SCSS)**：`0` が真値であり偽値は `false` と `null` のみである為，`@if $count != 0` を `@if $count` へ書換えると判定が反転する．数値の 0 判定には `!= 0` を用いる．

**例外 (PHP)**：偽値判定が `0` 以外に `0.0` / `""` / `"0"` / `[]` / `null` / `false` を含み `=== 0` と一致しない為，4.12 の厳密比較を優先し `=== 0` / `!== 0` を用いる．真偽値変数のみ `if($flag)` を許可する．

**例外 (Ruby)**：`0` / `""` / `[]` が何れも真値であり，偽値は `nil` と `false` のみである為，`Count != 0` を `Count` へ書換えると判定が反転する．数値の 0 判定には `.zero?` 又は `!= 0` を用いる．

**注意 (Python)**：`0` / `0.0` / `""` / `[]` / `{}` / `None` が全て偽値と為る為，数値である事が確実な変数にのみ論理演算子への書換を適用する．`None` と 0 を区別する場合は `is None` / `is not None` を用いる．

**注意 (C++, JavaScript, TypeScript, Python)**：独自型の真偽値変換や NaN の存在を考慮する．JS / TS に於いて `NaN !== 0` と `!!NaN` は非等価である為，数値型である事だけでなく値域も確認する．

```cpp
// ❌
if(Count == 0) return;
if(Count != 0) Process(Count);
if(Count > 0) Process(Count); // 非負整数型の > 0
size_t Idx = 5;
while(Idx > 0) --Idx;

// ✅
if(!Count) return;
if(Count) Process(Count);
while(Idx) --Idx;
const int Side = SplitSide(Token); // -1/0/+1 番兵値
if(Side > 0) ProcessLeading();
```

## 4.7 bool リテラル (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python

真偽値型 (`bool` / `boolean` / `Boolean`) の値には `true` / `false`（Python は `True` / `False`）を使用する．`0` や `1` で代用してはならない．C 言語は C23 で `bool` / `true` / `false` が予約語と為った為対象に含める．C17 以前では `<stdbool.h>` をインクルードした上で使用する．

整数型のフラグや外部インタフェース定義の型は変更してはならない．Python の `flag: bool = 1` も実体は整数であり，型注釈の記述のみを理由として `True` へ変換してはならない．尚，以下のコード例は契約上も真偽値型である場合を前提とする．

```cpp
// ❌
int IsReady = 1, Done = 0;

// ✅
bool IsReady = true, Done = false;
```

## 4.8 bool 反転 (MUST)

**対象**：全言語共通（C は `<stdbool.h>` 取込済又は C23）

真偽値（bool 値）の反転には，`!` 演算子に依る再代入ではなく排他的論理和代入 `^= true`（Python は `^= True`）を使用する．組込の真偽値型に限り適用し，演算子の多重定義，プロキシオブジェクト，プロパティアクセサ等の意味論を変更してはならない．尚，`^=` 自体にスレッド安全な原子性は無い．

**例外 (Go, Kotlin, Swift)**：真偽値の複合代入 `^=` を持たない為，`Flag = !Flag` と記述する．

**例外 (PHP, JavaScript, TypeScript)**：`^` が被演算子を数値へ変換し結果が整数と為る為（PHP の論理 XOR は `xor` キーワード），`Flag = !Flag` と記述する．

**例外 (CSS, SCSS)**：複合代入演算子及び排他的論理和演算子を持たない為，`$flag: not $flag;` と記述する．

```cpp
// ❌
Flag = !Flag;

// ✅
Flag ^= true;
```

## 4.9 インクリメント・デクリメント (MUST)

**対象**：全言語共通

単項の増減演算には原則として前置演算子を使用する．整数型・浮動小数点型の変数に於いて `+= 1` / `-= 1` を使用する事は冗長である為禁止する．後置は戻り値（インクリメント／デクリメント前の値）が必要な場合のみ使用する．

**例外 (Rust, Swift, Ruby, Python)**：`++` / `--` を持たない為，`+= 1` / `-= 1` を用いる．

**例外 (CSS, SCSS)**：増減演算子と複合代入を持たない為，`$i: $i + 1;` と記述する．

**例外 (Go)**：`i++` が文であり前置形が存在しない為，`i++` を用いる．

**例外**：文字列やコレクション等の連結・追加目的で `+=` を使用する場合は本規則の対象外とする．此等は前置 `++` で代替出来ない為，其の儘 `+=` を使用する．

**注意 (PHP, JavaScript, TypeScript)**：変数の型が静的に定まらない場合は，`+= 1` から `++` への機械的な変換を行ってはならない（PHP の `++` は文字列にも作用し `'az'++` は `'ba'` に為る．JavaScript 及び TypeScript の `+= 1` は文字列では連結に為る）．変数が確実に数値であると判明して居る場合にのみ適用する．

```cpp
// ❌
i++;
i += 1;
Count -= 1;

// ✅
++i;
--Count;
```

```cpp
Array[i++] = Value; // 現在の i の位置に代入してからインクリメント
```

```javascript
// ✅ 文字列連結（数値ではない為対象外）
let Log = "";
Log += "header\n";
```

## 4.10 三項演算子 (MUST)

**対象**：C, C++, C#, Java, Swift, PHP, JavaScript, TypeScript, Ruby, Python

変数への代入を伴う条件分岐では，`if-else` 文で分岐させるのではなく三項演算子を用いて変数宣言と代入を一体化する．但し，両分岐に於ける型，暗黙の型変換，副作用，代入対象の評価回数を同一に保持出来る場合に限る．

１行の文字数が規約の行長制限（2.3: １２８文字）を超える場合に限り改行する．条件が連鎖する場合（`else if` 相当）も含め，改行位置は 2.3 に従い，`?` 及び `:` は前行の末尾に配置する．

**例外 (PHP)**：三項演算子のネストには括弧が必須である（PHP 8 以降，括弧無のネストは構文解析時に致命的エラーと為る）．`$a > $b ? "A" : ($a < $b ? "B" : "Draw")` の様に記述し，此の括弧は 4.4 の冗長な括弧には該当しない．

**例外 (Python)**：記号に依る三項演算子が存在しない為，条件式 `<真値> if <条件> else <偽値>` を用いる．

```cpp
// ✅ 単純な選択
int Sign = x > 0 ? 1 : -1;
std::cout << (n == 1 ? "Yes" : "No") << std::endl;

// ✅ チェーン（else if 相当）
std::cout << (a > b ? "A" : a < b ? "B" : "Draw") << std::endl;
```

```typescript
// ✅ 条件連鎖（else if 相当）
const result = condition > 10 ?
primaryResultValue :
condition > 6 ? secondaryResultValue : condition > 2 ? tertiaryResultValue : defaultResultValue;
```

```cpp
// ❌
std::string Result;
if(Score > 59) Result = "合格";
else Result = "不合格";

// ✅
std::string Result = Score > 59 ? "合格" : "不合格";
```

## 4.11 比較演算子の選択 (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python

`>=` / `<=` よりも `>` / `<` を優先的に使用する．境界値を調整する事で `>=` / `<=` を `>` / `<` に置換えれる場合は置換える．

**例外**：以下の場合は `>=` / `<=` を使用する．

- `i <= n` を `i < n + 1` に書換える等，余計な計算コストが発生する場合．
- 意味の有るリテラル（文字コード境界や列挙値等）に依る境界判定を行う場合（例：`c >= 'a' && c <= 'z'` に於ける `'a'` や `'z'` は文字コード隣接の境界其の物を表す為，`>=` / `<=` を使用する）．
- 値が整数である事を型や文脈から保証出来ない場合（PHP / JavaScript / Ruby / Python 等で浮動小数点数値を取り得る変数等）．例えば `59.5 >= 60` は偽であるが `59.5 > 59` は真と為り判定が変化する為，境界値の調整を行ってはならない．
- 境界値の加減算に依ってオーバーフロー，アンダーフロー，又は整数精度の喪失が発生する場合，或いは多重定義された比較演算の等価性が確認出来ない場合．
- 尚，Kotlin に於ける範囲反復では `0..n - 1` ではなく `0 until n` を用いる事で本規則を満たす物とする．

```cpp
// ❌
if(Score >= 60) return "合格";
for(int i = 0; i <= n - 1; ++i) Process(i);

// ✅
if(Score > 59) return "合格";
for(int i = 0; i < n; ++i) Process(i);

// ✅ 余計な計算が発生する為 <= を使用
for(int i = 0; i <= n; ++i) Process(i);
```

## 4.12 比較演算子の使分 (MUST)

**対象**：C#, Java, Kotlin, PHP, JavaScript, TypeScript, Python

等価比較演算子（`==` / `!=`）及び厳密等価比較演算子（`===` / `!==`）は，言語仕様及び文脈に応じて以下の様に使分ける（0 判定に就いては 4.6 を参照）．

C#: 値の比較には `Equals`，参照の同一性判定には `ReferenceEquals` を用いる．`==` は演算子の多重定義に依って型毎に動作が異なる為，`string` 等の値意味論が明確な型に限定して使用する．

Java: オブジェクトの内容比較には `equals`（`null` 安全な比較には `Objects.equals`）を用いる．`==` は参照の同一性判定，プリミティブ型，及び `enum` 定数の比較にのみ用いる．

Kotlin: 内容比較には `==`（内部で `equals` を呼び出す），参照の同一性判定には `===` を用いる．

PHP: `undefined` が存在せず，`==` に依る暗黙の型変換が意図しない一致を招く為，**常に `===` / `!==` を使用する**．型を揃えた上で厳密比較を行う事で，比較の意図を明確にする．尚，PHP の `switch` 文は緩い比較 (`==`) で分岐する為，厳密な同一性判定が必要な場合は `match` 式（PHP 8.0 以降）を用いる．

```php
// ❌ 型変換に依存する緩い比較
if($count == "0") return;

// ✅ 厳密比較（型変換を挟まず値の同一性のみで判定する）
if($code === 404) return;
if($value === null) return;
```

JavaScript, TypeScript:

- **数値同士の比較**：`==` / `!=`
- **`null` / `undefined` の個別判定**：`===` / `!==`
- **`typeof` に依る型判定**：`===` / `!==`
- **其の他の値**：`===` / `!==` を原則とし，異なる型の値を暗黙の型変換に依って一致させてはならない．尚，数値同士の `==` / `!=` は，両辺が同一の数値型である事を確認出来る場合に限る．`Number` と `BigInt` の比較，外部入力，型が未確定の値には厳密比較を用いる．

**例外**：`null` と `undefined` を区別せず両方を纏めて判定したい場合に限り，`==` / `!=` を使用する．

```tsx
// ❌ 数値に ===
if(index === -1) return null;

// ✅ 数値は ==
if(index == -1) return null;

// ✅ null/undefined を個別判定
if(value === null) return;
if(value !== undefined) Process(value);

// ✅ typeof は ===
if(typeof item === "string") Process(item);

// ❌ null と undefined を区別しない場合に ===
if(value === null || value === undefined) return;

// ✅ null と undefined を区別しない場合は ==
if(value == null) return;
```

Python: `None` / `True` / `False` との比較には `is` / `is not` を用いる（`==` は `__eq__` の多重定義に依って挙動が偽装され得る）．値の等価判定には `==` を用い，整数や文字列の内部キャッシュに依存した `is` 比較は行ってはならない．

---

# 第５章 変数・型

## 5.1 変数化 (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python

一度しか使用しない値は変数化せず，直接代入する．

**注意**：代入直後の return は，局所変数の型に依る変換・丸め・寿命・コピー省略を保持出来る場合に直接 return する．例えば `float` の局所変数を経由する `double` の返却は，変数化を省略すると丸め処理が変わり得る．

**例外**：以下の場合は変数化する．

- 同じ値を複数回使用する場合
- 式が非常に長く，可読性が著しく低下する場合（目安：６４文字超）
- マジックナンバーに意味の有る名前を付ける場合

**例外 (Go)**：多値返却の受取 (`value, resultError := f()`) は言語構造上変数化が避けれない為，対象外とする．

```cpp
// ❌
int Total = CalcTotal(Items);
SetTotal(Total);
const int Result = Calculate(x);
return Result;

// ✅
SetTotal(CalcTotal(Items));
return Calculate(x);
```

## 5.2 制御文での即時束縛 (MUST)

**対象**：C++17 以降，C#, Java, Go, Rust, Kotlin, Swift, Python

代入と判定を同時に行う場合は，初期化文等を用いて変数の束縛箇所を判定に近付け，構文上可能な場合はスコープも限定する．型を記述出来ない束縛形式（Go の `:=`，Rust の `if let` 等）は，5.4 の規定よりも本節を優先する．

後続の処理で値又は寿命が必要な場合は初期化文へ移動させず，ロック・資源・借用の終了時点も保持する．Python の代入式は独立したスコープを生成せず，Swift の `guard let` は後続スコープへ束縛を公開する点に注意する．

- C++17: `if(init; cond)`, `switch(init; cond)`
- C#: パターンマッチに依る束縛 `if(expr is Type name)`
- Java: パターン変数 `if(expr instanceof Type name)`（Java 16 以降）
- Go: `if cond := expr; cond { ... }`, `switch x := expr; x { ... }`
- Rust: `if let Pat = expr { ... }`（束縛と条件の同時記述）
- Kotlin: `when(val x = expr) { ... }`, `x?.let { ... }`, `if(x != null)` に依るスマートキャスト
- Swift: `if let x = expr { ... }`, `guard let x = expr else { ... }`
- Python: `if (x := expr) is not None:`（ウォルラス演算子）

```cpp
// ❌ 不必要に広いスコープ
const int Result = Compute();
if(Result > 0) Process(Result);

// ✅ 初期化文でスコープ限定
if(const int Result = Compute(); Result > 0) Process(Result);
```

```cpp
// ❌ 一時変数を外側に定義
const auto Iter = Map.find(Key);
if(Iter != Map.end()) Use(Iter->second);

// ✅ 初期化文
if(const auto Iter = Map.find(Key); Iter != Map.end()) Use(Iter->second);
```

```python
# ❌
ptr = connection_map.get(pos)
if ptr is not None:
	return ptr

# ✅
if (ptr := connection_map.get(pos)) is not None:
	return ptr

# ✅ 内包表記内での使用
results = [y for x in data if (y := transform(x)) is not None]
```

## 5.3 同一型の一括宣言 (SHOULD)

**対象**：C, C++, C#, Java, Go

似た性質を持つ同一型の変数・定数は，カンマ区切で纏めて宣言する．**対象は関数ローカルの宣言に限る**（ファイルスコープの変数やクラスのメンバは，宣言毎にコメント・アクセス修飾子・意味の分類が異なり，纏めると其れ等が失われる為１宣言１行とする）．

```cpp
void Prepare() {
	int n, m, Ans = 0;
	float x, y, z;
	constexpr int Width = 32, Height = 16;
	int a, *b, &c = a; // ポインタ・参照の混在も可（`&c` は参照型を持つ C++ 限定）
	// 終了
	return;
}
```

## 5.4 auto / var / any の使用制限 (MUST)

**対象**：全言語共通

型推論キーワード（C++ の `auto`，C# や Java の `var` 等）は原則として使用せず，型を明示的に記述する．変数宣言時に値を代入しない場合は，型指定が任意の言語であっても必ず型を明記する．関数の戻り値の型は，型推論が可能な場合であっても省略せず必ず明示する（例：`: JSX.Element`, `: Promise<void>`, `: string`）．

**注意 (Kotlin, Swift)**：`var` / `val` / `let` は可変性の宣言子であり型推論キーワードではない為，本節の禁止対象ではない（対象は型注釈の省略）．Kotlin の戻り値型 `Unit` は省略する．

**注意 (C)**：C17 以前の `auto` は記憶域クラス指定子であり型推論ではない．C23 以降の型推論に用いる `auto` は本節の型明示規則に従う．

**注意 (Java)**：`var` は Java 10 以降の局所変数・拡張 for 文の変数・try-with-resources の資源，Java 11 以降のラムダ引数で使用出来る．フィールド・引数・戻り値には記述出来ない為，「戻り値の型を必ず明示」の規則とは衝突しない．

**例外**：ラムダ式の格納，テンプレートメタプログラミングの戻り値等，型名の記述が不可能又は著しく冗長な場合（目安：６４文字超）に限り許可する．

**例外 (JavaScript, Ruby)**：型注釈構文を持たない為，型明示の規定は適用しない．Ruby で型情報が必要な場合は `sig/*.rbs` に記述する（`def f(x: Integer)` は型注釈ではなくキーワード引数の既定値である）．

**例外 (JSON, HTML, CSS)**：変数宣言と型注釈の構文を持たない為，本節の対象外とする．

**例外 (C#)**：匿名型 (`var x = new { A = 1 };`) は型名が存在せず `var` 以外で記述出来ない為許可する．

**例外 (Go)**：戻り値の型が非公開で型注釈を記述出来ない局所変数に限り `:=` を用いる．初期値を伴わない宣言は `var <名前> <型>` で型を明記する．

**例外 (Rust)**：型を名前で記述出来ないクロージャ・`impl Trait`・イテレータ連鎖は局所的な型注釈を省略する．名前付関数とクロージャを区別し，後者で推論可能な引数型等を言語が常に要求して居るとは解釈しない．

**例外 (Swift)**：名前で記述出来ない不透明な具体型は局所的な型注釈を省略する．クロージャには `(Int) -> Int` 等の関数型を記述出来る為，匿名である事のみを省略の理由としない．

**例外 (PHP)**：局所変数に型宣言を記述出来ない為，型は引数・戻り値・プロパティ・クラス定数に付与する（6.10 参照）．局所変数の型は変数名と代入式で自明にし，必要に応じて PHPDoc の `@var` で補う．

**例外 (Python)**：型注釈のみの `x: int` は変数を束縛せず，参照時に `UnboundLocalError` と為る為，宣言の代用としない．初期値が定まらない場合は `x: int | None = None` とする．局所変数は 6.10 の自明な型を省略する規則を優先する．

**注意**：TypeScript の `any` は型安全性を放棄する為，`auto` とは異なり一切使用禁止とする．`unknown` 又は具体型を使用する事．

```cpp
// ❌
auto Name = User.GetName();

// ✅
std::string Name = User.GetName();
```

```tsx
// ❌ 型省略
let count;
const getName = () => "hello";
function App() {
	return <div />;
}

// ✅ 型明記
let count: number;
const getName = (): string => "hello";
function App(): JSX.Element {
	return <div />;
}
```

## 5.5 整数型の符号選択 (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift

論理的に負の値を取らない整数変数（カウンタ・インデックス・ネストの深さ・サイズ・バイトオフセット等）は `int` ではなく **unsigned 整数型**で宣言する．各言語で使用する型名は次の通りとする．

符号無整数であっても，減算・逆順走査・サイズ計算に因ってアンダーフロー（巻戻）が発生し得る．「非負である」と言う意図のみで安全と判断せず，値の範囲を確認する．Go の `uintptr` は一般的な計数ではなくアドレス表現の用途に限る．

- C, C++: `size_t` / `uint8_t` / `uint16_t` / `uint32_t` / `uint64_t`.
- C#: `byte` / `ushort` / `uint` / `ulong`.
- Go: `uint` / `uint8` / `uint16` / `uint32` / `uint64` / `uintptr`.
- Rust: `u8` / `u16` / `u32` / `u64`. 添字と長さには `usize` を用いる（スライスの添字は `usize` のみを受け付ける）．
- Kotlin: `UByte` / `UShort` / `UInt` / `ULong`.
- Swift: `UInt` / `UInt8` / `UInt16` / `UInt32` / `UInt64`.

**例外**：以下の何れかに該当する変数は符号付整数の使用を許可する．

- `-1` を番兵値として使用する場合
- `std::numeric_limits<int>::min/max` を番兵値として使用する場合
- 戻り値として `-1` / `0` / `+1` を返す関数の結果を受け取る場合
- 演算に因って一時的に負の値を取り得る場合
- `for(int i = 0; i < n; ++i)` 等の慣用的なループカウンタ（5.12 が定める小文字１文字変数）
- API やコレクションの添字型・差分型が符号付整数を要求する場合（Go の `len` は `int`，Kotlin の標準コレクション添字は `Int`，Swift の `Array` 添字は `Int`）．不要な型変換を挟まず，利用先の型契約に合わせる

**例外 (Java)**：`char` を除いて符号無整数型が言語仕様に存在しない為，`int` 又は `long` を用い，非負である事は変数名とドキュメントコメントで表現する．符号無としての演算が必要な箇所では `>>>`, `Integer.toUnsignedLong`, `Long.compareUnsigned` 等を用いる．

```cpp
// ❌
int ChangedCount = 0; // 単純な計数 → unsigned で十分
int NestDepth = 0; // 負に為らない深さ → unsigned
int ByteOffset = 100; // バイト位置 → unsigned

// ✅
size_t ChangedCount = 0;
uint32_t NestDepth = 0;
uint32_t ByteOffset = 100;
int LastRank = -1; // -1 番兵値
int BaseIndent = std::numeric_limits<int>::max(); // numeric_limits 番兵値
int Indent = SegIndent[LineIdx]; // 演算で一時的に負値に為り得るので int
if(IsCloseBracket) --Indent;
if(Indent < 0) Indent = 0;
```

## 5.6 引数無の void 省略 (MUST)

**対象**：C, C++

引数無の関数宣言及び定義では，C++ 及び C23 以降では `(void)` ではなく `()` を使用する．C17 以前では引数無を検査出来る `(void)` を使用する．

**注意**：上記の C++ の記法を C17 以前に適用してはならない．C17 以前に於いて `int GetCount();` の様な宣言は「引数未指定」を意味し，`int GetCount(void);` が持つ呼出時の引数型検査が機能しなく為る．

```cpp
// ❌
void Initialize(void);
int GetCount(void);

// ✅
void Initialize();
int GetCount();
```

```c
// ✅ C17 以前では引数無を明示
void Initialize(void);
int GetCount(void);
```

## 5.7 return の明示 (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python

### main 関数

**対象**：C, C++

`main` 関数の末尾には `return 0;` を必ず記述する．C++ 標準の仕様上は省略可能であるが，本規約では明示する．

```cpp
int main() {
	// ...
	return 0;
}
```

### 其の他の起動点

本節の其の他の対象言語に於けるプログラム起動点は，以下の通りとする．強制終了 API は `defer`，デストラクタ，各種終了処理が実行されない場合が有る為，正常な資源解放を完了させてからプロセスを終了する事．

- **C#, Java, Kotlin**: 起動点は `void` / `Unit` を返す（C# に於いて `static int Main()` を選択した場合のみ戻り値を返す）．異常終了は `Environment.Exit(1)` (C#) / `System.exit(1)` (Java) / `exitProcess(1)` (Kotlin) で表す．
- **Go, Rust**: `func main()` / `fn main()` は値を返さない．終了コードは `os.Exit(N)` / `std::process::exit(N)`，又は Rust の `fn main() -> Result<(), E>` で表す．
- **Swift**: 起動点はトップレベルコード (`main.swift`) 又は `@main` を付与した型の `static func main()` であり，戻り値を持たない．
- **PHP**: `main` 関数の概念が存在しない．
- **JavaScript, TypeScript, Ruby**: 言語仕様として値を返す `main` 関数を要求しない．実行環境が定める起動点に従い，終了コードと関数の戻り値を混同しない様にする．
- **Python**: `def main() -> int:` 及び `if __name__ == "__main__": sys.exit(main())` の形式とし，`return 0` は `main` 関数の末尾にのみ記述する．

### void 関数・コンストラクタ・デストラクタ

通常の void 関数，コンストラクタ，デストラクタの末尾が**到達可能**な場合は，使用言語が許容する返戻構文を用いて終端を明示する．C++ のデストラクタ，PHP の `__destruct`，Swift の `deinit`，C# のファイナライザ等は呼出契機や構文が異なる為区別して扱う．末尾の返戻の直前には，其の言語のコメント記法で「終了」と記述する．

末尾が**到達不能**な場合（無限ループ `for(;;)` / `while(true)`，全 `case` が `return` / `throw` / `abort` で終了する `switch` 文，全分岐が `return` / `throw` する `if-else` 文等）は，末尾の `return;` を**記述しない**．

C, C++, C#, Java, PHP, JavaScript, TypeScript は文脈上許容される関数本体で `return;` を用い，Go, Kotlin, Swift, Python は `return` を用いる．Ruby は後述の規定に従う．初期化ブロック，式本体，ジェネレータ，コルーチン等には通常の終端 return を挿入しない．

**例外**：本体が空の関数 (`void Foo() {}`) は，2.5 に従い波括弧を同一行に配置し，末尾の `return;` は記述しない．

**例外 (Rust)**：ブロック末尾の式が其の儘戻り値と為る為，末尾に `return` を記述せず末尾式で返す（`;` を付与すると戻り値が `()` に変化する為注意する事．2.15 参照）．

**例外 (Kotlin)**：式本体関数 (`fun Square(x: Int): Int = x * x`) の式本体には通常の `return` を記述出来ない為，対象外とする．

**例外 (Ruby)**：１行メソッド定義（`def 名前(引数) = 式`）は 6.12 に従い `return` を記述しない．

```cpp
void Initialize() {
	Setup();
	// 終了
	return;
}

Ball::Ball() {
	ModelHandle = MV1LoadModel("models\\ball.mv1");
	// 終了
	return;
}

// 到達不能 — 末尾 return; は書かない
[[noreturn]] void RunForever() {
	while(true) Tick();
}
```

### Ruby メソッド

Ruby は末尾式の評価値が暗黙の戻り値と為るが，呼出元が戻り値を利用するメソッドの末尾には `return` を明示する．副作用専用のメソッド（戻り値が利用されないセッター，入出力処理，ログ出力等）は `return` を記述しなくて良い．

メソッド本体の rescue 節で値を返す場合，rescue 節の最終式に `return` を明示する．ensure 節内での `return` は禁止する（rescue で捕捉した例外やメソッド本体の return 値を破棄してしまう為）．ブロック (`do ... end` / `{ ... }`) 内の `return` は，rescue 節内に限らず外側のメソッド自体を脱出する為，意図した動作でない限り記述しない．ブロックの値を返す目的には `next` を用いる（`lambda` 内の `return` は lambda のみを脱出する）．

```ruby
# ❌ ensure 内 return
def bad
	return api_call
ensure
	return :ensured
end

# ❌ イテレータブロック内 rescue で return（外側メソッドを脱出してしまう）
def process(items)
	return items.each do |item|
		do_work(item)
	rescue => e
		return :err
	end
end

# ✅ 末尾式に return を明示
def total(a, b)
	# 合計の返戻
	return a + b
end

# ✅ 副作用専用 — return 不要
def log_event(msg)
	puts msg
end

# ✅ rescue 節最終式に return を明示
def fetch
	# 取得結果の返戻
	return api_call
rescue => e
	# 代替値の返戻
	return default_value(e)
ensure
	cleanup
end
```

### 値を返す関数

値を返す `return` 文の直前には，`// 〜の返戻` の形式でコメントを付与する．早期リターンにも同様に適用する．コメント形式の詳細は 7.10 を参照する事．

**例外**：以下の場合は `// 〜の返戻` のコメントを省略して良い．

- switch 文内の各 case に置く `return`
- 関数全体が１〜２行で構成される単純な関数

```cpp
FIntPoint GetConnection(const FIntPoint &Position) {
	// マップに接続先が有る場合の返戻
	const FIntPoint *Ptr = ConnectionMap.Find(Position);
	if(Ptr) return *Ptr;
	// 接続先の導出
	FIntPoint Result = Derive(Position);
	// マップへの接続先追加と返戻
	return ConnectionMap.Add(Position, Result);
}

FVector2f GetMidPoint(const FVector2f &a, const FVector2f &b) {
	// 中点の返戻
	return 0.5F * (a + b);
}
```

## 5.8 const 優先 (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python

宣言時に値が確定する変数は，再代入可能な変数（`let` 等）ではなく定数 (`const`) として宣言し，宣言と同時に値を代入する．後から値を変更する必要が無い限り，全ての変数を `const` とする．

**対象範囲**：ローカル変数・関数引数・for ループの一時変数も対象とする．`bool`, `int`, `size_t`, `char`，参照型，ポインタ等の基本型であっても `const` を省略しない．ポインタ自体の不変と指す対象の不変は個別に判定し，指す対象を変更する場合は `T *const`，指す対象も変更しない場合は `const T *const` とする．Java では `final int Result = ...;` の形式でローカル変数・引数・拡張 for 文の変数に付与する．

Swift, JavaScript, TypeScript, Ruby, Python に於ける `let`, `const` や定数名規則は，其れ単体でコンパイル時評価を要求する構文ではない．又，不変束縛と深い不変性（ディープイミュータビリティ）は区別する．型照会・オーバーロード解決・ムーブ・`FnMut` の呼出・外部契約に影響が出る場合は，不変指定を機械的に付与・除去してはならない．

各言語に於ける定数宣言は以下の通りとする．

- C, C++: `const`（C++ の `constexpr` は const を含意する為，重複して `const` を付けない）．
- C#: `const` 又は `readonly`．
- Java: `final`.
- Go: `const`（コンパイル時定数のみ）．
- Rust: `let`（既定で不変）．
- Kotlin: `val`.
- Swift: `let`.
- PHP: クラス定数は `const`，プロパティは `readonly`（局所変数に不変指定は存在しない）．
- JavaScript, TypeScript: `const`.
- Ruby: 定数は大文字で宣言する（例：`MAX_NAMES = ["a", "b"].freeze`）．`freeze` は可変オブジェクト（String / Array / Hash 等）にのみ付与する（Integer / Symbol / `true` 等のリテラルは既定で凍結済であり `freeze` の呼出は不要である）．文字列リテラルはファイル先頭の `# frozen_string_literal: true` で凍結する．尚，Ruby の定数は再代入を完全には禁止せず，警告の出力に留まる点に留意する．
- Python: 慣習的に `UPPER_SNAKE_CASE` で定数を示す．

**対象範囲の例外**：

- **関数引数に不変指定を持たない言語 (C#, Go, PHP, JavaScript, TypeScript, Python)**：局所変数のみを対象とする．
- **C#**: `const` は局所変数にも使用出来るが定数式に限られる．実行時に確定する局所変数や値渡し引数に対する一般的な不変指定は存在しない．フィールドには `readonly` を優先する．`in` は参照渡しの意味も併せ持つ為，不変指定の代用として追加しない．
- **Java**: ラムダ式や匿名クラスがキャプチャする局所変数は `final` 又は実質的に final である必要が有る．明示可能な宣言には `final` を付与する．try-with-resources で宣言する資源は暗黙的に final と為る．
- **Go**: `const` はコンパイル時に確定するスカラ値（真偽値・数値・文字列・rune）のみを保持出来，関数呼出結果・スライス・マップ・構造体は保持出来ない為，`const` 化の対象外とする（宣言形式は 5.4 に従う）．関数引数やループ変数には不変指定が存在しない為対象外とする．
- **Rust**: 既定で不変である為，`let mut` の使用を最小限に留める事で本節を満たす．
- **Ruby**: メソッド内の局所変数は定数化出来ない為（`dynamic constant assignment` の構文エラー），対象外とする．定数化はクラス・モジュール直下の定義にのみ適用する．

```cpp
// ❌ 変数として宣言して後から代入
int Result;
if(Score > 59) Result = 80;
else Result = 40;

// ✅ const として宣言時に値を確定
const int Result = Score > 59 ? 80 : 40;
```

```typescript
// ❌
let name;
if(user.isAdmin) name = "管理者";
else name = user.displayName;

// ✅
const name = user.isAdmin ? "管理者" : user.displayName;
```

## 5.9 コンパイル時定数 (MUST)

**対象**：C（C23 以降），C++, C#, Java, Go, Rust, Kotlin, PHP

コンパイル時に値が確定する定数には，各言語のコンパイル時定数機能を使用する．実行時にしか確定しない値には通常の定数機能を使用する．

- C（C23 以降）：`constexpr`.
- C++: `constexpr`. クラス内の定数には `static constexpr` を用いる．
- C#: `const`（定数式のみ）．
- Java: クラス定数には `static final`，局所変数には `final` を用いる．コンパイル時定数と為るのは，プリミティブ型又は `String` を定数式で初期化する場合に限られる．
- Go: `const`．定数式で初期化出来る型及び値に限る．
- Rust: `const`. コンパイル時に評価可能な関数には `const fn` を用いる．
- Kotlin: `const val`. トップレベル，名前付 `object`，`companion object` の何れかに配置する（クラス本体の直下には記述出来ない）．型はプリミティブ型と `String` に限られる為，其れ以外の型は `object` 内の `val` を用いる．局所変数には指定出来ない為 `val` とする．クラス内定数は `companion object { const val CellCount = 16 }` とする．
- PHP: 使用バージョンが許容する定数式に `const` を用いる．定数式として記述出来ない評価処理を無理に定数化しない．

```cpp
// ❌
const int MaxCount = 100;

// ✅
constexpr int MaxCount = 100;

// クラス内
class Config {
private:

	static constexpr int CellCount = 16;
	static constexpr float CellScale = 100.0F;
};
```

## 5.10 遅延初期化・静的初期化 (SHOULD)

**対象**：C++, C#, Java, Kotlin, Swift, PHP, JavaScript, TypeScript

複雑な初期化ロジックが必要な静的メンバや定数には，各言語の遅延初期化機能又は即時実行パターンを使用する．

即時実行と遅延実行を混同しない事．初回の実行タイミング，例外発生時の再試行動作，再入性，複数スレッドからの初期化に関するスレッドセーフ保証を確認して適切な方式を選択する．

- C++: 即時実行ラムダ `[]() {...}()`．
- C#: `Lazy<T>` 又は静的コンストラクタ．
- Java: 静的初期化ブロック．
- Kotlin: `by lazy { ... }`（`lateinit var` の使用可否は 6.10 に従う）．
- Swift: 型の保持プロパティには `static let` / `static var` に依る遅延初期化を用いる．`lazy var` はインスタンスの保持プロパティ用であり，型プロパティとは区別して使用する．
- PHP: 局所 `static` 変数等で初回評価を管理する．`??=` は評価結果が非 null の場合に限る．結果が null と為り得る場合は初期化済の状態を別途管理する．
- JavaScript, TypeScript: 即時実行関数式 `(() => { ... })()`．

```cpp
class Chunk {
private:

	static inline const std::vector<int> Triangles = []() -> std::vector<int> {
		std::vector<int> Result;
		for(int x = 0; x < CellCount; ++x) for(int y = 0; y < CellCount; ++y) AppendTriangle(Result, x, y);
		return Result;
	}
	();
};
```

## 5.11 クラス設計 (MUST)

**対象**：C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python

### メンバ関数

型の内部処理は補助関数であってもメンバとして配置する．但し，ADL 用の `swap`，非メンバ演算子，外部コールバック，型其の物に属さない処理は除く．Kotlin の拡張関数等は 6.9 の規定に従う．

**注意 (Go, Rust)**：クラス構文の代わりに，型とレシーバ付関数 (Go) 又は `impl` ブロック (Rust) を用いる．型に属する処理は其れ等の中に記述し，型に紐付かない処理のみをパッケージ又はモジュールの直下に配置する．

### 宣言順序

下記 A 〜 E の詳細順序は **C++ を対象とする**（`friend`, `virtual`, `mutable`, `volatile`, `constexpr`，コピー／ムーブ代入演算子，メンバ関数の `const` 修飾等は C++ 固有の機能であり，他言語に対応する要素が存在しない項目が大半を占める為である）．他の言語は各言語に実在する構成要素のみを対象とし，次の簡約順序に従う．

- **C#**: 定数 → フィールド → コンストラクタ → プロパティ → メソッド → ネスト型
- **Java**: 定数 → `static` フィールド → インスタンスフィールド → `static` メソッド → コンストラクタ → インスタンスメソッド → ネスト型
- **Go**: 型定義 → 定数 → 変数 → コンストラクタ相当 (`NewXxx`) → 値レシーバのメソッド → ポインタレシーバのメソッド
- **Rust**: `impl` 内を関連定数 → 関連関数（`new` 等）→ `&self` → `&mut self` → `self` の順とし，trait 実装は固有 impl の後に配置する
- **Kotlin**: プロパティ → `init` ブロック → セカンダリコンストラクタ → メソッド → `companion object` → ネスト型
- **Swift**: `typealias` → ネスト型 → `static let` → `let` → `var` → `init` → `deinit` → メソッド → 演算子（`protected` は存在せず，2.7 の可視性順に従う）
- **PHP**: `use <Trait>` → 定数 → `static` プロパティ → プロパティ → `__construct` → `__destruct` → 其の他のマジックメソッド → メソッド（クラス内に型を宣言出来ない為，下記 B「ネスト型」は対象外）
- **JavaScript, TypeScript**: `static` フィールド → インスタンスフィールド → `constructor` → メソッド（TypeScript は `private` → `protected` → `public` の順を上位とする）
- **Ruby**: `include` / `extend` → 定数 → `attr_*` → `initialize` → 公開メソッド → `private` 以降の非公開メソッド（`private` は以降の全メソッドを非公開にする区画指定である為，非公開メソッド群を後ろに配置する）
- **Python**: クラス変数 → `__init__` → 非公開メソッド（`_` 接頭辞）→ 公開メソッド

以下の順序は，可視性と既存の意味を保持した上で適用する．C++ はアクセス指定子のセクション内で適用し，2.7 の其の他の対象言語は可視性順を上位・種別順を下位として並べる．Ruby は後述の公開メソッド → `private` 区画の順序を優先する．Go のエクスポート識別子等，可視性を変更する様な改名は行わない．

尚，初期化・破棄・メモリ配置・集成体初期化・外部契約に於いて順序が意味を持つ場合は其の順序を保持する．C++ のメンバ変数，Java / Kotlin / JavaScript の初期化処理，Python / Ruby のクラス本体の実行文等も其の対象とする．

以下の順序で宣言する．同じ順位内では，意味を保持出来る範囲で型・定数・変数を使用順に，関数を被呼出側（呼び出される側）から呼出側の順に配置する．相互再帰は一群として扱い，依存関係で一意に決まらない順序は最初の呼出元での初出順とする．異なる呼出元からの逆順要求を同時に満たす事は強制しない．必要な前方宣言は此れを認める．

#### A. クラス関係宣言（`private` セクション内）

1. `friend` 宣言

#### B. ネスト型

2. `using` / `typedef`（型エイリアス）
3. `enum` / `enum class`（列挙型）
4. `struct`（POD・データ集約体）
5. `class`（カプセル化されたクラス）

#### C. メンバ変数

6. `static constexpr` 静的メンバ定数
7. `static constinit` 静的メンバ変数 (C++20)
8. `static const` 静的メンバ定数（`constexpr` 化が不可能な型）
9. `static inline` 静的メンバ変数 (C++17)
10. `static thread_local` 静的メンバ変数
11. `static volatile` 静的メンバ変数
12. `static` 静的メンバ変数
13. 非適用：`constexpr` 非静的データメンバは宣言不可（型の定数は 6，インスタンス毎の不変値は 14 に従う）
14. `const` インスタンスメンバ変数
15. 非適用：`inline` 非静的データメンバは宣言不可（inline 変数は 9 の静的メンバで用いる）
16. `volatile` インスタンスメンバ変数
17. `mutable` インスタンスメンバ変数
18. インスタンスメンバ変数

#### D. メンバ関数

19. `static consteval` 静的メンバ関数 (C++20)
20. `static constexpr` 静的メンバ関数
21. `static inline` 静的メンバ関数
22. 非適用：静的メンバ関数に `this` を修飾する `const` / `volatile` は付与不可
23. `static` 静的メンバ関数
24. 純粋仮想関数 (`= 0`)
25. `virtual` インスタンスメンバ関数（非純粋）
		- 25a. `const` 修飾有（`this` を変更しない）
		- 25b. `const` 修飾無（`this` を変更し得る）
26. `override` / `final` インスタンスメンバ関数
		- 26a. `const` 修飾有
		- 26b. `const` 修飾無
27. `consteval` インスタンスメンバ関数
		- 27a. `const` 修飾有
		- 27b. `const` 修飾無
28. `constexpr` インスタンスメンバ関数
		- 28a. `const` 修飾有
		- 28b. `const` 修飾無
29. `inline` インスタンスメンバ関数
		- 29a. `const` 修飾有
		- 29b. `const` 修飾無
30. `volatile` インスタンスメンバ関数
		- 30a. `const` 修飾有 (`const volatile`)
		- 30b. `const` 修飾無
31. 通常のインスタンスメンバ関数（演算子ではなく，参照修飾子・`noexcept`・属性は順位に影響しない）
		- 31a. `const` 修飾有
		- 31b. `const` 修飾無
32. メンバ演算子オーバーロード
		- 32a. `const` 修飾有（`==` / `<=>` / `[]` 等の読取専用演算子）
		- 32b. `const` 修飾無（`+=` / `++` 等の変更系演算子）
		- 32c. コピー代入演算子（`operator=`，特殊メンバ関数）
		- 32d. ムーブ代入演算子（`operator=`，特殊メンバ関数）

#### E. 特殊メンバ関数（各アクセスセクション内）

生成・破棄に関する公開範囲は設計に従う．非公開コンストラクタや保護されたデストラクタを順序の為に公開しない．複数の分類に該当するメンバは特殊メンバ関数・演算子の分類を優先し，其の他は該当する上位の順位に配置する．

33. コンストラクタ（`explicit` を含む）
		- 33a. 値受取コンストラクタ
		- 33b. コピーコンストラクタ
		- 33c. ムーブコンストラクタ
		- 33d. 既定コンストラクタ
34. デストラクタ

```cpp
class Model {
private:

	friend class ModelManager; // 1. friend

	using Coord = VECTOR; // 2. using

	enum class Phase { Idle, Active }; // 3. enum class

	struct State {
		float X;
		float Y;
	}; // 4. struct

	static constexpr float MaxSlope = DX_PI_F / 6.0F; // 6. static constexpr
	static const std::string Tag; // 8. static const
	static thread_local int CallCount; // 10. static thread_local
	static int InstanceCount; // 12. static
	const int Id; // 14. const
	mutable bool CacheDirty; // 17. mutable
	int Size; // 18. 其の他のインスタンスメンバ変数
	bool Goal;
	MATRIX RotationMat;
	Coord Center;
	int Handle;
	float CamAngH;
	void DrawMarker(const Coord &C, const int &Handle) const; // 31a. const 通常（DrawModel から呼ばれる）
	void SetMarker(); // 31b. 非 const 通常（Recompute から呼ばれる末端）
	void Recompute(); // 31b. 非 const 通常（SetMarker を呼ぶ）

public:

	void DrawModel() const; // 31a. const 通常（DrawMarker を呼ぶ）
	float Rotate(const float &CamAngH); // 31b. 非 const 通常（Recompute を呼ぶ）
	VECTOR GetBallLocation(); // 31b. 非 const 通常（Rotate と DrawModel を呼ぶ）

	Model(const Model &) = delete; // 33b. コピー

	Model(Model &&) noexcept; // 33c. ムーブ
	Model(); // 33d. 既定
	~Model(); // 34. デストラクタ
};
```

### 定義順序

ファイル内の関数定義は，公開範囲・初期化順序・言語別区画を保持した上で，被呼出側（末端の関数）を先に，呼出側（上位の関数）を後に配置する．クラス外定義にはクラス内宣言の種別順を適用しない．相互再帰と競合する呼出順序は，宣言順序の規則に従う．

下例はクラス外定義に於ける依存関係順の配置例を示す．

```cpp
void Model::SetMarker() {} // 末端（Recompute から呼ばれる）

void Model::Recompute() {
	SetMarker();
	// 終了
	return;
} // SetMarker を呼ぶ

float Model::Rotate(const float &CamAngH) {
	Recompute();
	// 回転後の角度の返戻
	return CamAngH;
} // Recompute を呼ぶ

void Model::DrawMarker(const Coord &C, const int &Handle) const {} // 末端（DrawModel から呼ばれる）

void Model::DrawModel() const {
	DrawMarker(Center, Handle);
	// 終了
	return;
} // DrawMarker を呼ぶ

VECTOR Model::GetBallLocation() {
	Rotate(CamAngH);
	DrawModel();
	// ボール位置の返戻
	return Center;
} // エントリポイント
```

## 5.12 命名 (MUST)

**対象**：全言語共通

識別子の命名は以下の原則に従う．

- 意味を表す名前を使用する．
- 識別子は原則として英語で命名する．ローマ字表記（`kensaku` 等）は使用しない．
- 識別子に使用する文字は ASCII の英数字と `_` に限る．言語仕様が Unicode 識別子を許容して居る場合であっても，非 ASCII 文字（漢字・仮名・キリル文字・ギリシャ文字・絵文字等）は使用しない．
	- 言語仕様上必須とされる接頭辞や接尾辞の記号は名前の一部と見做さず，本制限の対象外とする（C# の `@`，Rust のライフタイム `'`，PHP の `$`，JavaScript / TypeScript の `#`，Ruby の `@` / `@@` / `$` 及び末尾の `?` / `!` / `=`，Ruby の演算子メソッド名 `<=>` / `[]`）．

	**例外**：定訳の無い固有名詞・製品名・外部 API が定める名称は其の儘用いる．

命名規則は以下の通りとする（言語毎の一般的な慣行と異なる場合であっても，言語間の一貫性を優先し本規約に従う）：

- C, C++, C#, Java: 全て `PascalCase`（ローカル変数，関数，クラス，定数を含む）
- Go: エクスポート識別子は `PascalCase`，非エクスポート識別子は `camelCase`
- Rust: ローカル変数・関数は `snake_case`，型・トレイトは `PascalCase`，定数は `UPPER_SNAKE_CASE`
- Kotlin: 全て `PascalCase`（ローカル変数，関数，クラス，定数を含む）
- Swift: ローカル変数・関数は `camelCase`，型・プロトコルは `PascalCase`，`enum` の case は `camelCase`
- PHP: ローカル変数・メソッドは `camelCase`，クラス・インタフェース・トレイトは `PascalCase`，定数は `UPPER_SNAKE_CASE`
- JavaScript, TypeScript: ローカル変数・関数は `camelCase`，型・クラスは `PascalCase`，定数は `UPPER_SNAKE_CASE`
	- React コンポーネントの関数・束縛は `PascalCase` とする．`const` に依る再代入不可と言う理由のみで，全ての局所束縛を定数名記法に変更してはならない．`UPPER_SNAKE_CASE` は固定の設定値等の定数に用いる
- Ruby: ローカル変数・メソッドは `snake_case`，クラスは `PascalCase`，定数は `UPPER_SNAKE_CASE`
- Python: ローカル変数・関数は `snake_case`，クラス・型エイリアスは `PascalCase`，定数は `UPPER_SNAKE_CASE`

識別子の種類や用途に応じた個別規則は以下の通りとする．

- 言語仕様に因って固定されて居る名前（C / C++ の `main` 関数等）は本節の対象外とする．
- **HTML, CSS の識別子**：`id`，`class`，カスタムプロパティは `kebab-case` とする．`id` は文書内で一意にする．
- **JSON のキー**：`camelCase` とする（外部仕様が定める場合は其れに従う）．同名キーを重複させない．
- **真偽値**：真偽値（変数・プロパティ・真偽を返す関数）は `is` / `has` / `can` / `should` 等の動詞・助動詞で開始し，真偽で回答出来る命名にする（大文字・小文字は各言語の規則に従う．例：`isValid` / `IsValid`, `hasError`, `canRetry`）．
	- **例外 (Ruby)**：真偽を返すメソッドは `?` 接尾辞で表し，`is_` / `has_` 接頭辞は付与しない (`valid?`, `empty?`)．レシーバを破壊的に変更するメソッドには `!` を付与し，非破壊版と対で提供する．
- **アクセサ**：セッターの接頭辞は `set` とし，ゲッターの接頭辞は `get` とする（大文字・小文字は各言語の規則に従う．例：`setCount` / `SetCount`, `getCount` / `GetCount`, `set_count` / `get_count`）．
- **１文字変数**：C, C++, C#, Java, Kotlin に於ける１文字変数の大文字・小文字は以下に従う．
	- 意味を持たないカウンタ・一時変数（`i`, `j`, `k`, `n`, `x`, `a`, `b` 等）：小文字
	- 意味を持つ１文字（座標の `X` / `Y` / `Z`，テンプレート引数の `T` 等）：大文字
- **意味の伝わらない省略名の禁止**：`I`, `Nl`, `Op`, `Sv` 等，語を切り詰めて意味が不明瞭に為った短縮名は禁止し，意味の伝わる名前（`Iterator`, `Newline`, `Operator`, `Source` 等）を使用する．大文字・小文字は各言語の規則に従う．尚，前項の慣用的な１文字変数や，慣用イテレータ名（`it` / `lhs` / `rhs`）は本禁止事項の対象としない．

```cpp
int uc; // ❌
int UserCount; // ✅

for(int i = 0; i < n; ++i) // ✅ 意味の無いカウンタは小文字
float X = Position.X; // ✅ 座標を表す X は大文字
```

- **ファイル名**：定義する主要なクラス名・モジュール名と一致させる（JSON / HTML / CSS，及び外部ツールが名称を固定して居る設定ファイルは，其のツールの要求仕様を優先する）．
	- C, C++, C#, Java: `PascalCase`（Java は public クラス名との一致が言語仕様上の必須要件）
	- Go, Rust: `snake_case`
	- Kotlin, Swift: `PascalCase`（Kotlin は複数のトップレベル宣言を持てる為，内容を表す適切な名前で良い）
	- PHP（クラスを定義するファイル）：`PascalCase`
	- JavaScript, TypeScript（React コンポーネント）：`PascalCase`
	- JavaScript, TypeScript（非コンポーネント）：`camelCase`
	- Ruby, Python: `snake_case`
	- JSON, HTML, CSS: `kebab-case`

**注意 (Go)**：`_test`, `_<GOOS>`, `_<GOARCH>` で終わるファイル名は暗黙のビルド制約と為り，該当環境以外では自動的にビルド対象から除外される．意図した場合を除き，末尾に `test`, `windows`, `linux`, `darwin`, `amd64`, `arm64` 等を配置しない．

**注意 (SCSS)**：パーシャルファイルは先頭に `_` を付与する (`_variables.scss`)．

### エラー変数の命名

エラーや例外を扱う変数は以下の規則に従って命名する．

- `e`: `catch` 節の例外変数（大文字・小文字は上記の言語別規則に従う．PascalCase 系言語では `E`）
- `error`: コールバック関数の引数等で受け取る非例外エラー
- 具体名（`databaseError`, `retryError` 等）：ネストされた `catch` 節等で外側の例外と区別が必要な場合

```cpp
// catch: E を使用
try {
	Process();
} catch(const std::exception &E) {
	std::cerr << "failed: " << E.what() << std::endl;
}

// コールバック：Error を使用
Exporter.Export(
	Scene,
	[](const Result &R) -> void {
		HandleResult(R);
	},
	[](const Error &Error) -> void {
		std::cerr << "export failed: " << Error.what() << std::endl;
	}
);

// ネストされた catch：具体名で区別
try {
	Attempt();
} catch(const std::exception &E) {
	try {
		Retry();
	} catch(const std::exception &RetryError) {
		std::cerr << "retry failed: " << RetryError.what() << std::endl;
	}
}
```

---

# 第６章 ファイル構成・言語固有機能

## 6.1 include / import の順序 (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python, HTML, CSS（SCSS を含む）

以下の順序で記述する．import / include 同士の間には空行を入れない（2.4 参照）．

前提マクロ・初期化・登録・循環依存・カスケード等に意味を持つ読込順序は保持する．Python の `from __future__` 等，言語仕様が要求する位置も優先する．

**C, C++:**
1. 対応するヘッダ（`foo.cpp` に対する `"foo.h"` 等）
2. プロジェクト内ヘッダ（`"..."` 形式）
3. サードパーティライブラリヘッダ
4. 標準ライブラリヘッダ（`<...>` 形式）

```cpp
#include "Chunk.h"
#include "NoiseGenerator.h"
#include "Terrain.h"
#include "Mesh/RealtimeMeshAlgo.h"
#include "RealtimeMeshSimple.h"
#include <algorithm>
#include <vector>
```

**C#:**
1. `System.*`
2. サードパーティ
3. プロジェクト内
4. `using static` とエイリアス (`using X = Y;`)

**Java:**
1. `java.*` / `javax.*`
2. サードパーティ
3. プロジェクト内
4. `import static`

**Go:** 1. 標準ライブラリ 2. サードパーティ 3. 自プロジェクトの順とし，グループ内はアルファベット順とする．未使用の import はコンパイルエラーに為る為残さない．

**Rust:** 1. `std` 2. 外部 crate 3. `crate` / `self` / `super` の順．グループ内はアルファベット順とする．

**Kotlin:** Java と同順とし，`import ... as` は各グループの末尾に置く．

**Swift:** 1. 標準ライブラリ (`Swift`, `Foundation`) 2. Apple 製フレームワーク 3. サードパーティ 4. プロジェクト内モジュール．グループ内はアルファベット順とする．

**PHP:** 1. `<?php` 2. `declare(strict_types=1);` 3. `namespace` 4. `use`（クラス → `use function` → `use const` の順，各グループ内はアルファベット順）．

**JavaScript, TypeScript:**
1. React / React Native
2. サードパーティライブラリ
3. プロジェクト内モジュール（絶対パス）
4. 型定義

名前付インポート（`import { B, A }` 等）はアルファベット順（大文字・小文字不問）で並べ替える (MUST)．

```typescript
// ❌
import { View, Text, StyleSheet } from "react-native";

// ✅
import { StyleSheet, Text, View } from "react-native";
```

**Ruby:** `require`（外部ライブラリ）→ `require_relative`（自プロジェクト）の順とする．

**Python:** 1. 標準ライブラリ 2. サードパーティ 3. 自プロジェクトの順とし，グループ内はアルファベット順とする．

**HTML:** `<link rel="stylesheet">` は `<head>` 内へ，`<script>` は `defer` を付けて `<head>` 内へ置くか，`</body>` の直前へ置く．

**CSS:** `@import` は `@charset` と `@layer` 文より後，其の他の全ての規則より前へ置く（後に置くと無効に為る）．

**SCSS:** `@use` / `@forward` は言語仕様が許す先頭位置へ置き，設定と読込の依存関係を保持する．同一モジュールを再公開且つ利用する場合は `@forward` → `@use` の順とし，利用側の設定を先に適用する．設定用変数の宣言や出力 CSS の順序も保持する．`@import` は使用しない（6.16 参照）．

**例外**：競技プログラミング等の小規模コードでは，`#include <bits/stdc++.h>` の使用を許可する．

## 6.2 ヘッダガード (MUST)

**対象**：C, C++

ヘッダファイルには，対象の処理系が対応している場合 `#pragma once` を使用する．`#ifndef` ガードは通常使用しない．

**例外**：非標準の `#pragma once` に対応しない処理系への移植要件が有る場合は，`#ifndef` ガードを用いる．又，既存のガード名が外部の条件分岐等に作用している場合は，単なる重複インクルード防止として削除しない．

```cpp
// ❌
#ifndef FOO_H
#define FOO_H
// ...
#endif

// ✅
#pragma once
// ...
```

## 6.3 宣言と定義の分離 (MUST)

**対象**：C, C++

関数の宣言（プロトタイプ）はヘッダファイル (`.h` / `.hpp`) に，定義（実装）はソースファイル (`.c` / `.cpp`) に記述する．ヘッダファイル内に関数本体を記述してはならない．此れは C のフリー関数，並びに C++ のフリー関数及びメンバ関数に適用する．

引数の `const` 修飾は宣言側と定義側の両方に記述する．値渡しの最上位 `const` は言語仕様上シグネチャに影響しないが，記述を統一する事で「関数本体内で再代入しない」という意図を呼出側・実装側双方に明示する．ポインタや参照の `const`（C は `const T *`，C++ は `const T *` / `const T &`）は型の一部であり，宣言と定義で必ず一致させる必要が有る．

**例外**：次の関数は，必要な翻訳単位で定義を参照出来る様に配置する（必ずしも公開ヘッダへ置くという意味ではない）．

- 意図してヘッダへ実装を置く明示的な `inline` 関数（C の `static inline` を含む）
- `template` 関数（明示的実体化で定義を分離出来る場合を除く）
- `constexpr` / `consteval` 関数
- ローカルクラスのメンバ定義等，言語仕様がクラス内部での定義を要求する物

尚，クラス内定義が暗黙に `inline` に為る事だけを理由に，分離規則の例外としてはならない．又，外部へ公開しない関数・実装専用型・`main` の宣言を公開ヘッダへ追加する必要は無い（実装内で必要な前方宣言を用いる）．

```c
// ❌ C ヘッダ内に定義
// foo.h
int Add(int A, int B) { return A + B; }

// ✅ C ヘッダには宣言のみ
// foo.h
int Add(int A, int B);

// ✅ C ソースに定義
// foo.c
int Add(int A, int B) {
	// 和の返戻
	return A + B;
}
```

```cpp
// ❌ C++ ヘッダ内に定義
class Foo {
public:

	void Bar() {
		DoSomething();
	}
};

// ✅ C++ ヘッダには宣言のみ
class Foo {
public:

	void Bar();
};

// ✅ C++ ソースに定義
void Foo::Bar() {
	DoSomething();
	// 終了
	return;
}
```

```cpp
// ❌ 宣言と定義で const が不一致
// foo.hpp
void Bar(int Value, const std::string &Name);

// foo.cpp
void Bar(const int Value, const std::string &Name) {
	// ...
}

// ✅ 宣言と定義の両方に const
// foo.hpp
void Bar(const int Value, const std::string &Name);

// foo.cpp
void Bar(const int Value, const std::string &Name) {
	// ...
}
```

## 6.4 キャスト (MUST)

**対象**：C, C++

C++ では C 形式のキャスト `(int)x` の使用を禁止し，C++ のキャスト演算子を使用する．C では C 形式のキャストを使用する（C++ のキャスト演算子が存在しない為）．

キャスト演算子の使分：
- `static_cast`: 一般的な型変換（整数↔浮動小数点，列挙型↔整数等）
- `reinterpret_cast`: ポインタ型の再解釈（極力避ける）
- `const_cast`: `const` 外し（極力避ける）
- `dynamic_cast`: ポリモーフィズムに依るダウンキャスト

```cpp
// ❌
int n = (int)Str.length();
float f = (float)x;

// ✅
int n = static_cast<int>(Str.length());
float f = static_cast<float>(x);
```

## 6.5 マクロの使用制限 (MUST)

**対象**：C, C++

定数に `#define` は使用しない．C++ では `constexpr`（5.9 参照）又は `const` を使用する．C では `const` を優先し，C23 では `constexpr` を用いて良い．

定数以外のマクロは，条件コンパイル（`#ifdef` 等），又は外部 ABI・処理系が要求し他の機能で代替出来ない場合に限る．インクルードガードは 6.2 に従う．

**例外**：
- C++ に於いて，`#if` 等の前処理で参照する定数は `#define` を用いる．
- C に於いて，`const` で表現出来ない場合（配列長，`case` ラベル，ビットフィールド幅，列挙値初期化子等，定数式が必要な文脈）に限り `#define` の使用を許可する．

```cpp
// ❌
#define MAX_SIZE 100
#define PI 3.14159F

// ✅
constexpr int MaxSize = 100;
constexpr float Pi = 3.14159F;
```

## 6.6 using namespace (MUST)

**対象**：C++

`using namespace` は原則禁止とする．名前空間は明示的に記述する．

**例外**：競技プログラミング等の小規模な単一ファイルコードでは，`using namespace std;` を許可する．

```cpp
// ❌
using namespace std;
cout << "hello" << endl;

// ✅
std::cout << "hello" << std::endl;
```

## 6.7 NULL と nullptr (MUST)

**対象**：C, C++

C++ では `NULL` の使用を禁止し，`nullptr` を使用する．C17 以前の C では `NULL` を使用する（`nullptr` が存在しない為）．C23 では `nullptr` を使用する．

```cpp
// ❌
int *Ptr = NULL;
if(Ptr == NULL) return;

// ✅
int *Ptr = nullptr;
if(!Ptr) return;
```

## 6.8 エラー処理 (SHOULD)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python

エラーを握り潰す（空の `catch` ブロック，Go の `_ = f()`，Rust の `let _ = ...` 等）事は禁止する．呼出側への伝播，明示された回復処理，又は適切な境界での記録を行う．意図した代替値の返却は握り潰しとは区別する．記録には認証情報や不要な入力本文を含めず，同一の異常に対する重複記録も避ける．

各言語のエラー処理方針は以下の通りとする．

- C: 例外機構が無い為，戻り値と `errno` で異常を返す．呼出側は必ず判定する．
- C++: 例外 (`throw` / `try` / `catch`) を基本とする．パフォーマンスが要求される箇所では戻り値に依る判定を許可する．
- C#: 例外を基本とする．捕捉は具体型に限り，`catch(Exception)` は再送出を伴う場合のみ許可する．リソース解放は `using` 宣言で行う．
- Java: 例外を基本とする．回復可能な異常は検査例外，前提条件違反は非検査例外（`IllegalArgumentException` 等）とする．検査例外を意味の無い非検査例外へ包み直さない．リソース解放は try-with-resources で行う．
- Go: 例外を用いず `error` を最終戻り値で返す．呼出側は必ず判定し，握り潰し (`_ = f()`) を禁止する．文脈追加は `fmt.Errorf("…: %w", resultError)`，判定は `errors.Is` / `errors.As` で行う．`panic` は不変条件の破壊時のみとする．リソース解放は `defer` で行う．
- Rust: `Result<T, E>` を返し，伝播は `?` 演算子で行う．`unwrap` / `expect` は不変条件が保証出来る箇所に限る．エラー型は `std::error::Error` を実装し，文脈追加は `map_err` で行う．`Result` の無視 (`let _ = ...`) を禁止する．
- Kotlin: 例外を基本とする（検査例外は無い）．Java から呼ばれる API にのみ `@Throws` を付ける．リソース解放は `use` で行う．
- Swift: `throws` と `do` / `catch` を基本とする．`try!` は禁止する．`try?` は失敗を `nil` として扱う事が意味的に正しい場合のみ用いる．
- PHP: 例外 (`throw` / `try` / `catch` / `finally`) を基本とする．捕捉型は `Throwable` ではなく具体型を指定し，`Error`（内部エラー）は原則として捕捉しない．複数型は `catch(FooException|BarException $e)` で束ねる．
- JavaScript, TypeScript: `try` / `catch` を基本とする．非同期処理では `.catch()` 又は `await` を伴う `try` / `catch` を使用する．
- Ruby: 例外 (`raise` / `rescue`) を基本とする．代替値の返却のみで足り，且つ捕捉対象が `StandardError` 全域で構わない場合に限り修飾子 `rescue`（`<式> rescue <代替値>`）を用いる．ログ出力・例外種別の限定が必要な場合，本体が複数文の場合，複数 rescue 分岐・else／ensure を伴う場合は通常形式の `begin / rescue / end` を用いる．
- Python: `try` / `except` を基本とする．裸の `except:` は `KeyboardInterrupt` 迄捕捉する為禁止し，`except <具体型> as e` とする．再送出時は `raise X from e` で原因を保つ．リソース管理は `with` 文で行う．

```ruby
# ❌ 単一文を begin/rescue/end で包む
value = begin
	JSON.parse(str)
rescue
	{}
end

# ✅ 単一文 — 修飾子 rescue
value = JSON.parse(str) rescue {}

# ✅ 複数文本体，複数 rescue 分岐，又は ensure を伴う場合 — 通常形式
result = begin
	complex_op_a
	complex_op_b
rescue ArgumentError => e
	handle_argument(e)
rescue StandardError => e
	handle_standard(e)
end
```

## 6.9 言語固有の必須事項 (MUST)

**対象**：C, C#, Java, Go, Rust, Kotlin, Swift, PHP, Ruby, Python

各言語の中核機能の内，安全性 (1.3) に直結する事項を以下に定める．

### C

- 外部へ公開しない関数・変数には `static` を付け，内部リンケージにする．
- ヘッダは自己完結させる（必要な `#include` を自ら持つ）．ヘッダには公開宣言のみを置き，定義は１つの翻訳単位に置く（6.3 参照）．
- 文字列・バッファ操作は境界付関数（`snprintf` 等）を用いる．`size_t` の書式指定子は `%zu` とする．
- `restrict` は別名が存在しない事を保証出来る場合のみ付ける．
- 使用する C の版 (C17 / C23) をプロジェクトで固定し，`bool`／`nullptr`／`constexpr`／２進リテラルの可否を明示する．

### C#

- 公開データはプロパティで表し，公開フィールドを作らない．必要な変更可否に応じて `get` のみ・`init`・非公開 `set` 等を選び，不要な公開 setter を追加しない．尚，`record` だけでは参照先を含む不変性は保証されない点に注意する．

### Java

- `record` は不変データ，`sealed interface` は限定された派生に用いる．

### Go

- `defer` はループ内に置かない（関数終了迄解放されない為）．リソース解放の方針は 6.8 に従う．
- `nil` 判定は `if resultError != nil` の形で明示的に書く（4.6 の対象外）．
- ゴルーチンは起動元が停止責任を持つ．`context.Context` を取る関数では第１引数に置く．
- `interface{}` ではなく `any` を用い，型を特定出来る箇所では具体型を用いる．

### Rust

- 引数は既定で借用 (`&T` / `&mut T`) とし，所有権が必要な場合のみ値渡しにする．`clone()` で借用検査を回避しない．
- 文字列は引数に `&str`，保持に `String` を用いる．スライスは引数に `&[T]`，保持に `Vec<T>` を用いる．
- `unsafe` はプロジェクトで許可した箇所のみとする．
- 条件付の初期化は `let x = if c { a } else { b };` 又は `match` 式で行う（三項演算子は存在しない）．

### Kotlin

- トップレベル関数・拡張関数を用いて良い．`data class` は値の集約，`object` は単一実体，`companion object` は型に紐付く定数・生成関数に用いる．
- 条件付の初期化は `if` 又は `when` の式で行う．

### Swift

- 先ず `struct`（値型）を用い，参照同一性・継承・`deinit` が必要な場合のみ `class` を用いる．`struct` の状態変更メソッドには `mutating` を付ける．
- 保持関係を調べ，循環参照が生じる場合は `[weak self]` 等で解消する．`[unowned self]` はクロージャの使用中に参照先が生存する保証が有る場合に限る．処理完了迄の生存に必要な強参照を一律に弱めない．
- リソース解放・後処理は `defer` で行う．
- 準拠毎に `extension` を分けて良い（5.11 の宣言順序は各 `extension` 内で適用する）．

### PHP

- 純粋な PHP ファイルでは終了タグ `?>` を書かない（末尾の余分な出力を防ぐ為）．
- 合成には `trait` を用い，クラス本体の先頭で `use` する（5.11 参照）．
- ヒアドキュメント（展開有）とナウドキュメント（展開無）は意味が異なる為，用途に応じて選び機械的に変換しない．
- 論理演算には `&&` / `||` を用いる（`and` / `or` / `xor` は代入より優先順位が低く括弧が必要と為る．4.4 参照）．

### Ruby

- ハッシュのキーはシンボル (`{ key: value }`) を既定とし，外部データの写しのみ文字列キーとする．
- 論理演算には `&&` / `||` を用いる（`and` / `or` は代入より優先順位が低く括弧が必要と為る．4.4 参照）．

### Python

- 可変オブジェクト (`[]` / `{}` / `set()`) を既定引数に指定してはならない（既定値は定義時に１度だけ評価され，呼出間で共有される為）．`None` を既定にして本体で生成する．
- `global` / `nonlocal` は使用しない．
- 反復には `enumerate` / `zip` を用い，添字の手動管理を避ける．

## 6.10 型ヒント・null 安全 (MUST)

**対象**：C#, Kotlin, Swift, PHP, TypeScript, Python

関数の引数と戻り値の型明記は 5.4 に従う．

- **C#**: プロジェクト全体で `<Nullable>enable</Nullable>` を有効化する．null 免除演算子 `!` は使用禁止とし，`?.` / `??` / `ArgumentNullException.ThrowIfNull` で代替する．
- **Kotlin**: 値が null を取り得るか否かを型 (`T` / `T?`) で表現する．`!!` は使用禁止とし，`?.`／`?:`／`requireNotNull`／スマートキャストで代替する．`lateinit var` は依存注入等で初期化時点を制御出来ない場合に限る．
- **Swift**: Optional (`T?`) で null 可能性を表現する．強制アンラップ `!` と `try!` は使用禁止とし，`if let` / `guard let` / `??` / `flatMap` で代替する．`guard` は早期脱出に用いる．
- **PHP**: ファイル先頭の許される位置へ `declare(strict_types=1);` を置く．スカラー引数の厳密性は原則として呼出元ファイルに依存し，宣言側だけで全呼出を厳密に出来る訳ではない点に留意する．無効時も型宣言自体は存在し，許される型変換の範囲が異なる．未定義と null を同一視する場合は `isset()` / `??`，null 其の物の判定は `=== null` を用いる．`empty()` は偽値全般を真とする為，`0` や空文字列を区別する文脈では使用しない．
- **TypeScript**: `tsconfig.json` で `strict`（少なくとも `strictNullChecks`）を有効化する．非 null 表明 `!` は使用禁止とし，`?.`／`??`／型ガードで代替する．

PHP は引数・戻り値・属性へ型宣言を付ける．`?T` と `T|null` は `?T` に統一し，`mixed` は型が定まらない場合のみ使用する．

```php
class Cart {
	private readonly int $itemCount;

	public function findItem(string $code): ?Item {
		return $this->items[$code] ?? null;
	}
}
```

Python は使用する版をプロジェクトで固定し，ジェネリクス・合併型・省略可能型の記法を其の版で有効な単一の形式へ統一する．引数・戻り値と型が自明でない変数へ型ヒントを付け，型エイリアスは `PascalCase` で定義する．`typing` からは組込に対応の無い物のみ取り込む．

```python
Tree = str | tuple

def parse_ids(ids: str) -> tuple[Tree, int]:
return ids, 0

def count_strokes(cp: int) -> int:
	return 0
```

変数の型ヒントは，型が自明でない場合のみ付ける．

```python
# 不要（右辺から自明）
name: str = "hello"

# 必要（型が不明瞭）
components: dict[str, dict[str, set[str]]] = { idc: { role: set() for role in roles } for idc, roles in IDC_ROLES.items() }
```

## 6.11 JSX 条件レンダリング (MUST)

**対象**：JavaScript, TypeScript

数値を `&&` の描画条件に使用する場合は `!!` で真偽値化する．数値が `0` の場合に式の値も `0` と為り，React は数値を其の儘描画する．React Native では `Text` コンポーネントの直下以外のテキストがエラーに為る為，条件の偽値として数値を返してはならない．

```tsx
// ❌（length が０の時 "0" が描画される）
{items.length && <List items={items} />}

// ✅
{!!items.length && <List items={items} />}
```

## 6.12 Ruby メソッドの１行定義 (MUST)

**対象**：Ruby

Ruby 3.0+ で導入された `def 名前(引数) = 式` 形式（メソッドの１行定義）は１行で記述する．１行定義は末尾式が戻り値である事が形式上自明である為，5.7 の `return` 明示義務及び 7.10 の返戻コメントの対象外とする．

**例外**：セッター (`name=`) は言語仕様上１行定義に出来ない為，通常の複数行形式で記述する．

```ruby
def double(x) = x << 1
def square(x) = x ** 2
```

## 6.13 内包表記 (SHOULD)

**対象**：Python

リスト・辞書・集合の生成には `for` ループよりも内包表記を優先する．ネストは１段迄を推奨し，２段以上は可読性を考慮して判断する．

```python
# ❌
result = []
for x in items:
	if x.is_valid():
		result.append(x.value)

# ✅
result = [x.value for x in items if x.is_valid()]

# ✅ 辞書内包表記
char_rank = { cp: i for i, cp in enumerate(sorted_chars) }

# ✅ 集合内包表記
unique_ids = { ids for id_list in df["IDS"] for ids in id_list }
```

## 6.14 JSON (MUST)

**対象**：JSON

- キーの命名規則と重複禁止は 5.12 に従う．
- キーは意味的なまとまり毎に並べ，同一グループ内は宣言順を保つ．
- 数値は受信側の精度・範囲の契約に合わせる．一般的な binary64 受信側で整数を厳密に扱う場合は絶対値を `2^53 - 1` 以下に収める．`0.1` 等の小数が２進数で厳密に表現出来ない事を理由に禁止せず，許容誤差を定める．厳密な１０進値・巨大な整数が必要なら合意した文字列等の表現を用い，書式変更で値を丸めない．
- 指数記号は `E` に統一する．
- 素の JSON はコメントを書けない為，説明は別文書に記述する（7.1 参照）．JSONC は `//` と `/* */` を記述出来る為，第７章の全言語共通規約に従う．

## 6.15 HTML 構造 (MUST)

**対象**：HTML

- 完全な HTML 文書の先頭に `<!DOCTYPE html>` を置く（指定が無い場合は後方互換モードに為る）．HTML 断片へ文書構造タグを追加してはならない．
- 完全な文書では `html` の `lang` を本文の言語に合わせ（日本語は `ja`），`<meta charset="UTF-8">` を置く．断片は埋込先の文書構造に従う．
- 意味付タグ (`header` / `nav` / `main` / `article` / `section` / `aside` / `footer`) を用い，`<div>` は意味を持たない配置目的に限る．
- `<img>` には `alt` を必ず付ける．装飾目的の画像は `alt=""` とする．
- 入力欄は `<label>` と関連付ける（`for` 属性との対応に依り，ラベルのクリックが入力欄へ届く様にする）．
- `<script>` の文字列リテラル中に `</script>` を直接記述出来ない為，`<\/script>` へエスケープする．
- 空要素は `<br>` の形で書き，自己終端のスラッシュ `/` を付けない．真偽属性は属性名のみを書く (`disabled`)．但し，インライン SVG / MathML（外来コンテンツ）では `/` が要素の終了を意味する為，此の規則は適用しない．
- 属性は `id` → `class` → `data-*` → 其の他の順に並べる．`id` の命名と一意性は 5.12 に従う．
- インラインの `style` 属性と `on*` 属性は使用しない．

## 6.16 CSS / SCSS (MUST)

**対象**：CSS, SCSS

- `!important` は他の手段が無い場合のみ用いる．
- セレクタと `{` は同一行に置く．最後の宣言にも `;` を付ける．
- １６進カラーコードは大文字で書く (`#ABCDEF`)．
- 識別子（`id`・`class`・カスタムプロパティ）の命名は 5.12 に従う．
- プロパティは，カスケードの結果を保持出来る場合に「配置 → 寸法 → 装飾」の順に並べる．但し，重複宣言・フォールバック・一括指定と個別指定・論理方向と物理方向・SCSS の評価順が関係する物は相対順を保持する．
- SCSS の除算は `math.div` を用いる（`/` に依る除算は非推奨で将来削除される為）．
- SCSS の取込は `@use` / `@forward` を用い，`@import` は使用しない．

---

# 第７章 コメント

## 7.1 基本 (MUST)

**対象**：全言語共通（素の JSON を除く）

- 日本語で記述する．
- 句点（．）は文末・文中を問わず使用しない（句読点の文字種は 8.5 参照）．文の区切りは読点（，）・連用形・行分割で表す．日本語文中での例外は数式の小数点のみとする．
- 但し，数式・変数名・英文を其の儘記述する場合の `.` は日本語文の句点ではない為，本項の対象外とする．
- **１文１行**とする（行コメント・ブロックコメント・ドキュメントコメントを問わない）．複数文からなる場合は文の境目で行を分け，１文を複数行にまたがって記述しない．
- 関数本体のコメントには「何をしているか」（コードを読めば分かる事）ではなく「何故そうしているか」を記述する（関数のドキュメントコメントは 7.8 に従い別途必須とする）．
- 但し，処理ブロックの見出として「何をしているか」を簡潔に示すコメントの記述は推奨する．

**例外**：シバン，コンパイラや型検査の指示子，cgo 前置宣言等は原文と適用位置を保持する．実行時データの docstring も通常のコメントと区別して扱う．

## 7.2 コメント形式 (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python, HTML, CSS（SCSS を含む）

通常のコメントには `//` を使用する．`//` とコメント文の間にはスペースを１つ挿入する．タブや２つ以上のスペースを挿入してはならない．内容の無い空のコメント行（`//` や `#` だけの行）は配置しない（`/** ... */` 内の ` *` だけの行は記法の一部と見なす）．

`/** ... */` 形式は，以下の２つの用途でのみ使用する．

1. **ドキュメントコメント**（7.8 参照）：其の言語で此の形式を用い，関数等の目的・引数・戻り値を記述する場合．
2. **セクション区切り**（7.9 参照）：`/** ========== セクション名 ========== */` 形式．

`/** ... */` 形式の空行ルール：

- 上側の空行は，コメント対象の構文のみを配置した場合と同様にする．コメントの有無に依って空行を増減させない．
- ドキュメントコメントと対象構文の間には空行を挿入しない．

**例外 (Ruby, Python)**：`//` ではなく `#` を使用する．`#` とコメント文の間のスペース規則も同様とする．

**例外 (Ruby, Python)**：１行目のシバン (`#!`)，マジックコメント (`# frozen_string_literal:`, `# -*- coding: -*-`)，ツール指示子 (`# rubocop:disable`, `# noqa`, `# type: ignore`) は原文を保持し，スペースを挿入しない．

**例外 (Go)**：`//go:build` / `//go:embed` / `//go:generate` 等のコンパイラ指示子と，`//nolint:` / `//lint:` 等のツール指示子は，スペースを挿入すると指示が無効化され黙って失われる為，スペースを入れない．

**例外 (HTML)**：`<!-- ... -->` を使用する．コメント本文に `<!--` / `-->` / `--!>` の３種の文字列を含めてはならない（終端と誤認され，以降が本文として描画される為）．コメント本文を `>` や `->` で開始したり，`<!-` で終了したりしてはならない．

**例外 (CSS)**：`/* ... */` を使用する．

**例外 (SCSS)**：`//` を使用する．`/* ... */` は生成された CSS に出力され，実装説明が配信物へ混入する為，出力に残したい著作権表示等にのみ `/*! ... */` を用いる．

```typescript
// ❌ 通常コメントに /** */ を使用
/** ボードのディープコピーを返す */
const cloneBoard = (board: BoardState): BoardState => { ... };

// ✅ 通常コメントには // を使用
// ボードのディープコピーを返す
const cloneBoard = (board: BoardState): BoardState => { ... };

// ✅ JSDoc には /** */ を使用（対象構文の空行規則に従い，下に空行禁止）
/**
 * 石を置いて盤面を更新する
 * @param board 現在の盤面
 * @param player 石を置くプレイヤー
 * @returns 更新後の盤面
 */
export const applyMove = (...) => { ... };

// ✅ セクション区切りには /** */ を使用（周囲の構文の空行規則に従う）
/** ========== スタイル ========== */
const styles = StyleSheet.create({ ... });

// ✅ 対象構文間の空行を維持
/**
 * 盤面を初期化する
 * @returns 初期状態の盤面
 */
export const createBoard = (): BoardState => { ... };

/**
 * 盤面の複製を返す
 * @param board 複製元の盤面
 * @returns 複製した盤面
 */
export const cloneBoard = (board: BoardState): BoardState => { ... };
```

## 7.3 禁止 (MUST)

**対象**：全言語共通（素の JSON を除く）

以下の対象にはコメントを記述しない：

- 同じパターンの繰返（最初の１箇所にのみコメントすれば十分である為）
- import 文及び include 文
- JSX のタグ自体

**例外**：言語や外部仕様が要求するコメントは，本項の禁止対象に含めない．Go の `import "C"` に付随する cgo 前置宣言等の位置と内容を保持する．

## 7.4 規約番号参照の禁止 (MUST)

**対象**：全言語共通（素の JSON を除く）

ソースコード内のコメントで本規約の節番号を直接参照してはならない．`規約 N.M`，`(規約 N.M 準拠)`，`CODING_STANDARDS.md 参照` 等の表記は禁止し，代わりに**意図其の物**を直接コメントとして記述する．

```cpp
// ❌ 規約番号を参照
if(c >= 'a' && c <= 'z') return true; // 規約 4.11 例外：ASCII 隣接

// ✅ 意図を直接記述
if(c >= 'a' && c <= 'z') return true; // 文字コード隣接境界の意図を保つ為，>= / <= 維持
```

## 7.5 宣言グループ (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python

関連する宣言グループの先頭に，行コメント（`//` 又は `#`）で見出を付ける．グループ内の自明な個別項目にはコメントを記述しない．特殊値・単位・制約等の説明が必要な項目は 7.7 に従う．

```cpp
// ファイル参照
File *Front = nullptr, *Back = nullptr;
// サウンド
int BgmHandle, SeHandle;
```

## 7.6 ステップコメント (MUST)

**対象**：全言語共通（素の JSON を除く）

関数本体の全コードを処理内容毎に分け，各処理の直前には内容を示す行コメント（`//` 又は `#`）を置く．コメントは対象のコードと同じ深さにインデントし，コードとの間に空行を入れない．ステップコメントは「対象の処理」の形（例：画像の読込）とし，体言止で簡潔に記述する．

```cpp
// 画像の読込
Image *Image = LoadImage(Path);
// マスクの生成
Mask *Mask = CreateMask(Image);
// 輪郭の抽出
Contour *Contour = ExtractContour(Mask);
```

## 7.7 インラインコメント (MUST)

**対象**：全言語共通（素の JSON を除く）

名前から意味が自明でない構造体・クラスのメンバやマジックナンバーには，対象と同じ行のインラインコメント（`//` 又は各言語のコメント記法）で説明を付ける．自明な値にはコメントを記述しない．特殊値（`null` と `undefined` の使分等）の説明は必ず記述する．

行末コメントの開始記号 (`//` / `#` / `/*` / `<!--`) の前には，スペース１つ又はタブの何れかを挿入する．複数のスペースを挿入してはならない．列を揃えたい場合はタブを使用する．

```cpp
// ❌ 複数スペースで列揃え
int BaseWidth = 32;   // 台座中心から接地限界迄の幅
int Margin = 4;       // 余白の基準幅（凸部の最小半径）

// ✅ スペース１つ
int BaseWidth = 32; // 台座中心から接地限界迄の幅
int Margin = 4; // 余白の基準幅（凸部の最小半径）

// ✅ タブで列揃え
int BaseWidth = 32;	// 台座中心から接地限界迄の幅
int Margin = 4;		// 余白の基準幅（凸部の最小半径）
```

## 7.8 ドキュメントコメント (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python

全ての名前付関数の定義に，各言語のドキュメントコメントを付与する．宣言と定義が分かれる言語では定義側にのみ記述し，宣言側には付与しない．引数・戻り値が存在する場合は其の説明を記述し，存在しない項目を空欄の儘追加してはならない．数学的な制約が有る場合は数学記号で示す．匿名関数に就いては代入先や引数の役割を説明し，文法上付与出来ない箇所へドキュメントコメントを挿入してはならない．

ドキュメントコメントの記述は，関数の使用に必要最低限の概要説明（関数の目的，引数の意味，戻り値の意味）にとどめる．関数内部のアルゴリズムの説明・実装詳細・最適化手法等は，呼出側からは不可視である為ドキュメントコメントに記載してはならない．此れらは関数本体内の該当箇所にステップコメント（7.6 参照）として記述する．

- C, C++: Doxygen (`/** ... */`) を使用する．`@param` で引数，`@return` で戻り値を記述する．
- C#: XML Doc (`/// <summary>`) を使用する．
- Java: Javadoc (`/** ... */`) を使用する．`@param` で引数，`@return` で戻り値を記述する．
- Go: godoc（関数名の直前に `//` コメントを配置）を使用する．godoc は宣言名で始まる文を要求し `@param` / `@return` に相当する記法を持たない為，引数と戻り値は本文中に平文で述べる．
- Rust: ドキュメントコメント（`///`，モジュール内部は `//!`）を使用する．引数・戻り値・失敗条件は `# Arguments` / `# Returns` / `# Errors` / `# Panics` の見出で示す．
- Kotlin: KDoc (`/** ... */`) を使用する．`@param` で引数，`@return` で戻り値を記述する．
- Swift: ドキュメントコメント（`///` 又は `/** ... */`）を使用する．
- PHP: PHPDoc (`/** ... */`) を使用する．`@param` で引数，`@return` で戻り値を記述する．
- JavaScript, TypeScript: JSDoc (`/** ... */`) を使用する．`@param` で引数，`@returns` で戻り値を記述する．
- Ruby: YARD (`# @param` / `# @return`) を使用し，`def` の直前に配置する．
- Python: Docstring (`"""`) を使用する．他言語と異なり `def` / `class` の**直後**（本体の先頭）に配置する．引数・戻り値・送出例外はプロジェクトで１つの見出書式に統一する．
- 補足タグ：C# は `<param>` / `<returns>` / `<exception>`，Java / Kotlin は `@throws`，Kotlin は `@property` も用いる．

`/** ... */` 形式を使用する言語に於ける空行ルールは 7.2 に従う．

主要な関数（算法を実装する関数，反復・再帰で入力の大きさに応じて費用が変わる関数）のドキュメントコメントには，計算量を O 記法で記載する．入力の何を大きさと看做すかも併せて示す．

```cpp
/**
 * 標高を算出する
 * 計算量：O(1)（標本の取得は格子の添字引き）
 * @param Location 当該地点の平面座標
 * @return 当該地点の標高
 */
FVector2f CalculateElevation(const FVector2f &Location) {
	// 当該地点の標高の返戻
	return Heightmap.Sample(Location);
}
```

```typescript
/**
 * マスク上の原点を取得する
 * @param mask バイナリマスク（0：背景，1：対象領域）
 * @param baseWidth 台座幅の½（∈ℤ，≧0）
 * @returns 原点座標 [X, Y]
 */
function findOrigin(mask: number[][], baseWidth: number): [number, number] {
	return [0, 0];
}
```

## 7.9 セクション区切り (SHOULD)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python, HTML, CSS（SCSS を含む）

ファイル内の大きな機能ブロックの区切りには `/** ========== セクション名 ========== */` 形式を使用する．`/** ... */` 形式を用いる言語に於ける空行ルールは 7.2 に従う．

区切り前後の空行は周囲の構文のみに基付いて決定し，区切りの有無に依って増減させない．

**例外 (HTML, CSS)**：`<!-- ========== セクション名 ========== -->` (HTML)・`/* ========== セクション名 ========== */` (CSS) を用いる（SCSS は `// ========== セクション名 ==========`）．

**例外 (Go)**：直後の宣言との間に１行の空行を入れる（空行が無いと区切りが直後の宣言の説明文へ取り込まれる為）．

**例外 (Rust)**：`/** ... */` はドキュメントコメントであり，直後に項目が無いとコンパイルエラーに為る為，`// ========== セクション名 ==========` を用いる．

**例外 (Ruby)**：`/** ... */` 形式が存在しない為 `# ========== セクション名 ==========` を用いる．

**例外 (Python)**：`/** ... */` 形式が存在しない為 `# ========== セクション名 ==========` を用いる．

```cpp
/** ========== セクション名 ========== */
```

## 7.10 return コメント (MUST)

**対象**：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python

- void 関数・コンストラクタ・デストラクタの末尾 `return;` の直前に `// 終了` コメントを記述する．
- 値を返す `return` 文の直前に `// 〜の返戻` コメントを記述する．
- 早期リターンにも `// 〜が有る場合の返戻` 等の形式で適用する．
- switch 文内の各 case の `return` や，１〜２行の単純な関数では省略可能とする．

`return` 文を記述する義務と其の例外は 5.7 に従う．

コメント記号は使用言語の形式へ読み替える．省略可能とする「１〜２行」はコメントを除くコードの構造で判定し，コメントの追加に依って要否を変更してはならない．

## 7.11 useEffect コメント (MUST)

**対象**：JavaScript, TypeScript

`useEffect` の直前に `// 〜時の〜の更新` の形式でトリガー条件と更新対象を記述する．

```typescript
// 正面ファイル又はスケール更新時のシェイプの更新
useEffect((): void => { UpdateShape(frontFile, scale); }, [frontFile, scale]);

// マウント・アンマウント時の処理
useEffect((): void => { Setup(); }, []);
```

## 7.12 JSX コメント (MUST)

**対象**：JavaScript, TypeScript

JSX 内では `{/* コメント */}` を使用する．UI セクションの見出として記述する．

```tsx
function Sample(): JSX.Element {
	return (
		<>
			{/* 3D モデルプレビュー */}
			<div ref={containerRef} className="preview"/>
			{/* 操作パネル */}
			<View style={styles.controlPanel}>{/* ボタン群 */}</View>
		</>
	);
}
```

---

# 第８章 表記統一

**本章はコメント・ドキュメント内の表記にのみ適用する**（コード中の値・式・識別子には適用しない）．HTML の本文テキストと CSS の `content` 値は文書の内容其の物である為，本章の対象とする．

内容への適用は執筆・翻訳・改訂時の表記方針であり，表記統一だけを理由に既存の値・引用・外部データを変更しない．

## 8.1 英単語の混入制限 (MUST)

日本語文中で英単語を混入させない．日本語訳が定着している場合は日本語化する．日本語訳が無い・不自然な場合のみ片仮名化を許容する．

- 「status」→「状態」
- 「issue」→「問題」
- 「prefix」→「接頭辞」
- 「pending」→「保留中」
- 「fetch」→「取得する」
- 「create」→「作成する」
- 「update」→「更新する」
- 「fail」→「失敗する」
- 「render」→「描画する」
- 「disable」→「無効化する」

「risk」「interface」「algorithm」等の日本語訳が不自然な語のみ片仮名化する．例：「リスク」「インターフェース」「アルゴリズム」．

**例外**：以下は対象外で英語の儘記述して良い．

- プログラミング言語の予約語・キーワード（`if`, `for`, `return` 等）
- 変数名・関数名・クラス名のコード内引用
- 外部ライブラリや API の固有名詞（`React`, `tree-sitter` 等）
- URL・ファイルパス・コードリテラル

## 8.2 漢字表記 (MUST)

漢字で表記可能な語は原則漢字で表記する．

形式名詞・形式用言：
- 「〜の上」（×「〜のうえ」）
- 「〜の下」（×「〜のもと」）
- 「〜の時」（×「〜のとき」）
- 「〜の為」（×「〜のため」）
- 「〜の訳」（×「〜のわけ」）
- 「〜の通り」（×「〜のとおり」）
- 「〜の様に」（×「〜のように」）
- 「〜の物」（×「〜のもの」）
- 「〜の他」（×「〜のほか」）
- 「〜の所」（×「〜のところ」）
- 「〜する事」（×「〜すること」）
- 「〜毎」（×「〜ごと」）
- 「〜等」（×「〜など」）
- 「〜有る」（×「〜ある」）
- 「〜無い」（×「〜ない」）
- 「〜出来る」（×「〜できる」）
- 「〜に於いて」（×「〜において」）
- 「〜に就いて」（×「〜について」）

副詞：
- 「敢えて」（×「あえて」）
- 「取分」（×「とりわけ」）
- 「直ちに」（×「ただちに」）
- 「予め」（×「あらかじめ」）
- 「既に」（×「すでに」）
- 「更に」（×「さらに」）
- 「概ね」（×「おおむね」）
- 「殆ど」（×「ほとんど」）
- 「全て」（×「すべて」）
- 「恐らく」（×「おそらく」）
- 「何故」（×「なぜ」）

接続詞：
- 「即ち」（×「すなわち」）
- 「従って」（×「したがって」）
- 「及び」（×「および」）
- 「且つ」（×「かつ」）
- 「併せて」（×「あわせて」）
- 「又」（×「また」）
- 「或いは」（×「あるいは」）
- 「若しくは」（×「もしくは」）
- 「但し」（×「ただし」）
- 「尚」（×「なお」）

代名詞：
- 「此れ」（×「これ」）
- 「其れ」（×「それ」）
- 「彼れ」（×「あれ」）
- 「何れ」（×「いずれ」／「どれ」）
- 「此処」（×「ここ」）
- 「其処」（×「そこ」）
- 「彼処」（×「あそこ」）
- 「何処」（×「どこ」）
- 「此方」（×「こちら」）
- 「其方」（×「そちら」）
- 「彼方」（×「あちら」）
- 「何方」（×「どちら」）

連体詞：
- 「此の」（×「この」）
- 「其の」（×「その」）
- 「彼の」（×「あの」）
- 「何の」（×「どの」）
- 「凡ゆる」（×「あらゆる」）

動詞（「付く」系）：
- 「片付く」（×「片づく」）
- 「気付く」（×「気づく」）
- 「近付く」（×「近づく」）
- 「根付く」（×「根づく」）
- 「基付く」（×「基づく」）

「より」の使分（漢字化対象の３語と，平仮名保持の２用法を文脈で区別する）：
- 「依り」：依拠・依存関係．例：「設定に依り動作が変わる」「型情報に依り判定する」．
- 「拠り」：根拠・出発点．例：「証拠に拠り結論する」「AST に拠り判定する」．
- 「因り」：原因・理由．例：「バグに因り処理失敗」「構文解析エラーに因り巻戻」．
- 平仮名「より」の儘（漢字化対象外）：
	- 比較．例：「A より B が大きい」．
	- 起点．例：「明日より開始」「先頭より走査」．

動詞「ある」（存在動詞）の使分（「有る」と「在る」を文脈で判定）：
- 「有る」（所有・抽象的存在）．例：「必要が有る」「機能が有る」．
- 「在る」（場所的存在）．例：「内部に在る」「次の兄弟ノードが在る」．

助数詞は「箇」を用い，仮名で書かない．

- 「１箇月」（×「１か月」「１ヶ月」「１ケ月」）
- 「１箇所」（×「１か所」「１ヶ所」「１ケ所」）
- 「１箇条」（×「１か条」「１ヶ条」）

**例外**：地名等の固有名詞（「八ヶ岳」「関ヶ原」等）は原文の表記に従う．

**例外（平仮名表記対象）**：

- 動詞活用 + 助動詞「ない」：「〜しない」（×「〜し無い」）．形容詞「〜無い」（存在の否定）とは別語．
- 助動詞「である」「でない」：「〜である」「〜でない」（×「〜で有る」「〜で無い」）．動詞「ある」・形容詞「無い」（存在）とは別語．

## 8.3 ら抜き言葉 (MUST)

可能形はら抜き（ar 抜き）を使用する．

- 「食べられる」→「食べれる」
- 「見られる」→「見れる」
- 「行かれる」→「行ける」

**例外**：法令・告示・報告書等の正式名称は原文の儘引用する為，本節と 8.2 の対象外とする．

**参考**：国語審議会「新しい時代に応じた国語施策について（審議経過報告）」（平成７年）は，ら抜き形で可能を受身・自発・尊敬と区別する事を合理的と認めつつ，共通語の改まった場での使用は認知しかねるとした．本規約は可能の意味が一意に定まる利点を採る．

## 8.4 複合名詞の送仮名省略 (MUST)

複合動詞の連用形から成る名詞では，各動詞の送仮名を省略する．動詞として使用する場合は省略しない．

- 「受け取り」→「受取」（動詞：「受け取る」は其の儘）
- 「書き込み」→「書込」（動詞：「書き込む」は其の儘）
- 「切り替え」→「切替」（動詞：「切り替える」は其の儘）
- 「切り捨て」→「切捨」（動詞：「切り捨てる」は其の儘）
- 「組み合わせ」→「組合せ」（動詞：「組み合わせる」は其の儘）
- 「取り込み」→「取込」（動詞：「取り込む」は其の儘）
- 「問い合わせ」→「問合せ」（動詞：「問い合わせる」は其の儘）
- 「繰り返し」→「繰返」（動詞：「繰り返す」は其の儘）
- 「呼び出し」→「呼出」（動詞：「呼び出す」は其の儘）
- 「読み込み」→「読込」（動詞：「読み込む」は其の儘）
- 「〜有り」→「〜有」
- 「〜入り」→「〜入」
- 「〜済み」→「〜済」
- 「〜付き」→「〜付」
- 「〜無し」→「〜無」
- 「〜向け」→「〜向」

**例外**：法令・告示・報告書等の正式名称は原文の儘引用する為，本節の対象外とする．

**参考**：「送り仮名の付け方」（昭和４８年内閣告示第２号）通則７は慣用が固定した複合名詞に送り仮名を付けないとし（「受付」「受取」「取扱《注意》」「引換《券》」「申込《書》」等），通則６の許容は読み間違える虞の無い場合の省略を認める（「取り扱い（取扱い・取扱）」等）．本規約は此の許容を複合名詞の全体へ及ぼす．

```cpp
// ✅ 名詞（送仮名省略）
// 画像の読込
// データの書込
// モデルの読込

// ✅ 動詞（其の儘）
// 画像を読み込む
// データを書き込む
```

## 8.5 全角・半角の使分 (MUST)

### 判定の対象

本節と 8.6 の判定は，文書記法の構文を除いた実文字列に対して行う．

- 装飾記号 (`**` / `` ` `` / `_`)・見出記号 (`#`)・箇条書記号 (`-` / `*` / `+`)・番号付リストの番号と `.`・引用記号 (`>`)，及び其れらの直後のスペースは対象に含めない．判定は其れらを除いた本文の先頭から始める．
- 例示の可否を示す印（`❌` / `✅` / `⚠`）と其の直後のスペースは，箇条書記号と同じく対象に含めない．判定は印を除いた本文の先頭から始める．
- 表の行はセル毎に独立した判定単位とし，区切り `|` と，`|` に隣接するセル前後のスペースは対象に含めない．
- リンク記法の記号部（`[` / `]` / `(` / `)` 及びリンク先の文字列）は記法其の物であり，全角化するとリンクが機能しない為，括弧・連結記号の規則を適用しない．
- コードスパンの内容は其の儘の文字種で周囲との隣接を判定するが，内側には括弧・連結記号・区切子・単位・スペースの規則を適用しない（`for(初期化; 条件; 更新)` の様な擬似コードも言語の記法通りに書く）．内側のスペースは対象に含めず，外側のスペースとの連続も数えない．
- コードフェンス（`` ``` `` で囲む区画）の内側は，コメント（`//` / `#` / `/* */` / `<!-- -->` に依る行）のみを対象とし，コード其の物は対象外とする．
- 素の URL・ファイルパス（`https://…`，`a/b/c.txt` 等）は全体を一つの半角の語として扱い，内部の区切子・連結記号・スペースには本節と 8.6 の規則を適用しない．
- 節番号・版番号・列挙番号（`2.3`，`PHP 8.1`，`1.`，`A.` 等）及び数値の小数点の `.` は数の一部で区切子ではない為，本節と 8.6 の規則を適用しない．

### 文字種の判定

ASCII の可視文字（U+0021 〜 U+007E），表音文字（ラテン文字・ギリシャ文字・キリル文字・ハングル等の，語の間を空白で区切る文字），数値との間にスペースを入れない単位記号（`%` `‰` `°` `′` `″` 等），及び通貨記号（`$` `¥` `€` `£` `₹` 等）を半角とし，其れ以外の字（仮名・漢字・全角形，及び `→` `↔` `—` `…` `×` `÷` `※` 等の記号）を全角とする．数式・コード片の中では何れも半角．

### 区切子（読点・句点・コロン・セミコロン）

半角・全角の両方が存在する区切子の全種類が対象．読点 `,`／`，`，句点 `.`／`．`，コロン `:`／`：`，セミコロン `;`／`；`．**区切子より前の一区切**の文字種で選ぶ．一区切とは，一つ前の区切子（無ければ行頭）から当該の区切子迄の範囲を指す．括弧の内側は独立した範囲とし，一区切は開き括弧の内側から始まる．

- 一区切が全て半角文字 → 半角 `,` `.` `:` `;`
- 一区切に全角文字が１つでも含まれる → 全角 `，` `．` `：` `；`

```
❌ 対応言語は C，C++，Java である. 文字コード:UTF-8
✅ 対応言語は C, C++, Java である．文字コード：UTF-8
✅ 対象は数値，文字列，真偽値である．
```

**例外**：語を列挙する読点は，文を区切るのではなく語を繋ぐ為，連結記号と同じく**列挙する語の全体**の文字種で選ぶ．此の読点は語を繋ぐ記号であり，一区切を区切らない．和文の読点 `、` と句点 `。` は使用しない（全角の読点は `，`，句点は `．` を用いる）．文全体が数式・英文である場合は其の記法に従う．

### 括弧（丸括弧・角括弧・波括弧・山括弧）

半角・全角の両方が存在する括弧の全種類が対象．丸括弧 `()`／`（）`，角括弧 `[]`／`［］`，波括弧 `{}`／`｛｝`，山括弧 `<>`／`＜＞`．**括弧の内側**の文字種で選ぶ．内側とは，開き括弧から対応する閉じ括弧迄の範囲を指す．

- 内側が全て半角文字 → 半角 `(` `)` `[` `]` `{` `}` `<` `>`
- 内側に全角文字が１つでも含まれる → 全角 `（` `）` `［` `］` `｛` `｝` `＜` `＞`

```
❌ 説明 (識別子の意味)，項目 [対象範囲]，集合 {要素群}
✅ 説明（識別子の意味），項目［対象範囲］，集合｛要素群｝
✅ 指示子 (foo)，配列 [foo]，型 (`int`)
```

**例外**：全角のみで半角形が無い括弧（鍵括弧 `「」`，二重鍵括弧 `『』`，隅付括弧 `【】`，山形括弧 `〈〉` 等）は内側の文字種に関わらず全角を使用する．

### 連結記号（スラッシュ・ハイフン・中黒等）

半角・全角の両方が存在する連結記号の全種類が対象．スラッシュ `/`／`／`，ハイフン `-`／`－`，中黒 `･`／`・`，算術演算子 `+`／`＋`・`*`／`×`・`/`／`÷`，波ダッシュ `~`／`〜`，等号 `=`／`＝`，アンパサンド `&`／`＆`等．**連結対象の全体**の文字種で選ぶ．連結対象とは，連結記号が繋ぐ語の全てを指す．

- 連結対象が全て半角文字 → 半角（`/` `-` `･` `+` `*` `~` `=` `&` 等）
- 連結対象に全角文字が１つでも含まれる → 全角（`／` `－` `・` `＋` `×` `÷` `〜` `＝` `＆` 等）

```
❌ 比較演算子/代入演算子，配列-連結リスト，行数+桁数，１~３行，幅=４，型&値
✅ 比較演算子／代入演算子，配列・連結リスト，行数＋桁数，１〜３行，幅＝４，型＆値
✅ A / B / C, if-else, a + b, 1~3, x=4, a&b
```

### 数字と単位

単位の文字種に，数字及び数量を表す英字（`N`, `M`, `X` 等の変数記号）の文字種を合わせる．

- 単位が半角（`px`, `em`, `Hz`, `KB`, `ms`, `%`, `‰`, `°`, `′`, `″` 等）→ 数字・数量英字も半角．数値と単位の間に半角スペースを入れる．但し，`%`，`‰`，角度の `°` / `′` / `″` にはスペースを入れない．例：`128 px`, `60 Hz`, `5 ms`, `N px`, `50%`, `30°`．
- 単位を伴わずに全角の語（漢字・仮名）へ直に続く数字も全角とする．例：「１６進」「２進」「８進」「２冪乗」「０の時」．半角の儘では全角との間に半角スペースが要り，語が分断される．数式・コード片の中の数値，及び句読点や括弧の直前の数値は対象外とする．
- 単位が全角（`個`，`行`，`文字`，`回`，`本`，`メートル` 等の漢字・片仮名表記，及び `㌕`，`㏄`，`㎏`，`㎝`，`㎜`，`㍉`，`㍍`，`㎞`，`㎡` 等の単位合字）→ 数字・数量英字も全角．スペースは入れない．例：「１個」「２行」「１２８文字」「１０㎏」「５㎝」「３㌕」「Ｎ回」「Ｍ個」「Ｘ文字」．

## 8.6 スペースの入れ方 (MUST)

半角と全角が接する箇所のスペースを定める．判定の対象は 8.5 に従う．**スペースを入れない条件が，入れる条件に優先する．**

**全体**

- スペースは２つ以上連続させない（インデント・整列の為の詰物も含む．インデントはタブで行う）．
- 行頭と行末にスペースを置かない．但しブロックコメントの継続行の `*` の前（` * ` の形）の様に，コメントの記法が定める行頭のスペースは此の限りではない．

**スペースを入れない**

- 全角文字同士が接する箇所（日本語は分かち書きをしない）．
- 全角の区切子・括弧類・連結記号に接する箇所（字形の前後に余白を含む為，隣が半角の語・区切子・括弧であっても入れない）．
- 半角の区切子の前，及び半角の括弧の内側．
- 半角の区切子・括弧の外側の隣が，区切子又は括弧である箇所．

**スペースを１つ入れる**（前項のスペースを入れない条件に該当する箇所では，本項の条件に該当する場合でもスペースを入れない）

- 半角の語（識別子・コード片・英単語・数字，単位・通貨記号を含む数量，及び `C++` `C#` `F#` `.NET` の様に記号を含む固有名詞）と全角文字が接する箇所．
- 半角の区切子の後ろ．
- 半角の括弧の外側．

**例外**：見出の章番号と題名の区切り（「第１章 基本方針」の形．目次の項目と本文中の引用を含む），及び矢印（`→` `↔`）・ダッシュ（`—`）の前後は可読性の為に半角スペースを許可する．コードスパンの外周に接するスペースは，隣接する文字が全角であっても入れて良い（コードスパンが連続する場合は区切りのスペースを必ず入れる．除去すると記法が壊れる）．

```
❌ 関数(foo)を呼ぶ
❌ 関数 ( foo ) を呼ぶ
❌ ASCIIの記号
❌ 型  (`int`)  を返す
❌ 型 (`int`) を返す (MUST) ．
❌ 虚数の i ・虚部の j
❌ 括弧 （真偽値，文字列）
✅ ASCII の記号
✅ 関数 (foo) を呼ぶ
✅ 関数（識別子）を呼ぶ
✅ 呼出 (f(x)) を評価する
✅ 型 (`int`) を返す (MUST)．
✅ 虚数の i・虚部の j
✅ 括弧（真偽値，文字列）
```

---

# 第９章 例外規定

例外は，合理的な理由が有る場合に限り許可する．

- 可読性が明確に向上する場合
- パフォーマンス要件が有る場合
- 外部ライブラリ・フレームワークの API が規約と矛盾する場合

但し，本章を理由に 1.3 の上位原則と意味保存を緩めてはならない．

例外を適用する場合は，レビュー時にコメントで理由を説明しなければならない．

---

# 第１０章 運用

- 自動整形はコードの意味を保持し，同じ構文構造の入力であれば空白・改行・通常コメントの差異に結果を依存させず，再適用しても結果を変化させない．整形の失敗時は元のファイルを保持する．第９章の規定を理由に此れらを緩和してはならない．
- 本規約は自動整形ツール及びリンターと整合させなければならない．
- 推奨ツール：**Shave Format**（本規約に基付く１６言語対応の自動整形・静的検査ツール）
	- 対応言語：C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python, JSON, HTML, CSS（SCSS を含む）
	- CLI: `shavefmt -w <file>`
	- マーケットプレイス：https://marketplace.visualstudio.com/items?itemName=foogoo.shavefmt
	- リポジトリ：https://github.com/FugoShimizu/ShaveFormat
- 補助ツール（各言語特有の追加整形が必要な場合）：
	- C, C++: clang-format
	- JavaScript, TypeScript: dprint
	- Ruby: RuboCop（Shave Format 適用後に実行）
- ツールの設定が本規約と矛盾する場合は，本規約に整合する設定を優先する．

---

# 第１１章 付録Ａ（典型アンチパターン）

本章の例示コメントには，対比の為に節番号を添えている．此れは規約本文の例示に限った記法であり，実際のソースコードでは 7.4 に従って節番号を記載しない．

### フォーマット

```cpp
// ❌ 括弧前スペース (3.1)
if (Ready) return;
// ✅
if(Ready) return;

// ❌ { 前改行 (2.5)
if(Ready)
{
	Prepare();
	Start();
}
// ✅
if(Ready) {
	Prepare();
	Start();
}

// ❌ 演算子周りのスペース欠如 (2.9)
int x=a+b;
// ✅
int x = a + b;

// ❌ &/* が型名側 (2.11)
int* Ptr;
const int& Ref = x;
// ✅
int *Ptr;
const int &Ref = x;

// ❌ F 接尾辞無 (2.8)
float Pi = 3.14;
// ✅
float Pi = 3.14F;

// ❌ sizeof 括弧無 (2.12)
int n = sizeof arr / sizeof arr[0];
// ✅
int n = sizeof(arr) / sizeof(arr[0]);

// ❌ アクセス修飾子の順序 (2.7)
class Foo {
public:

	void Bar();

private:

	int Baz;
};
// ✅
class Foo {
private:

	int Baz;

public:

	void Bar();
};
```

### 制御構文

```cpp
// ❌ 単一文に波括弧 (3.2)
if(Cond) {
	return;
}
// ✅
if(Cond) return;

// ❌ 中間ループに波括弧 (3.6)
for(int i = 0; i < h; ++i) {
	for(int j = 0; j < w; ++j) Process(i, j);
}
// ✅
for(int i = 0; i < h; ++i) for(int j = 0; j < w; ++j) Process(i, j);

// ❌ 冗長な空本体ループ（3.6，Bottom は非負整数で添字は範囲内）
while(Bottom > 0) {
	--Bottom;
	if(Mask[Bottom]) break;
}
// ✅
while(Bottom && !Mask[--Bottom]);
```

```ruby
# ❌ 単一文本体の冗長な通常形式 (3.8)
if cond
	return
end
# ✅
return if cond

# ❌ case を通常形式で展開 (3.8)
case x
when 1
	"a"
when 2
	"b"
else
	"c"
end
# ✅
case x when 1 then "a" when 2 then "b" else "c" end
```

### 式・演算

```cpp
// ❌ 順序違反（中間値も整数型の範囲内，4.1）
z = 1 - 3 * y + x * x;
// ✅
z = x * x - 3 * y + 1;

// ❌ += 1 (4.9)
i += 1;
// ✅
++i;

// ❌ 後置インクリメント・戻り値不使用 (4.9)
i++;
// ✅
++i;

// ❌ bool 反転に ! を使用 (4.8)
Flag = !Flag;
// ✅
Flag ^= true;

// ❌ 三項演算子を使わない条件代入 (4.10)
std::string Msg;
if(Ok) Msg = "成功";
else Msg = "失敗";
// ✅
std::string Msg = Ok ? "成功" : "失敗";

// ❌ 整数境界に >= を使用 (4.11)
if(Score >= 60) return "合格";
// ✅
if(Score > 59) return "合格";

// ❌ ==0 / !=0 で 0 判定 (4.6)
if(Count == 0) return;
if(Count != 0) Process(Count);
// ✅
if(!Count) return;
if(Count) Process(Count);

// ❌ bool に 0/1 代用 (4.7)
int IsReady = 1, Done = 0;
// ✅
bool IsReady = true, Done = false;
```

```typescript
// ❌ クランプの順序違反 (4.5)
const v = Math.max(lower, Math.min(value, upper));
// ✅
const v = Math.min(Math.max(value, lower), upper);

// ❌ 同じ数値型同士に === (4.12)
if(index === -1) return null;
if(value === null || value === undefined) return;
// ✅
if(index == -1) return null;
if(value == null) return;
```

### 変数・型

```cpp
// ❌ 無意味な変数 (5.1)
int x = Get();
return x;
// ✅
return Get();

// ❌ auto (5.4)
auto Val = GetValue();
// ✅
int Val = GetValue();

// ❌ void 関数で return 省略 (5.7)
void Setup() {
	Init();
}
// ✅
void Setup() {
	Init();
	// 終了
	return;
}

// ❌ 非負整数値を int で宣言 (5.5)
int ChangedCount = 0;
int ByteOffset = 100;
// ✅
size_t ChangedCount = 0;
uint32_t ByteOffset = 100;
```

```typescript
// ❌ any 乱用 / 型注釈漏れ (5.4)
let count;
const getName = () => "hello";
function handle(data: any) {}
// ✅
let count: number;
const getName = (): string => "hello";
function handle(data: unknown): void {}

// ❌ 値が確定する変数を let で宣言 (5.8)
let name;
if(user.isAdmin) name = "管理者";
else name = user.displayName;
// ✅
const name = user.isAdmin ? "管理者" : user.displayName;
```

### 言語固有

```cpp
// ❌ C スタイルキャスト (6.4)
int n = (int)Str.length();
// ✅
int n = static_cast<int>(Str.length());

// ❌ NULL (6.7)
int *Ptr = NULL;
// ✅
int *Ptr = nullptr;

// ❌ using namespace (6.6)
using namespace std;
// ✅
std::cout << "hello" << std::endl;

// ❌ ヘッダ内に関数定義 (6.3)
class Foo {
	void Bar() {
		DoSomething();
	}
};

// ✅ ヘッダには宣言のみ
class Foo {
private:
	void Bar();
};

// ❌ #define に依る定数定義 (6.5)
#define MAX_SIZE 100
#define PI 3.14159F
// ✅
constexpr int MaxSize = 100;
constexpr float Pi = 3.14159F;
```

```python
# ❌ for ループで内包表記を代用 (6.13)
result = []
for x in items:
	if x.is_valid():
		result.append(x.value)
# ✅
result = [x.value for x in items if x.is_valid()]
```

```tsx
// ❌ 数値条件で 0 描画クラッシュ (6.11)
{items.length && <List items={items} />}
// ✅
{!!items.length && <List items={items} />}
```

```typescript
// ❌ インタフェース本体にセミコロン (2.16)
interface Props {
	firstReasonablyLongPropertyNameToPreventCollapse: string;
	secondReasonablyLongPropertyNameToPreventCollapse: number;
}
// ✅
interface Props {
	firstReasonablyLongPropertyNameToPreventCollapse: string,
	secondReasonablyLongPropertyNameToPreventCollapse: number
}
```

```ruby
# ❌ 単一文を begin/rescue/end で包む (6.8)
value = begin
	JSON.parse(str)
rescue
	{}
end
# ✅
value = JSON.parse(str) rescue {}

# ❌ １行定義の不要改行 (6.12)
def double(x) =
	x << 1
# ✅
def double(x) = x << 1
```

```python
# ❌ キーワード引数にスペース (2.9 I)
func(key = value, timeout = 30)
# ✅
func(key=value, timeout=30)
```

### 意味を変える変換

| 対象 | 避ける変更 | 保持する形・条件 |
|---|---|---|
| C | C17 の `(void)` を `()` へ変更 | 引数検査を保つ (5.6) |
| C++ | `9.0F / 5.0F` を `0.2F * 9.0F` へ変更 | 丸めが異なる為，除算を保持 (4.3) |
| C# / Java | 局所定数に `static` を追加 | C# は `const`，Java は `final` (5.9) |
| Go | `import "C"` 直前の宣言コメントを削除 | cgo 前置宣言を保持 (7.1) |
| Rust / Swift | 網羅済の分岐へ既定分岐を強制 | 未処理の値が有る時のみ補う (3.5) |
| Kotlin / Swift | 標準添字を無条件に符号無へ変更 | API が要求する `Int` を使用 (5.5) |
| PHP | null を返す初期化に `??=` のみ使用 | 初期化済状態を別に保持 (5.10) |
| JS / TS | `NaN !== 0` を `!!NaN` へ変更 | 真から偽へ変わる為，比較を保持 (4.6) |
| TSX | `<T,>` のカンマを削除 | JSX との曖昧性を避けるカンマを保持 (2.14) |
| Ruby | `'#@token'` を `"#@token"` へ変更 | 非補間の値を保持 (2.13) |
| Python | `if` の `:=` が外へ漏れないと仮定した変更 | 外側スコープへの束縛を考慮 (5.2) |
| JSON | 大整数を binary64 へ丸める変更 | 受取側と合意した精度を保持 (6.14) |
| HTML | 断片へ `html`・DOCTYPE を追加 | 断片の儘埋め込む (6.15) |
| CSS / SCSS | 宣言又は取込を無条件に並替 | カスケード・設定・評価順を保持 (6.1, 6.16) |

### コメント

```cpp
// ❌ ドキュメントコメント漏れ (7.8)
FVector2f CalculateElevation(const FVector2f &Location) {
	// 当該地点の標高の返戻
	return Heightmap.Sample(Location);
}
// ✅
/**
 * 標高を算出する
 * @param Location 当該地点の平面座標
 * @return 当該地点の標高
 */
FVector2f CalculateElevation(const FVector2f &Location) {
	// 当該地点の標高の返戻
	return Heightmap.Sample(Location);
}

// ❌ return コメント漏れ (7.10)
FIntPoint GetConnection(const FIntPoint &Position) {
	const FIntPoint *Ptr = ConnectionMap.Find(Position);
	if(Ptr) return *Ptr;
	FIntPoint Result = Derive(Position);
	return ConnectionMap.Add(Position, Result);
}
// ✅
FIntPoint GetConnection(const FIntPoint &Position) {
	// マップに接続先が有る場合の返戻
	const FIntPoint *Ptr = ConnectionMap.Find(Position);
	if(Ptr) return *Ptr;
	// 接続先の導出
	FIntPoint Result = Derive(Position);
	// マップへの接続先追加と返戻
	return ConnectionMap.Add(Position, Result);
}
```

---

# 第１２章 付録Ｂ（競技プログラミング用テンプレート）

```cpp
#include <bits/stdc++.h>

using namespace std;

int main() {
	cin.tie(nullptr)->sync_with_stdio(false);
	cout << "HelloWorld!" << endl;
	return 0;
}
```
