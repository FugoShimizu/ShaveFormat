#include "DocSig.hpp"
#include "NodeKind.hpp"
#include "Postprocess.hpp"
#include <algorithm>
#include <cctype>
#include <utility>

/**
 * 対象言語（ドキュメントコメント自動生成対象）の判定関数
 * @param Language 対象言語
 * @return C/C++/Java/Kotlin/PHP/JS/TS/Ruby なら true
 */
bool DocSig::IsTargetLanguage(const Lang Language) {
	// ドキュメントコメント自動生成対象の返戻（PHP は PHPDoc で Doxygen と同形式）
	return Language.IsCFamily() || Language == Lang::Java || Language == Lang::Kotlin || Language == Lang::PHP ||
	Language.IsJsTs() || Language == Lang::Ruby;
}

/**
 * ドキュメントコメント付与アンカーノード（先行コメントが束縛されるノード）の取得関数
 * @param FuncNode 対象関数ノード
 * @param Language 対象言語
 * @return ドキュメントのアンカーノード
 */
TSNode DocSig::DocAnchor(const TSNode FuncNode, const Lang Language) {
	// Parent の初期化
	const TSNode Parent = ts_node_parent(FuncNode);
	// 親ノードが無い場合の返戻
	if(ts_node_is_null(Parent)) return FuncNode;
	// 親ノード種別の取得
	const std::string_view ParentType(ts_node_type(Parent));
	// C++ の template と JS / TS の export，TS のアンビエント宣言の包装は包装直前の文書を採用
	if(NodeKind::DocWrapper.Contains(ParentType)) return Parent;
	// Ruby の先頭メソッドの先行文書は，外へ巻き上げる解析器に合わせ body_statement から取得
	if(Language == Lang::Ruby && ParentType == "body_statement") {
		bool IsFirst = false;
		ForEachNamedChild(
			Parent,
			[&](const TSNode Child) -> bool {
				// 関数ノードが先頭の名前付子かの判定
				IsFirst = ts_node_eq(Child, FuncNode);
				// 最初の名前付の子のみ確認して打切の返戻
				return false;
			}
		);
		// 先頭の名前付子である場合の返戻
		if(IsFirst) return Parent;
	}
	// 既定は関数ノード自身の返戻
	return FuncNode;
}

/**
 * 言語のドキュメントコメント記法スタイルの取得関数
 * @param Language 対象言語
 * @return Ruby は Hash（行コメント），其れ以外は Block（ブロックコメント）
 */
DocSig::Style DocSig::StyleOf(const Lang Language) {
	// Ruby は YARD（行コメント），其れ以外は Doxygen / JSDoc / KDoc の返戻（ブロックコメント）
	return Language == Lang::Ruby ? Style::Hash : Style::Block;
}

/**
 * 戻値タグ文字列の取得関数
 * @param Language 対象言語
 * @return JS/TS は `@returns`，其れ以外は `@return`
 */
std::string_view DocSig::ReturnTag(const Lang Language) {
	// JSDoc は @returns，Doxygen / Javadoc / KDoc / YARD は @return の返戻
	return Language.IsJsTs() ? "@returns" : "@return";
}

/**
 * View 先頭空白（ASCII の空白／タブ）の破壊的削除関数
 * @param View 走査対象の string_view（破壊的に先頭を進める）
 */
void DocSig::SkipSpace(std::string_view &View) {
	// View からの先頭除去
	while(!View.empty() && (View.front() == ' ' || View.front() == '\t')) View.remove_prefix(1);
	// 終了
	return;
}

/**
 * 文書化コメントブロックの判定関数（Doxygen 形式の開始記号で始まり，等号連続のセクション区切り線を含む物は除外）
 * @param Text コメント全文
 * @return 文書化コメントブロックなら true
 */
bool DocSig::IsDocBlock(const std::string_view Text) {
	// Head の初期化
	std::string_view Head = Text;
	SkipSpace(Head);
	// 接頭辞一致と区切り線不在の判定結果の返戻
	return (Head.starts_with("/**") || Head.starts_with("/*!")) && Text.find("====") == std::string_view::npos;
}

/**
 * 次行へ作用する指令コメント判定関数（文書化コメントを其の手前へ置く為に用いる）
 * @param Text コメント全文
 * @return 次行へ作用する指令なら true
 */
