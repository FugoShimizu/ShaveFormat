#include "Lint.hpp"
#include "../Util/DocSig.hpp"
#include "../Util/NodeKind.hpp"
#include "../Util/Parallel.hpp"
#include "../Util/TextEdit.hpp"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/** ========== 共通判定 ========== */
/**
 * ノードのソース上テキスト参照取得関数
 * @param Source ソース文字列
 * @param Node 対象ノード
 * @return ノード範囲のテキスト参照（不正範囲なら空）
 */
std::string_view LintPass::NodeView(const std::string &Source, const TSNode Node) {
	// 字面を参照するバイト範囲の取得
	const uint32_t StartByte = ts_node_start_byte(Node), EndByte = ts_node_end_byte(Node);
	// 範囲の壊れた節点の空の返戻
	if(StartByte >= EndByte || EndByte > Source.size()) return {};
	// ソース上の対象範囲を参照として返戻
	return std::string_view(Source.data() + StartByte, EndByte - StartByte);
}

/**
 * 代入可能式の包み剥離識別子名取得関数
 * @param Source ソース文字列
 * @param Lhs 開始ノード
 * @return 識別子名（剥離失敗時は空）
 */
std::string_view LintPass::UnwrapAssignableName(const std::string &Source, TSNode Lhs) {
	// 代入先を包む構文の順次除去
	while(!ts_node_is_null(Lhs)) {
		const std::string_view LhsView(ts_node_type(Lhs));
		// 識別子の名前の返戻
		if(NodeKind::IdentifierLeafLike.Contains(LhsView)) return NodeView(Source, Lhs);
		// 名前を包む節点でない代入先は名前を持たない事の返戻
		if(!NodeKind::KotlinAssignableWrapper.Contains(LhsView)) return {};
		// 最初の名前付子を直接取得する（全子走査を省略，中身の無い包みは空ノードで繰返を離脱）
		Lhs = ts_node_named_child(Lhs, 0);
	}
	// 剥離失敗時は空参照の返戻
	return {};
}

/**
 * メンバ宣言順検査対象言語判定関数
 * @param Language 対象言語
 * @return C++ / C# / Java / PHP の何れかなら true
 */
bool LintPass::IsClassOrderLang(const Lang Language) {
	// 対象言語該当の有無の返戻
	return Language == Lang::Cpp || Language == Lang::CSharp || Language == Lang::Java || Language == Lang::PHP;
}

/**
 * 違反１件追加関数
 * @param Node 違反位置のノード（行・列を取得する）
 * @param Message 警告文（上限を超える分を切り詰めて複製する）
 * @param Out 警告格納先
 */
void LintPass::Push(const TSNode Node, const std::string_view Message, std::vector<LintWarning> &Out) {
	// 警告を結び付けるソース位置の取得
	const TSPoint Point = ts_node_start_point(Node);
	size_t Cut = std::min(Message.size(), MessageMax);
	while(Cut && Cut < Message.size() && (static_cast<unsigned char>(Message[Cut]) & 0XC0) == 0X80) --Cut;
	std::string Text(Message.substr(0, Cut));
	if(Cut < Message.size()) Text += "...";
	// 集成体は `emplace_back` の括弧初期化を受けれない為，波括弧で構築して追加
	Out.push_back({ Point.row, Point.column, std::move(Text) });
	// 終了
	return;
}

/** ========== 規約検査 ========== */
/**
 * アクセス修飾子順序違反検出関数
 * @param Node 対象ノード
 * @param Source ソース文字列
 * @param Language 対象言語
 * @param Out 警告格納先
 */
void LintPass::CheckAccessSpecifierOrder(
	const TSNode Node,
	const std::string &Source,
	const Lang Language,
	std::vector<LintWarning> &Out
) {
	// 条件成立時の返戻
	if(!IsClassOrderLang(Language)) return;
	// アクセス修飾子の順位判定関数
	const auto RankOf = [](const std::string_view Spec) -> int {
		// private:0 / protected:1 / public:2, 其れ以外は -1 の返戻（番兵値）
		return Spec == "private" ? 0 : Spec == "protected" ? 1 : Spec == "public" ? 2 : -1;
	};
	// 順位追跡状態の初期化
	int LastRank = -1;
	bool IsReported = false;
	// 検出順位の追跡関数
	const auto TrackRank = [&](const TSNode Member, const int Rank) -> bool {
		// アクセス修飾子の無いメンバを読み飛ばす事の返戻
		if(Rank < 0) return true;
		// 順位後退時の警告
		if(Rank < LastRank) {
			Push(Member, "Access specifier order violation; expected private → protected → public", Out);
			IsReported = true;
			// 報告済の為，走査打切の返戻
			return false;
		}
		// 最新順位の記録
		LastRank = Rank;
		// 次のメンバへ走査継続の返戻
		return true;
	};
	// C++ はクラス本体内の `access_specifier` 区切り (`public:`/`private:`/`protected:`) で判定
	if(Language == Lang::Cpp) {
		ForEachNamedChild(
			Node,
			[&](const TSNode Child) -> bool {
				// クラス本体のアクセス指定子走査
				if(NodeKind::ClassBody.Contains(Child)) {
					ForEachChild(
						Child,
						[&](const TSNode GrandChild) -> bool {
							// アクセス指定子以外を読み飛ばす事の返戻
							if(std::string_view(ts_node_type(GrandChild)) != "access_specifier") return true;
							// 指定子の字面（`:` を含まない語だけ）の順位に依る走査継続可否の返戻
							return TrackRank(GrandChild, RankOf(NodeView(Source, GrandChild)));
						}
					);
				}
				// 報告済なら走査打切の返戻
				return !IsReported;
			}
		);
	} else {
		// 言語別の修飾子型の準備
		const bool IsJava = Language == Lang::Java;
		const std::string_view ModifierType = Language == Lang::PHP ? "visibility_modifier" : "modifier";
		const std::string_view BodyType = IsJava ? "class_body" : "declaration_list";
		// メンバ毎のアクセス順位判定
		ForEachNamedChild(
			Node,
			[&](const TSNode Body) -> bool {
				// Java の列挙子後の宣言部を含むクラス本体節点の抽出
				const std::string_view Type(ts_node_type(Body));
				if(
					const TSNode Members =
					IsJava && Type == "enum_body" ? FirstNamedChildOfType(Body, "enum_body_declarations") : Type == BodyType ? Body : TSNode{};
					!ts_node_is_null(Members)
				) {
					// 宣言本体のメンバ走査
					ForEachNamedChild(
						Members,
						[&](const TSNode Member) -> bool {
							// 順序の対象外のメンバ（フィールド／メソッド／プロパティ／コンストラクタ以外）を読み飛ばす事の返戻
							if(!NodeKind::AccessOrderMember.Contains(Member)) return true;
							// 修飾子子ノードからの最初のアクセスキーワード取出
							int Rank = -1;
							ForEachChild(
								Member,
								[&](const TSNode Mod) -> bool {
									// 修飾子節点型の判定
									const std::string_view ModView(ts_node_type(Mod));
									// Java の修飾子ノードが持つ複数キーワード子からの採用
									if(IsJava && ModView == "modifiers") {
										ForEachChild(
											Mod,
											[&](const TSNode Keyword) -> bool {
												Rank = RankOf(ts_node_type(Keyword));
												// アクセスキーワード発見で走査打切の返戻
												return Rank < 0;
											}
										);
									} else if(!IsJava && ModView == ModifierType) Rank = RankOf(NodeView(Source, Mod));
									// アクセスキーワード発見で走査打切の返戻
									return Rank < 0;
								}
							);
							// 修飾子の順位に依る走査継続可否の返戻
							return TrackRank(Member, Rank);
						}
					);
				}
				// 報告済なら走査打切の返戻
				return !IsReported;
			}
		);
	}
	// 終了
	return;
}

/**
 * 独立した制御フロー境界の判定関数
 * @param Type 節点の型名
 * @param Language 対象言語
 * @return 外側の制御フローから分離する境界なら true
 */
bool LintPass::IsFlowBoundary(const std::string_view Type, const Lang Language) {
	// Kotlin の inline ラムダからは外側のループへ脱出出来る事の返戻
	return NodeKind::FunctionLikeAny.Contains(Type) || Type == "lambda_literal" && Language != Lang::Kotlin ||
	NodeKind::ClassBody.Contains(Type);
}

/**
 * goto／ラベル付 break・continue 検出関数
 * @param Node 対象ノード
 * @param Language 対象言語
 * @param HasGoto 対象言語が `goto` を持つか（跳び先のラベルの可否は `goto` の側で判ずる）
 * @param Labels ラベルが名付けるループの索引（親を一度だけ走って纏めて記録する）
 * @param Out 警告格納先
 */
void LintPass::CheckGotoOrLabel(
	const TSNode Node,
	const Lang Language,
	const bool HasGoto,
	LabelTargetIndex &Labels,
	std::vector<LintWarning> &Out
) {
	// 節点の型は `Run` の振分で確定済
	if(std::string_view(ts_node_type(Node)) == "goto_statement") {
		// C# の `goto case` / `goto default` は `switch` の転送で，素通りを禁じる言語仕様が要求する書き方の為に対象からの除外
		if(const TSNode Target = ts_node_child(Node, 1); NodeKind::SwitchGotoTarget.Contains(Target)) return;
		// 祖先のループ系ノードを辿り２段以上ネストして居れば多重ループ脱出パターンとして許容
		uint32_t LoopDepth = 0;
		for(TSNode Parent = ts_node_parent(Node); !ts_node_is_null(Parent); Parent = ts_node_parent(Parent)) {
			// 条件成立時の返戻
			if(NodeKind::Loop.Contains(Parent) && ++LoopDepth > 1) return;
		}
		Push(Node, "`goto` outside a multi-loop break; keep it only where other constructs would leak or duplicate cleanup", Out);
	} else {
		// 脱出の参照と，参照されない `goto` 対応言語のラベルを先に除外
		if(
			NodeKind::JumpWithLabel.Contains(ts_node_parent(Node)) || HasGoto && !HasDescendantOf(
				Node,
				[](const TSNode Cur) -> bool {
					// ラベルを伴う脱出を含むかの返戻（`break L` / `continue L` は跳び先の名前を子に保持）
					return NodeKind::LoopExitStatement.Contains(Cur) && ts_node_named_child_count(Cur);
				}
			)
			// 条件成立時の返戻
		) return;
		// ラベル対象がループかの判定関数
		const auto IsLoopNode = [Language](const TSNode Cur) -> bool {
			// 繰返かの返戻
			return NodeKind::Loop.Contains(Cur) && !(Language == Lang::Swift && std::string_view(ts_node_type(Cur)) == "do_statement");
		};
		// 言語別配置に応じたラベル対象ループの位置解決
		TSNode Target {};
		ForEachNamedChild(
			Node,
			[&Target, &IsLoopNode](const TSNode Child) -> bool {
				if(IsLoopNode(Child)) Target = Child;
				// ループを包むラベル (Java / JavaScript / TypeScript / Go / C#) を見付ける迄の走査継続の返戻
				return ts_node_is_null(Target);
			}
		);
		// 文を包むラベルの子に限定した対象解決
		const bool IsWrapping = std::string_view(ts_node_type(Node)) == "labeled_statement";
		// ラベルがループの子に在る形（Rust の `'outer: for`）
		if(!IsWrapping && ts_node_is_null(Target) && IsLoopNode(ts_node_parent(Node))) Target = ts_node_parent(Node);
		// ラベルがループの後の兄弟に在る形
		if(!IsWrapping && ts_node_is_null(Target)) {
			// ラベル節点の型名
			const std::string_view LabelType(ts_node_type(Node));
			if(Labels.find(Node.id) == Labels.end()) {
				TSNode Pending {};
				ForEachNamedChild(
					ts_node_parent(Node),
					// ラベルと直後の文の対応付け関数
					[&Labels, &Pending, LabelType, &IsLoopNode](const TSNode Child) -> void {
						// 間のコメントは飛ばして次の実体を待つ
						if(NodeKind::Comment.Contains(Child)) return;
						if(!ts_node_is_null(Pending)) {
							// ラベル名別の節点索引への登録
							Labels.emplace(Pending.id, IsLoopNode(Child) ? Child : TSNode{});
							Pending = TSNode{};
						}
						if(std::string_view(ts_node_type(Child)) == LabelType) Pending = Child;
					}
				);
				if(!ts_node_is_null(Pending)) Labels.emplace(Pending.id, TSNode{});
			}
			if(const LabelTargetIndex::const_iterator Hit = Labels.find(Node.id); Hit != Labels.end()) Target = Hit->second;
		}
		// goto 対応言語の残余ラベルの脱出可否検査からの除外
		if(ts_node_is_null(Target)) {
			if(!HasGoto) Push(Node, "Labeled statement outside a multi-loop break; extract the loop into a function instead", Out);
			// 終了
			return;
		}
		// ラベル対象内のループ又は switch 保持有無の確認
		bool IsLabelNeeded = false;
		WalkChildrenCursor(
			Target,
			[&IsLabelNeeded, Language](const TSNode Cur) -> bool {
				// 走査は兄弟へ続く為，見付けた後に上書きしない（別の関数の中は外側の脱出に関わらない為に走査除外）
				const std::string_view CurType(ts_node_type(Cur));
				// 発見済又は制御フローの境界なら降りない事の返戻
				if(IsLabelNeeded || IsFlowBoundary(CurType, Language)) return false;
				// Swift の `do` は繰返でない為，内部の多重脱出を調査
				if(
					!(Language == Lang::Swift && CurType == "do_statement") &&
					(NodeKind::LoopOrSwitch.Contains(CurType) || NodeKind::SwitchOrWithExpression.Contains(CurType))
				) IsLabelNeeded = true;
				// 見付かる迄の降下の継続の返戻
				return !IsLabelNeeded;
			}
		);
		// 条件成立時の返戻
		if(IsLabelNeeded) return;
		Push(Node, "Labeled statement outside a multi-loop break; extract the loop into a function instead", Out);
	}
	// 終了
	return;
}

/**
 * 無意味な局所変数検出関数
 * @param Node 対象ノード
 * @param Source ソース文字列
 * @param Out 警告格納先
 */
void LintPass::CheckUselessLocalVariable(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out) {
	// 直前の宣言と返戻文の対応追跡状態の初期化
	TSNode Prev {};
	ForEachNamedChild(
		Node,
		// 直前宣言と返戻文の照合関数
		[&](const TSNode Child) -> void {
			const TSNode Decl = std::exchange(Prev, Child);
			if(ts_node_is_null(Decl) || std::string_view(ts_node_type(Child)) != "return_statement" || !NodeKind::DeclLike.Contains(Decl)) {
				// 終了
				return;
			}
			// 宣言を包む variable_declaration が有れば其の中の宣言子を確認
			const TSNode Wrapper = FirstNamedChildOfType(Decl, "variable_declaration");
			std::string_view DeclName;
			ForEachNamedChild(
				ts_node_is_null(Wrapper) ? Decl : Wrapper,
				[&Source, &DeclName](const TSNode Declarator) -> bool {
					// 初期化付の宣言子以外を読み飛ばす事の返戻
					if(!NodeKind::DeclInitDeclarator.Contains(Declarator)) return true;
					// 宣言子からの識別名抽出
					if(const TSNode Identifier = FirstNamedChildOfType(Declarator, "identifier"); !ts_node_is_null(Identifier)) {
						DeclName = NodeView(Source, Identifier);
					}
					// 宣言名取得済なら走査打切，未取得なら次の兄弟への返戻
					return DeclName.empty();
				}
			);
			// 条件成立時の返戻
			if(DeclName.empty()) return;
			// 宣言名と一致する返却識別子の冗長局所変数認定
			if(
				// 返却された識別子節点
				const TSNode Returned = FirstNamedChildOfType(Child, "identifier");
				!ts_node_is_null(Returned) && NodeView(Source, Returned) == DeclName
			) Push(Decl, "Variable immediately returned; inline it if semantics are unchanged", Out);
		}
	);
	// 終了
	return;
}

/**
 * 部分木の指定型ノード包含判定関数
 * @param Node 対象ノード
 * @param Type 探索するノード型名
 * @return 指定型の子孫を含めば true
 */
bool LintPass::HasDescendantType(const TSNode Node, const std::string_view Type) {
	// 指定型子孫の有無の返戻
	return HasDescendantOf(
		Node,
		// 指定型との一致判定関数
		[Type](const TSNode Cur) -> bool {
			// 指定型ノードに合致するかの返戻
			return std::string_view(ts_node_type(Cur)) == Type;
		}
	);
}

/**
 * 型推論キーワードの例外文脈判定関数
 * @param AutoNode auto / var / any 等のキーワードノード
 * @return 標準仕様の例外文脈に有れば true
 */
bool LintPass::IsAutoExceptionContext(const TSNode AutoNode) {
	// 型推論の置かれた宣言文脈の取得
	const TSNode Parent = ts_node_parent(AutoNode);
	// 親の無い auto は例外でない事の返戻
	if(ts_node_is_null(Parent)) return false;
	const std::string_view ParentView(ts_node_type(Parent));
	// `[](auto &x){}` のジェネリックラムダ仮引数
	if(NodeKind::AutoParameterHost.Contains(ParentView)) return true;
	// 通常宣言の場合は初期化子部分を解析して例外判定
	if(ParentView != "declaration") return false;
	// 例外文脈に当たる初期化子を持つ宣言子の有無の返戻
	return HasChildOf(
		Parent,
		// 初期化子付宣言子の判定関数
		[](const TSNode Child) -> bool {
			// 初期化子付の宣言子が例外文脈の初期化子を持つかの返戻
			return std::string_view(ts_node_type(Child)) == "init_declarator" && HasChildOf(
				Child,
				// 型推論を許容する初期化子の判定関数
				[](const TSNode GrandChild) -> bool {
					// 型名を書けない構造化束縛・ラムダと冗長なテンプレート呼出での型推論の許容
					const std::string_view GrandChildView(ts_node_type(GrandChild));
					// 例外文脈の初期化子かの返戻
					return NodeKind::CppUnnamableInitializer.Contains(GrandChildView) ||
					GrandChildView == "call_expression" && HasDescendantType(GrandChild, "template_function");
				}
			);
		}
	);
}

