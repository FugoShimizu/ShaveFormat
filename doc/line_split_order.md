# LineSplit 分割位置確定順仕様

本書は LineSplitPass の分割位置決定アルゴリズムを３つの具体例で示す．番号付け規則 (post-order)・文字数判定・グループ化・最終出力の対応関係を順に追い，[src/Pass/LineSplit.cpp](../src/Pass/LineSplit.cpp) の `CollectBreaks` / `GreedyMerge` の挙動を実例と突合可能とする．

## 1. 概要

LineSplitPass は１行が `MaxChars` を超える場合に AST 構造に沿った分割位置を選定し，改行を挿入する．分割候補は unnamed トークン（`,` `;` `(` `[` `{` `=>` 等）の直前 / 直後に限られ，AST の post-order 走査で深い部分木から順に確定する．深い部分木が単独で行幅に収まれば分割せず，収まらない部分木のみ親部分木の分割候補を採用する事で，必要最小限の改行で行幅制約を満たす．

**本書の対象範囲**: 上記の post-order 走査に依る通常分割機構のみを扱う．LineSplit は他に以下の分割処理も含むが本書では扱わない．詳細は [processing_spec.md](processing_spec.md) § 3.7 を参照．

- `SplitWideVarDecls`: 統合変数宣言 (`int A = 1, B = 2, ...`) が `MaxChars` 超過時に別宣言（型接頭部を繰り返す `int A = 1; int B = 2; ...` 形）へグリーディ分割する処理．通常分割機構より前に確定させ，カンマ継続分割を抑止する．
- `BreakWideRubyBranches`: Ruby の１行の分岐を分割する時に，単一文本体を保って後続の節境界で改行し，割り位置を求め直す処理．
- `ApplyBraceWraps`: 行幅超過の単一文本体を持つ制御構文に `{ ... }` を付与する処理．

具体例は次の３つの１行 TypeScript 文を扱う．

- `const polygon = isoLines(new QuadTree(mask), [1], { linearRing: true, noFrame: true })[0].reduce((max, arr) => arr.length > max.length ? arr : max, []);`
- `const isCollinear = (a: number, b: number, c: number): boolean => Math.abs((polygon[b][0] - polygon[a][0]) * (polygon[c][1] - polygon[a][1]) - (polygon[b][1] - polygon[a][1]) * (polygon[c][0] - polygon[a][0])) < tolerance;`
- `for(let i = 0; i < imageData.data.length; i += 4) if(imageData.data[i + 3] < alpha) imageData.data.set([0xff, 0xff, 0xff, 0x1], i);`

各例に就いて AST ダンプ，分割候補への番号付与，候補毎の文字数判定，最終出力の４段を示す．

## 2. 番号付け規則 (post-order traversal)

AST ダンプ中の丸囲み数字（①②③…）は分割候補の優先度を表し，以下の規則で付与する．

- **unnamed 子 [U] のみ番号を持つ**．named 子 [N] には決して番号は付かない．
- **先頭キーワードは番号無**．`const` / `new` 等，部分木の先頭で他 unnamed 子と分割可能性を共有しない先頭キーワードは分割候補から除外する．
- **同じ親 named 子直下の unnamed 子は同番号**．例えば `(new QuadTree(mask), [1], {...})` の `(` `,` `,` `,` `)` は親 named 子が共通の為，全て同じ番号⑥を持つ．
- **番号順は post-order 走査**．AST を深さ優先で左から右に辿り，子部分木を完全に処理してから親に番号を付ける．即ち「左の部分木の全 unnamed 子が番号付与された後，次の部分木，最後に当該親 named 子直下の unnamed 子群」の順で番号が大きくなる．
- 結果として，**番号が小さい程「深く＆左寄り」，大きい程「浅く＆右寄り」**の優先度を持つ．

例１の文 `const polygon = isoLines(new QuadTree(mask), [1], { ... })[0].reduce(...);` の番号付与順を追うと：

| 順 | 番号 | 対象 unnamed 子 | 完了する部分木 |
| --: | --- | --- | --- |
| 1 | ① | `(` `)` | `[N] (mask)` |
| 2 | ② | `[` `]` | `[N] [1]` |
| 3 | ③ | `:` | `[N] linearRing: true` |
| 4 | ④ | `:` | `[N] noFrame: true` |
| 5 | ⑤ | `{` `,` `}` | `[N] { ... }` |
| 6 | ⑥ | `(` `,` `,` `,` `)` | `[N] (new QuadTree(mask), [1], {...})` |
| 7 | ⑦ | `[` `]` | `[N] [0]` |
| 8 | ⑧ | `.` | 以降同様に右へ進む |
| 9 | ⑯ | `=` | `[N] polygon = ...` |
| 10 | ⑰ | `;` | ルート `[N] const polygon ... ;` |

## 3. 分割判定アルゴリズム

### 3.1 分割可能位置

- 分割は unnamed 子の前後何れか**片側のみ**で行える．named 子同士の間では分割出来ない．
- 各 unnamed 子は `SplitSide` で「字句の前で分割し，字句を次行の先頭へ置く (Head)」「字句の後で分割し，字句を前行の末尾へ置く (Tail)」「分割不可 (None)」の何れかに分類される．None の unnamed 子では分割出来ない．丸囲み数字は分割位置に置く（Head は字句の前，Tail は字句の後）．
	- Head：閉じ括弧とメンバアクセス（`)` `]` `}` `.` `?.`．連鎖の起点を読み取れる様に次行の先頭へ置く．Swift / Kotlin の `.` は名前付の `navigation_suffix` の最初の字句に為るが，同じく其の前で割る），Ruby の節区切り（`elsif` `when` `else` `rescue` `ensure` `end`）と Go の `case` / `default`（直前の文と同じ行へ潰れると節の対応や自動セミコロン挿入が崩れる）．
	- Tail：区切記号と開き括弧（`,` `;` `(` `[` `{` `=>`），及び演算子（代入・算術・比較・論理・ビット・シフト・三項・複合代入，Python の語演算子）．演算子は最初の名前付子より後の物だけを候補とし，Go の自動セミコロン挿入・Ruby の行末改行の文区切り・Kotlin の行末字句に依る式継続の判定で行頭の演算子が構文を壊す為，全言語で行末へ揃える．Go の `.` も同じ理由で行末へ置く．
- 制御構文の複合文が単一文でブレースが無い場合は，ブレースを付与した状態で判定する．其の儘では制御構文と複合文は named 子同士の関係で分割不能だが，ブレース付与によって内側での改行が可能となる．
- 但し見出し（`if(Cond) {`）自体が行幅に収まらず条件を割る場合は，本体を行頭の閉じ括弧へ続け（`) Body;`），其の行も行幅を超える時に限りブレースを付与する．
- 単一文の複合文にブレースを付与した場合は，次の行に有る if-else の `else` 又は do-while の `while` を閉じブレースと同一行に移動し，`} else ` / `} while(...);` の形にする．
- 分割判定は**番号の小さい順（= 深い部分木から順）** に行い，行幅が `MaxChars` 以内なら分割しない．`MaxChars` 超の場合は分割位置として残す．
- 次の位置は分割候補に載せない：Java / Swift の `import` 宣言の完全修飾名，PHP のキャスト式，C# の `#pragma` 行，`name?: T` の `?` と型注釈の間，Ruby の `rescue` 修飾子と隣接文字列連結，Rust のマクロ本体，Ruby のヒアドキュメント開始トークンを含む行の全体（本文の起点が行に束縛されて居る為），型注釈の `:` と初期化子の `=` を直接の子に持つ Rust の宣言の `:`（`=` と組にすると `let x:` の後で割れる），仮引数列を囲む `|`（Ruby `{ |x, y| }` / Rust `|x, y|`．閉じの `|` の後を候補にすると親の閉じ波括弧の前の候補と交差し，行に収まっても分割が残る），C++ のラムダの捕捉並びの `&` `=` `*`（参照捕捉・既定捕捉・`*this` の接頭辞で，後ろで割ると捕捉名から離れる），PHP の地の文の境界（閉じタグの前等．同じ組の他の候補が地の文の改行を跨いで内側展開済と看做され，行に収まる見出しまで割れる）．