bool DocSig::IsNextLineDirective(const std::string_view Text) {
	// コメント記号と前置空白を除いた指令本文の照合
	std::string_view Body = Text;
	while(
		!Body.empty() &&
		(Body.front() == '/' || Body.front() == '#' || Body.front() == '*' || Body.front() == ' ' || Body.front() == '\t')
	) Body.remove_prefix(1);
	// 直後の１行へ作用する指令の判定（単独行では作用せず並替不要の `# noqa`・`# type: ignore` 等は除外）
	static constexpr std::string_view Directives[] = {
		"@phpstan-ignore-next-line",
		"@ts-expect-error",
		"@ts-ignore",
		"NOLINTNEXTLINE",
		"biome-ignore",
		"cppcheck-suppress",
		"deno-lint-ignore",
		"eslint-disable-next-line",
		"istanbul ignore next",
		"nolint:",
		"oxlint-disable-next-line",
		"phpcs:ignore",
		"prettier-ignore",
		"pylint: disable-next",
		"stylelint-disable-next-line",
		"swiftlint:disable:next"
	};
	// 次の行への指示子の返戻
	for(const std::string_view Directive : Directives) if(!Body.compare(0, Directive.size(), Directive)) return true;
	// 何れにも該当しない事の返戻
	return false;
}

/**
 * 処理系が読むシバン・マジックコメントの判定関数（文書化の本文へ取り込むと `# !` 等へ変わり効かなく為る為に用いる）
 * @param Text コメント全文
 * @return シバン (`#!`)，Emacs 形式（`# -*- … -*-`），又は符号化・凍結等の指定（`# frozen_string_literal: true` 等）なら true
 */
bool DocSig::IsMagicComment(const std::string_view Text) {
	// シバンの返戻
	if(Text.starts_with("#!")) return true;
	// コメント印を除いた照合対象
	std::string_view Body = Text.substr(Text.starts_with('#') ? 1 : 0);
	SkipSpace(Body);
	// Emacs 形式の指定の返戻
	if(Body.starts_with("-*-")) return true;
	// 大文字小文字を無視する照合用複製
	std::string Lower(Body);
	// 小文字への正規化
	for(char &Char : Lower) Char = static_cast<char>(std::tolower(static_cast<unsigned char>(Char)));
	// 符号化の指定は Ruby が行中の何処でも読む（`# vim: set fileencoding=utf-8 :` 等）
	if(Lower.find("coding:") != std::string::npos || Lower.find("coding=") != std::string::npos) return true;
	static constexpr std::string_view Keys[] = { "frozen_string_literal:", "shareable_constant_value:", "typed:", "warn_indent:" };
	// 魔法のコメントの鍵の返戻
	for(const std::string_view Key : Keys) if(Lower.starts_with(Key)) return true;
	// 何れにも該当しない事の返戻
	return false;
}

/**
 * ハッシュドキュメント１行の論理内容への変換関数
 * @param Raw ドキュメントコメントの１行（`#` 始まり）
 * @return マーカを除いた論理内容
 */
std::string DocSig::StripHashLine(const std::string_view Raw) {
	// Line の初期化
	std::string_view Line = Raw;
	SkipSpace(Line);
	// コメント印と後続空白の除去
	if(!Line.empty() && Line.front() == '#') Line.remove_prefix(1);
	if(!Line.empty() && Line.front() == ' ') Line.remove_prefix(1);
	// 論理内容の返戻
	return std::string(Line);
}

/**
 * ブロックドキュメント１行の論理内容（マーカ・装飾除去）への変換関数
 * @param Raw ドキュメントコメントの１行
 * @param Protected コード内の保護標識
 * @return マーカを除いた論理内容
 */
std::string DocSig::StripBlockLine(const std::string_view Raw, const std::span<const unsigned char> Protected) {
	// 装飾除去中の生行
	std::string_view Line = Raw;
	SkipSpace(Line);
	const bool HasMarker = Line.starts_with("/*") || Line.starts_with("*");
	if(!HasMarker) Line = Raw;
	// 開きトークン除去（`/*` ＋任意のドキュメント印 `*` / `!` １文字）
	if(Line.starts_with("/*")) {
		Line.remove_prefix(2);
		if(!Line.empty() && (Line.front() == '*' || Line.front() == '!')) Line.remove_prefix(1);
	}
	// 継続印より先の末尾空白と閉じトークンの除去
	while(!Line.empty() && (Line.back() == ' ' || Line.back() == '\t') && !Protected[Line.data() + Line.size() - Raw.data() - 1]) {
		Line.remove_suffix(1);
	}
	// ブロックコメントの閉じトークンの除去
	if(Line.ends_with("*/")) {
		Line.remove_suffix(2);
		while(!Line.empty() && (Line.back() == ' ' || Line.back() == '\t') && !Protected[Line.data() + Line.size() - Raw.data() - 1]) {
			Line.remove_suffix(1);
		}
	}
	// 装飾用の空白１個だけを除き，コード例の相対インデントを残す
	if(!Line.empty() && Line.front() == '*') Line.remove_prefix(1);
	if(HasMarker && !Line.empty() && Line.front() == ' ') Line.remove_prefix(1);
	// 論理内容の返戻
	return std::string(Line);
}

