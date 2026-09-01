#include "LineSplit.hpp"
#include "Edit.hpp"
#include "Structure.hpp"
#include "../Util/DeclEdit.hpp"
#include "../Util/NodeKind.hpp"
#include "../Util/Parallel.hpp"
#include "../Util/TextEdit.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// 行の可視文字数上限
size_t LineSplitPass::MaxChars = 128;

/** ========== 分割候補 ========== */
/**
 * 最大文字数の設定関数
 * @param Chars 行長上限値
 */
void LineSplitPass::SetMaxChars(const size_t Chars) {
	// 後続分割へ使う上限値
	MaxChars = Chars;
	// 終了
	return;
}

/**
 * Python の暗黙継続が可能な最外括弧範囲の収集関数
 * @param Src 解析済のソース
 * @return 開き字句の開始から閉じ字句の開始迄の非重複範囲
 */
std::vector<std::pair<uint32_t, uint32_t>> LineSplitPass::PythonContinuationRanges(const TSSource &Src) {
	// 最外括弧の継続範囲列
	std::vector<std::pair<uint32_t, uint32_t>> Ranges;
	uint32_t Open = 0, Depth = 0;
	WalkChildrenCursor(
		Src.GetRoot(),
		[&](const TSNode Node) -> bool {
			// 文字列内部を走査しない場合の返戻
			if(NodeKind::StringLikeInnerPreserve.Contains(Node)) return false;
			// 無名字句の括弧深さ更新
			if(
				const std::string_view Token = ts_node_is_named(Node) ? std::string_view{} : Src.View(Node);
				!ts_node_is_named(Node) && (Token == "(" || Token == "[" || Token == "{")
			) {
				// 最外の開き位置の捕捉
				if(!Depth++) Open = Src.Start(Node);
			} else if(!ts_node_is_named(Node) && (Token == ")" || Token == "]" || Token == "}") && Depth && !--Depth) {
				// 外側迄閉じた継続範囲の確定
				Ranges.emplace_back(Open, Src.Start(Node));
			}
			// 文字列以外の構文子を走査する事の返戻
			return true;
		}
	);
	// 構文順の非重複範囲の返戻
	return Ranges;
}

/**
 * Python の継続括弧内かの判定関数
 * @param Ranges 昇順の非重複括弧範囲
 * @param Pos 対象範囲の開始位置
 * @param End 対象範囲の終了位置
 * @return 開き字句の直後から閉じ字句の直前迄なら true
 */
bool LineSplitPass::IsInPythonContinuation(
	const std::vector<std::pair<uint32_t, uint32_t>> &Ranges,
	const uint32_t Pos,
	const uint32_t End
) {
	const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Iter = std::lower_bound(
		Ranges.begin(),
		Ranges.end(),
		Pos,
		[](const std::pair<uint32_t, uint32_t> &Range, const uint32_t Value) -> bool {
			return Range.second < Value;
		}
	);
	// 閉じ字句直前は含め，開き字句の直前は除く事の返戻
	return Iter != Ranges.end() && Iter->first < Pos && End <= Iter->second;
}

/**
 * 終端ノードかの判定関数
 * @param Type tree-sitter ノード型名
 * @return 終端なら true
 */
bool LineSplitPass::IsTerminal(const std::string_view Type) {
	// 型の無い節点は終端の返戻
	if(Type.empty()) return true;
	// 葉ノード又はコメントは末端扱いで子に降りない事の返戻
	return NodeKind::Leaf.Contains(Type) || NodeKind::Comment.Contains(Type);
}

/**
 * 圧縮対象ノードかの判定関数
 * @param Type tree-sitter ノード型名
 * @return 圧縮対象（内部で改行を挿入しない属性的ノード）なら true
 */
bool LineSplitPass::IsCompactNode(const std::string_view Type) {
	// jsx_attribute と property_signature は内部に分割可能な要素（jsx_expression/object_type 等）を含み得るので再帰対象
	if(Type.empty() || NodeKind::CompactRecurse.Contains(Type)) return false;
	// 属性ノードは内部で改行を挿入しない事の返戻
	return NodeKind::Compact.Contains(Type);
}

/**
 * 単一文字範囲かの判定関数
 * @param Source ソースコード
 * @param Start 範囲の開始バイト
 * @param End 範囲の終端バイト
 * @param Char 比較する文字
 * @return 範囲が１バイトで境界内に在り文字が一致すれば true
 */
bool LineSplitPass::IsSingleChar(const std::string &Source, const uint32_t Start, const uint32_t End, const char Char) {
	// 長さ１・境界内・文字一致の返戻
	return End == Start + 1 && Start < Source.size() && Source[Start] == Char;
}

/**
 * トークンの分割側の判定関数
 * @param Token 分類対象のトークン文字列
 * @param Language 対象言語
 * @param IsFirst 当該トークンが親ノードの最初の名前付子より前に有るか
 * @return `Tail`（行末側）／`Head`（行頭側）／`None`（分割対象外）
 */
LineSplitPass::BreakSide LineSplitPass::SplitSide(const std::string_view Token, const Lang Language, const bool IsFirst) {
	// 行頭へ置くトークン：閉じ括弧とメンバアクセスは，連鎖の起点を読み取れる様に次行の先頭へ置く
	static const NodeKind::NodeTypeSet LineHead = { ")", "]", "}", ".", "?." }, TailAnywhere = { ",", ";", "(", "[", "{", "=>" };
	// 先頭以外で行末に残す演算子
	static const NodeKind::NodeTypeSet TailAfterFirst = {
		"?",
		":",
		"=",
		"+",
		"-",
		"*",
		"/",
		"%",
		"^",
		"<",
		">",
		"|",
		"&",
		"==",
		"!=",
		"<=",
		">=",
		"===",
		"!==",
		"+=",
		"-=",
		"*=",
		"/=",
		"%=",
		"**=",
		"||=",
		"&&=",
		"?\?=",
		"||",
		"&&",
		"?\?",
		"**",
		"<<",
		">>",
		">>>"
	};
	// Python 固有の行末字句集合
	static const NodeKind::NodeTypeSet PythonTail = { "//", "@", "and", "or", "in", "is", "not in", "is not", "if", "else" };
	// Python の語演算子は行末に置く事の返戻
	if(Language == Lang::Python && !IsFirst && PythonTail.Contains(Token)) return BreakSide::Tail;
	// Ruby の節区切りと `end`
	static const NodeKind::NodeTypeSet RubyLineHead = { "elsif", "when", "else", "rescue", "ensure", "end" };
	static const NodeKind::NodeTypeSet GoLineHead = { "case", "default" };
	// Go は行末の識別子・`)`・`]` の後へ自動的に文区切りを補う為，`.` を行頭へ置くと直前行で文が閉じ構文が壊れる
	if(Language == Lang::Go && Token == ".") return BreakSide::Tail;
	if(Language == Lang::Ruby && RubyLineHead.Contains(Token) || Language == Lang::Go && GoLineHead.Contains(Token)) {
		// 行頭側での分割の返戻
		return BreakSide::Head;
	}
	// 行頭へ置く字句（行の先頭では分割しない）の返戻
	if(LineHead.Contains(Token)) return IsFirst ? BreakSide::None : BreakSide::Head;
	// 何処でも行末に置く字句の返戻
	if(TailAnywhere.Contains(Token)) return BreakSide::Tail;
	// 演算子の全言語での行末配置
	return !IsFirst && TailAfterFirst.Contains(Token) ? BreakSide::Tail : BreakSide::None;
}

/**
 * 分割位置の算出関数
 * @param Source ソースコード
 * @param ChildStart 対象の名無ノードの開始バイト
 * @param ChildEnd 対象の名無ノードの終端バイト
 * @param Language 入力言語
 * @param IsFirst 兄弟中で最初の非空白トークンなら true
 * @param OutPos 分割位置（出力）
 * @return 算出に成功したら true
 */
bool LineSplitPass::ComputeBreakPos(
	const std::string &Source,
	const uint32_t ChildStart,
	const uint32_t ChildEnd,
	const Lang Language,
	const bool IsFirst,
	uint32_t &OutPos
) {
	// 範囲外の字句は分割位置を持たない事の返戻
	if(ChildStart >= Source.size() || ChildEnd > Source.size()) return false;
	// 字句の抽出と前後空白除去
	std::string_view Token(Source.data() + ChildStart, ChildEnd - ChildStart);
	TextEdit::TrimView(Token);
	// 空の字句は分割位置を持たない事の返戻
	if(Token.empty()) return false;
	// 言語と兄弟位置に基付く分割側の決定
	const BreakSide Side = LineSplitPass::SplitSide(Token, Language, IsFirst);
	// 分割候補でない字句の返戻
	if(Side == BreakSide::None) return false;
	// 字句を前の行へ残す分割位置の探索
	if(Side == BreakSide::Tail) {
		if(const uint32_t BreakPos = TextEdit::SkipSpRight(Source, ChildEnd); BreakPos < Source.size() && Source[BreakPos] != '\n') {
			// 行末字句後の分割位置
			OutPos = BreakPos;
			// 行末へ置くトークン（カンマや開き括弧等）の直後で分割可能の返戻
			return true;
		}
	} else if(const uint32_t BreakPos = TextEdit::SkipSpLeft(Source, ChildStart); BreakPos && Source[BreakPos - 1] != '\n') {
		// 行頭字句前の分割位置
		OutPos = BreakPos;
		// 末尾系トークン（`)` `,` `=` 等）の直前で分割可能の返戻
		return true;
	}
	// 当該トークンと文脈では分割不可の返戻
	return false;
}

/**
 * 無名子ノードの分割位置収集関数
 * @param Source ソースコード
 * @param Language 入力言語
 * @param TypeView 親ノードの型名
 * @param IsTypeColonParent `:` 直後を分割候補として上書きする親（鍵値対等）なら true
 * @param ParentId 親ノードの開始バイト（グループ識別子）
 * @param Depth 構文木深さ
 * @param Begin 名無子情報範囲の先頭
 * @param End 名無子情報範囲の終端
 * @param Entries 分割候補リスト（追記先）
 */
void LineSplitPass::CollectUnnamedBreaks(
	const std::string &Source,
	const Lang Language,
	const std::string_view TypeView,
	const bool IsTypeColonParent,
	const uint32_t ParentId,
	const uint32_t Depth,
	const std::vector<UnnamedInfo>::const_iterator Begin,
	const std::vector<UnnamedInfo>::const_iterator End,
	std::vector<BreakEntry> &Entries
) {
	const bool IsOptionalTypeMember = NodeKind::OptionalTypeMember.Contains(TypeView);
	const bool IsSwiftOptionalCall = Language == Lang::Swift && NodeKind::SwiftPostfixCall.Contains(TypeView);
	const bool IsGoParenExcluded = Language == Lang::Go && !NodeKind::MultilineCommaContainer.Contains(TypeView);
	const bool IsUnsplittableParen = Language == Lang::PHP && NodeKind::PhpUnsplittable.Contains(TypeView) ||
	// 不可分な括弧構文か
	Language == Lang::Swift && TypeView == "import_declaration" || Language == Lang::Kotlin && TypeView == "super_expression";
	// Rust 宣言の型注釈と初期化子の組の保持
	const bool IsTypedBinding = NodeKind::TypedBinding.Contains(TypeView);
	const bool IsPipeParamList = NodeKind::PipeParamList.Contains(TypeView);
	const bool IsLambdaCapture = TypeView == "lambda_capture_specifier", IsNavigationSuffix = TypeView == "navigation_suffix";
	const bool IsSwiftHeader = Language == Lang::Swift && NodeKind::SwiftHeaderStatement.Contains(TypeView);
	const bool IsSwiftOptionalType = Language == Lang::Swift && TypeView == "optional_type";
	const bool IsSwiftInfix = Language == Lang::Swift && NodeKind::InfixOp.Contains(TypeView);
	// 同じ親に属する字句の候補収集
	for(std::vector<UnnamedInfo>::const_iterator Iter = Begin; Iter != End; ++Iter) {
		const UnnamedInfo &Info = *Iter;
		const uint32_t ChildStart = ts_node_start_byte(Info.Child), ChildEnd = ts_node_end_byte(Info.Child);
		const char SingleChar = ChildEnd == ChildStart + 1 && ChildStart < Source.size() ? Source[ChildStart] : '\0';
		// 型注釈・接頭辞・既定値等の不可分位置と JSX 本文境界の除外
		if(
			std::string_view("+-!~&*").find(SingleChar) != std::string_view::npos && Info.IsLast ||
			(IsOptionalTypeMember || IsSwiftOptionalCall) && SingleChar == '?' || IsSwiftOptionalType ||
			IsSwiftInfix && ChildStart && (Source[ChildStart] == '?' || Source[ChildStart] == '!') &&
			!std::isspace(static_cast<unsigned char>(Source[ChildStart - 1])) || SingleChar == '=' &&
			(NodeKind::StripEq.Contains(TypeView) || IsSwiftHeader || NodeKind::KotlinParameterHost.Contains(TypeView)) ||
			IsPipeParamList && SingleChar == '|' || IsLambdaCapture && (SingleChar == '&' || SingleChar == '=' || SingleChar == '*') ||
			Language == Lang::Ruby && SingleChar == ')' && !ts_node_is_null(Info.Prev) &&
			NodeKind::RubyAnonymousForward.Contains(Info.Prev) && !ts_node_named_child_count(Info.Prev) ||
			SingleChar == '>' && Language.IsJsTs() && TypeView == "jsx_opening_element" &&
			TSSource::HasDisputedJsxText(Source, ts_node_parent(Info.Child))
		) continue;
		// TS の比較式を型引数へ誤読させる `>` 後の分割抑止
		if(SingleChar == '>' && Language == Lang::TypeScript && TypeView == "binary_expression" && !ts_node_is_null(Info.Prev)) {
			if(
				const TSNode Less = TSSource::FieldChild(Info.Prev, "operator");
				!ts_node_is_null(Less) && IsSingleChar(Source, ts_node_start_byte(Less), ts_node_end_byte(Less), '<')
				// 比較演算子の誤読回避
			) continue;
		}
		if(SingleChar == ':') {
			// アクセス修飾子／case/default／ラベル等の後続 `:` は分割対象外（節見出は１行で出力）
			if(IsTypedBinding || !ts_node_is_null(Info.Prev) && NodeKind::SectionHeader.Contains(Info.Prev)) continue;
			// PHP の代替構文の空の本体の `:` の後は，閉じの語を同じ行に置く為に割らない
			if(Language == Lang::PHP) {
				if(const TSNode Next = ts_node_next_sibling(Info.Child); !ts_node_is_null(Next) && NodeKind::PhpAltCloser.Contains(Next)) {
					// 空本体と閉じ語の同行保持
					continue;
				}
			}
			// 鍵値対・型注釈のコロン直後の分割候補化
			if(IsTypeColonParent) {
				if(const uint32_t BreakPos = TextEdit::SkipSpRight(Source, ChildEnd); BreakPos < Source.size() && Source[BreakPos] != '\n') {
					// コロン後の候補追加
					Entries.push_back({ BreakPos, ParentId, Depth });
				}
				// 通常の字句側判定の省略
				continue;
			}
		}
		// 浮動小数点リテラルの小数点は分割不可
		const bool IsInsideNumber =
		SingleChar == '.' && ChildStart && ChildEnd < Source.size() && Source[ChildStart - 1] >= '0' && Source[ChildStart - 1] <= '9' &&
		Source[ChildEnd] >= '0' && Source[ChildEnd] <= '9';
		const char Closer = SingleChar == '(' ? ')' : SingleChar == '[' ? ']' : SingleChar == '{' ? '}' : '\0';
		const char Opener = SingleChar == ')' ? '(' : SingleChar == ']' ? '[' : SingleChar == '}' ? '{' : '\0';
		const uint32_t Before = Opener ? TextEdit::SkipSpLeft(Source, ChildStart) : 0;
		if(
			const bool IsEmptyPair =
			Closer && Source[TextEdit::SkipSpRight(Source, ChildEnd)] == Closer || Before && Source[Before - 1] == Opener;
			IsInsideNumber || IsEmptyPair ||
			IsGoParenExcluded && (SingleChar == '[' || SingleChar == ']' || SingleChar == '(' || SingleChar == ')') || IsUnsplittableParen
			// 不可分な字句の除外
		) continue;
		// 除外条件を通過した字句の分割候補化
		if(uint32_t Pos; ComputeBreakPos(Source, ChildStart, ChildEnd, Language, Info.IsFirst && !IsNavigationSuffix, Pos)) {
			// 算出済候補の追加
			Entries.push_back({ Pos, ParentId, Depth });
		}
	}
	// 終了
	return;
}