/**
 * auto / var / any 使用検出関数
 * @param Node 対象ノード
 * @param Source ソース文字列
 * @param Language 対象言語
 * @param Out 警告格納先
 */
void LintPass::CheckTypeInferenceKeyword(
	const TSNode Node,
	const std::string &Source,
	const Lang Language,
	std::vector<LintWarning> &Out
) {
	// 節点の型は `Run` の振分で言語毎に確定済
	switch(Language.Id) {
	case Lang::Cpp:
		// 型名を書けない又は冗長な構文に於ける auto の許容
		if(!IsAutoExceptionContext(Node)) Push(Node, "`auto` is discouraged; use an explicit type", Out);
		// 終了
		return;
	case Lang::CSharp:
		// C# `var` は implicit_type ノード，識別子テキストが `var` の場合は変数名／参照と区別出来ない為，対象外
		Push(Node, "`var` is discouraged; use an explicit type", Out);
		// 終了
		return;
	case Lang::Java:
		// Java 10 の `var` に対する type_identifier としての取扱
		if(NodeView(Source, Node) == "var") Push(Node, "`var` is discouraged; use an explicit type", Out);
		// 終了
		return;
	case Lang::TypeScript:
		if(NodeView(Source, Node) == "any") Push(Node, "`any` is discouraged; use `unknown` or a specific type", Out);
		// 終了
		return;
	default:
		// 終了
		return;
	}
}

/**
 * 識別子内の Unicode 書式文字判定関数
 * @param Point 符号点
 * @return C# の識別子比較で除外する `Cf` なら true
 */
bool LintPass::IsIdentifierFormat(const uint32_t Point) {
	// Unicode 17.0.0 の `General_Category=Cf` 一覧
	static constexpr uint32_t Ranges[][2] = {
		{ 0XAD, 0XAD },
		{ 0X600, 0X605 },
		{ 0X61C, 0X61C },
		{ 0X6DD, 0X6DD },
		{ 0X70F, 0X70F },
		{ 0X890, 0X891 },
		{ 0X8E2, 0X8E2 },
		{ 0X180E, 0X180E },
		{ 0X200B, 0X200F },
		{ 0X202A, 0X202E },
		{ 0X2060, 0X2064 },
		{ 0X2066, 0X206F },
		{ 0XFEFF, 0XFEFF },
		{ 0XFFF9, 0XFFFB },
		{ 0X110BD, 0X110BD },
		{ 0X110CD, 0X110CD },
		{ 0X13430, 0X1343F },
		{ 0X1BCA0, 0X1BCA3 },
		{ 0X1D173, 0X1D17A },
		{ 0XE0001, 0XE0001 },
		{ 0XE0020, 0XE007F }
	};
	// 識別子を成さない文字の範囲に入るかの返戻
	for(const uint32_t (&Range)[2] : Ranges) if(Point >= Range[0] && Point <= Range[1]) return true;
	// `Cf` 以外である事の返戻
	return false;
}

/**
 * ラベル識別子の表記統一関数
 * @param Name 識別子の原表記
 * @param Language 対象言語
 * @return 比較用の名前（復号不正・空識別子は空）
 */
std::string LintPass::LabelName(std::string_view Name, const Lang Language) {
	// PHP のラベルは其の儘比べる事の返戻
	if(Language == Lang::PHP) return std::string(Name);
	// C# の逐語的識別子接頭辞の除去
	if(Language == Lang::CSharp && Name.starts_with('@')) Name.remove_prefix(1);
	// 統一表記の組立
	std::string Result;
	for(size_t Idx = 0; Idx < Name.size();) {
		// 識別文字の開始位置
		const size_t StartPos = Idx;
		uint32_t Value = 0;
		const bool IsEscaped = Name[Idx] == '\\';
		if(IsEscaped) {
			// `\u` / `\U` でない逃避は解決に使わない事の返戻
			if(Idx + 1 == Name.size() || Name[Idx + 1] != 'u' && Name[Idx + 1] != 'U') return {};
			// 数値リテラルの数字列
			const size_t Digits = Name[Idx + 1] == 'u' ? 4 : 8;
			// 桁の足りない逃避は解決に使わない事の返戻
			if(Name.size() - Idx < Digits + 2) return {};
			const char *const Start = Name.data() + Idx + 2;
			const std::from_chars_result Parsed = std::from_chars(Start, Start + Digits, Value, 16);
			if(
				Parsed.ec != std::errc{} || Parsed.ptr != Start + Digits || Value > 0X10FFFF || Value > 0XD7FF && Value < 0XE000 ||
				Language.IsCFamily() && Value < 0XA0 && Value != 0X24 && Value != 0X40 && Value != 0X60
				// 不正な文字表記は飛先の解決に使わない事の返戻
			) return {};
			// 走査位置の索引の進行
			Idx += Digits + 2;
			// 不正な UTF-8 の名前は解決に使わない事の返戻
		} else if(!TextEdit::DecodeUtf8(Name, Idx, Value)) return {};
		if(Language == Lang::CSharp && IsIdentifierFormat(Value)) {
			// `Cf` は識別子の先頭には使用不可
			if(!StartPos) return {};
			continue;
		}
		if(!IsEscaped) Result.append(Name.substr(StartPos, Idx - StartPos));
		else TextEdit::EncodeUtf8(Result, Value);
	}
	// 同じ識別子に対する統一表記の返戻
	return Result;
}

/**
 * 名前無子トークンからの候補一致トークン検索関数
 * @param Source ソース文字列
 * @param Node 走査対象ノード
 * @param Option1 １番目の候補トークン
 * @param Option2 ２番目の候補トークン
 * @return 一致トークン参照（見付からなければ空）
 */
std::string_view LintPass::FindUnnamedToken(
	const std::string &Source,
	const TSNode Node,
	const std::string_view Option1,
	const std::string_view Option2
) {
	// 検索結果の格納先準備
	std::string_view Result;
	ForEachChild(
		Node,
		[&](const TSNode Child) -> bool {
			// 名前無子の候補照合
			if(!ts_node_is_named(Child)) if(const std::string_view Token = NodeView(Source, Child); Token == Option1 || Token == Option2) {
				Result = Token;
				// 一致発見時の走査打切の返戻（対象字句は１節点に高々１つ）
				return false;
			}
			// 未発見の為，走査継続の返戻
			return true;
		}
	);
	// 一致トークンの返戻
	return Result;
}

/**
 * 関数内 goto 飛先収集関数
 * @param Body 関数本体
 * @param Source ソース文字列
 * @param Language 対象言語
 * @return 飛先と其れに因る到達候補位置
 */
LintPass::GotoInfo LintPass::CollectGotoTargets(const TSNode Body, const std::string &Source, const Lang Language) {
	// 飛先索引の初期化
	GotoInfo Result;
	// goto を持たない言語の空の結果の返戻
	if(!Language.IsCFamily() && Language != Lang::CSharp && Language != Lang::PHP) return Result;
	// ラベル名別の節点索引
	std::unordered_multimap<std::string, TSNode> Labels;
	std::vector<TSNode> Jumps;
	HeldCursor Cursor(Body);
	for(bool IsDone = false; !IsDone;) {
		const TSNode Node = ts_tree_cursor_current_node(&Cursor);
		const std::string_view Type(ts_node_type(Node));
		if(Type == "goto_statement") Jumps.push_back(Node);
		else if(NodeKind::LabeledStatement.Contains(Type)) {
			// 対象の識別名
			const std::string Name = LabelName(NodeView(Source, ts_node_named_child(Node, 0)), Language);
			if(!Name.empty()) Labels.emplace(Name, Node);
		}
		if(!IsFlowBoundary(Type, Language) && ts_tree_cursor_goto_first_child(&Cursor)) continue;
		while(!ts_tree_cursor_goto_next_sibling(&Cursor)) if(!ts_tree_cursor_goto_parent(&Cursor)) {
			IsDone = true;
			break;
		}
	}
	// 移動先が未解決か
	bool IsUnknown = false;
	for(const TSNode Jump : Jumps) {
		TSNode Target {};
		if(Language == Lang::CSharp && !FindUnnamedToken(Source, Jump, "case", "default").empty()) {
			for(TSNode Parent = ts_node_parent(Jump); !ts_node_is_null(Parent); Parent = ts_node_parent(Parent)) {
				if(std::string_view(ts_node_type(Parent)) == "switch_statement") {
					Target = Parent;
					break;
				}
				if(IsFlowBoundary(ts_node_type(Parent), Language)) break;
			}
		} else {
			// 対象の識別名
			TSNode Name {};
			ForEachNamedChild(
				Jump,
				[&](const TSNode Child) -> bool {
					// コメントを読み飛ばす事の返戻
					if(NodeKind::Comment.Contains(Child)) return true;
					// 最初の実引数の採用
					Name = Child;
					// 最初の実引数で探索を終える事の返戻
					return false;
				}
			);
			// 担当範囲の終端索引
			const auto [Begin, End] = Labels.equal_range(LabelName(NodeView(Source, Name), Language));
			uint32_t Closest = 0;
			for(std::unordered_multimap<std::string, TSNode>::const_iterator Iter = Begin; Iter != End; ++Iter) {
				// 対象ラベルの節点
				const TSNode Label = Iter->second;
				if(Language == Lang::CSharp) {
					TSNode Scope = ts_node_parent(Label);
					while(!ts_node_is_null(Scope) && !NodeKind::ScopeBody.Contains(Scope)) Scope = ts_node_parent(Scope);
					if(
						ts_node_is_null(Scope) || ts_node_start_byte(Jump) < ts_node_start_byte(Scope) ||
						ts_node_end_byte(Jump) > ts_node_end_byte(Scope) || !ts_node_is_null(Target) && ts_node_start_byte(Scope) <= Closest
					) continue;
					Closest = ts_node_start_byte(Scope);
				}
				Target = Label;
			}
		}
		// ジャンプ別の到達先索引への登録
		Result.Targets.emplace(Jump.id, Target);
		if(ts_node_is_null(Target)) IsUnknown = true;
		else Result.Entries.push_back(ts_node_start_byte(Target));
	}
	// 未解決の飛先を持つ関数では，何のラベルも到達不能と断定の回避
	if(IsUnknown) for(const auto &[Name, Label] : Labels) Result.Entries.push_back(ts_node_start_byte(Label));
	// 外部から到達するラベル位置列の位置順への整列
	std::sort(Result.Entries.begin(), Result.Entries.end());
	// 外部から到達するラベル位置列からの対象要素除去
	Result.Entries.erase(std::unique(Result.Entries.begin(), Result.Entries.end()), Result.Entries.end());
	// 関数内で共有する飛先の返戻
	return Result;
}

/**
 * 節点内への goto 入口有無判定関数
 * @param Node 検査する節点
 * @param Gotos 同じ関数内の goto 飛先
 * @return 外から到達候補となるラベルが有れば true
 */
bool LintPass::HasGotoEntry(const TSNode Node, const GotoInfo &Gotos) {
	// 節点範囲の最初の入口候補の探索
	const std::vector<uint32_t>::const_iterator Entry =
	std::lower_bound(Gotos.Entries.begin(), Gotos.Entries.end(), ts_node_start_byte(Node));
	// 節点の範囲に入口が含まれるか返戻
	return Entry != Gotos.Entries.end() && *Entry < ts_node_end_byte(Node);
}

/**
 * 先頭の子の型名取得関数
 * @param Node 対象ノード
 * @return 先頭の子（無名の字句を含む）の型名（子が無ければ空）
 */
std::string_view LintPass::FirstChildType(const TSNode Node) {
	const TSNode First = ts_node_child(Node, 0);
	// 先頭の子の型名の返戻
	return ts_node_is_null(First) ? std::string_view{} : ts_node_type(First);
}

/**
 * 同じ制御フロー内の返戻文包含判定関数
 * @param Node 検索する節点
 * @param Language 対象言語
 * @return ネスト関数の本体を除いて返戻文が有れば true
 */
bool LintPass::HasReturnInFlow(const TSNode Node, const Lang Language) {
	// 走査状態の初期化
	bool IsFound = false;
	// 構文木走査の開始
	HeldCursor Cursor(Node);
	for(bool IsDone = false; !IsDone;) {
		// 現在の節点
		const TSNode Current = ts_tree_cursor_current_node(&Cursor);
		if(const std::string_view Type(ts_node_type(Current)); !IsFlowBoundary(Type, Language)) {
			// Kotlin / Swift の return は脱出の文の先頭の語で区別
			if(Type == "return_statement" || NodeKind::KeywordJump.Contains(Type) && FirstChildType(Current) == "return") {
				IsFound = true;
				break;
			}
			if(ts_tree_cursor_goto_first_child(&Cursor)) continue;
		}
		while(!ts_tree_cursor_goto_next_sibling(&Cursor)) if(!ts_tree_cursor_goto_parent(&Cursor)) {
			IsDone = true;
			break;
		}
	}
	// 同じ関数に属する返戻文の有無を返戻
	return IsFound;
}

/**
 * 内部転送の判定範囲包含関数
 * @param Node 転送先
 * @param Scope 全節を併せて判定する範囲
 * @return 転送先が判定範囲内なら true
 */
bool LintPass::IsWithinScope(const TSNode Node, const TSNode Scope) {
	// 空の範囲は内部転送を許可しない事の返戻
	return !ts_node_is_null(Node) && !ts_node_is_null(Scope) && ts_node_start_byte(Node) >= ts_node_start_byte(Scope) &&
	ts_node_end_byte(Node) <= ts_node_end_byte(Scope);
}

/**
 * 対象ループからの脱出有無判定関数
 * @param Body 検索する本体
 * @param Loop 対象ループ
 * @param Source ソース文字列
 * @param Language 対象言語
 * @param Gotos 同じ関数内の goto 飛先
 * @param TransferScope 内部転送を認める切替文の判定範囲
 * @return ループ自身又は其の外側への脱出が有れば true
 */