/**
 * ブロックドキュメントコメント全体の論理行列への分解関数（端の裸の開閉トークン行は除外，中間空行は維持）
 * @param BlockText ブロックコメント形式のドキュメントコメントテキスト
 * @return 論理行列
 */
std::vector<std::string> DocSig::BlockLogicalLines(const std::string_view BlockText) {
	// BlockText の１回走査に依る結果の直接構築
	std::vector<std::string> Result;
	Result.reserve(static_cast<size_t>(std::count(BlockText.begin(), BlockText.end(), '\n')) + 1);
	// コード例を削らない為の保護標識
	const std::vector<unsigned char> Protected = Postprocess::CommentCodeMask(BlockText);
	bool SawAny = false;
	// 原文行毎の論理内容抽出
	for(size_t Cursor = 0;;) {
		const size_t Newline = BlockText.find('\n', Cursor);
		const bool IsLast = Newline == std::string_view::npos;
		const size_t Length = IsLast ? BlockText.size() - Cursor : Newline - Cursor;
		std::string Content = StripBlockLine(BlockText.substr(Cursor, Length), std::span(Protected).subspan(Cursor, Length));
		// 開閉トークン側の空行だけ除外し，中間の空行を保持
		if(SawAny && !IsLast || !Content.empty()) {
			Result.push_back(std::move(Content));
			SawAny = true;
		}
		// 最終論理行での走査終了
		if(IsLast) break;
		Cursor = Newline + 1;
	}
	// 論理行列の返戻
	return Result;
}

/**
 * C/C++ の次の宣言子取得関数
 * @param Node 外側の宣言子又は関数ノード
 * @return 仮引数・属性を除いた内側の宣言子（無ければ空節点）
 */
TSNode DocSig::NextDeclarator(const TSNode Node) {
	TSNode Next = TSSource::FieldChild(Node, "declarator");
	if(ts_node_is_null(Next)) {
		// 名前を持たない括弧内の宣言子だけの走査
		ForEachNamedChild(
			Node,
			[&](const TSNode Child) -> bool {
				// 宣言子型の選別
				// 内側の宣言子候補の選別
				if(
					const std::string_view Type(ts_node_type(Child));
					!NodeKind::CFunctionOrFieldDeclarator.Contains(Type) && Type != "parenthesized_declarator"
					// 宣言子以外を読み飛ばす事の返戻
				) return true;
				// 内側宣言子の保持
				Next = Child;
				// 最初の宣言子で探索を終える事の返戻
				return false;
			}
		);
	}
	// 内側の宣言子の返戻
	return Next;
}

/**
 * C / C++ のポインタ／参照返戻判定関数
 * @param FuncNode 関数ノード
 * @return 戻値の宣言子がポインタ又は参照なら true
 */
bool DocSig::ReturnsPointerOrReference(const TSNode FuncNode) {
	// 宣言子鎖の参照及びポインタの探索
	for(TSNode Node = NextDeclarator(FuncNode); !ts_node_is_null(Node); Node = NextDeclarator(Node)) {
		// 宣言子種別の照合
		// ポインタ・参照の宣言子が有る事の返戻
		if(NodeKind::PointerOrRefDeclarator.Contains(Node)) return true;
	}
	// 間接返戻でない事の返戻
	return false;
}

/**
 * 関数本体の値付戻り文有無の判定関数
 * @param Body 関数本体ノード
 * @param Language 対象言語
 * @return 値付戻り文が見付かれば true
 */
bool DocSig::HasBodyValueReturn(const TSNode Body, const Lang Language) {
	// 本体が空ノードの場合の返戻
	if(ts_node_is_null(Body)) return false;
	const bool IsRuby = Language == Lang::Ruby;
	// ネスト関数定義の戻り文は当該関数に属さない為，其の部分木へは降りない
	HeldCursor Cursor(Body);
	while(true) {
		const TSNode Node = ts_tree_cursor_current_node(&Cursor);
		// 戻り文探索中の節点型
		const std::string_view TypeView(ts_node_type(Node));
		const bool IsNestedFunc = !ts_node_eq(Node, Body) && NodeKind::FunctionLikeAny.Contains(TypeView);
		if(!IsNestedFunc) {
			// JavaScript / TypeScript の return_statement は名前付の子（戻値式）が有れば値付
			if(!IsRuby && TypeView == "return_statement" && ts_node_named_child_count(Node)) break;
			// IsRuby の return は argument_list（戻値）が有れば値付
			if(IsRuby && TypeView == "return") {
				bool HasArg = false;
				ForEachNamedChild(
					Node,
					[&](const TSNode Child) -> bool {
						// 子節点型の照合
						// 実引数の並び以外を読み飛ばす事の返戻
						if(std::string_view(ts_node_type(Child)) != "argument_list") return true;
						HasArg = true;
						// 発見で打切の返戻
						return false;
					}
				);
				// Ruby の値付 return 発見時の走査終了
				if(HasArg) break;
			}
		}
		// ネスト関数でない場合の子への降下
		if(!IsNestedFunc && ts_tree_cursor_goto_first_child(&Cursor)) continue;
		// 兄弟への移動と兄弟不在時の親への復帰
		while(!ts_tree_cursor_goto_next_sibling(&Cursor)) {
			// 値付戻り文が無い場合の false の返戻
			if(!ts_tree_cursor_goto_parent(&Cursor) || ts_node_eq(ts_tree_cursor_current_node(&Cursor), Body)) return false;
		}
	}
	// 値付戻り文発見時の true の返戻
	return true;
}