/**
 * JSX タグの分割位置収集関数
 * @param Source ソースコード
 * @param Node 対象の JSX タグノード
 * @param ParentId 親ノードの開始バイト（グループ識別子）
 * @param Depth 構文木深さ
 * @param Entries 分割候補リスト（追記先）
 */
void LineSplitPass::CollectJsxTagBreaks(
	const std::string &Source,
	const TSNode Node,
	const uint32_t ParentId,
	const uint32_t Depth,
	std::vector<BreakEntry> &Entries
) {
	// 最初の名前付子（タグ名）を線形に通過してから，残り全子の直前を分割候補化
	bool IsTagNamePassed = false, HasAttribute = false;
	// JSX タグの全子走査
	ForEachChild(
		Node,
		[&](const TSNode Child) -> void {
			// タグ名と属性の境界の特定
			if(!IsTagNamePassed) {
				// タグ名通過の記録
				if(ts_node_is_named(Child)) IsTagNamePassed = true;
				// 終了
				return;
			}
			const uint32_t ChildStart = ts_node_start_byte(Child);
			const std::string_view View(Source.data() + ChildStart, ts_node_end_byte(Child) - ChildStart);
			const bool IsNamed = ts_node_is_named(Child);
			// 属性の後だけ閉じ字句を分割候補にする印付
			if(IsNamed) HasAttribute = true;
			// 条件成立時の返戻
			if(!ChildStart || !IsNamed && (!HasAttribute || View != ">" && View != "/>")) return;
			if(const uint32_t BreakPos = TextEdit::SkipSpLeft(Source, ChildStart); BreakPos && Source[BreakPos - 1] != '\n') {
				// 属性又は閉じ字句前の候補追加
				Entries.push_back({ BreakPos, ParentId, Depth });
			}
		}
	);
	// 終了
	return;
}

/**
 * 分割位置収集関数
 * @param Source ソースコード
 * @param Node 走査対象のノード
 * @param Language 入力言語
 * @param Depth 構文木深さ
 * @param Entries 分割候補リスト（追記先）
 * @param UnnamedPool 名無子情報の作業用領域
 */
void LineSplitPass::CollectBreaks(
	const std::string &Source,
	const TSNode Node,
	const Lang Language,
	const uint32_t Depth,
	std::vector<BreakEntry> &Entries,
	std::vector<UnnamedInfo> &UnnamedPool
) {
	// 深いネストは走脈領域を使い切る前に，新しい走脈で続きの分割候補を集める
	const RecursionGuard Guard;
	if(Guard.IsOverflow) {
		Parallel::RunOnFreshStack(
			[&]() -> void {
				// 新しい走脈での収集継続
				CollectBreaks(Source, Node, Language, Depth, Entries, UnnamedPool);
			}
		);
		// 新しい走脈での収集完了
		return;
	}
	const std::string_view TypeView(ts_node_type(Node));
	// 物理行必須又は意味保存対象の節点内部の分割抑止
	if(
		IsTerminal(TypeView) || TypeView == "ERROR" || NodeKind::PreprocFlat.Contains(TypeView) ||
		NodeKind::BreakOpaque.Contains(TypeView) ||
		(Language == Lang::Swift || Language == Lang::Java) && TypeView == "import_declaration"
		// 条件成立時の返戻
	) return;
	// 前処理の条件指令は行末改行が指令の終端で継続行 `\` 無には折り返せない為，条件式を分割候補から外す
	if(NodeKind::PreprocBlock.Contains(TypeView)) {
		// 分割禁止の条件式
		const TSNode Condition = TSSource::FieldChild(Node, "condition");
		ForEachNamedChild(
			Node,
			[&](const TSNode Child) -> void {
				// 前処理条件式の原文１行への保持
				if(!ts_node_is_null(Condition) && ts_node_eq(Condition, Child)) return;
				// 本体側の候補収集
				CollectBreaks(Source, Child, Language, Depth + 1, Entries, UnnamedPool);
			}
		);
		// 条件式以外の子のみを走査しての終了
		return;
	}
	const uint32_t ChildCount = ts_node_child_count(Node);
	// 条件成立時の返戻
	if(!ChildCount) return;
	// 属性的節点の名無子除外と名前付子の再帰
	if(IsCompactNode(TypeView)) {
		ForEachNamedChild(
			Node,
			[&](const TSNode Child) -> void {
				// 複合式内部の候補収集
				CollectBreaks(Source, Child, Language, Depth + 1, Entries, UnnamedPool);
			}
		);
		// 終了
		return;
	}
	if(ChildCount == 1) {
		if(const TSNode Only = ts_node_child(Node, 0); ts_node_is_named(Only)) {
			// 同一深度での単一子走査
			CollectBreaks(Source, Only, Language, Depth, Entries, UnnamedPool);
		}
		// 終了
		return;
	}
	// jsx_attribute の `=` は分割対象外（属性は原子扱い）但し内部の jsx_expression は再帰で処理済
	const bool IsJsxAttribute = TypeView == "jsx_attribute", IsJsxTag = NodeKind::JsxTag.Contains(TypeView);
	const size_t PoolBase = UnnamedPool.size();
	bool HasSeenNamed = false, HasErrorChild = false;
	TSNode PrevChild {};
	const bool IsInitHeader = NodeKind::InitHeader.Contains(TypeView);
	// 現在節点の全子走査
	ForEachChild(
		Node,
		[&](const TSNode Child) -> void {
			if(ts_node_is_named(Child)) {
				// `ERROR` 状態の記録と名前付子内部の候補収集
				if(ts_node_has_error(Child)) HasErrorChild = true;
				const size_t EntriesBefore = Entries.size();
				CollectBreaks(Source, Child, Language, Depth + 1, Entries, UnnamedPool);
				HasSeenNamed = true;
				if(IsInitHeader) {
					// 子文の最終字句の見出候補群への移動
					TSNode Last = Child;
					for(uint32_t Count = ts_node_child_count(Last); Count; Count = ts_node_child_count(Last)) Last = ts_node_child(Last, Count - 1);
					if(!ts_node_eq(Last, Child) && std::string_view(ts_node_type(Last)) == ";") {
						const uint32_t BreakPos = TextEdit::SkipSpRight(Source, ts_node_end_byte(Last));
						// 子文群からの終端候補除去
						Entries.erase(
							std::remove_if(
								Entries.begin() + static_cast<std::ptrdiff_t>(EntriesBefore),
								Entries.end(),
								[BreakPos](const BreakEntry &Entry) -> bool {
									// 子の文の終わりの `;` の後の候補かの返戻
									return Entry.Pos == BreakPos;
								}
							),
							// 除去範囲の終端
							Entries.end()
						);
						// 見出群への終端字句移動
						UnnamedPool.push_back({ Last, false, Child });
					}
				}
				// 名無子情報の一時保存
			} else UnnamedPool.push_back({ Child, !HasSeenNamed, PrevChild });
			// 次の子用の直前節点
			PrevChild = Child;
		}
	);
	// 末尾字句の印付
	if(!ts_node_is_null(PrevChild) && !ts_node_is_named(PrevChild)) UnnamedPool.back().IsLast = true;
	const uint32_t ParentId = ts_node_start_byte(Node);
	// 区切りの無い隣接リテラルの名前付子前の候補化
	if(NodeKind::StringSeqContainer.Contains(TypeView)) {
		// 先頭文字列の除外状態
		bool IsFirst = true;
		// 隣接文字列の順次走査
		ForEachNamedChild(
			Node,
			[&](const TSNode Child) -> void {
				if(IsFirst && !NodeKind::RubyArrayLiteral.Contains(TypeView)) {
					// 先頭子通過の記録
					IsFirst = false;
					// 終了
					return;
				}
				// 後続子の記録
				IsFirst = false;
				if(
					const uint32_t ChildStart = ts_node_start_byte(Child), BreakPos = TextEdit::SkipSpLeft(Source, ChildStart);
					BreakPos && Source[BreakPos - 1] != '\n'
					// 後続文字列前の候補追加
				) Entries.push_back({ BreakPos, ParentId, Depth });
			}
		);
	}
	// 鍵値対のコロン後に在る値の前の候補化
	bool IsTypeColonParent = NodeKind::TypeColonParent.Contains(TypeView);
	// type_annotation は親が宣言又は仮引数系で，型が単純な場合のみ対象
	if(!IsTypeColonParent && TypeView == "type_annotation") {
		// 型注釈の親存在条件
		if(const TSNode Parent = ts_node_parent(Node); !ts_node_is_null(Parent)) {
			const std::string_view ParentType(ts_node_type(Parent));
			if(NodeKind::TypeAnnotationParent.Contains(ParentType)) {
				// 省略可能属性の有無
				bool IsOptionalProp = false;
				if(ParentType == "property_signature") {
					// 省略可能印の探索
					ForEachChild(
						Parent,
						[&](const TSNode ParentChild) -> bool {
							// 名前付の子を読み飛ばす事の返戻
							if(ts_node_is_named(ParentChild)) return true;
							// 無名字句の疑問符判定
							if(IsSingleChar(Source, ts_node_start_byte(ParentChild), ts_node_end_byte(ParentChild), '?')) {
								// 省略可能属性の検出
								IsOptionalProp = true;
								// 内側に属性／ラムダ等のネスト分割対象が存在する事の返戻
								return false;
							}
							// 内側に分割対象が無い場合は親階層での折返対象の返戻
							return true;
						}
					);
				}
				// 単純型（識別子／組込型／配列）は `:` 後の分割を許可し，複合型（合併型／交差型／関数型等）は内側分割を優先
				if(
					IsOptionalProp ||
					ts_node_named_child_count(Node) == 1 && NodeKind::TypeAnnotationSimpleType.Contains(ts_node_named_child(Node, 0))
					// コロン後分割の有効化
				) IsTypeColonParent = true;
			}
		}
	}
	// 分割対象外：`jsx_attribute`／Ruby の括弧無引数列／Ruby の `exception_variable`／`ERROR` 子を持つノード
	if(!IsJsxAttribute && !HasErrorChild && !(Language == Lang::Ruby && TypeView == "exception_variable")) {
		// 現在親の名無子候補収集
		CollectUnnamedBreaks(
			Source,
			Language,
			TypeView,
			IsTypeColonParent,
			ParentId,
			Depth,
			UnnamedPool.begin() + PoolBase,
			UnnamedPool.end(),
			Entries
		);
	}
	// JSX の開きタグ／自己閉じタグ：タグ名以降の各名前付子の直前を分割候補化
	if(IsJsxTag && !HasErrorChild) CollectJsxTagBreaks(Source, Node, ParentId, Depth, Entries);
	// UnnamedPool の本関数分を末尾から取り除く（積重状の再利用）確保済容量は維持される為，呼出元の連続再帰で確保解放を平均化
	UnnamedPool.resize(PoolBase);
	// 終了
	return;
}

/**
 * 分割位置の並列収集関数
 * @param Source ソースコード
 * @param Root 走査対象のルートノード
 * @param Language 入力言語
 * @param Entries 分割候補リスト（結果格納先）
 */
void LineSplitPass::CollectBreaksParallel(
	const std::string &Source,
	const TSNode Root,
	const Lang Language,
	std::vector<BreakEntry> &Entries
) {
	// ルート自身の名無子処理は program 型の様に通常は空の為，省略，単一子の連鎖や子数が少ない場合は逐次に委ねる
	Entries.clear();
	TSNode Top = Root;
	uint32_t ChildCount = 0;
	while(true) {
		// 条件成立時の返戻
		if(const std::string_view TypeView(ts_node_type(Top)); IsTerminal(TypeView) || IsCompactNode(TypeView)) return;
		// 現在候補の子数更新
		ChildCount = ts_node_child_count(Top);
		if(ChildCount != 1) break;
		// 唯一の子
		const TSNode Only = ts_node_child(Top, 0);
		// 条件成立時の返戻
		if(!ts_node_is_named(Only)) return;
		// 単一名前付子への降下
		Top = Only;
	}
	// 条件成立時の返戻
	if(!ChildCount) return;
	std::vector<UnnamedInfo> UnnamedPool;
	// 末端候補からの逐次収集
	CollectBreaks(Source, Top, Language, 0, Entries, UnnamedPool);
	// 終了
	return;
}

/**
 * 分割位置の貪欲統合関数
 * @param Source ソースコード
 * @param Breaks 分割位置集合（結果格納先）
 * @param LockedBreaks 除去対象外の分割位置
 * @param Entries 収集済の分割候補
 * @param Newlines Source 内の `\n` 位置（昇順）
 * @param JoinBraceEnds 前の行の `}` へ続ける行の直前の位置（昇順）
 */