bool LintPass::HasLoopExit(
	const TSNode Body,
	const TSNode Loop,
	const std::string &Source,
	const Lang Language,
	const GotoInfo &Gotos,
	const TSNode TransferScope
) {
	// 本体の無い繰返は脱出しない事の返戻
	if(ts_node_is_null(Body)) return false;
	// 脱出先が対象ループを離れるかの判定関数
	const auto LeavesLoop = [Loop](TSNode Target, const bool Continues) -> bool {
		// 継続文が指す実際のループの解決
		if(Continues) {
			while(std::string_view(ts_node_type(Target)) == "labeled_statement") {
				// 対象要素の個数
				const uint32_t Count = ts_node_named_child_count(Target);
				// 文を持たないラベルは繰返を指さない事の返戻
				if(!Count) return true;
				Target = ts_node_named_child(Target, Count - 1);
			}
			// 飛先が繰返自身なら脱出でない事の返戻
			if(ts_node_eq(Target, Loop)) return false;
		}
		// 対象ループを包むラベル付ブロックへの脱出も含める事の返戻
		return ts_node_start_byte(Target) <= ts_node_start_byte(Loop) && ts_node_end_byte(Target) >= ts_node_end_byte(Loop);
	};
	// ラベル又は段数からの脱出先の解決
	const auto ResolveExit = [&](const TSNode Node, const bool Continues) -> bool {
		// 対象の引数節点
		TSNode Argument {};
		ForEachNamedChild(
			Node,
			[&](const TSNode Child) -> bool {
				// コメントを読み飛ばす事の返戻
				if(NodeKind::Comment.Contains(Child)) return true;
				// 最初の実引数の採用
				Argument = Child;
				// 最初の実引数で探索を終える事の返戻
				return false;
			}
		);
		// 脱出指定のラベル又は段数への分解
		std::string_view Label;
		uint32_t Level = 1;
		if(!ts_node_is_null(Argument)) {
			if(Language == Lang::PHP) {
				while(!ts_node_is_null(Argument) && std::string_view(ts_node_type(Argument)) == "parenthesized_expression") {
					Argument = ts_node_named_child(Argument, 0);
				}
				// 飛先の無い文を読み飛ばす事の返戻
				if(ts_node_is_null(Argument)) return true;
				// 数値リテラルの数字列
				std::string Digits(NodeView(Source, Argument));
				std::erase(Digits, '_');
				std::string_view Number(Digits);
				int Base = 10;
				// 脱出段数の基数接頭辞の解釈
				if(Number.size() > 1 && Number.front() == '0') {
					// 基数を表す接頭辞
					const char Prefix = Number[1];
					Base = Prefix == 'x' || Prefix == 'X' ? 16 : Prefix == 'b' || Prefix == 'B' ? 2 : 8;
					if(Prefix == 'x' || Prefix == 'X' || Prefix == 'b' || Prefix == 'B' || Prefix == 'o' || Prefix == 'O') Number.remove_prefix(2);
				}
				const std::from_chars_result Parsed = std::from_chars(Number.data(), Number.data() + Number.size(), Level, Base);
				// 脱出段数の解析不能時に於ける無限ループ断定の回避
				if(Parsed.ec != std::errc{} || Parsed.ptr != Number.data() + Number.size() || !Level) return true;
			} else Label = NodeView(Source, Argument);
		}
		// 脱出対象となる祖先文の探索
		for(TSNode Parent = ts_node_parent(Node); !ts_node_is_null(Parent); Parent = ts_node_parent(Parent)) {
			const std::string_view Type(ts_node_type(Parent));
			// 関数等の境界で遡りを止めた結果の返戻
			if(IsFlowBoundary(Type, Language)) return LeavesLoop(Parent, false);
			if(Label.empty()) {
				if(
					const bool IsBreakable = NodeKind::LoopOrSwitch.Contains(Type) && !(Language == Lang::Swift && Type == "do_statement") ||
					Language == Lang::Java && Type == "switch_expression";
					IsBreakable && (!Continues || Language == Lang::PHP || NodeKind::Loop.Contains(Type)) && !--Level
					// 指定段数の脱出先との位置関係の返戻
				) return LeavesLoop(Parent, Continues && !(Language == Lang::PHP && Type == "switch_statement"));
			} else {
				if(Type == "labeled_statement" && NodeView(Source, ts_node_named_child(Parent, 0)) == Label) {
					// 最も内側の一致ラベルへの脱出判定の返戻
					return LeavesLoop(Parent, Continues);
				}
				// Swift/Kotlin のラベルは被修飾文の直前兄弟に存在
				if(Language == Lang::Swift || Language == Lang::Kotlin) {
					for(
						// 直前の節点
						TSNode Previous = ts_node_prev_named_sibling(Parent);
						!ts_node_is_null(Previous);
						Previous = ts_node_prev_named_sibling(Previous)
					) {
						// 直前節点の型名
						const std::string_view PreviousType(ts_node_type(Previous));
						if(NodeKind::Comment.Contains(PreviousType) || Language == Lang::Kotlin && PreviousType == "annotation") continue;
						if(!NodeKind::LabelNode.Contains(PreviousType)) break;
						std::string_view Name = NodeView(Source, Previous);
						if(!Name.empty() && (Name.back() == ':' || Name.back() == '@')) Name.remove_suffix(1);
						// ラベルの一致した文を脱出先とする結果の返戻
						if(Name == Label) return LeavesLoop(Parent, Continues);
					}
				}
			}
		}
		// 未解決の脱出指定を無限ループの証拠にしない事の返戻
		return true;
	};
	// 脱出文走査状態の初期化
	bool IsFound = false;
	// 構文木走査の開始
	HeldCursor Cursor(Body);
	for(bool IsDone = false; !IsDone;) {
		const TSNode Node = ts_tree_cursor_current_node(&Cursor);
		const std::string_view Type(ts_node_type(Node));
		if(Type == "goto_statement") {
			// 事前収集したラベル転送先の照会
			const std::unordered_map<const void *, TSNode>::const_iterator FoundTarget = Gotos.Targets.find(Node.id);
			const TSNode Destination = FoundTarget == Gotos.Targets.end() ? TSNode{} : FoundTarget->second;
			TSNode Target = Destination;
			// ループ自身のラベルへの goto の再開に向けた実体文抽出
			while(
				!ts_node_is_null(Target) &&
				(std::string_view(ts_node_type(Target)) == "named_label_statement" || NodeKind::Comment.Contains(Target))
			) Target = ts_node_next_named_sibling(Target);
			while(!ts_node_is_null(Target) && std::string_view(ts_node_type(Target)) == "labeled_statement") {
				const uint32_t Count = ts_node_named_child_count(Target);
				if(!Count) break;
				Target = ts_node_named_child(Target, Count - 1);
			}
			if(
				!IsWithinScope(Destination, TransferScope) && !ts_node_eq(Target, Loop) && (
					ts_node_is_null(Destination) || ts_node_start_byte(Destination) < ts_node_start_byte(Loop) ||
					ts_node_start_byte(Destination) >= ts_node_end_byte(Loop)
				)
			) {
				IsFound = true;
				break;
			}
		} else if(
			// ループ脱出文の該当有無
			const bool IsExitStatement = NodeKind::LoopExitStatement.Contains(Type);
			IsExitStatement || NodeKind::KeywordJump.Contains(Type)
		) {
			// Kotlin・Swift の脱出文の先頭語に依る区別
			const std::string_view KeywordType = FirstChildType(Node);
			if(
				const bool Continues = NodeKind::ContinueKeyword.Contains(KeywordType);
				(IsExitStatement || Continues || NodeKind::BreakKeyword.Contains(KeywordType)) && ResolveExit(Node, Continues)
			) {
				IsFound = true;
				break;
			}
		}
		// 制御フロー境界を除く深さ優先走査
		if(!IsFlowBoundary(Type, Language) && ts_tree_cursor_goto_first_child(&Cursor)) continue;
		while(!ts_tree_cursor_goto_next_sibling(&Cursor)) if(!ts_tree_cursor_goto_parent(&Cursor)) {
			IsDone = true;
			break;
		}
	}
	// 解決済の脱出有無の返戻
	return IsFound;
}

/**
 * ループ条件の真値リテラル判定関数
 * @param Cond 判定対象の条件式ノード
 * @param Source ソース文字列
 * @return 真値リテラルなら true
 */
bool LintPass::IsAlwaysTrueCondition(TSNode Cond, const std::string &Source) {
	// 常真判定に不要な条件の包みの除去
	while(!ts_node_is_null(Cond)) {
		// 条件節点型の字面
		const std::string_view CondView(ts_node_type(Cond));
		// 常に真の条件の返戻
		if(CondView == "true" || CondView == "number_literal" && NodeView(Source, Cond) == "1") return true;
		if(CondView == "boolean") {
			// 対象の字面
			const std::string_view Text = NodeView(Source, Cond);
			// PHP の真偽値名は大文字小文字を区別しない事の返戻
			return Text.size() == 4 && (Text[0] | 0X20) == 't' && (Text[1] | 0X20) == 'r' && (Text[2] | 0X20) == 'u' &&
			(Text[3] | 0X20) == 'e';
		}
		// C# / Kotlin / Swift は真値キーワードを真偽値リテラルノードで包む為，名前無子トークンの型名が `true` なら真
		if(CondView == "boolean_literal") {
			// 括弧内の節点
			const TSNode Inner = ts_node_child(Cond, 0);
			// 中身の無い真偽値は常に真でない事の返戻
			if(ts_node_is_null(Inner)) return false;
			// 内側トークンの真値キーワード該当有無の返戻
			return std::string_view(ts_node_type(Inner)) == "true";
		}
		if(NodeKind::CondParenWrap.Contains(CondView)) {
			Cond = ts_node_named_child(Cond, 0);
			continue;
		}
		// 真値以外の条件は無限ループと判定不可の返戻
		return false;
	}
	// 条件が空の為，無限ループ判定不可の返戻
	return false;
}

/**
 * 切替文の全分岐終端判定関数
 * @param Node 切替文
 * @param Source ソース文字列
 * @param Language 対象言語
 * @param Gotos 同じ関数内の goto 飛先
 * @param TransferScope 外側の切替文の判定範囲（単独判定時は空）
 * @return 未一致・素通り・外部脱出の経路が無ければ true
 */
bool LintPass::DoesSwitchTerminate(
	const TSNode Node,
	const std::string &Source,
	const Lang Language,
	const GotoInfo &Gotos,
	const TSNode TransferScope
) {
	// 切替本体の抽出
	TSNode Body = TSSource::FieldChild(Node, "body");
	if(ts_node_is_null(Body)) {
		if(Language == Lang::Kotlin || Language == Lang::Swift) Body = Node;
		else {
			ForEachNamedChild(
				Node,
				// 切替本体節点の採用関数
				[&](const TSNode Child) -> void {
					if(NodeKind::SwitchBody.Contains(Child)) Body = Child;
				}
			);
		}
	}
	// 本体の無い切替文は終端しない事の返戻
	if(ts_node_is_null(Body)) return false;
	// 移動先を探索する範囲
	const TSNode Scope = ts_node_is_null(TransferScope) ? Node : TransferScope;
	// 本体から抜け出る切替文は終端しない事の返戻
	if(HasLoopExit(Body, Node, Source, Language, Gotos, Scope)) return false;
	// 分岐節点の列
	std::vector<TSNode> Cases;
	bool IsInside = !ts_node_eq(Body, Node), IsUnknown = false;
	uint32_t Depth = 0;
	HeldCursor Cursor(Body);
	// 分岐構造の収集
	for(bool IsDone = false; !IsDone;) {
		const TSNode Child = ts_tree_cursor_current_node(&Cursor);
		const std::string_view Type(ts_node_type(Child));
		const bool IsDirect = Depth == 1, IsRoot = !Depth;
		if(IsDirect && Type == "{") IsInside = true;
		if(!IsRoot && IsInside && ts_node_is_named(Child) && !NodeKind::Comment.Contains(Type)) {
			if(const bool IsCase = NodeKind::CaseClause.Contains(Type); IsCase && IsDirect) Cases.push_back(Child);
			else if(IsCase || IsDirect) IsUnknown = true;
		}
		if(IsUnknown) break;
		// C/C++ の case は文の内側にも置けるが，ネスト switch・関数には非所属
		if(
			(IsRoot || Language.IsCFamily() && Type != "switch_statement" && !IsFlowBoundary(Type, Language)) &&
			ts_tree_cursor_goto_first_child(&Cursor)
		) {
			// 現在のネスト深さの進行
			++Depth;
			continue;
		}
		while(!ts_tree_cursor_goto_next_sibling(&Cursor)) {
			if(!ts_tree_cursor_goto_parent(&Cursor)) {
				IsDone = true;
				break;
			}
			// 現在のネスト深さの後退
			--Depth;
		}
	}
	// ネストした case 又は前処理分岐を含む場合の全節終端認定の回避
	if(IsUnknown || Cases.empty()) return false;
	// 既定分岐の保持有無
	bool HasDefault = false, DoesNextTerminate = false;
	for(std::vector<TSNode>::const_reverse_iterator Iter = Cases.rbegin(); Iter != Cases.rend(); ++Iter) {
		// 分岐本体の保持有無
		bool HasBody = false, IsClosed = false, IsInBody = false, IsArrow = false;
		ForEachChild(
			*Iter,
			[&](const TSNode Child) -> void {
				// 分岐子の型判定
				const std::string_view Type(ts_node_type(Child));
				if(Type == "switch_label") {
					// 既定ラベルの検出
					ForEachChild(
						Child,
						[&](const TSNode LabelPart) -> void {
							if(std::string_view(ts_node_type(LabelPart)) == "default") HasDefault = true;
						}
					);
					// 連続するラベルと本文の区別
					return;
				}
				// 分岐本文の閉鎖状態更新
				if(!IsInBody) {
					if(NodeKind::DefaultLabel.Contains(Type)) HasDefault = true;
					if(NodeKind::CaseBodyOpener.Contains(Type) || Language == Lang::PHP && Type == ";") {
						IsInBody = true;
						IsArrow = Type == "->";
					}
				} else if(Language == Lang::Swift && Type == "fallthrough") {
					HasBody = true;
					IsClosed = DoesNextTerminate;
				} else if(ts_node_is_named(Child) && !NodeKind::Comment.Contains(Type)) {
					HasBody = true;
					if(HasGotoEntry(Child, Gotos)) IsClosed = false;
					if(!IsClosed) IsClosed = TerminatesAlways(Child, Source, Language, Gotos, Scope);
				}
			}
		);
		// 本文の無い節は終端しない事の返戻
		if(!IsInBody) return false;
		if(
			const bool IsFallthrough =
			Language.IsCFamily() || Language == Lang::PHP || Language == Lang::Java && !IsArrow || Language == Lang::CSharp && !HasBody;
			!IsClosed && !(IsFallthrough && DoesNextTerminate)
			// 閉じない節（素通りする先の節も閉じない物を含む）が有れば終端しない事の返戻
		) return false;
		// 前節から素通りする到達先としての終端扱い
		DoesNextTerminate = true;
	}
	// 全節が閉じた上で，未一致の素通り経路も default で閉じている事の返戻
	return HasDefault;
}

/**
 * 制御フロー上の必ず終端する文の判定関数
 * @param Node 判定対象ノード
 * @param Source ソース文字列
 * @param Language 対象言語
 * @param Gotos 同じ関数内の goto 飛先
 * @param TransferScope 内部転送を認める切替文の判定範囲
 * @return 終端する場合 true
 */
bool LintPass::TerminatesAlways(
	const TSNode Node,
	const std::string &Source,
	const Lang Language,
	const GotoInfo &Gotos,
	const TSNode TransferScope
) {
	// 文列は途中からの入口を考慮し，if は両枝
	if(ts_node_is_null(Node)) return false;
	// 深いネストでの別走脈に依る終端判定の継続
	const RecursionGuard Guard;
	if(Guard.IsOverflow) {
		// 当該経路の終端有無
		bool Terminates = false;
		Parallel::RunOnFreshStack(
			[&]() -> void {
				Terminates = TerminatesAlways(Node, Source, Language, Gotos, TransferScope);
			}
		);
		// 新しい走脈で判定した結果の返戻
		return Terminates;
	}
	const std::string_view TypeView(ts_node_type(Node));
	// return / throw は終端する事の返戻
	if(NodeKind::ReturnOrThrow.Contains(TypeView)) return true;
	if(TypeView == "goto_statement") {
		// 制御の移動先節点
		const std::unordered_map<const void *, TSNode>::const_iterator Target = Gotos.Targets.find(Node.id);
		// 全節を併せて判定する switch 内でのみ内部転送を認める事の返戻
		return Target != Gotos.Targets.end() && IsWithinScope(Target->second, TransferScope);
	}
	if(TypeView == "labeled_statement") {
		// 対象要素の個数
		const uint32_t Count = ts_node_named_child_count(Node);
		// 文を持たないラベルは終端しない事の返戻
		if(!Count) return false;
		// 走査中の子節点
		const TSNode Child = ts_node_named_child(Node, Count - 1);
		// ラベル末尾が常に転送し且つループを抜けないかの返戻
		return TerminatesAlways(Child, Source, Language, Gotos, TransferScope) &&
		!HasLoopExit(Child, Node, Source, Language, Gotos, TransferScope);
	}
	if(
		Language == Lang::Swift && TypeView == "simple_identifier" && NodeView(Source, Node) == "fallthrough" &&
		std::string_view(ts_node_type(ts_node_parent(Node))) == "statements" && !ts_node_is_null(TransferScope)
	) for(TSNode Parent = ts_node_parent(Node); !ts_node_is_null(Parent); Parent = ts_node_parent(Parent)) {
		if(std::string_view(ts_node_type(Parent)) != "switch_entry") continue;
		TSNode Next = ts_node_next_named_sibling(Parent);
		while(!ts_node_is_null(Next) && NodeKind::Comment.Contains(Next)) Next = ts_node_next_named_sibling(Next);
		// 次節が有る場合だけ内部転送とする事の返戻
		return !ts_node_is_null(Next) && std::string_view(ts_node_type(Next)) == "switch_entry" && IsWithinScope(Next, TransferScope);
	}
	// Kotlin / Swift は return・throw・break 等を同じ節点に包む為，先頭の制御語を区別
	if(NodeKind::KeywordJump.Contains(TypeView)) return NodeKind::ExitKeyword.Contains(FirstChildType(Node));
	if(NodeKind::LoopForOrWhile.Contains(TypeView)) {
		// 条件式の節点
		TSNode Cond = TSSource::FieldChild(Node, "condition"), Body = TSSource::FieldChild(Node, "body");
		// tree-sitter-kotlin と tree-sitter-swift はフィールド名経由で取得出来ない為，最初の名前付子を条件候補
		if(TypeView == "while_statement" && (ts_node_is_null(Cond) || ts_node_is_null(Body))) {
			TSNode First {}, Last {};
			ForEachNamedChild(
				Node,
				[&](const TSNode Child) -> void {
					if(ts_node_is_null(First)) First = Child;
					Last = Child;
				}
			);
			if(ts_node_is_null(Cond)) Cond = First;
			if(ts_node_is_null(Body) && !ts_node_eq(First, Last)) Body = Last;
		}
		bool IsAlwaysTrue = IsAlwaysTrueCondition(Cond, Source);
		// Swift の複数 condition 場に対する先頭だけでの恒真認定の回避
		if(TypeView == "while_statement" && IsAlwaysTrue) for(uint32_t Idx = 0, Count = ts_node_child_count(Node); Idx < Count; ++Idx) {
			// 子節点に対応するフィールド名
			const char *const Field = ts_node_field_name_for_child(Node, Idx);
			if(Field && std::string_view(Field) == "condition" && !IsAlwaysTrueCondition(ts_node_child(Node, Idx), Source)) {
				// 条件式の恒真該当状態の解除
				IsAlwaysTrue = false;
				break;
			}
		}
		if(
			(TypeView == "for_statement" && ts_node_is_null(Cond) || IsAlwaysTrue) && !ts_node_is_null(Body) &&
			!HasLoopExit(Body, Node, Source, Language, Gotos, TransferScope)
			// 脱出の無い無限ループとして終端の返戻
		) return true;
	}
	// 文列は順に判定し，終端後の宣言・文で終端性を保持
	if(NodeKind::SequentialStmtContainer.Contains(TypeView) || TypeView == "control_structure_body") {
		// 当該経路の終端済有無
		bool IsClosed = false;
		ForEachNamedChild(
			Node,
			[&](const TSNode Child) -> void {
				// 条件成立時の返戻
				if(NodeKind::Comment.Contains(Child)) return;
				// 文毎の終端状態更新
				if(HasGotoEntry(Child, Gotos)) IsClosed = false;
				if(!IsClosed) IsClosed = TerminatesAlways(Child, Source, Language, Gotos, TransferScope);
			}
		);
		// 文列を素通り出来ない場合に終端とする事の返戻
		return IsClosed;
	}
	// 全プリプロセッサ分岐と後続分岐の終端時に於ける終端認定
	if(NodeKind::PreprocBlock.Contains(TypeView) || TypeView == "preproc_elifdef") {
		// 不成立側の本体節点
		const TSNode Alternative = TSSource::FieldChild(Node, "alternative");
		bool IsClosed = false, IsGuarded = TypeView != "preproc_else";
		ForEachNamedChild(
			Node,
			[&](const TSNode Child) -> void {
				// 条件成立時の返戻
				if(NodeKind::Comment.Contains(Child) || ts_node_eq(Child, Alternative)) return;
				// 前処理分岐内の終端状態更新
				if(IsGuarded) IsGuarded = false;
				else {
					if(HasGotoEntry(Child, Gotos)) IsClosed = false;
					if(!IsClosed) IsClosed = TerminatesAlways(Child, Source, Language, Gotos, TransferScope);
				}
			}
		);
		// #else 以外は後続の枝が無いと全条件不成立で素通り出来る事の返戻
		return IsClosed && (TypeView == "preproc_else" || TerminatesAlways(Alternative, Source, Language, Gotos, TransferScope));
	}
	if(NodeKind::IfNode.Contains(TypeView)) {
		TSNode Then = TSSource::IfConsequence(Node), Else = TSSource::FieldChild(Node, "alternative");
		// フィールド名未対応の文法向代替経路：条件以外の名前付子を順に取得
		if(ts_node_is_null(Then)) {
			TSNode First {}, Second {};
			ForEachNamedChild(
				Node,
				[&](const TSNode Child) -> void {
					// 条件部を除く分岐本体の抽出
					if(
						// 子節点型の字面
						const std::string_view ChildView(ts_node_type(Child));
						!NodeKind::SequentialStmtContainer.Contains(ChildView) && ChildView != "control_structure_body" &&
						!(NodeKind::IfNode.Contains(ChildView) && !ts_node_is_null(First))
						// 条件成立時の返戻
					) return;
					if(ts_node_is_null(First)) First = Child;
					else if(ts_node_is_null(Second)) Second = Child;
				}
			);
			Then = First;
			Else = Second;
		}
		// else 不在時は素通り可能で非終端の返戻
		if(ts_node_is_null(Else)) return false;
		// else 節包みの中身の取出
		if(std::string_view(ts_node_type(Else)) == "else_clause") Else = ts_node_named_child(Else, 0);
		if(
			!TerminatesAlways(Then, Source, Language, Gotos, TransferScope) ||
			!TerminatesAlways(Else, Source, Language, Gotos, TransferScope)
			// 一方でも素通り出来る場合は非終端の返戻
		) return false;
		// Swift のラベルは if の前の兄弟なので，両枝の return を迂回する脱出も確認
		if(Language == Lang::Swift) {
			TSNode Previous = ts_node_prev_named_sibling(Node);
			while(!ts_node_is_null(Previous) && NodeKind::Comment.Contains(Previous)) Previous = ts_node_prev_named_sibling(Previous);
			if(!ts_node_is_null(Previous) && std::string_view(ts_node_type(Previous)) == "statement_label") {
				// 自身のラベルへ脱出しない場合のみ終端の返戻
				return !HasLoopExit(Node, Node, Source, Language, Gotos, TransferScope);
			}
		}
		// 両分岐が終端する場合の返戻
		return true;
	}
	if(NodeKind::SwitchStatementLike.Contains(TypeView) || Language == Lang::Java && TypeView == "switch_expression") {
		// 構文が異なる切替文も同じ全分岐判定へ渡す事の返戻
		return DoesSwitchTerminate(Node, Source, Language, Gotos, TransferScope);
	}
	// 上記何れにも非該当のノードの非終端の返戻
	return false;
}

