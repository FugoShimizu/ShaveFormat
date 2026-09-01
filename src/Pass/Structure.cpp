#include "Structure.hpp"
#include "../Util/BracketPairs.hpp"
#include "../Util/HtmlTag.hpp"
#include "../Util/NodeKind.hpp"
#include "../Util/Parallel.hpp"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

/** ========== 構造前処理 ========== */
/**
 * 括弧内継続行平坦化関数（括弧内の非意味的改行のみを畳む）
 * @param Src 整形対象のソース（結果で上書きされる）
 * @param Language 対象言語（逐語保護の言語固有分を切り替える）
 */
void StructurePass::FlattenBracketContinuations(TSSource &Src, const Lang Language) {
	// 条件成立時の返戻
	if(!Src.IsParsed()) return;
	// Ruby 規則の適用有無
	const bool IsRuby = Language == Lang::Ruby;
	const std::string &Source = Src;
	std::vector<std::pair<uint32_t, uint32_t>> Verbatim, Terminators;
	std::vector<std::pair<uint32_t, int>> Brackets;
	const auto AddTerminator = [&Source, &Terminators](const uint32_t From, const uint32_t To) -> void {
		if(To > From && std::memchr(Source.data() + From, '\n', To - From)) Terminators.emplace_back(From, To);
	};
	// 構文木の走査位置
	HeldCursor Cursor(Src.GetRoot());
	bool IsDone = false;
	while(!IsDone) {
		// 対象節点
		const TSNode Node = ts_tree_cursor_current_node(&Cursor);
		const std::string_view Type(ts_node_type(Node));
		bool ShouldDescend = true;
		// Ruby の区切り文字付リテラルは複数字句の開きを持つ為，内部へ走査除外
		if(NodeKind::StringLikeInnerPreserve.Contains(Type) || IsRuby && NodeKind::RubyDelimitedLiteral.Contains(Type)) {
			// 担当範囲の終端索引
			uint32_t End = ts_node_end_byte(Node);
			// ヒアドキュメントの終端タグは独自の行を要する為，直後の改行も畳まずに保持
			if(Type == "heredoc_body" && End < Source.size() && Source[End] == '\n') ++End;
			// 逐語保持する範囲列への追加
			Verbatim.emplace_back(ts_node_start_byte(Node), End);
			// 文字列とヒアドキュメント内部の走査除外に依る括弧・改行の保持
			ShouldDescend = false;
		} else if(IsRuby && NodeKind::RubyTerminatorHost.Contains(Type)) {
			uint32_t GapStart = ts_node_start_byte(Node);
			bool IsAfterOpen = false;
			ForEachChild(
				Node,
				[&](const TSNode Child) -> void {
					if(!IsAfterOpen && (ts_node_is_named(Child) || Src.View(Child) != ")")) AddTerminator(GapStart, ts_node_start_byte(Child));
					// 開始括弧直後の状態の更新
					IsAfterOpen = !ts_node_is_named(Child) && Src.View(Child) == "(";
					// 次の間隙開始位置の更新
					GapStart = ts_node_end_byte(Child);
				}
			);
			AddTerminator(GapStart, ts_node_end_byte(Node));
		} else if(!ts_node_child_count(Node)) {
			BracketPairs::AppendBracketDelta(Brackets, Source, ts_node_start_byte(Node), ts_node_end_byte(Node));
		}
		if(ShouldDescend && ts_tree_cursor_goto_first_child(&Cursor)) continue;
		while(true) {
			if(ts_tree_cursor_goto_next_sibling(&Cursor)) break;
			if(!ts_tree_cursor_goto_parent(&Cursor)) {
				// 構文木走査の完了状態の設定
				IsDone = true;
				break;
			}
		}
	}
	// 括弧位置と深さの変化列の位置順への整列
	std::sort(Brackets.begin(), Brackets.end());
	std::sort(Verbatim.begin(), Verbatim.end());
	std::sort(Terminators.begin(), Terminators.end());
	const auto IsInVerbatim = [&Verbatim](const uint32_t Pos) -> bool {
		const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Iter = std::upper_bound(
			Verbatim.begin(),
			Verbatim.end(),
			Pos,
			[](const uint32_t Value, const std::pair<uint32_t, uint32_t> &Range) -> bool {
				return Value < Range.first;
			}
		);
		// 直前候補 (first <= Pos) の second を超えなければ範囲内の返戻
		return Iter != Verbatim.begin() && Pos < (Iter - 1)->second;
	};
	// Ruby の複数行文字列を含む最外括弧の範囲を保護対象への限定
	std::vector<std::pair<uint32_t, uint32_t>> ProtectedRanges;
	if(IsRuby) {
		// 最外括弧からの深さ
		int OuterDepth = 0;
		uint32_t OpenPos = 0;
		for(const std::pair<uint32_t, int> &Bracket : Brackets) {
			if(!OuterDepth && Bracket.second == 1) OpenPos = Bracket.first;
			// 現在の括弧深さの進行
			OuterDepth += Bracket.second;
			if(!OuterDepth && Bracket.second == -1 && OpenPos < Source.size()) {
				// 括弧範囲の開始文字
				const char Open = Source[OpenPos];
				bool ShouldProtect = false;
				if(Open == '[' || Open == '{') {
					for(
						std::vector<std::pair<uint32_t, uint32_t>>::const_iterator VerbIter = std::lower_bound(
							Verbatim.begin(),
							Verbatim.end(),
							OpenPos,
							[](const std::pair<uint32_t, uint32_t> &Range, const uint32_t Value) -> bool {
								return Range.first < Value;
							}
						);
						VerbIter != Verbatim.end() && VerbIter->first < Bracket.first;
						++VerbIter
					) {
						if(
							VerbIter->second <= Bracket.first && std::memchr(Source.data() + VerbIter->first, '\n', VerbIter->second - VerbIter->first)
						) {
							// 括弧範囲の保護要否の設定
							ShouldProtect = true;
							break;
						}
					}
				}
				if(ShouldProtect) ProtectedRanges.emplace_back(OpenPos, Bracket.first);
			}
		}
	}
	// 適用予定の編集列
	std::vector<TextEdit> Edits;
	const uint32_t Size = static_cast<uint32_t>(Source.size());
	size_t BracketIdx = 0, ProtectedIdx = 0, TerminatorIdx = 0;
	int Depth = 0;
	for(uint32_t Pos = 0; Pos < Size; ++Pos) {
		// 当該位置より前の括弧トークンで括弧深さを更新する（Pos 単調増加・BracketIdx 単調進行）
		while(BracketIdx < Brackets.size() && Brackets[BracketIdx].first < Pos) Depth += Brackets[BracketIdx++].second;
		// 改行以外・括弧外・逐語範囲内の保持
		if(Source[Pos] != '\n' || !Depth || IsInVerbatim(Pos)) continue;
		// Ruby の文・見出を終える改行は括弧の中でも保持
		while(TerminatorIdx < Terminators.size() && Terminators[TerminatorIdx].second <= Pos) ++TerminatorIdx;
		if(TerminatorIdx < Terminators.size() && Terminators[TerminatorIdx].first <= Pos) continue;
		// 複数行文字列を含む最外括弧式の範囲内の改行は畳まない（先行コメントの文字列内混入を防止）
		while(ProtectedIdx < ProtectedRanges.size() && ProtectedRanges[ProtectedIdx].second < Pos) ++ProtectedIdx;
		if(ProtectedIdx < ProtectedRanges.size() && ProtectedRanges[ProtectedIdx].first <= Pos) continue;
		uint32_t Left = Pos;
		while(Left && (Source[Left - 1] == ' ' || Source[Left - 1] == '\t' || Source[Left - 1] == '\r')) --Left;
		// 括弧内の行継続バックスラッシュは冗長な為，改行と共に畳んで除去する（`"a" \<改行> "b"` → `"a" "b"`，残すと構文破壊）
		if(Left && Source[Left - 1] == '\\') {
			// 空白範囲の左端位置の後退
			--Left;
			while(Left && (Source[Left - 1] == ' ' || Source[Left - 1] == '\t')) --Left;
		}
		// 空白範囲の右端位置
		uint32_t Right = Pos + 1;
		while(Right < Size && (Source[Right] == ' ' || Source[Right] == '\t' || Source[Right] == '\n' || Source[Right] == '\r')) {
			// 右端の節点の進行
			++Right;
		}
		// 原文編集列への置換登録
		TextEdit::Push(Left, Right, " ", Edits);
		// 次の反復で範囲外へ進む様に，畳んだ範囲の末尾へ移す
		Pos = Right - 1;
	}
	// 登録済編集の原文への適用
	TextEdit::Apply(Src, Edits);
	// 終了
	return;
}

/** ========== 字句間隙 ========== */
/**
 * 原文の儘写す葉の判定関数
 * PHP の名前空間だけ空白を禁止し，C# の型名と区別する
 * @param Type ノード型
 * @param Language 対象言語
 * @return 原文の儘写す葉なら true
 */
bool StructurePass::IsVerbatimLeaf(const std::string_view Type, const Lang Language) {
	// 葉の集合に属し，PHP 以外の型の名前でない事の返戻
	return NodeKind::Leaf.Contains(Type) && (Language == Lang::PHP || Type != "qualified_name");
}

/**
 * 山括弧誤解析の一括判定関数（C++ の `b < c && d > e`・C# / TS の `a < b, c >> d` が型引数リストへ化けた形かを構造で見分ける）
 * @param Src ソースコード
 * @param Language 対象言語
 */
void StructurePass::PrepareTemplateContexts(const TSSource &Src, const Lang Language) {
	// 節点別の判定控え
	std::unordered_map<const void *, uint8_t> &Memo = Src.GetContextMemo();
	// 型引数を誤読し得ない言語，控え済か，型引数リストを持ち得ない（`<` の無い）ソースの終了
	if(
		!(Language.IsCFamily() || Language == Lang::CSharp || Language == Lang::TypeScript) || !Memo.empty() ||
		Src.find('<') == std::string::npos
		// 条件成立時の返戻
	) return;
	// 構文木の根節点
	const TSNode Root = Src.GetRoot();
	// 根節点の控え済印との兼用
	Memo[Root.id] = 0;
	const auto Misparsed = [](
		const TSNode List,
		const TSNode Template,
		const TSNode After,
		const std::string_view Outer,
		const bool IsExpression
	) -> bool {
		// 深さ三乗を避ける構文木の単一先行順走査
		if(!ts_node_is_null(After) && !ts_node_is_named(After)) {
			// 後続節点の型名
			const std::string_view AfterType = ts_node_type(After);
			// 解析器が型引数の閉じへ割った >>・>= 等は元字句を保ち，正当なネストの >> は二項式の外なので除外
			if(
				ts_node_end_byte(Template) == ts_node_start_byte(After) && (AfterType.starts_with('>') || AfterType == "=") &&
				(Outer == "binary_expression" || NodeKind::AssignmentExpression.Contains(Outer))
				// 字句を割った誤解析である事の返戻
			) return true;
			// 閉じの後に `::` が続く型引数は，比較の被演算子に為れない字句が続く為に正当なテンプレートである事の返戻
			if(AfterType == "::") return false;
		}
		if(!ts_node_is_null(After) && std::string_view(ts_node_type(After)) == "argument_list" && !ts_node_named_child_count(After)) {
			// 空の実引数が閉じの後に続く型引数も正当と認定
			return false;
		}
		// 括弧内の節点
		const TSNode Inner = ts_node_named_child(List, 0);
		// 宣言子側の実体化は正当なテンプレート，初期化子側の二項式は誤解析として区別する（f<N + 1> と b < c && d >> e）
		return !ts_node_is_null(Inner) && std::string_view(ts_node_type(Inner)) == "binary_expression" && IsExpression;
	};
	// 実体化の節点の最初の型引数リスト（無ければ空）
	const auto ListOf = [](const TSNode Template) -> TSNode {
		// 型引数リストの節点
		TSNode List {};
		ForEachNamedChild(
			Template,
			[&List](const TSNode Child) -> bool {
				// 山括弧の並び以外を読み飛ばす事の返戻
				if(!NodeKind::AngleBracketList.Contains(Child)) return true;
				// 型引数リスト節点の確定
				List = Child;
				// 型引数リストを発見した為，走査打切の返戻
				return false;
			}
		);
		// 型引数リストの返戻
		return List;
	};
	// 巡る節点・其の後に続く字句・親の型・其の中が式の側か
	struct Frame {
		TSNode Node; // 対象節点
		TSNode After; // 直後の節点
		std::string_view Outer; // 対象を包む節点
		bool IsExpression; // 式側の文脈か
	};
	std::vector<Frame> Frames { { Root, TSNode{}, std::string_view(), true } };
	std::vector<TSNode> Kids;
	while(!Frames.empty()) {
		// 現在処理する走査枠
		const Frame Top = Frames.back();
		// 構文木走査の作業列からの末尾要素取出
		Frames.pop_back();
		// 節点の型名
		const std::string_view Type = ts_node_type(Top.Node);
		// 走査用の子節点列の初期化
		Kids.clear();
		ForEachChild(
			Top.Node,
			[&Kids](const TSNode Child) -> void {
				// 走査用の子節点列への追加
				Kids.push_back(Child);
			}
		);
		// 子の文脈：初期化子を持つ宣言子の初期化子は式の側
		const TSNode Value = Type == "init_declarator" ? TSSource::FieldChild(Top.Node, "value") : TSNode{};
		const bool IsHost = NodeKind::MisparsedTemplateHost.Contains(Type);
		for(size_t Idx = 0; Idx < Kids.size(); ++Idx) {
			// 走査中の子節点
			const TSNode Child = Kids[Idx], After = Idx + 1 < Kids.size() ? Kids[Idx + 1] : TSNode{};
			const bool IsExpression = Type == "init_declarator" ?
			ts_node_eq(Child, Value) :
			Type != "declaration" && (Top.IsExpression || NodeKind::CppScopeBoundary.Contains(Type));
			if(const std::string_view ChildType = ts_node_type(Child); NodeKind::MisparsedTemplateHost.Contains(ChildType)) {
				// 実体化の節点と其の型引数リストは同一判定
				if(const TSNode List = ListOf(Child); !ts_node_is_null(List)) {
					// 節点別の判定控えの確定
					Memo[Child.id] = Memo[List.id] = Misparsed(List, Child, After, Type, IsExpression);
				}
			} else if(!IsHost && NodeKind::AngleBracketList.Contains(ChildType)) {
				// 節点別の判定控えの確定
				Memo[Child.id] = Misparsed(Child, Top.Node, Top.After, Top.Outer, IsExpression);
			}
			if(ts_node_child_count(Child)) Frames.push_back({ Child, After, Type, IsExpression });
		}
	}
	// 終了
	return;
}

/**
 * 山括弧誤解析判定関数
 * @param Src ソースコード（PrepareTemplateContexts が判定を控えた物）
 * @param Node 実体化の節点又は型引数リスト
 * @return 誤解析なら true（型引数リストが二項式を直に持つ形，型・リテラルを取る正当な実体化は false）
 */
bool StructurePass::IsMisparsedTemplate(const TSSource &Src, const TSNode Node) {
	// 節点別の判定控え
	const std::unordered_map<const void *, uint8_t> &Memo = Src.GetContextMemo();
	const std::unordered_map<const void *, uint8_t>::const_iterator Hit = Memo.find(Node.id);
	// 控えた判定（型引数リストを持たない節点は誤解析でない）の返戻
	return Hit != Memo.end() && Hit->second;
}

/**
 * 無名トークン種別照合関数
 * @param IsNamed トークンが名前付か
 * @param Type トークンの種別
 * @param Toks 照合する種別の列挙集合
 * @return 無名トークンで種別が列挙集合内に在れば true
 */
bool StructurePass::IsUnnamedToken(
	const bool IsNamed,
	const std::string_view Type,
	const std::initializer_list<std::string_view> Toks
) {
	// 無名の字句で種別が何れかに一致するかの返戻
	return !IsNamed && std::find(Toks.begin(), Toks.end(), Type) != Toks.end();
}

/**
 * 開いた範囲演算子で終わるかの判定関数
 * @param Node 判定対象のノード
 * @return 最後の字句が範囲演算子 `..` / `...` なら true
 */
bool StructurePass::EndsWithOpenRange(TSNode Node) {
	// 末尾子を辿った最終字句の取得
	while(!ts_node_is_null(Node) && ts_node_child_count(Node)) Node = ts_node_child(Node, ts_node_child_count(Node) - 1);
	// 最後の字句が範囲演算子かの返戻
	return !ts_node_is_null(Node) && NodeKind::RangeOperator.Contains(Node);
}

/**
 * `(` 始まりの型の判定関数
 * @param Node 判定対象のノード
 * @return 先頭の `(` が型の括弧（組型・関数型・DNF 型等）なら true
 */
bool StructurePass::StartsWithParenType(TSNode Node) {
	// 先頭の子を辿り，`(` を開く型に行着くかを見る（配列型 `(int, int)[]` 等は先頭子の組型が `(` を開始）
	for(; !ts_node_is_null(Node) && ts_node_is_named(Node); Node = ts_node_child(Node, 0)) {
		// `(` を開く型に行着いた場合の返戻
		if(NodeKind::ParenLeadType.Contains(Node)) return true;
	}
	// 無名の字句に行着いた場合の返戻
	return false;
}

/**
 * C# の宣言式と誤読した乗算かの判定関数
 * C# 解析器が実引数の乗算を宣言式と誤読したかを判定する
 * @param Src ソースコード
 * @param PointerType 宣言式の型に読まれたポインタ型
 * @return 乗算と読む位置なら true
 */
bool StructurePass::ReadsAsCsProduct(const TSSource &Src, const TSNode PointerType) {
	// 宣言式の節点
	const TSNode Declaration = ts_node_parent(PointerType);
	const TSNode Argument = ts_node_parent(Declaration);
	if(
		std::string_view(ts_node_type(Declaration)) != "declaration_expression" ||
		std::string_view(ts_node_type(Argument)) != "argument" || HasUnnamedTokenChild(Src, Argument, "out")
		// 実引数の宣言式でない又は `out` の実引数の宣言は，本当の宣言の返戻
	) return false;
	// 組のネストを外側へ遡り，分解の代入の左辺と foreach の分解の変数は宣言と解釈
	TSNode Tuple = ts_node_parent(Argument);
	while(std::string_view(ts_node_type(Tuple)) == "tuple_expression") {
		// 対象を保持する節点
		const TSNode Host = ts_node_parent(Tuple);
		if(std::string_view(ts_node_type(Host)) == "argument") {
			// タプル候補の節点の確定
			Tuple = ts_node_parent(Host);
			continue;
		}
		// 分解の代入の左辺・foreach の分解の変数でなければ乗算の返戻
		return !(
			std::string_view(ts_node_type(Host)) == "assignment_expression" && ts_node_eq(TSSource::FieldChild(Host, "left"), Tuple) ||
			std::string_view(ts_node_type(Host)) == "foreach_statement"
		);
	}
	// 呼出の実引数は乗算の返戻
	return true;
}

/**
 * 兄弟ノード間スペース有無決定関数
 * @param Src ソースコード
 * @param Parent 親ノード
 * @param ParentType 親ノードの種別
 * @param Cur 現在の子ノード
 * @param CurType 現在の子ノードの種別
 * @param Next 現在の子の直後の子ノード
 * @param NextType 直後の子ノードの種別
 * @param CurIndex Cur の親内インデックス
 * @param Language 対象言語
 * @return 間にスペースを挿入すべき場合 true
 */