### 3.2 文字数判定方法（行長制限が６４文字の場合）

- 文字数は半角・全角等の文字種に関係無く単純な文字数を用いる．但し制御文字は文字数に含めない．
- 行の文字数は，判定する分割位置の両側に有る分割位置間の区間の文字数を，両側のスペースを除いて測定する．
- 同じ番号の分割位置が複数有り，其れらの間に別の分割位置が有って区間が１つに定まらない場合は，文字数に依らず分割する．
- 付与したブレースを分割候補とする場合は，ブレースと其の両側のスペースを外した状態（元の単一文の状態）の文字数で判定する．分割しない場合はブレースを外して元の単一文に戻す．

### 3.3 区間 (span) 境界の詳細

「判定する分割位置の両側に有る分割位置」とは，現在評価中の候補から見て左右其々で最も近い**分割候補位置**を指し，以下の両方を含む．

- **未評価の候補位置**（番号が現在より大きく，此れから判定される候補）
- **分割確定の候補位置**（過去に「分割する」と決定された候補）

但し**「分割しない」と決定済の候補位置は境界から除外する**（既に行に溶け込んでおり行を区切らない為）．

区間は左右の境界の分割位置の間の文字列で，分割位置は字句の前 (Head) 又は後 (Tail) に在る為，境界の字句が区間に入るかは其の側で決まる．例１の⑥評価時の区間は，左隣の⑯（`=` の後，未評価）から右隣の⑦（`[` の後，未評価）までの `isoLines(new QuadTree(mask), [1], { linearRing: true, noFrame: true })[` → ７１文字．

例１の⑦評価時（⑥は分割確定済）の区間は，左隣⑥の最右位置（`)` の前，確定）から右隣⑧（`.` の前）までの `)[0]` の４文字．⑥確定により `)` が新行の先頭となり，⑦の評価対象は「`)` で始まる新行のうち，次の候補⑧の手前まで」となる．

## 4. 例１: `const polygon = isoLines(...)`

### 4.1 AST ダンプ

丸囲み数字は分割可能な unnamed 子．

```
└─ [N] const polygon = isoLines(...)[0].reduce(...);
	├─ [U] const
	├─ [N] polygon = isoLines(...)[0].reduce(...)
	│	├─ [N] polygon
	│	├─ [U] = ⑯
	│	└─ [N] isoLines(...)[0].reduce(...)
	│		├─ [N] isoLines(...)[0].reduce
	│		│	├─ [N] isoLines(...)[0]
	│		│	│	├─ [N] isoLines(...)
	│		│	│	│	├─ [N] isoLines
	│		│	│	│	└─ [N] (new QuadTree(mask), [1], {...})
	│		│	│	│		├─ [U] ( ⑥
	│		│	│	│		├─ [N] new QuadTree(mask)
	│		│	│	│		│	├─ [U] new
	│		│	│	│		│	├─ [N] QuadTree
	│		│	│	│		│	└─ [N] (mask)
	│		│	│	│		│		├─ [U] ( ①
	│		│	│	│		│		├─ [N] mask
	│		│	│	│		│		└─ [U] ) ①
	│		│	│	│		├─ [U] , ⑥
	│		│	│	│		├─ [N] [1]
	│		│	│	│		│	├─ [U] [ ②
	│		│	│	│		│	├─ [N] 1
	│		│	│	│		│	└─ [U] ] ②
	│		│	│	│		├─ [U] , ⑥
	│		│	│	│		├─ [N] { linearRing: true, noFrame: true }
	│		│	│	│		│	├─ [U] { ⑤
	│		│	│	│		│	├─ [N] linearRing: true
	│		│	│	│		│	│	├─ [N] linearRing
	│		│	│	│		│	│	├─ [U] : ③
	│		│	│	│		│	│	└─ [N] true
	│		│	│	│		│	├─ [U] , ⑤
	│		│	│	│		│	├─ [N] noFrame: true
	│		│	│	│		│	│	├─ [N] noFrame
	│		│	│	│		│	│	├─ [U] : ④
	│		│	│	│		│	│	└─ [N] true
	│		│	│	│		│	└─ [U] } ⑤
	│		│	│	│		└─ [U] ) ⑥
	│		│	│	├─ [U] [ ⑦
	│		│	│	├─ [N] 0
	│		│	│	└─ [U] ] ⑦
	│		│	├─ [U] . ⑧
	│		│	└─ [N] reduce
	│		└─ [N] ((max, arr) => arr.length > max.length ? arr : max, [])
	│			├─ [U] ( ⑮
	│			├─ [N] (max, arr) => arr.length > max.length ? arr : max
	│			│	├─ [N] (max, arr)
	│			│	│	├─ [U] ( ⑨
	│			│	│	├─ [N] max
	│			│	│	├─ [U] , ⑨
	│			│	│	├─ [N] arr
	│			│	│	└─ [U] ) ⑨
	│			│	├─ [U] => ⑭
	│			│	└─ [N] arr.length > max.length ? arr : max
	│			│		├─ [N] arr.length > max.length
	│			│		│	├─ [N] arr.length
	│			│		│	│	├─ [N] arr
	│			│		│	│	├─ [U] . ⑩
	│			│		│	│	└─ [N] length
	│			│		│	├─ [U] > ⑫
	│			│		│	└─ [N] max.length
	│			│		│		├─ [N] max
	│			│		│		├─ [U] . ⑪
	│			│		│		└─ [N] length
	│			│		├─ [U] ? ⑬
	│			│		├─ [N] arr
	│			│		├─ [U] : ⑬
	│			│		└─ [N] max
	│			├─ [U] , ⑮
	│			├─ [N] []
	│			└─ [U] ) ⑮
	└─ [U] ; ⑰
```

### 4.2 番号付与済の評価対象

コード内の丸囲み数字は分割候補位置．

`const polygon =⑯ isoLines(⑥new QuadTree(①mask①),⑥ [②1②],⑥ {⑤ linearRing: ③true, ⑤noFrame: ④true ⑤}⑥)[⑦0⑦]⑧.reduce(⑮(⑨max,⑨ arr⑨) =>⑭ arr⑩.length >⑫ max⑪.length ?⑬ arr :⑬ max,⑮ []⑮);⑰`

① `new QuadTree(mask),` → ６４文字以内（１９文字）の為，分割しない．

`const polygon =⑯ isoLines(⑥new QuadTree(mask),⑥ [②1②],⑥ {⑤ linearRing: ③true, ⑤noFrame: ④true ⑤}⑥)[⑦0⑦]⑧.reduce(⑮(⑨max,⑨ arr⑨) =>⑭ arr⑩.length >⑫ max⑪.length ?⑬ arr :⑬ max,⑮ []⑮);⑰`