/**
 * 関数本体終端と到達不能 return 検証関数
 * @param Node 対象ノード
 * @param Source ソース文字列
 * @param Language 対象言語
 * @param Out 警告格納先
 */
void LintPass::CheckMissingExplicitReturn(
	const TSNode Node,
	const std::string &Source,
	const Lang Language,
	std::vector<LintWarning> &Out
) {
	// C 系・Java・Kotlin・Swift・PHP の関数本体の明示終端検査
	if(!NodeKind::ReturnOrDocFunctionLike.Contains(Node)) return;
	// 本体と戻値型に基付く終端検査の準備
	TSNode Body {};
	bool HasReturnType = false, ReturnsVoid = false, IsNeverReturning = false;
	ForEachChild(
		Node,
		[&](const TSNode Child) -> void {
			// 子節点型の字面
			const std::string_view ChildView(ts_node_type(Child));
			if(NodeKind::FunctionBodyContainer.Contains(ChildView)) Body = Child;
			// 明示された返却値型の確認
			if(NodeKind::TypeExpression.Contains(ChildView)) {
				HasReturnType = true;
				const std::string_view TypeName = NodeView(Source, Child);
				// PHP の型名比較関数
				const auto PhpTypeIs = [Language, TypeName](const std::string_view Lower) -> bool {
					// 小文字へ揃えた一致の返戻
					return Language == Lang::PHP && std::equal(
						TypeName.begin(),
						TypeName.end(),
						Lower.begin(),
						Lower.end(),
						// ASCII 小文字との一致判定関数
						[](const char Char, const char LowerChar) -> bool {
							return (Char | 0X20) == LowerChar;
						}
					);
				};
				ReturnsVoid = PhpTypeIs("void") || TypeName == "void" || Language == Lang::Swift && TypeName == "Void" ||
				Language == Lang::Kotlin && TypeName == "Unit";
				// 戻らない関数（Swift の `Never`・Kotlin の `Nothing`・PHP の `never`）は末尾の `return` を記述不能
				IsNeverReturning =
				Language == Lang::Swift && TypeName == "Never" || Language == Lang::Kotlin && TypeName == "Nothing" || PhpTypeIs("never");
			}
		}
	);
	// 終端を確定出来ない本体と戻らない関数の除外
	if(ts_node_is_null(Body) || ts_node_has_error(Body) || IsNeverReturning) return;
	// Kotlin の式本体（`= 式`）は式の値を返し `return` を書けない為，波括弧の本体だけを見る（型を省いた波括弧の本体は `Unit`）
	if(Language == Lang::Kotlin) {
		// 条件成立時の返戻
		if(const TSNode First = ts_node_child(Body, 0); ts_node_is_null(First) || std::string_view(ts_node_type(First)) != "{") return;
		if(!HasReturnType) HasReturnType = ReturnsVoid = true;
	}
	// 条件成立時の返戻
	if(!HasReturnType) return;
	// Kotlin / Swift の statements 包みを外し，他言語と同じ文列として走査
	ForEachNamedChild(
		Body,
		[&](const TSNode Child) -> void {
			if(std::string_view(ts_node_type(Child)) == "statements") Body = Child;
		}
	);
	if(
		ReturnsVoid && (!Language.IsCFamily() || !DocSig::ReturnsPointerOrReference(Node)) && !HasChildOf(
			Body,
			// 実体子の存在判定関数
			[](const TSNode Child) -> bool {
				return ts_node_is_named(Child) && !NodeKind::Comment.Contains(Child);
			}
		)
		// 条件成立時の返戻
	) return;
	// 到達不能判定で考慮する転送先の収集
	const GotoInfo Gotos = CollectGotoTargets(Body, Source, Language);
	bool IsTerminated = false;
	ForEachNamedChild(
		Body,
		[&](const TSNode Child) -> void {
			// 別経路からの到達を考慮した終端状態の更新
			if(HasGotoEntry(Child, Gotos)) IsTerminated = false;
			if(IsTerminated && HasReturnInFlow(Child, Language)) Push(Child, "Unreachable `return` after terminating statement", Out);
			if(!IsTerminated && TerminatesAlways(Child, Source, Language, Gotos)) IsTerminated = true;
		}
	);
	// 通常経路が残る関数終端の報告
	if(!IsTerminated) Push(Node, "Function is missing trailing return", Out);
	// 終了
	return;
}

/**
 * 部分木の JSX 要素包含判定関数
 * @param Node 対象ノード
 * @return JSX 子孫を含めば true
 */
bool LintPass::HasJsxDescendant(const TSNode Node) {
	// JSX 要素子孫の有無の返戻
	return HasDescendantOf(
		Node,
		// JSX 要素との一致判定関数
		[](const TSNode Cur) -> bool {
			// jsx_element / jsx_self_closing_element の何れかに合致するかの返戻
			return NodeKind::JsxElement.Contains(Cur);
		}
	);
}

/**
 * 関数命名規則違反検出関数
 * @param Node 対象ノード
 * @param Source ソース文字列
 * @param Language 対象言語
 * @param Out 警告格納先
 */
void LintPass::CheckFunctionNamingConvention(
	const TSNode Node,
	const std::string &Source,
	const Lang Language,
	std::vector<LintWarning> &Out
) {
	// 命名規則を適用する関数名の抽出
	TSNode Identifier {};
	ForEachNamedChild(
		Node,
		[&](const TSNode Child) -> bool {
			// 関数名識別子の言語別表現：identifier
			if(const std::string_view ChildView(ts_node_type(Child)); NodeKind::FunctionNameIdentifier.Contains(ChildView)) {
				Identifier = Child;
			} else if(ChildView == "function_declarator") Identifier = FirstNamedChildOfType(Child, "identifier");
			// 関数名識別子の取得済なら走査打切，未取得なら次の兄弟への返戻
			return ts_node_is_null(Identifier);
		}
	);
	// 条件成立時の返戻
	if(ts_node_is_null(Identifier)) return;
	// 関数名の字面取得
	const std::string_view Name = NodeView(Source, Identifier);
	// 言語仕様又は外部インターフェースで名前が固定された関数の除外
	if(
		Name.empty() || (Language.IsCFamily() || Language == Lang::Java || Language == Lang::Kotlin) && Name == "main" ||
		Language == Lang::PHP && Name.starts_with("__")
		// 条件成立時の返戻
	) return;
	// 上書き関数と演算子関数の除外
	if(Language == Lang::Java || Language == Lang::Kotlin) if(
		HasDescendantOf(
			FirstNamedChildOfType(Node, "modifiers"),
			[&Source, Language](const TSNode Modifier) -> bool {
				// 修飾子の字面
				const std::string_view Word = NodeView(Source, Modifier);
				// 言語別の上書き又は演算子修飾子かの返戻
				return Language == Lang::Java ? Word == "Override" : Word == "override" || Word == "operator";
			}
		)
		// 条件成立時の返戻
	) return;
	// 大文字小文字規則の基準文字取得
	const char FirstChar = Name[0];
	// 言語別の関数名規則（`PascalCase`・`snake_case`・`camelCase`）
	switch(Language.Id) {
	case Lang::C:
	case Lang::Cpp:
	case Lang::CSharp:
	case Lang::Java:
	case Lang::Kotlin:
		if(FirstChar < 'A' || FirstChar > 'Z') Push(Identifier, "Function name is not PascalCase", Out);
		break;
	case Lang::Rust:
	case Lang::Ruby:
	case Lang::Python:
		if(
			std::any_of(
				Name.begin(),
				Name.end(),
				// 大文字の包含判定関数
				[](const char NameChar) -> bool {
					return NameChar >= 'A' && NameChar <= 'Z';
				}
			)
		) Push(Identifier, "Function name is not snake_case", Out);
		break;
	case Lang::Swift:
	case Lang::PHP:
	case Lang::JavaScript:
	case Lang::TypeScript:
		if((FirstChar < 'a' || FirstChar > 'z') && !HasJsxDescendant(Node)) Push(Identifier, "Function name is not camelCase", Out);
		break;
	default:
		break;
	}
	// 終了
	return;
}

/**
 * Python 関数戻値型ヒント不在検出関数
 * @param Node 対象ノード
 * @param Out 警告格納先
 */
void LintPass::CheckPythonReturnTypeHint(const TSNode Node, std::vector<LintWarning> &Out) {
	// 条件成立時の返戻
	if(std::string_view(ts_node_type(Node)) != "function_definition") return;
	// 戻値型節点の確認
	if(ts_node_is_null(TSSource::FieldChild(Node, "return_type"))) Push(Node, "Function is missing return type hint", Out);
	// 終了
	return;
}

/**
 * 関数ドキュメントコメント不在検出関数
 * @param Node 対象ノード
 * @param Source ソース文字列
 * @param Language 対象言語
 * @param Out 警告格納先
 */
void LintPass::CheckDocCommentPresence(
	const TSNode Node,
	const std::string &Source,
	const Lang Language,
	std::vector<LintWarning> &Out
) {
	// 自動付与しない Swift・Python の三行以上の関数だけ文書コメントを検査し，短い関数では子走査も省略
	if(
		!NodeKind::ReturnOrDocFunctionLike.Contains(Node) || ts_node_end_point(Node).row - ts_node_start_point(Node).row < 3 ||
		!HasChildOf(
			Node,
			[](const TSNode Child) -> bool {
				return NodeKind::FunctionBodyContainer.Contains(Child);
			}
		)
		// 条件成立時の返戻
	) return;
	// 文書化コメントの保持有無
	bool HasDoc = false;
	if(Language == Lang::Python) {
		// Python は前置コメントではなく関数本体先頭のドキュメント文字列をドキュメントコメントと認定
		if(const TSNode Body = TSSource::FieldChild(Node, "body"); !ts_node_is_null(Body)) {
			if(
				// 本体先頭の文節点
				const TSNode FirstStmt = ts_node_named_child(Body, 0);
				!ts_node_is_null(FirstStmt) && std::string_view(ts_node_type(FirstStmt)) == "string"
			) HasDoc = true;
		}
	} else if(const TSNode Prev = ts_node_prev_named_sibling(Node); !ts_node_is_null(Prev) && NodeKind::Comment.Contains(Prev)) {
		// Swift: ドキュメントコメントは関数定義直前の `///` 又は `/**` 形式（Swift に包みノードは無い為，直接前の兄弟を確認）
		const std::string_view CommentText = NodeView(Source, Prev);
		HasDoc = CommentText.starts_with("/**") || CommentText.starts_with("///");
	}
	if(!HasDoc) Push(Node, "Function is missing a documentation comment", Out);
	// 終了
	return;
}

/**
 * ドキュメントコメント説明欠落検出関数（自動生成対象言語）
 * @param Node 対象ノード
 * @param Source ソース文字列
 * @param Language 対象言語
 * @param Out 警告格納先
 */
void LintPass::CheckDocCommentCompleteness(
	const TSNode Node,
	const std::string &Source,
	const Lang Language,
	std::vector<LintWarning> &Out
) {
	// 自動付与の雛形は説明文が空の為，概要・@param・@return の説明記入を要求
	if(ts_node_has_error(Node)) return;
	// 関数直前のドキュメントコメントからの論理行取得（基準点は先導コメントが結合されるノードに整合）
	const TSNode Anchor = DocSig::DocAnchor(Node, Language);
	std::vector<std::string> Lines;
	if(DocSig::StyleOf(Language) == DocSig::Style::Block) {
		// 直前の節点
		const TSNode Prev = ts_node_prev_named_sibling(Anchor);
		// 条件成立時の返戻
		if(ts_node_is_null(Prev) || !NodeKind::Comment.Contains(Prev)) return;
		const std::string_view Text = NodeView(Source, Prev);
		// ドキュメントブロック以外の除外
		if(!DocSig::IsDocBlock(Text)) return;
		Lines = DocSig::BlockLogicalLines(Text);
	} else {
		// Ruby: 基準点直前の連続 `#` コメント（ソース順）のドキュメント扱い
		const TSNode Scope = ts_node_parent(Anchor);
		// 条件成立時の返戻
		if(ts_node_is_null(Scope)) return;
		// 原文中での `#` 開始行の遡及収集
		const size_t Head = ts_node_start_byte(Anchor);
		std::vector<size_t> Starts;
		for(size_t Pos = Head ? Source.find_last_of('\n', Head - 1) : std::string::npos; Pos != std::string::npos;) {
			// 直前の改行位置
			const size_t Break = Pos ? Source.find_last_of('\n', Pos - 1) : std::string::npos;
			const size_t Text = Source.find_first_not_of(" \t", Break == std::string::npos ? 0 : Break + 1);
			if(Text == std::string::npos || Text >= Pos || Source[Text] != '#') break;
			Starts.push_back(Text);
			Pos = Break;
		}
		// `#` で始まる行の並びの中では複数行の構文が始まり得ない為，先頭の行が実際にコメントならば残りも全てコメント
		while(!Starts.empty()) {
			const uint32_t First = static_cast<uint32_t>(Starts.back());
			const TSNode Node = ts_node_descendant_for_byte_range(Scope, First, First);
			if(!ts_node_is_null(Node) && NodeKind::Comment.Contains(Node) && ts_node_start_byte(Node) == First) break;
			// 前の行から続く文字列や埋込ドキュメントが食い込んで居る為，其の終わりより前の行を候補から除外
			const size_t End = std::max<size_t>(ts_node_is_null(Node) ? First : ts_node_end_byte(Node), First + 1);
			while(!Starts.empty() && Starts.back() < End) Starts.pop_back();
		}
		// 条件成立時の返戻
		if(Starts.empty()) return;
		// 文書化コメントの行列の格納領域予約
		Lines.reserve(Starts.size());
		// 対象範囲の字面
		const std::string_view View(Source);
		for(size_t Idx = Starts.size(); Idx; --Idx) {
			// 対象の字面
			const size_t Text = Starts[Idx - 1], Stop = Source.find('\n', Text);
			Lines.push_back(DocSig::StripHashLine(View.substr(Text, (Stop == std::string::npos ? Source.size() : Stop) - Text)));
		}
	}
	const std::vector<bool> Code = DocSig::CodeLineStarts(Lines);
	bool HasSummary = false;
	for(size_t Index = 0; Index < Lines.size(); ++Index) {
		// 走査中の行
		const std::string &Line = Lines[Index];
		// 概要行の条件（非空且つ `@` タグでない行）
		if(const size_t Head = Line.find_first_not_of(" \t"); Head != std::string::npos && Line[Head] != '@') HasSummary = true;
		// コード例のタグを説明検査から除外
		if(Code[Index]) continue;
		if(
			// 対象の識別名
			const std::string_view Name = DocSig::ParamTagName(Line, Language);
			!Name.empty() && !DocSig::HasTagLineDescription(Line, Language)
		) Push(Node, "@param " + std::string(Name) + " is missing a description", Out);
		else if(Name.empty() && DocSig::IsReturnTagLine(Line) && !DocSig::HasTagLineDescription(Line, Language)) {
			Push(Node, "@return is missing a description", Out);
		}
	}
	// 概要の無い文書コメントの報告
	if(!HasSummary) Push(Node, "Documentation comment is missing a summary", Out);
	// 終了
	return;
}