/**
 * 関数ノードからの引数名列・戻値有無の抽出関数
 * @param Src 構文木保持元
 * @param FuncNode 対象関数ノード
 * @param Language 対象言語
 * @return 引数名列と戻値有無
 */
DocSig::Info DocSig::Extract(const TSSource &Src, const TSNode FuncNode, const Lang Language) {
	// シグネチャ抽出結果の蓄積先
	Info Result;
	const bool IsKotlin = Language == Lang::Kotlin;
	// 直接子を走査し型名述語に一致する最後のノードを取得（無ければ空ノード）
	const auto FindLastChildBy = [](const TSNode From, const auto &Pred) -> TSNode {
		// 型名述語に一致する最後の子の探索
		TSNode Match {};
		ForEachNamedChild(
			From,
			[&](const TSNode Child) -> void {
				// 型名述語に一致した子の保持
				if(Pred(std::string_view(ts_node_type(Child)))) Match = Child;
			}
		);
		// 該当ノードの返戻（無ければ空ノード）
		return Match;
	};
	// 直接子から特定の型のノードを１つ取得
	const auto FindChild = [&](const std::string_view Want) -> TSNode {
		// 型名一致の最後の子の返戻（無ければ空ノード）
		return FindLastChildBy(
			FuncNode,
			[&](const std::string_view Type) -> bool {
				// 要求する型名との照合
				return Type == Want;
			}
		);
	};
	// 引数コンテナの特定（C/C++ は宣言子配下，他言語は直接の子）
	TSNode Container {};
	if(Language.IsCFamily()) {
		// 名前に最も近い関数宣言子を採り，返戻先の関数型の仮引数と区別する
		for(TSNode Declarator = NextDeclarator(FuncNode); !ts_node_is_null(Declarator); Declarator = NextDeclarator(Declarator)) {
			if(std::string_view(ts_node_type(Declarator)) == "function_declarator") {
				Container = TSSource::FieldChild(Declarator, "parameters");
			}
		}
	} else {
		// 宣言子を持たない言語は関数直下の最後の引数コンテナを採る
		Container = FindLastChildBy(
			FuncNode,
			[](const std::string_view Type) -> bool {
				// 引数コンテナ型との照合
				return NodeKind::ParamContainerLike.Contains(Type);
			}
		);
	}
	// 引数名の抽出（分割代入パターンは単一名を持たない為，除外）
	if(!ts_node_is_null(Container)) {
		ForEachNamedChild(
			Container,
			[&](const TSNode Entry) -> void {
				// 仮引数候補の選別
				const std::string_view EntryType(ts_node_type(Entry));
				// Kotlin の既定値と Rust の属性等，仮引数でない子を除外
				if(Language == Lang::Kotlin && EntryType != "parameter" || NodeKind::AttachPrefix.Contains(EntryType)) return;
				// 分割代入は単一名を持たない為，JSDoc の内部名を消さない様に既存文書を全保持
				bool IsDestructure = NodeKind::DestructurePattern.Contains(EntryType);
				if(!IsDestructure) {
					ForEachNamedChild(
						Entry,
						[&IsDestructure](const TSNode Sub) -> void {
							// 分割代入パターンの探索
							if(NodeKind::DestructurePattern.Contains(Sub)) IsDestructure = true;
						}
					);
				}
				if(IsDestructure) {
					Result.HasUnnamedParam = true;
					// 分割代入パターンの場合の終了
					return;
				}
				// 言語別の名前識別子を採用（C++ のメンバポインタは限定宣言子の名前）
				const std::string_view ParamNameType = IsKotlin ? "simple_identifier" : Language == Lang::PHP ? "variable_name" : "identifier";
				TSNode Found {};
				// 仮引数名の照合関数
				const auto MatchName = [&](const TSNode Node) -> bool {
					// 仮引数名候補との照合
					const std::string_view NodeType = ts_node_type(Node);
					// メンバポインタ名の抽出
					const TSNode Member = NodeType == "pointer_type_declarator" ? TSSource::FieldChild(Node, "declarator") : TSNode{};
					const bool IsMemberName = !ts_node_is_null(Member) && std::string_view(ts_node_type(Member)) == "type_identifier";
					// 仮引数名の型でない節点の返戻
					if(NodeType != ParamNameType && !IsMemberName) return false;
					Found = IsMemberName ? Member : Node;
					// 最初の発見で打切の返戻
					return true;
				};
				// C / C++ は宣言子の場を優先し，場が無ければ属性内の identifier を除いて探索
				if(
					const TSNode Declarator = Language.IsCFamily() ? TSSource::FieldChild(Entry, "declarator") : TSNode{};
					!ts_node_is_null(Declarator)
				) HasDescendantOf(Declarator, MatchName);
				else if(Language.IsCFamily()) {
					ForEachNamedChild(
						Entry,
						[&](const TSNode Sub) -> void {
							// 型部位を除く仮引数名の探索
							if(ts_node_is_null(Found) && !NodeKind::DeclTypeChild.Contains(Sub)) HasDescendantOf(Sub, MatchName);
						}
					);
				} else if(!MatchName(Entry)) {
					// 注釈・デコレータ・属性（`@PathVariable("id") long id` 等）の中の名前は仮引数名でない為，降りない
					WalkChildrenCursor(
						Entry,
						[&](const TSNode Node) -> bool {
							// 見付ける迄，装飾以外の中へ降りる事の返戻
							return ts_node_is_null(Found) && !MatchName(Node) && !NodeKind::AttachPrefix.Contains(Node);
						}
					);
				}
				// 発見した仮引数名の記録
				if(!ts_node_is_null(Found)) Result.Params.emplace_back(Src.View(Found));
				// C/C++ で名前を持たない実引数（`const T &` 等，引数無を示す `void` や可変長 `...` は除く）の記録
				else if(Language.IsCFamily() && EntryType == "parameter_declaration") {
					if(const std::string_view EntryText = Src.View(Entry); EntryText != "void" && EntryText != "...") Result.HasUnnamedParam = true;
				}
			}
		);
	}
	// 言語別の戻値有無の判定
	switch(Language.Id) {
	case Lang::C:
	case Lang::Cpp:
		{
			// 型節点不在時のコンストラクタ又はデストラクタ扱い
			TSNode TypeNode {};
			ForEachNamedChild(
				FuncNode,
				[&](const TSNode Child) -> void {
					// C / C++ の返戻型節点の探索
					if(ts_node_is_null(TypeNode) && NodeKind::TypeExpression.Contains(Child)) TypeNode = Child;
				}
			);
			// 型が無ければ戻値無，ポインタ・参照なら戻値有，其の他は void 判定
			Result.HasReturn = !ts_node_is_null(TypeNode) && (ReturnsPointerOrReference(FuncNode) || Src.View(TypeNode) != "void");
			Result.ReturnsFromSignature = true;
			break;
		}
	case Lang::Java:
		{
			// 構築子は戻値無，メソッドは void_type 以外の型ノードが有れば戻値有
			if(std::string_view(ts_node_type(FuncNode)) != "constructor_declaration") {
				TSNode TypeNode {};
				ForEachNamedChild(
					FuncNode,
					[&](const TSNode Child) -> void {
						// Java の返戻型節点の探索
						if(ts_node_is_null(TypeNode) && NodeKind::TypeExpression.Contains(Child)) TypeNode = Child;
					}
				);
				Result.HasReturn = !ts_node_is_null(TypeNode) && std::string_view(ts_node_type(TypeNode)) != "void_type";
			}
			Result.ReturnsFromSignature = true;
			break;
		}
	case Lang::Kotlin:
		{
			// function_value_parameters の後最初に現れる型ノードが戻値型（Unit は戻値無扱い）
			bool IsAfterParams = false;
			ForEachNamedChild(
				FuncNode,
				[&](const TSNode Child) -> bool {
					// Kotlin の仮引数列後の返戻型探索
					const std::string_view ChildType(ts_node_type(Child));
					// 仮引数列通過前の処理
					if(!IsAfterParams) {
						if(ChildType == "function_value_parameters") IsAfterParams = true;
						// 引数列より前は走査継続の返戻
						return true;
					}
					if(NodeKind::KotlinReturnType.Contains(ChildType)) Result.HasReturn = Src.View(Child) != "Unit";
					// 引数列の後最初の名前付の子で確定の返戻（型なら判定済，function_body 等なら戻値無）
					return false;
				}
			);
			Result.ReturnsFromSignature = true;
			break;
		}
	case Lang::PHP:
		{
			// 仮引数列の後最初に現れる型ノードが戻値型で，`void` / `never` は値を返さない
			bool IsAfterParams = false, HasReturnType = false;
			ForEachNamedChild(
				FuncNode,
				[&](const TSNode Child) -> bool {
					// Rust の仮引数列後の返戻型探索
					const std::string_view ChildType(ts_node_type(Child));
					// 仮引数列通過前の処理
					if(!IsAfterParams) {
						if(ChildType == "formal_parameters") IsAfterParams = true;
						// 仮引数列より前は走査継続の返戻
						return true;
					}
					// 返戻型の照合
					if(NodeKind::PhpReturnType.Contains(ChildType)) {
						HasReturnType = true;
						if(const std::string_view TypeText = Src.View(Child); TypeText != "void" && TypeText != "never") Result.HasReturn = true;
					}
					// 仮引数列の後最初の名前付の子で確定の返戻（型なら判定済，compound_statement 等なら戻値無）
					return false;
				}
			);
			// PHP の戻値型宣言は任意の為，宣言が無い関数は本体の return から推論する
			if(HasReturnType) Result.ReturnsFromSignature = true;
			else Result.HasReturn = HasBodyValueReturn(FindChild("compound_statement"), Language);
			break;
		}
	case Lang::TypeScript:
		// 直接の子の type_annotation が戻値型注釈（引数の type_annotation は formal_parameters 内）
		if(const TSNode RetAnno = FindChild("type_annotation"); !ts_node_is_null(RetAnno)) {
			// 注釈内の void は値付 return の文書化対象から外す
			bool IsVoid = false;
			ForEachNamedChild(
				RetAnno,
				[&](const TSNode TypeChild) -> void {
					// TypeScript の void 型の探索
					if(Src.View(TypeChild) == "void") IsVoid = true;
				}
			);
			Result.HasReturn = !IsVoid;
			Result.ReturnsFromSignature = true;
		} else Result.HasReturn = HasBodyValueReturn(FindChild("statement_block"), Language);
		break;
	case Lang::JavaScript:
		Result.HasReturn = HasBodyValueReturn(FindChild("statement_block"), Language);
		break;
	case Lang::Ruby:
		{
			// 無終端の定義 (`def f(x) = x + 1`) の本体は式で，其の値を返す
			const TSNode Body = TSSource::FieldChild(FuncNode, "body");
			Result.HasReturn =
			!ts_node_is_null(Body) && (std::string_view(ts_node_type(Body)) != "body_statement" || HasBodyValueReturn(Body, Language));
			break;
		}
	default:
		break;
	}
	// 抽出結果の返戻
	return Result;
}