bool StructurePass::NeedsGapBetween(
	const TSSource &Src,
	const TSNode Parent,
	const std::string_view ParentType,
	const TSNode Cur,
	const std::string_view CurType,
	const TSNode Next,
	const std::string_view NextType,
	const uint32_t CurIndex,
	const Lang Language
) {
	// Kotlin の注釈と CSS/SCSS の単一名断片は密着し，複数注釈を並べる角括弧形だけは名前の融合を防ぐ為に空白を保持
	if(
		NodeKind::KotlinAnnotationLike.Contains(ParentType) && !(CurType == "user_type" && NextType == "user_type") ||
		Language == Lang::CSS && NodeKind::CssNameFragmentHost.Contains(ParentType)
		// 不可分の名前を密着させる事の返戻
	) return false;
	// 現在節点が名前付か
	const bool IsCurNamed = ts_node_is_named(Cur), IsNextNamed = ts_node_is_named(Next);
	const uint32_t CurEnd = Src.End(Cur), NextStart = Src.Start(Next);
	const char PrevChar = CurEnd ? Src[CurEnd - 1] : '\0', NextChar = NextStart < Src.size() ? Src[NextStart] : '\0';
	// Swift の後置演算子は原文の隣接を保持
	if(Language == Lang::Swift && NodeKind::InfixOp.Contains(ParentType)) {
		const auto AttachedMark = [&Src](const TSNode Operator) -> bool {
			// 演算子の開始位置
			const uint32_t OperatorStart = Src.Start(Operator);
			// 左の字へ空白無で接する `?` `!` で始まる字句かの返戻
			return OperatorStart && (Src[OperatorStart] == '?' || Src[OperatorStart] == '!') &&
			!std::isspace(static_cast<unsigned char>(Src[OperatorStart - 1]));
		};
		// 原文の隣接の返戻
		if(AttachedMark(Next) || CurIndex && AttachedMark(Cur)) return CurEnd != NextStart;
	}
	// C# の分解の宣言 (`var (m, n) = t`) の `var` と組の形は，呼出でない為に空白で区切る事の返戻
	if(Language == Lang::CSharp && CurType == "implicit_type" && NextChar == '(') return true;
	// PHP の地の文と PHP の字句の間は空白で区切る (`<?= $x ?>` / `: ?>` / `<?php endforeach`)
	if(
		Language == Lang::PHP && (
			CurType == "text_interpolation" || NextType == "text_interpolation" ||
			NextChar == ':' && NodeKind::PhpAltHost.Contains(NextType)
		)
		// 地の文の境界の空白要，本体の `:` の密着の返戻
	) return NextChar != ':';
	// 型引数直後の比較演算子誤分割に対する原文空白へ依存しない出力
	if(Language.IsCFamily() || Language == Lang::CSharp || Language == Lang::TypeScript) {
		// 誤解析で割れた比較演算子は元は１トークンの為密着
		if(!IsNextNamed && (NextChar == '=' || NextChar == '>')) {
			// 正当な型引数の後だけ空白を許可する事の返戻
			if(NodeKind::MisparsedTemplateHost.Contains(CurType)) return !IsMisparsedTemplate(Src, Cur);
			TSNode Right = Cur;
			while(!ts_node_is_null(Right) && !NodeKind::MisparsedTemplateHost.Contains(Right)) {
				// 対象要素の個数
				const uint32_t Count = ts_node_child_count(Right);
				// 右端の節点の確定
				Right = Count ? ts_node_child(Right, Count - 1) : TSNode{};
			}
			// 右端に包んだ誤読の型引数の後の割れた演算子を密着させる事の返戻
			if(!ts_node_is_null(Right) && IsMisparsedTemplate(Src, Right)) return false;
		}
		// 同じ誤解析の左側は本来の比較演算子で，常に空白を配置
		if(
			(
				NodeKind::MisparsedTemplateHost.Contains(ParentType) && NodeKind::AngleBracketList.Contains(NextType) ||
				NodeKind::AngleBracketList.Contains(ParentType)
			) && IsMisparsedTemplate(Src, Parent) ||
			ParentType == "call_expression" && CurType == "template_function" && NextType == "argument_list" &&
			IsMisparsedTemplate(Src, Cur)
			// 比較演算子を空白で区切り，コンマを前へ密着させる事の返戻
		) return NextType != ",";
	}
	// 無名トークン種別が列挙集合内かの集約判定
	const auto CurIs = [&](const std::initializer_list<std::string_view> Toks) -> bool {
		// 現トークン側の判定を共通核へ委譲して返戻
		return IsUnnamedToken(IsCurNamed, CurType, Toks);
	};
	const auto NextIs = [&](const std::initializer_list<std::string_view> Toks) -> bool {
		// 次トークン側の判定を共通核へ委譲して返戻
		return IsUnnamedToken(IsNextNamed, NextType, Toks);
	};
	// Ruby の `not` の後の括弧は，空白を挟まなければ呼出に似た一次式
	if(Language == Lang::Ruby && ParentType == "unary" && CurIs({ "not" }) && NextChar == '(') return CurEnd != NextStart;
	// 密着対象の次種別（コロン・ドット・三項・任意連鎖等）かの判定
	const auto NextAttachColonLike = [&]() -> bool {
		// TS の後置任意マーカーの直前字句への密着
		if(NodeKind::OptionalMarker.Contains(NextType)) {
			// 省略可能型の前に空白を置く事の返戻
			if(NextType == "optional_type") return false;
			// 先頭の子節点
			const TSNode FirstChild = ts_node_child(Next, 0);
			// 後置形の任意マーカーならば true の返戻
			return ts_node_is_null(FirstChild) || Src.View(FirstChild) != "?";
		}
		// 密着対象の次種別ならば true の返戻
		return NodeKind::AttachColonOrDot.Contains(NextType) ||
		NodeKind::Skip.Contains(NextType) && (NextChar == ':' || NextChar == '?') ||
		NodeKind::NormColon.Contains(NextType) && NextChar == ':';
	};
	// Ruby・Python の密着構文
	if(
		!IsCurNamed && (
			Language == Lang::Ruby && (NodeKind::HashSplat.Contains(ParentType) || CurType == "defined?" && NextChar == '(') ||
			Language == Lang::Python && CurType == "type" && ParentType == "type_alias_statement" && NextChar == '('
		) || !PrevChar || !NextChar || CurType == "ERROR" || NextType == "ERROR" ||
		Language == Lang::HTML && (CurIs({ "<", "</", "/", ">", "/>" }) || NextIs({ ">" })) || Language == Lang::PHP && (
			NodeKind::TypeCombination.Contains(ParentType) || CurType == "reference_modifier" ||
			NodeKind::PhpNamespaceUse.Contains(ParentType) && (CurIs({ "\\", "{" }) || NextIs({ "\\", "{", "}" }))
		)
		// 型・名前空間の記号を密着させる事の返戻
	) return false;
	// CSS/SCSS の結合子セレクタのオペランド境界は常に空白で区切られる別要素の為，一律で空白を挿入
	if(NodeKind::CombinatorSelector.Contains(ParentType)) return true;
	// JSX の子の間で描画される空白は jsx_text として出現
	if(NodeKind::JsxContainer.Contains(ParentType)) return false;
	// CSS/SCSS の単位断片と減算らしい値列は原文の隣接を保持
	if(
		Language == Lang::CSS && CurEnd == NextStart && (NextType == "plain_value" || NodeKind::CssNumericValue.Contains(NextType))
	) {
		// 参照型末尾の節点
		TSNode Tail = Cur;
		while(!NodeKind::CssNumericValue.Contains(Tail) && ts_node_named_child_count(Tail)) {
			// 末尾の参照型節点の確定
			Tail = ts_node_named_child(Tail, ts_node_named_child_count(Tail) - 1);
		}
		// 数値と単位の接続を保つ事の返戻
		if(NodeKind::CssNumericValue.Contains(Tail)) return false;
	}
	// 空文を直前の字句へ密着させる事の返戻 (`if(c);` / `else;`)
	if(IsNextNamed && NextChar == ';' && !(NextType == "enum_body_declarations" && CurIs({ "{" }))) return false;
	// ラムダキャプチャの `[` と接頭辞 `&` `*`(`*this`) の後，`]` `,` の前は密着させ，其れ以外は空白を置く事の返戻
	if(ParentType == "lambda_capture_specifier") return !CurIs({ "[", "&", "*" }) && !NextIs({ "]", "," });
	// Kotlin の制御本体・ラベル・注釈と後続式の空白区切り
	if(
		Language == Lang::Kotlin &&
		(NextType == "control_structure_body" || ParentType == "prefix_expression" && NodeKind::LabelNode.Contains(CurType))
		// 本体・被修飾式との区切りの返戻
	) return true;
	// Kotlin・Swift・CSS・C++・JS/TS・C#・Java の列挙した複合字句は言語別の不可分トークンとして密着を保持
	if(
		// Kotlin の this@O・super<A>，C# の this[int]，Rust が分割した ..= は空白で構文が変わる為密着を保持
		Language == Lang::Kotlin && (
			NodeKind::KotlinNegatableTest.Contains(ParentType) && CurType == "!" && NodeKind::KotlinNotIsIn.Contains(NextType) ||
			NodeKind::KotlinNumericLiteralSuffix.Contains(ParentType)
		) || Language == Lang::Ruby && NodeKind::RubyNumericLiteralSuffix.Contains(ParentType) ||
		Language == Lang::Swift && NodeKind::RangeOrUnaryExpression.Contains(ParentType) ||
		Language == Lang::CSS && (CurType == "%" && IsNextNamed || NodeKind::CssSelectorAtomic.Contains(ParentType)) || IsNextNamed && (
			ParentType == "variadic_type_parameter_declaration" && CurType == "..." ||
			NodeKind::GeneratorStarHost.Contains(ParentType) && CurType == "*"
		) || NodeKind::NewParenCall.Contains(ParentType) && CurType == "new" && NextChar == '(' ||
		ParentType == "function_type" && (CurType == "." || NextType == ".") || ParentType == "spread_parameter" && NextType == "..." ||
		ParentType == "jump_expression" && NextType == "label" || ParentType == "this_expression" && CurType == "this@" ||
		ParentType == "super_expression" && (CurIs({ "super", "super@", "<", ">", "@" }) || NextIs({ "<", ">", "@" })) ||
		ParentType == "indexer_declaration" && CurType == "this" && NextType == "bracketed_parameter_list" ||
		// C# の型を省いた配列の生成 `new[] { 1 }` は `new` と `[` を密着
		ParentType == "implicit_array_creation_expression" && CurType == "new" ||
		// 後置の `!`は前の名前付要素と後続の型注釈へ密着
		(Language == Lang::Swift || Language == Lang::TypeScript) &&
		(IsCurNamed && NextIs({ "!" }) || CurIs({ "!" }) && NextType == "type_annotation") ||
		Language == Lang::Rust && ParentType == "assignment_expression" &&
		(NextIs({ "=" }) && EndsWithOpenRange(Cur) || CurIs({ "=" }) && EndsWithOpenRange(ts_node_named_child(Parent, 0)))
		// 言語固有の不可分トークンを密着させる事の返戻
	) return false;
	// HTML 属性間と型を始める括弧の前には空白を配置
	if(
		Language == Lang::HTML && ParentType == "start_tag" && NextType == "attribute" || NextChar == '(' && (
			ParentType == "singleton_method" && CurType == "def" ||
			(IsCurNamed || std::isalpha(static_cast<unsigned char>(PrevChar))) && StartsWithParenType(Next)
		) || NodeKind::CastExpr.Contains(ParentType) && (CurIs({ "as" }) || NextIs({ "as" }))
		// 属性・構文要素を空白で区切る事の返戻
	) return true;
	if(
		ParentType == "attribute_declaration" && (CurIs({ "[[" }) || NextIs({ "]]" })) || NodeKind::CastExpr.Contains(ParentType) ||
		ParentType == "template_string" || ParentType == "template_substitution" && (CurIs({ "${" }) || NextIs({ "}" })) ||
		ParentType == "block_argument" && CurIs({ "&" }) ||
		ParentType == "class_selector" && (CurType == "nesting_selector" || NodeKind::CssSelectorName.Contains(NextType)) ||
		ParentType == "id_selector" ||
		NodeKind::RubyArrayLiteral.Contains(ParentType) && (CurIs({ "%w(", "%i(" }) || NextIs({ ")" })) ||
		ParentType == "attribute_selector" && CurType != "ERROR" && NextType != "ERROR"
		// 括弧・引用等の内側を密着させる事の返戻
	) return false;
	// Kotlin の名前付演算子と括弧の境界は呼出と誤認しない様に空白を配置
	if(ParentType == "infix_expression") return true;
	if(IsCurNamed && IsNextNamed) {
		// 親・境界別の密着規則と末尾ラムダだけの空白区切り
		if(
			NodeKind::MacroLike.Contains(ParentType) || CurType == "optional_chain" ||
			PrevChar == '.' && !NodeKind::RubyArrayLiteral.Contains(ParentType) || NextChar == ',' ||
			NodeKind::UnaryPre.Contains(ParentType) && CurType == "bang" || ParentType == "postfix_expression" && NextType == "bang" ||
			NodeKind::AttachBracket.Contains(NextType) || NextAttachColonLike() ||
			NextType == "call_suffix" && (NextChar == '(' || NextChar == '<') ||
			NodeKind::LeadingTypeArguments.Contains(ParentType) && CurType == "type_arguments" ||
			ParentType == "call_expression" && NextType == "template_string"
			// 接続する構文要素を密着させる事の返戻
		) return false;
		// Kotlin の型注釈は原文の空白で実引数と関数型を区別
		if(Language == Lang::Kotlin && ParentType == "constructor_invocation" && NextType == "value_arguments") {
			if(const TSNode Annotation = ts_node_parent(Parent); std::string_view(ts_node_type(Annotation)) == "annotation") {
				// 原文の隣接の返戻
				if(std::string_view(ts_node_type(ts_node_parent(Annotation))) == "type_modifiers") return CurEnd != NextStart;
			}
		}
		// C# のラムダの仮引数の並びの前は名前でなく修飾・戻値の型・属性の為，空白で区切る事の返戻
		if(Language == Lang::CSharp && ParentType == "lambda_expression") return true;
		if(NodeKind::AttachParen.Contains(NextType)) {
			const bool IsRubyBareArgs = Language == Lang::Ruby && NextChar == '(' && IsNamedNode(ts_node_child(Next, 0));
			// `(` 直前は密着，但しタプル返戻の `func()(...)` の続き `(` と Ruby の括弧無の実引数列のみ空白有の返戻
			return NextChar != '(' || NodeKind::ReturnTuple.Contains(ParentType) && PrevChar == ')' || IsRubyBareArgs;
		}
		// 型仮引数の並びの前は修飾子の後だけ空白を置く事の返戻
		if(NodeKind::AngleBracketList.Contains(NextType)) return CurType == "modifiers";
		// Ruby の語と記号の空白区切り
		if(NodeKind::RubyArrayLiteral.Contains(ParentType)) return true;
		if(NextChar == '(' || NextChar == '[') {
			// 文区切・コンマ・型・制御本体等の括弧前だけ空白を置き，条件に本体の括弧を密着させて呼出へ不変
			return PrevChar == ';' || PrevChar == ',' ||
			PrevChar == ')' && (NodeKind::ReturnTuple.Contains(ParentType) || NodeKind::Control.Contains(ParentType)) ||
			NextChar == '(' && NodeKind::CParenDeclarator.Contains(NextType) ||
			NextChar == '[' && (NodeKind::BracketType.Contains(NextType) || CurType == "placeholder_type_specifier") ||
			NodeKind::SwiftKeywordNode.Contains(CurType);
		}
		// `{` を密着で開く親（複合リテラル等）なら密着で開く事の返戻
		return !(NodeKind::BraceAttach.Contains(ParentType) && NextChar == '{');
	}
	// Python の except*（例外グループ標識）は except と * を密着，* と型の間に空白を置く (except* TypeError)
	if(ParentType == "except_clause") {
		// `except*` を密着させる事の返戻
		if(CurType == "except" && NextType == "*") return false;
		// `*` の後に空白を置く事の返戻
		if(CurType == "*") return true;
	}
	// C# の後置ポインタ記号は型名へ密着
	if(Language == Lang::CSharp && NodeKind::PointerDecl.Contains(ParentType) && IsCurNamed && NextIs({ "*", "&" })) {
		// 誤読した乗算かの返戻
		return ParentType == "pointer_type" && ReadsAsCsProduct(Src, Parent);
	}
	if(NodeKind::PointerDecl.Contains(ParentType) && !IsCurNamed) {
		// Kotlin の展開は実引数以外へ記述不能
		if(
			Language == Lang::Kotlin && ParentType == "spread_expression" &&
			std::string_view(ts_node_type(ts_node_parent(Parent))) != "value_argument"
			// 読み違えた展開の原文の隣接の返戻
		) return CurEnd != NextStart;
		// `*` / `&` / `...` 各記号直後は密着，但しキーワード + 識別子の場合は英数字同士で空白要
		if(NodeKind::OpenTailToken.Contains(CurType) || !IsNextNamed) return false;
		// 英数字同士の境界（キーワード + 識別子）なら空白要の返戻
		return std::isalnum(static_cast<unsigned char>(PrevChar)) && std::isalnum(static_cast<unsigned char>(NextChar));
	}
	// 前置単項／更新式の子先頭が英字（キーワード）ならトークン区切で空白要の返戻
	if(NodeKind::UnaryPreOrUpdate.Contains(ParentType) && !IsCurNamed && IsNextNamed) {
		// Ruby の符号を数値で始まる式へ密着させると符号付の数値に為る為，空白で分離する事の返戻
		if(Language == Lang::Ruby && (PrevChar == '-' || PrevChar == '+') && !Src.AttachesRubySign(Next)) return true;
		// 演算子トークンが英字（キーワード）始まりなら空白要の返戻
		return std::isalpha(static_cast<unsigned char>(Src.Start(Cur) < Src.size() ? Src[Src.Start(Cur)] : '\0'));
	}
	// 後置の演算子へ密着させる事の返戻
	if(NodeKind::PostfixOrUpdateExpression.Contains(ParentType) && IsCurNamed && !IsNextNamed) return false;
	// PHP の Elvis 演算子 `?:` は一体の記号として密着
	if(NodeKind::Ternary.Contains(ParentType) && (CurIs({ "?", ":" }) || NextIs({ "?", ":" }))) {
		// Elvis 演算子の `?` `:` 境界以外で空白要の返戻
		return !(CurIs({ "?" }) && NextIs({ ":" }));
	}
	// 範囲演算子同士 (Rust `.. ..`) を密着させると `....` が別の字句 `...` `.` と読まれる為，空白で区切る事の返戻
	if(PrevChar == '.' && NextChar == '.' && NodeKind::RangeLike.Contains(ParentType)) return true;
	// Swift の二重 Optional 型は型への密着必須
	if(
		NodeKind::OptionalMarker.Contains(ParentType) && (CurIs({ "??" }) || NextIs({ "??" })) || NextIs({ "?", "?." }) ||
		CurIs({ "?", "?." }) && !NextIs({ "=" }) || NodeKind::RangeLike.Contains(ParentType)
		// 省略可能記号・範囲演算子を密着させる事の返戻
	) return false;
	if(NodeKind::InfixOp.Contains(ParentType)) {
		// 解析器が二項式へ誤読した SCSS の -$base と Rust の 0..*a は右被演算子の前置単項として密着を復元
		if(ParentType == "binary_expression") {
			if(const TSNode FirstChild = ts_node_named_child(Parent, 0); !ts_node_is_null(FirstChild)) {
				// 幅の無い子の後を密着させる事の返戻
				if(ts_node_start_byte(FirstChild) == ts_node_end_byte(FirstChild)) return false;
				if(Language == Lang::Rust && std::string_view(ts_node_type(FirstChild)) == "range_expression") {
					if(
						// 範囲式の子節点数
						const uint32_t RangeChildCount = ts_node_child_count(FirstChild);
						RangeChildCount && std::string_view(ts_node_type(ts_node_child(FirstChild, RangeChildCount - 1))) == ".."
						// 終端の無い範囲を密着させる事の返戻
					) return false;
				}
			}
		}
		// C 形式型変換を二項式と誤読した時はキャストの記号を密着
		if(Language.IsCFamily() && ParentType == "binary_expression") {
			const auto IsCastLikeParen = [](const TSNode Paren) -> bool {
				// 括弧の式でない事の返戻
				if(std::string_view(ts_node_type(Paren)) != "parenthesized_expression") return false;
				// 括弧内の名前付節点
				TSNode NamedChild = {};
				uint32_t NamedCount = 0;
				ForEachNamedChild(
					Paren,
					[&](const TSNode Child) -> void {
						// 名前付子節点の確定
						NamedChild = Child;
						// 名前付子節点数の進行
						++NamedCount;
					}
				);
				// 単一の子でない括弧はキャスト風でない事の返戻
				if(NamedCount != 1) return false;
				// 中身が型名相当ノード種別の何れかに該当するかの返戻
				return NodeKind::CCastTypeNameInner.Contains(NamedChild);
			};
			if(
				IsCurNamed && (NextChar == '-' || NextChar == '+' || NextChar == '!' || NextChar == '~') && IsCastLikeParen(Cur) ||
				!IsCurNamed && NodeKind::UnaryPrefixToken.Contains(CurType) && CurIndex &&
				IsCastLikeParen(ts_node_child(Parent, CurIndex - 1)) && PrevChar != NextChar
				// キャストに続く単項演算子を密着させる事の返戻
			) return false;
		}
		// 本関数は同じ親の隣接子対毎に呼ばれる為，名前付子の両端を都度数えると親の子数の二乗に増大
		struct NamedBoundsCache {
			const void *NodeId = nullptr; // キャッシュ同一性の照合値
			const TSTree *Tree = nullptr;
			const char *Type = nullptr;
			uint32_t StartByte = 0; // 対象の開始バイト位置
			uint32_t EndByte = 0; // 対象の終端バイト位置
			uint32_t ChildCount = 0; // 子節点の個数
			uint32_t FirstNamed = 0; // 先頭の名前付子索引
			uint32_t LastNamed = 0; // 末尾の名前付子索引
			uint64_t LastUse = 0; // 最後に使った順番（溢れた時の犠牲の選択に使う）
		};
		// 親子を交互に訪ねる走査に対する複数件の境界控え
		static constexpr size_t BoundsSlots = 8;
		static thread_local NamedBoundsCache Bounds[BoundsSlots];
		static thread_local uint64_t BoundsClock = 0;
		const uint32_t ParentChildCount = ts_node_child_count(Parent), ParentStart = ts_node_start_byte(Parent);
		const uint32_t ParentEnd = ts_node_end_byte(Parent);
		const char *const ParentTypeRaw = ts_node_type(Parent);
		// 構文木・型・範囲・子数の一致確認後に於ける境界控えの使用
		NamedBoundsCache *Slot = nullptr, *Victim = &Bounds[0];
		for(NamedBoundsCache &Entry : Bounds) {
			if(
				Entry.NodeId == Parent.id && Entry.Tree == Parent.tree && Entry.Type == ParentTypeRaw && Entry.StartByte == ParentStart &&
				Entry.EndByte == ParentEnd && Entry.ChildCount == ParentChildCount
			) {
				// 使用する境界控えの更新
				Slot = &Entry;
				break;
			}
			if(Entry.LastUse < Victim->LastUse) Victim = &Entry;
		}
		if(!Slot) {
			uint32_t First = ParentChildCount, Last = 0, SibIdx = 0;
			ForEachChild(
				Parent,
				[&](const TSNode Sib) -> void {
					if(ts_node_is_named(Sib)) {
						if(First == ParentChildCount) First = SibIdx;
						// 末尾の対象節点の確定
						Last = SibIdx;
					}
					// 兄弟節点の索引の進行
					++SibIdx;
				}
			);
			// 使用する境界控えの更新
			Slot = Victim;
			*Slot = { Parent.id, Parent.tree, ParentTypeRaw, ParentStart, ParentEnd, ParentChildCount, First, Last, 0 };
		}
		// 境界控えの最終使用世代の更新
		Slot->LastUse = ++BoundsClock;
		// 中置演算子の被演算子間は空白で区切るが，関数キーワード直後の仮引数列は演算子境界でない為，密着
		if(
			// 先頭の名前付子索引
			const uint32_t FirstNamed = Slot->FirstNamed, LastNamed = Slot->LastNamed;
			(
				!IsCurNamed && CurIndex > FirstNamed && CurIndex < LastNamed && !NodeKind::SeparatorToken.Contains(CurType) ||
				!IsNextNamed && CurIndex + 1 > FirstNamed && CurIndex + 1 < LastNamed && !NodeKind::SeparatorToken.Contains(NextType) &&
				NextType != ")"
			) && !(IsNextNamed && NodeKind::ParameterContainer.Contains(NextType))
			// 中置演算子を空白で区切る事の返戻
		) return true;
	}
	// HTML タグ開閉記号の内側は空白を挿入しないが `/>` 直前は１空白へ正規化
	if(NodeKind::HtmlTag.Contains(ParentType)) return !CurIs({ "<", "</" }) && !NextIs({ ">" });
	if(NodeKind::MacroLike.Contains(ParentType) || NodeKind::StripEq.Contains(ParentType) && (CurIs({ "=" }) || NextIs({ "=" }))) {
		// `operator_name` 親で識別子同士の境界（`operator new`/`operator delete` 等のキーワード演算子）のみ空白要の返戻
		return ParentType == "operator_name" && PrevChar && NextChar &&
		(std::isalpha(static_cast<unsigned char>(PrevChar)) || PrevChar == '_') &&
		(std::isalpha(static_cast<unsigned char>(NextChar)) || NextChar == '_');
	}
	// C# の終了子の `~` は名前の一部で，構築子の名前と同じく密着させる事の返戻
	if(ParentType == "destructor_declaration" && CurIs({ "~" })) return false;
	// 代入の `=` の前後に空白を配置
	if(
		NodeKind::NormEq.Contains(ParentType) && (CurIs({ "=" }) || NextIs({ "=" })) ||
		Language == Lang::CSS && CurIs({ ":" }) && NextIs({ ";" })
		// 代入の `=` の前後と空の値の `:` `;` の間に空白を置く事の返戻
	) return true;
	// 型引数に誤解析された比較式の山括弧密着回避
	if(
		NodeKind::Compact.Contains(ParentType) && (CurIs({ "=" }) || NextIs({ "=" })) || CurIs({ "::" }) || NextIs({ "::" }) ||
		ParentType == "channel_type" && !IsCurNamed && !IsNextNamed || CurIs({ "(", "[" }) || NextIs({ ")", "]" }) || CurIs({ "<!" }) ||
		CurIs({ "]" }) && NodeKind::GoType.Contains(ParentType) ||
		NodeKind::AngleBracketList.Contains(ParentType) && (CurIs({ "<" }) || NextIs({ ">" })) || NextIs({ ";", "," }) ||
		CurIs({ "@" }) && IsNextNamed || NextIs({ "..." }) && ParentType == "parameter" ||
		CurIs({ "\\" }) && ParentType == "key_path_expression" || CurIs({ "#", "#[" }) ||
		CurIs({ "&" }) && ParentType == "reference_assignment_expression"
		// 区切記号を密着させる事の返戻
	) return false;
	// `;`・`,`・`:` 直後に於ける後続括弧又は同じ字句以外への空白挿入
	if(CurIs({ ";", ",", ":" })) {
		// 閉じ括弧・同トークン以外が続く場合の空白要の返戻
		return NextChar != ')' && NextChar != ']' && NextChar != (CurType == ":" ? '}' : ';') &&
		!(NextChar == '|' && NodeKind::PipeParamList.Contains(ParentType));
	}
	// 現トークンが `{` 又は次トークンが `}` の共通分岐
	if(const bool IsCurOpen = CurIs({ "{" }), IsNextClose = NextIs({ "}" }); IsCurOpen || IsNextClose) {
		// 空白を置かない波括弧の返戻
		if(NodeKind::SpacelessBrace.Contains(ParentType) || IsCurOpen && IsNextClose) return false;
		if(
			NodeKind::ObjectLike.Contains(ParentType) || NodeKind::IndentContainer.Contains(ParentType) ||
			Language == Lang::Swift && ParentType == "switch_statement" || ParentType == "lambda_literal"
			// `{ ... }` の内側に空白を置く波括弧の返戻
		) return true;
	}
	if(
		(NodeKind::PipeParamList.Contains(ParentType) || Language == Lang::CSS && NodeKind::CssNamespacePrefix.Contains(ParentType)) &&
		(CurIs({ "|" }) || NextIs({ "|" }))
		// 仮引数を囲む `|` と CSS の名前空間の区切り `|`(`svg|a` / `[xlink|href]`) を密着させる事の返戻
	) return false;
	if(NextIs({ ":" })) {
		// `:` の前は制御文用／中置演算子／通常コロン／節先頭直後以外で空白挿入の返戻
		return !(
			NodeKind::ColonControl.Contains(ParentType) || NodeKind::InfixOp.Contains(ParentType) ||
			NodeKind::NormColon.Contains(ParentType) || IsCurNamed && NodeKind::SectionHeader.Contains(CurType)
		);
	}
	// Swift の `case` と名前付の節点に為る語の直後の `.` と組の照合の `(`はキーワード扱いで空白を保持
	if(
		CurIs({ "case" }) && NextIs({ "." }) || IsCurNamed && (
			NodeKind::SwiftKeywordNode.Contains(CurType) && (NextIs({ "." }) || NextChar == '(') ||
			NextIs({ "." }) && (Language == Lang::Python || Language.IsJsTs()) && NodeKind::NumberLiteral.Contains(CurType) &&
			Src.View(Cur).find_first_not_of("0123456789_") == std::string_view::npos
		)
		// 語・数値と後続の区切りの間に空白を置く事の返戻
	) return true;
	// Ruby のラムダ演算子と直後の仮引数列開始括弧の密着
	if(
		CurIs({ ".", "&.", "?." }) || NextIs({ ".", "&.", "?." }) ||
		IsNextNamed && (NextChar == '.' && NodeKind::Member.Contains(ParentType) || NextType == "optional_chain") ||
		IsCurNamed && (PrevChar == '.' || CurType == "optional_chain") ||
		Language == Lang::Ruby && ParentType == "lambda" && CurIs({ "->" }) && NextChar == '('
		// メンバアクセスとラムダの仮引数列を密着させる事の返戻
	) return false;
	// 矢印の演算子はメンバの参照以外で空白を置く事の返戻
	if(CurIs({ "->", "=>", "?->" }) || NextIs({ "->", "=>", "?->" })) return !NodeKind::Member.Contains(ParentType);
	// Go の空の構造体・インタフェース本体は型名へ密着
	if(NodeKind::BraceNoSpace.Contains(ParentType) && NextChar == '{') {
		// 成員を持つ本体かの返戻
		return ts_node_named_child_count(NextType == "field_declaration_list" ? Next : Parent);
	}
	// `{` は密着対象の親以外で空白挿入
	if(NextIs({ "{" })) return !NodeKind::BraceAttach.Contains(ParentType);
	// `func()(...)` のタプル返戻連鎖呼出のみ空白挿入
	if(IsCurNamed && NextIs({ "(", "[" })) return NextType == "(" && NodeKind::ReturnTuple.Contains(ParentType) && PrevChar == ')';
	// C# の型演算子 `typeof` と開始括弧の密着
	if((Language == Lang::CSharp && CurType == "typeof" || CurType == "sizeof") && NextChar == '(') return false;
	if(!IsCurNamed && (NextChar == '(' || NextChar == '[')) {
		const bool IsCtrlKw =
		NodeKind::ControlKeyword.Contains(CurType) || Language == Lang::Cpp && ParentType == "if_statement" && CurType == "constexpr";
		// 無名の括弧前は文脈別に空白を決め
		return CurType == "func" && ParentType == "method_declaration" || ParentType == "export_statement" ||
		ParentType == "function_declaration" && CurType != "func" && Language != Lang::Swift || IsCtrlKw && (
			IsNextNamed && NodeKind::PatternKind.Contains(NextType) || Language == Lang::Go || Language == Lang::Rust ||
			Language == Lang::Swift || Language == Lang::Python || Language == Lang::Ruby
		) || (
			IsNextNamed && NextType == "argument_list" && NextChar == '[' || !IsCtrlKw && !(
				NodeKind::RubyFlowExpr.Contains(ParentType) && IsNextNamed && NextType == "argument_list" &&
				!IsNamedNode(ts_node_child(Next, 0))
			) && (
				// `=` / `:=` の右が括弧で始まる形は，密着させると `=(` が別の演算子として読まれる言語が有る為，常に空白を配置
				CurIs({ "=", ":=" }) && (NextChar == '(' || NextChar == '[' || NextChar == '{') ||
				CurType == "=" && IsNextNamed && NodeKind::InfixOp.Contains(ParentType) || NodeKind::SpaceKeyword.Contains(CurType) ||
				NodeKind::ArrowToken.Contains(CurType) ||
				!(IsNextNamed && (NodeKind::AttachParen.Contains(NextType) || NodeKind::AttachBracket.Contains(NextType))) && (
					IsNextNamed && std::isalpha(static_cast<unsigned char>(PrevChar)) && !NodeKind::GoType.Contains(ParentType) ||
					CurType == ")" && NodeKind::Control.Contains(ParentType)
				)
			)
		);
	}
	if(!IsCurNamed && IsNextNamed) {
		// Swift の演算子関数名と型仮引数開始字句の融合を防ぐ空白区切り
		if(NodeKind::AngleBracketList.Contains(NextType)) {
			// 修飾子・`fun`・演算子関数の名前の後の空白要の返戻
			return NodeKind::KotlinFunctionHead.Contains(CurType) || Language == Lang::Swift && ParentType == "function_declaration";
		}
		// コロン類を密着させる事の返戻
		if(NextAttachColonLike()) return false;
	}
	// 既定では兄弟間に空白挿入が必要の返戻
	return true;
}