/**
 * クラスメンバ宣言順違反検出関数
 * @param Node 対象ノード
 * @param Language 対象言語
 * @param Out 警告格納先
 */
void LintPass::CheckMemberDeclarationOrder(const TSNode Node, const Lang Language, std::vector<LintWarning> &Out) {
	// 条件成立時の返戻
	if(!IsClassOrderLang(Language)) return;
	// C/C++ のフィールド宣言内の関数宣言子判定関数
	const auto IsFieldDeclarationMethod = [](const TSNode FieldNode) -> bool {
		bool IsFound = false;
		// 宣言子の包みの内側だけを探索
		WalkChildrenCursor(
			FieldNode,
			[&IsFound](const TSNode WalkChild) -> bool {
				// 発見済なら降りない事の返戻
				if(IsFound) return false;
				// 走査中の子節点型
				const std::string_view WalkChildView(ts_node_type(WalkChild));
				// 関数宣言子の検出
				if(WalkChildView == "function_declarator") {
					IsFound = true;
					// 関数宣言子を発見した事の返戻
					return false;
				}
				// 宣言子の包みへ降りる事の返戻
				return NodeKind::CFunctionOrFieldDeclarator.Contains(WalkChildView);
			}
		);
		// 関数宣言子を子孫に含むかの返戻
		return IsFound;
	};
	// 型・メンバ変数・メンバ関数の三段階への簡易順位限定
	const auto MemberRank = [&IsFieldDeclarationMethod](const std::string_view ChildType, const TSNode Child) -> int {
		// 型エイリアス／ネストの型宣言（順位 1）
		if(NodeKind::MemberTypeRank.Contains(ChildType)) return 1;
		// メンバ変数／プロパティ（順位 2）但し C/C++ の `field_declaration` は内部の宣言子で関数か変数か判別
		if(ChildType == "field_declaration") return IsFieldDeclarationMethod(Child) ? 3 : 2;
		// メンバ変数の順位の返戻
		if(NodeKind::MemberFieldRank.Contains(ChildType)) return 2;
		// メンバ関数の順位の返戻
		if(NodeKind::MemberMethodRank.Contains(ChildType)) return 3;
		// 何れのメンバ順位にも該当しない事の返戻
		return -1;
	};
	// 順位追跡状態の初期化
	int LastRank = -1;
	ForEachNamedChild(
		Node,
		[&](const TSNode Child) -> bool {
			// 子節点型の字面
			const std::string_view ChildView(ts_node_type(Child));
			// アクセス区切りでの順位リセット
			if(ChildView == "access_specifier") {
				LastRank = -1;
				// リセットのみで次の子へ走査継続の返戻
				return true;
			}
			// アクセス範囲の順位
			const int Rank = MemberRank(ChildView, Child);
			// 順位の無いメンバを読み飛ばす事の返戻
			if(Rank < 0) return true;
			if(Rank < LastRank) {
				Push(Child, "Class member declaration order violation; expected types → variables → methods", Out);
				// 警告は最初の１件のみの為，走査打切の返戻
				return false;
			}
			LastRank = Rank;
			// 次のメンバへ走査継続の返戻
			return true;
		}
	);
	// 終了
	return;
}

/**
 * constexpr 化候補検出関数
 * @param Node 対象ノード
 * @param Source ソース文字列
 * @param Out 警告格納先
 */
void LintPass::CheckConstexprCandidate(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out) {
	// `const` + リテラル右辺の組合せのみの抽出（関数呼出や式は実行時依存の可能性が有る為，対象外）
	bool HasConst = false, HasConstexpr = false, HasLiteral = false;
	ForEachChild(
		Node,
		[&](const TSNode Child) -> void {
			// 修飾子と初期値の分類
			if(const std::string_view ChildView(ts_node_type(Child)); ChildView == "type_qualifier") {
				if(const std::string_view View = NodeView(Source, Child); View == "const") HasConst = true;
				else if(View == "constexpr") HasConstexpr = true;
			} else if(!HasLiteral && ChildView == "init_declarator") {
				HasLiteral = HasChildOf(
					Child,
					// 定数初期値リテラルの判定関数
					[](const TSNode GrandChild) -> bool {
						return NodeKind::CppConstLiteral.Contains(GrandChild);
					}
				);
			}
		}
	);
	// 候補該当時の警告登録
	if(HasConst && !HasConstexpr && HasLiteral) Push(Node, "`const` with literal initializer; consider `constexpr`", Out);
	// 終了
	return;
}

/**
 * C/C++ const 候補検出関数
 * @param Node 対象宣言ノード
 * @param Source ソース文字列
 * @param MutatedNames 明示的な代入・更新を受ける名前
 * @param Out 警告格納先
 */
void LintPass::CheckCppConstCandidate(
	const TSNode Node,
	const std::string &Source,
	const std::unordered_set<std::string> &MutatedNames,
	std::vector<LintWarning> &Out
) {
	// const 修飾子の保持有無
	bool HasConst = false, HasBuiltinType = false;
	TSNode Init {};
	uint32_t InitCount = 0;
	ForEachNamedChild(
		Node,
		// 宣言子構成の収集関数
		[&](const TSNode Child) -> void {
			// 宣言子構成の分類
			if(const std::string_view Type(ts_node_type(Child)); Type == "type_qualifier") {
				if(const std::string_view Qualifier = NodeView(Source, Child); Qualifier == "const" || Qualifier == "constexpr") {
					HasConst = true;
				}
			} else if(NodeKind::CppBuiltinType.Contains(Type)) HasBuiltinType = true;
			else if(Type == "init_declarator") {
				Init = Child;
				// 初期化式の個数の進行
				++InitCount;
			}
		}
	);
	// 条件成立時の返戻
	if(HasConst || !HasBuiltinType || InitCount != 1) return;
	// 変数名を持つ宣言子
	const TSNode Declarator = TSSource::FieldChild(Init, "declarator");
	// 条件成立時の返戻
	if(ts_node_is_null(Declarator) || std::string_view(ts_node_type(Declarator)) != "identifier") return;
	const std::string Name(NodeView(Source, Declarator));
	// 条件成立時の返戻
	if(Name.empty() || MutatedNames.contains(Name)) return;
	// 局所の宣言だけを対象とする（ts_node_parent は根から降り直す為，安価な判定の後に配置）
	if(
		// 対象の親節点
		const TSNode Parent = ts_node_parent(Node);
		ts_node_is_null(Parent) || std::string_view(ts_node_type(Parent)) != "compound_statement"
		// 条件成立時の返戻
	) return;
	// const 候補変数の警告登録
	Push(Node, "Consider `const` if mutation, type, and initialization behavior are unchanged", Out);
	// 終了
	return;
}

/**
 * リテラル文字列の浮動小数判定関数
 * @param Text リテラルの文字列
 * @return 浮動小数なら true
 */
bool LintPass::IsFloatLiteral(const std::string_view Text) {
	// 大文字化されない Rust の十六進接頭辞に対する x・X 両方の検査
	bool HasHexPrefix = false, HasHexExponent = false, HasDecExponent = false;
	// 構成文字の走査
	for(const char Char : Text) switch(Char) {
	case '.':
		// 小数点包含時は浮動小数の返戻（整数リテラルは小数点を持たない・全言語共通）
		return true;
	case 'x':
	case 'X':
		HasHexPrefix = true;
		break;
	case 'P':
		// 指数記号の記録
		HasHexExponent = true;
		break;
	case 'E':
		HasDecExponent = true;
		break;
	}
	// １６進接頭辞包含時は指数 P の有無で判定し桁 E の１０進誤認を回避する，１０進は指数 E の有無で判定する事の返戻
	return HasHexPrefix ? HasHexExponent : HasDecExponent;
}

/**
 * C/C++ 浮動小数接尾辞候補検出関数
 * @param Node 対象宣言ノード
 * @param Source ソース文字列
 * @param Out 警告格納先
 */
void LintPass::CheckFloatSuffixCandidate(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out) {
	// float 型の宣言だけを対象への限定
	if(
		!HasChildOf(
			Node,
			[&Source](const TSNode Child) -> bool {
				// float 型の指定子かの返戻
				return ts_node_is_named(Child) && std::string_view(ts_node_type(Child)) == "primitive_type" &&
				NodeView(Source, Child) == "float";
			}
		)
		// 条件成立時の返戻
	) return;
	// 宣言子毎の初期値検査
	ForEachNamedChild(
		Node,
		[&](const TSNode Child) -> void {
			// 条件成立時の返戻
			if(std::string_view(ts_node_type(Child)) != "init_declarator") return;
			if(
				// 変数名を持つ宣言子
				const TSNode Declarator = TSSource::FieldChild(Child, "declarator");
				ts_node_is_null(Declarator) || std::string_view(ts_node_type(Declarator)) != "identifier"
				// 条件成立時の返戻
			) return;
			TSNode Value = TSSource::FieldChild(Child, "value");
			// 初期値を包む単項式の除去
			while(!ts_node_is_null(Value)) {
				if(
					// 節点の型名
					const std::string_view Type(ts_node_type(Value));
					Type == "parenthesized_expression" || Type == "unary_expression" && !FindUnnamedToken(Source, Value, "+", "-").empty()
				) Value = ts_node_named_child(Value, 0);
				else break;
			}
			// 条件成立時の返戻
			if(ts_node_is_null(Value) || std::string_view(ts_node_type(Value)) != "number_literal") return;
			// 初期値のリテラル節点
			const std::string_view Literal = NodeView(Source, Value);
			// 条件成立時の返戻
			if(!IsFloatLiteral(Literal) || Literal.empty()) return;
			// 条件成立時の返戻
			if(const char Last = Literal.back(); Last == 'F' || Last == 'f' || Last == 'L' || Last == 'l') return;
			Push(Value, "Consider an `F` suffix only if rounding and exceptions are unchanged", Out);
		}
	);
	// 終了
	return;
}

/**
 * #define 定数検出関数
 * @param Node 対象ノード
 * @param Out 警告格納先
 */
void LintPass::CheckMacroConstant(const TSNode Node, std::vector<LintWarning> &Out) {
	// 値無 `#define` の検査対象からの除外（インクルードガード又は機能フラグ）
	if(
		HasChildOf(
			Node,
			[](const TSNode Child) -> bool {
				return std::string_view(ts_node_type(Child)) == "preproc_arg";
			}
		)
	) Push(Node, "Prefer `constexpr` or `const` when this macro is not required", Out);
	// 終了
	return;
}

/**
 * C/C++ ヘッダガード候補検出関数
 * @param Node 対象前処理ノード
 * @param Source ソース文字列
 * @param Out 警告格納先
 */
void LintPass::CheckHeaderGuardCandidate(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out) {
	// ヘッダガード開始指令の確認
	if(const TSNode Directive = ts_node_child(Node, 0); ts_node_is_null(Directive) || NodeView(Source, Directive) != "#ifndef") {
		// 終了
		return;
	}
	TSNode GuardName {}, Define {};
	ForEachNamedChild(
		Node,
		// ガード名と定義の抽出関数
		[&](const TSNode Child) -> bool {
			// コメントを読み飛ばす事の返戻
			if(NodeKind::Comment.Contains(Child)) return true;
			// ガード識別子の抽出
			if(ts_node_is_null(GuardName)) {
				// 識別子でない条件はヘッダガードでない事の返戻
				if(std::string_view(ts_node_type(Child)) != "identifier") return false;
				GuardName = Child;
				// 次の実体子へ走査継続の返戻
				return true;
			}
			// 直後のマクロ定義の採用
			if(std::string_view(ts_node_type(Child)) == "preproc_def") Define = Child;
			// ガード名の次の実体子だけを確認する事の返戻
			return false;
		}
	);
	// 条件成立時の返戻
	if(ts_node_is_null(GuardName) || ts_node_is_null(Define)) return;
	if(
		// マクロ定義の識別名
		const TSNode DefineName = ts_node_named_child(Define, 0);
		ts_node_is_null(DefineName) || NodeView(Source, DefineName) != NodeView(Source, GuardName)
		// 条件成立時の返戻
	) return;
	// 最上位の条件だけを対象とする（ts_node_parent は根から降り直す為，安価な判定の後に配置）
	if(
		// 対象の親節点
		const TSNode Parent = ts_node_parent(Node);
		ts_node_is_null(Parent) || std::string_view(ts_node_type(Parent)) != "translation_unit"
		// 条件成立時の返戻
	) return;
	// ヘッダガード置換候補の警告登録
	Push(Node, "Consider `#pragma once` only if supported and semantics are preserved", Out);
	// 終了
	return;
}

/**
 * using namespace 検出関数
 * @param Node 対象ノード
 * @param Source ソース文字列
 * @param Out 警告格納先
 */
void LintPass::CheckUsingNamespace(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out) {
	// `using namespace X` と `using X::Y`（エイリアス）は同じノード型の為，テキストで判別
	if(NodeView(Source, Node).find("using namespace") != std::string_view::npos) {
		Push(Node, "`using namespace` is discouraged; qualify names explicitly", Out);
	}
	// 終了
	return;
}

/**
 * コメントを除くコードの行数の取得関数
 * @param Source ソース文字列
 * @param Comments コメントの範囲（開始の昇順）
 * @param Start 範囲の開始
 * @param End 範囲の終端
 * @param Limit 数える上限（達した時点で打ち切る）
 * @return コメントと空白以外の字句を含む行の数（上限で打ち切る）
 */
size_t LintPass::CountCodeLines(
	const std::string &Source,
	const std::vector<std::pair<uint32_t, uint32_t>> &Comments,
	const uint32_t Start,
	const uint32_t End,
	const size_t Limit
) {
	// 行数計数状態の初期化
	size_t Lines = 0;
	bool HasCodeOnLine = false;
	std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Next =
	std::lower_bound(Comments.begin(), Comments.end(), std::pair<uint32_t, uint32_t>(Start, 0));
	// 対象範囲の字句走査
	for(uint32_t Pos = Start; Pos < End && Lines < Limit;) {
		if(Next != Comments.end() && Next->first == Pos) {
			if(std::memchr(Source.data() + Pos, '\n', Next->second - Pos)) HasCodeOnLine = false;
			Pos = std::max(Next->second, Pos + 1);
			// コメント範囲の反復子の進行
			++Next;
			continue;
		}
		if(const char Char = Source[Pos++]; Char == '\n') HasCodeOnLine = false;
		else if(!HasCodeOnLine && Char != ' ' && Char != '\t' && Char != '\r') {
			HasCodeOnLine = true;
			// 文書化コメントの行列の進行
			++Lines;
		}
	}
	// コードの行数の返戻
	return Lines;
}

/**
 * return コメント不在検出関数
 * @param Node 対象ノード
 * @param Source ソース文字列
 * @param Comments コメントの範囲（開始の昇順）
 * @param Language 対象言語
 * @param Out 警告格納先
 */
void LintPass::CheckReturnCommentPresence(
	const TSNode Node,
	const std::string &Source,
	const std::vector<std::pair<uint32_t, uint32_t>> &Comments,
	const Lang Language,
	std::vector<LintWarning> &Out
) {
	// 値無 return と CSS / SCSS の @return は対象外
	if(Language == Lang::CSS || !ts_node_named_child_count(Node)) return;
	// 構文木に依る return と同一行のコメント確認
	const uint32_t End = ts_node_end_byte(Node), LineEnd = static_cast<uint32_t>(std::min(Source.find('\n', End), Source.size()));
	const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator After =
	std::lower_bound(Comments.begin(), Comments.end(), std::pair<uint32_t, uint32_t>(End, 0));
	// 同じ行にコメントが有る場合の終了
	if(After != Comments.end() && After->first < LineEnd) return;
	// return の行の直前の行で終わるコメント（行の途中の return は其の行で始まる文の直前のコメント）が有れば警告不要
	if(const uint32_t ReturnLine = TextEdit::LineStartOf(Source, ts_node_start_byte(Node)); ReturnLine) {
		// コメントは重ならず開始の昇順が終端の昇順でもある為，直前の行より後で終わる最初のコメントを確認
		const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Above = std::lower_bound(
			Comments.begin(),
			Comments.end(),
			TextEdit::LineStartOf(Source, ReturnLine - 1) + 1,
			[](const std::pair<uint32_t, uint32_t> &Comment, const uint32_t Pos) -> bool {
				// コメントの終端が位置より前かの返戻
				return Comment.second < Pos;
			}
		);
		// 直前の行にコメントが有る場合の終了
		if(Above != Comments.end() && Above->second <= ReturnLine) return;
	}
	// switch case 内の return と１〜２行の単純な関数／ラムダ内 return のみ例外
	for(TSNode Ancestor = ts_node_parent(Node); !ts_node_is_null(Ancestor); Ancestor = ts_node_parent(Ancestor)) {
		const std::string_view AncestorView(ts_node_type(Ancestor));
		// 条件成立時の返戻
		if(NodeKind::SwitchCaseAncestor.Contains(AncestorView)) return;
		if(NodeKind::ReturnOrDocFunctionLike.Contains(AncestorView)) {
			// 省略可の１〜２行はコメントを除くコードの行で数え，コメントの追加に依って要否を不変
			if(CountCodeLines(Source, Comments, ts_node_start_byte(Ancestor), ts_node_end_byte(Ancestor), 4) < 4) return;
			break;
		}
	}
	// return 前コメント欠落の警告登録
	Push(Node, "Missing comment before `return`", Out);
	// 終了
	return;
}