/**
 * 括弧範囲の破壊的削除関数（View 先頭が `Open` で始まれば `Close` 直後迄を削除，Doxygen `[in]` / JSDoc `{T}` の読飛し用）
 * @param View 走査対象の string_view（破壊的に先頭を進める）
 * @param Open 開括弧文字
 * @param Close 閉括弧文字
 */
void DocSig::SkipBracketed(std::string_view &View, const char Open, const char Close) {
	// 対象括弧の有無の判定
	if(!View.empty() && View.front() == Open) {
		const size_t Pos = View.find(Close);
		View.remove_prefix(Pos == std::string_view::npos ? View.size() : Pos + 1);
	}
	// 終了
	return;
}

/**
 * View 先頭非空白トークン（最初の空白／タブ又は終端迄）長の取得関数
 * @param View 走査対象の string_view
 * @return 先頭トークンの長さ
 */
size_t DocSig::TokenLength(const std::string_view View) {
	// End の初期化
	size_t End = 0;
	while(End < View.size() && View[End] != ' ' && View[End] != '\t') ++End;
	// トークン長の返戻
	return End;
}

/**
 * `@param` タグ見出と Doxygen `[in]` / JSDoc `{T}` の消費関数（View を引数名先頭へ進める）
 * @param View 走査対象の string_view（破壊的に先頭を進める）
 * @param Language 対象言語
 * @return 消費成功なら true，@param でないか別タグなら false
 */