/**
 * 間隙置換エディット追記関数
 * @param Src ソースコード
 * @param Parent 親ノード
 * @param ParentType 親ノードの種別
 * @param Prev 間隙の直前の子ノード
 * @param PrevType 直前の子ノードの種別
 * @param Next 間隙の直後の子ノード
 * @param NextType 直後の子ノードの種別
 * @param PairIdx Prev の親内インデックス
 * @param Language 対象言語
 * @param Edits 収集先のエディット一覧
 */
void StructurePass::PushGapEditIfNeeded(
	const TSSource &Src,
	const TSNode Parent,
	const std::string_view ParentType,
	const TSNode Prev,
	const std::string_view PrevType,
	const TSNode Next,
	const std::string_view NextType,
	const uint32_t PairIdx,
	const Lang Language,
	std::vector<TextEdit> &Edits
) {
	// 字句間隙の開始位置
	const uint32_t GapStart = Src.End(Prev), GapEnd = Src.Start(Next);
	// 条件成立時の返戻
	if(GapStart > GapEnd) return;
	// 空白・タブ以外を含む間隙の除外
	for(uint32_t Byte = GapStart; Byte < GapEnd; ++Byte) if(Src[Byte] != ' ' && Src[Byte] != '\t') return;
	if(
		const char *const Expected =
		NeedsGapBetween(Src, Parent, ParentType, Prev, PrevType, Next, NextType, PairIdx, Language) ? " " : "";
		std::string_view(Src.data() + GapStart, GapEnd - GapStart) != Expected
	) TextEdit::Push(GapStart, GapEnd, Expected, Edits);
	// 終了
	return;
}

/**
 * 行内スペース収集関数
 * @param Src ソースコード
 * @param Node 走査対象のルートノード
 * @param Language 対象言語
 * @param Edits 収集先のエディット一覧
 */
void StructurePass::CollectInlineSpaceEdits(
	const TSSource &Src,
	const TSNode Node,
	const Lang Language,
	std::vector<TextEdit> &Edits
) {
	// 条件成立時の返戻
	if(ts_node_is_null(Node)) return;
	// 構文木の走査位置
	HeldCursor Cursor(Node);
	while(true) {
		// 走査中の節点
		const TSNode CurrentNode = ts_tree_cursor_current_node(&Cursor);
		bool ShouldDescend = true;
		const std::string_view NodeTypeView(ts_node_type(CurrentNode));
		const bool ShouldRecurseLeaf = NodeTypeView == "template_string" || NodeKind::AngleBracketList.Contains(NodeTypeView);
		if(
			NodeKind::Comment.Contains(NodeTypeView) || IsVerbatimLeaf(NodeTypeView, Language) && !ShouldRecurseLeaf ||
			NodeKind::Preproc.Contains(NodeTypeView) || NodeKind::NoInnerSpaceEdit.Contains(NodeTypeView)
		) ShouldDescend = false;
		else if(ts_node_child_count(CurrentNode) > 1 && !NodeKind::SkipGapEdit.Contains(NodeTypeView)) {
			TSNode Prev {};
			bool HasPrev = false;
			uint32_t ChildIdx = 0;
			ForEachChild(
				CurrentNode,
				[&](const TSNode Child) -> void {
					if(HasPrev) {
						// `ERROR` 隣接間隙の識別子合体を避ける逐語保持
						if(
							// 子節点の型名
							const std::string_view PrevType(ts_node_type(Prev)), ChildType(ts_node_type(Child));
							PrevType != "ERROR" && ChildType != "ERROR"
						) PushGapEditIfNeeded(Src, CurrentNode, NodeTypeView, Prev, PrevType, Child, ChildType, ChildIdx, Language, Edits);
						// 子節点の索引の進行
						++ChildIdx;
					}
					// 直前の節点の確定
					Prev = Child;
					// 直前節点の保持状態の設定
					HasPrev = true;
				}
			);
		}
		if(ShouldDescend && ts_tree_cursor_goto_first_child(&Cursor)) continue;
		while(!ts_tree_cursor_goto_next_sibling(&Cursor)) {
			// 起点ノードへ復帰した場合の終了
			if(!ts_tree_cursor_goto_parent(&Cursor) || ts_node_eq(ts_tree_cursor_current_node(&Cursor), Node)) return;
		}
	}
}

/**
 * 行内トークン間スペース正規化関数
 * @param Src 整形対象のソース（結果で上書きされる）
 * @param Language 対象言語
 */
void StructurePass::NormalizeInlineSpaces(TSSource &Src, const Lang Language) {
	// Python 向の字句間隙整形（構造化再構築が出来ない言語に限定）
	if(!Src.IsParsed()) return;
	// 構文木の根節点
	const TSNode Root = Src.GetRoot();
	const uint32_t ChildCount = ts_node_child_count(Root);
	std::vector<TextEdit> Edits;
	const size_t NumThreads = Parallel::DecideThreads(ChildCount, 8);
	if(NumThreads < 2 || ChildCount < 2) {
		CollectInlineSpaceEdits(Src, Root, Language, Edits);
		// 登録済編集の原文への適用
		TextEdit::Apply(Src, Edits);
		// 終了
		return;
	}
	// `ts_node_child(Root, Idx)` の累積 O(N ²) を避ける為，１度の TSTreeCursor でベクタに収集
	std::vector<TSNode> RootKids;
	// 根直下の子節点列の格納領域予約
	RootKids.reserve(ChildCount);
	ForEachChild(
		Root,
		[&](const TSNode Child) -> void {
			// 根直下の子節点列への追加
			RootKids.push_back(Child);
		}
	);
	// Root レベルの間隙（子間スペース）は逐次で収集，Root の各子内部は並列に走査
	if(const std::string_view RootType(ts_node_type(Root)); !NodeKind::SkipGapEdit.Contains(RootType)) {
		// 隣接対の型名は前反復の値を引継，子毎の再取得（strlen 相当）を排除
		std::string_view CurType(ts_node_type(RootKids[0]));
		for(uint32_t Idx = 0; Idx + 1 < ChildCount; ++Idx) {
			// 後続節点の型名
			const std::string_view NextType(ts_node_type(RootKids[Idx + 1]));
			PushGapEditIfNeeded(Src, Root, RootType, RootKids[Idx], CurType, RootKids[Idx + 1], NextType, Idx, Language, Edits);
			// 現在節点の型名の確定
			CurType = NextType;
		}
	}
	std::vector<std::vector<TextEdit>> ThreadEdits(NumThreads);
	Parallel::ForChunks(
		ChildCount,
		NumThreads,
		[&](const size_t Start, const size_t End, const size_t Tid) -> void {
			for(size_t Idx = Start; Idx < End; ++Idx) CollectInlineSpaceEdits(Src, RootKids[Idx], Language, ThreadEdits[Tid]);
		}
	);
	// 要素の総数
	size_t Total = Edits.size();
	for(const std::vector<TextEdit> &Local : ThreadEdits) Total += Local.size();
	// 適用予定の編集列の格納領域予約
	Edits.reserve(Total);
	for(std::vector<TextEdit> &Local : ThreadEdits) {
		// 適用予定の編集列への範囲追加
		Edits.insert(Edits.end(), std::make_move_iterator(Local.begin()), std::make_move_iterator(Local.end()));
	}
	// 登録済編集の原文への適用
	TextEdit::Apply(Src, Edits);
	// 終了
	return;
}

/**
 * 解析に失敗した前処理指令を子に持つかの判定関数
 * C 解析器が式内指令を `ERROR` に落とした場合も原文を保つ
 * 前処理指令は行単位の構文で，組み直して行を繋ぐと後続が指令へ取り込まれる為，其の節点は原文の儘写す
 * @param Src ソースコード
 * @param Node 判定対象のノード
 * @return `#` で始まる `ERROR` を直接の子に持てば true
 */
bool StructurePass::HoldsBrokenDirective(const TSSource &Src, const TSNode Node) {
	// 失敗を含まない節点の返戻
	if(!ts_node_has_error(Node)) return false;
	// 前処理指令の字句を落とした `ERROR` の有無の返戻
	return HasChildOf(
		Node,
		[&Src](const TSNode Child) -> bool {
			// `#` で始まる `ERROR` かの返戻
			return std::string_view(ts_node_type(Child)) == "ERROR" && Src.Start(Child) < Src.size() && Src[Src.Start(Child)] == '#';
		}
	);
}

/**
 * 片側だけ空白の有る Swift の利用者定義演算子を持つかの判定関数
 * Swift は演算子の前後の空白の有無で前置・後置・中置を見分け（`a^^` の後に改行が続けば後置），両側に空白を置くと中置に変わる
 * 解析器は宣言を知らず中置の式と読む為（`a^^↵print(b)` を `a ^^ print(b)`），片側だけ空白の有る利用者定義演算子の式は原文の儘写す
 * @param Src ソースコード
 * @param Node 判定対象のノード
 * @param Language 対象言語
 * @return 片側だけ空白の有る利用者定義演算子を直接の子に持つ Swift の中置の式なら true
 */
bool StructurePass::HoldsLopsidedOperator(const TSSource &Src, const TSNode Node, const Lang Language) {
	// Swift の中置の式でない場合の返戻
	if(Language != Lang::Swift || std::string_view(ts_node_type(Node)) != "infix_expression") return false;
	// 利用者定義演算子の前後の空白の有無が揃わないかの返戻
	return HasChildOf(
		Node,
		[&Src](const TSNode Child) -> bool {
			// 演算子でない子の返戻
			if(std::string_view(ts_node_type(Child)) != "custom_operator") return false;
			// 対象直前の状態列
			const TSNode Before = ts_node_prev_sibling(Child), After = ts_node_next_sibling(Child);
			// 片側だけ空白の有る演算子かの返戻
			return !ts_node_is_null(Before) && !ts_node_is_null(After) &&
			Src.End(Before) == Src.Start(Child) != (Src.End(Child) == Src.Start(After));
		}
	);
}

/**
 * CSS の `url(...)` の引数の並びの字面の取得関数
 * CSS の URL は丸括弧の内側だけ詰め，参照先の字面を保つ
 * @param Src ソースコード
 * @param Node `url` 呼出の引数の並び
 * @return 丸括弧の内側の両端の余白を詰めた原文
 */
std::string StructurePass::CssUrlArgumentsText(const TSSource &Src, const TSNode Node) {
	// 参照先を変えずに詰める丸括弧内の範囲
	std::string_view Text = Src.View(Node);
	const bool IsOpen = Text.starts_with('('), IsClose = Text.size() > 1 && Text.ends_with(')');
	// 括弧の外形と内側の余白の分離
	Text.remove_prefix(IsOpen);
	// 対象の字面からの末尾区切り除去
	Text.remove_suffix(IsClose);
	TextEdit::TrimView(Text);
	// 組立中の結果
	std::string Result;
	// 組立中の結果の格納領域予約
	Result.reserve(Text.size() + 2);
	// 原文に存在した括弧だけの復元
	if(IsOpen) Result += '(';
	// 組立中の結果への字面追加
	Result.append(Text);
	if(IsClose) Result += ')';
	// 詰めた原文の返戻
	return Result;
}

/**
 * HTML の逐語保持要素の組立関数
 * 空白を描画する要素は子の間の空白と本文を原文の儘保ち，内側のタグだけを整える（空白の描画は内側の要素へ継承される）
 * 内容を文字として読む要素は，開始・終了タグの間を原文の儘保つ
 * @param Out 追記先
 * @param Src ソースコード
 * @param Node 対象の要素
 * @param Kind 内容の逐語保持の区分
 */
void StructurePass::AppendHtmlVerbatim(std::string &Out, const TSSource &Src, const TSNode Node, const HtmlVerbatim Kind) {
	const RecursionGuard Guard;
	if(Guard.IsOverflow) {
		Parallel::RunOnFreshStack(
			[&]() -> void {
				// 逐語保持範囲の出力への追加
				AppendHtmlVerbatim(Out, Src, Node, Kind);
			}
		);
		// 深いネストの続きを新しい走脈で組み立てて終了
		return;
	}
	// 直前の節点
	TSNode Prev = {};
	bool HasPrev = false;
	ForEachChild(
		Node,
		[&](const TSNode Child) -> void {
			// 子節点の型名
			const std::string_view ChildType = ts_node_type(Child);
			const bool IsTag = NodeKind::HtmlTag.Contains(ChildType);
			// 内容を文字として読む要素の子（構文解析器が読んだ要素）はタグの間の原文として纏めて複写
			if(Kind == HtmlVerbatim::RawContent && !IsTag) return;
			// 子の末尾空白は再帰で複写済の為，子の終端から間隙を複写
			if(HasPrev) if(const uint32_t GapStart = Src.End(Prev), GapEnd = Src.Start(Child); GapStart < GapEnd) {
				// 結果の格納先への字面追加
				Out.append(Src.data() + GapStart, GapEnd - GapStart);
			}
			// 逐語本文を維持したタグ部分だけの整形
			if(IsTag) BuildFlatFromAST(Src, Child, Lang::Get(Lang::HTML), Out);
			else if(ChildType == "element") {
				// 親から継承する空白保持と生の内容の区別
				const HtmlVerbatim ChildKind = HtmlVerbatimOf(Src, Child);
				// 逐語保持範囲の出力への追加
				AppendHtmlVerbatim(Out, Src, Child, ChildKind == HtmlVerbatim::RawContent ? ChildKind : HtmlVerbatim::Preformatted);
			} else Out.append(Src.View(Child));
			// 直前の節点の確定
			Prev = Child;
			// 直前節点の保持状態の設定
			HasPrev = true;
		}
	);
	// 終了タグが無い場合は，最後の子の後から要素の終端迄（内容を文字として読む要素では開始タグの後の全て）を複写
	if(HasPrev && std::string_view(ts_node_type(Prev)) != "end_tag") {
		// 末尾間隙の開始位置
		const uint32_t TailStart = Src.End(Prev);
		// 結果の格納先への字面追加
		Out.append(Src.data() + TailStart, Src.End(Node) - TailStart);
	}
	// 終了
	return;
}

/**
 * 末尾空白削除関数
 * @param Text 走査対象の文字列（破壊的に末尾を縮める）
 */
void StructurePass::TrimTrailingSpaces(std::string &Text) {
	// 末尾に連続する空白の除去
	while(!Text.empty() && Text.back() == ' ') Text.pop_back();
	// 終了
	return;
}

/**
 * プリプロセッサ指令判定関数
 * @param Raw 判定対象テキスト
 * @param Pos 判定開始位置
 * @return `#` の直後（空白を挟んでも良い）に指令名が有れば true
 */
bool StructurePass::IsPreprocDirectiveAt(const std::string_view Raw, const size_t Pos) {
	// `#` で始まらない事の返戻
	if(Pos >= Raw.size() || Raw[Pos] != '#') return false;
	// 対象範囲の開始位置
	size_t Start = Pos + 1;
	while(Start < Raw.size() && (Raw[Start] == ' ' || Raw[Start] == '\t')) ++Start;
	// 指令名は処理系の拡張（`#import` / `#include_next` / `#ident` 等）も有る為，名前の種類を限らない事の返戻
	return Start < Raw.size() && std::isalpha(static_cast<unsigned char>(Raw[Start]));
}

/**
 * プリプロセッサノードテキスト構築関数
 * @param Raw 元のプリプロセッサノード範囲のテキスト
 * @return 前後空白・行頭空白を除去して再構築したテキスト
 */
std::string StructurePass::BuildPreprocText(const std::string_view Raw) {
	std::string_view View = Raw;
	TextEdit::TrimView(View);
	// 結果の格納先
	std::string Out;
	// 結果の格納先の格納領域予約
	Out.reserve(View.size() + 8);
	// 行頭位置か
	bool IsLineStart = true;
	char Quote = '\0';
	bool IsLineNote = false, IsBlockNote = false, IsAfterHash = false, IsIncludeLine = false;
	const auto BeginDirective = [&](const size_t HashIdx) -> void {
		// 結果の格納先への追加
		Out.push_back('#');
		// プリプロセッサ記号直後の状態の設定
		IsAfterHash = true;
		// 指示子名の開始位置
		size_t NameIdx = HashIdx + 1;
		while(NameIdx < View.size() && (View[NameIdx] == ' ' || View[NameIdx] == '\t')) ++NameIdx;
		// 対象の識別名
		const std::string_view Name = View.substr(NameIdx);
		// 取込指示子の行該当有無の更新
		IsIncludeLine = Name.starts_with("include") || Name.starts_with("import");
	};
	for(size_t Idx = 0; Idx < View.size(); ++Idx) {
		// 走査中の文字
		char Char = View[Idx];
		if(Char == '\r') Char = '\n';
		if(Char == '\n') {
			// 行継続に於ける行コメントと引用符状態の論理行内での継承
			if(!(Idx && View[Idx - 1] == '\\')) {
				// 行コメント内の状態の解除
				IsLineNote = false;
				// 現在開いている引用符の初期化
				Quote = '\0';
				// 取込指示子の行該当状態の解除
				IsIncludeLine = false;
			}
			// 出力末尾の余分な空白除去
			TrimTrailingSpaces(Out);
			// 結果の格納先への追加
			Out.push_back('\n');
			// 行頭状態の設定
			IsLineStart = true;
			// プリプロセッサ記号直後の状態の解除
			IsAfterHash = false;
			continue;
		}
		const bool IsInCode = !Quote && !IsLineNote && !IsBlockNote;
		if(IsInCode && Char == '/' && Idx + 1 < View.size() && View[Idx + 1] == '/') IsLineNote = true;
		else if(IsInCode && Char == '/' && Idx + 1 < View.size() && View[Idx + 1] == '*') IsBlockNote = true;
		else if(IsBlockNote && Char == '/' && Idx && View[Idx - 1] == '*') IsBlockNote = false;
		else if(IsInCode && (Char == '"' || Char == '\'' || IsIncludeLine && Char == '<')) Quote = Char == '<' ? '>' : Char;
		else if(Quote && Char == '\\') {
			// 逃避文字の次は引用符でも状態を変えない為，其の儘写して読飛し
			Out.push_back(Char);
			if(++Idx < View.size()) Out.push_back(View[Idx]);
			// 行頭状態の解除
			IsLineStart = false;
			// プリプロセッサ記号直後の状態の解除
			IsAfterHash = false;
			continue;
		} else if(Quote == Char) Quote = '\0';
		// 引用・コメント状態を保った行頭の正規化
		if(IsLineStart) {
			if(Char == ' ' || Char == '\t') continue;
			// 行頭状態の解除
			IsLineStart = false;
			if(IsInCode && Char == '#' && IsPreprocDirectiveAt(View, Idx)) {
				if(!Out.empty() && Out.back() != '\n') Out.push_back('\n');
				BeginDirective(Idx);
				continue;
			}
			// 結果の格納先への追加
			Out.push_back(Char);
			continue;
		}
		// 字句区切りの空白一つへの畳込と指令名直前の空白除去
		if(IsInCode && (Char == ' ' || Char == '\t')) {
			if(!IsAfterHash && Out.back() != ' ') Out.push_back(' ');
			continue;
		}
		// プリプロセッサ記号直後の状態の解除
		IsAfterHash = false;
		// 結果の格納先への追加
		Out.push_back(Char);
	}
	// 構築済の構造化テキストの返戻
	return Out;
}

/**
 * Kotlin の暗黙の主構築子かの判定関数（`constructor` キーワードを字面に持たない形）
 * tree-sitter-kotlin は主構築子の `constructor` を子ノードに出さない為，字面に有る形は子から組み直すと消える
 * 省略形は失う字句が無く，内側の型引数リスト等を規約通りに整形出来る
 * @param Src 判定対象を含むソース
 * @param Node 判定対象のノード
 * @param TypeView 判定対象のノード型
 * @param Language 対象言語
 * @return 再構築して良い主構築子なら true
 */
bool StructurePass::IsKotlinImplicitCtor(
	const TSSource &Src,
	const TSNode Node,
	const std::string_view TypeView,
	const Lang Language
) {
	// 暗黙の主構築子かの返戻
	return Language == Lang::Kotlin && TypeView == "primary_constructor" &&
	Src.View(Node).find("constructor") == std::string_view::npos;
}

/**
 * バイト範囲一致判定関数
 * @param Src ソースコード
 * @param Lhs 比較対象のノード
 * @param Rhs 比較対象のノード
 * @return 開始・終了バイトが共に一致すれば true
 */
bool StructurePass::HasSameByteRange(const TSSource &Src, const TSNode Lhs, const TSNode Rhs) {
	// 開始・終了バイトの一致有無の返戻
	return Src.Start(Lhs) == Src.Start(Rhs) && Src.End(Lhs) == Src.End(Rhs);
}

/**
 * HTML の子の並びの文脈判定関数
 * 外来要素の根 (`<svg>` / `<math>`) の内側は子の間の空白を描画せず，文字・字句の要素の内側だけが空白を描画する
 * 最も内側の外来要素の根・HTML へ戻る要素・HTML の行を区切る要素の何れかで決まる
 * @param Src ソースコード
 * @param Container 子を持つノード (element / document)
 * @return 子の並びの文脈
 */
StructurePass::HtmlContext StructurePass::HtmlContextOf(const TSSource &Src, const TSNode Container) {
	// 祖先要素からの HTML 文脈の取得
	std::unordered_map<const void *, uint8_t> &Memo = Src.GetContextMemo();
	if(Memo.empty()) {
		// 各深さの節点迄の文脈（外来要素の根・HTML へ戻る要素で決まる文脈と，其の内側の字句の要素の内か）
		std::vector<std::pair<HtmlContext, bool>> States;
		HtmlNameBuffer Buffer;
		// 親迄の HTML 文脈への節点タグの反映と控え
		const auto Enter = [&](const TSNode Node, std::pair<HtmlContext, bool> State) -> void {
			if(const std::string_view Tag = HtmlLowerName(HtmlTagName(Src, Node), Buffer); Tag == "svg" || Tag == "math") {
				// HTML の継承文脈の更新
				State = { HtmlContext::Figure, false };
			} else if(Tag == "foreignobject" || Tag == "annotation-xml" || NodeKind::HtmlBlockTag.Contains(Tag)) {
				// HTML の継承文脈の更新
				State = { HtmlContext::Html, false };
			} else State.second = State.second || NodeKind::HtmlForeignTextTag.Contains(Tag);
			// 子を持つ節点の外来要素内外に応じた HTML 文脈の控え
			if(States.empty() || std::string_view(ts_node_type(Node)) == "element") {
				// 節点別の判定控えの確定
				Memo[Node.id] = static_cast<uint8_t>(State.first == HtmlContext::Figure && State.second ? HtmlContext::Text : State.first);
			}
			// 節点別の HTML 文脈列への追加
			States.push_back(State);
		};
		// 構文木の走査位置
		HeldCursor Cursor(Src.GetRoot());
		Enter(Src.GetRoot(), { HtmlContext::Html, false });
		// 要素の子の並びだけを持つ要素へ降りる（タグ・本文・生の本文の中に要素は無い）
		for(bool IsDone = false; !IsDone;) {
			if(std::string_view(ts_node_type(ts_tree_cursor_current_node(&Cursor))) == "element" || States.size() == 1) {
				if(ts_tree_cursor_goto_first_child(&Cursor)) {
					Enter(ts_tree_cursor_current_node(&Cursor), States.back());
					continue;
				}
			}
			while(!ts_tree_cursor_goto_next_sibling(&Cursor)) {
				// 節点別の HTML 文脈列からの末尾要素取出
				States.pop_back();
				if(!ts_tree_cursor_goto_parent(&Cursor)) {
					// 構文木走査の完了状態の設定
					IsDone = true;
					break;
				}
			}
			if(!IsDone) {
				// 節点別の HTML 文脈列からの末尾要素取出
				States.pop_back();
				Enter(ts_tree_cursor_current_node(&Cursor), States.back());
			}
		}
	}
	// 検索結果を指す反復子
	const std::unordered_map<const void *, uint8_t>::const_iterator Iter = Memo.find(Container.id);
	// 記録済の文脈又は既定の HTML 文脈の返戻
	return Iter == Memo.end() ? HtmlContext::Html : static_cast<HtmlContext>(Iter->second);
}