/**
 * 真偽値として評価される位置の印付関数
 * 制御構文の条件から括弧・論理積・論理和・論理否定だけを降り，其の下に在る節点へ印を付ける
 * 計算量：条件の節点数 C に対し O(C)（各節点を一度だけ訪れる）
 * @param Host 条件を持つ制御構文の節点
 * @param Source ソース文字列
 * @param Index 印を付ける索引（節点の識別子 → JSX の式の中か）
 */
void LintPass::MarkCondContext(const TSNode Host, const std::string &Source, CondContextIndex &Index) {
	// 条件根の収集準備
	const std::string_view HostType(ts_node_type(Host));
	const bool IsInJsx = HostType == "jsx_expression";
	std::vector<TSNode> Pending;
	const bool HasConditionField = NodeKind::ConditionFieldHost.Contains(HostType);
	const TSNode Condition = HasConditionField ? TSSource::FieldChild(Host, "condition") : TSNode{};
	// 条件根の登録
	if(HasConditionField && !ts_node_is_null(Condition)) Pending.push_back(Condition);
	else if(!HasConditionField) {
		ForEachNamedChild(
			Host,
			// 条件候補の登録関数
			[&Pending](const TSNode Child) -> void {
				Pending.push_back(Child);
			}
		);
	}
	while(!Pending.empty()) {
		// 現在の節点
		const TSNode Cur = Pending.back();
		// 未処理の節点列からの末尾要素取出
		Pending.pop_back();
		Index.emplace(Cur.id, IsInJsx);
		// 括弧・論理積・論理和・論理否定の下も同じ真偽値の文脈に存在
		if(
			const std::string_view CurType(ts_node_type(Cur));
			!NodeKind::CondParenWrap.Contains(CurType) &&
			!(CurType == "binary_expression" && !FindUnnamedToken(Source, Cur, "&&", "||").empty()) &&
			!(CurType == "unary_expression" && HasUnnamedTokenChild(Source, Cur, "!"))
		) continue;
		ForEachNamedChild(
			Cur,
			// 次の条件候補の登録関数
			[&Pending](const TSNode Child) -> void {
				Pending.push_back(Child);
			}
		);
	}
	// 終了
	return;
}

/**
 * 制御構文条件の整数ゼロ比較検出関数 (C/C++/JS/TS)
 * @param Node 対象ノード
 * @param Source ソース文字列
 * @param Conditions 真偽値として評価される位置の索引（制御構文の側から先に印を付けた物）
 * @param Language 対象言語
 * @param Out 警告格納先
 */
void LintPass::CheckZeroComparisonCondition(
	const TSNode Node,
	const std::string &Source,
	const CondContextIndex &Conditions,
	const Lang Language,
	std::vector<LintWarning> &Out
) {
	// 比較被演算子の取得
	const TSNode Lhs = ts_node_named_child(Node, 0), Rhs = ts_node_named_child(Node, 1);
	// 条件成立時の返戻
	if(ts_node_is_null(Lhs) || ts_node_is_null(Rhs)) return;
	const std::string_view NumberType = Language.IsJsTs() ? "number" : "number_literal";
	// ゼロ値リテラルの判定関数
	const auto IsZeroLiteral = [&Source, NumberType](const TSNode Target) -> bool {
		// 数値リテラルでない事の返戻
		if(std::string_view(ts_node_type(Target)) != NumberType) return false;
		// リテラル字面の取得
		const std::string_view View = NodeView(Source, Target);
		// ０で始まらない事の返戻
		if(View.empty() || View[0] != '0') return false;
		// 整数接尾辞の検証
		for(size_t Idx = 1; Idx < View.size(); ++Idx) {
			// 整数の接尾辞以外が有る事の返戻
			if(const char Suffix = View[Idx]; Suffix != 'L' && Suffix != 'U' && Suffix != 'l' && Suffix != 'u') return false;
		}
		// 0 と整数接尾辞だけで構成される値の返戻
		return true;
	};
	// 条件成立時の返戻
	if(!IsZeroLiteral(Lhs) && !IsZeroLiteral(Rhs)) return;
	// 比較演算子の抽出
	std::string_view Operator = FindUnnamedToken(Source, Node, "==", "!=");
	if(Operator.empty() && Language.IsJsTs()) Operator = FindUnnamedToken(Source, Node, "===", "!==");
	// 条件成立時の返戻
	if(Operator.empty()) return;
	// 真偽値として評価される位置に在る比較だけを対象にする（印は制御構文の側から先に付けて存在）
	const CondContextIndex::const_iterator Hit = Conditions.find(Node.id);
	// 条件成立時の返戻
	if(Hit == Conditions.end()) return;
	Push(
		Node,
		Operator == "==" || Operator == "===" ? "If equivalent, use `!x` for zero in control condition" : Hit->second ?
		"If equivalent, use `!!x` for nonzero in JSX condition" :
		"If equivalent, use `x` for nonzero in control condition",
		Out
	);
	// 終了
	return;
}

/**
 * 数値比較の厳密等価検出関数 (JS/TS)
 * @param Node 対象ノード
 * @param Source ソース文字列
 * @param Out 警告格納先
 */
void LintPass::CheckNumberStrictEquality(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out) {
	// 比較被演算子の取得
	const TSNode Lhs = ts_node_named_child(Node, 0), Rhs = ts_node_named_child(Node, 1);
	// 条件成立時の返戻
	if(ts_node_is_null(Lhs) || ts_node_is_null(Rhs)) return;
	// 厳密等価演算子の抽出
	const std::string_view Operator = FindUnnamedToken(Source, Node, "===", "!==");
	// 条件成立時の返戻
	if(Operator.empty()) return;
	// 数値リテラルの判定関数
	const auto IsNumberLiteral = [&Source](const TSNode Target) -> bool {
		// 節点の型名
		const std::string_view Type(ts_node_type(Target));
		// 数値リテラルの返戻
		if(Type == "number") return true;
		// 負の数値リテラルでない事の返戻
		if(Type != "unary_expression" || !HasUnnamedTokenChild(Source, Target, "-")) return false;
		// 負号の被演算子取得
		const TSNode Operand = ts_node_named_child(Target, 0);
		// 負号の被演算子が数値リテラルかの返戻
		return !ts_node_is_null(Operand) && std::string_view(ts_node_type(Operand)) == "number";
	};
	// 厳密比較を維持する値の判定関数
	const auto IsExcluded = [&Source](const TSNode Target) -> bool {
		// 除外候補の字面取得
		const std::string_view View = NodeView(Source, Target);
		// null 系又は typeof 式かの返戻
		return View == "null" || View == "undefined" ||
		std::string_view(ts_node_type(Target)) == "unary_expression" && HasUnnamedTokenChild(Source, Target, "typeof");
	};
	// 条件成立時の返戻
	if(IsExcluded(Lhs) || IsExcluded(Rhs) || !IsNumberLiteral(Lhs) && !IsNumberLiteral(Rhs)) return;
	Push(
		Node,
		Operator == "===" ?
		"Use `==` if both operands have the same numeric type" :
		"Use `!=` if both operands have the same numeric type",
		Out
	);
	// 終了
	return;
}

/**
 * JS/TS のヌリッシュ比較縮約候補検出関数
 * @param Node 対象二項式
 * @param Source ソース文字列
 * @param Out 警告格納先
 */
void LintPass::CheckNullishComparisonCandidate(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out) {
	// 外側の論理演算子の抽出
	const std::string_view OuterOperator = FindUnnamedToken(Source, Node, "||", "&&");
	// 条件成立時の返戻
	if(OuterOperator.empty()) return;
	// 比較式二項の取得
	const TSNode Lhs = ts_node_named_child(Node, 0), Rhs = ts_node_named_child(Node, 1);
	// 条件成立時の返戻
	if(ts_node_is_null(Lhs) || ts_node_is_null(Rhs)) return;
	struct Comparison {
		std::string_view Subject; // 条件式の比較対象
		std::string_view Nullish; // ヌル値の被演算子
		std::string_view Operator; // 演算子節点
	};
	// 比較式の解析関数
	const auto Parse = [&Source](const TSNode Target, Comparison &Result) -> bool {
		// 二項式でない事の返戻
		if(std::string_view(ts_node_type(Target)) != "binary_expression") return false;
		// 厳密等価演算子の抽出
		Result.Operator = FindUnnamedToken(Source, Target, "===", "!==");
		// 厳密等価でない事の返戻
		if(Result.Operator.empty()) return false;
		// 両被演算子の抽出
		const TSNode Left = ts_node_named_child(Target, 0), Right = ts_node_named_child(Target, 1);
		// 被演算子の欠けた式の返戻
		if(ts_node_is_null(Left) || ts_node_is_null(Right)) return false;
		// ヌル値側と比較主体の分離
		if(
			const std::string_view LeftText = NodeView(Source, Left), RightText = NodeView(Source, Right);
			LeftText == "null" || LeftText == "undefined"
		) {
			// 左辺がヌル値の場合の解析結果格納
			Result.Nullish = LeftText;
			Result.Subject = RightText;
		} else if(RightText == "null" || RightText == "undefined") {
			Result.Nullish = RightText;
			Result.Subject = LeftText;
			// null / undefined との比較でない事の返戻
		} else return false;
		// 比較主体を取得出来たかの返戻
		return !Result.Subject.empty();
	};
	// 左右の比較結果の取得
	Comparison Left {}, Right {};
	// 条件成立時の返戻
	if(!Parse(Lhs, Left) || !Parse(Rhs, Right) || Left.Subject != Right.Subject || Left.Nullish == Right.Nullish) return;
	// 縮約可能な演算子対の判定
	const bool IsEqualityPair = OuterOperator == "||" && Left.Operator == "===" && Right.Operator == "===";
	if(
		// 非同値比較の論理積か
		const bool IsInequalityPair = OuterOperator == "&&" && Left.Operator == "!==" && Right.Operator == "!==";
		!IsEqualityPair && !IsInequalityPair
		// 条件成立時の返戻
	) return;
	Push(
		Node,
		IsEqualityPair ?
		"Consider `== null` only if nullish semantics, reads, and bindings are preserved" :
		"Consider `!= null` only if nullish semantics, reads, and bindings are preserved",
		Out
	);
	// 終了
	return;
}

/**
 * C 形式キャスト検出関数 (C++)
 * @param Node 対象ノード
 * @param Out 警告格納先
 */
void LintPass::CheckCStyleCast(const TSNode Node, std::vector<LintWarning> &Out) {
	// C 形式キャストの検出
	Push(Node, "Use `static_cast` / `reinterpret_cast` / `const_cast` instead of C-style cast", Out);
	// 終了
	return;
}

/**
 * C++ の `NULL` 候補検出関数
 * @param Node 対象ノード
 * @param Source ソース文字列
 * @param Out 警告格納先
 */
void LintPass::CheckNullMacroCandidate(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out) {
	// `NULL` マクロの警告登録
	if(NodeView(Source, Node) == "NULL") Push(Node, "If semantics are preserved, prefer `nullptr` to `NULL`", Out);
	// 終了
	return;
}

/**
 * 波括弧言語の if 統合候補検出関数
 * @param Node 対象 if 文
 * @param Source ソース文字列
 * @param Out 警告格納先
 */
void LintPass::CheckUnsafeIfMergeCandidate(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out) {
	// 単一本体の抽出関数
	const auto SingleBody = [](TSNode Body) -> TSNode {
		// 波括弧本体の単一文への縮約
		if(!ts_node_is_null(Body) && NodeKind::FunctionBodyContainer.Contains(Body) && ts_node_named_child_count(Body) == 1) {
			Body = ts_node_named_child(Body, 0);
		}
		// 波括弧を除いた単一本体の返戻
		return Body;
	};
	// 統合可能な単純条件の判定関数
	const auto SimpleCondition = [&Source](const TSNode IfNode) -> bool {
		// 条件式の節点
		TSNode Condition = TSSource::FieldChild(IfNode, "condition");
		if(!ts_node_is_null(Condition) && NodeKind::CondParenWrap.Contains(Condition)) {
			// 括弧内の節点
			TSNode Inner {};
			ForEachNamedChild(
				Condition,
				[&Inner](const TSNode Child) -> void {
					if(std::string_view(ts_node_type(Child)) != "init_statement") Inner = Child;
				}
			);
			Condition = Inner;
		}
		// 条件の無い分岐を対象外とする事の返戻
		if(ts_node_is_null(Condition)) return false;
		// 評価順と回数を保持可能な否定条件の統合
		if(
			NodeKind::UnaryPre.Contains(Condition) && ts_node_named_child_count(Condition) == 1 &&
			NodeView(Source, ts_node_child(Condition, 0)) == "!"
		) Condition = ts_node_named_child(Condition, 0);
		// 型を構文から確定出来ない単純識別子条件かの返戻
		return NodeKind::PlainVariable.Contains(Condition);
	};
	const TSNode Consequence = SingleBody(TSSource::IfConsequence(Node)), Alternative = TSSource::FieldChild(Node, "alternative");
	// ネストした if は論理積への統合候補
	if(
		ts_node_is_null(Alternative) && !ts_node_is_null(Consequence) && NodeKind::IfNode.Contains(Consequence) &&
		ts_node_is_null(TSSource::FieldChild(Consequence, "alternative")) && SimpleCondition(Node) && SimpleCondition(Consequence)
	) {
		Push(Node, "If semantics are preserved, consider merging nested conditions with `&&`", Out);
		// 論理積への統合候補を報告して終了
		return;
	}
	// 同じ退出文を持つ直後の if は論理和への統合候補
	if(ts_node_is_null(Alternative) && !ts_node_is_null(Consequence) && NodeKind::FlowExit.Contains(Consequence)) {
		TSNode Next = ts_node_next_named_sibling(Node);
		while(!ts_node_is_null(Next) && NodeKind::Comment.Contains(Next)) Next = ts_node_next_named_sibling(Next);
		if(!ts_node_is_null(Next) && NodeKind::IfNode.Contains(Next) && SimpleCondition(Node) && SimpleCondition(Next)) {
			if(
				// 後続分岐の本体節点
				const TSNode NextBody = SingleBody(TSSource::IfConsequence(Next));
				ts_node_is_null(TSSource::FieldChild(Next, "alternative")) && !ts_node_is_null(NextBody) &&
				NodeView(Source, Consequence) == NodeView(Source, NextBody)
			) {
				Push(Node, "If semantics are preserved, consider merging consecutive conditions with `||`", Out);
				// 論理和への統合候補を報告して終了
				return;
			}
		}
	}
	// 同一本体の else-if は論理和への統合候補
	TSNode ElseIf {};
	if(!ts_node_is_null(Alternative)) {
		// 不成立側節点型の字面
		const std::string_view AlternativeView(ts_node_type(Alternative));
		if(NodeKind::ConditionalAlternative.Contains(AlternativeView)) ElseIf = Alternative;
		else if(AlternativeView == "else_clause") {
			ForEachNamedChild(
				Alternative,
				// else-if 節点の採用関数
				[&ElseIf](const TSNode Child) -> void {
					if(ts_node_is_null(ElseIf) && NodeKind::IfNode.Contains(Child)) ElseIf = Child;
				}
			);
		}
	}
	if(!ts_node_is_null(ElseIf) && SimpleCondition(Node) && SimpleCondition(ElseIf)) {
		if(
			const TSNode ElseBody = SingleBody(TSSource::IfConsequence(ElseIf));
			!ts_node_is_null(Consequence) && !ts_node_is_null(ElseBody) && NodeView(Source, Consequence) == NodeView(Source, ElseBody)
		) Push(Node, "If semantics are preserved, consider merging alternative conditions with `||`", Out);
	}
	// 終了
	return;
}

/**
 * 規約番号参照の検出関数（節番号は規約の改訂でずれ，参照が陳腐化する為，意図其の物を書く）
 * @param Node 対象のコメントノード
 * @param Source ソースコード
 * @param Out 警告の格納先
 */
void LintPass::CheckStandardsReference(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out) {
	// 空の字面と為る破損節点の探索不一致
	const std::string_view Text = NodeView(Source, Node);
	bool IsFound = Text.find("CODING_STANDARDS") != std::string_view::npos;
	static constexpr std::string_view SectionMark = "§";
	for(size_t Pos = Text.find(SectionMark); !IsFound && Pos != std::string_view::npos; Pos = Text.find(SectionMark, Pos + 1)) {
		if(const size_t Scan = Pos + SectionMark.size(); Scan < Text.size() && std::isdigit(static_cast<unsigned char>(Text[Scan]))) {
			IsFound = true;
		}
	}
	// 「規約」に続く `N.M` 形の節番号を探す語と番号の間に助詞が挟まる形も同じ参照の為，数字が現れる迄の仮名を読飛し
	static constexpr size_t KeywordLen = 6;
	for(size_t Pos = Text.find("規約"); !IsFound && Pos != std::string_view::npos; Pos = Text.find("規約", Pos + KeywordLen)) {
		size_t Scan = Pos + KeywordLen;
		// 「規約」と番号の間に置かれ得るのは空白と１語の助詞だけで，其れ以外が挟まれば別の文脈と確認
		while(Scan < Text.size() && (Text[Scan] == ' ' || Text[Scan] == '\t')) ++Scan;
		// 番号を括弧で包む形も同じ参照の為，助詞の後の開き括弧（全角・半角）も読飛し
		for(const std::string_view Joint : { std::string_view("の"), std::string_view("は"), std::string_view("で") }) {
			if(Text.compare(Scan, Joint.size(), Joint)) continue;
			// 規約番号の走査位置の進行
			Scan += Joint.size();
			while(Scan < Text.size() && (Text[Scan] == ' ' || Text[Scan] == '\t')) ++Scan;
			break;
		}
		if(!Text.compare(Scan, 3, "（")) Scan += 3;
		else if(Scan < Text.size() && Text[Scan] == '(') ++Scan;
		// 節番号の数字開始位置
		const size_t DigitStart = Scan;
		while(Scan < Text.size() && std::isdigit(static_cast<unsigned char>(Text[Scan]))) ++Scan;
		if(Scan == DigitStart || Scan >= Text.size() || Text[Scan] != '.') continue;
		// 規約番号の走査位置の進行
		++Scan;
		if(Scan >= Text.size() || !std::isdigit(static_cast<unsigned char>(Text[Scan]))) continue;
		// 版番号と節番号の区別
		while(Scan < Text.size() && (std::isdigit(static_cast<unsigned char>(Text[Scan])) || Text[Scan] == '.')) ++Scan;
		while(Scan < Text.size() && (Text[Scan] == ' ' || Text[Scan] == '\t')) ++Scan;
		IsFound = !!Text.compare(Scan, 3, "版");
	}
	if(IsFound) Push(Node, "Comment refers to a coding standard section; describe the intent instead", Out);
	// 終了
	return;
}