void LineSplitPass::GreedyMerge(
	const std::string &Source,
	std::vector<uint32_t> &Breaks,
	const std::vector<uint32_t> &LockedBreaks,
	const std::vector<BreakEntry> &Entries,
	const std::vector<uint32_t> &Newlines,
	const std::vector<uint32_t> &JoinBraceEnds
) {
	// 可変・固定候補の昇順一意列の構築
	Breaks.clear();
	Breaks.reserve(Entries.size() + LockedBreaks.size());
	for(const BreakEntry &Entry : Entries) Breaks.push_back(Entry.Pos);
	for(const uint32_t Pos : LockedBreaks) Breaks.push_back(Pos);
	std::sort(Breaks.begin(), Breaks.end());
	Breaks.erase(std::unique(Breaks.begin(), Breaks.end()), Breaks.end());
	std::vector<uint8_t> BreakActive(Breaks.size(), 1);
	const auto LineCharsActive = [&Source, &Breaks, &BreakActive, &Newlines, &JoinBraceEnds](const uint32_t Pos) -> size_t {
		// 幅を測る論理行の始端の探索
		uint32_t Start = 0;
		if(Pos) {
			if(
				const std::vector<uint32_t>::const_iterator NlIter = std::upper_bound(Newlines.begin(), Newlines.end(), Pos - 1);
				NlIter != Newlines.begin()
				// 直前物理改行後の位置
			) Start = *std::prev(NlIter) + 1;
		}
		// 直前と次の有効境界の探索
		const std::vector<uint32_t>::const_iterator BrIter = std::upper_bound(Breaks.begin(), Breaks.end(), Pos);
		for(std::vector<uint32_t>::const_iterator PrevIter = BrIter; PrevIter != Breaks.begin() && *std::prev(PrevIter) > Start;) {
			if(BreakActive[--PrevIter - Breaks.begin()]) {
				// 直前有効分割の位置
				Start = *PrevIter;
				break;
			}
		}
		// 幅を測る論理行の終端の探索
		uint32_t End = static_cast<uint32_t>(Source.size());
		if(
			const std::vector<uint32_t>::const_iterator NlIter2 = std::upper_bound(Newlines.begin(), Newlines.end(), Pos);
			NlIter2 != Newlines.end() && *NlIter2 < End
			// 次の物理改行位置
		) End = *NlIter2;
		for(std::vector<uint32_t>::const_iterator NextIter = BrIter; NextIter != Breaks.end() && *NextIter < End; ++NextIter) {
			if(BreakActive[NextIter - Breaks.begin()]) {
				// 次の有効分割位置
				End = *NextIter;
				break;
			}
		}
		// 字下げを除いた本文幅の測定準備
		while(Start < End && (Source[Start] == ' ' || Source[Start] == '\t')) ++Start;
		// 行末空白の除外
		End = TextEdit::SkipCharsLeftBounded(Source, End, Start, " \t\n");
		// 空白だけの範囲の長さの返戻
		if(Start >= End) return 0;
		const std::string_view Line(Source.data() + Start, End - Start);
		const uint32_t Before = TextEdit::SkipCharsLeftBounded(Source, Start, 0, " \t\n");
		const size_t Joined = std::binary_search(JoinBraceEnds.begin(), JoinBraceEnds.end(), Before) ? 2 : 0;
		// Pos を含む論理行範囲の可視文字数を計算の返戻
		return TextEdit::ContentChars(Line) + Joined;
	};
	// Entries は後行順走査の追加順で保持される為，追加整列不要
	std::vector<size_t> GroupIdxs;
	size_t GroupStart = 0;
	while(GroupStart < Entries.size()) {
		const uint32_t Depth = Entries[GroupStart].Depth, ParentId = Entries[GroupStart].ParentId;
		size_t GroupEnd = GroupStart;
		while(GroupEnd < Entries.size() && Entries[GroupEnd].Depth == Depth && Entries[GroupEnd].ParentId == ParentId) ++GroupEnd;
		// 現在の親群に属する可変分割の収集
		GroupIdxs.clear();
		for(size_t GroupIdx = GroupStart; GroupIdx < GroupEnd; ++GroupIdx) {
			// 候補の分割位置
			const uint32_t Pos = Entries[GroupIdx].Pos;
			// 固定分割の除外
			if(!LockedBreaks.empty() && std::binary_search(LockedBreaks.begin(), LockedBreaks.end(), Pos)) continue;
			if(
				const std::vector<uint32_t>::const_iterator Iter = std::lower_bound(Breaks.begin(), Breaks.end(), Pos);
				Iter != Breaks.end() && *Iter == Pos
				// 可変分割番号の追加
			) GroupIdxs.push_back(static_cast<size_t>(Iter - Breaks.begin()));
		}
		// 可変分割を持たない群の除外
		if(GroupIdxs.empty()) {
			// 次の親群への前進
			GroupStart = GroupEnd;
			continue;
		}
		// グループ位置へ無効の印を付ける
		for(const size_t Idx : GroupIdxs) BreakActive[Idx] = 0;
		bool HasInner = false;
		// 群の両端に挟まれた内部分割の検査
		if(GroupIdxs.size() > 1) {
			// 群の両端探索
			const auto [MinIter, MaxIter] = std::minmax_element(GroupIdxs.begin(), GroupIdxs.end());
			const size_t MinIdx = *MinIter, MaxIdx = *MaxIter;
			for(size_t Idx = MinIdx + 1; Idx < MaxIdx; ++Idx) if(BreakActive[Idx]) {
				// 有効内部分割の検出
				HasInner = true;
				break;
			}
			// 候補分割に無い原文改行の確認
			if(!HasInner) {
				const std::vector<uint32_t>::const_iterator NlIter = std::upper_bound(Newlines.begin(), Newlines.end(), Breaks[MinIdx]);
				// 原文改行の検出
				HasInner = NlIter != Newlines.end() && *NlIter < Breaks[MaxIdx];
			}
		}
		// 内部分割又は幅超過が有る親群の展開維持
		if(HasInner || LineCharsActive(Breaks[GroupIdxs.front()]) > MaxChars) for(const size_t Idx : GroupIdxs) BreakActive[Idx] = 1;
		// 次の親群への前進
		GroupStart = GroupEnd;
	}
	size_t Write = 0;
	// 有効位置の前詰
	for(size_t Read = 0; Read < Breaks.size(); ++Read) if(BreakActive[Read]) Breaks[Write++] = Breaks[Read];
	// 無効位置の末尾除去
	Breaks.resize(Write);
	// 終了
	return;
}

/** ========== 言語別分割 ========== */
/**
 * Python の最も外の型注釈の列挙関数
 * 括弧の付与の前後で同じ順番に為る（括弧は型注釈の節点を増減させない）為，付与した注釈を順番で識別する
 * @param Src 解析済のソース
 * @return 先行順の型注釈（型注釈の中の型は含めない）
 */
std::vector<TSNode> LineSplitPass::OuterPythonTypes(const TSSource &Src) {
	// 最外型注釈の原文順列
	std::vector<TSNode> Types;
	WalkChildrenCursor(
		Src.GetRoot(),
		[&Types](const TSNode Node) -> bool {
			const std::string_view Type = NamedTypeOf(Node);
			// 最外型注釈の追加
			if(Type == "type") Types.push_back(Node);
			// 型注釈と文字列の中へ降りない事の返戻
			return Type != "type" && !NodeKind::StringLikeInnerPreserve.Contains(Type);
		}
	);
	// 型注釈の列の返戻
	return Types;
}

/**
 * Python の幅超過式への継続括弧付与関数
 * @param Src 解析済のソース
 * @return 注釈の始まりで割る為に全体を包んだ型注釈の順番（OuterPythonTypes の順）
 */
std::vector<uint32_t> LineSplitPass::WrapPythonExpressions(TSSource &Src) {
	// 既存の継続括弧範囲
	const std::vector<std::pair<uint32_t, uint32_t>> Ranges = PythonContinuationRanges(Src);
	std::vector<TSNode> Values;
	std::unordered_set<const void *> ValueRoots;
	const auto AddValueRoots = [&](const TSNode Node) -> void {
		// 文直下の子走査
		ForEachNamedChild(
			Node,
			[&](const TSNode Child) -> void {
				// 値式候補の登録
				if(NodeKind::PythonValueRoot.Contains(ts_node_type(Child))) ValueRoots.insert(Child.id);
			}
		);
	};
	const auto AddValue = [&Values](const TSNode Value) -> void {
		// 非空候補の追加
		if(!ts_node_is_null(Value)) Values.push_back(Value);
	};
	// ファイル直下の値式の収集開始
	AddValueRoots(Src.GetRoot());
	// Python 構文木の走査
	WalkChildrenCursor(
		Src.GetRoot(),
		[&](const TSNode Node) -> bool {
			const std::string_view Type = ts_node_type(Node);
			// 型別名文へ誤読された `type(x).attr = v` 内部の除外
			if(
				NodeKind::StringLikeInnerPreserve.Contains(Type) ||
				Type == "type_alias_statement" && Src[Src.Start(ts_node_named_child(Node, 0))] == '('
				// 内容保持節点又は誤読した文の内側へ降りない事の返戻
			) return false;
			if(ts_node_is_named(Node) && Type == "type") {
				// 型注釈の候補追加
				AddValue(Node);
				// 型内部を重複走査しない事の返戻
				return false;
			}
			// 本体直下候補の登録
			if(Type == "block") AddValueRoots(Node);
			// 登録済値式の確定
			if(ValueRoots.erase(Node.id)) AddValue(Node);
			// 別名指定の内側に在る値式の抽出
			if(NodeKind::PythonAsClause.Contains(Type)) {
				// 別名指定の値式
				TSNode Value = TSSource::FieldChild(Node, "value");
				if(!ts_node_is_null(Value) && std::string_view(ts_node_type(Value)) == "as_pattern") Value = ts_node_named_child(Value, 0);
				// 別名指定の値式追加
				AddValue(Value);
			}
			if(NodeKind::PythonRightValueHost.Contains(Type)) {
				// 代入の左辺と for の対象の括弧の無い組 (`a, b = f()` / `for a, b in c:`) も，括弧で包めば要素の間で折り返せる
				if(
					const TSNode Left = TSSource::FieldChild(Node, "left");
					!ts_node_is_null(Left) && std::string_view(ts_node_type(Left)) == "pattern_list"
					// 組左辺の候補追加
				) AddValue(Left);
				// 型注釈の先行処理と連鎖代入以外の右辺の遅延包装
				if(
					const TSNode Right = TSSource::FieldChild(Node, "right");
					!ts_node_is_null(Right) && std::string_view(ts_node_type(Right)) != "assignment"
					// 右辺節点の候補登録
				) ValueRoots.insert(Right.id);
			} else if(NodeKind::PythonConditionHost.Contains(Type)) AddValue(TSSource::FieldChild(Node, "condition"));
			else if(NodeKind::PythonValueStatement.Contains(Type)) {
				// 値文の名前付子走査
				ForEachNamedChild(
					Node,
					[&](const TSNode Child) -> void {
						// 包装可能子の追加
						if(!NodeKind::PythonUnwrappedChild.Contains(ts_node_type(Child))) AddValue(Child);
					}
				);
			}
			// 文中の独立した値式を探索する事の返戻
			return true;
		}
	);
	// 包めば合法に為る分割位置の区間を集める
	std::vector<std::pair<uint32_t, uint32_t>> Legal;
	std::unordered_set<const void *> Callees;
	for(const TSNode Value : Values) {
		const auto Visit = [&](const TSNode Node) -> bool {
			// 探索節点の型と開始位置の取得
			const std::string_view Type = ts_node_type(Node);
			const uint32_t Start = Src.Start(Node);
			if(
				IsInPythonContinuation(Ranges, Start, Start) || NodeKind::StringLikeInnerPreserve.Contains(Type) ||
				NodeKind::PythonWrapOpaque.Contains(Type)
				// 既存の括弧の中と不可分の範囲へ降りない事の返戻
			) return false;
			// 呼出先を単独で包まない為の節点記録
			if(Type == "call") Callees.insert(ts_node_child(Node, 0).id);
			// 包装可能区間の追加
			if(NodeKind::PythonWrappable.Contains(Type) && !Callees.contains(Node.id)) Legal.emplace_back(Start + 1, Src.End(Node));
			// 値式の子を走査する事の返戻
			return true;
		};
		if(NamedTypeOf(Value) == "type") {
			// 型注釈開始の分割を合法化する全体包装
			const uint32_t Start = Src.Start(Value);
			// 型注釈開始の合法区間
			Legal.emplace_back(Start, Start + 1);
			ForEachNamedChild(
				Value,
				[&](const TSNode Child) -> void {
					// 型内部の包装可能区間探索
					if(Visit(Child)) WalkChildrenCursor(Child, Visit);
				}
			);
			// 値式内部の包装可能区間探索
		} else if(Visit(Value)) WalkChildrenCursor(Value, Visit);
	}
	std::sort(Legal.begin(), Legal.end());
	size_t Merged = 0;
	for(const std::pair<uint32_t, uint32_t> &Range : Legal) {
		if(Merged && Range.first <= Legal[Merged - 1].second) {
			// 重複区間の終端拡張
			Legal[Merged - 1].second = std::max(Legal[Merged - 1].second, Range.second);
			// 非重複区間の確定
		} else Legal[Merged++] = Range;
	}
	// 統合済の合法区間だけの確定
	Legal.resize(Merged);
	// 包装後も不正な括弧外候補の除外
	std::vector<BreakEntry> Entries;
	// 全構文候補の収集
	CollectBreaksParallel(Src, Src.GetRoot(), Lang::Get(Lang::Python), Entries);
	std::erase_if(
		Entries,
		[&](const BreakEntry &Entry) -> bool {
			// 既存の括弧の中の候補を残す事の返戻
			if(IsInPythonContinuation(Ranges, Entry.Pos, Entry.Pos)) return false;
			// 候補を含む合法区間の検索
			const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Next =
			std::upper_bound(Legal.begin(), Legal.end(), std::make_pair(Entry.Pos, UINT32_MAX));
			// 包める値式の内側でない候補かの返戻
			return Next == Legal.begin() || Entry.Pos >= std::prev(Next)->second;
		}
	);
	std::vector<uint32_t> Breaks, Newlines;
	for(size_t Pos = Src.find('\n'); Pos != std::string::npos; Pos = Src.find('\n', Pos + 1)) {
		// 原文改行位置の追加
		Newlines.push_back(static_cast<uint32_t>(Pos));
	}
	// 幅に収まる候補の削減
	GreedyMerge(Src, Breaks, {}, Entries, Newlines, {});
	// 追加の継続括弧を要する分割位置の抽出
	std::vector<uint32_t> WrapPoints;
	// 括弧外分割の抽出
	for(const uint32_t Pos : Breaks) if(!IsInPythonContinuation(Ranges, Pos, Pos)) WrapPoints.push_back(Pos);
	// 既存括弧内の分割だけで足りる場合の返戻
	if(WrapPoints.empty()) return {};
	std::vector<TextEdit> Edits;
	std::vector<TSNode> Types;
	std::vector<uint32_t> Wrapped;
	uint32_t CoveredEnd = 0;
	const auto WrapValue = [&](const TSNode Value) -> void {
		const auto Visit = [&](const TSNode Node) -> bool {
			// 現在節点の型名
			const std::string_view Type = ts_node_type(Node);
			const uint32_t Start = Src.Start(Node), End = Src.End(Node);
			// 包装済又は不可分の範囲へ降りない事の返戻
			if(
				Start < CoveredEnd || IsInPythonContinuation(Ranges, Start, Start) || NodeKind::StringLikeInnerPreserve.Contains(Type) ||
				NodeKind::PythonWrapOpaque.Contains(Type)
				// 包装対象外の返戻
			) return false;
			// 包装候補区間との照合
			if(NodeKind::PythonWrappable.Contains(Type) && !Callees.contains(Node.id)) {
				if(
					const std::vector<uint32_t>::const_iterator Point = std::upper_bound(WrapPoints.begin(), WrapPoints.end(), Start);
					Point != WrapPoints.end() && *Point < End
				) {
					TextEdit::Push(Start, Start, "(", Edits);
					TextEdit::Push(End, End, ")", Edits);
					// 内側候補の重複包装抑止
					CoveredEnd = End;
					// 包装対象内へ重複して降りない事の返戻
					return false;
				}
			}
			// 値式の子を走査する事の返戻
			return true;
		};
		// 値式内部の包装対象探索
		if(Visit(Value)) WalkChildrenCursor(Value, Visit);
	};
	for(const TSNode Value : Values) {
		if(NamedTypeOf(Value) != "type") {
			// 通常値式の包装予約
			WrapValue(Value);
			// 型注釈用経路の省略
			continue;
		}
		// 括弧外の型注釈の式包装と合法な演算子位置での折返
		if(
			const uint32_t Start = Src.Start(Value);
			Start >= CoveredEnd && std::binary_search(WrapPoints.begin(), WrapPoints.end(), Start) &&
			NamedTypeOf(ts_node_named_child(Value, 0)) != "parenthesized_expression"
		) {
			// 型注釈の終了位置
			const uint32_t End = Src.End(Value);
			TextEdit::Push(Start, Start, "(", Edits);
			TextEdit::Push(End, End, ")", Edits);
			// 内側候補の重複包装抑止
			CoveredEnd = End;
			// 順番対応用の注釈列取得
			if(Types.empty()) Types = OuterPythonTypes(Src);
			// 型注釈の開始位置による先行順番号の探索
			Wrapped.push_back(
				static_cast<uint32_t>(
					std::lower_bound(
						Types.begin(),
						Types.end(),
						Start,
						[](const TSNode Type, const uint32_t At) -> bool {
							// 開始位置の前後の比較の返戻
							return ts_node_start_byte(Type) < At;
						}
					) - Types.begin()
				)
			);
			continue;
		}
		ForEachNamedChild(
			Value,
			[&](const TSNode Child) -> void {
				// 型注釈内の値式包装予約
				WrapValue(Child);
			}
		);
	}
	// 継続括弧の付与と内側空白の再正規化
	if(!Edits.empty()) {
		TextEdit::Apply(Src, Edits);
		// 括弧内空白の正規化
		StructurePass::NormalizeInlineSpaces(Src, Lang::Get(Lang::Python));
	}
	// 全体を包んだ型注釈の順番の返戻
	return Wrapped;
}