/**
 * HTML の子の流れの区分判定関数
 * @param Src ソースコード
 * @param Child 対象の子
 * @param Context 子の並びの文脈
 * @return 子の流れの区分
 */
StructurePass::HtmlFlow StructurePass::HtmlFlowOf(const TSSource &Src, const TSNode Child, const HtmlContext Context) {
	const std::string_view Type = ts_node_type(Child);
	// 容器の開始・終了タグの返戻
	if(NodeKind::JsxHtmlTagOpenClose.Contains(Type)) return HtmlFlow::Edge;
	// 箱を作らない埋込とコメント（描画されず，前後の空白は隣の子の空白と合わせて描画される）の返戻
	if(NodeKind::HtmlRawTextElement.Contains(Type) || NodeKind::Comment.Contains(Type)) return HtmlFlow::Hidden;
	// 文書型宣言の前後の空白は構文解析で捨てられる為，行を区切る子の返戻
	if(Type == "doctype") return HtmlFlow::Block;
	// 本文・文字参照と，空白を描画する並びの要素の返戻
	if(Type != "element" || Context == HtmlContext::Text) return HtmlFlow::Inline;
	// 間の空白を描画しない並びの要素の返戻
	if(Context == HtmlContext::Figure) return HtmlFlow::Block;
	HtmlNameBuffer Buffer;
	// 小文字化したタグ名
	const std::string_view Tag = HtmlLowerName(HtmlTagName(Src, Child), Buffer);
	// 既定の表示形式に依る区分の返戻（未知の要素は行内表示としての取扱）
	return NodeKind::HtmlBlockTag.Contains(Tag) ?
	HtmlFlow::Block :
	NodeKind::HtmlHiddenTag.Contains(Tag) ? HtmlFlow::Hidden : HtmlFlow::Inline;
}

/**
 * HTML の子の内容の終端取得関数
 * 終了タグを持たない要素（空要素 `<br>`・暗黙に閉じる要素）は，構文解析器が後続の本文と空白を次のタグ迄範囲に含める
 * 末尾の空白は要素の外（親の子の境界）に在る物として扱う為，最後の子の終端迄を内容とする
 * 逐語保持要素（`<pre>` / `<textarea>` 等）は末尾の空白も内容の為，終了タグが無くても要素の終端迄を内容とする
 * @param Src ソースコード
 * @param Node 対象の子
 * @return 内容の終端バイト
 */
uint32_t StructurePass::HtmlContentEnd(const TSSource &Src, TSNode Node) {
	// 終了タグを持たない要素の実内容末尾への下降
	while(std::string_view(ts_node_type(Node)) == "element" && HtmlVerbatimOf(Src, Node) == HtmlVerbatim::None) {
		// 対象要素の個数
		const uint32_t Count = ts_node_child_count(Node);
		if(!Count) break;
		// 末尾の節点
		const TSNode Last = ts_node_child(Node, Count - 1);
		if(std::string_view(ts_node_type(Last)) == "end_tag") break;
		// 対象節点の確定
		Node = Last;
	}
	// 内容の終端の返戻
	return Src.End(Node);
}

/**
 * HTML の子の境界の区分算出関数
 * 行を区切る子の前後と，行を区切る容器の内側の端の空白は行頭・行末に当たり描画されない為，改行を置ける
 * 其れ以外の境界の空白は１個の空白として描画される為，有無を保つ（改行を置くと空白が生じ，空白を除くと語が繋がる）
 * 箱を作らない子の前後の空白は更に隣の子の空白と合わせて描画される為，箱を作らない子を越えて判定する
 * @param Src ソースコード
 * @param Container 子を持つノード (element / document)
 * @param Out 子の間の境界の区分（添字 i は i 番目と i + 1 番目の子の間）
 */
void StructurePass::CollectHtmlBoundaries(const TSSource &Src, const TSNode Container, std::vector<HtmlBoundary> &Out) {
	// 親要素の HTML 文脈
	const HtmlContext Context = HtmlContextOf(Src, Container);
	HtmlNameBuffer Buffer;
	// 文書根と行区切り要素に於ける内側端の描画空白除外
	const bool IsBlockContainer = std::string_view(ts_node_type(Container)) != "element" || Context == HtmlContext::Figure ||
	Context == HtmlContext::Html && NodeKind::HtmlBlockTag.Contains(HtmlLowerName(HtmlTagName(Src, Container), Buffer));
	// 整形対象の子節点列
	std::vector<TSNode> Children;
	std::vector<HtmlFlow> Heads, Tails;
	ForEachChild(
		Container,
		[&](const TSNode Child) -> void {
			// 整形対象の子節点列への追加
			Children.push_back(Child);
			HtmlFlow Head = HtmlFlowOf(Src, Child, Context), Tail = Head;
			if(
				const uint32_t ChildCount = ts_node_child_count(Child);
				std::string_view(ts_node_type(Child)) == "element" && ChildCount > 1
			) if(const TSNode Last = ts_node_child(Child, ChildCount - 1); std::string_view(ts_node_type(Last)) != "end_tag") {
				// 末尾の参照型節点の確定
				Tail = HtmlFlowOf(Src, Last, Context);
				if(Head == HtmlFlow::Hidden) Head = HtmlFlowOf(Src, ts_node_child(Child, 1), Context);
			}
			// 各子節点の先頭表示種別への追加
			Heads.push_back(Head);
			// 各子節点の末尾表示種別への追加
			Tails.push_back(Tail);
		}
	);
	// 対象要素の個数
	const size_t Count = Children.size();
	Out.assign(Count ? Count - 1 : 0, HtmlBoundary::Tight);
	// 各子の位置から前後へ見た最寄の箱を作る子の区分（無ければ容器の端）
	std::vector<HtmlFlow> Before(Count), After(Count);
	HtmlFlow Nearest = HtmlFlow::Edge;
	for(size_t Idx = 0; Idx < Count; ++Idx) {
		if(Tails[Idx] != HtmlFlow::Hidden) Nearest = Tails[Idx];
		// 直前側の表示種別列の更新
		Before[Idx] = Nearest;
	}
	// 直近の表示種別の更新
	Nearest = HtmlFlow::Edge;
	for(size_t Idx = Count; Idx--;) {
		if(Heads[Idx] != HtmlFlow::Hidden) Nearest = Heads[Idx];
		// 直後側の表示種別列の更新
		After[Idx] = Nearest;
	}
	for(size_t Idx = 0; Idx + 1 < Count; ++Idx) {
		// 字句間隙の開始位置
		const uint32_t GapStart = HtmlContentEnd(Src, Children[Idx]), GapEnd = Src.Start(Children[Idx + 1]);
		const std::string_view Gap(Src.data() + GapStart, GapEnd > GapStart ? GapEnd - GapStart : 0);
		const HtmlFlow Left = Before[Idx], Right = After[Idx + 1];
		// 描画される空白（空白・タブ・改行）以外の文字は内容其の物の為，原文の儘保持
		if(Gap.find_first_not_of(" \t\n\r") != std::string_view::npos) Out[Idx] = HtmlBoundary::Verbatim;
		else if(
			Left == HtmlFlow::Block || Right == HtmlFlow::Block || IsBlockContainer && (Left == HtmlFlow::Edge || Right == HtmlFlow::Edge)
		) Out[Idx] = HtmlBoundary::Break;
		else if(!Gap.empty()) Out[Idx] = HtmlBoundary::Space;
	}
	// 終了
	return;
}

/**
 * 隠れた開き括弧の判定関数
 * C# の修飾子付暗黙型ラムダ仮引数 `(ref x) => x` の `(` は文法が子に出さず，子の間の原文にだけ残る
 * @param Src ソースコード
 * @param From 間隙の開始位置（直前の子の終端，先頭の子ならノードの開始）
 * @param To 間隙の終了位置（子の開始）
 * @return 間隙に隠れた開き括弧が在れば true
 */
bool StructurePass::HasHiddenOpenParen(const TSSource &Src, const uint32_t From, const uint32_t To) {
	// 節点間の原文字列
	std::string_view Gap(Src.data() + From, To > From ? To - From : 0);
	TextEdit::TrimView(Gap);
	// 間隙が開き括弧だけかの返戻
	return Gap == "(";
}

/**
 * 解析器が読み飛ばした字句の判定関数
 * 誤り回復で節点に含まれない字句を検出する
 * @param Src ソースコード
 * @param From 間隙の開始位置
 * @param To 間隙の終了位置
 * @return 間隙に空白以外の字が在れば true
 */
bool StructurePass::HasSkippedText(const TSSource &Src, const uint32_t From, const uint32_t To) {
	// 空白以外の字を含むかの返戻
	return From < To && std::string_view(Src.data() + From, To - From).find_first_not_of(" \t\r\n") != std::string_view::npos;
}

/**
 * 間隙逐語複写追記関数
 * @param Out 追記対象
 * @param Src ソースコード
 * @param Prev 間隙の直前のノード
 * @param Next 間隙の直後のノード
 */
void StructurePass::AppendGapVerbatim(std::string &Out, const TSSource &Src, const TSNode Prev, const TSNode Next) {
	// 前の節点の末尾の区切り `;`は構造化で落ちる為，文の区切りを保つ様に復元
	if(
		// 直前節点の終端位置
		const uint32_t PrevEnd = Src.End(Prev);
		PrevEnd > Src.Start(Prev) && Src[PrevEnd - 1] == ';' && !Out.empty() && Out.back() != ';' && Out.back() != '\n'
	) Out += ';';
	if(const uint32_t GapStart = Src.End(Prev), GapEnd = Src.Start(Next); GapStart < GapEnd) {
		// 結果の格納先への字面追加
		Out.append(Src.data() + GapStart, GapEnd - GapStart);
	}
	// 終了
	return;
}

/**
 * HTML の子の境界の区切り追記関数
 * @param Out 追記先
 * @param Src ソースコード
 * @param Prev 境界の前の子
 * @param Next 境界の後の子
 * @param Kind 境界の区分
 * @param IsExpanded 空白を置ける境界へ改行を置く場合 true（平坦化では描画されない境界を詰め，描画される空白を１個にする）
 */
void StructurePass::AppendHtmlBoundary(
	std::string &Out,
	const TSSource &Src,
	const TSNode Prev,
	const TSNode Next,
	const HtmlBoundary Kind,
	const bool IsExpanded
) {
	// 境界種別毎の区切りの追記
	switch(Kind) {
	case HtmlBoundary::Verbatim:
		if(const uint32_t GapStart = HtmlContentEnd(Src, Prev), GapEnd = Src.Start(Next); GapStart < GapEnd) {
			// 結果の格納先への字面追加
			Out.append(Src.data() + GapStart, GapEnd - GapStart);
		}
		break;
	// 描画される空白は改行でも１個の空白として描画される為，展開時は改行を配置
	case HtmlBoundary::Space:
		// 同じ表示空白を保つ区切りの選択
		Out += IsExpanded ? '\n' : ' ';
		break;
	case HtmlBoundary::Break:
		if(IsExpanded) Out += '\n';
		break;
	default:
		break;
	}
	// 終了
	return;
}

/** ========== 平坦組立 ========== */
/**
 * １行テキスト生成関数
 * @param Src ソースコード
 * @param Node 対象ノード
 * @param Language 対象言語
 * @param Out 追記先（最上位呼出側が必要容量を reserve 済の事）
 */
void StructurePass::BuildFlatFromAST(const TSSource &Src, const TSNode Node, const Lang Language, std::string &Out) {
	// 追記先引数 Out への直接追記（返戻文字列方式の部分木複写と全段の文字列確保の回避）
	if(ts_node_is_null(Node)) return;
	// 再帰深度の監視値
	const RecursionGuard Guard;
	if(Guard.IsOverflow) {
		Parallel::RunOnFreshStack(
			[&]() -> void {
				BuildFlatFromAST(Src, Node, Language, Out);
			}
		);
		// 深いネストの続きを新しい走脈で組み立てて終了
		return;
	}
	// 節点型の字面
	const std::string_view TypeView(ts_node_type(Node));
	// 子から再構築出来ない節点と空白が描画へ出る JSX 要素の原文保持
	if(
		NodeKind::VerbatimNode.Contains(TypeView) || Language == Lang::Kotlin && NodeKind::KotlinImportLeaf.Contains(TypeView) ||
		NodeKind::JsxContainer.Contains(TypeView) && TSSource::HasDisputedJsxText(Src, Node)
	) {
		// 結果の格納先への字面追加
		Out.append(Src.View(Node));
		// 逐語維持節点の原文追記後の終了
		return;
	}
	// HTML の `<pre>` / `<textarea>` 等は内容の空白・字面が値又は描画に現れる為，内容を原文の儘保持
	if(
		// 間隙の境界種別
		const HtmlVerbatim Kind = Language == Lang::HTML && TypeView == "element" ? HtmlVerbatimOf(Src, Node) : HtmlVerbatim::None;
		Kind != HtmlVerbatim::None
	) {
		// 逐語保持範囲の出力への追加
		AppendHtmlVerbatim(Out, Src, Node, Kind);
		// 逐語保持要素の追記で終了
		return;
	}
	// テンプレート文字列・テンプレート挿入は読み飛ばし集合に在るが内部式の空白正規化が必要な為，再構築走査対象への限定
	const bool IsTemplateString = TypeView == "template_string";
	const bool ShouldRecurseTemplate = IsTemplateString || TypeView == "template_substitution" && !HasChildOf(
		Node,
		[](const TSNode Child) -> bool {
			// コメントの子かの返戻
			return NodeKind::Comment.Contains(Child);
		}
	);
	// 型引数・型仮引数・テンプレート文字列・Ruby の連結文字列は葉集合に在るが内部正規化の為，再帰必要
	if(
		const bool ShouldRecurseLeaf =
		NodeKind::AngleBracketList.Contains(TypeView) || IsTemplateString || Language == Lang::Ruby && TypeView == "chained_string";
		TypeView == "jsx_text" ||
		NodeKind::Skip.Contains(TypeView) && !NodeKind::StripEq.Contains(TypeView) && !NodeKind::NormEq.Contains(TypeView) &&
		!NodeKind::NormColon.Contains(TypeView) && !ShouldRecurseTemplate || IsVerbatimLeaf(TypeView, Language) && !ShouldRecurseLeaf
	) {
		// 結果の格納先への字面追加
		Out.append(Src.View(Node));
		// JSX の本文・読み飛ばし集合・葉のノードは原文追記で終了
		return;
	}
	// 行単位の前処理指令の専用組立
	if(NodeKind::Preproc.Contains(TypeView)) {
		// 結果の格納先への追加
		Out += BuildPreprocText(Src.View(Node));
		// プリプロセッサ行の整形済字面追記後の終了
		return;
	}
	// 字面が値に為る C 系マクロの原文保持
	if(
		NodeKind::Unformattable.Contains(TypeView) && !(TypeView == "template_type" && Language.IsCFamily()) &&
		!ShouldRecurseTemplate && !IsKotlinImplicitCtor(Src, Node, TypeView, Language) ||
		Language.IsCFamily() && (Src.StringizesArguments(Node) || HoldsBrokenDirective(Src, Node)) ||
		HoldsLopsidedOperator(Src, Node, Language)
	) {
		// 結果の格納先への字面追加
		Out.append(Src.View(Node));
		// 整形不能・字面の儘の節点は原文追記で終了
		return;
	}
	// 参照先の字面を守る引数処理への振分
	if(Language == Lang::CSS && Src.IsCssUrlArguments(Node)) {
		// 結果の格納先への追加
		Out += CssUrlArgumentsText(Src, Node);
		// CSS の `url(...)` の中身の参照先の原文追記で終了
		return;
	}
	// 子節点の個数
	const uint32_t ChildCount = ts_node_child_count(Node);
	if(!ChildCount) {
		// 結果の格納先への字面追加
		Out.append(Src.View(Node));
		// 子無節点の原文追記後の終了
		return;
	}
	if(ChildCount == 1) {
		const TSNode Only = ts_node_child(Node, 0);
		// HTML 要素の単一子開始タグは末尾改行等で範囲が完全一致しない事が在るが内部の空白正規化が必要
		if(!HasSameByteRange(Src, Only, Node) && !NodeKind::HtmlElementWrap.Contains(TypeView)) {
			// 結果の格納先への字面追加
			Out.append(Src.View(Node));
			// 範囲不一致の単一子ノードは原文追記で終了
			return;
		}
		// 単一名前付子なら再帰展開，無名トークン子なら原文を其のまま追記
		if(ts_node_is_named(Only)) BuildFlatFromAST(Src, Only, Language, Out);
		else Out.append(Src.View(Node));
		// 単一子の処理を終えて終了
		return;
	}
	// 行区切りを補う要素列の分類
	const bool IsGoMemberList = Language == Lang::Go && NodeKind::GoMemberList.Contains(TypeView);
	const bool IsSwiftEnumEntry = Language == Lang::Swift && TypeView == "enum_entry";
	const bool IsCsLambda = Language == Lang::CSharp && TypeView == "lambda_expression";
	const bool IsRubyStatements = Language == Lang::Ruby && NodeKind::RubyStatementSequence.Contains(TypeView);
	std::vector<HtmlBoundary> HtmlGaps;
	if(Language == Lang::HTML && TypeView == "element") CollectHtmlBoundaries(Src, Node, HtmlGaps);
	// 子の境界判定に使う直前子と走査位置
	uint32_t LastEnd = Src.Start(Node);
	TSNode LastChild = {};
	uint32_t LastIndex = 0;
	bool HasLast = false;
	uint32_t ChildIdx = 0;
	std::string_view LastTypeView;
	bool IsAfterControlError = false;
	const bool HasError = ts_node_has_error(Node);
	ForEachChild(
		Node,
		[&](const TSNode Child) -> void {
			// 現在節点の兄弟索引
			const uint32_t MyIdx = ChildIdx++;
			// 既に出力した範囲と重なる子の除外
			if(Src.Start(Child) < LastEnd) return;
			// 名前付節点か
			const bool IsNamed = ts_node_is_named(Child);
			std::string_view UnnamedView;
			if(!IsNamed) {
				// 無名字句の字面の確定
				UnnamedView = Src.View(Child);
				// 条件成立時の返戻
				if(UnnamedView.empty() || (IsGoMemberList || IsSwiftEnumEntry) && UnnamedView == ";") return;
			}
			// 子の型名は境界判定・HTML 属性判定・直前子型の引継で共有する為，此処で１回だけ取得
			const std::string_view ChildTypeView(ts_node_type(Child));
			const size_t SepStart = Out.size();
			// 構文木に現れない開始括弧の補完と仮引数への密着
			if(IsCsLambda && HasHiddenOpenParen(Src, LastEnd, Src.Start(Child))) {
				if(HasLast) Out += ' ';
				// 結果の格納先への追加
				Out += '(';
			} else if(HasLast) {
				// `ERROR` に接する境界の原文に依る逐語複写
				if(
					LastTypeView == "ERROR" || ChildTypeView == "ERROR" || IsAfterControlError ||
					HasError && HasSkippedText(Src, LastEnd, Src.Start(Child))
				) AppendGapVerbatim(Out, Src, LastChild, Child);
				else if(!HtmlGaps.empty()) AppendHtmlBoundary(Out, Src, LastChild, Child, HtmlGaps[MyIdx - 1], false);
				// Ruby の文と Go の成員間の改行を文区切りで代替
				else if((IsRubyStatements || IsGoMemberList) && IsNamed && ts_node_is_named(LastChild)) Out += "; ";
				// Swift のコンパイラ指示子と後続節点の改行区切り
				else if(Language == Lang::Swift && LastTypeView == "directive") Out += '\n';
				// CSS・SCSS で値へ貼り付いた後続宣言コロンでの境界分割
				else if(
					Language == Lang::CSS && TypeView == "declaration" && ChildTypeView == "plain_value" && LastTypeView != "property_name" &&
					Src.View(Child).ends_with(':')
				) Out += ";\n";
				// 前処理指令ブロックの終端指令行後の改行配置
				else if(
					NodeKind::PreprocBlock.Contains(LastTypeView) || NodeKind::Preproc.Contains(ChildTypeView) && Src[Src.Start(Child)] == '#' ||
					IsNamed && NodeKind::ElseLikeClause.Contains(ChildTypeView) && TypeView != "guard_statement" && Out.back() != '}'
				) Out += '\n';
				else if(NeedsGapBetween(Src, Node, TypeView, LastChild, LastTypeView, Child, ChildTypeView, LastIndex, Language)) Out += ' ';
			}
			if(IsNamed) {
				const size_t ChildStart = Out.size();
				BuildFlatFromAST(Src, Child, Language, Out);
				if(Out.size() == ChildStart) {
					// 空の子の区切りを取消し，直前子情報を保持
					Out.resize(SepStart);
					// 終了
					return;
				}
			} else Out.append(UnnamedView.data(), UnnamedView.size());
			// 次の境界判定へ渡す直前子情報の更新
			LastEnd = Src.End(Child);
			// 末尾の子節点の確定
			LastChild = Child;
			// 末尾節点型の字面の確定
			LastTypeView = ChildTypeView;
			// 末尾節点の兄弟索引の確定
			LastIndex = MyIdx;
			// 末尾節点の保持状態の設定
			HasLast = true;
			// 制御見出直後の解析エラー保持有無の更新
			IsAfterControlError = IsAfterControlError || ChildTypeView == "ERROR" && NodeKind::Control.Contains(TypeView);
		}
	);
	// 解析失敗部分木が末尾へ残した無所属字句の補完
	if(HasLast && LastEnd < Src.End(Node) && ts_node_has_error(Node)) Out.append(Src.data() + LastEnd, Src.End(Node) - LastEnd);
	// Kotlin の空 for 本体は文法が区切りを節点外へ置く為，後続文を本体にしないよう補完
	if(
		Language == Lang::Kotlin && TypeView == "for_statement" &&
		ts_node_is_null(FirstNamedChildOfType(Node, "control_structure_body"))
	) Out += ';';
	// 子の連結を終えて終了
	return;
}

/**
 * フラットテキストキャッシュ取得関数
 * @param Src ソースコード
 * @param Node 対象ノード
 * @param Language 対象言語
 * @param FlatByNode フラットテキストのキャッシュ
 * @return フラットテキストへの参照
 */
const std::string &StructurePass::GetFlatText(
	const TSSource &Src,
	const TSNode Node,
	const Lang Language,
	NodeTextMap &FlatByNode
) {
	// 節点識別子に対応する本文格納先の取得
	const auto [Iter, Inserted] = FlatByNode.emplace(Node.id, std::string());
	// `BuildFlatFromAST` は `FlatByNode` に触れない為，再帰中の再ハッシュは発生を避けたキャッシュ枠への直接構築
	if(Inserted && Src.Len(Node)) {
		Iter->second.reserve(Src.Len(Node));
		BuildFlatFromAST(Src, Node, Language, Iter->second);
	}
	// キャッシュ参照済のフラットテキストの返戻
	return Iter->second;
}

/**
 * ヒアドキュメント本体の本文の取得関数
 * ヒアドキュメント開始行の改行を重ねず本文の値を保つ
 * 改行様式が混在する入力は本文の CRLF を保つ為，先頭の改行も CRLF で有り得る
 * @param Src ソースコード
 * @param Body ヒアドキュメント本体
 * @return 先頭の改行を除いた本体の原文
 */
std::string_view StructurePass::HeredocBodyText(const TSSource &Src, const TSNode Body) {
	// 変換前の字面
	const std::string_view Raw = Src.View(Body);
	// 先頭の改行を除いた本文の返戻
	return Raw.substr(Raw.starts_with("\r\n") ? 2 : Raw.starts_with('\n') ? 1 : 0);
}

/**
 * 全名前付子コメント判定関数
 * @param Node 対象ノード
 * @return 全名前付子がコメントなら true（空ノードも含む）
 */
bool StructurePass::AreAllNamedChildrenComments(const TSNode Node) {
	// コメントでない名前付子が無い事の返戻
	return !HasChildOf(
		Node,
		[](const TSNode Child) -> bool {
			// コメントでない名前付子かの返戻
			return ts_node_is_named(Child) && !NodeKind::Comment.Contains(Child);
		}
	);
}

/**
 * 全名前付子コメント判定関数（キャッシュ版）
 * @param Node 対象ノード
 * @param Cache 共有キャッシュ
 * @return 全名前付子がコメントなら true（空名前付子も含む）
 */
bool StructurePass::AreAllNamedChildrenCommentsCached(const TSNode Node, ContainsStmtBlockMap &Cache) {
	// 控えへの新規登録有無
	const auto [Iter, Inserted] = Cache.emplace(Node.id, 0);
	if(!(Iter->second & OnlyCommentsComputedBit)) {
		// 節点別判定控えの更新
		Iter->second |= static_cast<uint8_t>(OnlyCommentsComputedBit | (AreAllNamedChildrenComments(Node) ? OnlyCommentsBit : 0));
	}
	// キャッシュ済の判定結果の返戻
	return Iter->second & OnlyCommentsBit;
}

/**
 * JSX 内容子ノード数取得関数
 * @param Src ソースコード
 * @param Node 対象ノード
 * @return 内容子ノード数（空白のみの本文の並びは除外，全呼出側の閾値判定が「２以上か」のみの為，上限２で打ち切る）
 */
