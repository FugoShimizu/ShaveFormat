#include "SyntaxCheck.hpp"
#include "NodeKind.hpp"
#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * 許容破損の判定関数（解析器の未対応に由来し，書掛ではない `ERROR` / `MISSING`）
 * @param Src 判定対象を含むソース
 * @param Node 判定対象のノード
 * @param Ancestors Node の祖先の並び（走査の道筋から渡す，末尾が親）
 * @return 解析器の追随遅れに由来するなら true
 */
bool SyntaxCheck::IsToleratedBreak(const TSSource &Src, const TSNode Node, const std::span<const TSNode> Ancestors) {
	// Sass の正当な `as *` 取込・先頭結合子に解析器が補う空選択子の `MISSING` を許容
	const char *const Type = ts_node_type(Node);
	// 型を持たない節点は許容対象でない事の返戻
	if(!Type) return false;
	// 節点型名のビュー
	const std::string_view TypeView = Type;
	// 解析器の追随遅れに由来する空のネスト選択子
	if(TypeView == "nesting_selector") return true;
	// Sass の空の仮引数・実引数に対する誤った `MISSING` / `ERROR` を許容
	if(!Ancestors.empty()) {
		// 直近の祖先節点
		const TSNode Parent = Ancestors.back();
		// 親節点の型名
		const std::string_view ParentType = ts_node_type(Parent);
		// 仮引数の祖父を調べ，空の引数と未対応 placeholder 名を許容する事の返戻
		if(
			const TSNode Host =
			ParentType == "parameter" && !Src.Len(Parent) && Ancestors.size() > 2 ? Ancestors[Ancestors.size() - 3] : TSNode{};
			ts_node_is_missing(Node) && !ts_node_is_null(Host) && NodeKind::ScssParameterHost.Contains(Host) ||
			TypeView == "ERROR" && ParentType == "include_statement" && ts_node_child_count(Node) == 2 &&
			Src.View(ts_node_child(Node, 0)) == "(" || TypeView == "identifier" && ParentType == "placeholder"
			// 解析器の既知の誤読を許容する事の返戻
		) return true;
	}
	// 取込の `as` の `ERROR` だけ許容する返戻（空選択子と対で許し，真の破損は保持）
	return TypeView == "ERROR" && Src.View(Node) == "as" && std::any_of(
		Ancestors.begin(),
		Ancestors.end(),
		[](const TSNode Ancestor) -> bool {
			// `@use` 規則の内側かの判定の返戻
			return std::string_view(ts_node_type(Ancestor)) == "use_statement";
		}
	);
}

/**
 * 構文破損ノードの判定関数（`ERROR` / `MISSING`，但し文字列リテラルの内側は除く）
 * @param Src 判定対象を含むソース
 * @param Node 判定対象のノード
 * @param Ancestors Node の祖先の並び（走査の道筋から渡す，末尾が親，根では空）
 * @return 構文の破損と見做すなら true
 */
bool SyntaxCheck::IsBrokenNode(const TSSource &Src, const TSNode Node, const std::span<const TSNode> Ancestors) {
	// 欠落節点か
	const bool IsMissing = ts_node_is_missing(Node);
	// `ERROR` 照合用の型名
	const char *const Type = ts_node_type(Node);
	// `ERROR` と `MISSING` の対を同条件で許容する解析器追随遅れの扱い
	if(!IsMissing && (!Type || std::string_view(Type) != "ERROR") || IsToleratedBreak(Src, Node, Ancestors)) return false;
	// 文字列直下の `ERROR` だけ許容し，`MISSING` 又は文字列外を破損とする返戻
	return IsMissing || Ancestors.empty() || !NodeKind::StringLikeAll.Contains(Ancestors.back());
}

/**
 * JSONC の末尾コンマを表す破損からコンマを得る関数
 * @param Src 判定対象を含むソース
 * @param Node 判定対象の `ERROR` 又は `MISSING`
 * @return 配列又は物体の最後の要素に続くコンマの字句（該当しなければ空）
 */