bool DocSig::ConsumeParamHeader(std::string_view &View, const Lang Language) {
	// 引数札の先頭空白の除去
	SkipSpace(View);
	// @param でなければ消費失敗の返戻
	if(!View.starts_with("@param")) return false;
	// タグ語の消費
	View.remove_prefix(6);
	// @paramXxx 別タグ除外（直後は空白／`[`/`{` の何れか）
	if(!View.empty() && View.front() != ' ' && View.front() != '\t' && View.front() != '[' && View.front() != '{') return false;
	// 入出力指定と型注釈の消費
	SkipSpace(View);
	SkipBracketed(View, '[', ']');
	SkipSpace(View);
	SkipBracketed(View, '{', '}');
	SkipSpace(View);
	// PHPDoc だけ裸の型を `$` 始まりの引数名迄読飛し，他言語の説明内の `$` は保持
	if(Language == Lang::PHP && !View.empty() && View.front() != '$') {
		// 型名走査ビューの準備
		std::string_view Scan = View;
		// 引数名迄の型字句の読飛し
		do {
			Scan.remove_prefix(TokenLength(Scan));
			SkipSpace(Scan);
			if(Scan.empty() || Scan.front() == '$') break;
		} while(!Scan.empty());
		if(!Scan.empty()) View = Scan;
	}
	// 消費成功の返戻
	return true;
}