uint32_t StructurePass::JsxContentCount(const TSSource &Src, const TSNode Node) {
	// 対象要素の個数
	uint32_t Count = 0;
	bool IsInText = false, IsTextCounted = false;
	const auto BlankText = [&Src](std::string_view Text) -> bool {
		// 挿入する空白文字
		const std::string_view Space = Src.GetJsxBlankMark(' '), Tab = Src.GetJsxBlankMark('\t');
		while(!Text.empty()) {
			if(Text.front() == ' ' || Text.front() == '\t' || Text.front() == '\n') Text.remove_prefix(1);
			else if(Text.starts_with(Space)) Text.remove_prefix(Space.size());
			else if(Text.starts_with(Tab)) Text.remove_prefix(Tab.size());
			// 其の他の字を含む事の返戻
			else return false;
		}
		// 空白と印だけの本文の返戻
		return true;
	};
	ForEachNamedChild(
		Node,
		[&](const TSNode Child) -> bool {
			const std::string_view ChildType = ts_node_type(Child);
			const bool IsText = NodeKind::JsxTextLike.Contains(ChildType);
			if(!IsText || !IsInText) IsTextCounted = false;
			// 文字列内容の走査状態の確定
			IsInText = IsText;
			// 空白のみの本文（行端の空白を表す文字参照を含む）と空の式は展開要因として計数除外
			if(
				!IsTextCounted && !(
					NodeKind::JsxHtmlTagOpenClose.Contains(ChildType) || ChildType == "jsx_text" && BlankText(Src.View(Child)) ||
					ChildType == "jsx_expression" && !ts_node_named_child_count(Child)
				)
			) {
				// 対象要素の個数の進行
				++Count;
				// 現在文字の集計済有無の更新
				IsTextCounted = IsText;
			}
			// ２到達で閾値判定が確定する為，走査打切の返戻
			return Count < 2;
		}
	);
	// 内容子ノード数の返戻
	return Count;
}

/**
 * Kotlin の `->` の後の値の if 式の一括判定関数（`else` を次の行へ置くと tree-sitter-kotlin が `else "b"` を when の枝と読み違える）
 * else if の連鎖の内側の if 式は，先に巡った連鎖の外の if 式の判定を引き継ぐ
 * @param Top 巡る部分木の根（最上位の子）
 * @param TopParent Top の親
 * @param Cache 判定の控え（当たる if 式に印を書く）
 */
void StructurePass::MarkKotlinArrowBodies(const TSNode Top, const TSNode TopParent, ContainsStmtBlockMap &Cache) {
	// 部分木の先行順による単一走査
	HeldCursor Cursor(Top);
	std::vector<TSNode> Path { TopParent };
	for(bool IsDone = false; !IsDone;) {
		// 対象節点
		const TSNode Node = ts_tree_cursor_current_node(&Cursor);
		if(std::string_view(ts_node_type(Node)) == "if_expression") {
			// 対象の親節点
			const TSNode Parent = Path.back(), Host = Path.size() > 1 ? Path[Path.size() - 2] : TSNode{};
			const std::string_view ParentType = ts_node_type(Parent);
			const std::string_view HostType = ts_node_is_null(Host) ? std::string_view() : ts_node_type(Host);
			const ContainsStmtBlockMap::const_iterator Outer = Cache.find(Host.id);
			if(
				ParentType == "control_structure_body" &&
				(HostType == "if_expression" ? Outer != Cache.end() && Outer->second & KotlinArrowBit : HostType == "when_entry") ||
				ParentType == "statements" && HostType == "lambda_literal" && ts_node_named_child_count(Parent) == 1
			) Cache[Node.id] |= KotlinArrowBit;
		}
		if(ts_tree_cursor_goto_first_child(&Cursor)) {
			// 祖先へ遡る節点列への追加
			Path.push_back(Node);
			continue;
		}
		while(!ts_tree_cursor_goto_next_sibling(&Cursor)) {
			if(Path.size() == 1 || !ts_tree_cursor_goto_parent(&Cursor)) {
				// 構文木走査の完了状態の設定
				IsDone = true;
				break;
			}
			// 祖先へ遡る節点列からの末尾要素取出
			Path.pop_back();
		}
	}
	// 終了
	return;
}

/**
 * Kotlin の `->` の後の値の if 式かの判定関数
 * @param Node if_expression ノード
 * @param Cache 判定の控え（MarkKotlinArrowBodies が印を書いた物）
 * @return when の枝・ラムダの値の if 式なら true
 */
bool StructurePass::IsKotlinArrowBody(const TSNode Node, const ContainsStmtBlockMap &Cache) {
	// 判定控えの検索結果
	const ContainsStmtBlockMap::const_iterator Hit = Cache.find(Node.id);
	// 印の有無の返戻
	return Hit != Cache.end() && Hit->second & KotlinArrowBit;
}

/**
 * 展開要因包含判定関数
 * @param Src ソースコード
 * @param Node 判定対象ノード
 * @param Language 対象言語
 * @param Cache 共有キャッシュ
 * @return 展開要因が含まれて居れば true
 */
bool StructurePass::ContainsStmtBlock(
	const TSSource &Src,
	const TSNode Node,
	const Lang Language,
	ContainsStmtBlockMap &Cache
) {
	// 再帰深度の監視値
	const RecursionGuard Guard;
	if(Guard.IsOverflow) {
		bool IsDeep = false;
		Parallel::RunOnFreshStack(
			[&]() -> void {
				// 深い構造の保持有無の更新
				IsDeep = ContainsStmtBlock(Src, Node, Language, Cache);
			}
		);
		// 新しい走脈で判定した結果の返戻
		return IsDeep;
	}
	// 控えへの新規登録有無
	const auto [Iter, Inserted] = Cache.emplace(Node.id, 0);
	uint8_t &Entry = Iter->second;
	// 算出済の結果の返戻
	if(Entry & ContainsComputedBit) return Entry & ContainsBit;
	// 値を返す式文脈か
	bool IsResult = false;
	const std::string_view TypeView(ts_node_type(Node));
	// 逐語保持要素と文字列化される実引数の内部展開要因探索からの除外
	if(
		Language == Lang::HTML && TypeView == "element" && HtmlVerbatimOf(Src, Node) != HtmlVerbatim::None ||
		Language.IsCFamily() && Src.StringizesArguments(Node)
	) {
		// 節点別判定控えの更新
		Entry |= ContainsComputedBit;
		// 展開要因を持たない事の返戻
		return false;
	}
	// 名前付子節点の個数
	const uint32_t NamedCount = ts_node_named_child_count(Node);
	if(!TypeView.empty()) {
		// Swift の switch と Java 列挙宣言部
		const bool IsBraceLessControlBody = TypeView == "control_structure_body" && [&Node]() -> bool {
			// 先頭の対象要素
			const TSNode First = ts_node_child(Node, 0);
			// 波括弧を持たない事の返戻
			return ts_node_is_null(First) || std::string_view(ts_node_type(First)) != "{";
		}();
		const TSNode RubyBlockBody = Language == Lang::Ruby && TypeView == "block" ?
		FirstNamedChildOfType(Node, "block_body") :
		Language == Lang::Ruby && TypeView == "block_body" ? Node : TSNode{};
		const bool IsRubyInlineBlock = Language == Lang::Ruby && (
			TypeView == "lambda" ||
			NodeKind::RubyBraceBlock.Contains(TypeView) && (ts_node_is_null(RubyBlockBody) || ts_node_named_child_count(RubyBlockBody) < 2)
		);
		if(
			(
				NodeKind::IndentContainer.Contains(TypeView) && !NodeKind::ObjectLike.Contains(TypeView) &&
				(Language == Lang::Ruby || !NodeKind::RubyClassDefLike.Contains(TypeView)) || TypeView == "enum_body_declarations" ||
				Language == Lang::Ruby && NodeKind::RubyBlock.Contains(TypeView) || Language == Lang::Swift && TypeView == "switch_statement" ||
				NodeKind::DoLoop.Contains(TypeView)
			) && !IsBraceLessControlBody && !IsRubyInlineBlock && NamedCount && !AreAllNamedChildrenCommentsCached(Node, Cache) ||
			Language == Lang::Ruby && TypeView == "heredoc_body" ||
			Language == Lang::Kotlin && TypeView == "if_expression" && HasUnnamedTokenChild(Src, Node, "else") &&
			!IsKotlinArrowBody(Node, Cache) || (
				Language == Lang::Kotlin && NodeKind::KotlinBraceBodyHost.Contains(TypeView) ||
				Language == Lang::Swift && NodeKind::SwiftBraceBodyHost.Contains(TypeView)
			) && !ts_node_is_null(FirstNamedChildOfType(Node, "statements")) ||
			NodeKind::JsxContainer.Contains(TypeView) && JsxContentCount(Src, Node) > 1
		) IsResult = true;
		else if(Language == Lang::HTML && TypeView == "element") {
			// 二個以上の子要素を持つ HTML 要素の展開対象化
			uint32_t ElemChildren = 0;
			ForEachNamedChild(
				Node,
				[&](const TSNode Child) -> bool {
					if(NodeKind::HtmlElementWrap.Contains(Child)) ++ElemChildren;
					// ２到達で展開対象が確定する為，走査打切の返戻
					return ElemChildren < 2;
				}
			);
			if(ElemChildren > 1) {
				// 保持する間隙文字列の列
				std::vector<HtmlBoundary> Gaps;
				CollectHtmlBoundaries(Src, Node, Gaps);
				// 開始タグの直後と終了タグの直前は子の間ではない為，計数除外
				const std::vector<HtmlBoundary>::const_iterator Last =
				Gaps.cend() - (std::string_view(ts_node_type(ts_node_child(Node, ts_node_child_count(Node) - 1))) == "end_tag");
				IsResult = std::any_of(
					Gaps.cbegin() + 1,
					Last,
					[](const HtmlBoundary Kind) -> bool {
						// 改行を置ける境界かの返戻
						return Kind == HtmlBoundary::Break || Kind == HtmlBoundary::Space;
					}
				);
			}
		}
	}
	// 子孫に展開要因を持つ部分木の展開対象化
	if(!IsResult && NamedCount) {
		IsResult = HasChildOf(
			Node,
			[&](const TSNode Child) -> bool {
				// 展開要因を含む名前付子かの返戻
				return ts_node_is_named(Child) && ContainsStmtBlock(Src, Child, Language, Cache);
			}
		);
	}
	// 節点別判定控えの確定
	Entry = static_cast<uint8_t>(Entry | ContainsComputedBit | (IsResult ? ContainsBit : 0));
	// 展開要因の有無の返戻
	return IsResult;
}

/**
 * ヒアドキュメント本体追記関数
 * @param Dst 追記対象
 * @param Src ソースコード
 * @param Child ヒアドキュメント本体ノード
 * @return 生テキストが空でなく追記された場合 true，空で読み飛ばした場合 false
 */
bool StructurePass::AppendHeredocBody(std::string &Dst, const TSSource &Src, const TSNode Child) {
	// 空ヒアドキュメントの為，読み飛ばす旨の返戻
	if(!Src.Len(Child)) return false;
	// 本文で始まる文字列の行頭空白を保つ前置改行印
	if(Dst.empty() || Dst.back() != '\n') Dst += '\n';
	// 出力文字列への字面追加
	Dst.append(HeredocBodyText(Src, Child));
	if(Dst.back() != '\n') Dst += '\n';
	// 追記成功の返戻
	return true;
}

/**
 * 前後空白除去ビュー取得関数
 * @param Text 元の文字列（返戻ビューの参照先の為，使用中は生存必須）
 * @return 前後空白除去済のビュー
 */
std::string_view StructurePass::TrimmedView(const std::string &Text) {
	// 対象範囲の字面
	std::string_view View(Text);
	// ヒアドキュメントの本文で始まる文字列は，本文の行頭の空白が値の為，其の改行だけを除外
	if(View.starts_with('\n')) {
		// 対象範囲の字面からの先頭区切り除去
		View.remove_prefix(1);
		while(!View.empty() && (View.back() == ' ' || View.back() == '\t' || View.back() == '\n')) View.remove_suffix(1);
		// 本文のインデントを保ったビューの返戻
		return View;
	}
	TextEdit::TrimView(View);
	// 前後空白除去済のビューの返戻
	return View;
}

/**
 * 展開閉じ括弧後の改行判定関数
 * 展開閉じ括弧後の字句を同じ行へ続けるか判定する
 * 許可する字句を列挙すると列挙漏れの字句が次行へ落ち，演算子が行頭に来たり `} as const` の様に構文が壊れたりする
 * @param Text 構築済のテキスト
 * @param Next 後続のテキスト
 * @param Language 対象言語
 * @return 後続の閉じ括弧を次行へ送る改行が必要なら true
 */
bool StructurePass::ShouldBreakAfterExpandedBrace(
	const std::string_view Text,
	const std::string_view Next,
	const Lang Language
) {
	// Go は閉じ波括弧後の改行が自動セミコロン挿入を招く為，送らない（閉じ括弧を次行へ置く形は行分割が末尾コンマを補って生成）
	if(Language == Lang::Go || !Text.ends_with("\n}") || Next.empty()) return false;
	// 後続が閉じ括弧かの返戻
	return IsCloseBracketChar(Next.front());
}

/** ========== 展開組立 ========== */
/**
 * 引数列・JSX 式等展開形式構築関数
 * @param Src ソースコード
 * @param Node 対象ノード
 * @param Language 対象言語
 * @param FlatByNode フラットテキストキャッシュ
 * @param StructuredByNode 構造化テキストキャッシュ
 * @param ContainsByNode 展開要因有無キャッシュ
 * @return 展開形式のテキスト
 */
std::string StructurePass::BuildExpandedContainer(
	const TSSource &Src,
	const TSNode Node,
	const Lang Language,
	NodeTextMap &FlatByNode,
	NodeTextMap &StructuredByNode,
	ContainsStmtBlockMap &ContainsByNode
) {
	// 結果の格納先
	std::string Out;
	// 結果の格納先の格納領域予約
	Out.reserve(Src.Len(Node) + 16);
	// 空白範囲の左端位置
	TSNode Left = {};
	HeldCursor Cursor(Node);
	if(ts_tree_cursor_goto_first_child(&Cursor)) {
		do {
			// 走査中の子節点
			const TSNode Child = ts_tree_cursor_current_node(&Cursor);
			const std::string_view ChildType(ts_node_type(Child));
			if(NodeKind::Comment.Contains(ChildType)) continue;
			// Ruby のヒアドキュメント本体は終端タグが独自行頭・行末必須
			if(ChildType == "heredoc_body") {
				// 出力末尾の余分な空白除去
				TrimTrailingSpaces(Out);
				// ヒアドキュメント本体の出力への追加
				AppendHeredocBody(Out, Src, Child);
				// 空白範囲の左端位置の更新
				Left = Child;
				continue;
			}
			// 取り出した構造化テキストの所有体（Tok が参照する為，使用中は生存必須）
			std::string SubText;
			std::string_view Tok;
			if(ts_node_is_named(Child)) {
				// 子節点の整形結果の確定
				SubText = TakeStructuredText(Src, Child, Language, FlatByNode, StructuredByNode, ContainsByNode);
				// 対象の字句の更新
				Tok = TrimmedView(SubText);
			} else Tok = Src.View(Child);
			if(Tok.empty()) continue;
			// 開始括弧の有無
			const bool IsOpen = Tok == "(" || Tok == "{" || Tok == "[", IsClose = Tok == ")" || Tok == "}" || Tok == "]";
			const bool IsComma = Tok == ",";
			// 出力末尾の余分な空白除去
			TrimTrailingSpaces(Out);
			// Go の複数行列は閉じ字句の前に末尾コンマが必要で，改行前に補って構文を保持
			if(Language == Lang::Go && IsClose && ts_node_is_named(Left) && NodeKind::MultilineCommaContainer.Contains(Node)) Out += ',';
			if(ShouldBreakAfterExpandedBrace(Out, Tok, Language) || IsClose && !Out.empty() && Out.back() != '\n') Out += '\n';
			else if(!IsOpen && !IsClose && !IsComma && !Out.empty() && Out.back() != '\n') Out += ' ';
			// 結果の格納先への字面追加
			Out.append(Tok);
			// 空白範囲の左端位置の更新
			Left = Child;
			if(IsOpen || IsComma) Out += '\n';
		} while(ts_tree_cursor_goto_next_sibling(&Cursor));
	}
	// 展開形式の連結テキストの返戻
	return Out;
}

/**
 * 本文を控えたヒアドキュメント開始の数の更新と最後の開始節点の取得関数
 * 部分木の開始の数を足し，本文の数を引く（本文が前の兄弟の開始に対応する形も有る為，負に為れば０）
 * 計算量：対象範囲の長さ N と節点数 V に対し O(N + V)
 * @param Open 本文を控えた開始の数（更新される）
 * @param Src ソースコード
 * @param Node 走査対象のノード（ノード自身を含む）
 * @return 最後の開始節点（開始が無ければ空節点）
 */
TSNode StructurePass::TrackOpenHeredocs(uint32_t &Open, const TSSource &Src, const TSNode Node) {
	// 本文を控えた開始が無く，開始の字句 `<<` も無ければ数は変わらない為，空節点の返戻
	if(!Open && Src.View(Node).find("<<") == std::string_view::npos) return TSNode{};
	uint32_t Begins = 0, Bodies = 0;
	TSNode Last {};
	WalkAst(
		Node,
		[&Begins, &Bodies, &Last](const TSNode Cur) -> void {
			if(const std::string_view Type(ts_node_type(Cur)); Type == "heredoc_beginning") {
				// 開始字句の該当有無の進行
				++Begins;
				// 末尾の対象節点の確定
				Last = Cur;
			} else if(Type == "heredoc_body") ++Bodies;
		}
	);
	// 未閉鎖ヒアドキュメント数の更新
	Open = Open + Begins > Bodies ? Open + Begins - Bodies : 0;
	// 最後の開始節点の返戻
	return Last;
}

/**
 * 本文を控えたヒアドキュメントの後の文の区切り追記関数
 * ヒアドキュメント開始後の文を同じ行へ区切る関数
 * @param Base 追記対象
 */
void StructurePass::AppendHeredocPendingSeparator(std::string &Base) {
	// 出力末尾の余分な空白除去
	TrimTrailingSpaces(Base);
	if(!Base.empty() && Base.back() != ';') Base += ';';
	// 組立中の文字列への追加
	Base += ' ';
	// 終了
	return;
}

/**
 * ノード末尾区切り追記関数
 * @param Text 追記対象（末尾へ区切りを足す）
 * @param Src ソースコード
 * @param Node 対象ノード（末尾直後の句読点判定に用いる）
 * @param Language 対象言語
 */
void StructurePass::AppendTrailingSeparator(std::string &Text, const TSSource &Src, const TSNode Node, const Lang Language) {
	// 名前付兄弟が所有しない無名の区切りを復元
	if(const uint32_t NodeEnd = Src.End(Node); NodeEnd < Src.size() && (Src[NodeEnd] == ';' || Src[NodeEnd] == ',')) {
		// Go・Kotlin・Swift の最上位文の改行区切り
		if((Language == Lang::Go || Language == Lang::Kotlin || Language == Lang::Swift) && Src[NodeEnd] == ';') return;
		// 後続の名前付兄弟が所有する区切りの重複追加回避
		bool IsOwnedByNext = false;
		if(
			// 後続の兄弟節点
			const TSNode NextSibling = ts_node_next_sibling(Node);
			!ts_node_is_null(NextSibling) && ts_node_start_byte(NextSibling) == NodeEnd
		) {
			if(ts_node_is_named(NextSibling)) IsOwnedByNext = true;
			else if(const std::string_view NextView = Src.View(NextSibling); NextView == ";" || NextView == ",") {
				// 前処理指令直下の無名セミコロンの親走査による出力
				if(const TSNode Parent = ts_node_parent(Node); !ts_node_is_null(Parent) && NodeKind::PreprocBlock.Contains(Parent)) {
					// 区切りの後続節点所有状態の設定
					IsOwnedByNext = true;
				} else {
					// 解析に失敗した節点の手前の間隙は字面の儘写される為，其処に在る `;` を足すと二重化
					TSNode After = ts_node_next_sibling(NextSibling);
					while(!ts_node_is_null(After) && !ts_node_is_named(After)) After = ts_node_next_sibling(After);
					// 区切りの後続節点所有有無の確定
					IsOwnedByNext = !ts_node_is_null(After) && std::string_view(ts_node_type(After)) == "ERROR";
				}
			}
		}
		if(const char Last = Text.empty() ? '\0' : Text.back(); !IsOwnedByNext && Last != ';' && Last != ',') {
			// 対象の字面の進行
			Text += static_cast<char>(Src[NodeEnd]);
		}
	}
	// 終了
	return;
}

/**
 * 複数行の逐語の葉の包含判定関数
 * ネストの前処理の塊は段毎に行を整形する為，節点毎に控えて部分木を巡り直さない
 * type_arguments / type_parameters / template_string は字句を組み直した結果の為，逐語の葉に数えず中を探す
 * @param Src ソースコード
 * @param Node 判定対象ノード（自身を含む）
 * @param Language 対象言語
 * @param Cache 共有キャッシュ
 * @return 範囲に改行を含む逐語の葉が有れば true
 */
bool StructurePass::HasMultilineVerbatimLeaf(
	const TSSource &Src,
	const TSNode Node,
	const Lang Language,
	ContainsStmtBlockMap &Cache
) {
	// 深いネストでの別走脈に依る複数行葉判定の継続
	const RecursionGuard Guard;
	if(Guard.IsOverflow) {
		// 深い構造の検出有無
		bool IsDeep = false;
		Parallel::RunOnFreshStack(
			[&]() -> void {
				// 深い構造の保持有無の更新
				IsDeep = HasMultilineVerbatimLeaf(Src, Node, Language, Cache);
			}
		);
		// 新しい走脈で判定した結果の返戻
		return IsDeep;
	}
	const auto [Iter, Inserted] = Cache.emplace(Node.id, 0);
	// 算出済の結果の返戻
	if(Iter->second & MultilineVerbatimComputedBit) return Iter->second & MultilineVerbatimBit;
	// 節点型の字面
	const std::string_view TypeView(ts_node_type(Node));
	const uint32_t StartByte = ts_node_start_byte(Node), EndByte = ts_node_end_byte(Node);
	const bool IsResult =
	IsVerbatimLeaf(TypeView, Language) && !NodeKind::AngleBracketList.Contains(TypeView) && TypeView != "template_string" ?
	StartByte < EndByte && EndByte <= Src.size() && std::memchr(Src.data() + StartByte, '\n', EndByte - StartByte) :
	HasChildOf(
		Node,
		[&](const TSNode Child) -> bool {
			// 子の部分木に複数行の逐語の葉が有るかの返戻
			return HasMultilineVerbatimLeaf(Src, Child, Language, Cache);
		}
	);
	// 再帰中の挿入で再ハッシュされても要素参照は有効（規格 [unord.req]）の為，捕捉した反復子で書込
	Iter->second |= static_cast<uint8_t>(MultilineVerbatimComputedBit | (IsResult ? MultilineVerbatimBit : 0));
	// 判定結果の返戻
	return IsResult;
}

/**
 * 構造化テキスト行整形関数
 * @param Src ソースコード
 * @param Node 対象ノード（末尾直後の句読点判定に用いる）
 * @param Language 対象言語
 * @param Line 整形対象の構造化テキスト
 * @param ContainsByNode 展開要因有無キャッシュ（複数行の逐語の葉の包含の控えを兼ねる）
 * @return 整形されたテキスト
 */
std::string StructurePass::CleanupLine(
	const TSSource &Src,
	const TSNode Node,
	const Lang Language,
	std::string Line,
	ContainsStmtBlockMap &ContainsByNode
) {
	// 複数行の逐語の葉（文字列等）を持つ場合は，行の整形が中身を壊す為，末尾の余白を除いて区切りだけを補完
	if(HasMultilineVerbatimLeaf(Src, Node, Language, ContainsByNode)) {
		while(!Line.empty() && (Line.back() == ' ' || Line.back() == '\n')) Line.pop_back();
		// 必要な末尾区切りの追加
		AppendTrailingSeparator(Line, Src, Node, Language);
		// 末尾の余白だけを除いた行の返戻
		return Line;
	}
	// 対象範囲の開始位置
	size_t Start = 0, End = Line.size();
	while(Start < End && Line[Start] == '\n') ++Start;
	while(End > Start && (Line[End - 1] == '\n' || Line[End - 1] == ' ')) --End;
	// 空白整理後の文字列
	std::string Clean;
	// 空白整理後の文字列の格納領域予約
	Clean.reserve(End - Start + 1);
	// 文字列リテラル内 (`"..."` / `'...'` / `\`...\``) の空白は意味的に保持必須
	char StringDelim = '\0';
	for(size_t Idx = Start; Idx < End; ++Idx) {
		const char Char = Line[Idx];
		// 文字列の開閉判定ではエスケープ直後を引用符と取扱除外
		if(!StringDelim && (Char == '"' || Char == '\'' || Char == '`')) StringDelim = Char;
		else if(StringDelim && Char == StringDelim && (!Idx || Line[Idx - 1] != '\\')) StringDelim = '\0';
		if(StringDelim) {
			// 空白整理後の文字列の進行
			Clean += Char;
			continue;
		}
		if(Char == '\n') {
			// 出力末尾の余分な空白除去
			TrimTrailingSpaces(Clean);
			// 空白整理後の文字列への追加
			Clean += '\n';
			continue;
		}
		if(Char == ' ' && Idx + 1 < End && Line[Idx + 1] == '\n') continue;
		// 空白整理後の文字列の進行
		Clean += Char;
	}
	// 出力末尾の余分な空白除去
	TrimTrailingSpaces(Clean);
	// 必要な末尾区切りの追加
	AppendTrailingSeparator(Clean, Src, Node, Language);
	// 末尾余白除去後のクリーン行の返戻
	return Clean;
}