/**
 * Python の合法な分割位置の収集関数
 * @param Src 解析済のソース
 * @param Breaks 幅超過時に採用する分割位置
 * @param Newlines 既存の改行位置
 */
void LineSplitPass::CollectPythonBreaks(const TSSource &Src, std::vector<uint32_t> &Breaks, std::vector<uint32_t> &Newlines) {
	const std::vector<std::pair<uint32_t, uint32_t>> Ranges = PythonContinuationRanges(Src);
	std::vector<BreakEntry> Entries;
	// 括弧内候補の収集
	CollectBreaksParallel(Src, Src.GetRoot(), Lang::Get(Lang::Python), Entries);
	Entries.erase(
		std::remove_if(
			Entries.begin(),
			Entries.end(),
			[&](const BreakEntry &Entry) -> bool {
				// 括弧外候補の除去条件
				return !IsInPythonContinuation(Ranges, Entry.Pos, Entry.Pos);
			}
		),
		Entries.end()
	);
	for(size_t Pos = Src.find('\n'); Pos != std::string::npos; Pos = Src.find('\n', Pos + 1)) {
		// 原文改行位置の追加
		Newlines.push_back(static_cast<uint32_t>(Pos));
	}
	// 括弧内候補の幅選別
	GreedyMerge(Src, Breaks, {}, Entries, Newlines, {});
	// 終了
	return;
}

/**
 * Python の文境界とインデントを保つ行分割関数
 * @param Src 解析済のソース
 */
void LineSplitPass::ApplyPython(TSSource &Src) {
	// 幅無制限の場合の返戻
	if(!MaxChars) return;
	// 全体を包んだ型注釈番号
	const std::vector<uint32_t> WrappedTypes = WrapPythonExpressions(Src);
	std::vector<uint32_t> Breaks, Newlines;
	// Python の採用境界収集
	CollectPythonBreaks(Src, Breaks, Newlines);
	std::vector<TextEdit> Edits;
	size_t Line = 0;
	uint32_t LineStart = 0;
	for(const uint32_t Pos : Breaks) {
		// 対応する原文行の探索
		while(Line < Newlines.size() && Newlines[Line] < Pos) LineStart = Newlines[Line++] + 1;
		// 元本文の開始位置
		const uint32_t ContentStart = TextEdit::SkipSpRight(Src, LineStart);
		TextEdit::Push(Pos, Pos, "\n" + Src.substr(LineStart, ContentStart - LineStart), Edits);
	}
	TextEdit::Apply(Src, Edits);
	// 注釈の始まりで割る為に包んだ型注釈が，括弧の内側で割られずに１行に収まった場合は，冗長な括弧を外す
	if(WrappedTypes.empty()) return;
	// 包装後の最外型注釈列
	const std::vector<TSNode> Types = OuterPythonTypes(Src);
	std::vector<TextEdit> Unwraps;
	for(const uint32_t Ordinal : WrappedTypes) {
		// 再解析後に消えた注釈の除外
		if(Ordinal >= Types.size()) continue;
		const TSNode Paren = ts_node_named_child(Types[Ordinal], 0);
		if(NamedTypeOf(Paren) != "parenthesized_expression" || std::memchr(Src.data() + Src.Start(Paren), '\n', Src.Len(Paren))) {
			// 複数行又は非包装注釈の保持
			continue;
		}
		// 不要な開き括弧の除去予約
		TextEdit::Push(Src.Start(Paren), Src.Start(Paren) + 1, "", Unwraps);
		TextEdit::Push(Src.End(Paren) - 1, Src.End(Paren), "", Unwraps);
	}
	// 不要括弧の一括除去
	if(!Unwraps.empty()) TextEdit::Apply(Src, Unwraps);
	// 終了
	return;
}

/**
 * 幅超過変数宣言の分割関数
 * @param Src ソースコード（破壊的に書き換える）
 * @param Language 対象言語
 */
void LineSplitPass::SplitWideVarDecls(TSSource &Src, const Lang Language) {
	// 行幅上限を超える統合変数宣言の型頭部を繰り返した別宣言への分割
	if(!Language.IsBraceLang()) return;
	// 幅超過宣言の分割
	DeclEdit::SplitDeclarations(
		Src,
		Language,
		[&Src, Language](std::vector<DeclEdit::SplitCandidate> &Cands) -> void {
			WalkChildrenCursor(
				Src.GetRoot(),
				[&](const TSNode Node, const TSNode Parent) -> bool {
					// 文字列化マクロの実引数内に在る宣言の分割抑止
					if(Language.IsCFamily() && Src.StringizesArguments(Node)) return false;
					// 照合する型は全て名前付の為，無名字句は型名の取得も集合検索もせずに落とす
					if(!ts_node_is_named(Node)) return true;
					// 名前付節点の型取得
					const char *const TypeRaw = ts_node_type(Node);
					// AnalyzeDecl と宣言子収集の幅超過宣言への限定
					if(
						!NodeKind::DeclLike.Contains(TypeRaw) || TextEdit::ContentChars(Src.View(Node)) <= MaxChars ||
						!NodeKind::SplittableDeclParent.Contains(Parent)
						// 分割対象外の子へ降りる事の返戻
					) return true;
					// 宣言の構成情報
					const DeclEdit::DeclSummary Decl = DeclEdit::AnalyzeDecl(Src, Node, Language);
					// 解析出来ない宣言の子へ降りる事の返戻
					if(!Decl.IsValid) return true;
					std::vector<TSNode> Declarators;
					// 宣言子列の収集
					DeclEdit::CollectDeclarators(Node, Decl.TypePrefixEnd, Language, Declarators);
					// 単一の宣言子は別宣言へ分割不能
					if(Declarators.size() > 1) Cands.push_back({ Node, TSNode{}, Decl, std::move(Declarators), TypeRaw });
					// 宣言の中（初期化子のラムダ等）へも降りる事の返戻
					return true;
				}
			);
		},
		[](const std::vector<size_t> &Widths, const size_t TypeWidth, const size_t Begin) -> size_t {
			// 先頭宣言子から幅に収まる群の算出
			size_t Width = TypeWidth + Widths[Begin] + 2, End = Begin + 1;
			for(; End < Widths.size() && Width + Widths[End] + 2 <= MaxChars; ++End) Width += Widths[End] + 2;
			// 群に入れない最初の宣言子の索引の返戻
			return End;
		},
		// KeepsIndent：行分割は後段のインデントに委ねる為に写さない
		false,
		// 従来対象の言語だけ同じ巡で不変修飾を付与して不動点へ到達させる
		Language == Lang::Java || Language == Lang::JavaScript || Language == Lang::TypeScript ?
		std::function<void(TSSource &)>(
			[Language](TSSource &Target) -> void {
				// 分割後宣言の修飾更新
				EditPass::ApplyImmutableQualifiers(Target, Language);
			}
		) :
		std::function<void(TSSource &)>()
	);
	// 終了
	return;
}

/**
 * 幅超過の Ruby の１行の分岐の節境界分割関数
 * `then` と単一文本体を保った儘，後続の節の前で改行する
 * @param Src ソースコード（破壊的に書き換える）
 * @param Breaks 現在のソースに対する分割位置（昇順）
 * @param Newlines 現在のソースの改行位置（昇順）
 * @return 節境界を分割したら true
 */
bool LineSplitPass::ExpandWideRubyBranches(
	TSSource &Src,
	const std::vector<uint32_t> &Breaks,
	const std::vector<uint32_t> &Newlines
) {
	// 改行編集列の準備
	std::vector<TextEdit> Edits;
	const auto BreakBefore = [&Src, &Edits](const uint32_t Token) {
		if(const uint32_t SpaceStart = TextEdit::SkipSpLeft(Src, Token); SpaceStart && Src[SpaceStart - 1] != '\n') {
			// 節字句前の改行予約
			Edits.push_back({ SpaceStart, Token, "\n" });
		}
	};
	// Ruby の then 周囲と else 後の改行への置換
	const auto BreakAfter = [&Src, &Edits](const TSNode Token, const TSNode Body) {
		// 節字句に応じた前後空白の改行置換
		if(Src.View(Token) == "then") {
			Edits.push_back(
				{
					TextEdit::SkipSpLeft(Src, Src.Start(Token)),
					ts_node_is_null(Body) ? Src.End(Token) : Src.Start(Body),
					ts_node_is_null(Body) ? "" : "\n"
				}
			);
		} else if(!ts_node_is_null(Body)) Edits.push_back({ Src.End(Token), Src.Start(Body), "\n" });
	};
	// 分岐の節を巡り，`then` と `else` の後を改行し，節の語の前で改行する（本体の中のネストの分岐へは降りない）
	std::vector<TSNode> Clauses;
	const auto Visit = [&](const TSNode Branch) {
		// 根分岐だけの初期化
		Clauses.assign(1, Branch);
		while(!Clauses.empty()) {
			const TSNode Clause = Clauses.back();
			// 取得済節を除いて本体待ち状態で子を走査
			Clauses.pop_back();
			TSNode Pending {};
			ForEachChild(
				Clause,
				[&](const TSNode Child) {
					if(ts_node_is_named(Child)) {
						// 待機節と本体の分離
						if(!ts_node_is_null(Pending)) BreakAfter(Pending, Child);
						// 処理済節字句の初期化
						Pending = TSNode{};
						// 内側節の走査予約
						if(NodeKind::RubyBranchClause.Contains(Child)) Clauses.push_back(Child);
						// 節と本体の子の終了
						return;
					}
					const std::string_view Token = Src.View(Child);
					// 本体待ちの then 記録
					if(Token == "then") Pending = Child;
					else if(NodeKind::RubyClauseWord.Contains(Token)) {
						// 節字句前の分割予約
						BreakBefore(Src.Start(Child));
						// 本体待ちの else 記録
						if(Token == "else") Pending = Child;
					}
				}
			);
			// 本体の無い待機節の確定
			if(!ts_node_is_null(Pending)) BreakAfter(Pending, TSNode{});
		}
	};
	// 分割位置・改行位置の並びで，位置より前の最後の境界の後（行の始まり）と，位置以後の最初の境界（行の終わり）
	const auto LastBefore = [](const std::vector<uint32_t> &Sorted, const uint32_t Pos, const uint32_t Skip) {
		const std::vector<uint32_t>::const_iterator Iter = std::upper_bound(Sorted.begin(), Sorted.end(), Pos);
		// 位置より前の最終境界直後の返戻
		return Iter == Sorted.begin() ? 0 : *(Iter - 1) + Skip;
	};
	const auto FirstFrom = [&Src](const std::vector<uint32_t> &Sorted, const uint32_t Pos) {
		// 位置以後の最初の境界
		const std::vector<uint32_t>::const_iterator Iter = std::lower_bound(Sorted.begin(), Sorted.end(), Pos);
		// 最初の境界又は原稿末尾の返戻
		return Iter == Sorted.end() ? static_cast<uint32_t>(Src.size()) : *Iter;
	};
	uint32_t SegmentStart = std::numeric_limits<uint32_t>::max();
	size_t SegmentWidth = 0;
	// Ruby 構文木の分岐探索
	WalkChildrenCursor(
		Src.GetRoot(),
		[&](const TSNode Node) {
			// 現在節点の型名
			const std::string_view Type = ts_node_type(Node);
			// 分岐でない節点は文字列の中へ降りずに巡る事の返戻
			if(!NodeKind::RubyBranch.Contains(Type)) return !NodeKind::Leaf.Contains(Type);
			const uint32_t Start = Src.Start(Node), End = Src.End(Node);
			// 既に複数行の分岐は，本体の中の１行の分岐を探す事の返戻
			if(FirstFrom(Newlines, Start) < End) return true;
			// 分岐の中の割り位置
			const bool IsSplit = FirstFrom(Breaks, Start + 1) < End;
			if(!IsSplit) {
				const uint32_t LineStart = std::max(LastBefore(Newlines, Start, 1), LastBefore(Breaks, Start, 0));
				if(LineStart != SegmentStart) {
					// 幅キャッシュの鍵更新
					SegmentStart = LineStart;
					// 論理行の幅計測
					SegmentWidth = TextEdit::ContentChars(
						std::string_view(Src).substr(LineStart, std::min(FirstFrom(Newlines, End), FirstFrom(Breaks, End)) - LineStart)
					);
				}
			}
			if(IsSplit || SegmentWidth > MaxChars) {
				// 幅超過分岐の展開予約
				Visit(Node);
				// 展開済分岐内に在る幅超過の１行分岐の追加展開
				WalkChildrenCursor(
					Node,
					[&](const TSNode Inner) {
						// 内側節点の型名
						const std::string_view InnerType = ts_node_type(Inner);
						if(
							NodeKind::RubyBranch.Contains(InnerType) && FirstFrom(Newlines, Src.Start(Inner)) >= Src.End(Inner) &&
							TextEdit::ContentChars(Src.View(Inner)) > MaxChars
							// 幅超過の内側分岐の展開予約
						) Visit(Inner);
						// 文字列の中へ降りずに巡る事の返戻
						return !NodeKind::Leaf.Contains(InnerType);
					}
				);
			}
			// 割らない分岐の中の分岐も割らず，戻した分岐の中の分岐は上で見た為，降りない事の返戻
			return false;
		}
	);
	// 戻す分岐が無い事の返戻
	if(Edits.empty()) return false;
	// 節境界改行の適用
	TextEdit::Apply(Src, Edits);
	// 戻した事の返戻
	return true;
}

/**
 * ヒアドキュメント開始行の分割候補除去関数
 * @param Source ソースコード
 * @param Root ルートノード
 * @param Entries 分割候補（開始トークンの在る行に掛かる候補を除去する）
 */