② `[1],` → ６４文字以内（４文字）の為，分割しない．

`const polygon =⑯ isoLines(⑥new QuadTree(mask),⑥ [1],⑥ {⑤ linearRing: ③true, ⑤noFrame: ④true ⑤}⑥)[⑦0⑦]⑧.reduce(⑮(⑨max,⑨ arr⑨) =>⑭ arr⑩.length >⑫ max⑪.length ?⑬ arr :⑬ max,⑮ []⑮);⑰`

③ `linearRing: true,` → ６４文字以内（１７文字）の為，分割しない．

`const polygon =⑯ isoLines(⑥new QuadTree(mask),⑥ [1],⑥ {⑤ linearRing: true, ⑤noFrame: ④true ⑤}⑥)[⑦0⑦]⑧.reduce(⑮(⑨max,⑨ arr⑨) =>⑭ arr⑩.length >⑫ max⑪.length ?⑬ arr :⑬ max,⑮ []⑮);⑰`

④ `noFrame: true` → ６４文字以内（１３文字）の為，分割しない．

`const polygon =⑯ isoLines(⑥new QuadTree(mask),⑥ [1],⑥ {⑤ linearRing: true, ⑤noFrame: true ⑤}⑥)[⑦0⑦]⑧.reduce(⑮(⑨max,⑨ arr⑨) =>⑭ arr⑩.length >⑫ max⑪.length ?⑬ arr :⑬ max,⑮ []⑮);⑰`

⑤ `{ linearRing: true, noFrame: true }` → ６４文字以内（３５文字）の為，分割しない．

`const polygon =⑯ isoLines(⑥new QuadTree(mask),⑥ [1],⑥ { linearRing: true, noFrame: true }⑥)[⑦0⑦]⑧.reduce(⑮(⑨max,⑨ arr⑨) =>⑭ arr⑩.length >⑫ max⑪.length ?⑬ arr :⑬ max,⑮ []⑮);⑰`

⑥ `isoLines(new QuadTree(mask), [1], { linearRing: true, noFrame: true })[` → ６４文字超（７１文字）の為，**分割する**．

`const polygon =⑯ isoLines(⑥new QuadTree(mask),⑥ [1],⑥ { linearRing: true, noFrame: true }⑥)[⑦0⑦]⑧.reduce(⑮(⑨max,⑨ arr⑨) =>⑭ arr⑩.length >⑫ max⑪.length ?⑬ arr :⑬ max,⑮ []⑮);⑰`

⑦ `)[0]` → ６４文字以内（４文字）の為，分割しない．

`const polygon =⑯ isoLines(⑥new QuadTree(mask),⑥ [1],⑥ { linearRing: true, noFrame: true }⑥)[0]⑧.reduce(⑮(⑨max,⑨ arr⑨) =>⑭ arr⑩.length >⑫ max⑪.length ?⑬ arr :⑬ max,⑮ []⑮);⑰`

⑧ `)[0].reduce(` → ６４文字以内（１２文字）の為，分割しない．

`const polygon =⑯ isoLines(⑥new QuadTree(mask),⑥ [1],⑥ { linearRing: true, noFrame: true }⑥)[0].reduce(⑮(⑨max,⑨ arr⑨) =>⑭ arr⑩.length >⑫ max⑪.length ?⑬ arr :⑬ max,⑮ []⑮);⑰`

⑨ `(max, arr) =>` → ６４文字以内（１３文字）の為，分割しない．

`const polygon =⑯ isoLines(⑥new QuadTree(mask),⑥ [1],⑥ { linearRing: true, noFrame: true }⑥)[0].reduce(⑮(max, arr) =>⑭ arr⑩.length >⑫ max⑪.length ?⑬ arr :⑬ max,⑮ []⑮);⑰`

⑩ `arr.length >` → ６４文字以内（１２文字）の為，分割しない．

`const polygon =⑯ isoLines(⑥new QuadTree(mask),⑥ [1],⑥ { linearRing: true, noFrame: true }⑥)[0].reduce(⑮(max, arr) =>⑭ arr.length >⑫ max⑪.length ?⑬ arr :⑬ max,⑮ []⑮);⑰`

⑪ `max.length ?` → ６４文字以内（１２文字）の為，分割しない．

`const polygon =⑯ isoLines(⑥new QuadTree(mask),⑥ [1],⑥ { linearRing: true, noFrame: true }⑥)[0].reduce(⑮(max, arr) =>⑭ arr.length >⑫ max.length ?⑬ arr :⑬ max,⑮ []⑮);⑰`

⑫ `arr.length > max.length ?` → ６４文字以内（２５文字）の為，分割しない．

`const polygon =⑯ isoLines(⑥new QuadTree(mask),⑥ [1],⑥ { linearRing: true, noFrame: true }⑥)[0].reduce(⑮(max, arr) =>⑭ arr.length > max.length ?⑬ arr :⑬ max,⑮ []⑮);⑰`

⑬ `arr.length > max.length ? arr : max,` → ６４文字以内（３６文字）の為，分割しない．

`const polygon =⑯ isoLines(⑥new QuadTree(mask),⑥ [1],⑥ { linearRing: true, noFrame: true }⑥)[0].reduce(⑮(max, arr) =>⑭ arr.length > max.length ? arr : max,⑮ []⑮);⑰`

⑭ `(max, arr) => arr.length > max.length ? arr : max,` → ６４文字以内（５０文字）の為，分割しない．

`const polygon =⑯ isoLines(⑥new QuadTree(mask),⑥ [1],⑥ { linearRing: true, noFrame: true }⑥)[0].reduce(⑮(max, arr) => arr.length > max.length ? arr : max,⑮ []⑮);⑰`

⑮ `)[0].reduce((max, arr) => arr.length > max.length ? arr : max, []);` → ６４文字超（６７文字）の為，**分割する**．

`const polygon =⑯ isoLines(⑥new QuadTree(mask),⑥ [1],⑥ { linearRing: true, noFrame: true }⑥)[0].reduce(⑮(max, arr) => arr.length > max.length ? arr : max,⑮ []⑮);⑰`

⑯ `const polygon = isoLines(` → ６４文字以内（２５文字）の為，分割しない．

`const polygon = isoLines(⑥new QuadTree(mask),⑥ [1],⑥ { linearRing: true, noFrame: true }⑥)[0].reduce(⑮(max, arr) => arr.length > max.length ? arr : max,⑮ []⑮);⑰`

⑰ `);` → ６４文字以内（２文字）の為，分割しない．

`const polygon = isoLines(⑥new QuadTree(mask),⑥ [1],⑥ { linearRing: true, noFrame: true }⑥)[0].reduce(⑮(max, arr) => arr.length > max.length ? arr : max,⑮ []⑮);` 残った分割位置（⑥，⑮）に，スペースが有れば削除した上で改行を入れ，括弧の内側のインデントを深くして完了．

### 4.3 最終出力

```
const polygon = isoLines(
	new QuadTree(mask),
	[1],
	{ linearRing: true, noFrame: true }
)[0].reduce(
	(max, arr) => arr.length > max.length ? arr : max,
	[]
);
```

## 5. 例２: `const isCollinear = ...`

### 5.1 AST ダンプ