/**
 * 条件付プリプロセッサブロック構造化整形関数
 * @param Src ソースコード
 * @param Node 対象プリプロセッサブロックノード
 * @param Language 対象言語
 * @param FlatByNode フラットテキストキャッシュ
 * @param StructuredByNode 構造化テキストキャッシュ
 * @param ContainsByNode 文ブロック包含キャッシュ
 * @return 構造化整形済テキスト
 */
std::string StructurePass::BuildPreprocBlockText(
	const TSSource &Src,
	const TSNode Node,
	const Lang Language,
	NodeTextMap &FlatByNode,
	NodeTextMap &StructuredByNode,
	ContainsStmtBlockMap &ContainsByNode
) {
	// 深いネスト（長い `#elif` の連鎖等）は走脈領域を使い切る前に，新しい走脈で続きを組立
	const RecursionGuard Guard;
	if(Guard.IsOverflow) {
		// 深い構造の整形結果
		std::string Deep;
		Parallel::RunOnFreshStack(
			[&]() -> void {
				// 深い構造の整形結果の更新
				Deep = BuildPreprocBlockText(Src, Node, Language, FlatByNode, StructuredByNode, ContainsByNode);
			}
		);
		// 新しい走脈で組み立てた結果の返戻
		return Deep;
	}
	// 結果の格納先
	std::string Out;
	// 結果の格納先の格納領域予約
	Out.reserve(Src.Len(Node) + 16);
	// 直前節点の保持有無
	bool HasPrev = false, IsPrevMultiLine = false, IsAwaitingCondition = false;
	std::string_view PrevKind;
	ForEachChild(
		Node,
		[&](const TSNode Child) -> void {
			const std::string_view TypeView(ts_node_type(Child)), RawText = Src.View(Child);
			if(!ts_node_is_named(Child)) {
				// 改行・空白のみの無名トークンは構造的意味を持たないので読飛し
				if(!RawText.empty() && RawText.find_first_not_of(" \t\n\r") == std::string_view::npos) return;
				// `#ifdef` / `#if` / `#ifndef` / `#elif` の開始・分岐トークンは次の条件式と連結
				if(NodeKind::PreprocDirectiveOpen.Contains(TypeView)) {
					if(HasPrev) Out += "\n\n";
					// 結果の格納先への字面追加
					Out.append(RawText);
					// 直前節点の保持状態の解除
					HasPrev = false;
					// プリプロセッサ条件の待機状態の設定
					IsAwaitingCondition = true;
					// 終了
					return;
				}
				// 単独の `#else`・`#endif` 指令の空行区切り
				if(NodeKind::PreprocDirectiveClose.Contains(TypeView)) {
					if(HasPrev) Out += "\n\n";
					// 結果の格納先への字面追加
					Out.append(RawText);
					// 直前節点の保持状態の設定
					HasPrev = true;
					// 直前要素の複数行該当状態の解除
					IsPrevMultiLine = false;
					// 直前節点の構造種別の確定
					PrevKind = "directive_line";
					// プリプロセッサ条件の待機状態の解除
					IsAwaitingCondition = false;
					// 終了
					return;
				}
				// 上記以外の無名トークンは空白を挟まず其のまま結合
				Out.append(RawText);
				// 終了
				return;
			}
			// 条件式待ちの名前付子の同一行への連結
			if(IsAwaitingCondition) {
				// 結果の格納先への追加
				Out += ' ';
				// 結果の格納先への追加
				Out += ts_node_has_error(Child) ? BuildPreprocText(RawText) : GetFlatText(Src, Child, Language, FlatByNode);
				// プリプロセッサ条件の待機状態の解除
				IsAwaitingCondition = false;
				// 直前節点の保持状態の設定
				HasPrev = true;
				// 直前要素の複数行該当状態の解除
				IsPrevMultiLine = false;
				// 直前節点の構造種別の確定
				PrevKind = "directive_line";
				// 終了
				return;
			}
			const std::string ChildText = NodeKind::PreprocBlock.Contains(TypeView) ?
			BuildPreprocBlockText(Src, Child, Language, FlatByNode, StructuredByNode, ContainsByNode) :
			NodeKind::Preproc.Contains(TypeView) ? BuildPreprocText(RawText) : CleanupLine(
				Src,
				Child,
				Language,
				TakeStructuredText(Src, Child, Language, FlatByNode, StructuredByNode, ContainsByNode),
				ContainsByNode
			);
			// 複数行構造か
			const bool IsMultiLine = ChildText.find('\n') != std::string::npos;
			if(HasPrev) {
				// 結果の格納先への追加
				Out += '\n';
				if(IsPrevMultiLine || IsMultiLine || PrevKind != TypeView) Out += '\n';
			}
			// 結果の格納先への字面追加
			Out.append(ChildText);
			// 直前節点の保持状態の設定
			HasPrev = true;
			// 直前要素の複数行該当有無の更新
			IsPrevMultiLine = IsMultiLine;
			// 直前節点の構造種別の確定
			PrevKind = TypeView;
		}
	);
	// 構造化済プリプロセッサブロックの返戻
	return Out;
}

/**
 * 終端トークン追記関数
 * @param Base 追記対象
 * @param IsMulti 多行形か（true なら改行区切，false なら空白区切）
 * @param Token 追記するトークン（通常 `end`）
 */
void StructurePass::AppendEndToken(std::string &Base, const bool IsMulti, const std::string_view Token) {
	// 複数行形の終端前改行の追加
	if(IsMulti && !Base.empty() && Base.back() != '\n') Base += '\n';
	// 空の波括弧の内側空白除去
	else if(!IsMulti && !Base.empty() && Base.back() != ' ' && Base.back() != '\n' && !(Base.back() == '{' && Token == "}")) {
		// 組立中の文字列への追加
		Base += ' ';
	}
	// 組立中の文字列への字面追加
	Base.append(Token.data(), Token.size());
	// 終了
	return;
}

/**
 * Ruby の節本体に含まれる修飾子形式の判定関数
 * @param Node Ruby の `if` / `unless` ノード
 * @return 節本体に修飾子形式の式が在る場合 true
 */
bool StructurePass::HasModifierBody(const TSNode Node) {
	// 節の中身が既に修飾子形の式なら，`then` で一行へ畳むと条件が二つ並ぶ読み解けない形への変化
	return HasChildOf(
		Node,
		[](const TSNode Child) -> bool {
			// 修飾子形の式を子に持つ文の容器かの返戻
			return ts_node_is_named(Child) && NodeKind::RubyStatementHost.Contains(Child) && HasChildOf(
				Child,
				[](const TSNode Inner) -> bool {
					// 修飾子形の式かの返戻
					return ts_node_is_named(Inner) && NodeKind::RubyModifierExpr.Contains(Inner);
				}
			);
		}
	);
}

/**
 * 改行を跨いで前の文へ繋がる文の判定関数
 * Kotlin・Swift は改行の後の `{` を前の式の末尾ラムダとして読み，tree-sitter-kotlin は改行の後の `(` `[` `::` も前の式の呼出・添字・参照へ繋ぐ
 * @param Language 対象言語
 * @param Next 次の文の構造化テキスト
 * @return 改行では区切れず `;` が要るなら true
 */
bool StructurePass::JoinsAcrossNewline(const Lang Language, const std::string_view Next) {
	// 空の文は繋がらない事の返戻
	if(Next.empty()) return false;
	// 次の文の始まりが前の式へ繋がるかの返戻
	return Language.JoinsBraceAcrossNewline() && Next.front() == '{' ||
	Language == Lang::Kotlin && (Next.front() == '(' || Next.front() == '[' || Next.starts_with("::"));
}

/**
 * PHP の代替構文の本体を開く `:` の判定関数
 * @param Parent `:` の親ノード
 * @param Colon 判定対象の `:` の字句
 * @return 本体を開く `:`（`colon_block` / `switch_block` の先頭，`for` / `declare` の直下）なら true
 */
bool StructurePass::IsPhpAltOpener(const TSNode Parent, const TSNode Colon) {
	// 節点の型名
	const std::string_view Type = ts_node_type(Parent);
	// 代替構文の本体を持つ親の `:` かの返戻
	return NodeKind::PhpAltHost.Contains(Type) &&
	(!NodeKind::PhpAltLeadingColon.Contains(Type) || ts_node_start_byte(Colon) == ts_node_start_byte(Parent));
}

/**
 * PHP の地の文との境界の区切り決定関数
 * 地の文 (`text` / `text_interpolation`) は出力其の物で，開始タグ (`<?php` / `<?=`) と閉じタグ (`?>`) の間だけが PHP の空白
 * @param Src ソースコード
 * @param Prev 直前の子
 * @param Cur 現在の子
 * @param Built 組立済の文字列（閉じタグの前の区切りを直前の開始タグ以降の行数で決める）
 * @return 区切り（地の文の境界でなければ nullptr）
 */
const char *StructurePass::PhpInlineHtmlGap(
	const TSSource &Src,
	const TSNode Prev,
	const TSNode Cur,
	const std::string_view Built
) {
	// 直前節点の型名
	const std::string_view PrevType = ts_node_type(Prev), CurType = ts_node_type(Cur);
	// 先頭の地の文の直後の開始タグは地の文へ直に続けて，出力へ空白を足さない事の返戻
	if(PrevType == "text") return "";
	// 開始タグの後は，次の閉じタグ迄が１文なら同じ行へ続け (`<?php endif; ?>`)
	if(
		const bool IsAfterOpen = NodeKind::PhpOpenTagEnd.Contains(PrevType) &&
		(PrevType != "php_tag" || !ts_node_is_null(ts_node_prev_named_sibling(Prev)) || Src.View(Prev) == "<?=");
		IsAfterOpen
	) {
		// 文の無い `<?php ?>` を同じ行に置く事の返戻
		if(CurType == "text_interpolation") return " ";
		// 後続の節点
		const TSNode Next = ts_node_next_named_sibling(Cur);
		// 次の閉じタグ迄の文の数に依る区切りの返戻
		return ts_node_is_null(Next) || std::string_view(ts_node_type(Next)) == "text_interpolation" ? " " : "\n";
	}
	// 地の文の境界でない場合の返戻
	if(CurType != "text_interpolation") return nullptr;
	const size_t LineEnd = Built.rfind('\n');
	// 改行の無い組立済の返戻
	if(LineEnd == std::string_view::npos) return " ";
	// 最後の行の開始タグの有無に依る区切りの返戻
	return Built.find("<?", LineEnd + 1) == std::string_view::npos ? "\n" : " ";
}

/**
 * PHP の代替構文の本体で終わるかの判定関数
 * @param Node 判定対象のノード
 * @return 代替構文の本体 (`colon_block`) 其の物か，其れで終わる節 (`elseif(...): ...` / `else: ...`) なら true
 */
bool StructurePass::EndsWithPhpColonBlock(const TSNode Node) {
	// 名前付の節でない場合の返戻
	if(ts_node_is_null(Node) || !ts_node_is_named(Node)) return false;
	// 代替構文の本体其の物の場合の返戻
	if(std::string_view(ts_node_type(Node)) == "colon_block") return true;
	// 子節点の個数
	const uint32_t ChildCount = ts_node_named_child_count(Node);
	// 名前付の子を持たない節の場合の返戻
	if(!ChildCount) return false;
	// 末尾の節点
	const TSNode Last = ts_node_named_child(Node, ChildCount - 1);
	// 代替構文の本体かの返戻
	return !ts_node_is_null(Last) && std::string_view(ts_node_type(Last)) == "colon_block";
}

/** ========== 構造化組立 ========== */
/**
 * 構造化テキストの本体の構築関数
 * @param Src ソースコード
 * @param Node 対象ノード
 * @param Language 対象言語
 * @param FlatByNode フラットテキストキャッシュ
 * @param StructuredByNode 構造化テキストキャッシュ
 * @param ContainsByNode 展開要因有無キャッシュ
 * @return 構造化テキスト（展開要因が在れば子毎に改行，無ければ平坦形式）
 */