void LineSplitPass::DropHeredocLineBreaks(const std::string &Source, const TSNode Root, std::vector<BreakEntry> &Entries) {
	std::vector<std::pair<uint32_t, uint32_t>> Lines;
	HeldCursor Cursor(Root);
	bool IsDone = false;
	while(!IsDone) {
		if(const TSNode Node = ts_tree_cursor_current_node(&Cursor); std::string_view(ts_node_type(Node)) == "heredoc_beginning") {
			// 開始字句の位置
			const uint32_t Start = ts_node_start_byte(Node);
			const size_t Head = Source.rfind('\n', Start), Tail = Source.find('\n', Start);
			// 開始字句を含む行範囲の追加
			Lines.emplace_back(
				Head == std::string::npos ? 0 : static_cast<uint32_t>(Head + 1),
				Tail == std::string::npos ? static_cast<uint32_t>(Source.size()) : static_cast<uint32_t>(Tail)
			);
		}
		// 葉である開始字句から兄弟への走査移動
		if(!ts_tree_cursor_goto_first_child(&Cursor)) while(!ts_tree_cursor_goto_next_sibling(&Cursor)) {
			if(!ts_tree_cursor_goto_parent(&Cursor) || ts_node_eq(ts_tree_cursor_current_node(&Cursor), Root)) {
				// 根へ戻った走査の完了
				IsDone = true;
				break;
			}
		}
	}
	// ヒアドキュメントが無い場合の終了
	if(Lines.empty()) return;
	std::sort(Lines.begin(), Lines.end());
	// 開始行上の候補除去
	Entries.erase(
		std::remove_if(
			Entries.begin(),
			Entries.end(),
			[&Lines](const BreakEntry &Entry) -> bool {
				const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Iter = std::upper_bound(
					Lines.begin(),
					Lines.end(),
					Entry.Pos,
					[](const uint32_t Pos, const std::pair<uint32_t, uint32_t> &Line) -> bool {
						// 候補位置と行開始の比較
						return Pos < Line.first;
					}
				);
				// 開始行に掛かる候補の除去の返戻（掛からなければ保持）
				return Iter != Lines.begin() && Entry.Pos <= (Iter - 1)->second;
			}
		),
		Entries.end()
	);
	// 終了
	return;
}

/**
 * PHP の地の文の境界の分割候補除去関数
 * @param Root ルートノード
 * @param Entries 分割候補（地の文の直前・直後の候補を除去する）
 */
void LineSplitPass::DropInlineHtmlBreaks(const TSNode Root, std::vector<BreakEntry> &Entries) {
	// PHP の字句間空白に対する構造化結果の保持
	std::vector<uint32_t> Bounds;
	// PHP 地の文の探索
	WalkChildrenCursor(
		Root,
		[&Bounds](const TSNode Node) -> bool {
			// 地の文以外の子孫走査を続ける事の返戻
			if(!NodeKind::PhpInlineHtml.Contains(Node)) return true;
			// 地の文の開始境界
			Bounds.push_back(ts_node_start_byte(Node));
			// 地の文の終了境界
			Bounds.push_back(ts_node_end_byte(Node));
			// 地の文の内側へ降りない事の返戻
			return false;
		}
	);
	// 地の文が無い場合の終了
	if(Bounds.empty()) return;
	std::sort(Bounds.begin(), Bounds.end());
	// 地の文境界候補の除去
	Entries.erase(
		std::remove_if(
			Entries.begin(),
			Entries.end(),
			[&Bounds](const BreakEntry &Entry) -> bool {
				// 地の文の境界に在る候補の除去の返戻（其れ以外は保持）
				return std::binary_search(Bounds.begin(), Bounds.end(), Entry.Pos);
			}
		),
		Entries.end()
	);
	// 終了
	return;
}

/**
 * 字面の儘の実引数の中の分割候補除去関数 (C / C++)
 * 実引数を文字列化するマクロ (`#x`) と assert の実引数は字面が値に為り，割ると文字列に空白が入る為，中の候補を除く
 * @param Src ソースコード
 * @param Entries 分割候補（実引数列の内側の候補を除去する）
 */
void LineSplitPass::DropStringizedArgumentBreaks(const TSSource &Src, std::vector<BreakEntry> &Entries) {
	// 字面保持実引数列の範囲収集
	std::vector<std::pair<uint32_t, uint32_t>> Ranges;
	WalkChildrenCursor(
		Src.GetRoot(),
		[&](const TSNode Node) -> bool {
			// 字面の儘の実引数列以外の子孫走査を続ける事の返戻
			if(!Src.StringizesArguments(Node)) return true;
			// 実引数列範囲の追加
			Ranges.emplace_back(Src.Start(Node), Src.End(Node));
			// 実引数列の内側へ降りない事の返戻
			return false;
		}
	);
	// 対象の実引数列が無い場合の終了
	if(Ranges.empty()) return;
	// 字面保持範囲内の候補除去
	Entries.erase(
		std::remove_if(
			Entries.begin(),
			Entries.end(),
			[&Ranges](const BreakEntry &Entry) -> bool {
				const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Iter = std::upper_bound(
					Ranges.begin(),
					Ranges.end(),
					Entry.Pos,
					[](const uint32_t Pos, const std::pair<uint32_t, uint32_t> &Range) -> bool {
						// 候補位置と範囲開始の比較
						return Pos < Range.first;
					}
				);
				// 実引数列の内側の候補の除去の返戻
				return Iter != Ranges.begin() && Entry.Pos < (Iter - 1)->second;
			}
		),
		Entries.end()
	);
	// 終了
	return;
}

/** ========== 制御構文の折返 ========== */
/**
 * 制御構文の探索関数
 * @param Source 走査対象を含むソース
 * @param Node 走査対象のノード
 * @param Language 対象言語
 * @param Blocks 文ブロックの範囲の一覧（開始の昇順，ネストは外側が先）
 * @param Out 収集結果（追記先）
 * @param IsElseAfterBlock 波括弧ブロックの then 句に続く else 節か（親から受け渡す）
 * @param Chain 畳んだ Kotlin の if の連鎖の印（連鎖の else の本体と其の中の if へ受け渡す，連鎖の外は 0）
 */
void LineSplitPass::FindCtrlFlows(
	const TSSource &Source,
	const TSNode Node,
	const Lang Language,
	const std::vector<std::pair<uint32_t, uint32_t>> &Blocks,
	std::vector<CtrlFlowInfo> &Out,
	const bool IsElseAfterBlock,
	const uint32_t Chain
) {
	// 葉・無効ノード，Ruby，文字列化マクロ実引数の波括弧付与対象からの除外
	if(ts_node_is_null(Node) || Language == Lang::Ruby || Language.IsCFamily() && Source.StringizesArguments(Node)) return;
	const RecursionGuard Guard;
	if(Guard.IsOverflow) {
		Parallel::RunOnFreshStack(
			[&]() -> void {
				// 新しい走脈での探索継続
				FindCtrlFlows(Source, Node, Language, Blocks, Out, IsElseAfterBlock, Chain);
			}
		);
		// 新しい走脈での探索完了
		return;
	}
	// 子数は早期返戻判定と後段の明示波括弧本体判定で共有する（同一ノードへの二重呼出を排除）
	const uint32_t NodeChildCount = ts_node_child_count(Node);
	// 条件成立時の返戻
	if(!NodeChildCount) return;
	// else・catch 等の節
	static const NodeKind::NodeTypeSet BraceWrapBodyExcluded = {
		"ERROR",
		"catch_clause",
		"else_clause",
		"else_if_clause",
		"finally_clause",
		"seh_finally_clause",
		"switch_expression_arm",
		"where_clause"
	};
	static const NodeKind::NodeTypeSet BraceWrapThenExcluded = { "if_statement", "parenthesized_expression" };
	const std::string_view NType(ts_node_type(Node));
	uint32_t NodeChain = 0;
	TSNode ChainElseBody {};
	const bool IsPhpAltSyntax = Language == Lang::PHP && NodeKind::Control.Contains(NType) && HasChildOf(
		Node,
		[](const TSNode Child) -> bool {
			// PHP 子節点の型名
			const char *const Type = ts_node_type(Child);
			// 代替構文の印（本体の `colon_block`，又は条件の直後に置かれる無名の `:`）に合致するかの返戻
			return Type && (std::string_view(Type) == "colon_block" || !ts_node_is_named(Child) && std::string_view(Type) == ":");
		}
	);
	if(NodeKind::Control.Contains(NType) && !IsPhpAltSyntax) {
		const bool IsIfStmt = NType == "if_statement" || Language == Lang::Kotlin && NType == "if_expression";
		const bool IsKotlinControl = Language == Lang::Kotlin && NodeKind::SingleIndented.Contains(NType);
		TSNode ThenCandidate {}, PrevNamed {}, FirstNamed {}, ElseToken {}, PrevChild {};
		bool HasFoundElse = false, IsLastAfterElse = false;
		// 直前の兄弟は走査中に持ち回る
		ForEachChild(
			Node,
			[&](const TSNode Child) -> void {
				// 直前子の取得と次回用更新
				const TSNode Prev = PrevChild;
				PrevChild = Child;
				// 条件成立時の返戻
				if(!ts_node_is_named(Child) || IsKotlinControl && std::string_view(ts_node_type(Child)) != "control_structure_body") return;
				// 最初の名前付子記録
				if(ts_node_is_null(FirstNamed)) FirstNamed = Child;
				if(
					IsIfStmt && !HasFoundElse && (
						NodeKind::ElseLikeClause.Contains(Child) ||
						Language == Lang::Kotlin && !ts_node_is_null(Prev) && std::string_view(ts_node_type(Prev)) == "else"
					)
				) {
					// then 候補と else 状態の確定
					ThenCandidate =
					!ts_node_is_null(Prev) && !ts_node_is_named(Prev) && std::string_view(ts_node_type(Prev)) == "}" ? TSNode{} : PrevNamed;
					HasFoundElse = true;
					if(Language == Lang::Kotlin) ElseToken = Prev;
				}
				// Swift の名前付 else 葉の字句種別による識別
				IsLastAfterElse = !ts_node_is_null(Prev) && NodeKind::ElseKeyword.Contains(Prev);
				// 次回用の直前名前付子
				PrevNamed = Child;
			}
		);
		// else を持つ Kotlin の if 式が１行に畳まれて居れば
		if(Language == Lang::Kotlin && NType == "if_expression" && HasFoundElse) {
			// 連鎖識別値
			NodeChain = Chain ? Chain : ts_node_start_point(Node).row == ts_node_end_point(Node).row ? ts_node_start_byte(Node) + 1 : 0;
			// 連鎖 else の本体記録
			ChainElseBody = PrevNamed;
		}
		std::vector<std::pair<uint32_t, uint32_t>> Excluded;
		bool IsExcludedBuilt = false;
		const auto BuildExcluded = [&](const uint32_t Limit) -> void {
			// 条件成立時の返戻
			if(IsExcludedBuilt) return;
			// 除外範囲構築済状態の確定
			// 重複構築の抑止
			IsExcludedBuilt = true;
			const auto StartsBefore = [](const std::pair<uint32_t, uint32_t> &Block, const uint32_t Pos) -> bool {
				// 文ブロックが位置より前に始まるかの返戻
				return Block.first < Pos;
			};
			const auto StartsAfter = [](const uint32_t Pos, const std::pair<uint32_t, uint32_t> &Block) -> bool {
				// 文ブロックが位置より後に始まるかの返戻
				return Pos < Block.first;
			};
			// 制御構文は見出の字句で始まる為，同じ所に始まる文ブロックは子孫でなく祖先か自身で，飛ばす
			for(
				std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Block =
				std::upper_bound(Blocks.begin(), Blocks.end(), ts_node_start_byte(Node), StartsAfter);
				Block != Blocks.end() && Block->first < Limit;
				Block = std::lower_bound(Block + 1, Blocks.end(), Excluded.back().second, StartsBefore)
			) Excluded.push_back(*Block); // 子孫ブロック範囲の追加
		};
		// 単一インデント本体の改行時包装対象への限定
		const bool IsParentSingleIndented = NodeKind::SingleIndented.Contains(NType);
		const auto NeedsNewlineWrap = [IsParentSingleIndented](const TSNode BodyNode) -> bool {
			TSNode Inner = BodyNode;
			if(std::string_view(ts_node_type(Inner)) == "control_structure_body" && ts_node_named_child_count(Inner) == 1) {
				// Kotlin 包装内の唯一文
				Inner = ts_node_named_child(Inner, 0);
			}
			// 実本体の型名
			const std::string_view BodyType = ts_node_type(Inner);
			// 改行で波括弧を付けるかの返戻
			return IsParentSingleIndented && !NodeKind::SingleIndented.Contains(BodyType) && !NodeKind::IfNode.Contains(BodyType);
		};
		if(!ts_node_is_null(PrevNamed)) {
			const TSNode Body = NodeKind::DoLoop.Contains(NType) ? FirstNamed : PrevNamed;
			const std::string_view BodyTypeView(ts_node_type(Body)), LastChildType = ts_node_type(ts_node_child(Node, NodeChildCount - 1));
			const bool IsBodyClosedByToken = LastChildType == "}" || LastChildType == ";" && !NodeKind::DoLoop.Contains(NType);
			// 親が if_statement / else_clause の else-if 連鎖と，else_clause を本体とする形は除外
			if(
				const bool IsElseIfChain =
				NodeKind::IfNode.Contains(BodyTypeView) && NodeKind::ElseIfChainParent.Contains(NType) && IsLastAfterElse ||
				Language == Lang::Kotlin && NType == "if_expression" && HasFoundElse && BodyTypeView == "control_structure_body" &&
				ts_node_named_child_count(Body) == 1 && std::string_view(ts_node_type(ts_node_named_child(Body, 0))) == "if_expression";
				// 無限反復を招く Swift の try 式と repeat 本体の除外
				!BodyTypeView.empty() && !EditPass::IsStmtBlockType(Body, Language) && !IsElseIfChain &&
				!BraceWrapBodyExcluded.Contains(BodyTypeView) && !NodeKind::BraceWrapStmtExcluded.Contains(NType) && !IsBodyClosedByToken
			) {
				// 本体内の除外範囲構築
				BuildExcluded(ts_node_end_byte(Body));
				// 後続 Then 句経路の有無に応じた Excluded の移動
				Out.push_back(
					{
						ts_node_start_byte(ts_node_is_null(ElseToken) ? Node : ElseToken),
						ts_node_start_byte(Body),
						ts_node_end_byte(Body),
						IsIfStmt && !ts_node_is_null(ThenCandidate) ? Excluded : std::move(Excluded),
						NeedsNewlineWrap(Body),
						IsElseAfterBlock ||
						!ts_node_is_null(ElseToken) && !ts_node_is_null(ThenCandidate) && EditPass::IsStmtBlockType(ThenCandidate, Language),
						NodeChain
					}
				);
			}
		}
		if(IsIfStmt && !ts_node_is_null(ThenCandidate)) {
			if(
				const std::string_view ThenTypeView = ts_node_type(ThenCandidate);
				!ThenTypeView.empty() && !BraceWrapThenExcluded.Contains(ThenTypeView) && !EditPass::IsStmtBlockType(ThenCandidate, Language)
			) {
				// then 内の除外範囲構築
				BuildExcluded(ts_node_end_byte(ThenCandidate));
				// Then 句追加時の Excluded の移動
				Out.push_back(
					{
						ts_node_start_byte(Node),
						ts_node_start_byte(ThenCandidate),
						ts_node_end_byte(ThenCandidate),
						std::move(Excluded),
						NeedsNewlineWrap(ThenCandidate),
						false,
						NodeChain
					}
				);
			}
		}
	}
	// else 節が閉じ `}` と同行化されるかは親の if 文でしか判らない
	TSNode ElseAfterBlockChild {};
	if(NType == "if_statement") {
		TSNode PrevChild {};
		// else 節までの子走査
		ForEachNamedChild(
			Node,
			[&ElseAfterBlockChild, &PrevChild](const TSNode Child) -> bool {
				// else 節迄の直前子更新
				if(std::string_view(ts_node_type(Child)) != "else_clause") {
					// 次回用の直前子更新
					PrevChild = Child;
					// else 節ではない為，走査継続の返戻
					return true;
				}
				if(!ts_node_is_null(PrevChild) && std::string_view(ts_node_type(PrevChild)) == "statement_block") ElseAfterBlockChild = Child;
				// else 節に到達した為，走査打切の返戻
				return false;
			}
		);
	}
	// 子制御構文の再帰走査
	ForEachNamedChild(
		Node,
		[&](const TSNode Child) -> void {
			// 畳んだ Kotlin の if の連鎖の印は，else の本体と其の中の if へだけ受け渡す（枝の本体の中の別の if へは渡さない）
			const uint32_t ChildChain = ts_node_eq(Child, ChainElseBody) ?
			NodeChain :
			NType == "control_structure_body" && std::string_view(ts_node_type(Child)) == "if_expression" ? Chain : 0;
			// 子節点内の制御構文探索
			FindCtrlFlows(
				Source,
				Child,
				Language,
				Blocks,
				Out,
				!ts_node_is_null(ElseAfterBlockChild) && ts_node_eq(Child, ElseAfterBlockChild),
				ChildChain
			);
		}
	);
	// 終了
	return;
}