/**
 * 真偽値反転代入パターン検出関数 (C/C++/C#/Java/Rust)
 * @param Node 対象ノード
 * @param Source ソース文字列
 * @param Out 警告格納先
 */
void LintPass::CheckBoolFlipPattern(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out) {
	// 型情報を持たない真偽値反転に対する条件付警告
	const TSNode Lhs = ts_node_named_child(Node, 0), Rhs = ts_node_named_child(Node, 1);
	// 両辺が揃い，左辺が単純識別子の反転代入だけを取扱
	if(
		ts_node_is_null(Lhs) || ts_node_is_null(Rhs) || std::string_view(ts_node_type(Lhs)) != "identifier" ||
		!NodeKind::UnaryPrefixExpression.Contains(Rhs) || !HasUnnamedTokenChild(Source, Node, "=")
		// 条件成立時の返戻
	) return;
	TSNode Operand {};
	bool HasBang = false;
	ForEachChild(
		Rhs,
		// 反転演算子と被演算子の抽出関数
		[&](const TSNode Child) -> void {
			if(ts_node_is_named(Child) && ts_node_is_null(Operand)) Operand = Child;
			else if(!ts_node_is_named(Child) && NodeView(Source, Child) == "!") HasBang = true;
		}
	);
	if(
		!HasBang || ts_node_is_null(Operand) || std::string_view(ts_node_type(Operand)) != "identifier" ||
		NodeView(Source, Lhs) != NodeView(Source, Operand)
		// 条件成立時の返戻
	) return;
	// 真偽値反転式の置換候補警告登録
	Push(Node, "If equivalent for a built-in Boolean, prefer `flag ^= true`", Out);
	// 終了
	return;
}

/**
 * 整数リテラル境界比較検出関数（全言語）
 * @param Node 対象ノード
 * @param Source ソース文字列
 * @param Out 警告格納先
 */
void LintPass::CheckCmpBoundary(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out) {
	// 比較右辺と演算子の抽出状態初期化
	TSNode Rhs {};
	std::string_view Operator;
	uint32_t NamedSeen = 0;
	ForEachChild(
		Node,
		// 比較右辺と演算子の抽出関数
		[&](const TSNode Child) -> void {
			// Swift の `infix_expression` は演算子を `custom_operator` 名前付ノードで持つ為，無名の字句と同じく演算子として確認
			if(ts_node_is_named(Child) && std::string_view(ts_node_type(Child)) != "custom_operator") {
				if(++NamedSeen == 2) Rhs = Child;
			} else if(const std::string_view Tok = NodeView(Source, Child); Tok == ">=" || Tok == "<=") Operator = Tok;
		}
	);
	// `Rhs` が整数リテラルか判定 C / C++ の `number_literal` と JS / TS の `number` は浮動小数も同型
	if(
		Operator.empty() || ts_node_is_null(Rhs) || !NodeKind::IntLiteralLike.Contains(Rhs) || IsFloatLiteral(NodeView(Source, Rhs))
		// 条件成立時の返戻
	) return;
	// 境界比較候補の警告登録
	Push(
		Node,
		Operator == ">=" ? "If equivalent, use `> N-1` instead of `>= N`" : "If equivalent, use `< N+1` instead of `<= N`",
		Out
	);
	// 終了
	return;
}

/**
 * 変数の文字列文脈推定判定関数
 * @param Source ソース文字列
 * @param AssignNode 代入式ノード
 * @param Name 判定対象の変数名
 * @param Index スコープ毎の宣言・代入の事象の索引（初めて訪れたスコープの物を此処で作り，同じ走査の後の判定で使い回す）
 * @return 文字列文脈と推定される場合 true
 */
bool LintPass::LooksLikeStringVariable(
	const std::string &Source,
	const TSNode AssignNode,
	const std::string_view Name,
	StringEventIndex &Index
) {
	// `let s = ""; s += 1;` 形式の文字列 `+=` 連結を警告抑制する為の発見的判定
	if(Name.empty()) return false;
	// 型ノードのテキストが `String` / `string` なら true（Java / C# / TS の文字列型）
	const auto IsStringTypeText = [](const std::string_view TypeText) -> bool {
		return TypeText == "String" || TypeText == "string";
	};
	// スコープ内の文字列事象索引の構築関数
	const auto BuildEvents = [&Source, &IsStringTypeText](const TSNode Scope) -> StringEventIndex::mapped_type {
		// 事象収集先の初期化
		std::unordered_map<std::string_view, std::vector<std::pair<uint32_t, bool>>> Events;
		std::unordered_map<std::string_view, uint32_t> FirstPos;
		// 走査節点の事象収集関数
		const auto Visit = [&](const TSNode VisitNode) -> bool {
			// 対象節点型の字面
			const std::string_view NodeTypeView(ts_node_type(VisitNode));
			// 別の関数の内側へ降りない事の返戻
			if(NodeKind::FunctionLikeAny.Contains(NodeTypeView)) return false;
			if(NodeKind::VariableDeclaration.Contains(NodeTypeView)) {
				// 宣言内の識別子と文字列初期化位置の収集
				FirstPos.clear();
				// 直近文字列の終端位置
				uint32_t LastString = 0;
				bool IsAnyString = false, IsStringType = false;
				WalkAst(
					VisitNode,
					[&](const TSNode Cur) -> void {
						// 宣言構成の分類
						if(const std::string_view CurType(ts_node_type(Cur)); NodeKind::IdentifierLeafLike.Contains(CurType)) {
							// 識別名別の初出位置への初出位置登録
							FirstPos.try_emplace(NodeView(Source, Cur), ts_node_start_byte(Cur));
						} else if(NodeKind::StringLikeAll.Contains(CurType)) {
							IsAnyString = true;
							LastString = ts_node_start_byte(Cur);
						} else if(NodeKind::SimpleTypeLeaf.Contains(CurType) && IsStringTypeText(NodeView(Source, Cur))) IsStringType = true;
					}
				);
				// 宣言名毎の文字列文脈の登録
				for(const auto &[Declared, Pos] : FirstPos) {
					Events[Declared].push_back({ ts_node_start_byte(VisitNode), IsStringType || IsAnyString && LastString > Pos });
				}
				// 宣言の内側へ降りない事の返戻
				return false;
			}
			// 代入事象の収集
			if(NodeKind::AssignmentExpression.Contains(NodeTypeView)) {
				if(const TSNode Lhs = ts_node_named_child(VisitNode, 0); !ts_node_is_null(Lhs)) {
					if(const std::string_view Assigned = UnwrapAssignableName(Source, Lhs); !Assigned.empty()) {
						const TSNode Rhs = ts_node_named_child(VisitNode, 1);
						Events[Assigned].push_back({ ts_node_start_byte(VisitNode), !ts_node_is_null(Rhs) && NodeKind::StringLikeAll.Contains(Rhs) });
						// 代入の内側へ降りない事の返戻
						return false;
					}
				}
			}
			// 子孫へ降りる事の返戻
			return true;
		};
		// 直下の子からの事象走査
		ForEachNamedChild(
			Scope,
			[&Visit](const TSNode Sib) -> void {
				if(Visit(Sib)) WalkChildrenCursor(Sib, Visit);
			}
		);
		// 名前毎の事象の返戻
		return Events;
	};
	// 関数／メソッドの仮引数コンテナを部分木から探索
	const auto FindParamContainer = [](const TSNode Func) -> TSNode {
		// 探索結果の初期化
		TSNode Result {};
		// 仮引数容器までの走査関数
		const auto Visit = [&Result](const TSNode Cur) -> bool {
			// 発見済なら降りない事の返戻
			if(!ts_node_is_null(Result)) return false;
			// 対象節点型の字面
			const std::string_view NodeTypeView(ts_node_type(Cur));
			// 仮引数容器の採用
			if(NodeKind::ParameterContainer.Contains(NodeTypeView)) {
				Result = Cur;
				// 発見した事の返戻
				return false;
			}
			// 本体系以外へ降りる事の返戻
			return !NodeKind::FunctionBodyContainer.Contains(NodeTypeView);
		};
		if(!ts_node_is_null(Func) && Visit(Func)) WalkChildrenCursor(Func, Visit);
		// 部分木走査結果の返戻
		return Result;
	};
	// 同名仮引数の文字列型判定関数
	const auto FindParameterEvent =
	[&Source, &Name, &IsStringTypeText, &FindParamContainer](const TSNode Func, bool &OutIsString) -> bool {
		// 仮引数節点の列
		const TSNode Params = FindParamContainer(Func);
		// 仮引数の並びが無い事の返戻
		if(ts_node_is_null(Params)) return false;
		// 仮引数列の名前一致探索
		bool IsFound = false;
		ForEachNamedChild(
			Params,
			[&](const TSNode Param) -> bool {
				const std::string_view ParamTypeView(ts_node_type(Param));
				std::string_view ParamName, ParamType;
				// JS の型注釈無等の単純な識別子仮引数
				if(NodeKind::IdentifierLeafLike.Contains(ParamTypeView)) ParamName = NodeView(Source, Param);
				else if(NodeKind::NamedParameterKind.Contains(ParamTypeView)) {
					ForEachNamedChild(
						Param,
						[&](const TSNode Sub) -> void {
							// 内側節点型の字面
							const std::string_view SubTypeView(ts_node_type(Sub));
							// 言語別の型包みの解決
							if(NodeKind::IdentifierLeafLike.Contains(SubTypeView) && ParamName.empty()) ParamName = NodeView(Source, Sub);
							else if(NodeKind::SimpleTypeLeaf.Contains(SubTypeView) && ParamType.empty()) ParamType = NodeView(Source, Sub);
							else if(SubTypeView == "user_type") {
								ForEachChild(
									Sub,
									// Kotlin 型識別子の採用関数
									[&](const TSNode GrandChild) -> void {
										if(std::string_view(ts_node_type(GrandChild)) == "type_identifier" && ParamType.empty()) {
											ParamType = NodeView(Source, GrandChild);
										}
									}
								);
							} else if(SubTypeView == "type_annotation") {
								ForEachChild(
									Sub,
									// TypeScript 型注釈の採用関数
									[&](const TSNode GrandChild) -> void {
										if(NodeKind::SimpleTypeLeaf.Contains(ts_node_type(GrandChild)) && ParamType.empty()) ParamType = NodeView(Source, GrandChild);
									}
								);
							}
						}
					);
				}
				// 同名の仮引数が持つ型情報の採用
				if(ParamName == Name) {
					IsFound = true;
					OutIsString = IsStringTypeText(ParamType);
				}
				// 対象仮引数発見で走査打切，未発見なら次の仮引数への返戻
				return !IsFound;
			}
		);
		// 対象名一致の仮引数発見有無の返戻
		return IsFound;
	};
	// 祖先の範囲に在る代入前の直近事象の探索
	bool HasEvent = false, IsLatestString = false;
	TSNode Cur = AssignNode;
	while(true) {
		// 対象の親節点
		const TSNode Parent = ts_node_parent(Cur);
		if(ts_node_is_null(Parent)) break;
		const std::string_view ParentView(ts_node_type(Parent));
		if(NodeKind::ScopeContainer.Contains(ParentView)) {
			StringEventIndex::iterator Scope = Index.find(Parent.id);
			// 未調査の範囲だけの事象索引の構築
			if(Scope == Index.end()) Scope = Index.emplace(Parent.id, BuildEvents(Parent)).first;
			// 代入を含む子より前の兄弟の事象（位置が其の子の始まりより前の物）の直近
			if(
				const std::unordered_map<std::string_view, std::vector<std::pair<uint32_t, bool>>>::const_iterator Named =
				Scope->second.find(Name);
				Named != Scope->second.end()
			) {
				// 引数の使用履歴
				const std::vector<std::pair<uint32_t, bool>> &Events = Named->second;
				const std::vector<std::pair<uint32_t, bool>>::const_iterator After = std::lower_bound(
					Events.begin(),
					Events.end(),
					ts_node_start_byte(Cur),
					// 利用位置より前の事象判定関数
					[](const std::pair<uint32_t, bool> &Event, const uint32_t Pos) -> bool {
						// 位置が手前かの返戻
						return Event.first < Pos;
					}
				);
				// 利用位置より手前の直近事象の採用
				if(After != Events.begin()) {
					HasEvent = true;
					IsLatestString = std::prev(After)->second;
				}
			}
			if(HasEvent) break;
		}
		// 関数／メソッド境界到達時の仮引数に依る遮蔽確認（同名仮引数有時は其の事象を追加し外側スコープ走査を停止）
		if(NodeKind::FunctionLikeAny.Contains(ParentView)) if(bool IsParamString = false; FindParameterEvent(Parent, IsParamString)) {
			HasEvent = true;
			IsLatestString = IsParamString;
			break;
		}
		Cur = Parent;
	}
	// 直近の事象が文字列文脈なら true の返戻
	return HasEvent && IsLatestString;
}

/**
 * 増分代入の前置インクリメント推奨検出関数 (C/C++/C#/Java/JS/TS/Kotlin)
 * @param Node 対象ノード
 * @param Source ソース文字列
 * @param Index スコープ毎の宣言・代入の事象の索引（文字列の変数の推定に使い回す）
 * @param Out 警告格納先
 */
void LintPass::CheckCompoundAssignIncrement(
	const TSNode Node,
	const std::string &Source,
	StringEventIndex &Index,
	std::vector<LintWarning> &Out
) {
	// 対応言語の文文脈に在る x += 1 等へ前置増減を勧め，文字列結合・演算子多重定義が有る為自動書換は見送
	const TSNode Lhs = ts_node_named_child(Node, 0), Rhs = ts_node_named_child(Node, 1);
	// 右辺が整数 `1` 以外の１バイト比較での棄却
	if(ts_node_is_null(Lhs) || ts_node_is_null(Rhs) || NodeView(Source, Rhs) != "1") return;
	const std::string_view OpToken = FindUnnamedToken(Source, Node, "+=", "-=");
	// 複合代入演算子を持たない節点の除外
	if(OpToken.empty()) return;
	// 親節点の文文脈該当有無の確認（式値を使う代入は対象外）
	const TSNode Parent = ts_node_parent(Node);
	// 条件成立時の返戻
	if(ts_node_is_null(Parent)) return;
	const std::string_view ParentType(ts_node_type(Parent));
	bool IsStmtCtx = NodeKind::StmtCtxParent.Contains(ParentType);
	if(!IsStmtCtx && ParentType == "for_statement") {
		TSNode UpdateSlot = TSSource::FieldChild(Parent, "update");
		if(ts_node_is_null(UpdateSlot)) UpdateSlot = TSSource::FieldChild(Parent, "increment");
		IsStmtCtx = !ts_node_is_null(UpdateSlot) && ts_node_eq(UpdateSlot, Node);
	}
	// 条件成立時の返戻
	if(!IsStmtCtx) return;
	// 左辺が単純識別子の場合，近傍の文字列リテラル初期化宣言を検索して文字列連結なら警告抑制
	if(
		// 更新対象の識別名
		const std::string_view LhsName = UnwrapAssignableName(Source, Lhs);
		!LhsName.empty() && LooksLikeStringVariable(Source, Node, LhsName, Index)
		// 条件成立時の返戻
	) return;
	Push(
		Node,
		OpToken == "+=" ?
		"If numeric and equivalent, use `++x` instead of `x += 1`" :
		"If numeric and equivalent, use `--x` instead of `x -= 1`",
		Out
	);
	// 終了
	return;
}

/**
 * C++ 後置増減候補検出関数
 * @param Node 対象更新式
 * @param Source ソース文字列
 * @param Out 警告格納先
 */
void LintPass::CheckCppPostfixIncrement(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out) {
	// 被演算子の取得
	const TSNode Operand = ts_node_named_child(Node, 0);
	// 条件成立時の返戻
	if(ts_node_is_null(Operand)) return;
	// 後置演算子の探索
	std::string_view Operator;
	uint32_t OperatorStart = 0;
	ForEachChild(
		Node,
		[&](const TSNode Child) -> bool {
			// 名前付の子を読み飛ばす事の返戻
			if(ts_node_is_named(Child)) return true;
			// 名前無字句の抽出
			const std::string_view Token = NodeView(Source, Child);
			// 増減以外の字句を読み飛ばす事の返戻
			if(Token != "++" && Token != "--") return true;
			// 演算子と開始位置の記録
			Operator = Token;
			OperatorStart = ts_node_start_byte(Child);
			// 更新演算子発見で走査打切の返戻
			return false;
		}
	);
	// 条件成立時の返戻
	if(Operator.empty() || OperatorStart < ts_node_end_byte(Operand)) return;
	// 被演算子を包む括弧の除去
	TSNode Expression = Node, Parent = ts_node_parent(Expression);
	while(!ts_node_is_null(Parent) && std::string_view(ts_node_type(Parent)) == "parenthesized_expression") {
		Expression = Parent;
		Parent = ts_node_parent(Expression);
	}
	// 条件成立時の返戻
	if(ts_node_is_null(Parent)) return;
	// 式値の未使用文脈判定
	const std::string_view ParentType(ts_node_type(Parent));
	bool IsResultUnused = ParentType == "expression_statement";
	if(!IsResultUnused && ParentType == "for_statement") {
		const TSNode Update = TSSource::FieldChild(Parent, "update");
		IsResultUnused = !ts_node_is_null(Update) && ts_node_eq(Update, Expression);
	}
	// 条件成立時の返戻
	if(!IsResultUnused) return;
	// 後置増減候補の警告登録
	Push(
		Node,
		Operator == "++" ?
		"If equivalent, prefer prefix `++x` when the result is unused" :
		"If equivalent, prefer prefix `--x` when the result is unused",
		Out
	);
	// 終了
	return;
}