TSNode SyntaxCheck::JsonTrailingCommaToken(const TSSource &Src, const TSNode Node) {
	// 欠落として補われたコンマか
	const bool IsMissing = ts_node_is_missing(Node);
	// JSONC の末尾コンマに対応して居ない解析器が立てる形だけを受け入れる
	if(!IsMissing && (std::string_view(ts_node_type(Node)) != "ERROR" || Src.View(Node) != ",")) return TSNode{};
	// 破損を含む親節点
	const TSNode Parent = ts_node_parent(Node);
	// 親を持たない破損は列の末尾ではない事の返戻
	if(ts_node_is_null(Parent)) return TSNode{};
	// JSON の配列と物体以外に現れたコンマは許容しない事の返戻
	if(const std::string_view ParentType = ts_node_type(Parent); ParentType != "array" && ParentType != "object") return TSNode{};
	// 対象通過済と後続値有無の旗
	bool IsAfter = false, HasValueAfter = false;
	// 直前の値と其れに続くコンマ
	TSNode LastValue {}, Comma {};
	ForEachChild(
		Parent,
		[&](const TSNode Child) -> void {
			// 対象破損へ到達した場合
			if(ts_node_eq(Child, Node)) {
				// 対象破損通過済の記録
				IsAfter = true;
				// 終了
				return;
			}
			// 対象破損より後の子の場合
			if(IsAfter) {
				if(ts_node_is_named(Child) && !NodeKind::Comment.Contains(Child)) HasValueAfter = true;
				// 終了
				return;
			}
			if(const bool IsNamed = ts_node_is_named(Child); IsNamed && !NodeKind::Comment.Contains(Child)) {
				// 対象破損より前の最後の値
				LastValue = Child;
				// 新しい値に対応するコンマの初期化
				Comma = {};
			} else if(!IsNamed && !ts_node_is_null(LastValue) && Src.View(Child) == ",") Comma = Child;
		}
	);
	if(!IsMissing && !ts_node_is_null(LastValue) && ts_node_child_count(Node) == 1) Comma = ts_node_child(Node, 0);
	// 空の列の `[,]` / `{,}` は末尾コンマでなく構文誤りで，後続に値が在るコンマも区切りの誤りとして許容しない事の返戻
	return IsAfter && !ts_node_is_null(LastValue) && !HasValueAfter ? Comma : TSNode{};
}

/**
 * 壊れたノード（`ERROR` / `MISSING`）の計数関数
 * @param Src 走査対象を含むソース
 * @param Node 走査対象のルートノード
 * @return 壊れたノードの個数
 */
uint32_t SyntaxCheck::CountBrokenNodes(const TSSource &Src, const TSNode Node) {
	// 破損節点の計数値
	uint32_t Count = 0;
	HasDescendantWithAncestorsOf(
		Node,
		[&Src, &Count](const TSNode Cur, const std::span<const TSNode> Ancestors) -> bool {
			// 破損節点の計数
			if(IsBrokenNode(Src, Cur, Ancestors)) ++Count;
			// 全ノードを数える迄走査を続ける事の返戻
			return false;
		}
	);
	// 壊れたノードの個数の返戻
	return Count;
}

/**
 * 最初の壊れたノードの位置の取得関数（部分整形の警告を誤りの在る行へ出す）
 * @param Src 走査対象のソース
 * @return 先行順で最初の壊れたノード（`ERROR` / `MISSING`）の開始位置（無ければ先頭）
 */
TSPoint SyntaxCheck::FirstBrokenPoint(const TSSource &Src) {
	TSPoint Point { 0, 0 };
	// 構文木を取得済の場合
	if(Src.IsParsed()) {
		HasDescendantWithAncestorsOf(
			Src.GetRoot(),
			[&Src, &Point](const TSNode Cur, const std::span<const TSNode> Ancestors) -> bool {
				// 壊れて居ない節点を読み進める事の返戻
				if(!IsBrokenNode(Src, Cur, Ancestors)) return false;
				// 最初の破損位置の記録
				Point = ts_node_start_point(Cur);
				// 最初の壊れたノードで打ち切る事の返戻
				return true;
			}
		);
	}
	// 位置の返戻
	return Point;
}