/**
 * 制御構文の並列探索関数
 * @param Source 走査対象のソース
 * @param Language 対象言語
 * @param Out 収集結果（順不同）
 */
void LineSplitPass::FindCtrlFlowsParallel(const TSSource &Source, const Lang Language, std::vector<CtrlFlowInfo> &Out) {
	const TSNode Root = Source.GetRoot();
	// 条件成立時の返戻
	if(ts_node_is_null(Root)) return;
	// 文ブロックの範囲（先行順で開始の昇順，ネストは外側が先）を１度の走査で集め，制御構文毎の除外範囲を二分探索で引く
	std::vector<std::pair<uint32_t, uint32_t>> Blocks;
	// 文ブロック範囲の収集
	WalkChildrenCursor(
		Root,
		[&](const TSNode Node) -> bool {
			// 文字列化マクロの実引数の中は字面を保つ為，文ブロックも収集しない事の返戻
			if(Language.IsCFamily() && Source.StringizesArguments(Node)) return false;
			// 名前付文ブロックの範囲登録
			if(ts_node_is_named(Node) && EditPass::IsStmtBlockType(Node, Language)) {
				// 文ブロック範囲の追加
				Blocks.push_back({ ts_node_start_byte(Node), ts_node_end_byte(Node) });
			}
			// 文ブロックの内側の文ブロックも集める為に降りる事の返戻
			return true;
		}
	);
	// 独立した制御構文情報の出力順保証の省略
	const uint32_t ChildCount = ts_node_child_count(Root);
	const size_t NumThreads = NodeKind::Control.Contains(Root) ? 1 : Parallel::DecideThreads(ChildCount, 8);
	if(NumThreads < 2) {
		// 単一走脈での全木探索
		FindCtrlFlows(Source, Root, Language, Blocks, Out);
		// 終了
		return;
	}
	// 根直下子列の容量確保と収集
	std::vector<TSNode> RootKids;
	RootKids.reserve(ChildCount);
	ForEachChild(
		Root,
		[&](const TSNode Child) -> void {
			// 根直下子の収集
			RootKids.push_back(Child);
		}
	);
	// 走脈別の制御構文情報
	std::vector<std::vector<CtrlFlowInfo>> ThreadOut(NumThreads);
	Parallel::ForChunks(
		ChildCount,
		NumThreads,
		[&](const size_t Start, const size_t End, const size_t Tid) -> void {
			// 担当範囲の探索
			for(size_t Idx = Start; Idx < End; ++Idx) FindCtrlFlows(Source, RootKids[Idx], Language, Blocks, ThreadOut[Tid]);
		}
	);
	// 全走脈結果の件数集計・容量確保・統合
	size_t Total = 0;
	for(const std::vector<CtrlFlowInfo> &Local : ThreadOut) Total += Local.size();
	Out.reserve(Out.size() + Total);
	for(std::vector<CtrlFlowInfo> &Local : ThreadOut) for(CtrlFlowInfo &Ctrl : Local) Out.push_back(std::move(Ctrl));
	// 終了
	return;
}

/**
 * else/while を `}` の行へ続ける位置の収集関数
 * @param Source 対象のソースコード
 * @param Language 対象言語
 * @param JoinBraceEnds 次の断片を同じ行へ続ける `}` の直後の位置の格納先（昇順，先に空にする）
 */
void LineSplitPass::CollectElseWhileJoins(const TSSource &Source, const Lang Language, std::vector<uint32_t> &JoinBraceEnds) {
	// 前回の結合位置破棄
	JoinBraceEnds.clear();
	// `else` も `while` も無い本文（JSON / SCSS 等）は結合が起き得ない為，構文木の走査毎省く
	if(Source.find("else") == std::string::npos && Source.find("while") == std::string::npos) return;
	std::unordered_set<uint32_t> ClosingBraces, ElseBytes, WhenElseBytes, DoWhileBytes;
	// 結合対象字句の構文走査
	WalkChildrenCursor(
		Source.GetRoot(),
		[&](const TSNode Node) -> bool {
			// 文字列化マクロの実引数内では原改行も字面の一部の為，`}` と後続語を結合しない事の返戻
			if(Language.IsCFamily() && Source.StringizesArguments(Node)) return false;
			// when の else は直前の分岐本文の形に依らず独立行に置く為，when の分岐の先頭の字句から集める
			if(!ts_node_is_named(Node)) {
				// 閉じ波括弧位置
				if(const std::string_view Token = Source.View(Node); Token == "}") ClosingBraces.insert(Source.Start(Node));
				// else 字句位置の追加
				else if(NodeKind::ElseKeyword.Contains(Token)) ElseBytes.insert(Source.Start(Node));
			} else if(const std::string_view Type = ts_node_type(Node); Type == "when_entry") {
				if(const TSNode First = ts_node_child(Node, 0); !ts_node_is_named(First) && Source.View(First) == "else") {
					// 独立行に保つ when else
					WhenElseBytes.insert(Source.Start(First));
				}
			} else if(NodeKind::DoLoop.Contains(Type)) {
				// do 文内の while 探索
				ForEachChild(
					Node,
					[&Source, &DoWhileBytes](const TSNode Child) -> bool {
						// 次の子へ進む事の返戻
						if(ts_node_is_named(Child) || Source.View(Child) != "while") return true;
						// do 後の while 位置
						DoWhileBytes.insert(Source.Start(Child));
						// `while` の字句で打ち切る事の返戻
						return false;
					}
				);
			}
			// 逐語本文へ降りずに構文だけを走査する事の返戻
			return !NodeKind::Leaf.Contains(Node);
		}
	);
	// `}` の後に空白・改行だけを挟んで `else`/`while` が続く位置を集める
	for(size_t Idx = Source.find('}'); Idx != std::string::npos; Idx = Source.find('}', Idx + 1)) {
		// 文字列内の波括弧除外
		if(!ClosingBraces.contains(static_cast<uint32_t>(Idx))) continue;
		// 閉じ波括弧後の本文候補
		size_t NextContent = Idx + 1;
		while(
			NextContent < Source.size() && (Source[NextContent] == ' ' || Source[NextContent] == '\t' || Source[NextContent] == '\n')
		) ++NextContent;
		if(
			ElseBytes.contains(static_cast<uint32_t>(NextContent)) && !WhenElseBytes.contains(static_cast<uint32_t>(NextContent)) ||
			DoWhileBytes.contains(static_cast<uint32_t>(NextContent))
			// 同行接続する波括弧終端の追加
		) JoinBraceEnds.push_back(static_cast<uint32_t>(Idx) + 1);
	}
	// 終了
	return;
}

/**
 * 波括弧の折返付与関数
 * 計算量：原稿の長さ N と包む段数 D に対し O(N * D)（段毎に１巡し，手間の上限を超えたら見送る）
 * @param Work ソースコード（入力時点で構文解析済，出力時は付与後テキストで再代入済）
 * @param Language 対象言語
 * @param OutBreaks 付与後テキストに対する最終分割位置集合（出力）
 * @param OutNewlines 付与後テキストに対する最終改行位置集合（昇順，出力）
 * @param OutJoinBraceEnds 付与後テキストの `}` の直後の結合位置集合（昇順，幅無制限では収集しない）
 * @return 手間の上限内で完了出来れば true
 */