/**
 * 冗長な二重符号 (`-(-x)` / `+(+x)`) 検出関数
 * @param Node 前置単項ノード
 * @param Source ソース文字列
 * @param Out 警告格納先
 */
void LintPass::CheckRedundantDoubleSign(const TSNode Node, const std::string &Source, std::vector<LintWarning> &Out) {
	const TSNode Sign = ts_node_child(Node, 0);
	// 演算子が１文字の `-`/`+` 以外（`*` `&` `!` `~` `--` 等）は対象外
	if(ts_node_is_named(Sign) || ts_node_end_byte(Sign) != ts_node_start_byte(Sign) + 1) return;
	const char Operator = Source[ts_node_start_byte(Sign)];
	// 条件成立時の返戻
	if(Operator != '-' && Operator != '+') return;
	// 符号の融合を避ける括弧 (`-(-x)`) の内側を被演算子として確認
	TSNode Operand = ts_node_named_child(Node, 0);
	const bool IsWrapped =
	!ts_node_is_null(Operand) && NodeKind::GroupingParen.Contains(Operand) && ts_node_named_child_count(Operand) == 1;
	if(IsWrapped) Operand = ts_node_named_child(Operand, 0);
	// 被演算子が同じ１文字の符号の前置単項でなければ冗長でない（`-a` / `-(a + b)` / 前置の減算 `-(--x)` 等は対象外）
	if(ts_node_is_null(Operand) || !NodeKind::UnaryPre.Contains(Operand)) return;
	if(
		const TSNode Inner = ts_node_child(Operand, 0);
		!ts_node_is_named(Inner) && ts_node_end_byte(Inner) == ts_node_start_byte(Inner) + 1 &&
		Source[ts_node_start_byte(Inner)] == Operator
	) {
		// 整形後の字面（融合する言語は括弧付，其れ以外は密着）で提示
		static constexpr std::string_view Messages[2][2] = {
			{
				"If semantics are preserved, consider removing double negation `--x`",
				"If semantics are preserved, consider removing double unary plus `++x`"
			},
			{
				"If semantics are preserved, consider removing double negation `-(-x)`",
				"If semantics are preserved, consider removing double unary plus `+(+x)`"
			}
		};
		Push(Node, Messages[IsWrapped][Operator == '+'], Out);
	}
	// 終了
	return;
}

/**
 * ネストの深さの検査関数
 * @param Root 走査の起点
 * @param Source 整形済の文面
 * @param Language 対象言語
 * @param Out 警告格納先
 */
void LintPass::CheckNestingDepth(
	const TSNode Root,
	const std::string &Source,
	const Lang Language,
	std::vector<LintWarning> &Out
) {
	// 段数は整形後のインデントで数え，上限は其の表示幅で決定
	const size_t Limit = Language.NestingLimit();
	bool IsPrevDeep = false;
	for(size_t Pos = 0, Row = 0; Pos < Source.size(); ++Row) {
		const size_t End = std::min(Source.find('\n', Pos), Source.size());
		size_t Depth = 0;
		while(Pos + Depth < End && Source[Pos + Depth] == '\t') ++Depth;
		// 上限を超えた行と，深い塊が続いて居る間の行だけ節点を引く（其の他の行は判定に非関与）
		if(Depth > Limit || IsPrevDeep) {
			// 複数行字句内の行頭タブの字下げ計数からの除外
			const uint32_t Before = Pos ? static_cast<uint32_t>(Pos - 1) : 0, At = static_cast<uint32_t>(Pos + Depth);
			const TSNode Outer = ts_node_descendant_for_byte_range(Root, Before, At);
			// 字句の内側の行は構文の段を成さず，深い塊の切れ目にも回避
			if(
				const bool IsInsideToken = Pos && ts_node_start_point(Outer).row < Row &&
				(!ts_node_child_count(Outer) || NodeKind::StringLikeInnerPreserve.Contains(Outer));
				!IsInsideToken
			) {
				// 同じ塊の続きは同じ違反の繰返しに為る為，上限を超え始めた行だけを報告
				if(Depth > Limit && !IsPrevDeep) {
					Push(ts_node_descendant_for_byte_range(Root, At, At), "Indentation exceeds " + std::to_string(Limit) + " levels", Out);
				}
				IsPrevDeep = Depth > Limit;
			}
		}
		Pos = End + 1;
	}
	// 終了
	return;
}

/** ========== 検査実行 ========== */
/**
 * 違反警告リスト返戻関数
 * 計算量：構文木の節点数 V と原稿の長さ N に対し O(V + N)（走査は１回で，節点毎の手間は定数か遡る原文の長さに比例する）
 * @param Src 整形済ソース
 * @param Language 対象言語
 * @return 検出された違反警告リスト
 */
std::vector<LintWarning> LintPass::Run(const TSSource &Src, const Lang Language) {
	// 検査結果を合流させる警告の格納先
	std::vector<LintWarning> Out;
	// 構文木の無い入力の空の結果の返戻
	if(!Src.IsParsed()) return Out;
	const TSNode Root = Src.GetRoot();
	const std::string &Source = Src;
	// 個別節点の検査に先立つネスト深さの検査
	CheckNestingDepth(Root, Source, Language, Out);
	// 言語依存フラグのループ外での一括評価
	const bool IsCOrCpp = Language.IsCFamily(), IsJsOrTs = Language.IsJsTs();
	const bool IsCppOnly = Language == Lang::Cpp && !Language.IsCShared;
	const bool IsBoolFlipLangOk = IsCOrCpp || Language == Lang::CSharp || Language == Lang::Java || Language == Lang::Rust;
	const bool IsExplicitReturnLangOk =
	IsCOrCpp || Language == Lang::CSharp || Language == Lang::Java || Language == Lang::Kotlin || Language == Lang::Swift ||
	Language == Lang::PHP;
	// 文書化コメント存在検査の対象言語か
	const bool IsDocPresenceLangOk = Language == Lang::Swift || Language == Lang::Python;
	const bool IsDocCompletenessLangOk = DocSig::IsTargetLanguage(Language);
	const bool IsCompoundIncLangOk =
	IsCOrCpp || IsJsOrTs || Language == Lang::CSharp || Language == Lang::Java || Language == Lang::Kotlin || Language == Lang::PHP;
	// return の前後のコメントの有無は構文木のコメントの範囲で引く為，並列の走査の前に開始の昇順で収集
	std::vector<std::pair<uint32_t, uint32_t>> Comments;
	std::unordered_set<std::string> MutatedNames;
	WalkAst(
		Root,
		[&](const TSNode Node) -> void {
			// 節点型の分類
			const std::string_view Type(ts_node_type(Node));
			if(NodeKind::Comment.Contains(Type)) {
				Comments.emplace_back(ts_node_start_byte(Node), ts_node_end_byte(Node));
				// コメントの範囲を記録して終了
				return;
			}
			// 変更候補の収集を C 系言語へ限定
			if(!IsCOrCpp) return;
			if(Language == Lang::Cpp && Type == "argument_list") {
				ForEachNamedChild(
					Node,
					[&](const TSNode Argument) -> void {
						if(const std::string_view Name = UnwrapAssignableName(Source, Argument); !Name.empty()) MutatedNames.emplace(Name);
					}
				);
				// 実引数の書換を記録して終了
				return;
			}
			if(
				// 識別子のアドレス取得箇所か
				const bool IsAddressTaken = Type == "pointer_expression" && NodeView(Source, Node).starts_with('&');
				!NodeKind::MutatingExpression.Contains(Type) && !IsAddressTaken
				// 条件成立時の返戻
			) return;
			// 更新対象名の収集
			const TSNode Operand = ts_node_named_child(Node, 0);
			// 条件成立時の返戻
			if(ts_node_is_null(Operand)) return;
			if(const std::string_view Name = UnwrapAssignableName(Source, Operand); !Name.empty()) MutatedNames.emplace(Name);
		}
	);
	std::string_view TypeInferType, GotoLabelType, LabelType;
	switch(Language.Id) {
	case Lang::C:
		GotoLabelType = "goto_statement";
		break;
	case Lang::Cpp:
		TypeInferType = "placeholder_type_specifier";
		GotoLabelType = "goto_statement";
		break;
	case Lang::CSharp:
		TypeInferType = "implicit_type";
		// C# はラベル付の `break` を持たない為，ラベルは `goto` の跳び先としてだけ確認
		GotoLabelType = "goto_statement";
		break;
	case Lang::Java:
		TypeInferType = "type_identifier";
		GotoLabelType = "labeled_statement";
		break;
	case Lang::Go:
		GotoLabelType = "goto_statement";
		LabelType = "labeled_statement";
		break;
	case Lang::PHP:
		GotoLabelType = "goto_statement";
		break;
	case Lang::Rust:
	case Lang::Kotlin:
		// Rust と Kotlin に於けるラベルのループ周辺配置
		LabelType = "label";
		break;
	case Lang::Swift:
		LabelType = "statement_label";
		break;
	case Lang::JavaScript:
		GotoLabelType = "labeled_statement";
		break;
	case Lang::TypeScript:
		TypeInferType = "predefined_type";
		GotoLabelType = "labeled_statement";
		break;
	default:
		break;
	}
	// 構文木走査の１回への集約とノード型での振分に依る関連 ShouldCheck のみの呼出
	const auto Dispatch = [&](
		const TSNode Node,
		std::vector<LintWarning> &Sink,
		StringEventIndex &Index,
		CondContextIndex &Conditions,
		LabelTargetIndex &Labels
	) -> void {
		// 名前無字句の型名取得前での一括除外
		if(!ts_node_is_named(Node)) return;
		// 節点型の字面
		const std::string_view TypeView(ts_node_type(Node));
		// 先行順走査の振分前に条件文脈を記録し，子の比較式から参照
		if((IsCOrCpp || IsJsOrTs) && NodeKind::CondContextParent.Contains(TypeView)) MarkCondContext(Node, Source, Conditions);
		// TypeView ベースの排他的振分の else if 連鎖での短絡，言語別振分はループ外で対象の型を確定済の為１比較で充足
		if(NodeKind::ClassLikeStrict.Contains(TypeView) || Language == Lang::Java && TypeView == "enum_declaration") {
			// クラス内のアクセス修飾子順序検査
			CheckAccessSpecifierOrder(Node, Source, Language, Sink);
		} else if(!GotoLabelType.empty() && TypeView == GotoLabelType || !LabelType.empty() && TypeView == LabelType) {
			// goto とラベル付文の用途検査
			CheckGotoOrLabel(Node, Language, GotoLabelType == "goto_statement", Labels, Sink);
		} else if(NodeKind::FunctionBodyContainer.Contains(TypeView)) CheckUselessLocalVariable(Node, Source, Sink);
		else if(!TypeInferType.empty() && TypeView == TypeInferType) CheckTypeInferenceKeyword(Node, Source, Language, Sink);
		else if(NodeKind::FunctionLikeDefinition.Contains(TypeView)) {
			if(IsExplicitReturnLangOk) CheckMissingExplicitReturn(Node, Source, Language, Sink);
			// 関数名の命名形式検査
			CheckFunctionNamingConvention(Node, Source, Language, Sink);
			if(Language == Lang::Python) CheckPythonReturnTypeHint(Node, Sink);
			if(IsDocPresenceLangOk) CheckDocCommentPresence(Node, Source, Language, Sink);
			// DocCompleteness は自動生成対象言語の関数定義（コンストラクタ含む）で説明欠落を検出
			if(IsDocCompletenessLangOk) CheckDocCommentCompleteness(Node, Source, Language, Sink);
		} else if(NodeKind::ClassBody.Contains(TypeView)) CheckMemberDeclarationOrder(Node, Language, Sink);
		else if(IsCOrCpp && TypeView == "declaration") {
			if(IsCppOnly) CheckConstexprCandidate(Node, Source, Sink);
			// C++ 変数の const 化候補検査
			CheckCppConstCandidate(Node, Source, MutatedNames, Sink);
			// 浮動小数点接尾辞の候補検査
			CheckFloatSuffixCandidate(Node, Source, Sink);
		} else if(IsCOrCpp && TypeView == "preproc_def") CheckMacroConstant(Node, Sink);
		else if(IsCOrCpp && TypeView == "preproc_ifdef") CheckHeaderGuardCandidate(Node, Source, Sink);
		else if(IsCOrCpp && TypeView == "using_declaration") CheckUsingNamespace(Node, Source, Sink);
		else if(TypeView == "return_statement") CheckReturnCommentPresence(Node, Source, Comments, Language, Sink);
		else if(TypeView == "binary_expression") {
			// 二項式検査の言語別振分（型比較を１回に統合）
			if(IsCOrCpp || IsJsOrTs) CheckZeroComparisonCondition(Node, Source, Conditions, Language, Sink);
			if(IsJsOrTs) {
				// 数値比較の厳密等価演算子検査
				CheckNumberStrictEquality(Node, Source, Sink);
				// ヌル値比較の簡約候補検査
				CheckNullishComparisonCandidate(Node, Source, Sink);
			}
		} else if(IsCppOnly && TypeView == "cast_expression") CheckCStyleCast(Node, Sink);
		else if(IsCppOnly && NodeKind::CppNullCandidate.Contains(TypeView)) CheckNullMacroCandidate(Node, Source, Sink);
		else if(Language.IsBraceLang() && NodeKind::IfNode.Contains(TypeView)) CheckUnsafeIfMergeCandidate(Node, Source, Sink);
		else if(NodeKind::Comment.Contains(TypeView)) CheckStandardsReference(Node, Source, Sink);
		else if(IsBoolFlipLangOk && TypeView == "assignment_expression") CheckBoolFlipPattern(Node, Source, Sink);
		// 全言語に対する比較境界検査
		if(NodeKind::CmpBoundaryNode.Contains(TypeView)) CheckCmpBoundary(Node, Source, Sink);
		// CompoundAssignIncrement: `++` 前置構文を持つ言語のみ対象
		if(IsCompoundIncLangOk && NodeKind::AssignmentExpression.Contains(TypeView)) {
			// １加減算の増減演算子候補検査
			CheckCompoundAssignIncrement(Node, Source, Index, Sink);
		}
		if(Language == Lang::Cpp && TypeView == "update_expression") CheckCppPostfixIncrement(Node, Source, Sink);
		// 二重符号 `- -x` / `+ +x` は全言語の前置単項で検出
		if(NodeKind::UnaryPre.Contains(TypeView)) CheckRedundantDoubleSign(Node, Source, Sink);
	};
	const uint32_t RootChildCount = ts_node_child_count(Root);
	const size_t NumThreads = Parallel::DecideThreads(RootChildCount, 8);
	std::vector<StringEventIndex> Indexes(std::max<size_t>(NumThreads, 1));
	std::vector<CondContextIndex> Conditions(std::max<size_t>(NumThreads, 1));
	std::vector<LabelTargetIndex> Labels(std::max<size_t>(NumThreads, 1));
	// 小規模入力の単独走査
	if(NumThreads < 2) {
		WalkAst(
			Root,
			[&](const TSNode Node) -> void {
				Dispatch(Node, Out, Indexes[0], Conditions[0], Labels[0]);
			}
		);
	} else {
		// 子の分担前に行う根自身の検査
		Dispatch(Root, Out, Indexes[0], Conditions[0], Labels[0]);
		// `ts_node_child(Root, Idx)` の累積二次計算量を避ける為，１度の走査でベクタに収集
		std::vector<TSNode> RootKids;
		// 根直下の子節点列の格納領域予約
		RootKids.reserve(RootChildCount);
		ForEachChild(
			Root,
			[&](const TSNode Child) -> void {
				RootKids.push_back(Child);
			}
		);
		// 走脈間で競合しない警告収集先の確保
		std::vector<std::vector<LintWarning>> ThreadOut(NumThreads);
		Parallel::ForChunks(
			RootKids.size(),
			NumThreads,
			[&](const size_t Start, const size_t End, const size_t Tid) -> void {
				// 節点単位の警告列
				std::vector<LintWarning> &Local = ThreadOut[Tid];
				for(size_t Idx = Start; Idx < End; ++Idx) {
					WalkAst(
						RootKids[Idx],
						[&](const TSNode Node) -> void {
							Dispatch(Node, Local, Indexes[Tid], Conditions[Tid], Labels[Tid]);
						}
					);
				}
			}
		);
		size_t Total = Out.size();
		for(const std::vector<LintWarning> &Local : ThreadOut) Total += Local.size();
		// 検査順に結合する警告領域の確保
		Out.reserve(Total);
		// 各担当区間の警告の所有権移動
		for(std::vector<LintWarning> &Local : ThreadOut) {
			Out.insert(Out.end(), std::make_move_iterator(Local.begin()), std::make_move_iterator(Local.end()));
		}
	}
	// 走査分担に依らない警告の安定した位置順整列
	std::stable_sort(
		Out.begin(),
		Out.end(),
		[](const LintWarning &Lhs, const LintWarning &Rhs) -> bool {
			// 行と桁の昇順の比較の返戻
			return Lhs.Row != Rhs.Row ? Lhs.Row < Rhs.Row : Lhs.Column < Rhs.Column;
		}
	);
	// 集計済警告ベクタの返戻
	return Out;
}