```
└─ [N] const isCollinear = (...): boolean => Math.abs(...) < tolerance;
	├─ [U] const
	├─ [N] isCollinear = (...): boolean => Math.abs(...) < tolerance
	│	├─ [N] isCollinear
	│	├─ [U] = ㊲
	│	└─ [N] (a: number, b: number, c: number): boolean => Math.abs(...) < tolerance
	│		├─ [N] (a: number, b: number, c: number)
	│		│	├─ [U] ( ④
	│		│	├─ [N] a: number
	│		│	│	├─ [N] a
	│		│	│	└─ [N] : number
	│		│	│		├─ [U] : ①
	│		│	│		└─ [N] number
	│		│	│			└─ [U] number
	│		│	├─ [U] , ④
	│		│	├─ [N] b: number
	│		│	│	├─ [N] b
	│		│	│	└─ [N] : number
	│		│	│		├─ [U] : ②
	│		│	│		└─ [N] number
	│		│	│			└─ [U] number
	│		│	├─ [U] , ④
	│		│	├─ [N] c: number
	│		│	│	├─ [N] c
	│		│	│	└─ [N] : number
	│		│	│		├─ [U] : ③
	│		│	│		└─ [N] number
	│		│	│			└─ [U] number
	│		│	└─ [U] ) ④
	│		├─ [N] : boolean
	│		│	├─ [U] : ⑤
	│		│	└─ [N] boolean
	│		│		└─ [U] boolean
	│		├─ [U] => ㊱
	│		└─ [N] Math.abs((...) * (...) - (...) * (...)) < tolerance
	│			├─ [N] Math.abs((...) * (...) - (...) * (...))
	│			│	├─ [N] Math.abs
	│			│	│	├─ [N] Math
	│			│	│	├─ [U] . ⑥
	│			│	│	└─ [N] abs
	│			│	└─ [N] ((...) * (...) - (...) * (...))
	│			│		├─ [U] ( ㉞
	│			│		├─ [N] (...) * (...) - (...) * (...)
	│			│		│	├─ [N] (polygon[b][0] - polygon[a][0]) * (polygon[c][1] - polygon[a][1])
	│			│		│	│	├─ [N] (polygon[b][0] - polygon[a][0])
	│			│		│	│	│	├─ [U] ( ⑫
	│			│		│	│	│	├─ [N] polygon[b][0] - polygon[a][0]
	│			│		│	│	│	│	├─ [N] polygon[b][0]
	│			│		│	│	│	│	│	├─ [N] polygon[b]
	│			│		│	│	│	│	│	│	├─ [N] polygon
	│			│		│	│	│	│	│	│	├─ [U] [ ⑦
	│			│		│	│	│	│	│	│	├─ [N] b
	│			│		│	│	│	│	│	│	└─ [U] ] ⑦
	│			│		│	│	│	│	│	├─ [U] [ ⑧
	│			│		│	│	│	│	│	├─ [N] 0
	│			│		│	│	│	│	│	└─ [U] ] ⑧
	│			│		│	│	│	│	├─ [U] - ⑪
	│			│		│	│	│	│	└─ [N] polygon[a][0]
	│			│		│	│	│	│		├─ [N] polygon[a]
	│			│		│	│	│	│		│	├─ [N] polygon
	│			│		│	│	│	│		│	├─ [U] [ ⑨
	│			│		│	│	│	│		│	├─ [N] a
	│			│		│	│	│	│		│	└─ [U] ] ⑨
	│			│		│	│	│	│		├─ [U] [ ⑩
	│			│		│	│	│	│		├─ [N] 0
	│			│		│	│	│	│		└─ [U] ] ⑩
	│			│		│	│	│	└─ [U] ) ⑫
	│			│		│	│	├─ [U] * ⑲
	│			│		│	│	└─ [N] (polygon[c][1] - polygon[a][1])
	│			│		│	│		├─ [U] ( ⑱
	│			│		│	│		├─ [N] polygon[c][1] - polygon[a][1]
	│			│		│	│		│	├─ [N] polygon[c][1]
	│			│		│	│		│	│	├─ [N] polygon[c]
	│			│		│	│		│	│	│	├─ [N] polygon
	│			│		│	│		│	│	│	├─ [U] [ ⑬
	│			│		│	│		│	│	│	├─ [N] c
	│			│		│	│		│	│	│	└─ [U] ] ⑬
	│			│		│	│		│	│	├─ [U] [ ⑭
	│			│		│	│		│	│	├─ [N] 1
	│			│		│	│		│	│	└─ [U] ] ⑭
	│			│		│	│		│	├─ [U] - ⑰
	│			│		│	│		│	└─ [N] polygon[a][1]
	│			│		│	│		│		├─ [N] polygon[a]
	│			│		│	│		│		│	├─ [N] polygon
	│			│		│	│		│		│	├─ [U] [ ⑮
	│			│		│	│		│		│	├─ [N] a
	│			│		│	│		│		│	└─ [U] ] ⑮
	│			│		│	│		│		├─ [U] [ ⑯
	│			│		│	│		│		├─ [N] 1
	│			│		│	│		│		└─ [U] ] ⑯
	│			│		│	│		└─ [U] ) ⑱
	│			│		│	├─ [U] - ㉝
	│			│		│	└─ [N] (polygon[b][1] - polygon[a][1]) * (polygon[c][0] - polygon[a][0])
	│			│		│		├─ [N] (polygon[b][1] - polygon[a][1])
	│			│		│		│	├─ [U] ( ㉕
	│			│		│		│	├─ [N] polygon[b][1] - polygon[a][1]
	│			│		│		│	│	├─ [N] polygon[b][1]
	│			│		│		│	│	│	├─ [N] polygon[b]
	│			│		│		│	│	│	│	├─ [N] polygon
	│			│		│		│	│	│	│	├─ [U] [ ⑳
	│			│		│		│	│	│	│	├─ [N] b
	│			│		│		│	│	│	│	└─ [U] ] ⑳
	│			│		│		│	│	│	├─ [U] [ ㉑
	│			│		│		│	│	│	├─ [N] 1
	│			│		│		│	│	│	└─ [U] ] ㉑
	│			│		│		│	│	├─ [U] - ㉔
	│			│		│		│	│	└─ [N] polygon[a][1]
	│			│		│		│	│		├─ [N] polygon[a]
	│			│		│		│	│		│	├─ [N] polygon
	│			│		│		│	│		│	├─ [U] [ ㉒
	│			│		│		│	│		│	├─ [N] a
	│			│		│		│	│		│	└─ [U] ] ㉒
	│			│		│		│	│		├─ [U] [ ㉓
	│			│		│		│	│		├─ [N] 1
	│			│		│		│	│		└─ [U] ] ㉓
	│			│		│		│	└─ [U] ) ㉕
	│			│		│		├─ [U] * ㉜
	│			│		│		└─ [N] (polygon[c][0] - polygon[a][0])
	│			│		│			├─ [U] ( ㉛
	│			│		│			├─ [N] polygon[c][0] - polygon[a][0]
	│			│		│			│	├─ [N] polygon[c][0]
	│			│		│			│	│	├─ [N] polygon[c]
	│			│		│			│	│	│	├─ [N] polygon
	│			│		│			│	│	│	├─ [U] [ ㉖
	│			│		│			│	│	│	├─ [N] c
	│			│		│			│	│	│	└─ [U] ] ㉖
	│			│		│			│	│	├─ [U] [ ㉗
	│			│		│			│	│	├─ [N] 0
	│			│		│			│	│	└─ [U] ] ㉗
	│			│		│			│	├─ [U] - ㉚
	│			│		│			│	└─ [N] polygon[a][0]
	│			│		│			│		├─ [N] polygon[a]
	│			│		│			│		│	├─ [N] polygon
	│			│		│			│		│	├─ [U] [ ㉘
	│			│		│			│		│	├─ [N] a
	│			│		│			│		│	└─ [U] ] ㉘
	│			│		│			│		├─ [U] [ ㉙
	│			│		│			│		├─ [N] 0
	│			│		│			│		└─ [U] ] ㉙
	│			│		│			└─ [U] ) ㉛
	│			│		└─ [U] ) ㉞
	│			├─ [U] < ㉟
	│			└─ [N] tolerance
	└─ [U] ; ㊳
```