bool LineSplitPass::ApplyBraceWraps(
	TSSource &Work,
	const Lang Language,
	std::vector<uint32_t> &OutBreaks,
	std::vector<uint32_t> &OutNewlines,
	std::vector<uint32_t> &OutJoinBraceEnds
) {
	std::vector<uint32_t> BraceLocks;
	// 前回結果を残さない出力範囲の初期化
	OutBreaks.clear();
	// 前回の出力改行破棄
	OutNewlines.clear();
	// 前回の結合位置破棄
	OutJoinBraceEnds.clear();
	// 幅無制限では行幅超過が起き得ず，波括弧の付与も行分割も要らない
	if(!MaxChars) return true;
	// 呼出元での Work.IsParsed() の守備済状態
	Work.SuspendAttachments();
	// 確保解放守備に依り全ての正常 return 経路から ResumeAttachments を保証し，再開漏れを構造的に予防
	struct AttachGuard {
		TSSource &Target;
		bool IsActive = true;

		/**
		 * デストラクタ（添付情報の再開）
		 * 通常終了時の再開失敗は上位の原文保持へ渡し，別の例外で巻き戻して居る時は再解析を始めない
		 */
		~AttachGuard() noexcept(false) {
			// 正常終了時の添付情報の再開
			if(IsActive && !std::uncaught_exceptions()) Target.ResumeAttachments();
			// 終了
			return;
		}
	} Guard { Work };
	// 此処では幅超過の波括弧だけを付け，初回だけ全走査し，改行位置を共有しつつ構文依存の制御候補は巡毎に求め直す
	std::vector<BreakEntry> Entries;
	std::vector<uint32_t> Newlines;
	bool HaveCachedEntries = false;
	size_t RoundLimit = 0;
	static constexpr size_t WrapBudget = 0X80000, WrapPerByteShift = 5;
	const size_t WrapCap = std::max(WrapBudget, Work.size() << WrapPerByteShift);
	size_t WrapWork = 0;
	std::vector<uint32_t> JoinBraceEnds;
	for(size_t Round = 0;; ++Round) {
		const std::string &Current = Work;
		if((WrapWork += Current.size()) > WrapCap) {
			// 未完了の原稿を再解析せずに上位へ見送を返す準備
			Guard.IsActive = false;
			// 手間の上限超過の返戻
			return false;
		}
		// 構文変更後の分割候補の再収集
		if(!HaveCachedEntries) {
			// 現在木の候補収集
			CollectBreaksParallel(Current, Work.GetRoot(), Language, Entries);
			switch(Language.Id) {
			case Lang::Ruby:
				// 開始行候補の除外
				DropHeredocLineBreaks(Current, Work.GetRoot(), Entries);
				break;
			case Lang::PHP:
				// 地の文境界候補の除外
				DropInlineHtmlBreaks(Work.GetRoot(), Entries);
				break;
			case Lang::C:
			case Lang::Cpp:
				// 字面保持範囲候補の除外
				DropStringizedArgumentBreaks(Work, Entries);
				break;
			default:
				break;
			}
			// 差分更新へ切り替える印
			HaveCachedEntries = true;
		}
		// 前回巡の改行位置破棄
		Newlines.clear();
		// 平均行長を基にした領域予約
		Newlines.reserve(Current.size() / 40 + 1);
		// `std::string::find` の内部実装による改行探索の高速化
		for(size_t Found = Current.find('\n'); Found != std::string::npos; Found = Current.find('\n', Found + 1)) {
			// 原改行位置の追加
			Newlines.push_back(static_cast<uint32_t>(Found));
		}
		// 今回巡の採用分割
		std::vector<uint32_t> Breaks;
		// 今回木での再収集準備
		JoinBraceEnds.clear();
		if(Language != Lang::Ruby) CollectElseWhileJoins(Work, Language, JoinBraceEnds);
		GreedyMerge(Current, Breaks, BraceLocks, Entries, Newlines, JoinBraceEnds);
		std::vector<CtrlFlowInfo> Ctrls;
		FindCtrlFlowsParallel(Work, Language, Ctrls);
		// 初回候補数による巡回上限
		if(!Round) RoundLimit = Ctrls.size() + 1;
		// 局面 1：各制御構文ノードの付与要否判定
		std::vector<CtrlFlowInfo> NeedWrapSet;
		bool IsBodyWrapFound = false;
		// `Ctrls` は本ループ以降未使用の為，付与必要時に `CtrlFlowInfo`をムーブで移し替える
		for(CtrlFlowInfo &Ctrl : Ctrls) {
			bool NeedsWrap = false;
			const auto ExcludedEnd = [&](const uint32_t Pos) -> uint32_t {
				// 除外範囲終端の返戻
				for(const std::pair<uint32_t, uint32_t> &Range : Ctrl.Excluded) if(Pos > Range.first && Pos < Range.second) return Range.second;
				// 除外範囲外の返戻
				return 0;
			};
			// 本体の内側に在る分割の探索
			std::vector<uint32_t>::const_iterator Iter = std::upper_bound(Breaks.begin(), Breaks.end(), Ctrl.BodyStart);
			while(Iter != Breaks.end() && *Iter < Ctrl.BodyEnd) {
				const uint32_t Skip = ExcludedEnd(*Iter);
				if(!Skip) {
					// 本体内分割による付与決定
					NeedsWrap = true;
					break;
				}
				Iter = std::lower_bound(Iter, Breaks.cend(), Skip);
			}
			// 既存の改行に対する本体包装の判定
			if(!NeedsWrap && Ctrl.IsNewlineWrap) {
				// 本体の終端ポインタ
				const char *const End = Current.data() + Ctrl.BodyEnd;
				for(const char *Cursor = Current.data() + Ctrl.BodyStart; Cursor < End;) {
					const char *const Newline = static_cast<const char *>(std::memchr(Cursor, '\n', End - Cursor));
					if(!Newline) break;
					const uint32_t Skip = ExcludedEnd(static_cast<uint32_t>(Newline - Current.data()));
					if(!Skip) {
						// 本体内改行による付与決定
						NeedsWrap = true;
						break;
					}
					// 除外範囲後への前進
					Cursor = Current.data() + Skip;
				}
			}
			// 閉じ波括弧の直後に在る else 節の幅を測る
			const auto DoesElseBranchOverflow = [&]() -> bool {
				// 閉じ `}` と同行化されない else なら結合が起きないので false の返戻
				if(!Ctrl.IsElseAfterBlock) return false;
				// else 行終端と枝終端の探索
				const std::vector<uint32_t>::const_iterator NlIter = std::lower_bound(Newlines.begin(), Newlines.end(), Ctrl.BodyEnd);
				const uint32_t LineEnd = NlIter != Newlines.end() ? *NlIter : static_cast<uint32_t>(Current.size());
				uint32_t BranchEnd = Ctrl.BodyEnd;
				// 区切り字句を含む else 枝の末尾位置決定
				while(BranchEnd < LineEnd && (Current[BranchEnd] == ';' || Current[BranchEnd] == ' ' || Current[BranchEnd] == '\t')) {
					++BranchEnd;
				}
				// 合成後の else 行
				const std::string_view ElseBody(Current.data() + Ctrl.NodeStart, BranchEnd - Ctrl.NodeStart);
				// 宙ぶらりんの else 解消後の合成行が MaxChars 超過なら付与が必要として返戻
				return TextEdit::ContentChars(ElseBody) + 2 > MaxChars;
			};
			if(!NeedsWrap && DoesElseBranchOverflow()) NeedsWrap = true;
			if(NeedsWrap) {
				// 本体包装前の内部分割の先行処理
				if(!IsBodyWrapFound) {
					// 条件分割候補の破棄
					NeedWrapSet.clear();
					// 本体包装候補の優先化
					IsBodyWrapFound = true;
				}
				NeedWrapSet.push_back(std::move(Ctrl));
			} else if(!IsBodyWrapFound) {
				bool IsCondSplit = false;
				uint32_t CondLineStart = 0;
				Iter = std::upper_bound(Breaks.begin(), Breaks.end(), Ctrl.NodeStart);
				while(Iter != Breaks.end() && *Iter <= Ctrl.BodyStart) {
					if(const uint32_t Skip = ExcludedEnd(*Iter)) {
						Iter = std::lower_bound(Iter, Breaks.cend(), Skip);
						continue;
					}
					// 見出内分割の検出
					IsCondSplit = true;
					// 最後の分割位置更新
					CondLineStart = *Iter;
					++Iter;
				}
				if(IsCondSplit) {
					// 見出の行は制御構文の開始より前で最後の改行又は分割から始まり，本体の開始で終わる
					uint32_t HeadLineStart = 0;
					if(
						const std::vector<uint32_t>::const_iterator NlIter = std::upper_bound(Newlines.begin(), Newlines.end(), Ctrl.NodeStart);
						NlIter != Newlines.begin()
					) HeadLineStart = *std::prev(NlIter) + 1;
					if(
						const std::vector<uint32_t>::const_iterator BrIter = std::upper_bound(Breaks.begin(), Breaks.end(), Ctrl.NodeStart);
						BrIter != Breaks.begin() && *std::prev(BrIter) > HeadLineStart
					) HeadLineStart = *std::prev(BrIter);
					// 畳んだ Kotlin の if の連鎖の枝は，連鎖の全体を包むと見出が枝の頭から始まる為，其の枝の見出で測る
					if(Ctrl.Chain) HeadLineStart = Ctrl.NodeStart;
					// 本体前の見出行
					std::string_view HeadLine(Current.data() + HeadLineStart, Ctrl.BodyStart - HeadLineStart);
					TextEdit::TrimView(HeadLine);
					// 本体を送った後の見出は末尾に ` {` の２文字が付く
					if(HeadLine.find('\n') == std::string_view::npos && TextEdit::ContentChars(HeadLine) + 2 <= MaxChars) {
						NeedWrapSet.push_back(std::move(Ctrl));
					} else {
						uint32_t LineEnd = static_cast<uint32_t>(Current.size());
						if(
							const std::vector<uint32_t>::const_iterator NlIter = std::upper_bound(Newlines.begin(), Newlines.end(), Ctrl.BodyStart);
							NlIter != Newlines.end()
						) LineEnd = *NlIter;
						if(
							const std::vector<uint32_t>::const_iterator BrIter = std::upper_bound(Breaks.begin(), Breaks.end(), Ctrl.BodyStart);
							BrIter != Breaks.end() && *BrIter < LineEnd
						) LineEnd = *BrIter;
						// 閉じ括弧を含む行
						std::string_view CloserLine(Current.data() + CondLineStart, LineEnd - CondLineStart);
						TextEdit::TrimView(CloserLine);
						if(TextEdit::ContentChars(CloserLine) > MaxChars) NeedWrapSet.push_back(std::move(Ctrl));
					}
				}
			}
		}
		{
			std::vector<uint32_t> Chains, Wrapped;
			for(const CtrlFlowInfo &Wrap : NeedWrapSet) {
				if(Wrap.Chain) Chains.push_back(Wrap.Chain);
				Wrapped.push_back(Wrap.BodyStart);
			}
			if(!Chains.empty()) {
				std::sort(Chains.begin(), Chains.end());
				std::sort(Wrapped.begin(), Wrapped.end());
				// Ctrls から移動した包装候補の位置と印の保持
				for(const CtrlFlowInfo &Ctrl : Ctrls) {
					if(
						Ctrl.Chain && std::binary_search(Chains.begin(), Chains.end(), Ctrl.Chain) &&
						!std::binary_search(Wrapped.begin(), Wrapped.end(), Ctrl.BodyStart)
					) {
						NeedWrapSet.push_back(
							{ Ctrl.NodeStart, Ctrl.BodyStart, Ctrl.BodyEnd, {}, Ctrl.IsNewlineWrap, Ctrl.IsElseAfterBlock, Ctrl.Chain }
						);
					}
				}
			}
		}
		// 包装不要又は巡回上限超過時の最終結果返却
		if(NeedWrapSet.empty() || Round > RoundLimit) {
			// 最終採用分割の返却
			OutBreaks = std::move(Breaks);
			// 最終改行位置の返却
			OutNewlines = std::move(Newlines);
			// 最終結合位置の返却
			OutJoinBraceEnds = std::move(JoinBraceEnds);
			// 終了
			return true;
		}
		// 局面２の他候補を内包しない包装対象の選択
		std::vector<size_t> InnermostOrder(NeedWrapSet.size());
		std::iota(InnermostOrder.begin(), InnermostOrder.end(), size_t{});
		std::sort(
			InnermostOrder.begin(),
			InnermostOrder.end(),
			[&NeedWrapSet](const size_t LhsIdx, const size_t RhsIdx) -> bool {
				// 包装範囲の開始順と包含順の比較
				const CtrlFlowInfo &Lhs = NeedWrapSet[LhsIdx], &Rhs = NeedWrapSet[RhsIdx];
				// 開始位置の順の比較の返戻
				if(Lhs.BodyStart != Rhs.BodyStart) return Lhs.BodyStart < Rhs.BodyStart;
				// 同一 BodyStart は範囲が広い側を先の返戻（祖先→子孫順）
				return Lhs.BodyEnd > Rhs.BodyEnd;
			}
		);
		std::vector<uint8_t> NotInnermost(NeedWrapSet.size(), 0);
		std::vector<size_t> AncestorStack;
		AncestorStack.reserve(NeedWrapSet.size());
		for(const size_t Idx : InnermostOrder) {
			// 現在の範囲順候補
			const CtrlFlowInfo &Cur = NeedWrapSet[Idx];
			while(!AncestorStack.empty() && NeedWrapSet[AncestorStack.back()].BodyEnd <= Cur.BodyStart) AncestorStack.pop_back();
			if(!AncestorStack.empty()) {
				const CtrlFlowInfo &Top = NeedWrapSet[AncestorStack.back()];
				// 同一本体の同位は互いに最内側判定に影響しないので飛ばす
				if(Top.BodyStart != Cur.BodyStart || Top.BodyEnd != Cur.BodyEnd) {
					for(size_t StackIdx = AncestorStack.size(); StackIdx; --StackIdx) {
						// 同一本体祖先の番号
						const size_t AncIdx = AncestorStack[StackIdx - 1];
						if(NotInnermost[AncIdx]) break;
						const CtrlFlowInfo &Anc = NeedWrapSet[AncIdx];
						if(Anc.BodyStart == Top.BodyStart && Anc.BodyEnd == Top.BodyEnd) NotInnermost[AncIdx] = true;
						else break;
					}
				}
			}
			AncestorStack.push_back(Idx);
		}
		// 今回付与する最内側候補
		std::vector<CtrlFlowInfo> Chosen;
		Chosen.reserve(NeedWrapSet.size());
		// 最終使用後の NeedWrapSet の移動
		for(size_t Idx = 0; Idx < NeedWrapSet.size(); ++Idx) if(!NotInnermost[Idx]) Chosen.push_back(std::move(NeedWrapSet[Idx]));
		// 最内側候補が無い場合の最終結果返却
		if(Chosen.empty()) {
			// 最終採用分割の返却
			OutBreaks = std::move(Breaks);
			// 最終改行位置の返却
			OutNewlines = std::move(Newlines);
			// 最終結合位置の返却
			OutJoinBraceEnds = std::move(JoinBraceEnds);
			// 終了
			return true;
		}
		std::sort(
			Chosen.begin(),
			Chosen.end(),
			[](const CtrlFlowInfo &Lhs, const CtrlFlowInfo &Rhs) -> bool {
				return Lhs.BodyStart < Rhs.BodyStart;
			}
		);
		// 非交差の波括弧付与後は BraceLocks を補正量 `2S + 2A` で移し，開始・終了位置の整列列を二分探索して差分再解析へ渡す
		std::vector<uint32_t> ChosenEnds;
		ChosenEnds.reserve(Chosen.size());
		for(const CtrlFlowInfo &Wrap : Chosen) ChosenEnds.push_back(Wrap.BodyEnd);
		std::sort(ChosenEnds.begin(), ChosenEnds.end());
		const auto CountStarts = [&Chosen](const uint32_t Pos) -> uint32_t {
			// BodyStart ≤ Pos の付与数の返戻
			return static_cast<uint32_t>(
				std::upper_bound(
					Chosen.begin(),
					Chosen.end(),
					Pos,
					[](const uint32_t Val, const CtrlFlowInfo &Wrap) -> bool {
						return Val < Wrap.BodyStart;
					}
				) - Chosen.begin()
			);
		};
		const auto CountEnds = [&ChosenEnds](const uint32_t Pos) -> uint32_t {
			// BodyEnd ≤ Pos の付与数の返戻
			return static_cast<uint32_t>(std::upper_bound(ChosenEnds.begin(), ChosenEnds.end(), Pos) - ChosenEnds.begin());
		};
		std::vector<TextEdit> Edits;
		Edits.reserve(Chosen.size() << 1);
		// 挿入後座標の固定分割
		std::vector<uint32_t> NewBraceLocks;
		NewBraceLocks.reserve(BraceLocks.size() + (Chosen.size() << 1));
		for(const uint32_t Pos : BraceLocks) NewBraceLocks.push_back(Pos + (CountStarts(Pos) << 1) + (CountEnds(Pos) << 1));
		std::vector<size_t> ChosenAncestors;
		ChosenAncestors.reserve(Chosen.size());
		// 完了した非交差包装の補正量
		uint32_t DisjointShift = 0;
		for(size_t Idx = 0; Idx < Chosen.size(); ++Idx) {
			const uint32_t Start = Chosen[Idx].BodyStart;
			while(!ChosenAncestors.empty() && Chosen[ChosenAncestors.back()].BodyEnd <= Start) {
				// 完了包装の４文字補正
				DisjointShift += 4;
				ChosenAncestors.pop_back();
			}
			// 現在本体の座標補正
			const uint32_t Shift = DisjointShift + (static_cast<uint32_t>(ChosenAncestors.size()) << 1);
			// `{ ` 挿入後の開き分割位置 = Start + Shift + 2
			NewBraceLocks.push_back(Start + Shift + 2);
			NewBraceLocks.push_back(Chosen[Idx].BodyEnd + Shift + 2);
			TextEdit::Push(Start, Start, "{ ", Edits);
			TextEdit::Push(Chosen[Idx].BodyEnd, Chosen[Idx].BodyEnd, " }", Edits);
			ChosenAncestors.push_back(Idx);
		}
		std::sort(NewBraceLocks.begin(), NewBraceLocks.end());
		NewBraceLocks.erase(std::unique(NewBraceLocks.begin(), NewBraceLocks.end()), NewBraceLocks.end());
		// 次巡へ渡す固定分割
		BraceLocks = std::move(NewBraceLocks);
		TextEdit::Apply(Work, Edits);
		// 編集適用後の構文解析失敗による打切
		if(!Work.IsParsed()) return true;
		uint32_t PrevRawParentId = std::numeric_limits<uint32_t>::max(), PrevAdjParentId = 0;
		for(BreakEntry &Entry : Entries) {
			// 候補位置前の挿入数
			const uint32_t PosStarts = CountStarts(Entry.Pos), PosEnds = CountEnds(Entry.Pos);
			Entry.Pos += (PosStarts << 1) + (PosEnds << 1);
			if(Entry.ParentId != PrevRawParentId) {
				// 補正前親位置のキャッシュ鍵
				PrevRawParentId = Entry.ParentId;
				PrevAdjParentId = Entry.ParentId + (CountStarts(Entry.ParentId) << 1) + (CountEnds(Entry.ParentId) << 1);
			}
			Entry.ParentId = PrevAdjParentId;
			Entry.Depth += PosStarts - PosEnds;
			// `Newlines` は次反復の冒頭で再走査する為，本反復内では更新しない（`std::memchr` は十分高速で差分位置補正より速い）
		}
	}
}

/** ========== 結果組立 ========== */
/**
 * 結果文字列の構築関数
 * @param Source ソースコード
 * @param Breaks 分割位置集合（昇順）
 * @param Newlines Source 内の `\n` 位置（昇順，呼出元で構築済）
 * @param Root 構文木ルートノード
 * @param JoinBraceEnds 次の断片を同じ行へ続ける `}` の直後の位置（昇順）
 * @return 出力テキスト
 */
