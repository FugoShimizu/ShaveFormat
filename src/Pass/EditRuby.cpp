#include "Edit.hpp"
#include "../Util/DeclEdit.hpp"
#include "../Util/NodeKind.hpp"
#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/**
 * Ruby の暗黙 return への明示 `return` 付与編集の収集関数（関数末尾の単純式が対象）
 * @param Src 整形対象ソース
 * @param Edits 編集列追加先
 */
void EditPass::CollectRubyExplicitReturnEdits(const TSSource &Src, std::vector<TextEdit> &Edits) {
	// 字句 `def` を検索で短絡（不在ならメソッド定義０件で確定）
	if(Src.find("def") == std::string::npos) return;
	// 本体毎に再利用する名前付子列の準備
	std::vector<TSNode> NamedKids;
	const auto EmitReturn = [&](const TSNode Body, const bool SkipsBodyTail) -> void {
		// 本体毎の直下の子の再収集
		NamedKids.clear();
		ForEachNamedChild(
			// 子を列挙する本体
			Body,
			// 名前付子の収集
			[&](const TSNode Child) -> void {
				// 出現順を保持して追加
				NamedKids.push_back(Child);
			}
		);
		// 空の本体での返戻値作成の省略
		if(NamedKids.empty()) return;
		// 返戻候補の子位置
		size_t LastIdx = NamedKids.size() - 1;
		// 後続節を読み飛ばす本体
		if(SkipsBodyTail) while(NodeKind::RubyBodyTailClause.Contains(NamedKids[LastIdx])) {
			// 条件成立時の返戻
			if(!LastIdx) return;
			// 直前の返戻候補へ移動
			--LastIdx;
		}
		// 本体末尾の返戻候補
		const TSNode Last = NamedKids[LastIdx];
		const std::string_view LastType(ts_node_type(Last)), LastView = Src.View(Last);
		// ヒアドキュメント終端子の除外（解析器は末尾に constant を補う為，前兄弟が heredoc_body なら構造の一部として保持）
		if(LastIdx && LastType == "constant" && std::string_view(ts_node_type(NamedKids[LastIdx - 1])) == "heredoc_body") return;
		const bool IsLastChainedLike = NodeKind::RubyChainedStringLike.Contains(LastType);
		// 末尾より前の兄弟を走査
		for(size_t Idx = 0; Idx < LastIdx; ++Idx) {
			if(
				const std::string_view PrevType = ts_node_type(NamedKids[Idx]);
				PrevType == "return" || IsLastChainedLike && LastType == PrevType
				// 条件成立時の返戻
			) return;
		}
		// 脱出・副作用専用呼出の除外（接頭辞でなく節点名・メソッド場を厳密照合）
		if(
			!NodeKind::RubyReturnSafe.Contains(LastType) ||
			LastType == "identifier" && (LastView == "raise" || NodeKind::RubySideEffectMethod.Contains(LastView))
			// 条件成立時の返戻
		) return;
		// 名前を持つ末尾呼出
		if(LastType == "call") if(const TSNode Method = TSSource::FieldChild(Last, "method"); !ts_node_is_null(Method)) {
			// 呼出名で例外送出と副作用専用の処理の除外
			if(
				const std::string_view MethodView = Src.View(Method);
				MethodView == "raise" || NodeKind::RubySideEffectMethod.Contains(MethodView)
				// 条件成立時の返戻
			) return;
			else if(NodeKind::RubyLoggerMethod.Contains(MethodView)) {
				// 記録専用の受け手での呼出結果返戻の省略
				if(
					const TSNode Receiver = TSSource::FieldChild(Last, "receiver");
					!ts_node_is_null(Receiver) && Src.View(Receiver).ends_with("logger")
					// 条件成立時の返戻
				) return;
			}
		}
		const auto RangeBeginIsValue = [](const TSNode Range) -> bool {
			const TSNode Begin = ts_node_child(Range, 0);
			// 始端が値式であるかの返戻
			return ts_node_is_named(Begin) && !NodeKind::RubyFlowExpr.Contains(Begin);
		};
		// return 直後に置けない語演算子と，return を始端へ取り込む範囲の除外
		if(
			LastType == "binary" && HasChildOf(
				Last,
				[&Src](const TSNode Child) -> bool {
					// 名前付の子は語の演算子でない事の返戻
					if(ts_node_is_named(Child)) return false;
					// 無名字句の字面取得
					const std::string_view ChildView = Src.View(Child);
					// キーワード演算子トークンかの返戻
					return ChildView == "and" || ChildView == "or";
				}
			) || LastType == "range" && !RangeBeginIsValue(Last)
			// 条件成立時の返戻
		) return;
		// return を修飾子形と読ませる先頭語と，Ruby 3.3 以前が許さない not の除外
		TSNode Head = Last;
		// 値の字面を始める最左の構文の探索
		while(true) {
			if(
				const std::string_view HeadType = ts_node_type(Head);
				NodeKind::RubyKeywordBlock.Contains(HeadType) || HeadType == "unary" && Src.View(ts_node_child(Head, 0)) == "not"
				// 条件成立時の返戻
			) return;
			// 葉に達したら先頭字句が確定
			if(!ts_node_named_child_count(Head)) break;
			const TSNode Leftmost = ts_node_named_child(Head, 0);
			if(ts_node_start_byte(Leftmost) != ts_node_start_byte(Head)) break;
			// 同じ開始位置の子へ降下
			Head = Leftmost;
		}
		// return 前置で構文が壊れる，裸の呼出を被演算子とする単項式の除外
		if(LastType == "unary") {
			if(
				const TSNode Operand = ts_node_named_child(Last, 0);
				!ts_node_is_null(Operand) && std::string_view(ts_node_type(Operand)) == "call"
			) {
				if(
					const TSNode Args = TSSource::FieldChild(Operand, "arguments");
					!ts_node_is_null(Args) && Src[ts_node_start_byte(Args)] != '('
					// 条件成立時の返戻
				) return;
			}
		}
		bool ShouldWrap = false;
		// 先頭の符号を検査
		if(std::string_view(ts_node_type(Head)) == "unary") {
			if(
				const TSNode Sign = ts_node_child(Head, 0), Operand = ts_node_named_child(Head, 0);
				!ts_node_is_named(Sign) && !ts_node_is_null(Operand) && (Src.View(Sign) == "-" || Src.View(Sign) == "+")
			) {
				const bool Attaches = Src.AttachesRubySign(Operand);
				// 符号後の空白を除去
				if(Attaches && Src.End(Sign) < Src.Start(Operand)) TextEdit::Push(Src.End(Sign), Src.Start(Operand), "", Edits);
				// 二項演算との誤読を防ぐ条件
				ShouldWrap = !Attaches || Src.View(Sign) == "+";
			}
		}
		const uint32_t Start = ts_node_start_byte(Last);
		TextEdit::Push(Start, Start, ShouldWrap ? "return(" : "return ", Edits);
		// 開き括弧を追加した式だけの終端閉鎖
		if(ShouldWrap) TextEdit::Push(ts_node_end_byte(Last), ts_node_end_byte(Last), ")", Edits);
	};
	// 各メソッドの独立した返戻値範囲としての走査
	WalkAst(
		// 走査起点
		Src.GetRoot(),
		// メソッド候補の処理
		[&](const TSNode Node) -> void {
			// 走査起点のメソッドへの限定（別スコープの rescue へ誤った return を付けない）
			if(!NodeKind::RubyMethodDef.Contains(ts_node_type(Node))) return;
			const TSNode Body = TSSource::FieldChild(Node, "body");
			// return 挿入が構文を壊す，式本体の１行定義の除外
			if(ts_node_is_null(Body) || std::string_view(ts_node_type(Body)) != "body_statement") return;
			// 後続節より前の末尾値を明示返戻
			EmitReturn(Body, true);
			// メソッド直下 rescue の末尾への付与（共有バッファ上書きを避け独立カーソルで列挙）
			ForEachNamedChild(
				// 列挙するメソッド本体
				Body,
				// 例外救済節の処理
				[&](const TSNode Sibling) -> void {
					// メソッド直下の例外救済節だけの選択
					if(std::string_view(ts_node_type(Sibling)) != "rescue") return;
					// 例外救済節の本体取得
					const TSNode Then = TSSource::FieldChild(Sibling, "body");
					// 条件成立時の返戻
					if(ts_node_is_null(Then) || std::string_view(ts_node_type(Then)) != "then") return;
					// 救済節の末尾値を明示返戻
					EmitReturn(Then, false);
				}
			);
		}
	);
	// 終了
	return;
}