/**
 * `@return` タグ語（直後の複数形 `s` 込）の消費関数（View を直後へ進める）
 * @param View 走査対象の string_view（破壊的に先頭を進める）
 * @return 消費成功なら true，@return 始まりでなければ View 不変で false
 */
bool DocSig::ConsumeReturnWord(std::string_view &View) {
	// @return でなければ消費失敗の返戻
	if(!View.starts_with("@return")) return false;
	// 基本タグ語の消費
	View.remove_prefix(7);
	// 複数形接尾辞の消費
	if(!View.empty() && View.front() == 's') View.remove_prefix(1);
	// 消費成功の返戻
	return true;
}

/**
 * `@return` / `@returns` タグ見出と JSDoc `{T}` の消費関数（View を説明文先頭へ進める）
 * @param View 走査対象の string_view（破壊的に先頭を進める）
 * @return 消費成功なら true，@return でないか別タグなら false
 */
bool DocSig::ConsumeReturnHeader(std::string_view &View) {
	// 返戻札の先頭空白の除去
	SkipSpace(View);
	// `@returnXxx` の別タグとしての除外
	if(!ConsumeReturnWord(View) || !View.empty() && View.front() != ' ' && View.front() != '\t' && View.front() != '{') {
		// 戻値タグ以外は消費しない事の返戻
		return false;
	}
	SkipSpace(View);
	SkipBracketed(View, '{', '}');
	SkipSpace(View);
	// 消費成功の返戻
	return true;
}

/**
 * ドキュメント論理行（@param / @return タグ行）の説明文有無の判定関数
 * @param LogicalLine マーカ除去済のドキュメント論理行
 * @param Language 対象言語
 * @return タグの後に説明文が有れば true
 */
bool DocSig::HasTagLineDescription(const std::string_view LogicalLine, const Lang Language) {
	// View の初期化
	std::string_view View = LogicalLine;
	if(ConsumeParamHeader(View, Language)) {
		// 名前字句の読飛し後の本文有無の判定
		View.remove_prefix(TokenLength(View));
		SkipSpace(View);
		// @param 名前後の説明文有無の返戻
		return !View.empty();
	}
	// 戻値タグの判定
	View = LogicalLine;
	// @return 後の説明文有無の返戻
	if(ConsumeReturnHeader(View)) return !View.empty();
	// タグ行でない事の返戻
	return false;
}

/**
 * 論理行の本文開始がコード内かの判定関数
 * @param Lines 文書の論理行列
 * @param Unclosed 未閉鎖のコード区画が有るかの出力先（不要なら nullptr）
 * @return 各行の最初の非空白文字がコード内なら true
 */
std::vector<bool> DocSig::CodeLineStarts(const std::vector<std::string> &Lines, bool *const Unclosed) {
	// Text の格納先の準備
	std::string Text;
	for(const std::string &Line : Lines) Text.append(Line).push_back('\n');
	// 結合本文のコード領域判定
	const std::vector<unsigned char> Protected = Postprocess::CommentCodeMask(Text, false, Unclosed);
	std::vector<bool> Result;
	Result.reserve(Lines.size());
	size_t Offset = 0;
	// 文書の論理行走査
	for(const std::string &Line : Lines) {
		const size_t First = Line.find_first_not_of(" \t");
		Result.push_back(First != std::string::npos && Protected[Offset + First]);
		Offset += Line.size() + 1;
	}
	// 各行のコード判定の返戻
	return Result;
}

/**
 * `@param <名>` 形式論理行の引数名取得関数
 * @param LogicalLine マーカ除去済のドキュメント論理行
 * @param Language 対象言語
 * @return 引数名（@param 行でなければ空）
 */
std::string_view DocSig::ParamTagName(const std::string_view LogicalLine, const Lang Language) {
	// 引数札の走査ビューの準備
	std::string_view View = LogicalLine;
	// `@param` で始まらない行の空の返戻
	if(!ConsumeParamHeader(View, Language)) return {};
	// 名前トークンの返戻
	return View.substr(0, TokenLength(View));
}