std::string StructurePass::BuildStructuredCore(
	const TSSource &Src,
	const TSNode Node,
	const Lang Language,
	NodeTextMap &FlatByNode,
	NodeTextMap &StructuredByNode,
	ContainsStmtBlockMap &ContainsByNode
) {
	// 再帰深度の監視値
	const RecursionGuard Guard;
	if(Guard.IsOverflow) {
		// 深い構造の整形結果
		std::string Deep;
		Parallel::RunOnFreshStack(
			[&]() -> void {
				// 深い構造の整形結果の更新
				Deep = BuildStructuredText(Src, Node, Language, FlatByNode, StructuredByNode, ContainsByNode);
			}
		);
		// 深いネストの続きを新しい走脈で組み立てた結果の返戻
		return Deep;
	}
	// 子節点の個数
	const uint32_t ChildCount = ts_node_child_count(Node), NamedCount = ts_node_named_child_count(Node);
	const std::string_view NodeTypeView = ts_node_type(Node);
	// 構造を信頼出来ない `ERROR` 節点の原文返戻
	if(
		NodeTypeView == "ERROR" || Language.IsCFamily() && (Src.StringizesArguments(Node) || HoldsBrokenDirective(Src, Node)) ||
		HoldsLopsidedOperator(Src, Node, Language)
		// 原文の返戻
	) return Src.Text(Node);
	// CSS の `url(...)` の中身の参照先の原文の返戻
	if(Language == Lang::CSS && Src.IsCssUrlArguments(Node)) return CssUrlArgumentsText(Src, Node);
	// 構文木に値が現れない HTML 文書型宣言の原文保持
	if(NodeTypeView == "doctype" || NodeKind::JsxContainer.Contains(NodeTypeView) && TSSource::HasDisputedJsxText(Src, Node)) {
		// 原文の返戻
		return Src.Text(Node);
	}
	// Ruby の連結文字列の独立文誤解析を避ける平坦化結果の返戻
	if(Language == Lang::Ruby && NodeTypeView == "chained_string") return GetFlatText(Src, Node, Language, FlatByNode);
	// Ruby のヒアドキュメント本体は終端タグが独自行頭・行末必須の為，逐語保持に加え末尾改行を強制
	if(Language == Lang::Ruby && NodeTypeView == "heredoc_body") {
		std::string Result(HeredocBodyText(Src, Node));
		if(Result.empty() || Result.back() != '\n') Result += '\n';
		// 末尾改行付の逐語テキストの返戻
		return Result;
	}
	// 展開要因を含む部分木の平坦化からの除外
	const bool HasStmtBlock = ContainsStmtBlock(Src, Node, Language, ContainsByNode);
	const bool ShouldExpand = HasStmtBlock && (
		Language != Lang::Ruby || !NodeKind::ExpandContainer.Contains(NodeTypeView) || HasChildOf(
			Node,
			[&](const TSNode Child) -> bool {
				// 複数の行に為る要素かの返戻
				return ts_node_is_named(Child) &&
				GetStructuredText(Src, Child, Language, FlatByNode, StructuredByNode, ContainsByNode).find('\n') != std::string::npos;
			}
		)
	);
	// 括弧コンテナか
	const bool IsParen = NodeTypeView == "parenthesized_expression";
	// HTML の展開不要（子の間に改行を置ける境界を持たない）要素は平坦化して行長判定を後続処理に委譲
	if(
		!HasStmtBlock && (
			NodeKind::JsxTag.Contains(NodeTypeView) || IsParen || NodeKind::JsxContainer.Contains(NodeTypeView) ||
			NodeKind::JsxElement.Contains(NodeTypeView) && JsxContentCount(Src, Node) < 2 ||
			Language == Lang::HTML && NodeTypeView == "element"
		)
		// 展開を要しないノードの平坦テキストの返戻
	) return GetFlatText(Src, Node, Language, FlatByNode);
	// Go の括弧式は ASI に依る構文崩壊を防ぐ為，汎用経路で密着を保持
	if(
		ShouldExpand && (
			NodeKind::ExpandContainer.Contains(NodeTypeView) || Language == Lang::Go && NodeKind::AngleBracketList.Contains(NodeTypeView)
		) && !(Language == Lang::Go && IsParen)
		// 展開結果の返戻
	) return BuildExpandedContainer(Src, Node, Language, FlatByNode, StructuredByNode, ContainsByNode);
	if(NodeTypeView == "body_statement") {
		// 結果の格納先
		std::string Out;
		// 結果の格納先の格納領域予約
		Out.reserve(Src.Len(Node));
		// 本文を控えたヒアドキュメント開始の数（本文の前の文は同じ行へ `;` で継続）
		uint32_t OpenHeredocs = 0;
		ForEachNamedChild(
			Node,
			[&](const TSNode Child) -> void {
				// Ruby のヒアドキュメント本体は終端タグが独自行頭・行末必須の為，逐語保持
				if(std::string_view(ts_node_type(Child)) == "heredoc_body") {
					// ヒアドキュメント本体の出力への追加
					AppendHeredocBody(Out, Src, Child);
					// 未閉鎖ヒアドキュメント数の更新
					TrackOpenHeredocs(OpenHeredocs, Src, Child);
					// ヒアドキュメント本体の場合の終了
					return;
				}
				// 子節点の整形結果
				const std::string SubText = TakeStructuredText(Src, Child, Language, FlatByNode, StructuredByNode, ContainsByNode);
				const std::string_view SubView = TrimmedView(SubText);
				// 条件成立時の返戻
				if(SubView.empty()) return;
				if(OpenHeredocs) AppendHeredocPendingSeparator(Out);
				else if(!Out.empty() && Out.back() != '\n') Out += '\n';
				// 結果の格納先への字面追加
				Out.append(SubView.data(), SubView.size());
				// 未閉鎖ヒアドキュメント数の更新
				TrackOpenHeredocs(OpenHeredocs, Src, Child);
			}
		);
		// 子毎に改行を挿入した構造化テキストの返戻
		return Out;
	}
	// 丸括弧を含む import 宣言の１行１指定での組立
	if(NodeTypeView == "import_declaration" && Src.View(Node).find('(') != std::string_view::npos) {
		std::string Out;
		// 結果の格納先の格納領域予約
		Out.reserve(Src.Len(Node) + 16);
		// 結果の格納先への追加
		Out += "import (\n";
		const auto AppendSpec = [&](const TSNode Spec) -> void {
			// 調査対象の部分文字列
			const std::string_view Sub = TrimmedView(GetFlatText(Src, Spec, Language, FlatByNode));
			// 条件成立時の返戻
			if(Sub.empty()) return;
			// 結果の格納先への字面追加
			Out.append(Sub.data(), Sub.size());
			// 結果の格納先への追加
			Out.push_back('\n');
		};
		ForEachNamedChild(
			Node,
			[&](const TSNode Child) -> void {
				// 現在節点の型名
				const std::string_view CurType = ts_node_type(Child);
				if(CurType == "import_spec") AppendSpec(Child);
				else if(CurType == "import_spec_list") {
					ForEachNamedChild(
						Child,
						[&](const TSNode Spec) -> void {
							if(std::string_view(ts_node_type(Spec)) == "import_spec") AppendSpec(Spec);
						}
					);
				}
			}
		);
		// 各指定の後の改行に続けて閉鎖
		Out += ')';
		// import 一覧を整形した結果の返戻
		return Out;
	}
	if(NodeKind::Preproc.Contains(NodeTypeView)) {
		// プリプロセッサブロックは専用関数で展開，其れ以外（空のブロックを含む）は単純テキストの返戻
		return NodeKind::PreprocBlock.Contains(NodeTypeView) && Src.Len(Node) ?
		BuildPreprocBlockText(Src, Node, Language, FlatByNode, StructuredByNode, ContainsByNode) :
		BuildPreprocText(Src.View(Node));
	}
	// then 節点か
	const bool IsThen = NodeTypeView == "then", IsDo = NodeTypeView == "do", IsElse = NodeTypeView == "else";
	const bool IsEnsure = NodeTypeView == "ensure", IsRescue = NodeTypeView == "rescue";
	const bool IsRubySection = Language == Lang::Ruby &&
	(IsThen || IsDo || IsElse || IsEnsure || IsRescue || NodeKind::RubyCondTerminator.Contains(NodeTypeView));
	if(IsRubySection) {
		// 条件又は rescue 節点か
		const bool IsCondOrRescue = NodeKind::RubyCondOrRescue.Contains(NodeTypeView);
		bool IsMulti = IsEnsure || !IsCondOrRescue && NamedCount > 1;
		// 本体が複数文か，単一文でも多行（`b.each do ... end` 等）なら，本体を見出の次行へ配置
		ForEachNamedChild(
			Node,
			[&](const TSNode Child) -> bool {
				if(
					// 現在節点の型名
					const std::string_view CurType = ts_node_type(Child);
					IsCondOrRescue ?
					!NodeKind::RubyThenElse.Contains(CurType) :
					!IsThen && !IsDo && !NodeKind::RubyElseEnsure.Contains(NodeTypeView)
					// 本体以外の子の走査継続の返戻
				) return true;
				IsMulti = IsMulti || IsCondOrRescue && ts_node_named_child_count(Child) > 1 ||
				GetStructuredText(Src, Child, Language, FlatByNode, StructuredByNode, ContainsByNode).find('\n') != std::string::npos;
				// 多行が確定する迄の走査継続の返戻
				return !IsMulti;
			}
		);
		bool IsHeaderOpenRange = false;
		if(IsMulti && NodeKind::RubyCondTerminator.Contains(NodeTypeView)) {
			ForEachNamedChild(
				Node,
				[&IsHeaderOpenRange](const TSNode Child) -> bool {
					// 本体の手前迄の見出の子の走査継続の返戻
					if(std::string_view(ts_node_type(Child)) == "then") return false;
					// 見出末尾の開区間該当有無の更新
					IsHeaderOpenRange = EndsWithOpenRange(Child);
					// 次の子の走査継続の返戻
					return true;
				}
			);
		}
		// then の行内配置要否
		const bool NeedsThenInline = (!IsMulti || IsHeaderOpenRange) && NodeKind::RubyCondTerminator.Contains(NodeTypeView);
		bool IsThenInserted = false;
		uint32_t OpenHeredocs = 0;
		std::string Base;
		// 組立中の文字列の格納領域予約
		Base.reserve(Src.Len(Node) + 16);
		// 構文木の走査位置
		HeldCursor Cursor(Node);
		if(ts_tree_cursor_goto_first_child(&Cursor)) {
			do {
				if(const TSNode Child = ts_tree_cursor_current_node(&Cursor); ts_node_is_named(Child)) {
					// 現在節点の型名
					const std::string_view CurType = ts_node_type(Child);
					// 後段の字下げ計算を保つヒアドキュメント本体の逐語保持
					if(CurType == "heredoc_body") {
						// ヒアドキュメント本体の出力への追加
						AppendHeredocBody(Base, Src, Child);
						// 未閉鎖ヒアドキュメント数の更新
						TrackOpenHeredocs(OpenHeredocs, Src, Child);
						continue;
					}
					// 本体扱いの子は構造化テキスト経由で取得
					const bool IsBody =
					NodeKind::RubyBodyTrigger.Contains(CurType) || IsThen || IsDo || NodeKind::RubyElseEnsure.Contains(NodeTypeView);
					if(NeedsThenInline && CurType == "then" && !Base.empty() && Base.back() != '\n') {
						// 組立中の文字列への追加
						Base += " then";
						// then の挿入済状態の設定
						IsThenInserted = true;
					}
					const std::string_view SubView = TrimmedView(
						IsBody ?
						GetStructuredText(Src, Child, Language, FlatByNode, StructuredByNode, ContainsByNode) :
						GetFlatText(Src, Child, Language, FlatByNode)
					);
					if(SubView.empty()) continue;
					// rescue 節の本体は単一文でも改行必須（`rescue X => e body` 等は構文違反）
					if(OpenHeredocs) AppendHeredocPendingSeparator(Base);
					else if(IsBody && (IsMulti || IsRescue) && !Base.empty()) Base += '\n';
					else if(!Base.empty()) Base += ' ';
					// 組立中の文字列への字面追加
					Base.append(SubView.data(), SubView.size());
					// 未閉鎖ヒアドキュメント数の更新
					TrackOpenHeredocs(OpenHeredocs, Src, Child);
				} else {
					// 対象の字句
					const std::string_view Token = Src.View(Child);
					if(Token.empty() || (IsThen || IsDo) && Token == NodeTypeView) continue;
					if(Token == "end") AppendEndToken(Base, IsMulti, Token);
					else {
						// カンマとセミコロンの左密着字句としての取扱
						if(Token != "," && Token != ";" && !Base.empty() && Base.back() != ' ' && Base.back() != '\n') Base += ' ';
						// 組立中の文字列への字面追加
						Base.append(Token.data(), Token.size());
					}
				}
			} while(ts_tree_cursor_goto_next_sibling(&Cursor));
		}
		// 空本体の `when` / `elsif` には末尾に ` then` を補填
		if(NeedsThenInline && !IsThenInserted && !Base.empty() && Base.back() != '\n') Base += " then";
		// Ruby の節先頭を整形した結果の返戻
		return Base;
	}
	// 同名節点の Ruby 構文だけへの限定
	if(Language == Lang::Ruby && NodeKind::RubyBlock.Contains(NodeTypeView)) {
		// 組立中の基底文字列
		std::string Base;
		// 組立中の文字列の格納領域予約
		Base.reserve(Src.Len(Node) + 16);
		const bool IsForceInMulti = NodeTypeView == "case_match";
		const bool IsForcedMulti = NodeKind::RubyBodyContainer.Contains(NodeTypeView) || IsForceInMulti;
		bool IsLastSectionMulti = NodeKind::RubyClassLike.Contains(NodeTypeView), HasBody = false, HasHeredocBeforeThen = false;
		uint32_t OpenHeredocs = 0;
		std::vector<TSNode> Children, Deferred, Owners, DeferredOwners;
		// 整形対象の子節点列の格納領域予約
		Children.reserve(ChildCount);
		// 子節点別の親節点列の格納領域予約
		Owners.reserve(ChildCount);
		// 後続節点へ委ねた区切り開始位置列
		std::vector<size_t> DeferredStarts;
		for(TSNode Parent = Node; !ts_node_is_null(Parent);) {
			// ネスト節の整形結果
			TSNode Nested {};
			// 後続節点へ委ねた区切り開始位置列への追加
			DeferredStarts.push_back(Deferred.size());
			ForEachChild(
				Parent,
				[&](const TSNode Child) -> void {
					if(!ts_node_is_null(Nested)) {
						// 後続へ移す子節点列への追加
						Deferred.push_back(Child);
						// 後続節点へ委ねた区切り所有者列への追加
						DeferredOwners.push_back(Parent);
					} else if(ts_node_is_named(Child) && std::string_view(ts_node_type(Child)) == "elsif") Nested = Child;
					else {
						// 整形対象の子節点列への追加
						Children.push_back(Child);
						// 子節点別の親節点列への追加
						Owners.push_back(Parent);
					}
				}
			);
			// 対象の親節点の確定
			Parent = Nested;
		}
		for(size_t Level = DeferredStarts.size(); Level--;) {
			// 対象範囲の終端位置
			const size_t To = Level + 1 < DeferredStarts.size() ? DeferredStarts[Level + 1] : Deferred.size();
			const std::ptrdiff_t From = static_cast<std::ptrdiff_t>(DeferredStarts[Level]), Until = static_cast<std::ptrdiff_t>(To);
			// 整形対象の子節点列への範囲追加
			Children.insert(Children.end(), Deferred.begin() + From, Deferred.begin() + Until);
			// 子節点別の親節点列への範囲追加
			Owners.insert(Owners.end(), DeferredOwners.begin() + From, DeferredOwners.begin() + Until);
		}
		// 分岐の何れかが多行又は修飾子形なら，全ての節を多行に置いて節境界を保持
		bool IsAnySectionMulti = HasModifierBody(Node);
		if(NodeKind::RubyBranch.Contains(NodeTypeView)) for(const TSNode Child : Children) {
			if(IsAnySectionMulti) break;
			if(!ts_node_is_named(Child)) continue;
			if(
				// 現在節点の型名
				const std::string_view CurType = ts_node_type(Child);
				NodeKind::ThenDo.Contains(CurType) || NodeKind::SectionHeader.Contains(CurType)
			) {
				IsAnySectionMulti =
				GetStructuredText(Src, Child, Language, FlatByNode, StructuredByNode, ContainsByNode).find('\n') != std::string::npos;
			}
		}
		for(size_t ChildIdx = 0; ChildIdx < Children.size(); ++ChildIdx) {
			if(const TSNode Child = Children[ChildIdx]; ts_node_is_named(Child)) {
				const std::string_view CurType = ts_node_type(Child);
				// ヒアドキュメント本体は開始行の次行から始まり終端タグが独自行を要する為，逐語保持＋前後改行強制
				if(CurType == "heredoc_body") {
					if(AppendHeredocBody(Base, Src, Child)) IsLastSectionMulti = IsAnySectionMulti = HasHeredocBeforeThen = true;
					// 未閉鎖ヒアドキュメント数の更新
					TrackOpenHeredocs(OpenHeredocs, Src, Child);
					continue;
				}
				// 子節点の整形結果
				const std::string SubText = TakeStructuredText(Src, Child, Language, FlatByNode, StructuredByNode, ContainsByNode);
				const std::string_view SubView = TrimmedView(SubText);
				if(SubView.empty()) continue;
				// then 又は do の節点か
				const bool IsThenDo = NodeKind::ThenDo.Contains(CurType), IsSection = IsThenDo || NodeKind::SectionHeader.Contains(CurType);
				const bool IsMulti = SubView.find('\n') != std::string_view::npos;
				if(!IsSection && NodeKind::RubyBodyContainer.Contains(NodeTypeView)) {
					// 本体節点の保持状態の設定
					HasBody = true;
					// `begin` の単一文形も本体として取扱
					if((NodeKind::AmbiguousBodyContainer.Contains(CurType) || NodeTypeView == "begin") && (IsMulti || NodeTypeView != "block")) {
						if(!Base.empty() && Base.back() != '\n') Base += '\n';
						// 末尾節の複数行該当状態の設定
						IsLastSectionMulti = true;
					} else if(!Base.empty() && Base.back() != ' ' && Base.back() != '\n') Base += ' ';
					// 組立中の文字列への字面追加
					Base.append(SubView.data(), SubView.size());
					// 未閉鎖ヒアドキュメント数の更新
					TrackOpenHeredocs(OpenHeredocs, Src, Child);
					continue;
				}
				if(IsSection) {
					// 本体節点の保持状態の設定
					HasBody = true;
					// 直前の節が複数行か
					const bool IsPrevMulti = IsLastSectionMulti;
					// rescue・ensure 節の本体が空の場合を含む複数行化
					IsLastSectionMulti = IsMulti || IsAnySectionMulti || NodeKind::RubyBodyTailClause.Contains(CurType);
					if(IsThenDo && (IsMulti || IsAnySectionMulti || HasHeredocBeforeThen)) {
						if(!Base.empty() && Base.back() != '\n') Base += '\n';
					} else if(IsThenDo) {
						if(!Base.empty() && Base.back() != ' ') Base += ' ';
						// 組立中の文字列への追加
						Base += CurType == "do" ? "do " : "then ";
					} else if((IsPrevMulti || IsForcedMulti || IsAnySectionMulti) && !OpenHeredocs) {
						if(!Base.empty() && Base.back() != '\n') Base += '\n';
					} else if(!Base.empty() && Base.back() != ' ') Base += ' ';
					// 組立中の文字列への字面追加
					Base.append(SubView.data(), SubView.size());
					if(IsThenDo) HasHeredocBeforeThen = false;
					// 未閉鎖ヒアドキュメント数の更新
					TrackOpenHeredocs(OpenHeredocs, Src, Child);
					continue;
				}
				// Ruby のラムダ `->(...)` は `->` と直後の仮引数列の `(` を密着
				if(
					!Base.empty() && Base.back() != ' ' && Base.back() != '\n' &&
					!(NodeTypeView == "lambda" && Base.back() == '>' && !SubView.empty() && SubView.front() == '(')
				) Base += ' ';
				Base.append(SubView.data(), SubView.size());
				TrackOpenHeredocs(OpenHeredocs, Src, Child);
				// 空の分岐本体への then 補完による else・elsif 境界の維持
				const TSNode Parent = Owners[ChildIdx];
				if(
					// 親節点の型名
					const std::string_view ParentType = ts_node_type(Parent);
					(ParentType == "if" || ParentType == "unless" || ParentType == "elsif") &&
					ts_node_eq(Child, TSSource::FieldChild(Parent, "condition")) &&
					(ParentType == "elsif" || !ts_node_is_null(TSSource::FieldChild(Parent, "alternative")))
				) {
					if(const TSNode Body = TSSource::FieldChild(Parent, "consequence"); ts_node_is_null(Body) || !ts_node_named_child_count(Body)) {
						Base += " then";
						HasBody = true;
						IsLastSectionMulti = false;
					}
				}
			} else {
				// 対象の字句
				const std::string_view Token = Src.View(Child);
				if(Token.empty()) continue;
				// 最後の節 (then / else / elsif / when / rescue / ensure) が単一文なら `end` を１行化
				if(Token == "end" || Token == "}") {
					AppendEndToken(
						Base,
						!OpenHeredocs && (IsLastSectionMulti || IsForceInMulti || !HasBody && NodeKind::RubyBranch.Contains(NodeTypeView)),
						Token
					);
				} else {
					// 平坦展開された elsif の複数行前節に続く改行配置
					if(Token == "elsif") {
						HasHeredocBeforeThen = false;
						if(IsLastSectionMulti && !Base.empty() && Base.back() != '\n') Base += '\n';
						else if(!Base.empty() && Base.back() != ' ' && Base.back() != '\n') Base += ' ';
					} else if(!Base.empty() && Base.back() != ' ' && Base.back() != '\n') Base += ' ';
					Base.append(Token.data(), Token.size());
				}
			}
		}
		// Ruby ブロック（begin/class/module 等）を整形した結果の返戻
		return Base;
	}
	if(NodeKind::NamespaceLike.Contains(NodeTypeView)) {
		std::string Base;
		Base.reserve(Src.Len(Node) + 16);
		// 構文木の走査位置
		HeldCursor Cursor(Node);
		if(ts_tree_cursor_goto_first_child(&Cursor)) {
			do {
				if(const TSNode Child = ts_tree_cursor_current_node(&Cursor); ts_node_is_named(Child)) {
					if(!Base.empty() && Base.back() != ' ' && Base.back() != '\n') Base += ' ';
					Base += TakeStructuredText(Src, Child, Language, FlatByNode, StructuredByNode, ContainsByNode);
				} else if(const std::string_view Token = Src.View(Child); !Token.empty()) {
					// 文末区切り（PHP の `namespace A\B;` 等）は直前の名前へ密着
					if(Token != ";" && Token != "," && !Base.empty() && Base.back() != ' ' && Base.back() != '\n') Base += ' ';
					Base.append(Token.data(), Token.size());
				}
			} while(ts_tree_cursor_goto_next_sibling(&Cursor));
		}
		// 名前空間定義又は内部モジュールの整形結果の返戻
		return Base;
	}
	// 親節点の有無に依るファイル根の判定
	if(NodeKind::Transparent.Contains(NodeTypeView) || IsTreeRoot(Node)) {
		// 組立中の基底文字列
		std::string Base;
		Base.reserve(Src.Len(Node) + 16);
		// ラベル直後の節点か
		bool IsAfterLabel = false;
		TSNode Prev {};
		ForEachChild(
			Node,
			[&](const TSNode Child) -> void {
				// 文の区切り `;` 以外の無名の子も文其の物
				const bool IsNamed = ts_node_is_named(Child);
				// 条件成立時の返戻
				if(!IsNamed && std::string_view(ts_node_type(Child)) == ";") return;
				const std::string Sub =
				IsNamed ? TakeStructuredText(Src, Child, Language, FlatByNode, StructuredByNode, ContainsByNode) : std::string(Src.View(Child));
				if(!Sub.empty()) {
					// 節点の型名
					const std::string_view Type(ts_node_type(Child));
					// `ERROR` 隣接は原文の間隙を保ち，前処理指令境界は改行，Kotlin・Swift の波括弧始まり等はセミコロンで分離
					if(!Base.empty()) {
						if(
							// 直前節点の型名
							const std::string_view PrevType(ts_node_type(Prev));
							(Type == "ERROR" || PrevType == "ERROR") && !(
								Language.IsCFamily() && (
									NodeKind::Preproc.Contains(PrevType) || NodeKind::Preproc.Contains(Type) || IsPreprocDirectiveAt(Sub, 0) ||
									IsPreprocDirectiveAt(Src.View(Prev), 0)
								)
							)
						) AppendGapVerbatim(Base, Src, Prev, Child);
						else Base += IsAfterLabel ? " " : JoinsAcrossNewline(Language, Sub) ? ";\n" : "\n";
					}
					Base += Sub;
					IsAfterLabel = Language == Lang::Kotlin && (Type == "label" || IsAfterLabel && Type == "annotation");
					Prev = Child;
				}
			}
		);
		// 透過親要素の文を分離し，Kotlin のラベルを後続へ接続した結果の返戻
		return Base;
	}
	if(ChildCount < 2) {
		if(ChildCount == 1 && NamedCount == 1) {
			if(const TSNode Only = ts_node_named_child(Node, 0); HasSameByteRange(Src, Only, Node)) {
				// 同じ範囲を占める子の構造化結果の返戻
				return TakeStructuredText(Src, Only, Language, FlatByNode, StructuredByNode, ContainsByNode);
			}
		}
		// 構造展開不要なノードは平坦テキストの返戻
		return GetFlatText(Src, Node, Language, FlatByNode);
	}
	// C / C++ のテンプレート型は整形可能だが TS の同名ノードは整形不能
	if(
		NodeKind::Unformattable.Contains(NodeTypeView) && !(NodeTypeView == "template_type" && Language.IsCFamily()) &&
		!NodeKind::TemplateStringLike.Contains(NodeTypeView) && !IsKotlinImplicitCtor(Src, Node, NodeTypeView, Language)
		// 整形対象外の原文の返戻
	) return Src.Text(Node);
	// コメントだけを持つ空ブロックの平坦化と空波括弧出力への確定
	if(
		!NamedCount || IsVerbatimLeaf(NodeTypeView, Language) || AreAllNamedChildrenCommentsCached(Node, ContainsByNode) ||
		Language == Lang::Ruby && NodeTypeView == "block" && !HasChildOf(
			Node,
			[](const TSNode Child) -> bool {
				// 複数の文を持つ本体かの返戻
				return ts_node_is_named(Child) && NodeKind::AmbiguousBodyContainer.Contains(Child) && ts_node_named_child_count(Child) > 1;
			}
		)
	) {
		// 葉・空本体・単一の本体の平坦テキストの返戻
		return GetFlatText(Src, Node, Language, FlatByNode);
	}
	const bool IsJsxTag = NodeKind::JsxTag.Contains(NodeTypeView);
	const bool IsJsxCont = NodeKind::JsxElement.Contains(NodeTypeView) && !IsJsxTag;
	const bool IsSwiftSwitchAsContainer = Language == Lang::Swift && NodeTypeView == "switch_statement";
	const bool IsObject = NodeKind::ObjectLike.Contains(NodeTypeView);
	bool IsObjectOpen = false, IsObjectClose = false;
	if(IsObject && ShouldExpand) {
		ForEachChild(
			Node,
			[&](const TSNode Child) -> void {
				// 条件成立時の返戻
				if(ts_node_is_named(Child)) return;
				// 対象の字句
				const std::string_view Token = Src.View(Child);
				IsObjectOpen = IsObjectOpen || Token == "{";
				IsObjectClose = IsObjectClose || Token == "}";
			}
		);
	}
	// class 節点の Ruby 定義としての取扱の Ruby への限定
	const bool IsForeignClass = Language != Lang::Ruby && NodeKind::RubyClassDefLike.Contains(NodeTypeView);
	const bool IsBraceBodyHost = Language == Lang::Kotlin && NodeKind::KotlinBraceBodyHost.Contains(NodeTypeView) ||
	Language == Lang::Swift && NodeKind::SwiftBraceBodyHost.Contains(NodeTypeView);
	const bool IsIndentContainer =
	(NodeKind::IndentContainer.Contains(NodeTypeView) && !IsForeignClass || IsSwiftSwitchAsContainer || IsBraceBodyHost) &&
	!IsObject || IsObjectOpen && IsObjectClose;
	// 文リスト・節見出の集合判定は構造展開判定と子連結ループの両方で共有
	const bool IsStatementList = NodeKind::StatementList.Contains(NodeTypeView) && (
		!NodeKind::GoGroupedDecl.Contains(NodeTypeView) || HasChildOf(
			Node,
			[&](const TSNode Child) -> bool {
				// 一括宣言を囲む開き丸括弧の子が居るかの返戻
				return !ts_node_is_named(Child) && Src.View(Child) == "(";
			}
		)
	);
	// 節見出を保持する親節点か
	const bool IsSectionHeaderParent = NodeKind::SectionHeader.Contains(NodeTypeView);
	const bool IsLambdaBody = NodeTypeView == "lambda_literal";
	const bool IsStructural = IsIndentContainer || IsStatementList || (IsLambdaBody || IsJsxTag) && ShouldExpand ||
	NamedCount && (IsJsxCont || IsSectionHeaderParent);
	// 構造展開要否は構文木由来の要因のみで決定
	const bool IsClassLike = NodeKind::RubyClassDefLike.Contains(NodeTypeView) && !IsForeignClass;
	const bool IsCompact = NodeKind::Compact.Contains(NodeTypeView);
	const bool HasPhpAltBody = Language == Lang::PHP && NodeKind::PhpAltHost.Contains(NodeTypeView) && HasChildOf(
		Node,
		[&](const TSNode Child) -> bool {
			// 本体を開く `:` の有無の返戻
			return !ts_node_is_named(Child) && Src.View(Child) == ":" && IsPhpAltOpener(Node, Child);
		}
	);
	if(!IsStructural && !HasStmtBlock && !HasPhpAltBody) {
		const bool HasElseLike = HasChildOrGrandchildOf(
			Node,
			[](const TSNode Cur) -> bool {
				return NodeKind::ElseLikeClause.Contains(Cur);
			}
		);
		// 多行 JSX 子の判定は子の構造化テキスト構築を伴う為，else 系不在の確定後に短絡評価
		if(
			!HasElseLike && !HasChildOf(
				Node,
				[&](const TSNode Child) -> bool {
					// 名前付でない又は JSX 親要素でない場合は対象外の返戻
					if(!ts_node_is_named(Child) || !NodeKind::JsxContainer.Contains(Child)) return false;
					// JSX 親要素の構造化テキストに改行が在る場合多行 JSX 子有の返戻
					return GetStructuredText(Src, Child, Language, FlatByNode, StructuredByNode, ContainsByNode).find('\n') != std::string::npos;
				}
			)
			// 平坦に組む節点の文字列の返戻
		) return GetFlatText(Src, Node, Language, FlatByNode);
	}
	// ループ不変なノード型判定を先に確定し，子毎の集合検索・型名比較の再評価を排除
	const bool IsSwitchEntry = NodeTypeView == "switch_entry";
	const bool IsStatementsOnlyBreak = NodeKind::StatementsOnlyBreak.Contains(NodeTypeView) || IsBraceBodyHost;
	const bool IsRubyMethodDefNode = NodeKind::RubyMethodDef.Contains(NodeTypeView);
	const bool IsUnaryPreOrUpdateNode = NodeKind::UnaryPreOrUpdate.Contains(NodeTypeView);
	const bool IsDoStatementNode = NodeKind::DoLoop.Contains(NodeTypeView);
	const bool IsIfParentNode = NodeKind::IfNode.Contains(NodeTypeView), IsJsonArray = NodeTypeView == "array";
	const bool IsGoGroupedDeclNode = NodeKind::GoGroupedDecl.Contains(NodeTypeView);
	const bool IsGoSwitch = Language == Lang::Go && NodeKind::GoSwitch.Contains(NodeTypeView);
	const bool IsCsLambda = Language == Lang::CSharp && NodeTypeView == "lambda_expression";
	const bool IsRubyStatements = Language == Lang::Ruby && NodeKind::RubyStatementSequence.Contains(NodeTypeView);
	std::vector<HtmlBoundary> HtmlGaps;
	if(Language == Lang::HTML && NodeTypeView == "element") CollectHtmlBoundaries(Src, Node, HtmlGaps);
	// 子ベクタへの収集を廃し，カーソル直接走査＋直前子変数 (`PrevChild`) 保持で子ノード列の複製確保を排除
	HeldCursor Cursor(Node);
	ts_tree_cursor_goto_first_child(&Cursor);
	// 直前の子節点
	TSNode PrevChild = {}, Child = {};
	std::string Base;
	Base.reserve(Src.Len(Node) + 16);
	// 名前付子節点の検出有無
	uint32_t NamedSeen = 0, OpenHeredocs = 0;
	bool IsAfterInherit = false, IsPhpAltBody = false;
	size_t PhpAltOpenEnd = std::string::npos;
	std::string_view ChildType, PrevType;
	bool IsAfterControlError = false;
	const bool HasError = ts_node_has_error(Node);
	for(
		// 子節点の索引
		uint32_t ChildIdx = 0;
		ChildIdx < ChildCount;
		++ChildIdx, PrevChild = Child, PrevType = ChildType, ts_tree_cursor_goto_next_sibling(&Cursor)
	) {
		Child = ts_tree_cursor_current_node(&Cursor);
		ChildType = std::string_view(ts_node_type(Child));
		IsAfterControlError = IsAfterControlError || ChildType == "ERROR" && NodeKind::Control.Contains(NodeTypeView);
		// JS / TS のデコレータ (`@Foo`) 直後は必ず改行する（`@Foo\nclass X` の慣習）
		if(!Base.empty() && Base.back() != '\n' && PrevType == "decorator") Base += '\n';
		if(ts_node_is_named(Child)) {
			// Ruby のヒアドキュメント本体は終端タグが独自行頭・行末必須の為，逐語保持＋前後改行強制で次トークンとの密着を防止
			if(Language == Lang::Ruby && ChildType == "heredoc_body") {
				AppendHeredocBody(Base, Src, Child);
				TrackOpenHeredocs(OpenHeredocs, Src, Child);
				++NamedSeen;
				continue;
			}
			// 子節点の整形結果
			const std::string SubText = TakeStructuredText(Src, Child, Language, FlatByNode, StructuredByNode, ContainsByNode);
			const std::string_view Sub = IsJsxCont && ChildType == "jsx_text" ? std::string_view(SubText) : TrimmedView(SubText);
			if(Sub.empty()) {
				++NamedSeen;
				continue;
			}
			// JSX 連続本文へ改行を足さず，Rust の式終端と Java 列挙宣言部の先頭セミコロンは前の文へ密着
			if(
				IsJsxCont && NodeKind::JsxTextLike.Contains(ChildType) && NodeKind::JsxTextLike.Contains(PrevType) ||
				NamedSeen && (Language == Lang::Rust && ChildType == "empty_statement" || ChildType == "enum_body_declarations")
			) {
				Base.append(Sub.data(), Sub.size());
				++NamedSeen;
				continue;
			}
			// C# ラムダの省略された開き丸括弧の補完
			if(IsCsLambda && HasHiddenOpenParen(Src, ChildIdx ? Src.End(PrevChild) : Src.Start(Node), Src.Start(Child))) {
				if(!Base.empty() && Base.back() != '\n' && Base.back() != ' ') Base += ' ';
				Base += '(';
				Base.append(Sub.data(), Sub.size());
				++NamedSeen;
				continue;
			}
			if(ShouldBreakAfterExpandedBrace(Base, Sub, Language)) Base += '\n';
			// 型宣言の型引数開始記号の分離
			if(IsClassLike && Sub.front() == '<') {
				if(!Base.empty() && Base.back() != ' ') Base += ' ';
				if(Sub.size() > 1 && Sub[1] != ' ') {
					Base += "< ";
					Base.append(Sub.data() + 1, Sub.size() - 1);
				} else Base.append(Sub.data(), Sub.size());
				++NamedSeen;
				continue;
			}
			const bool IsChildElseLike = NodeKind::ElseLikeClause.Contains(ChildType) && NodeTypeView != "guard_statement";
			if(IsRubyStatements && OpenHeredocs) AppendHeredocPendingSeparator(Base);
			else if(
				ChildIdx && (
					PrevType == "ERROR" || ChildType == "ERROR" || IsAfterControlError ||
					HasError && HasSkippedText(Src, Src.End(PrevChild), Src.Start(Child))
				)
			) {
				// `ERROR` 隣接と読み飛ばされた字句は原文の間隙を写し，他分岐の改行挿入や子の再構築による消失を防止
				if(!Base.empty() && Base.back() != '\n' && Base.back() != ' ') AppendGapVerbatim(Base, Src, PrevChild, Child);
			} else if(ChildIdx && !HtmlGaps.empty()) {
				// HTML の子の境界は空白を置ける所でのみ改行し，空白の無い行内の境界は密着
				if(!Base.empty() && Base.back() != '\n') AppendHtmlBoundary(Base, Src, PrevChild, Child, HtmlGaps[ChildIdx - 1], true);
			} else if(const char *const Gap = Language == Lang::PHP && ChildIdx ? PhpInlineHtmlGap(Src, PrevChild, Child, Base) : nullptr) {
				// PHP の地の文の境界は開閉タグの間の空白だけを整形
				if(!Base.empty() && Base.back() != '\n' && Base.back() != ' ') Base += Gap;
			} else if(
				Language == Lang::Swift && PrevType == "directive" || IsPhpAltBody || Language == Lang::PHP && EndsWithPhpColonBlock(PrevChild)
			) {
				if(!Base.empty() && Base.back() != '\n') Base += '\n';
			} else if(IsClassLike && IsAfterInherit) {
				IsAfterInherit = false;
				if(!Base.empty() && Base.back() == ' ') Base.pop_back();
				Base += ' ';
			} else if(!Base.empty() && Base.back() != '\n' && Base.back() != '}' && IsChildElseLike) Base += '\n';
			// ラムダの本体の文の並びは引数の後で改行して開始
			else if(IsLambdaBody && ChildType == "statements" || IsRubyStatements && ChildIdx && ts_node_is_named(PrevChild)) {
				Base += !Base.empty() && Base.back() != '\n' ? "\n" : "";
			} else if(IsStatementList) {
				// Java の enum の本体の宣言部は最初の成員も `;` の後の行へ配置
				Base += (NamedSeen || NodeTypeView == "enum_body_declarations") && !Base.empty() && Base.back() != '\n' ?
				"\n" :
				!Base.empty() && Base.back() != '\n' && Base.back() != ' ' ? " " : "";
			} else if(
				// `(` 始まりの子を直前の識別子に密着
				IsStructural && !Base.empty() && Base.back() != '\n' && (
					!IsLambdaBody && NamedSeen && !(
						Sub.front() == '(' &&
						(std::isalnum(static_cast<unsigned char>(Base.back())) || Base.back() == '=' || Base.back() == '?' || Base.back() == '!') &&
						!(IsRubyMethodDefNode && ChildType == "body_statement")
					) && !(IsChildElseLike && Base.back() == '}') && !(IsStatementsOnlyBreak && ChildType != "statements") &&
					!(IsGoSwitch && !NodeKind::SectionHeader.Contains(ChildType)) &&
					!(IsRubyMethodDefNode && !NodeKind::AmbiguousBodyContainer.Contains(ChildType)) ||
					!NamedSeen && IsSectionHeaderParent && (Base.back() == ':' || IsElse || IsEnsure || IsRescue || IsDo)
				)
			) Base += '\n';
			// 密集節点内の語の単項演算子と被演算子の空白区切り
			else if(
				(!IsCompact || IsUnaryPreOrUpdateNode && !ts_node_is_named(PrevChild)) && !Base.empty() && Base.back() != '\n' &&
				Base.back() != ' ' && ChildIdx
			) {
				if(NodeKind::PreprocBlock.Contains(PrevType)) Base += '\n';
				// `ERROR` 隣接境界は上のチェーン先頭で逐語複写済の為，此処は単項前置／演算子周辺の空白挿入のみ判定
				else if(
					IsUnaryPreOrUpdateNode && !ts_node_is_named(PrevChild) &&
					std::isalpha(static_cast<unsigned char>(Src.Start(PrevChild) < Src.size() ? Src[Src.Start(PrevChild)] : '\0')) ||
					NeedsGapBetween(Src, Node, NodeTypeView, PrevChild, PrevType, Child, ChildType, ChildIdx - 1, Language)
				) Base += ' ';
			}
			Base.append(Sub.data(), Sub.size());
			if(IsRubyStatements) TrackOpenHeredocs(OpenHeredocs, Src, Child);
			++NamedSeen;
			continue;
		}
		// 対象の字句
		const std::string_view Token = Src.View(Child);
		// PHP の代替構文：本体を開く `:` は見出へ密着させ，終端語（`endif` / `endfor` 等）は行頭に置く（地の文の直後は同じ行）
		if(Language == Lang::PHP) {
			if(Token == ":" && IsPhpAltOpener(Node, Child)) {
				TrimTrailingSpaces(Base);
				Base += ':';
				IsPhpAltBody = true;
				PhpAltOpenEnd = Base.size();
				continue;
			}
			if(NodeKind::PhpAltCloser.Contains(Token)) {
				IsPhpAltBody = false;
				// 本体の文の無い構文は，其れを持つ構文と同じく終端語を同じ行へ配置
				if(!Base.empty() && Base.back() != '\n' && Base.back() != ' ') {
					Base += PrevType == "text_interpolation" || Base.size() == PhpAltOpenEnd ? ' ' : '\n';
				}
				Base.append(Token.data(), Token.size());
				continue;
			}
		}
		// Swift の fallthrough は文列の外に無名トークンとして現れる場合も存在
		if(IsSwitchEntry && Token == "fallthrough") {
			if(!Base.empty() && Base.back() != '\n') Base += '\n';
			Base.append(Token);
			continue;
		}
		// Go の展開するリストは改行が区切りを代替する（平坦化時の区切り・ヘッダ・名前付空文は保持）
		if(
			Token.empty() || Token == ";" && (
				Language == Lang::Go && (IsStatementList || NodeKind::GoMemberList.Contains(NodeTypeView)) ||
				Language == Lang::Swift && NodeTypeView == "enum_entry"
			)
		) continue;
		if(ShouldBreakAfterExpandedBrace(Base, Token, Language)) Base += '\n';
		if(IsCompact) {
			Base.append(Token.data(), Token.size());
			continue;
		}
		if(IsClassLike && Token == "<") {
			IsAfterInherit = true;
			Base += " < ";
			continue;
		}
		if(ChildIdx && !Base.empty() && Base.back() != '\n' && Base.back() != ' ') {
			// 前処理条件の後は必ず改行し，do-while と Java 系の else は多行化し，Kotlin の値位置の if は一行を保持
			if(
				NodeKind::PreprocBlock.Contains(PrevType) || Base.back() != '}' && (
					ChildType == "while" && IsDoStatementNode ||
					ChildType == "else" && IsIfParentNode && (Language != Lang::Kotlin || ShouldExpand && !IsKotlinArrowBody(Node, ContainsByNode))
				)
			) Base += '\n';
			else if(NeedsGapBetween(Src, Node, NodeTypeView, PrevChild, PrevType, Child, ChildType, ChildIdx - 1, Language)) Base += ' ';
		}
		if(IsStructural) {
			// 属性を展開した開始又は自己閉じタグの閉じ字句の独立行配置
			if(IsJsxTag && NamedSeen > 1 && (Token == ">" || Token == "/>")) {
				TrimTrailingSpaces(Base);
				if(!Base.empty() && Base.back() != '\n') Base += '\n';
				Base.append(Token.data(), Token.size());
				continue;
			}
			// JSON 配列の角括弧内での改行配置
			if(Token == "{" || IsJsonArray && Token == "[" || IsGoGroupedDeclNode && Token == "(") {
				// Go の一括宣言開始括弧と直前キーワードの空白区切り
				if(Token == "(" && !Base.empty() && Base.back() != ' ' && Base.back() != '\n') Base += ' ';
				Base.append(Token.data(), Token.size());
				if(ChildIdx + 1 < ChildCount && !IsLambdaBody) Base += '\n';
				continue;
			}
			if(Token == "}" || Token == "end" || IsJsonArray && Token == "]" || IsGoGroupedDeclNode && Token == ")") {
				// 見出だけを名前付子に持つ空の本体 (`catch(e: T) {}`) は，開き括弧の後に置いた改行を戻して `{}` に畳込
				if(Base.ends_with("{\n")) Base.pop_back();
				else if(!Base.empty() && Base.back() != '\n' && Base.back() != '{' && Base.back() != '[' && Base.back() != '(') Base += '\n';
				Base.append(Token.data(), Token.size());
				continue;
			}
		}
		Base.append(Token.data(), Token.size());
	}
	// 子を順次連結した最終構造化テキスト Base の返戻
	return Base;
}

