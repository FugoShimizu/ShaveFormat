# Shave Format 字句間空白仕様

本書は，投影・構文正規化後の字句の役割から，`Canonicalizer` が字句間の密着・空白を決定する規則を規定する．解析器の名前付／無名の区分ではなく，構文の役割と `SyntaxFacts` に基付いて `FormatIR` を作る．

整形全体の契約は [整形仕様](formatter_spec.md)，書式と表記は [コーディング規約](../CODING_STANDARDS.md) に従う．幅に依る改行の採否は [行分割仕様](line_breaking_spec.md) で定める．本書の節は「本書」，整形仕様の節は「整形仕様」，コーディング規約の節は「規約」として参照する．

---

# 目次

- **[第１章 適用範囲と情報境界](#第１章-適用範囲と情報境界)**
	- [1.1 対象と位置付](#11-対象と位置付)
	- [1.2 情報境界](#12-情報境界)
	- [1.3 例示の範囲](#13-例示の範囲)
- **[第２章 決定手順](#第２章-決定手順)**
	- [2.1 境界の表現](#21-境界の表現)
	- [2.2 判定の優先順位](#22-判定の優先順位)
- **[第３章 構文上の役割別規則](#第３章-構文上の役割別規則)**
	- [3.1 演算子](#31-演算子)
	- [3.2 コロン](#32-コロン)
	- [3.3 カンマとセミコロン](#33-カンマとセミコロン)
	- [3.4 括弧と呼出](#34-括弧と呼出)
	- [3.5 語と後続字句](#35-語と後続字句)
- **[第４章 構文別の密着と区切り](#第４章-構文別の密着と区切り)**
	- [4.1 宣言と型の境界](#41-宣言と型の境界)
	- [4.2 式と呼出の境界](#42-式と呼出の境界)
- **[第５章 本文と前処理の境界](#第５章-本文と前処理の境界)**
	- [5.1 リテラルと補間](#51-リテラルと補間)
	- [5.2 JSX と HTML](#52-jsx-と-html)
	- [5.3 CSS と SCSS](#53-css-と-scss)
	- [5.4 前処理指令とマクロ](#54-前処理指令とマクロ)
- **[第６章 投影の対応と受入条件](#第６章-投影の対応と受入条件)**
	- [6.1 解析木から構文上の役割への対応](#61-解析木から構文上の役割への対応)
	- [6.2 受入条件](#62-受入条件)
	- [6.3 検体の保守](#63-検体の保守)

---

# 第１章 適用範囲と情報境界

## 1.1 対象と位置付

対象は C, C++, C#, Java, Go, Rust, Kotlin, Swift, PHP, JavaScript, TypeScript, Ruby, Python, JSON, HTML, CSS と其れらの対象方言である．JSX, TSX, JSONC 及び SCSS の区別は整形仕様第２章に従う．共通の解析器を使う事を，同じ言語上の意味を持つ根拠にしない．

## 1.2 情報境界

`SourceProjector` は元の空白の意味，解析器が省いた字句及び名前付／無名の区分に依らない構文の役割を確定する．`SyntaxNormalizer` は括弧・演算・宣言等を正規化し，`Canonicalizer` は其の結果と意味を持つ境界だけを読む．

本書の空白決定は言語識別・文法定義・解析器固有の節点名・原文の空白量・解析木のバイト範囲・行幅・コメントの状態を入力にせず，原文の隙間を読み直す経路を設けない．コードとコメントの前置区切り，及びコメント記号と本文の区切りは整形仕様第７章で扱い，ソース本体の空白・改行・空行へ影響させない．コメントでない標準の指令・属性は構文として扱う（整形仕様 3.5, 4.3, 4.5, 6.2）．

## 1.3 例示の範囲

表のコード片は，空白の境界に関する部分例である．括弧の除去，型の修正，文書化又は静的検査迄適用した完全な出力例ではない．

---

# 第２章 決定手順

## 2.1 境界の表現

| 表現 | 意味 | 中間表現への反映 |
|---|---|---|
| 密着 | 当該境界へ生成する空白を置かない． | 字句又は断片を直に続ける |
| 空白 | 同行の字句を空白１個で区切る． | `Space` 又は平坦時に空白を出す `SoftBreak` |
| 必須改行 | 構文・意味・構造に依って行を分ける． | `HardBreak` 又は対応する必須の展開形 |
| 選択可能な改行 | 同行形と改行形の何方も合法で，行分割の候補に為る． | 両方の表現を持つ `SoftBreak` と所属する `Group` |
| 字句の分離 | 密着又は空白では同じ字句列へ戻らない境界を保つ． | 本文を持たない `LexicalSeparator` |
| 内容の保持 | 値又は照合対象の空白を含む内容を再現する． | 意味を表す字句・境界又は `Verbatim` |

「空白」は行分割の許可を意味しない．Swift の演算子，Ruby の呼出形式等は，同じ空白に見えても改行で読みが変わり得る．改行を選べる位置だけを `SoftBreak` にする．

同じ境界へ複数の規則が空白を要求しても，空白を重複して出さない．折返を選んだ境界へ平坦時の区切空白を残して行末空白にしない．逐語内容の空白を此の除去対象へ含めない．

## 2.2 判定の優先順位

次の順で境界を決める．補助関数の偶然の分岐順又は字面だけの集合照合を優先順位にしない．

1. 逐語区画，文境界，必須の密着・空白・改行禁止等の意味保存条件を守る．
2. 投影済の構文上の役割と制約から，コロン・演算子・括弧・宣言・本文等の共通規則を適用する．
3. 前後の字句が別の字句へ融合しない事，語・値・型の解釈を変えない事を確認する．
4. 残る通常の境界へ，識別子同士を区切る等の共通規則を適用する．

規則は境界の役割と必要な事実で索引化し，該当する規則だけを上記の順で評価する．同じ結果の定義は共有し，同順位で相反する定義は登録時の不整合とする．言語毎の全規則を順に試す方式や，言語名だけを隠した関数呼出へ置き換えない．

役割を確認出来ない境界へ，既定で空白を挿入して読みを推測しない．投影に必要な情報が欠ける場合は入力側で解決し，失敗は整形仕様 4.3, 8.3 に従って扱う．

字句の融合を防ぐ為に括弧が必要な場合は，構文正規化又は中間表現の合法な選択肢として表す．単項の記号を被演算子から離す，原文の隙間を復元する，出力後に記号を補う等の方法で取り繕わない．

---

# 第３章 構文上の役割別規則

## 3.1 演算子

| 役割 | 空白 | 代表例・条件 |
|---|---|---|
| 二項の算術・比較・論理・ビット・シフト | 前後 | `a + b`, `a ** b`, `a // b`, `a < b`, `a && b`, `a and b`, `a << b` |
| 三項 | `?` と `:` の前後 | `c ? a : b` |
| `null` 合体・エルビス | 演算子全体の前後 | `a ?? b`, `a ?: b`. `?:` の内部は分断しない． |
| 型の比較・変換・包含 | 語又は演算子全体の前後 | `x as T`, `x is T`, `x !in xs`, `x instanceof T` |
| 型の合併・交差 | 前後を密着 | `A\|B`, `A&B`, `(A&B)\|null`. 型の構成要素を結ぶ記号とし，値のビット演算及び参照・借用の単項記号と区別する． |
| ラムダ・分岐・戻値型等のアロー | 前後 | `(x, y) => x + y`, `x -> x`, `fn f() -> T`. メンバ参照の `->` と区別する． |
| 前置の記号 | 被演算子へ密着 | `!x`, `-x`, `~x`, `*p`, `&x`, `++i`, `...xs`, `<-ch` |
| 前置の語 | 被演算子の前を区切る | `not x`, `await value`, `delete value`. 専用の括弧構文は本書 3.5 に従う． |
| 後置の記号 | 被演算子へ密着 | `i++`, `x!`, `x!!`, `expr?`. 記号の合法性と役割を言語毎に確定する． |
| メンバ参照・スコープ・安全呼出 | 原則として前後を密着 | `obj.member`, `ptr->member`, `obj?.member`, `std::name`, `obj&.method`, `$obj?->method()` |
| 記号の範囲 | 両側の値へ密着 | `a..b`, `a...b`, `a..=b`, `a..<b`. 語で表す範囲の区切りとは区別する． |
| C++ のメンバポインタ演算 | 前後 | `obj .* ptr`, `obj ->* ptr`. 通常のメンバ参照と同一視しない． |
| Go の送信・短縮宣言 | 前後 | `ch <- value`, `x := value`. 受信・チャネル型の `<-` と区別する． |
| PHP の文字列連結 | 前後 | `$a . $b`. `.` をメンバ参照として密着させない． |
| Python の代入式 | 前後 | `(x := value)`. キーワード引数の `=` と区別する． |

同じ親節点に `:` / `,` / `;` が在っても，全てを二項演算子として処理しない．各字句を演算・要素区切・型注釈・文終端等の役割へ分ける．

記号の単項同士が `--` / `++` / `&&` / `!~` / `!!` 等の別字句へ融合する場合は，整形仕様 5.4 に従う．融合しない言語の連続単項と，１個の演算子になる言語を区別する．

JavaScript / TypeScript と Python では，小数点も指数も持たない十進整数の直後の `.` を，小数点と見誤らせない境界にする (`1 .toString()`, `6 .real`)．数値内部へ空白を入れる規則ではない．

### 等号の役割

| 役割 | 空白 | 例 |
|---|---|---|
| 代入・初期化・仮引数の既定値 | 前後に１個 | `x = value`, `def f(x = 0, y: int = 1):` |
| 複合代入 | 前後に１個 | `x += value`, `x ??= value` |
| 呼出側のキーワード引数・名前付実引数 | 前後を密着 | `f(key=value)` |
| キーワードパターン | 前後を密着 | `case C(x=1)` |
| 属性値 | 前後を密着 | `name="value"` |
| 指示文の設定値 | 前後を密着 | `declare(strict_types=1);`, `declare(ticks=1);` |

## 3.2 コロン

コロンは字面でなく用途で分類する．型注釈・値・継承・本体の開始・ラベル・選択子を同じ既定規則へ落とさない．

| 用途 | 前 | 後 | 代表例 |
|---|---|---|---|
| 型注釈・欄・仮引数・戻値型 | 密着 | 空白 | `x: T`, `fun f(): Int`, `pub x: i32` |
| 辞書・オブジェクト・鍵付要素・名前付実引数 | 密着 | 空白 | `key: value`, `f(name: value)`, `[String: Int]` |
| 型の制約・関連型 | 密着 | 空白 | `T: Protocol`, `associatedtype X: Y`, `: Clone + Debug` |
| 三項・継承・範囲 for・C++ の初期化子 | 空白 | 空白 | `c ? a : b`, `class A : B`, `for(int x : xs)`, `Foo() : x(1)` |
| ラベル・節見出 | 密着 | 後続の配置規則に従う． | `case 1:`, `default:`, `label:`, `public:` |
| Kotlin の注釈の適用先 | 密着 | 密着 | `@file:JvmName("Name")` |
| PHP / Python の本体の開始 | 密着 | 本体の構造に従う． | `if($x):`, `if x:`, `def f():` |
| Ruby のキーワード仮引数 | 密着 | 値が続く時だけ空白 | `name:`, `age: 0` |
| Python のラムダ | 密着 | 空白 | `lambda x: x`. 複合文の本体と同じ必須改行にしない． |
| CSS の宣言・特性の問合せ | 密着 | 空白 | `property: value`, `(prop: value)` |
| CSS の擬似クラス・擬似要素 | 密着 | 密着 | `:hover`, `::before` |

Swift のクラス・プロトコルの継承は前後に空白を置き，型仮引数や関連型の制約とは区別する．区切りの後に値が無い場合，又は改行・閉じ字句へ続く場合は，空の値の為の余分な空白を生成しない．但し CSS の空白自体が値である宣言は本書 5.3 に従う．

## 3.3 カンマとセミコロン

カンマとセミコロンの前は密着させ，同じ行に次の要素が続く時は後へ空白１個を置く．直後が区切り・閉じ又は改行の場合は，要素を区切る為の空白を追加しない．非空の波括弧の内側に必要な空白は本書 3.4 で別に判断し，同じ境界へ重ねない．

`f(a, b)`, `for(int i = 0; i < n; ++i)`, `for(;;)` の様に，次の要素の有無を区別する．Python の単純文の列は構造上の行へ分け，`;` の後に空白を置いて同じ行へ残すだけの処理にしない．

カンマ・セミコロン自体の追加・除去は，本書の空白処理では行わない．文終端・末尾カンマ・単一要素の組・TSX の型仮引数・配列の空要素等は整形仕様 5.2，Go の条件付カンマは整形仕様 6.4 に従う．

## 3.4 括弧と呼出

| 境界 | 規則 | 代表例 |
|---|---|---|
| `(` / `[` の直後，`)` / `]` の直前 | 密着 | `f(x)`, `a[i]` |
| 関数・メソッドの名前と実引数・仮引数の `(` | 密着 | `f(x)`. 同じ字面でも型・式の別の包装は区別する． |
| 添字の対象と `[` | 密着 | `a[i]` |
| 同行で非空の `{ ... }` | 内側へ空白１個 | `{ key: value }` |
| 空の `{}` | 内側を密着 | `if(c) {}`, `struct {}` |
| 複合リテラルの型と `{` | 密着 | C の `(T){ 1 }`，Go の `T{ 1 }`． |
| 型本体の開始 | 前の語と `{` を区切る． | `struct {}`, `interface {}`. 成員の有無で変えない． |
| 型引数・型仮引数の `<` / `>` | 内側を密着 | `Name<T>`. 内側のカンマは要素区切りとして扱う． |
| JSX の式・文字列の補間 | 内側を密着 | `{value}`, `${value}`, `#{value}` |

本体を平坦にするか展開するかは，構造規則と行分割仕様で決める．本体を表す解析器の節点名だけで常に展開せず，同行の配列・辞書等の空白規則だけで必須の本体を畳まない．

Ruby の括弧無呼出で，最初の実引数が括弧で始まる形は，呼出名と引数を包む括弧との密着と区別する (`p (1..6).to_a`)．構文正規化後も必要な区切りを `SyntaxFacts` から生成し，原文の空白量は読まない．

## 3.5 語と後続字句

制御構文が条件等を括弧で包む言語では，制御語と其の構文の `(` を密着させる (`if(x)`, `for(...)`, `catch(...)`, `if constexpr(x)`)．Go / Rust / Swift / Ruby / Python 等で後続の括弧が条件の一部である場合は，語と式を区切る．

`return`, `throw`, `yield`, `await`, `delete`, `new`, `case`, `of` 等と後続の式・型・照合は，語を繋がない必要な空白で区切る．但し，語の綴りが同じでも呼出又は専用の構文として括弧へ密着する物を区別する．

| 文脈 | 形・条件 |
|---|---|
| 語と括弧又は角括弧で始まる値 | `return (a, b)`, `for(const x of (a, b))`, `next [a, b]` 等，合法な文脈で区切る． |
| 修飾・属性・生存期間等と括弧で始まる型 | `suspend () -> Unit`, `@escaping (Int) -> Void`, `&'a (i32, i32)` |
| 宣言の語と名前，語の並び | `class A`, `const int` 等を区切る．予約語という理由だけで前後を全て空白にしない． |
| C / C++ の `sizeof` | `sizeof(X)` |
| C# の型を取る `typeof` | `typeof(T)` |
| Ruby の `defined?` | `defined?(x)` |
| Ruby の `not` | `not(x)` と，語として式を取る `not (x)` の構文を区別する．実引数へ置く `g((not x))` 等の必要な包装は保持する（規約 4.4）． |

修飾語の後の改行で別の宣言になる形，`await` が識別子である形等は入力側で区別する．空白を配置しながら解析器の誤読を補修する工程を設けない．

---

# 第４章 構文別の密着と区切り

## 4.1 宣言と型の境界

次は共通の字面分類だけでは役割を決めれない境界である．記号内部の密着と，其の前後の語の区切りを別々に扱う．

| 言語 | 対象 | 形・条件 |
|---|---|---|
| C / C++ | ポインタ・参照の宣言子 | `int *p`, `const int &r`. 記号を名前側へ付ける．ポインタ自体の修飾は `T *const p` とし，`const` と名前を空白で区切る． |
| C / C++ | 型と括弧付宣言子 | `int (*p)`, `int (*f)(int)`. 型との間を区切る． |
| C / C++ | 属性の囲み | `[[nodiscard]]` の記号と属性名を分断しない． |
| C# | 暗黙型ラムダの修飾・戻値型・属性 | `async (x, y) => x`, `int (x) => x`, `[A] (x) => x`. 呼出の括弧と見誤らない． |
| C# | 分解宣言と構築子制約 | `var (a, b)`, `where T : new()` |
| Java | 可変長仮引数の型 | `T... args`. `...` を型から切り離さない． |
| Go | 関数の戻値の組 | `func(int) (int, error)`. 仮引数を閉じる `)` の後を区切る． |
| Go | チャネルの向き | `chan<- int`, `<-chan int`. 送信・受信の式と区別する． |
| Rust | 生存期間 | `'a` を分断しない． |
| Rust | 参照と型の語 | `&self`, `&mut value`, `dyn Trait`. `&` の密着と語の後の空白を区別する． |
| Kotlin | 型位置の注釈 | `@Composable () -> Unit` の括弧を注釈の実引数へ繋がない． |
| Kotlin | 注釈と適用先 | `@file:JvmName(...)` 等の内部を密着させる．別の名前や注釈との必要な区切りは残す． |
| Kotlin | 数値の接尾辞 | `100L`, `1.5F`, `100UL`. 数値から切り離さない． |
| Swift | 型の接尾記号 | `Foo!`, `Int?`. 型注釈の内側を二項演算として広げない． |
| Swift | 型変換と並びの型 | `x as! [Int: Int]`, `x as? [T]`. `as!` / `as?` と型を区切る． |
| PHP | 属性 | `#[Attribute(...)]` の開始記号を分断しない． |
| JavaScript / TypeScript / Ruby / Python 等 | 可変長・展開・残余 | `...rest`, `**opts`, `*args`, `**kwargs`. 単項・乗算・型の記号と役割を区別する． |

型引数と比較，キャストと二項式等の曖昧さは，整形仕様 4.2, 5.4 に従って入力側と構文正規化側で扱う．型又は式を誤認した儘，山括弧の空白だけを修正しない．

## 4.2 式と呼出の境界

| 言語 | 対象 | 形・条件 |
|---|---|---|
| C / C++ / C# / Java / PHP 等 | 型変換と値 | 正しく型変換と確定した `(int)x` 等では型の閉じと値を密着させる．名前を型と推測しない． |
| Rust | マクロ呼出・定義 | `println!` を密着させ，`macro_rules! name` は定義名との間を区切る．字句列の内部は本書 5.4 に従う． |
| Kotlin | 否定付の包含・型判定 | `!in`, `!is` を一つの演算子として密着させ，其の外側を区切る． |
| Kotlin | 上位型・ラベル | `super<A>@O`, `return@outer` を分断しない． |
| Swift | 前置・後置・中置の区別 | 前置・後置の `?` / `!` と値の隣接，範囲，任意の連鎖を投影済の役割通りに保つ． |
| Swift | 照合の語 | `case let .foo(x)` の `let` と `.` の間を区切る． |
| PHP | 参照 | `&$x`, `&...$x`, `function &f()`, `$ref = &$obj->cache`. 参照の記号を対象へ付ける． |
| PHP | 名前空間・変数 | `App\Service`, `\App\Model\Item`, `$name` の内部を分断しない． |
| JavaScript / TypeScript | タグ付テンプレート | タグとテンプレートを密着させる． |
| JavaScript / TypeScript | 生成器の記号 | `function* gen()`. `*` と語・名前の役割を通常のポインタ宣言へ読み替えない． |
| Ruby | ブロック実引数 | `arr.map(&:to_s)` の `&` と後続を密着させる． |
| Ruby | ラムダのアロー | `->(&b) {}` 等，残した仮引数の括弧は `->` へ密着させる． |
| Ruby | 分解を表す末尾カンマ | `\|a,\|` の閉じの前へ空白を入れない． |

Swift の空白に依る演算子の役割，Ruby の符号と数値・呼出の関係等は，元の隣接を調べるのではなく投影時の事実として保持する．`x!=y` 又は `- 1.abs` を，字面の一括整形で別の演算へ変えない．

---

# 第５章 本文と前処理の境界

## 5.1 リテラルと補間

引用符・接頭辞・接尾辞・区切文字及び本文の字句を，通常の語同士として空白で離さない．`"`，`'`，逆引用符，`${`，`#{` 等は，所属するリテラル又は補間の構造として扱う．

通常の本文，生文字列，正規表現，ヒアドキュメント，Python の文書文字列と f 文字列のデバッグ表示は，整形仕様 4.4 の内容保持に従う．補間の式を独立して整形出来る場合だけ内部のコード規則を適用し，補間の囲みへ余分な空白を入れない．

Ruby の `%w` / `%i` は要素を区切る空白を保持し，`,` / `;` 等で始まる要素も直前の要素へ繋がない (`%w(a ,b ;c)`)．引用符付シンボル，通常の文字列片及び補間の境界へ，要素の区切りを流用しない．

## 5.2 JSX と HTML

| 対象 | 空白の規則 |
|---|---|
| タグの開始 | `<` / `</` とタグ名を密着させる． |
| 属性の並び | 名前又は前の属性との間を必要な空白で区切る． |
| 属性の代入 | `=` の前後を密着させる．属性値内部は通常のコードとして変更しない． |
| タグの閉じ | `>` の前を密着させる．JSX 又は保持すべき自己終端の `/>` の前は空白１個を置く． |
| 本文・文字参照・子要素 | 表示される空白と文書構造を保持し，タグ内部の密着規則を適用しない． |
| JSX の式 | `{value}` の内側を密着させる． |

通常の HTML の空要素で不要な `/` を除く判断は構文正規化の責務とする．外来内容・XML の自己終端を同じ様に除かず，空白処理の副作用で閉じ方を変更しない．

JSX の空の式，HTML コメント，暗黙の閉じ及び空白保持要素は整形仕様 6.6 に従う．改行に接する空白を `{" "}` 等で表す必要が有る場合は，中間表現の選択肢へ含め，幅の決定後に補修しない．

## 5.3 CSS と SCSS

CSS / SCSS の記号には，選択子・識別子・単位・値の一部が有る．通常のプログラミング言語の二項演算子表を一括適用しない（規約 2.9 の CSS / SCSS の空白）．

| 役割 | 規則・代表例 |
|---|---|
| クラス・ID・色 | `.name`, `#name`, `#ABCDEF` の内部を密着させる． |
| 擬似クラス・擬似要素 | `:hover`, `::before` の内部を密着させる． |
| 子孫結合子 | `.a .b` の区切りを保持する．`.a.b` へ繋がない． |
| 子・隣接・後続の結合子 | `>` / `+` / `~` の前後へ空白を置く． |
| 識別子のハイフン | `margin-top`, `--main-color`, `hue-rotate` の内部を分断しない． |
| 数値・単位・百分率 | `10px`, `100%` 等の字句を分断しない．別の値の並びと区別する． |
| 特殊な値の記法 | `U+`, `An+B` の符号等を二項演算子として広げない． |
| `calc()` の二項演算 | `calc(100% - 10px)` 等の `+` / `-` の両側に必要な空白を置く． |
| 宣言・カンマ・本体の開始 | コロンの後，カンマの後，`{` の前に文脈上必要な空白を置く． |
| 優先指定 | `!important` の前を区切り，指定内部を分断しない． |
| アット規則・条件 | `@media (...) and (...)`，SCSS の `@if` / `@each` 等の語と式を区切る． |
| 引用符の無い `url()` | 引数全体を保持し，内部の `//` / `/*` をコメントと見誤らない． |
| SCSS のプレースホルダ・ネスト参照 | `%placeholder`, `&__x` 等，記号と名前の境界の意味を保持する． |
| SCSS の演算 | `$a + $b` 等，演算として確定した記号の前後へ空白を置く． |

`:where()` / `:is()` / `:not()` / `:has()` の内部にも子孫結合子が現れる．括弧の内側を密着させる規則は，要素同士の意味を持つ空白を削除する規則ではない．

CSS の `.a/**/.b` と SCSS の同じ字面，`10/* c */%`，`1px/* c */-2px` 等は，投影時に字句境界の意味を確定する．空白で同じ字句列を表せない場合は，前後の字句と `LexicalBoundary` から固定の `LexicalSeparator` を生成する（整形仕様 6.1, 6.6）．説明本文はコメント経路へ分離し，原文のコメントを `Verbatim` へ包まない．同じ境界で本文だけを変えた入力も独立性の比較対象にする．

数値に密着した単位と関数名の断片，符号付数，減算，空白自体が値であるカスタムプロパティを区別する．`--x: ;` の値の空白や，引用符の無い URL の内容を行末空白と同じ規則で除かない．

## 5.4 前処理指令とマクロ

C / C++ の前処理指令は，翻訳段階の連結を入力側で済ませた１論理行として表す．指令名の前の冗長な空白は詰め（`#  define` → `#define`），通常の字句間の区切りは１個へ揃える．指令の境界は必須改行とし，任意の幅分割を行わない．

マクロ名と `(` の密着は関数形式か否かを表す為，単なる呼出前空白の規則で変更しない．仮引数の名前・カンマ・`...` は構造として整え，文字列化・字句連結・見出名の内部を一般の空白規則へ含めない．

文字列・文字リテラル，`#include <a b.h>` の見出名及び逐語保護した実引数の内容を保持する．前処理指令は対象言語・版の標準構文に従い，`#pragma` 等で標準が内容の意味を定めない箇所は不透明な字句列として保持する．内容固有の解釈・置換・生成を行わない．指令本文の文字列を走査し直して空白を潰す処理は中核へ置かない．

Rust の未展開マクロの字句列も，通常の式として再解釈して空白・区切り・末尾カンマを変更しない．外側のマクロ名と `!` の配置と，内部の照合対象の保持を区別する．

---

# 第６章 投影の対応と受入条件

## 6.1 解析木から構文上の役割への対応

以下の節点名は `GrammarProfile` の投影対応を作る資料であり，文法の全節点を列挙する表ではない．使用する文法・版で名称・親子関係を検証し，解析器の識別子へ一度対応付ける．同じ役割を持つ複数の節点名は共通の事実へ投影し，字句の境界毎に型名の文字列比較又は祖先の再探索を行わない．

| 入力側の節点名の例 | 投影する役割・区別 |
|---|---|
| `binary_expression`, `additive_expression`, `multiplicative_expression`, `binary_operator`, `conjunction_expression`, `disjunction_expression`, `boolean_operator` | 演算子，左右の被演算子，言語別の結合． |
| `equality_expression`, `comparison_expression`, `comparison_operator`, `as_expression`, `is_expression`, `is_pattern_expression` | 比較・型演算．型引数等との誤読を解消する． |
| `assignment`, `assignment_expression`, `assignment_statement`, `augmented_assignment`, `augmented_assignment_expression`, `compound_assignment_expr`, `operator_assignment` | 代入・複合代入と，型注釈等の別の区切り． |
| `conditional`, `conditional_expression`, `conditional_type`, `ternary_expression`, `elvis_expression` | 三項・条件型・エルビス．同じコロンを値のコロンと混同しない． |
| `arrow_function`, `match_arm`, `switch_expression_arm`, `lambda_expression`, `lambda_literal`, `function_type` | アローの役割と前後の構文．内部の全記号をアロー扱いしない． |
| `send_statement`, `short_var_declaration`, `range_clause`, `named_expression` | Go の送信・束縛，Python の代入式等． |
| `unary_expression`, `pointer_expression`, `prefix_unary_expression`, `reference_expression`, `not_operator` | 前置の記号又は語． |
| `postfix_expression`, `postfix_unary_expression`, `update_expression`, `inc_statement`, `dec_statement`, `non_null_expression` | 前置・後置，式・文，型の記号との区別． |
| `range`, `range_expression`, `range_pattern`, `open_end_range_expression`, `open_start_range_expression` | 両側又は片側の範囲． |
| `pair`, `dict_pattern`, `field_initializer`, `keyed_element`, `dictionary_literal`, `dictionary_type`, `value_argument`, `attribute`, `pattern` | 鍵・値・名前付実引数・照合のコロン． |
| `property_signature`, `public_field_definition`, `type_annotation`, `parameter`, `class_parameter`, `variable_declaration`, `function_declaration` | 欄・仮引数・変数・戻値の型注釈． |
| `typed_parameter`, `typed_default_parameter`, `keyword_parameter`, `keyword_pattern` | 型注釈，既定値，キーワード仮引数・パターン．コロンの無い形も含む． |
| `field_declaration`, `let_declaration`, `const_item`, `static_item`, `trait_bounds` | Rust の型注釈・束縛・境界． |
| `inheritance_constraint`, `associatedtype_declaration`, `type_parameter`, `enum_type_parameters`, `lambda_parameter` | Swift の制約・関連型・仮引数等． |
| `base_class_clause`, `base_list`, `field_initializer_list`, `for_range_loop`, `enhanced_for_statement`, `class_declaration`, `protocol_declaration` | 継承・初期化子・範囲反復．型制約のコロンと区別する． |
| `class_definition`, `function_definition`, `if_statement`, `elif_clause`, `else_clause`, `for_statement`, `while_statement`, `match_statement`, `case_clause`, `with_statement`, `try_statement`, `except_clause`, `finally_clause` | Python の本体境界．同名の他言語の節点と区別する． |
| `lambda` | Python の式本体等．通常の複合文の本体とは区別する． |
| `case_statement`, `default_statement`, `default_case`, `expression_case`, `type_case`, `communication_case` | 節見出と本体． |
| `labeled_statement`, `switch_block_statement_group`, `switch_label`, `switch_case`, `switch_default`, `switch_section`, `switch_entry`, `access_specifier` | ラベル・節・アクセス区画． |
| `arguments`, `argument_list`, `parameters`, `array`, `object`, `hash`, `dictionary`, `interface_body`, `enum_body`, `initializer_list`, `literal_value` | 要素の並び，区切り及び本体との区別． |
| `block`, `compound_statement`, `statement_block`, `class_body`, `function_body` | 本体の種類・文数・包装の必要性． |
| `composite_literal`, `compound_literal_expression`, `interface_type`, `struct_type` | 型と値の境界，空の型本体． |
| `member_expression`, `field_expression`, `navigation_expression`, `navigation_suffix`, `scoped_identifier`, `qualified_identifier`, `directly_assignable_expression`, `optional_chain` | 後置参照・名前の修飾・任意の連鎖． |
| `member_access_expression`, `nullsafe_member_access_expression`, `nullsafe_member_call_expression`, `call_expression` | メンバ参照と呼出．同じ字面の他の演算と区別する． |
| `template_argument_list`, `template_parameter_list`, `type_argument_list`, `type_arguments`, `type_parameter_list`, `type_parameters` | 型引数・型仮引数の囲みと要素． |
| `start_tag`, `end_tag`, `self_closing_tag`, `jsx_opening_element`, `jsx_closing_element`, `jsx_self_closing_element`, `jsx_attribute`, `jsx_expression` | タグ・属性・式・本文の境界． |
| `string_literal`, `string`, `quoted_attribute_value`, `char_literal`, `character_literal`, `template_string`, `interpolation`, `template_substitution` | 内容と囲み，独立して整形出来る補間式． |
| `preproc_def`, `preproc_if`, `preproc_ifdef`, `preproc_function_def` | 指令・条件・マクロの構造． |
| `declaration`, `feature_query`, `class_selector`, `class_name`, `id_selector`, `color_value`, `pseudo_class_selector`, `pseudo_element_selector` | CSS / SCSS の宣言・値・選択子．通常コードの同名節点と区別する． |
| `placeholder`, `nesting_selector`, `child_selector`, `adjacent_sibling_selector`, `sibling_selector` | SCSS の記号，CSS / SCSS の結合子． |
| `lifetime`, `macro_invocation`, `macro_definition`, `dynamic_type`, `reference_type`, `self_parameter` | Rust の生存期間・マクロ・参照・型の語． |
| `attribute_declaration`, `spread_parameter`, `check_expression`, `annotation`, `file_annotation`, `use_site_target`, `jump_expression` | 属性，可変長引数，Kotlin の複合演算・注釈・ラベル． |
| `long_literal`, `real_literal`, `unsigned_literal` | 数値本体と合法な接尾辞． |
| `disjunctive_normal_form_type`, `intersection_type`, `type_list`, `union_type`, `reference_assignment_expression`, `attribute_group`, `namespace_name`, `qualified_name`, `variable_name` | PHP の型・参照・属性・不可分な名前． |
| `hash_splat_argument`, `hash_splat_parameter`, `block_argument`, `string_array`, `symbol_array` | Ruby の展開・ブロック実引数・内容の並び． |
| `type_alias_statement` | Python の型別名と誤読された通常の `type(x).attr` を入力側で区別する． |

対応表は言語・子の位置・投影した事実と併用し，空白の決定には第３章・第４章の役割別規則を用いる．

## 6.2 受入条件

| 項目 | 合格条件 |
|---|---|
| 字面の多義性 | 同じ `.` / `*` / `&` / `:` / `=` / `->` / `<-` を，役割毎に異なる境界へ正しく変換する． |
| 名前付／無名 | 同じ構文上の役割を持つ字句が，解析器の分類の違いだけで異なる空白にならない． |
| 元空白からの独立 | 意味を持たない空白・改行を変えても `SyntaxModel` と `FormatIR` の対応が変わらない． |
| 意味を持つ境界 | Swift の演算子，Ruby の呼出，文終端，マクロ名，CSS の子孫結合子等の差を消さない． |
| 単項の融合 | 被演算子への密着と合法な包装を両立し，別の演算子へ変換しない． |
| 本文の保護 | 文字列・補間・PHP の地の文・JSX / HTML の本文・URL・マクロを通常の隙間として変更しない． |
| 群との対応 | 平坦時の空白と折返時の改行を同じ境界へ対応付け，二重空白・行末空白を生成しない． |
| 共通化 | 異なる言語・文法の節点が同じ役割・境界条件へ投影された場合は，同じ規則を選ぶ．無関係な言語の対応追加で判定回数と結果が変わらない． |
| 情報境界 | `Canonicalizer` は言語識別・解析器の節点名・行幅と指示コメントを含むコメントの状態を読まず，`LineBreaker` は空白規則を原文から再判定しない．コメントに依る空行の要求を生成しない． |
| 共通の書式 | 仮引数の既定値の `=` は代入・初期化と同じく前後に空白１個を置き，呼出側のキーワード引数・名前付実引数は密着させる．キーワードパターン・属性値・指示文の設定値の `=` と型の合成の `\|` / `&` は密着を保ち，値の演算と区別する．同じ役割では言語名や型注釈の有無で空白を変えない． |
| 固定の境界 | 同じ字句列と境界から同じ分離記法を生成し，説明本文を中核又は幅の計数へ含めない． |
| 検証 | 整形仕様第８章に従い，構文・字句・保護区画を独立に検査する． |
| 冪等性 | 同じ設定の再整形で空白・構文・コメントの所属が変わらない． |

## 6.3 検体の保守

文法を更新する際は入力側の対応資料と検体を更新する．中核の規則を変える場合は，言語上の意味，規約及び整形仕様との対応を確認し，単に旧実装の結果へ合わせない．