/**
 * `@return` / `@returns` タグ行の判定関数
 * @param LogicalLine マーカ除去済のドキュメント論理行
 * @return 戻値タグ行なら true
 */
bool DocSig::IsReturnTagLine(const std::string_view LogicalLine) {
	// 返戻札の走査ビューの準備
	std::string_view View = LogicalLine;
	// 先頭空白の除去
	SkipSpace(View);
	// `@return` で始まらない事の返戻
	if(!ConsumeReturnWord(View)) return false;
	// @returnXxx の別タグを除外の返戻
	return View.empty() || View.front() == ' ' || View.front() == '\t';
}

/**
 * 既存ドキュメント論理行とシグネチャからの論理行整合関数（実シグネチャと照合し過不足の @param / @return を補正）
 * @param Existing 既存ドキュメントの論理行列（マーカ除去済）
 * @param Sig 実シグネチャ（引数名列・戻値有無）
 * @param ReturnTag 戻値タグ文字列 (@return / @returns)
 * @param Language 対象言語
 * @return 補正後の論理行列
 */
std::vector<std::string> DocSig::ReconcileLogicalLines(
	const std::vector<std::string> &Existing,
	const Info &Sig,
	const std::string_view ReturnTag,
	const Lang Language
) {
	// タグと其の継続説明を一単位として扱い，並替時もコード例の帰属を保つ
	struct Group {
		size_t Begin;
		size_t End;
		std::string_view Name;
		bool ShouldReturn;
		bool IsTagged;
	};
	bool IsUnclosed = false;
	// 論理行のコード領域判定
	const std::vector<bool> Code = CodeLineStarts(Existing, &IsUnclosed);
	// 未閉鎖のコードへ補完タグを挿入すると，説明ではなくコード例の内容に為る為，文書を保つ
	if(IsUnclosed) return Existing;
	std::vector<Group> Groups;
	// 初回の無札説明群の準備
	Groups.push_back({ 0, Existing.size(), {}, false, false });
	bool SawTag = false;
	// タグ開始位置毎の群分割
	for(size_t Index = 0; Index < Existing.size(); ++Index) {
		if(Code[Index]) continue;
		std::string_view Line = Existing[Index];
		SkipSpace(Line);
		if(Line.empty() || Line.front() != '@') continue;
		const std::string_view Name = ParamTagName(Line, Language);
		const bool ShouldReturn = IsReturnTagLine(Line);
		SawTag = SawTag || !Name.empty() || ShouldReturn;
		Groups.back().End = Index;
		Groups.push_back({ Index, Existing.size(), Name, ShouldReturn, SawTag });
	}
	// 引数タグと其の他タグの分類先
	std::vector<const Group *> Params, Other;
	const Group *ExistingReturn = nullptr;
	std::vector<std::string> Result;
	// 元の説明行と不足タグを１度で収められる容量を確保する
	Result.reserve(Existing.size() + Sig.Params.size() + 1);
	const auto Append = [&](const Group &Value) -> void {
		// 既存の論理行群の転写
		Result.insert(Result.end(), Existing.begin() + Value.Begin, Existing.begin() + Value.End);
	};
	// 引数・戻値・其の他のタグの分類
	for(const Group &Value : Groups) {
		if(!Value.Name.empty()) Params.push_back(&Value);
		else if(Value.ShouldReturn) {
			if(!ExistingReturn) ExistingReturn = &Value;
		} else if(Value.IsTagged) Other.push_back(&Value);
		else Append(Value);
	}
	if(Result.empty()) Result.emplace_back();
	// 引数名に対応する組の探索関数
	const auto FindParam = [&](const std::string_view Name) -> const Group * {
		// 名前の一致する仮引数の組の返戻
		for(const Group *const Value : Params) if(Value->Name == Name) return Value;
		// 一致する引数説明不在の返戻
		return nullptr;
	};
	// 無名引数では照合が不完全な為，既存説明を残して名前付引数の不足分だけを加える
	// シグネチャ順の引数タグ構築
	if(Sig.HasUnnamedParam) {
		for(const Group *const Value : Params) Append(*Value);
		for(const std::string &Name : Sig.Params) if(!FindParam(Name)) Result.push_back("@param " + Name);
	} else for(const std::string &Name : Sig.Params) {
		if(const Group *const Value = FindParam(Name)) Append(*Value);
		else Result.push_back("@param " + Name);
	}
	// 戻値がシグネチャで確定しない場合は，既存の説明を推論だけで消さない
	if(ExistingReturn && (Sig.HasReturn || !Sig.ReturnsFromSignature)) Append(*ExistingReturn);
	else if(Sig.HasReturn) Result.emplace_back(ReturnTag);
	// Other の Value 走査
	for(const Group *const Value : Other) Append(*Value);
	// 補正後の論理行列の返戻
	return Result;
}