### 5.2 番号付与済の評価対象

`const isCollinear =㊲ (④a:① number,④ b:② number,④ c:③ number④):⑤ boolean =>㊱ Math⑥.abs(㉞(⑫polygon[⑦b⑦][⑧0⑧] -⑪ polygon[⑨a⑨][⑩0⑩]⑫) *⑲ (⑱polygon[⑬c⑬][⑭1⑭] -⑰ polygon[⑮a⑮][⑯1⑯]⑱) -㉝ (㉕polygon[⑳b⑳][㉑1㉑] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

① `a: number,` → ６４文字以内（１０文字）の為，分割しない．

`const isCollinear =㊲ (④a: number,④ b:② number,④ c:③ number④):⑤ boolean =>㊱ Math⑥.abs(㉞(⑫polygon[⑦b⑦][⑧0⑧] -⑪ polygon[⑨a⑨][⑩0⑩]⑫) *⑲ (⑱polygon[⑬c⑬][⑭1⑭] -⑰ polygon[⑮a⑮][⑯1⑯]⑱) -㉝ (㉕polygon[⑳b⑳][㉑1㉑] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

② `b: number,` → ６４文字以内（１０文字）の為，分割しない．

`const isCollinear =㊲ (④a: number,④ b: number,④ c:③ number④):⑤ boolean =>㊱ Math⑥.abs(㉞(⑫polygon[⑦b⑦][⑧0⑧] -⑪ polygon[⑨a⑨][⑩0⑩]⑫) *⑲ (⑱polygon[⑬c⑬][⑭1⑭] -⑰ polygon[⑮a⑮][⑯1⑯]⑱) -㉝ (㉕polygon[⑳b⑳][㉑1㉑] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

③ `c: number` → ６４文字以内（９文字）の為，分割しない．

`const isCollinear =㊲ (④a: number,④ b: number,④ c: number④):⑤ boolean =>㊱ Math⑥.abs(㉞(⑫polygon[⑦b⑦][⑧0⑧] -⑪ polygon[⑨a⑨][⑩0⑩]⑫) *⑲ (⑱polygon[⑬c⑬][⑭1⑭] -⑰ polygon[⑮a⑮][⑯1⑯]⑱) -㉝ (㉕polygon[⑳b⑳][㉑1㉑] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

④ `(a: number, b: number, c: number):` → ６４文字以内（３４文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number):⑤ boolean =>㊱ Math⑥.abs(㉞(⑫polygon[⑦b⑦][⑧0⑧] -⑪ polygon[⑨a⑨][⑩0⑩]⑫) *⑲ (⑱polygon[⑬c⑬][⑭1⑭] -⑰ polygon[⑮a⑮][⑯1⑯]⑱) -㉝ (㉕polygon[⑳b⑳][㉑1㉑] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

⑤ `(a: number, b: number, c: number): boolean =>` → ６４文字以内（４５文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math⑥.abs(㉞(⑫polygon[⑦b⑦][⑧0⑧] -⑪ polygon[⑨a⑨][⑩0⑩]⑫) *⑲ (⑱polygon[⑬c⑬][⑭1⑭] -⑰ polygon[⑮a⑮][⑯1⑯]⑱) -㉝ (㉕polygon[⑳b⑳][㉑1㉑] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

⑥ `Math.abs(` → ６４文字以内（９文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(⑫polygon[⑦b⑦][⑧0⑧] -⑪ polygon[⑨a⑨][⑩0⑩]⑫) *⑲ (⑱polygon[⑬c⑬][⑭1⑭] -⑰ polygon[⑮a⑮][⑯1⑯]⑱) -㉝ (㉕polygon[⑳b⑳][㉑1㉑] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

⑦ `polygon[b][` → ６４文字以内（１１文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(⑫polygon[b][⑧0⑧] -⑪ polygon[⑨a⑨][⑩0⑩]⑫) *⑲ (⑱polygon[⑬c⑬][⑭1⑭] -⑰ polygon[⑮a⑮][⑯1⑯]⑱) -㉝ (㉕polygon[⑳b⑳][㉑1㉑] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

⑧ `polygon[b][0] -` → ６４文字以内（１５文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(⑫polygon[b][0] -⑪ polygon[⑨a⑨][⑩0⑩]⑫) *⑲ (⑱polygon[⑬c⑬][⑭1⑭] -⑰ polygon[⑮a⑮][⑯1⑯]⑱) -㉝ (㉕polygon[⑳b⑳][㉑1㉑] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

⑨ `polygon[a][` → ６４文字以内（１１文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(⑫polygon[b][0] -⑪ polygon[a][⑩0⑩]⑫) *⑲ (⑱polygon[⑬c⑬][⑭1⑭] -⑰ polygon[⑮a⑮][⑯1⑯]⑱) -㉝ (㉕polygon[⑳b⑳][㉑1㉑] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

⑩ `polygon[a][0]` → ６４文字以内（１３文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(⑫polygon[b][0] -⑪ polygon[a][0]⑫) *⑲ (⑱polygon[⑬c⑬][⑭1⑭] -⑰ polygon[⑮a⑮][⑯1⑯]⑱) -㉝ (㉕polygon[⑳b⑳][㉑1㉑] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

⑪ `polygon[b][0] - polygon[a][0]` → ６４文字以内（２９文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(⑫polygon[b][0] - polygon[a][0]⑫) *⑲ (⑱polygon[⑬c⑬][⑭1⑭] -⑰ polygon[⑮a⑮][⑯1⑯]⑱) -㉝ (㉕polygon[⑳b⑳][㉑1㉑] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

⑫ `(polygon[b][0] - polygon[a][0]) *` → ６４文字以内（３３文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (⑱polygon[⑬c⑬][⑭1⑭] -⑰ polygon[⑮a⑮][⑯1⑯]⑱) -㉝ (㉕polygon[⑳b⑳][㉑1㉑] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

⑬ `polygon[c][` → ６４文字以内（１１文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (⑱polygon[c][⑭1⑭] -⑰ polygon[⑮a⑮][⑯1⑯]⑱) -㉝ (㉕polygon[⑳b⑳][㉑1㉑] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

⑭ `polygon[c][1] -` → ６４文字以内（１５文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (⑱polygon[c][1] -⑰ polygon[⑮a⑮][⑯1⑯]⑱) -㉝ (㉕polygon[⑳b⑳][㉑1㉑] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

⑮ `polygon[a][` → ６４文字以内（１１文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (⑱polygon[c][1] -⑰ polygon[a][⑯1⑯]⑱) -㉝ (㉕polygon[⑳b⑳][㉑1㉑] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

⑯ `polygon[a][1]` → ６４文字以内（１３文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (⑱polygon[c][1] -⑰ polygon[a][1]⑱) -㉝ (㉕polygon[⑳b⑳][㉑1㉑] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

⑰ `polygon[c][1] - polygon[a][1]` → ６４文字以内（２９文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (⑱polygon[c][1] - polygon[a][1]⑱) -㉝ (㉕polygon[⑳b⑳][㉑1㉑] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

⑱ `(polygon[c][1] - polygon[a][1]) -` → ６４文字以内（３３文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (polygon[c][1] - polygon[a][1]) -㉝ (㉕polygon[⑳b⑳][㉑1㉑] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

⑲ `(polygon[b][0] - polygon[a][0]) * (polygon[c][1] - polygon[a][1]) -` → ６４文字超（６７文字）の為，**分割する**．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (polygon[c][1] - polygon[a][1]) -㉝ (㉕polygon[⑳b⑳][㉑1㉑] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

⑳ `polygon[b][` → ６４文字以内（１１文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (polygon[c][1] - polygon[a][1]) -㉝ (㉕polygon[b][㉑1㉑] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

㉑ `polygon[b][1] -` → ６４文字以内（１５文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (polygon[c][1] - polygon[a][1]) -㉝ (㉕polygon[b][1] -㉔ polygon[㉒a㉒][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

㉒ `polygon[a][` → ６４文字以内（１１文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (polygon[c][1] - polygon[a][1]) -㉝ (㉕polygon[b][1] -㉔ polygon[a][㉓1㉓]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

㉓ `polygon[a][1]` → ６４文字以内（１３文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (polygon[c][1] - polygon[a][1]) -㉝ (㉕polygon[b][1] -㉔ polygon[a][1]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

㉔ `polygon[b][1] - polygon[a][1]` → ６４文字以内（２９文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (polygon[c][1] - polygon[a][1]) -㉝ (㉕polygon[b][1] - polygon[a][1]㉕) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

㉕ `(polygon[b][1] - polygon[a][1]) *` → ６４文字以内（３３文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (polygon[c][1] - polygon[a][1]) -㉝ (polygon[b][1] - polygon[a][1]) *㉜ (㉛polygon[㉖c㉖][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

㉖ `polygon[c][` → ６４文字以内（１１文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (polygon[c][1] - polygon[a][1]) -㉝ (polygon[b][1] - polygon[a][1]) *㉜ (㉛polygon[c][㉗0㉗] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

㉗ `polygon[c][0] -` → ６４文字以内（１５文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (polygon[c][1] - polygon[a][1]) -㉝ (polygon[b][1] - polygon[a][1]) *㉜ (㉛polygon[c][0] -㉚ polygon[㉘a㉘][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

㉘ `polygon[a][` → ６４文字以内（１１文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (polygon[c][1] - polygon[a][1]) -㉝ (polygon[b][1] - polygon[a][1]) *㉜ (㉛polygon[c][0] -㉚ polygon[a][㉙0㉙]㉛)㉞) <㉟ tolerance;㊳`

㉙ `polygon[a][0]` → ６４文字以内（１３文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (polygon[c][1] - polygon[a][1]) -㉝ (polygon[b][1] - polygon[a][1]) *㉜ (㉛polygon[c][0] -㉚ polygon[a][0]㉛)㉞) <㉟ tolerance;㊳`

㉚ `polygon[c][0] - polygon[a][0]` → ６４文字以内（２９文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (polygon[c][1] - polygon[a][1]) -㉝ (polygon[b][1] - polygon[a][1]) *㉜ (㉛polygon[c][0] - polygon[a][0]㉛)㉞) <㉟ tolerance;㊳`

㉛ `(polygon[c][0] - polygon[a][0])` → ６４文字以内（３１文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (polygon[c][1] - polygon[a][1]) -㉝ (polygon[b][1] - polygon[a][1]) *㉜ (polygon[c][0] - polygon[a][0])㉞) <㉟ tolerance;㊳`

㉜ `(polygon[b][1] - polygon[a][1]) * (polygon[c][0] - polygon[a][0])` → ６４文字超（６５文字）の為，**分割する**．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (polygon[c][1] - polygon[a][1]) -㉝ (polygon[b][1] - polygon[a][1]) *㉜ (polygon[c][0] - polygon[a][0])㉞) <㉟ tolerance;㊳`

㉝ `(polygon[c][1] - polygon[a][1]) - (polygon[b][1] - polygon[a][1]) *` → ６４文字超（６７文字）の為，**分割する**．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (polygon[c][1] - polygon[a][1]) -㉝ (polygon[b][1] - polygon[a][1]) *㉜ (polygon[c][0] - polygon[a][0])㉞) <㉟ tolerance;㊳`

㉞ `Math.abs((polygon[b][0] - polygon[a][0]) *`，`(polygon[c][0] - polygon[a][0])) <` → 区間が複数ある為，**分割する**．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (polygon[c][1] - polygon[a][1]) -㉝ (polygon[b][1] - polygon[a][1]) *㉜ (polygon[c][0] - polygon[a][0])㉞) <㉟ tolerance;㊳`

㉟ `) < tolerance;` → ６４文字以内（１４文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean =>㊱ Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (polygon[c][1] - polygon[a][1]) -㉝ (polygon[b][1] - polygon[a][1]) *㉜ (polygon[c][0] - polygon[a][0])㉞) < tolerance;㊳`

㊱ `(a: number, b: number, c: number): boolean => Math.abs(` → ６４文字以内（５５文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean => Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (polygon[c][1] - polygon[a][1]) -㉝ (polygon[b][1] - polygon[a][1]) *㉜ (polygon[c][0] - polygon[a][0])㉞) < tolerance;㊳`

㊲ `const isCollinear = (a: number, b: number, c: number): boolean => Math.abs(` → ６４文字超（７５文字）の為，**分割する**．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean => Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (polygon[c][1] - polygon[a][1]) -㉝ (polygon[b][1] - polygon[a][1]) *㉜ (polygon[c][0] - polygon[a][0])㉞) < tolerance;㊳`

㊳ `) < tolerance;` → ６４文字以内（１４文字）の為，分割しない．

`const isCollinear =㊲ (a: number, b: number, c: number): boolean => Math.abs(㉞(polygon[b][0] - polygon[a][0]) *⑲ (polygon[c][1] - polygon[a][1]) -㉝ (polygon[b][1] - polygon[a][1]) *㉜ (polygon[c][0] - polygon[a][0])㉞) < tolerance;` 残った分割位置（⑲，㉜，㉝，㉞，㊲）に，スペースが有れば削除した上で改行を入れ，括弧の内側のインデントを深くして完了．

### 5.3 最終出力

```
const isCollinear =
(a: number, b: number, c: number): boolean => Math.abs(
	(polygon[b][0] - polygon[a][0]) *
	(polygon[c][1] - polygon[a][1]) -
	(polygon[b][1] - polygon[a][1]) *
	(polygon[c][0] - polygon[a][0])
) < tolerance;
```

## 6. 例３: `for(let i = 0; ...)`

### 6.1 AST ダンプ

```
└─ [N] for(let i = 0; i < imageData.data.length; i += 4) { if(...) { imageData.data.set(...); } }
	├─ [U] for
	├─ [U] ( ⑲
	├─ [N] let i = 0;
	│	├─ [U] let
	│	├─ [N] i = 0
	│	│	├─ [N] i
	│	│	├─ [U] = ①
	│	│	└─ [N] 0
	│	└─ [U] ; ②
	├─ [N] i < imageData.data.length
	│	├─ [N] i
	│	├─ [U] < ⑤
	│	└─ [N] imageData.data.length
	│		├─ [N] imageData.data
	│		│	├─ [N] imageData
	│		│	├─ [U] . ③
	│		│	└─ [N] data
	│		├─ [U] . ④
	│		└─ [N] length
	├─ [U] ; ⑲
	├─ [N] i += 4
	│	├─ [N] i
	│	├─ [U] += ⑥
	│	└─ [N] 4
	├─ [U] ) ⑲
	└─ [N] { if(imageData.data[i + 3] < alpha) { imageData.data.set(...); } }
		├─ [U] { ⑱
		├─ [N] if(imageData.data[i + 3] < alpha) { imageData.data.set(...); }
		│	├─ [U] if
		│	├─ [N] (imageData.data[i + 3] < alpha)
		│	│	├─ [U] ( ⑪
		│	│	├─ [N] imageData.data[i + 3] < alpha
		│	│	│	├─ [N] imageData.data[i + 3]
		│	│	│	│	├─ [N] imageData.data
		│	│	│	│	│	├─ [N] imageData
		│	│	│	│	│	├─ [U] . ⑦
		│	│	│	│	│	└─ [N] data
		│	│	│	│	├─ [U] [ ⑨
		│	│	│	│	├─ [N] i + 3
		│	│	│	│	│	├─ [N] i
		│	│	│	│	│	├─ [U] + ⑧
		│	│	│	│	│	└─ [N] 3
		│	│	│	│	└─ [U] ] ⑨
		│	│	│	├─ [U] < ⑩
		│	│	│	└─ [N] alpha
		│	│	└─ [U] ) ⑪
		│	└─ [N] { imageData.data.set([0XFF, 0XFF, 0XFF, 0X1], i); }
		│		├─ [U] { ⑰
		│		├─ [N] imageData.data.set([0XFF, 0XFF, 0XFF, 0X1], i);
		│		│	├─ [N] imageData.data.set([0XFF, 0XFF, 0XFF, 0X1], i)
		│		│	│	├─ [N] imageData.data.set
		│		│	│	│	├─ [N] imageData.data
		│		│	│	│	│	├─ [N] imageData
		│		│	│	│	│	├─ [U] . ⑫
		│		│	│	│	│	└─ [N] data
		│		│	│	│	├─ [U] . ⑬
		│		│	│	│	└─ [N] set
		│		│	│	└─ [N] ([0XFF, 0XFF, 0XFF, 0X1], i)
		│		│	│		├─ [U] ( ⑮
		│		│	│		├─ [N] [0XFF, 0XFF, 0XFF, 0X1]
		│		│	│		│	├─ [U] [ ⑭
		│		│	│		│	├─ [N] 0XFF
		│		│	│		│	├─ [U] , ⑭
		│		│	│		│	├─ [N] 0XFF
		│		│	│		│	├─ [U] , ⑭
		│		│	│		│	├─ [N] 0XFF
		│		│	│		│	├─ [U] , ⑭
		│		│	│		│	├─ [N] 0X1
		│		│	│		│	└─ [U] ] ⑭
		│		│	│		├─ [U] , ⑮
		│		│	│		├─ [N] i
		│		│	│		└─ [U] ) ⑮
		│		│	└─ [U] ; ⑯
		│		└─ [U] } ⑰
		└─ [U] } ⑱
```

### 6.2 番号付与済の評価対象

定数の英字は整形の前段 (EditPass) で大文字へ揃う（`0xff` → `0XFF`）為，行分割は揃えた後の文字列で判定する．

`for(⑲let i =① 0;② i <⑤ imageData③.data④.length;⑲ i +=⑥ 4⑲) {⑱ if(⑪imageData⑦.data[⑨i +⑧ 3⑨] <⑩ alpha⑪) {⑰ imageData⑫.data⑬.set(⑮[⑭0XFF,⑭ 0XFF,⑭ 0XFF,⑭ 0X1⑭],⑮ i⑮);⑯ ⑰} ⑱}`

① `let i = 0;` → ６４文字以内（１０文字）の為，分割しない．

`for(⑲let i = 0;② i <⑤ imageData③.data④.length;⑲ i +=⑥ 4⑲) {⑱ if(⑪imageData⑦.data[⑨i +⑧ 3⑨] <⑩ alpha⑪) {⑰ imageData⑫.data⑬.set(⑮[⑭0XFF,⑭ 0XFF,⑭ 0XFF,⑭ 0X1⑭],⑮ i⑮);⑯ ⑰} ⑱}`

② `let i = 0; i <` → ６４文字以内（１４文字）の為，分割しない．

`for(⑲let i = 0; i <⑤ imageData③.data④.length;⑲ i +=⑥ 4⑲) {⑱ if(⑪imageData⑦.data[⑨i +⑧ 3⑨] <⑩ alpha⑪) {⑰ imageData⑫.data⑬.set(⑮[⑭0XFF,⑭ 0XFF,⑭ 0XFF,⑭ 0X1⑭],⑮ i⑮);⑯ ⑰} ⑱}`

③ `imageData.data` → ６４文字以内（１４文字）の為，分割しない．

`for(⑲let i = 0; i <⑤ imageData.data④.length;⑲ i +=⑥ 4⑲) {⑱ if(⑪imageData⑦.data[⑨i +⑧ 3⑨] <⑩ alpha⑪) {⑰ imageData⑫.data⑬.set(⑮[⑭0XFF,⑭ 0XFF,⑭ 0XFF,⑭ 0X1⑭],⑮ i⑮);⑯ ⑰} ⑱}`

④ `imageData.data.length;` → ６４文字以内（２２文字）の為，分割しない．

`for(⑲let i = 0; i <⑤ imageData.data.length;⑲ i +=⑥ 4⑲) {⑱ if(⑪imageData⑦.data[⑨i +⑧ 3⑨] <⑩ alpha⑪) {⑰ imageData⑫.data⑬.set(⑮[⑭0XFF,⑭ 0XFF,⑭ 0XFF,⑭ 0X1⑭],⑮ i⑮);⑯ ⑰} ⑱}`

⑤ `let i = 0; i < imageData.data.length;` → ６４文字以内（３７文字）の為，分割しない．

`for(⑲let i = 0; i < imageData.data.length;⑲ i +=⑥ 4⑲) {⑱ if(⑪imageData⑦.data[⑨i +⑧ 3⑨] <⑩ alpha⑪) {⑰ imageData⑫.data⑬.set(⑮[⑭0XFF,⑭ 0XFF,⑭ 0XFF,⑭ 0X1⑭],⑮ i⑮);⑯ ⑰} ⑱}`

⑥ `i += 4` → ６４文字以内（６文字）の為，分割しない．

`for(⑲let i = 0; i < imageData.data.length;⑲ i += 4⑲) {⑱ if(⑪imageData⑦.data[⑨i +⑧ 3⑨] <⑩ alpha⑪) {⑰ imageData⑫.data⑬.set(⑮[⑭0XFF,⑭ 0XFF,⑭ 0XFF,⑭ 0X1⑭],⑮ i⑮);⑯ ⑰} ⑱}`

⑦ `imageData.data[` → ６４文字以内（１５文字）の為，分割しない．

`for(⑲let i = 0; i < imageData.data.length;⑲ i += 4⑲) {⑱ if(⑪imageData.data[⑨i +⑧ 3⑨] <⑩ alpha⑪) {⑰ imageData⑫.data⑬.set(⑮[⑭0XFF,⑭ 0XFF,⑭ 0XFF,⑭ 0X1⑭],⑮ i⑮);⑯ ⑰} ⑱}`

⑧ `i + 3` → ６４文字以内（５文字）の為，分割しない．

`for(⑲let i = 0; i < imageData.data.length;⑲ i += 4⑲) {⑱ if(⑪imageData.data[⑨i + 3⑨] <⑩ alpha⑪) {⑰ imageData⑫.data⑬.set(⑮[⑭0XFF,⑭ 0XFF,⑭ 0XFF,⑭ 0X1⑭],⑮ i⑮);⑯ ⑰} ⑱}`

⑨ `imageData.data[i + 3] <` → ６４文字以内（２３文字）の為，分割しない．

`for(⑲let i = 0; i < imageData.data.length;⑲ i += 4⑲) {⑱ if(⑪imageData.data[i + 3] <⑩ alpha⑪) {⑰ imageData⑫.data⑬.set(⑮[⑭0XFF,⑭ 0XFF,⑭ 0XFF,⑭ 0X1⑭],⑮ i⑮);⑯ ⑰} ⑱}`

⑩ `imageData.data[i + 3] < alpha` → ６４文字以内（２９文字）の為，分割しない．

`for(⑲let i = 0; i < imageData.data.length;⑲ i += 4⑲) {⑱ if(⑪imageData.data[i + 3] < alpha⑪) {⑰ imageData⑫.data⑬.set(⑮[⑭0XFF,⑭ 0XFF,⑭ 0XFF,⑭ 0X1⑭],⑮ i⑮);⑯ ⑰} ⑱}`

⑪ `if(imageData.data[i + 3] < alpha) {` → ６４文字以内（３５文字）の為，分割しない．

`for(⑲let i = 0; i < imageData.data.length;⑲ i += 4⑲) {⑱ if(imageData.data[i + 3] < alpha) {⑰ imageData⑫.data⑬.set(⑮[⑭0XFF,⑭ 0XFF,⑭ 0XFF,⑭ 0X1⑭],⑮ i⑮);⑯ ⑰} ⑱}`

⑫ `imageData.data` → ６４文字以内（１４文字）の為，分割しない．

`for(⑲let i = 0; i < imageData.data.length;⑲ i += 4⑲) {⑱ if(imageData.data[i + 3] < alpha) {⑰ imageData.data⑬.set(⑮[⑭0XFF,⑭ 0XFF,⑭ 0XFF,⑭ 0X1⑭],⑮ i⑮);⑯ ⑰} ⑱}`

⑬ `imageData.data.set(` → ６４文字以内（１９文字）の為，分割しない．

`for(⑲let i = 0; i < imageData.data.length;⑲ i += 4⑲) {⑱ if(imageData.data[i + 3] < alpha) {⑰ imageData.data.set(⑮[⑭0XFF,⑭ 0XFF,⑭ 0XFF,⑭ 0X1⑭],⑮ i⑮);⑯ ⑰} ⑱}`

⑭ `[0XFF, 0XFF, 0XFF, 0X1],` → ６４文字以内（２４文字）の為，分割しない．

`for(⑲let i = 0; i < imageData.data.length;⑲ i += 4⑲) {⑱ if(imageData.data[i + 3] < alpha) {⑰ imageData.data.set(⑮[0XFF, 0XFF, 0XFF, 0X1],⑮ i⑮);⑯ ⑰} ⑱}`

⑮ `imageData.data.set([0XFF, 0XFF, 0XFF, 0X1], i);` → ６４文字以内（４７文字）の為，分割しない．

`for(⑲let i = 0; i < imageData.data.length;⑲ i += 4⑲) {⑱ if(imageData.data[i + 3] < alpha) {⑰ imageData.data.set([0XFF, 0XFF, 0XFF, 0X1], i);⑯ ⑰} ⑱}`

⑯ `imageData.data.set([0XFF, 0XFF, 0XFF, 0X1], i);` → ６４文字以内（４７文字）の為，分割しない．

`for(⑲let i = 0; i < imageData.data.length;⑲ i += 4⑲) {⑱ if(imageData.data[i + 3] < alpha) {⑰ imageData.data.set([0XFF, 0XFF, 0XFF, 0X1], i); ⑰} ⑱}`

⑰ `if(imageData.data[i + 3] < alpha) imageData.data.set([0XFF, 0XFF, 0XFF, 0X1], i);` → ブレースを外して６４文字超（８１文字）の為，**分割する**．

`for(⑲let i = 0; i < imageData.data.length;⑲ i += 4⑲) {⑱ if(imageData.data[i + 3] < alpha) {⑰ imageData.data.set([0XFF, 0XFF, 0XFF, 0X1], i); ⑰} ⑱}`

⑱ `) { if(imageData.data[i + 3] < alpha) {`，`} }` → 区間が複数ある為，**分割する**．

`for(⑲let i = 0; i < imageData.data.length;⑲ i += 4⑲) {⑱ if(imageData.data[i + 3] < alpha) {⑰ imageData.data.set([0XFF, 0XFF, 0XFF, 0X1], i); ⑰} ⑱}`

⑲ `for(let i = 0; i < imageData.data.length; i += 4) {` → ６４文字以内（５１文字）の為，分割しない．

`for(let i = 0; i < imageData.data.length; i += 4) {⑱ if(imageData.data[i + 3] < alpha) {⑰ imageData.data.set([0XFF, 0XFF, 0XFF, 0X1], i); ⑰} ⑱}` 残った分割位置（⑰，⑱）に，スペースが有れば削除した上で改行を入れ，括弧の内側のインデントを深くして完了．

### 6.3 最終出力

```
for(let i = 0; i < imageData.data.length; i += 4) {
	if(imageData.data[i + 3] < alpha) {
		imageData.data.set([0XFF, 0XFF, 0XFF, 0X1], i);
	}
}
```

## 7. 関連仕様 / 実装

- 処理パイプライン全体における LineSplit の位置付け: [processing_spec.md](processing_spec.md) § 3.7
- 番号付け規則・分割判定の実装: [src/Pass/LineSplit.cpp](../src/Pass/LineSplit.cpp) の `CollectBreaks` / `CollectBreaksParallel` / `GreedyMerge`
- unnamed トークンの前後の分割側判定: 同ファイルの `SplitSide` / `ComputeBreakPos`
- 制御構文 + 非ブロック本体のブレース付与: 同ファイルの `FindCtrlFlows` / `FindCtrlFlowsParallel`
- 統合変数宣言の MaxChars 超過時の別宣言分割（本書では扱わない）: 同ファイルの `SplitWideVarDecls`
- Ruby の１行の分岐の節境界分割（本書では扱わない）: 同ファイルの `BreakWideRubyBranches`

---

*本仕様書はコード実装と併せて保守する．実装が変更された場合は本書を必ず追従させる事．*