std::string LineSplitPass::BuildResult(
	const std::string &Source,
	const std::vector<uint32_t> &Breaks,
	const std::vector<uint32_t> &Newlines,
	const TSNode Root,
	const std::vector<uint32_t> &JoinBraceEnds
) {
	// Breaks 位置での改行境界の構築
	std::vector<uint32_t> SplitList;
	SplitList.reserve(Breaks.size() + Newlines.size() + 2);
	SplitList.push_back(0);
	const size_t BreakSize = Breaks.size(), NewlineSize = Newlines.size();
	size_t BreakIdx = 0, NewlineIdx = 0;
	uint32_t Last = 0;
	// 分割候補と原文改行の昇順統合
	while(BreakIdx < BreakSize || NewlineIdx < NewlineSize) {
		const uint32_t BreakVal = BreakIdx < BreakSize ? Breaks[BreakIdx] : std::numeric_limits<uint32_t>::max();
		const uint32_t NewlineVal = NewlineIdx < NewlineSize ? Newlines[NewlineIdx] + 1 : std::numeric_limits<uint32_t>::max();
		// 次境界の選択と該当索引の前進
		uint32_t Pick;
		if(BreakVal <= NewlineVal) {
			Pick = BreakVal;
			++BreakIdx;
			if(BreakVal == NewlineVal) ++NewlineIdx;
		} else {
			Pick = NewlineVal;
			++NewlineIdx;
		}
		// 同じ位置の二重出力を防ぐ境界登録
		if(Pick != Last) {
			// 未登録境界の追加
			SplitList.push_back(Pick);
			// 重複比較用の直前位置
			Last = Pick;
		}
	}
	// ヒアドキュメントと複数行文字列リテラルの逐語範囲の一括収集
	struct HeredocInfo {
		uint32_t BodyStart; // 本文の開始位置
		uint32_t BodyEnd; // 本文の終了位置
	};
	std::vector<HeredocInfo> Heredocs;
	std::vector<std::pair<uint32_t, uint32_t>> LeafRanges;
	// 通常件数分の領域予約
	Heredocs.reserve(8);
	// 通常件数分の領域予約
	LeafRanges.reserve(16);
	// 名前付の子のみ再帰して走査量を削減
	const auto CollectVerbatim = [&Heredocs, &LeafRanges, &Source](const TSNode Node, auto &Self) -> void {
		// 再帰深さ保護
		const RecursionGuard Guard;
		if(Guard.IsOverflow) {
			Parallel::RunOnFreshStack(
				[&]() -> void {
					Self(Node, Self);
				}
			);
			// 新しい走脈で収集を終えた事の返戻
			return;
		}
		if(const std::string_view TypeView(ts_node_type(Node)); TypeView == "heredoc_body") {
			// 本文範囲の記録
			Heredocs.push_back({ ts_node_start_byte(Node), ts_node_end_byte(Node) });
			// heredoc_body 内部の逐語保持による子孫走査の省略
			return;
		} else if(NodeKind::Leaf.Contains(TypeView)) {
			// 再構築対象の型引数リストの逐語保持からの除外
			if(!NodeKind::AngleBracketList.Contains(TypeView)) {
				// 葉ノード系の内改行を跨ぐ物のバイト範囲を記録
				if(
					const uint32_t NodeStart = ts_node_start_byte(Node), NodeEnd = ts_node_end_byte(Node);
					NodeEnd > NodeStart + 1 && std::memchr(Source.data() + NodeStart, '\n', NodeEnd - NodeStart)
				) LeafRanges.push_back({ NodeStart, NodeEnd });
			}
			// 葉節点の子孫走査の省略
			return;
		}
		ForEachNamedChild(
			Node,
			[&](const TSNode Child) -> void {
				// 名前付の子孫範囲の収集
				Self(Child, Self);
			}
		);
	};
	// 空白を値として保つ範囲の収集実行
	CollectVerbatim(Root, CollectVerbatim);
	// PHP の先頭の地の文の各行の逐語保持
	uint32_t InlineHtmlHead = 0;
	if(
		const TSNode FirstChild = ts_node_named_child(Root, 0);
		!ts_node_is_null(FirstChild) && std::string_view(ts_node_type(Root)) == "program"
	) if(std::string_view(ts_node_type(FirstChild)) == "text") InlineHtmlHead = ts_node_end_byte(FirstChild);
	std::sort(
		Heredocs.begin(),
		Heredocs.end(),
		[](const HeredocInfo &Left, const HeredocInfo &Right) -> bool {
			// 本文開始位置の昇順比較
			return Left.BodyStart < Right.BodyStart;
		}
	);
	std::sort(LeafRanges.begin(), LeafRanges.end());
	std::string Out;
	// 区切り追加分を含む領域予約
	Out.reserve(Source.size() + SplitList.size());
	// 閉じ波括弧後の else 又は while の空白接続
	bool IsFirst = true, IsJoinPending = false;
	const auto EmitSeparator = [&Out, &IsFirst, &IsJoinPending]() -> void {
		// 先頭の断片の前に区切りを出さない事の返戻
		if(IsFirst) return;
		// 前の断片との接続方法の反映
		Out += IsJoinPending ? ' ' : '\n';
		// 接続待ちの消費
		IsJoinPending = false;
		// 終了
		return;
	};
	size_t HeredocCursor = 0, LeafCursor = 0, JoinCursor = 0;
	const size_t SplitCount = SplitList.size();
	const uint32_t SourceSize = static_cast<uint32_t>(Source.size());
	for(size_t SplitIdx = 0; SplitIdx < SplitCount; ++SplitIdx) {
		const uint32_t Start = SplitList[SplitIdx], End = SplitIdx + 1 < SplitCount ? SplitList[SplitIdx + 1] : SourceSize;
		// 先頭の地の文の逐語出力
		if(Start < InlineHtmlHead) {
			// 地の文より前の区切り出力
			EmitSeparator();
			Out.append(Source.data() + Start, (End > Start && Source[End - 1] == '\n' ? End - 1 : End) - Start);
			// 出力済状態への遷移
			IsFirst = false;
			continue;
		}
		// ヒアドキュメント本文と終端行の逐語保持
		while(HeredocCursor < Heredocs.size() && Heredocs[HeredocCursor].BodyEnd <= Start) ++HeredocCursor;
		if(
			const HeredocInfo *const HInfo =
			HeredocCursor < Heredocs.size() && Heredocs[HeredocCursor].BodyStart <= Start ? &Heredocs[HeredocCursor] : nullptr;
			HInfo
		) {
			// 境界改行を除く終端
			const uint32_t HEnd = End > Start && Source[End - 1] == '\n' ? End - 1 : End;
			// 本文先頭の境界改行だけの除外と先頭空行の保持
			if(Start == HInfo->BodyStart && HEnd == Start) continue;
			// ヒアドキュメント行の区切り出力
			EmitSeparator();
			// ヒアドキュメント本文のバイト列は <<~/<<-/<< の種別問わずバイト単位で完全保持
			Out.append(Source.data() + Start, HEnd - Start);
			// 出力済状態への遷移
			IsFirst = false;
			continue;
		}
		// 複数行の葉節点本文に在る行頭空白の逐語出力
		while(LeafCursor < LeafRanges.size() && LeafRanges[LeafCursor].second <= Start) ++LeafCursor;
		if(
			const std::pair<uint32_t, uint32_t> *const Leaf = LeafCursor < LeafRanges.size() && LeafRanges[LeafCursor].first < Start &&
			std::memchr(Source.data() + LeafRanges[LeafCursor].first, '\n', Start - LeafRanges[LeafCursor].first) ?
			&LeafRanges[LeafCursor] :
			nullptr;
			Leaf
		) {
			const uint32_t BodyEnd = End > Start && Source[End - 1] == '\n' ? End - 1 : End;
			// 葉本文行の区切り出力
			EmitSeparator();
			// 葉節点内に収まる断片全体の逐語保持
			if(Leaf->second >= BodyEnd) Out.append(Source.data() + Start, BodyEnd - Start);
			else {
				// 断片が葉ノード境界を跨ぐ閉じ行：葉ノード部分は逐語保持し，後続のコード部分は余分な空白の切詰
				Out.append(Source.data() + Start, Leaf->second - Start);
				// 葉の後続コードの暫定終端
				uint32_t CodeEnd = BodyEnd;
				if(LeafCursor + 1 >= LeafRanges.size() || LeafRanges[LeafCursor + 1].first >= BodyEnd) {
					while(CodeEnd > Leaf->second && (Source[CodeEnd - 1] == ' ' || Source[CodeEnd - 1] == '\t')) --CodeEnd;
				}
				// 後続コードの出力
				Out.append(Source.data() + Leaf->second, CodeEnd - Leaf->second);
			}
			// 出力済状態への遷移
			IsFirst = false;
			continue;
		}
		uint32_t TrimStart = Start, TrimEnd = End;
		while(TrimStart < TrimEnd && (Source[TrimStart] == ' ' || Source[TrimStart] == '\t')) ++TrimStart;
		// 末尾改行の検出状態
		bool HadTrailNewline = false;
		const uint32_t VerbatimFrom =
		LeafCursor < LeafRanges.size() && LeafRanges[LeafCursor].first < TrimEnd && LeafRanges[LeafCursor].second >= TrimEnd ?
		LeafRanges[LeafCursor].first :
		TrimEnd;
		while(TrimEnd > TrimStart) {
			if(Source[TrimEnd - 1] == '\n') {
				// 空行保存用の改行記録
				HadTrailNewline = true;
				// 末尾改行の切詰
				--TrimEnd;
				continue;
			}
			if(Source[TrimEnd - 1] != ' ' && Source[TrimEnd - 1] != '\t' || TrimEnd > VerbatimFrom) break;
			// 末尾空白の切詰
			--TrimEnd;
		}
		// 内容を持たない断片の空行処理
		if(TrimStart >= TrimEnd) {
			if(HadTrailNewline && !IsFirst && !IsJoinPending) Out += '\n';
			continue;
		}
		// 通常断片の区切り出力
		EmitSeparator();
		// 切詰済のコード断片の出力
		Out.append(Source.data() + TrimStart, TrimEnd - TrimStart);
		// 閉じ波括弧後の else 又は while の同行接続予約
		while(JoinCursor < JoinBraceEnds.size() && JoinBraceEnds[JoinCursor] < TrimEnd) ++JoinCursor;
		// 次断片の同行接続条件
		IsJoinPending = JoinCursor < JoinBraceEnds.size() && JoinBraceEnds[JoinCursor] == TrimEnd;
		// 出力済状態への遷移
		IsFirst = false;
	}
	// 出力テキストの返戻
	return Out;
}

/**
 * 行分割の適用関数
 * 計算量：原稿の長さ N と行幅を超える箇所の数 K に対し O(N + N * K)（分割の候補は１度の走査で集め，波括弧の付与だけが巡る）
 * @param Src 対象のソースコード
 * @param Language 対象言語
 * @param SkipReason 手間の上限超過時の見送理由格納先
 * @return 式の接続を保存出来れば true
 */
bool LineSplitPass::Apply(TSSource &Src, const Lang Language, std::string_view &SkipReason) {
	// 未解析原稿の行分割からの除外
	if(!Src.IsParsed()) return true;
	// 字下げを構文として扱う専用工程への振分
	if(Language == Lang::Python) {
		// Python 専用分割の適用
		ApplyPython(Src);
		// Python 専用工程の成功の返戻
		return true;
	}
	const bool IsOrigEndsNewline = !Src.empty() && Src.back() == '\n';
	// 統合変数宣言の MaxChars 超過を別宣言へグリーディ分割（汎用 break 機構より前に確定させ，カンマ継続分割を抑止する）
	if(MaxChars) SplitWideVarDecls(Src, Language);
	// 宣言の分割で解析に失敗した場合の返戻
	if(!Src.IsParsed()) return true;
	const std::vector<std::pair<std::string_view, std::string_view>> OldShape =
	Language == Lang::Ruby ? StructurePass::RubyExpressionShape(Src) : std::vector<std::pair<std::string_view, std::string_view>>{};
	// 行分割・原改行・後続節結合の位置管理
	std::vector<uint32_t> Breaks, Newlines, JoinBraceEnds;
	// 波括弧と分割境界の確定
	if(!ApplyBraceWraps(Src, Language, Breaks, Newlines, JoinBraceEnds)) {
		// 行分割の手間上限超過の通知
		SkipReason = "line wrapping exceeds formatting limit";
		// 未完了の行分割を適用しない事の返戻
		return false;
	}
	// 波括弧の付与で解析に失敗した場合の返戻
	if(!Src.IsParsed()) return true;
	// Ruby の１行の分岐を行分割が割るなら，節境界で改行して割り位置を求め直す
	while(MaxChars && Language == Lang::Ruby && ExpandWideRubyBranches(Src, Breaks, Newlines)) {
		// 節境界の分割で解析に失敗した場合の返戻
		if(!Src.IsParsed()) return true;
		// 展開後の境界再計算
		if(!ApplyBraceWraps(Src, Language, Breaks, Newlines, JoinBraceEnds)) {
			// 行分割の手間上限超過の通知
			SkipReason = "line wrapping exceeds formatting limit";
			// 未完了の行分割を適用しない事の返戻
			return false;
		}
		// 波括弧の付与で解析に失敗した場合の返戻
		if(!Src.IsParsed()) return true;
	}
	std::vector<uint32_t> BraceBreaks;
	WalkChildrenCursor(
		Src.GetRoot(),
		[&](const TSNode Node) -> bool {
			// 文字列化マクロの実引数は既存の波括弧内改行も含めて字面を保つ事の返戻
			if(Language.IsCFamily() && Src.StringizesArguments(Node)) return false;
			const uint32_t NamedCount = ts_node_named_child_count(Node);
			const TSNode Opening = NamedCount ? ts_node_named_child(Node, 0) : TSNode{};
			const TSNode Content = NamedCount == 3 ? ts_node_named_child(Node, 1) : TSNode{};
			const std::string_view ContentText = ts_node_is_null(Content) ? std::string_view{} : Src.View(Content);
			const bool HasSingleInlineText =
			!ContentText.empty() && std::string_view(ts_node_type(Content)) == "jsx_text" && ContentText.find_first_of(" \t\n\r") != 0 &&
			ContentText.find_last_of(" \t\n\r") != ContentText.size() - 1 && !ContentText.starts_with(Src.GetJsxBlankMark(' ')) &&
			!ContentText.starts_with(Src.GetJsxBlankMark('\t')) && !ContentText.ends_with(Src.GetJsxBlankMark(' ')) &&
			!ContentText.ends_with(Src.GetJsxBlankMark('\t'));
			if(Language.IsJsTs() && std::string_view(ts_node_type(Node)) == "jsx_element" && HasSingleInlineText) {
				// 属性の折返しだけで開始タグと単一本文の境界を割らない
				const std::vector<uint32_t>::iterator Break = std::lower_bound(Breaks.begin(), Breaks.end(), Src.End(Opening));
				if(Break != Breaks.end() && *Break == Src.End(Opening)) Breaks.erase(Break);
			} else if(Language.IsJsTs() && std::string_view(ts_node_type(Node)) == "jsx_element" && NamedCount > 2) {
				// 閉じタグ前端
				const uint32_t Tail = TextEdit::SkipSpLeft(Src, Src.Start(ts_node_named_child(Node, NamedCount - 1)));
				if(
					const std::vector<uint32_t>::const_iterator Break = std::lower_bound(Breaks.begin(), Breaks.end(), Src.Start(Node) + 1);
					Break != Breaks.end() && *Break < Tail && !TSSource::HasDisputedJsxText(Src, Node)
				) {
					// 開始タグ直後の境界
					BraceBreaks.push_back(Src.End(ts_node_named_child(Node, 0)));
					// 閉じタグ直前の境界
					BraceBreaks.push_back(Tail);
				}
			}
			uint32_t Open = 0;
			bool HasOpen = false;
			ForEachChild(
				Node,
				[&](const TSNode Child) -> void {
					// 条件成立時の返戻
					if(ts_node_is_named(Child)) return;
					if(const std::string_view Token = Src.View(Child); Token == "{") {
						// 対応範囲の開始位置
						Open = Src.End(Child);
						// 閉じ波括弧待ちへの遷移
						HasOpen = true;
					} else if(Token == "}" && HasOpen) {
						// 対応範囲の終了位置
						const uint32_t Close = Src.Start(Child);
						const std::vector<uint32_t>::const_iterator Break = std::lower_bound(Breaks.begin(), Breaks.end(), Open);
						if(std::memchr(Src.data() + Open, '\n', Close - Open) || Break != Breaks.end() && *Break <= Close) {
							// 閉じ波括弧直前の境界
							BraceBreaks.push_back(Close);
							if(
								const uint32_t Next = TextEdit::SkipSpRight(Src, Src.End(Child));
								Next < Src.size() && Src[Next] != '\n' &&
								StructurePass::ShouldBreakAfterExpandedBrace("\n}", std::string_view(Src).substr(Next), Language)
							) BraceBreaks.push_back(Next);
						}
						// 対応完了による待ち解除
						HasOpen = false;
					}
				}
			);
			// 文字列内容へ降りずに式とブロックを走査するかの返戻
			return !NodeKind::Leaf.Contains(Node);
		}
	);
	// 構造上必要な分割境界の統合
	if(!BraceBreaks.empty()) {
		// 追加境界の統合
		Breaks.insert(Breaks.end(), BraceBreaks.begin(), BraceBreaks.end());
		std::sort(Breaks.begin(), Breaks.end());
		// 重複境界の除去
		Breaks.erase(std::unique(Breaks.begin(), Breaks.end()), Breaks.end());
	}
	// 構文上の閉じ波括弧と else 又は while の同行化
	if(!MaxChars && Language != Lang::Ruby) CollectElseWhileJoins(Src, Language, JoinBraceEnds);
	// 確定した境界に基付く原稿の再構築
	std::string Result = BuildResult(Src, Breaks, Newlines, Src.GetRoot(), JoinBraceEnds);
	// 入力の末尾改行の復元
	if(!Result.empty() && IsOrigEndsNewline && Result.back() != '\n') Result += '\n';
	// 再構築した本文と構文木の差替
	Src.Assign(std::move(Result));
	// Ruby の式接続を変える場合の失敗返戻，Go の式内で不正改行が生じる場合の失敗返戻
	if(
		Language == Lang::Ruby && OldShape != StructurePass::RubyExpressionShape(Src) ||
		Language == Lang::Go && StructurePass::HasBrokenGoContinuation(Src)
		// 式の接続変化に依る失敗の返戻
	) return false;
	// 意味を保持した行分割の成功の返戻
	return true;
}