/**
 * C の文法に反する関数定義の計数関数
 * tree-sitter-c は C++ の `class A { ... };` を型 `class`・宣言子 `A` の関数定義と読み，破損を立てない
 * C の関数定義の宣言子は関数の宣言子に限る為，其れ以外の宣言子を持つ関数定義を C としての破損に数える
 * @param Src 走査対象のソース（C として解析済）
 * @return 宣言子が関数の宣言子でない関数定義の個数
 */
uint32_t SyntaxCheck::CountMalformedCDefinitions(const TSSource &Src) {
	// 不正な関数定義の計数値
	uint32_t Count = 0;
	WalkAst(
		Src.GetRoot(),
		[&Count](const TSNode Node) -> void {
			// C の関数定義と宣言子の抽出
			if(std::string_view(ts_node_type(Node)) != "function_definition") return;
			TSNode Declarator = TSSource::FieldChild(Node, "declarator");
			while(!ts_node_is_null(Declarator) && NodeKind::CDeclaratorWrapper.Contains(Declarator)) {
				const TSNode Inner = TSSource::FieldChild(Declarator, "declarator");
				Declarator = ts_node_is_null(Inner) ? ts_node_named_child(Declarator, 0) : Inner;
			}
			if(ts_node_is_null(Declarator) || std::string_view(ts_node_type(Declarator)) != "function_declarator") ++Count;
		}
	);
	// 関数の宣言子を持たない関数定義の個数の返戻
	return Count;
}

/**
 * 壊れたノードの有無判定関数（`ERROR` 又は `MISSING` の子孫）
 * @param Src 判定対象を含むソース
 * @param Node 判定対象のノード
 * @return 壊れた子孫が有れば true
 */
bool SyntaxCheck::HasErrorDescendant(const TSSource &Src, const TSNode Node) {
	// 未読字句の `ERROR` と補完字句の `MISSING` の有無の返戻（Ruby の `rescue X end` 等は `MISSING` だけの為，両者を検査）
	return HasDescendantWithAncestorsOf(
		Node,
		[&Src](const TSNode Cur, const std::span<const TSNode> Ancestors) -> bool {
			// 入力と出力で同じ破損定義を使い，不要な差戻を防ぐ判定結果の返戻
			return IsBrokenNode(Src, Cur, Ancestors);
		}
	);
}

/**
 * 入力エラー状態の収集関数（`ERROR`・`MISSING` の検出と，破損を整形しない言語の判定）
 * @param Src 解析済の TSSource
 * @param Language 対象言語
 * @param HasInputError 入力時点で `ERROR` 又は `MISSING` ノードが存在したら true を設定
 * @param SkipReason 整形を行わなかった理由の格納先（静的な字面を指す）
 * @return 整形続行で true，整形しない構文誤りの検出で false（原文返戻）
 */