/**
 * 構造化テキスト構築関数
 * ヒアドキュメント開始後の改行で後続字句が本文へ入る場合は平坦化する
 * @param Src ソースコード
 * @param Node 対象ノード
 * @param Language 対象言語
 * @param FlatByNode フラットテキストキャッシュ
 * @param StructuredByNode 構造化テキストキャッシュ
 * @param ContainsByNode 展開要因有無キャッシュ
 * @return 構造化テキスト（展開要因が在れば子毎に改行，無ければ平坦形式）
 */
std::string StructurePass::BuildStructuredText(
	const TSSource &Src,
	const TSNode Node,
	const Lang Language,
	NodeTextMap &FlatByNode,
	NodeTextMap &StructuredByNode,
	ContainsStmtBlockMap &ContainsByNode
) {
	// 別の枝で上限へ達した後の組立打切
	if(Src.HasExceededFormatWork()) return {};
	// 対象の字面
	std::string Text = BuildStructuredCore(Src, Node, Language, FlatByNode, StructuredByNode, ContainsByNode);
	// 深いネストで増大する節点別字面の控え上限
	if(!Src.AddFormatWork(Text.size())) return {};
	// 改行を含まない又は Ruby でない構造化テキストの返戻
	if(Language != Lang::Ruby || Text.find('\n') == std::string::npos) return Text;
	// 本文を節点の外に控えた開始（開始は本文と同じ順に並ぶ為，本文を控えるのは最後の開始）
	uint32_t Open = 0;
	const TSNode Last = TrackOpenHeredocs(Open, Src, Node);
	// 本文を控えた開始が無い構造化テキストの返戻
	if(!Open) return Text;
	if(const size_t At = Text.rfind(Src.View(Last)); At == std::string::npos || Text.find('\n', At) == std::string::npos) {
		// 開始の後で改行しない構造化テキストの返戻
		return Text;
	}
	// 開始の後で改行しない１行の平坦テキストの返戻
	return GetFlatText(Src, Node, Language, FlatByNode);
}

/**
 * 構造化テキスト取出関数（唯一の消費者＝親に依る最終消費用）
 * @param Src ソースコード
 * @param Node 対象ノード
 * @param Language 対象言語
 * @param FlatByNode フラットテキストのメモ化マップ
 * @param StructuredByNode 構造化テキストのメモ化マップ
 * @param ContainsByNode 展開要因有無のメモ化マップ
 * @return 構造化テキスト（所有権付）
 */
std::string StructurePass::TakeStructuredText(
	const TSSource &Src,
	const TSNode Node,
	const Lang Language,
	NodeTextMap &FlatByNode,
	NodeTextMap &StructuredByNode,
	ContainsStmtBlockMap &ContainsByNode
) {
	// 全ノードの部分木文字列を保持し続けると記憶量と確保数が「深さ×ソース長」に膨張する為，最終消費時に表から解放
	if(const NodeTextMap::iterator Iter = StructuredByNode.find(Node.id); Iter != StructuredByNode.end()) {
		// 結果の格納先
		std::string Out = std::move(Iter->second);
		StructuredByNode.erase(Iter);
		// メモ化済テキストの移動取出と返戻
		return Out;
	}
	// 未構築ならメモ表を経由せず直接構築して返戻（単一消費ノードの表登録と其の複製を排除）
	return BuildStructuredText(Src, Node, Language, FlatByNode, StructuredByNode, ContainsByNode);
}

/**
 * 構造化テキストメモ化取得関数
 * @param Src ソースコード
 * @param Node 対象ノード
 * @param Language 対象言語
 * @param FlatByNode フラットテキストのメモ化マップ
 * @param StructuredByNode 構造化テキストのメモ化マップ
 * @param ContainsByNode 展開要因有無のメモ化マップ
 * @return 構造化テキストへの参照
 */
const std::string &StructurePass::GetStructuredText(
	const TSSource &Src,
	const TSNode Node,
	const Lang Language,
	NodeTextMap &FlatByNode,
	NodeTextMap &StructuredByNode,
	ContainsStmtBlockMap &ContainsByNode
) {
	// 再ハッシュ前の挿入直後に於ける有効な要素参照の捕捉
	const auto [Iter, Inserted] = StructuredByNode.emplace(Node.id, std::string());
	std::string &Slot = Iter->second;
	if(Inserted) Slot = BuildStructuredText(Src, Node, Language, FlatByNode, StructuredByNode, ContainsByNode);
	// メモ化済の構造化テキストの返戻
	return Slot;
}

/** ========== 言語別検査・適用 ========== */
/**
 * Ruby 式接続列の取得関数
 * @param Text 解析済ソース
 * @return 呼出・添字・演算の型と親型の出現順の列
 */
std::vector<std::pair<std::string_view, std::string_view>> StructurePass::RubyExpressionShape(const TSSource &Text) {
	// 変換前の節点範囲列
	std::vector<std::pair<std::string_view, std::string_view>> Shape;
	const auto RecordChildren = [&Shape](const TSNode Parent) -> void {
		// 親節点の型名
		const std::string_view ParentType = ts_node_type(Parent);
		ForEachNamedChild(
			Parent,
			[&](const TSNode Child) -> void {
				if(const std::string_view Type = ts_node_type(Child); NodeKind::RubyExpressionLink.Contains(Type)) {
					Shape.emplace_back(Type, ParentType);
				}
			}
		);
	};
	RecordChildren(Text.GetRoot());
	WalkChildrenCursor(
		Text.GetRoot(),
		[&](const TSNode Node) -> bool {
			RecordChildren(Node);
			// 全ての子を走査する事の返戻
			return true;
		}
	);
	// 式の接続列の返戻
	return Shape;
}

/**
 * Go 式内の不正改行判定関数
 * @param Text 解析済ソース
 * @return 閉じ波括弧後の改行でセミコロンが挿入される式なら true
 */
bool StructurePass::HasBrokenGoContinuation(const TSSource &Text) {
	// 文法が許容してしまう，式内の閉じ波括弧と後続字句の分断有無の返戻
	return HasDescendantOf(
		Text.GetRoot(),
		[&](const TSNode Node) -> bool {
			if(
				// 節点の型名
				const std::string_view Type = ts_node_type(Node);
				NodeKind::GoStatementBoundary.Contains(Type) || NodeKind::MultilineCommaContainer.Contains(Type)
				// 独立した文・宣言の列と，後処理で末尾コンマを補う列は改行可能としての返戻
			) return false;
			// 直前の節点
			TSNode Previous {};
			bool IsTreeBroken = false;
			ForEachChild(
				Node,
				[&](const TSNode Child) -> bool {
					if(!ts_node_is_null(Previous)) {
						const uint32_t End = Text.End(Previous), Start = Text.Start(Child);
						const std::string_view Next = Text.View(Child);
						// 閉じ波括弧と後続字句（case 節・閉じ波括弧・区切りを除く）の間の改行
						IsTreeBroken = End && End < Start && Text[End - 1] == '}' && std::memchr(Text.data() + End, '\n', Start - End) &&
						!NodeKind::GoCaseClause.Contains(Child) && Next != "}" && Next != ";";
					}
					// 次の兄弟との接続判定に向けた位置更新
					Previous = Child;
					// 不正な継続の発見で走査打切の返戻
					return !IsTreeBroken;
				}
			);
			// 式内部の不正な継続の有無の返戻
			return IsTreeBroken;
		}
	);
}

/**
 * ソース全体構造化再構築関数
 * 計算量：構文木の節点数 V に対し O(V)（節点毎の空白と改行の判定を１度の走査で集める）
 * @param Src 整形対象のソース（結果で上書きされる）
 * @param Language 対象言語
 * @param SkipReason 手間の上限超過時の見送理由格納先
 * @return 式の接続を保存出来れば true
 */
bool StructurePass::NormalizeStructured(TSSource &Src, const Lang Language, std::string_view &SkipReason) {
	// Reparse 内部の深さ優先探索順に沿うコメント紐付
	if(!Src.IsParsed()) return true;
	// 構文木の根節点
	const TSNode Root = Src.GetRoot();
	// 根が `ERROR` の木は構造化しない事の返戻
	if(std::string_view(ts_node_type(Root)) == "ERROR") return true;
	// 並列組立前の HTML 文脈と型引数誤読判定の共通控えへの書込
	if(Language == Lang::HTML) HtmlContextOf(Src, Root);
	else PrepareTemplateContexts(Src, Language);
	// 最上位の組立単位数の取得
	const uint32_t Count = ts_node_named_child_count(Root);
	std::vector<TSNode> RootChildren;
	RootChildren.reserve(Count);
	ForEachNamedChild(
		Root,
		[&](const TSNode Child) -> void {
			RootChildren.push_back(Child);
		}
	);
	// 各最上位子の情報を並列に計算
	std::vector<std::string> ChildTexts(Count);
	std::vector<uint8_t> Skips(Count, 0);
	const size_t CacheReserveHint = (Src.size() >> 2) + 16;
	const auto ProcessRange = [&Src, &Language, Root, &RootChildren, &ChildTexts, &Skips, CacheReserveHint](
		const uint32_t Start,
		const uint32_t End
	) -> void {
		// 担当区間だけで共有する組立済本文の控え
		NodeTextMap FlatCache, StructuredCache;
		ContainsStmtBlockMap ContainsCache;
		// 小さい区間でも確保する控え領域の下限
		const size_t PerChunkReserve = (CacheReserveHint >> 2) + 64;
		// 担当区間の節点数に備えた控え領域の予約
		FlatCache.reserve(PerChunkReserve);
		StructuredCache.reserve(PerChunkReserve);
		ContainsCache.reserve(PerChunkReserve);
		// 担当する最上位節点の順次組立
		for(uint32_t Idx = Start; Idx < End; ++Idx) {
			const TSNode Child = RootChildren[Idx];
			const std::string_view Type(ts_node_type(Child));
			// 矢印の後の値を文と混同しない文脈の記録
			if(Language == Lang::Kotlin) MarkKotlinArrowBodies(Child, Root, ContainsCache);
			// 破損した直前の最上位子に続く宣言終端セミコロンの保持
			if(
				Type == "expression_statement" && Src.Len(Child) == 1 && Src.View(Child) == ";" &&
				!(Idx && ts_node_has_error(RootChildren[Idx - 1]))
			) {
				// 再結合時に空文を省く為の印付
				Skips[Idx] = true;
				continue;
			}
			// PHP の地の文は出力其の物の為，インデント・行末空白を含め原文を其の儘採用
			if(
				Language == Lang::PHP && NodeKind::PhpInlineHtml.Contains(Type) || HasChildOf(
					Child,
					[](const TSNode Sub) -> bool {
						// 壊れた節点かの返戻
						return std::string_view(ts_node_type(Sub)) == "ERROR" || ts_node_is_missing(Sub);
					}
				)
			) {
				// 再構築を避ける節点の原文保持
				ChildTexts[Idx] = Src.Text(Child);
				continue;
			}
			// 一度だけ構築する最上位子のメモ化経路からの除外
			ChildTexts[Idx] = NodeKind::Preproc.Contains(Type) || Language == Lang::HTML ?
			BuildStructuredText(Src, Child, Language, FlatCache, StructuredCache, ContainsCache) :
			CleanupLine(
				Src,
				Child,
				Language,
				BuildStructuredText(Src, Child, Language, FlatCache, StructuredCache, ContainsCache),
				ContainsCache
			);
		}
	};
	Parallel::ForChunks(
		Count,
		Parallel::DecideThreads(Count, 8),
		[&](const size_t Start, const size_t End, const size_t) -> void {
			ProcessRange(static_cast<uint32_t>(Start), static_cast<uint32_t>(End));
		}
	);
	// 深い再帰を抜けた後の主走脈からの上限通知
	if(Src.HasExceededFormatWork()) {
		SkipReason = "structured text exceeds formatting limit";
		// 組立失敗の返戻
		return false;
	}
	// 子の順序を保つ改行結合（空行は後段の配置工程で挿入）
	bool HasPrev = false;
	TSNode PrevChild = {};
	uint32_t OpenHeredocs = 0;
	std::string Result;
	Result.reserve(Src.size());
	// HTML の文書の子の境界は前後の子の表示形式で決まる為，子の並び全体から先に求める（文書の子は全て名前付）
	std::vector<HtmlBoundary> HtmlGaps;
	if(Language == Lang::HTML) CollectHtmlBoundaries(Src, Root, HtmlGaps);
	for(uint32_t Idx = 0; Idx < Count; ++Idx) {
		// 組立時に除外した空文の読飛し
		if(Skips[Idx]) continue;
		// 現在の節点
		const TSNode Cur = RootChildren[Idx];
		// Ruby の __END__ 境界から本文迄を逐語復元し，コードが無い時は先頭空白後から写して DATA の値を保持
		if(Language == Lang::Ruby && std::string_view(ts_node_type(Cur)) == "uninterpreted") {
			// 未解釈データと直前コードの境界の復元
			uint32_t GapStart = HasPrev ? Src.End(PrevChild) : 0;
			if(!HasPrev) {
				while(GapStart < Src.Start(Cur) && (Src[GapStart] == ' ' || Src[GapStart] == '\t' || Src[GapStart] == '\n')) ++GapStart;
			}
			if(const uint32_t End = Src.End(Cur); GapStart < End) Result.append(Src.data() + GapStart, End - GapStart);
			PrevChild = Cur;
			HasPrev = true;
			continue;
		}
		// 誤読された宣言の終端 `;` は独立した最上位子として現れる為，改行で切らず直前へ密着
		if(
			const bool IsAttachSemicolon =
			HasPrev && ChildTexts[Idx] == ";" && (ts_node_has_error(PrevChild) || Language == Lang::Rust || OpenHeredocs);
			HasPrev && !IsAttachSemicolon && !Result.empty() && Result.back() != '\n'
		) {
			// 最上位の `ERROR` 隣接は原文の間隙を写すが，前処理指令境界だけは必ず一改行にして条件コンパイルを保持
			if(
				// 現在節点型の字面
				const std::string_view PrevType(ts_node_type(PrevChild)), CurTypeView(ts_node_type(Cur));
				(PrevType == "ERROR" || CurTypeView == "ERROR") && !(
					Language.IsCFamily() && (
						NodeKind::Preproc.Contains(PrevType) || NodeKind::Preproc.Contains(CurTypeView) || IsPreprocDirectiveAt(Src.View(Cur), 0) ||
						IsPreprocDirectiveAt(Src.View(PrevChild), 0)
					)
				)
			) AppendGapVerbatim(Result, Src, PrevChild, Cur);
			else if(OpenHeredocs && CurTypeView != "heredoc_body") AppendHeredocPendingSeparator(Result);
			// 改行を跨いで前文へ繋がる文のセミコロン区切り
			else if(JoinsAcrossNewline(Language, ChildTexts[Idx])) Result += ";\n";
			else if(const char *const Gap = Language == Lang::PHP ? PhpInlineHtmlGap(Src, PrevChild, Cur, Result) : nullptr) Result += Gap;
			else if(!HtmlGaps.empty()) AppendHtmlBoundary(Result, Src, PrevChild, Cur, HtmlGaps[Idx - 1], true);
			else Result += '\n';
		}
		// 確定した子の本文の結合
		Result += ChildTexts[Idx];
		// 後続の本文待ち状態への追従
		if(Language == Lang::Ruby) TrackOpenHeredocs(OpenHeredocs, Src, Cur);
		PrevChild = Cur;
		HasPrev = true;
	}
	// 組立中に生じた末尾改行の整理
	while(!Result.empty() && Result.back() == '\n') Result.pop_back();
	// 原稿末尾の改行有無の復元
	if(!Src.empty() && Src.back() == '\n') Result += '\n';
	const std::vector<std::pair<std::string_view, std::string_view>> OldShape =
	Language == Lang::Ruby ? RubyExpressionShape(Src) : std::vector<std::pair<std::string_view, std::string_view>>{};
	// 構造化した本文と構文木の差替
	Src.Assign(std::move(Result));
	// 改行による式接続の変化の最終検査
	if(Language == Lang::Ruby && OldShape != RubyExpressionShape(Src) || Language == Lang::Go && HasBrokenGoContinuation(Src)) {
		// 構文・式結合を変える変換の失敗返戻
		return false;
	}
	// 式の接続を保持した構造化の成功の返戻
	return true;
}

/**
 * Python の１行に並べた文の展開関数（複合文の本体を次行へインデントして移し，`;` で並べた単純文を行毎に分ける）
 * @param Src 整形対象のソース（結果で上書きされる）
 */
void StructurePass::ExpandPythonInlineBlocks(TSSource &Src) {
	// Python は複合文の１行化を禁じる為，見出の `:` と同一行に置かれた本体ブロックを次行へ移動
	if(!Src.IsParsed()) return;
	// 原文座標を保った展開編集の収集準備
	const std::string &Source = Src;
	std::vector<TextEdit> Edits;
	WalkAst(
		Src.GetRoot(),
		[&](const TSNode Node) -> void {
			// 節点の型名
			const std::string_view Type(ts_node_type(Node));
			const bool IsBlock = Type == "block";
			// 文を並べる節以外の対象からの除外
			if(!IsBlock && Type != "module") return;
			// 本体の文のインデント（見出行に在る本体は移した先のインデント）
			std::string Indent;
			if(IsBlock) {
				// 対象範囲の開始位置
				const uint32_t Start = Src.Start(Node), LineBegin = TextEdit::LineStartOf(Source, Start);
				uint32_t Pos = LineBegin;
				while(Pos < Start && (Source[Pos] == ' ' || Source[Pos] == '\t')) ++Pos;
				// 本体又は見出の字下げの取得
				Indent.assign(Source, LineBegin, Pos - LineBegin);
				// 行頭から本体開始迄の空白に依る見出行該当有無の判定
				if(Pos < Start) {
					// 見出行のインデントを逐語で継承
					Indent += Pos > LineBegin && Source[Pos - 1] == '\t' ? '\t' : ' ';
					// 見出の `:` と本体の間の空白を改行とインデントで置換
					uint32_t GapStart = Start;
					while(GapStart > LineBegin && (Source[GapStart - 1] == ' ' || Source[GapStart - 1] == '\t')) --GapStart;
					TextEdit::Push(GapStart, Start, "\n" + Indent, Edits);
				}
			}
			TSNode Separator {};
			const auto Split = [&](const TSNode Next) -> void {
				// 文区切りと直前の空白の置換範囲
				uint32_t Begin = Src.Start(Separator);
				while(Begin && (Source[Begin - 1] == ' ' || Source[Begin - 1] == '\t')) --Begin;
				if(
					// 担当範囲の終端索引
					const uint32_t End = Src.End(Separator), NextStart = ts_node_is_null(Next) ? End : Src.Start(Next);
					ts_node_is_null(Next) || ts_node_is_extra(Next) || std::memchr(Source.data() + End, '\n', NextStart - End)
				) TextEdit::Push(Begin, End, "", Edits);
				else TextEdit::Push(Begin, NextStart, "\n" + Indent, Edits);
				// 終了
				return;
			};
			ForEachChild(
				Node,
				[&](const TSNode Child) -> void {
					// 後続節点に応じた文区切りの置換予約
					if(!ts_node_is_null(Separator)) Split(Child);
					// 次の文へ持ち越す区切り位置の更新
					Separator = !ts_node_is_named(Child) && Src.View(Child) == ";" ? Child : TSNode{};
				}
			);
			// 後続文の無い末尾区切りの除去予約
			if(!ts_node_is_null(Separator)) Split(TSNode{});
		}
	);
	// 収集した展開編集の適用
	TextEdit::Apply(Src, Edits);
	// 終了
	return;
}