/**
 * Ruby の文区切りセミコロン (`;`) の改行置換編集の収集関数
 * @param Src ソースコード
 * @param Edits 収集先のエディット一覧
 */
void EditPass::CollectRubySemicolonEdits(const TSSource &Src, std::vector<TextEdit> &Edits) {
	// 字句 `;` 不在なら短絡（文区切り０件で確定）
	if(Src.find(';') == std::string::npos) return;
	std::vector<uint32_t> HeredocBeginnings;
	// ヒアドキュメント開始字句の探索
	WalkAst(
		Src.GetRoot(),
		[&](const TSNode Current) -> void {
			// 開始行の後半を改行から保護する為の位置収集
			if(std::string_view(ts_node_type(Current)) == "heredoc_beginning") HeredocBeginnings.push_back(Src.Start(Current));
		}
	);
	std::sort(HeredocBeginnings.begin(), HeredocBeginnings.end());
	// 無名 `;` トークン（文区切り）を改行へ置換する（余分な空行は後続の構造化整形で畳まれる）
	WalkAst(
		Src.GetRoot(),
		[&](const TSNode Current) -> void {
			// 構文要素や文字列中の同じ文字を対象からの除外
			if(ts_node_is_named(Current) || std::string_view(ts_node_type(Current)) != ";") return;
			// ヒアドキュメント・括弧内複文・局所宣言・式展開の `;` を保持
			const TSNode Parent = ts_node_parent(Current);
			if(
				const std::vector<uint32_t>::const_iterator Beginning =
				std::lower_bound(HeredocBeginnings.begin(), HeredocBeginnings.end(), TextEdit::LineStartOf(Src, Src.Start(Current)));
				Beginning != HeredocBeginnings.end() && *Beginning < Src.Start(Current) ||
				!ts_node_is_null(Parent) && NodeKind::RubySemicolonKeep.Contains(Parent) || HasAncestorOf(
					Current,
					[](const TSNode Ancestor) -> bool {
						// 文字列の式展開内かの返戻
						return std::string_view(ts_node_type(Ancestor)) == "interpolation";
					}
				)
			) {
				// 終了
				return;
			}
			TSNode Last = ts_node_prev_sibling(Current);
			// 包装先頭の区切から包装前への遡及
			if(ts_node_is_null(Last) && !ts_node_is_null(Parent)) Last = ts_node_prev_sibling(Parent);
			// 直前の文を終える最右の字句の取得
			while(!ts_node_is_null(Last) && ts_node_child_count(Last)) Last = ts_node_child(Last, ts_node_child_count(Last) - 1);
			// 継続字句で終わる文
			if(!ts_node_is_null(Last) && !ts_node_is_named(Last) && NodeKind::OpenTailToken.Contains(Last)) {
				TSNode Wrap = ts_node_parent(Last);
				// 裸の引数列を探索
				if(std::string_view(ts_node_type(Wrap)) != "range") {
					// 継続字句を所有する裸の引数列迄の遡及
					while(!ts_node_is_null(Wrap) && !(NodeKind::RubyBareList.Contains(Wrap) && Src.View(ts_node_child(Wrap, 0)) != "(")) {
						// 一段外の候補へ移動
						Wrap = ts_node_parent(Wrap);
					}
				}
				// 括弧で保護出来る対象を発見
				if(!ts_node_is_null(Wrap)) {
					// 引数・仮引数列は名前後の空白を開き括弧へ置換し，範囲は直前へ追加
					if(const TSNode Callee = ts_node_prev_sibling(Wrap); NodeKind::RubyBareList.Contains(Wrap) && !ts_node_is_null(Callee)) {
						TextEdit::Push(Src.End(Callee), Src.Start(Wrap), "(", Edits);
						// 範囲の直前へ開き括弧を挿入
					} else TextEdit::Push(Src.Start(Wrap), Src.Start(Wrap), "(", Edits);
					TextEdit::Push(Src.End(Wrap), Src.End(Current), ")\n", Edits);
					// 閉じ括弧で文を閉じた為，終了
					return;
				}
			}
			TextEdit::Push(Src.Start(Current), Src.End(Current), "\n", Edits);
		}
	);
	// 終了
	return;
}