bool SyntaxCheck::ScanInputErrorStates(
	const TSSource &Src,
	const Lang Language,
	bool &HasInputError,
	std::string_view &SkipReason
) {
	// 入力破損の種類別の旗
	bool HasInputMissing = false, HasJsxError = false, HasOpenString = false;
	// 未閉じ文字列を同じ行の引用符対と区別し，索引を再利用して残りの本文・字下げを保護
	std::vector<uint32_t> LineEnds;
	std::unordered_map<std::string_view, std::vector<uint32_t>> QuotePositions;
	const auto IsUnclosed = [&Src, &LineEnds, &QuotePositions](const uint32_t Start, const std::string_view Quote) -> bool {
		// 指定位置の引用符の閉鎖判定
		const std::string &Text = Src;
		if(LineEnds.empty()) {
			// 原文中の行末位置の収集
			for(size_t Found = Text.find('\n'); Found != std::string::npos; Found = Text.find('\n', Found + 1)) LineEnds.push_back(Found);
			// 原文終端の行末登録
			LineEnds.push_back(static_cast<uint32_t>(Text.size()));
		}
		const auto [Iter, IsFresh] = QuotePositions.try_emplace(Quote);
		std::vector<uint32_t> &Positions = Iter->second;
		if(IsFresh) {
			// 当該引用符の出現位置収集
			for(size_t Found = Text.find(Quote); Found != std::string::npos; Found = Text.find(Quote, Found + Quote.size())) {
				Positions.push_back(Found);
			}
		}
		// 対象位置の属する行末
		const std::vector<uint32_t>::const_iterator LineEnd = std::lower_bound(LineEnds.begin(), LineEnds.end(), Start);
		const uint32_t LineStart = LineEnd == LineEnds.begin() ? 0 : *(LineEnd - 1) + 1;
		// 開き候補の位置
		const std::vector<uint32_t>::const_iterator Here = std::lower_bound(Positions.begin(), Positions.end(), Start);
		// 開き候補の後続引用符
		const std::vector<uint32_t>::const_iterator Close = std::lower_bound(Here, Positions.cend(), Start + Quote.size());
		// 行の手前の引用符が偶数個（開きの引用符）で，行末迄に閉じの引用符が無いかの返戻
		return !(Here - std::lower_bound(Positions.cbegin(), Here, LineStart) & 1) && (Close == Positions.end() || *Close >= *LineEnd);
	};
	// 接頭辞後の未閉じ判定（生・三重・逐語文字列は残り全体で閉じを探索）
	const auto OpensUnclosed = [&Src, &IsUnclosed](const uint32_t Start) -> bool {
		// 文字列接頭辞に対応する閉じ引用符の探索
		const std::string &Whole = Src;
		const std::string_view Text = std::string_view(Whole).substr(Start, 0X20);
		size_t Prefix = 0;
		for(size_t Length = std::min<size_t>(Text.size(), 3); Length; --Length) {
			if(NodeKind::StringPrefix.Contains(Text.substr(0, Length))) {
				// 一致した接頭辞長の記録
				Prefix = Length;
				break;
			}
		}
		const std::string_view Rest = Text.substr(Prefix);
		// 接頭辞の後に字句が無ければ文字列の開きではない事の返戻
		if(Rest.empty()) return false;
		std::string Close;
		size_t OpenLength = 1;
		if(Rest.front() == '#' && (!Prefix || Text[Prefix - 1] == 'r')) {
			const size_t Hashes = Rest.find_first_not_of('#');
			// `#` の並びの後が引用符でなければ文字列の開きではない事の返戻
			if(Hashes == std::string_view::npos || Rest[Hashes] != '"') return false;
			const size_t Quotes = Rest.substr(Hashes).starts_with("\"\"\"") ? 3 : 1;
			Close.assign(Quotes, '"').append(Hashes, '#');
			OpenLength = Hashes + Quotes;
			// 引用符無の返戻
		} else if(Rest.front() != '"' && Rest.front() != '\'' && Rest.front() != '`') return false;
		else if(Prefix && Text[Prefix - 1] == 'R' && Rest.front() == '"') {
			const size_t Paren = Rest.find('(');
			// 区切りの `(` が無ければ生文字列の開きではない事の返戻
			if(Paren == std::string_view::npos) return false;
			Close.assign(1, ')').append(Rest.substr(1, Paren - 1)).push_back('"');
			OpenLength = Paren + 1;
		} else if(Rest.starts_with("\"\"\"") || Rest.starts_with("'''")) {
			OpenLength = std::min(Rest.find_first_not_of(Rest.front()), Rest.size());
			Close.assign(OpenLength, Rest.front());
		} else if(Text.substr(0, Prefix).find('@') != std::string_view::npos) Close = "\"";
		// 通常の引用符が閉じて居ないかの返戻
		else return IsUnclosed(Start + static_cast<uint32_t>(Prefix), Rest.substr(0, 1));
		// 行を跨げる文字列の閉じの区切りがファイルの残りに無いかの返戻
		return Whole.find(Close, Start + Prefix + OpenLength) == std::string::npos;
	};
	// JSX は最外容器を１度だけ調べ，`ERROR` 毎の祖先探索の二乗化を回避
	uint32_t JsxCheckedEnd = 0;
	// 既存 `ERROR` は部分整形で許容し新規分だけ破壊判定（O(1) の旗で健全なら全木走査を省略）
	if(Src.IsParsed() && ts_node_has_error(Src.GetRoot())) {
		HasDescendantWithAncestorsOf(
			Src.GetRoot(),
			[&](const TSNode Cur, const std::span<const TSNode> Ancestors) -> bool {
				// 構文破損候補の種別取得
				const char *const Type = ts_node_type(Cur);
				// 型を持たない節点の内側は調べない事の返戻
				if(!Type) return false;
				if(Language.IsJsTs() && Src.Start(Cur) >= JsxCheckedEnd && ts_node_has_error(Cur) && NodeKind::JsxContainer.Contains(Type)) {
					// 検査済 JSX 容器範囲の更新
					JsxCheckedEnd = Src.End(Cur);
					HasJsxError = HasDescendantWithAncestorsOf(
						Cur,
						[&Src](const TSNode Inner, const std::span<const TSNode> InnerAncestors) -> bool {
							// 破損した `ERROR` かの返戻
							return std::string_view(ts_node_type(Inner)) == "ERROR" && IsBrokenNode(Src, Inner, InnerAncestors);
						}
					);
				}
				// 破損の判定は `IsBrokenNode` に集約する（文字列リテラル直下の `ERROR` は中身の字面で構文は壊れて居ない）
				const bool IsTreeBroken =
				IsBrokenNode(Src, Cur, Ancestors) && !(Language == Lang::JSON && !ts_node_is_null(JsonTrailingCommaToken(Src, Cur)));
				if(IsTreeBroken && std::string_view(Type) == "ERROR") {
					HasOpenString = OpensUnclosed(Src.Start(Cur));
					if(!HasOpenString) {
						ForEachChild(
							Cur,
							[&](const TSNode Child) -> bool {
								// C++ の `R"` 等の接頭辞付開始字句を含む引用符候補
								if(
									const std::string_view Token = ts_node_type(Child);
									!ts_node_is_named(Child) && (
										NodeKind::StringDelimiter.Contains(Token) ||
										Token.size() > 1 && Token.back() == '"' && NodeKind::StringPrefix.Contains(Token.substr(0, Token.size() - 1))
									)
								) HasOpenString = OpensUnclosed(Src.Start(Child));
								// 閉じない文字列を見付ける迄次の子へ進む事の返戻
								return !HasOpenString;
							}
						);
					}
				}
				// 未対応構文でも出る `ERROR` と区別し，閉じ括弧等の不足を表す `MISSING` を記録
				if(ts_node_is_missing(Cur) && IsTreeBroken) HasInputMissing = true;
				if(IsTreeBroken) HasInputError = true;
				// 整形しない事が確定したら早期に打切の返戻（HasDescendantOf は true 返戻で打切となる）
				return HasInputMissing && HasInputError || HasOpenString || HasJsxError;
			}
		);
	}
	// 本文・字下げを保てない破損だけ見送り，解析器未対応の正当な構文は許容
	if(
		HasInputMissing || HasJsxError || HasOpenString || HasInputError && (
			Language.IsIndentSensitive() || Language == Lang::Ruby || Language == Lang::HTML || Language == Lang::JSON ||
			Language == Lang::CSS && (
				std::string_view(ts_node_type(Src.GetRoot())) == "ERROR" || HasDescendantOf(
					Src.GetRoot(),
					[&Src](const TSNode Node) -> bool {
						// CSS の `ERROR` 節点の関数呼出判定
						const std::string_view Type = ts_node_type(Node);
						if(Type == "ERROR") {
							// CSS 関数名の照合字面
							static constexpr std::string_view Url = "url(";
							// 関数名を値と誤読した url の直前３字からの探索
							const uint32_t From = Src.Start(Node) - std::min<uint32_t>(Src.Start(Node), 3);
							// url 誤読の探索範囲
							const std::string_view Text(Src.data() + From, Src.End(Node) - From);
							// 大小を問わず url 内のコメント誤読を検出した結果の返戻（コメント無なら逐語保持可能）
							return std::search(
								Text.begin(),
								Text.end(),
								Url.begin(),
								Url.end(),
								[](const char Actual, const char Expected) -> bool {
									// 英字の大小を揃えた字の一致の返戻
									return std::tolower(static_cast<unsigned char>(Actual)) == Expected;
								}
							) != Text.end() && HasDescendantOf(
								Node,
								[](const TSNode Inner) -> bool {
									// コメントかの返戻
									return NodeKind::Comment.Contains(Inner);
								}
							);
						}
						// 要素の足りない規則は誤読の形でない事の返戻
						if(Type != "rule_set" || ts_node_named_child_count(Node) < 3) return false;
						const std::string_view Selector = Src.View(ts_node_named_child(Node, 0));
						const TSNode Colon = ts_node_named_child(Node, 1);
						// `--x` だけの選択子・`:` の `ERROR`・値の波括弧が並ぶ規則（カスタムプロパティの誤読）かの返戻
						return Selector.starts_with("--") && Selector.find_first_of(" \t\n:") == std::string_view::npos &&
						std::string_view(ts_node_type(Colon)) == "ERROR" && Src.View(Colon) == ":";
					}
				)
			) || Language == Lang::Swift && HasDescendantOf(
				Src.GetRoot(),
				[&Src](const TSNode Node) -> bool {
					// 正規表現でない事の返戻
					if(std::string_view(ts_node_type(Node)) != "regex_literal") return false;
					const std::string_view Text = Src.View(Node);
					// 正規表現末尾の逃避記号数
					size_t Escapes = 0;
					for(size_t At = Text.size() - 1; At && Text[At - 1] == '\\'; --At) ++Escapes;
					// 終わりの `/` が逃がされた（前に奇数個の `\` が続く）かの返戻
					return Text.size() > 1 && Text.back() == '/' && Escapes & 1;
				}
			) || Language == Lang::Kotlin && HasDescendantOf(
				Src.GetRoot(),
				[&Src](const TSNode Node) -> bool {
					// Kotlin が回復時に落とした字句を子の間隙から検出し，else 脱落等による文の誤結合を防止
					uint32_t PreviousEnd = std::numeric_limits<uint32_t>::max();
					// 子の間に落とした字句が在るかの返戻
					return HasChildOf(
						Node,
						[&](const TSNode Child) -> bool {
							// 現在の子の開始と直前の終端
							const uint32_t Start = Src.Start(Child), Previous = std::exchange(PreviousEnd, Src.End(Child));
							// 前の子との間に落とした字句が在るかの返戻
							return Previous < Start &&
							std::string_view(Src.data() + Previous, Start - Previous).find_first_not_of(" \t\r\n;") != std::string_view::npos;
						}
					);
				}
			) || Language == Lang::PHP && (
				Src.find("<<<") != std::string::npos || HasChildOf(
					Src.GetRoot(),
					[](const TSNode Child) -> bool {
						// 地の文の子かの返戻
						return NodeKind::PhpInlineHtml.Contains(Child);
					}
				)
			)
		)
	) {
		SkipReason = "parser could not read the source";
		// 構造が信用出来ない為，整形未実施の返戻
		return false;
	}
	// 整形続行を呼出側に通知の返戻
	return true;
}