/**
 * Ruby の修飾子形式への変換編集の収集関数（本体が単一文で他の節を伴わない分岐・繰返が対象）
 * @param Src 整形対象ソース
 * @param Edits 編集列追加先
 * @param PostAttaches 置換で節点型が変わる為のコメント引継先
 */
void EditPass::CollectRubyModifierFormEdits(
	TSSource &Src,
	std::vector<TextEdit> &Edits,
	std::vector<DeclEdit::PostEditAttach> &PostAttaches
) {
	// 修飾子形式の型対応と局所変数索引の準備
	static const std::unordered_map<std::string_view, const char *> ModifierFormTypes =
	{ { "if", "if_modifier" }, { "unless", "unless_modifier" }, { "until", "until_modifier" }, { "while", "while_modifier" } };
	const RubyLocals Locals = IndexRubyLocals(Src, NodeKind::RubyModifierForm);
	const auto Collect = [&](const TSNode Node) -> void {
		// 候補の型と名前付子の収集
		const std::string_view Type(ts_node_type(Node));
		std::vector<TSNode> Kids;
		ForEachNamedChild(
			Node,
			[&Kids](const TSNode Child) -> void {
				Kids.push_back(Child);
			}
		);
		// 他の節を伴う制御構文を候補からの除外
		if(Kids.size() != 2) return;
		const TSNode Condition = Kids[0], Body = Kids[1];
		// 条件で導入する変数又は本体の形が修飾子形式へ適さなければ保持
		if(
			HasDescendantOf(
				Condition,
				[](const TSNode Inner) -> bool {
					// 代入と照合に依る束縛の有無の返戻
					return NodeKind::RubyBinding.Contains(Inner);
				}
			) || !NodeKind::ThenDo.Contains(ts_node_type(Body))
			// 条件成立時の返戻
		) return;
		// 本体は単一文のみ（複数文は修飾子形式で表せない）
		std::vector<TSNode> BodyKids;
		ForEachNamedChild(
			Body,
			[&BodyKids](const TSNode Child) -> void {
				BodyKids.push_back(Child);
			}
		);
		// 一文だけの後置条件前への移動許可
		if(BodyKids.size() != 1) return;
		// 変換する単一文の取得
		const TSNode Statement = BodyKids[0];
		// ネスト制御構造と `begin ... end while` の修飾子形式化除外
		if(NodeKind::RubyKeywordBlock.Contains(Statement)) return;
		// 文末の最深字句への降下
		TSNode Last = Statement;
		while(ts_node_child_count(Last)) Last = ts_node_child(Last, ts_node_child_count(Last) - 1);
		// 条件成立時の返戻
		if(!ts_node_is_named(Last) && NodeKind::OpenTailToken.Contains(Last)) return;
		// 条件内の識別子名の収集
		std::unordered_set<std::string_view> ConditionNames;
		WalkAst(
			Condition,
			[&Src, &ConditionNames](const TSNode Inner) -> void {
				// 本体の束縛と衝突し得る条件の名前の収集
				if(std::string_view(ts_node_type(Inner)) == "identifier") ConditionNames.insert(Src.View(Inner));
			}
		);
		if(
			!ConditionNames.empty() && HasDescendantOf(
				Statement,
				[&Src, &ConditionNames, &Locals, Node](const TSNode Inner) -> bool {
					// 束縛候補の型判定
					const std::string_view InnerType = ts_node_type(Inner);
					// 束縛でない事の返戻
					if(!NodeKind::RubyBinding.Contains(InnerType)) return false;
					// 名前付捕捉は捕捉名を字句で持たない為，捕捉の有る正規表現は条件の名前を束縛し得ると看做す事の返戻
					if(InnerType == "regex") return Src.View(Inner).find("(?<") != std::string_view::npos;
					// 代入左辺への探索範囲限定
					const TSNode Left = TSSource::FieldChild(Inner, "left");
					// 条件に現れる未定義名を束縛する子孫の有無の返戻
					return HasDescendantOf(
						ts_node_is_null(Left) ? Inner : Left,
						[&Src, &ConditionNames, &Locals, Node](const TSNode Name) -> bool {
							// 条件に現れて未だ局所変数でない名前かの返戻
							return std::string_view(ts_node_type(Name)) == "identifier" && ConditionNames.contains(Src.View(Name)) &&
							!Locals.IsDefined(Node, Src.Start(Node), Src.View(Name));
						}
					);
				}
			)
			// 条件成立時の返戻
		) return;
		const auto HasHeredoc = [](const TSNode Target) -> bool {
			// 開始トークンの有無の返戻
			return HasDescendantOf(
				Target,
				[](const TSNode Inner) -> bool {
					// 本文の開始行を固定する字句かの返戻
					return std::string_view(ts_node_type(Inner)) == "heredoc_beginning";
				}
			);
		};
		// 条件と本体のヒアドキュメント開始位置の保持
		if(HasHeredoc(Statement) || HasHeredoc(Condition)) return;
		// 本体と条件を後置条件式へ再構成
		std::string Replacement(Src.View(Statement));
		Replacement += ' ';
		Replacement.append(Type.data(), Type.size());
		Replacement += ' ';
		const std::string_view ConditionView = Src.View(Condition);
		Replacement.append(ConditionView.data(), ConditionView.size());
		// 置換範囲と引き継ぐコメントの回収
		const uint32_t Start = Src.Start(Node);
		std::vector<CommentAttach> Leading = Src.TakeLeading(Node), Trailing = Src.TakeTrailing(Node);
		const auto Harvest = [&Src, &Leading, &Trailing](const TSNode Sub) -> void {
			// 内側の錨から先行・末尾を分けて回収
			std::vector<CommentAttach> InnerLead = Src.TakeLeading(Sub), InnerTrail = Src.TakeTrailing(Sub);
			Leading.insert(Leading.end(), std::make_move_iterator(InnerLead.begin()), std::make_move_iterator(InnerLead.end()));
			Trailing.insert(Trailing.end(), std::make_move_iterator(InnerTrail.begin()), std::make_move_iterator(InnerTrail.end()));
		};
		// 移動する本体と条件の両方からのコメント引継
		WalkAst(Body, Harvest);
		WalkAst(Condition, Harvest);
		// 再解析後の型と範囲を添えて紐付の復元を予約
		if(!Leading.empty() || !Trailing.empty()) {
			if(
				const std::unordered_map<std::string_view, const char *>::const_iterator Iter = ModifierFormTypes.find(Type);
				Iter != ModifierFormTypes.end()
			) {
				PostAttaches.push_back(
					{ Start, 0, Iter->second, std::move(Leading), std::move(Trailing), static_cast<uint32_t>(Replacement.size()) }
				);
			}
		}
		// 修飾子形式への置換編集追加
		TextEdit::Push(Start, Src.End(Node), std::move(Replacement), Edits);
	};
	// 値位置の代入を条件で消さない様，文の並び側から直下の分岐だけを候補化
	WalkAst(
		Src.GetRoot(),
		[&](const TSNode Host) -> void {
			// 式内部への後置条件導入の防止
			if(!NodeKind::RubyStatementHost.Contains(Host)) return;
			ForEachNamedChild(
				Host,
				[&Collect](const TSNode Node) -> void {
					if(NodeKind::RubyModifierForm.Contains(Node)) Collect(Node);
				}
			);
		}
	);
	// 終了
	return;
}

/**
 * Ruby 修飾子形式への変換の適用関数（紐付が生きた状態で単独ラウンドとして走らせる）
 * @param Src ソースコード（破壊的に書き換える）
 * @param Language 対象言語
 * @return 変換を行ったか
 */
bool EditPass::ApplyRubyModifierForm(TSSource &Src, const Lang Language) {
	// 独立巡での修飾子形式化と型変更後コメントの引継
	if(Language != Lang::Ruby || !Src.IsParsed()) return false;
	// 置換編集とコメント引継情報の収集
	std::vector<TextEdit> ModifierEdits;
	std::vector<DeclEdit::PostEditAttach> ModifierAttaches;
	CollectRubyModifierFormEdits(Src, ModifierEdits, ModifierAttaches);
	// 収集結果の適用と変更有無の保持
	const bool IsApplied = !ModifierEdits.empty();
	DeclEdit::ApplyByteRebind(Src, ModifierEdits, ModifierAttaches);
	// 変換を行ったかの返戻（呼出側が構造の組直を掛け直す判断に使う）
	return IsApplied;
}
