#include "Edit.hpp"
#include "../Util/NodeKind.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * Python の行継続の結合関数
 * 括弧除去より前に済ませ，行継続の改行を括弧の内側の改行と読んで括弧を残す判断（次の巡で漸く外れる）を防ぐ
 * 計算量：原稿の長さ N に対し O(N)（行継続の位置を１度の走査で集める）
 * @param Src 対象のソース（Python, 解析済である事を前提とする）
 */
void EditPass::ApplyLineContinuations(TSSource &Src) {
	// 行継続編集列と結合処理の準備
	std::vector<TextEdit> Edits;
	const auto PushJoin = [&Src, &Edits](const uint32_t Begin, const uint32_t End) -> void {
		// 行継続直前の非空白位置
		const uint32_t Left = TextEdit::SkipSpLeft(Src, Begin);
		const bool IsLineHead = !Left || Src[Left - 1] == '\n';
		TextEdit::Push(
			IsLineHead ? Begin : Left,
			IsLineHead && Left == Begin ? End : TextEdit::SkipSpRight(Src, End),
			IsLineHead ? "" : " ",
			Edits
		);
	};
	const char *const Text = Src.data();
	const TSNode Root = Src.GetRoot();
	uint32_t PrevEnd = Src.Start(Root);
	// 構文木の葉と字句間隙に在る行継続の収集
	WalkChildrenCursor(
		Root,
		// 現在の葉節点の処理
		[&](const TSNode Current) -> bool {
			// 現在節点の型
			const std::string_view Type = ts_node_type(Current);
			// 葉でない構文は内側へ降りる事の返戻
			if(ts_node_child_count(Current) && !NodeKind::StringLikeInnerPreserve.Contains(Type)) return true;
			const uint32_t Start = Src.Start(Current);
			// 字句間の隙間から木に現れない行継続の収集
			for(uint32_t Pos = PrevEnd; Pos < Start;) {
				const void *const Found = std::memchr(Text + Pos, '\\', Start - Pos);
				// 逆斜線が無ければ間隙走査終了
				if(!Found) break;
				Pos = static_cast<uint32_t>(static_cast<const char *>(Found) - Text) + 1;
				// 改行を伴う逆斜線の結合
				if(Pos < Start && Text[Pos] == '\n') PushJoin(Pos - 1, Pos + 1);
			}
			// 木が識別した行継続も同じ結合処理への引渡し
			if(Type == "line_continuation") PushJoin(Start, Src.End(Current));
			// 走査済範囲の終端を更新
			PrevEnd = std::max(PrevEnd, Src.End(Current));
			// 葉の内側へは降りない事の返戻
			return false;
		}
	);
	TextEdit::Apply(Src, Edits);
	// 終了
	return;
}

/**
 * Ruby の局所変数の導入の索引の構築関数
 * Ruby は代入を解析した時点から名前を局所変数と読み，其れ迄は同じ名前をメソッドの呼出と読む
 * スコープ毎に名前の最初の導入位置（代入の左辺・仮引数の名前の終わり）を１回の走査で集め，対象の節点毎に導入の見えるスコープを控える
 * 読み切れない形（ネストの分割代入等）は導入して居ないと看做し，呼出側を保守側へ倒す
 * @param Src 整形対象ソース
 * @param Targets 導入の見えるスコープを控える節点の種別
 * @return 導入の索引
 */
EditPass::RubyLocals EditPass::IndexRubyLocals(const TSSource &Src, const NodeKind::NodeTypeSet &Targets) {
	// 構築する Ruby 局所変数索引
	RubyLocals Locals;
	std::vector<std::pair<const void *, bool>> Scopes;
	std::vector<bool> Opened;
	const auto Record = [&](const TSNode Identifier) -> void {
		// 条件成立時の返戻
		if(ts_node_is_null(Identifier) || std::string_view(ts_node_type(Identifier)) != "identifier" || Scopes.empty()) return;
		// 再代入による最初の導入位置の上書防止
		Locals.First[Scopes.back().first].try_emplace(Src.View(Identifier), Src.End(Identifier));
	};
	// 節点進入時の索引更新関数
	const auto Enter = [&](const TSNode Node) -> void {
		const std::string_view Type(ts_node_type(Node));
		// Ruby 仮引数列の場合
		if(NodeKind::RubyParameters.Contains(Type)) {
			// 仮引数は名前か，名前を先頭の子に持つ節点（`b = 1` / `*c` 等）
			ForEachNamedChild(
				Node,
				// 各仮引数の識別子抽出
				[&Record](const TSNode Parameter) -> void {
					// 包装を剥がした仮引数名の記録
					Record(std::string_view(ts_node_type(Parameter)) == "identifier" ? Parameter : ts_node_named_child(Parameter, 0));
				}
			);
		} else if(
			// 代入左辺となる束縛節点
			const TSNode Left = NodeKind::RubyBinding.Contains(Type) ? TSSource::FieldChild(Node, "left") : TSNode{};
			!ts_node_is_null(Left)
		) {
			// 分割代入は並びの直下の名前
			if(std::string_view(ts_node_type(Left)) == "left_assignment_list") ForEachNamedChild(Left, Record);
			else Record(Left);
		}
		// 索引対象となる候補節点の場合
		if(Targets.Contains(Type)) {
			std::vector<const void *> &Chain = Locals.Chains[Node.id];
			// 内側から外側へのスコープ走査
			for(size_t Idx = Scopes.size(); Idx--;) {
				Chain.push_back(Scopes[Idx].first);
				if(Scopes[Idx].second) break;
			}
		}
		// 子の導入記録前の有効範囲開始
		const bool IsScope = NodeKind::RubyLocalScope.Contains(Type);
		// スコープ経路へ境界を追加
		if(IsScope) Scopes.emplace_back(Node.id, NodeKind::RubyFreshScope.Contains(Type));
		// 節点のスコープ開始有無を記録
		Opened.push_back(IsScope);
		// 終了
		return;
	};
	const auto Leave = [&]() -> void {
		// 開始したスコープと節点印の除去
		if(Opened.back()) Scopes.pop_back();
		Opened.pop_back();
		// 終了
		return;
	};
	// 深さ優先走査カーソルと根の有効範囲の初期化
	HeldCursor Cursor(Src.GetRoot());
	Enter(ts_tree_cursor_current_node(&Cursor));
	// 構文木の深さ優先走査
	while(true) {
		// 最初の子へ降りれる場合
		if(ts_tree_cursor_goto_first_child(&Cursor)) {
			Enter(ts_tree_cursor_current_node(&Cursor));
			continue;
		}
		// 兄弟又は親を探す退出走査
		bool IsDone = false;
		while(true) {
			// 現在節点の退出処理
			Leave();
			if(ts_tree_cursor_goto_next_sibling(&Cursor)) {
				Enter(ts_tree_cursor_current_node(&Cursor));
				break;
			}
			// 親へ戻れない場合
			if(!ts_tree_cursor_goto_parent(&Cursor)) {
				IsDone = true;
				// 祖先復帰の終了
				break;
			}
		}
		// 完了時に外側走査を終了
		if(IsDone) break;
	}
	// 構築した Ruby 局所変数索引の返戻
	return Locals;
}

/**
 * Ruby の局所変数の導入済判定関数（索引の対象の節点の手前で，名前が局所変数に為って居るか）
 * @param Node 索引の対象の節点
 * @param Start 判定位置
 * @param Name 局所変数の名前
 * @return 同じスコープと外を見るブロックの，判定位置の手前の代入・仮引数で導入済なら true
 */
bool EditPass::RubyLocals::IsDefined(const TSNode Node, const uint32_t Start, const std::string_view Name) const {
	const std::unordered_map<const void *, std::vector<const void *>>::const_iterator Chain = Chains.find(Node.id);
	// 索引を持たない節点の未定義返戻
	if(Chain == Chains.end()) return false;
	// 参照可能な有効範囲だけで名前の導入位置を照合
	for(const void *const Scope : Chain->second) {
		const std::unordered_map<const void *, std::unordered_map<std::string_view, uint32_t>>::const_iterator Names =
		// スコープ別の名前導入索引
		First.find(Scope);
		// 名前を持たないスコープを読飛ばし
		if(Names == First.end()) continue;
		if(
			// 対象名の最初の導入位置
			const std::unordered_map<std::string_view, uint32_t>::const_iterator Found = Names->second.find(Name);
			Found != Names->second.end() && Found->second <= Start
			// 手前で導入済の返戻
		) return true;
	}
	// 全スコープで未定義の返戻
	return false;
}

/**
 * Ruby の曖昧な実引数の読みの統一関数
 * Ruby の空白で実引数に為る式を構文木へ反映する
 * tree-sitter-ruby は其の一部を二項式・添字・範囲・受信子と読み，整形で空白を揃えると読みが変わる為，他の編集より前に読みを Ruby へ揃える
 * 必須の括弧は実引数列全体へ移し，内側の整理を後段へ委ねる
 * @param Src ソースコード
 */
void EditPass::ApplyRubyCommandArguments(TSSource &Src) {
	const auto IsBlank = [&Src](const uint32_t Pos) -> bool {
		// 指定位置が空白かを返戻
		return Pos < Src.size() && (Src[Pos] == ' ' || Src[Pos] == '\t' || Src[Pos] == '\n');
	};
	const auto LeadsAmbiguously = [&Src](TSNode Lead) -> bool {
		// 実引数先頭の葉まで降下して字面を取得
		while(!ts_node_is_null(Lead) && ts_node_child_count(Lead)) {
			if(
				std::string_view(ts_node_type(Lead)) == "unary" && Src.View(ts_node_child(Lead, 0)) == "-" &&
				!Src.AttachesRubySign(ts_node_named_child(Lead, 0))
				// 空白を挟んで残る符号の返戻
			) return true;
			// 括弧を透過して最左子へ移動
			Lead = NodeKind::GroupingParen.Contains(Lead) ? ts_node_named_child(Lead, 0) : ts_node_child(Lead, 0);
		}
		const std::string_view Text = ts_node_is_null(Lead) ? std::string_view() : Src.View(Lead);
		// 単項加算又は範囲演算子かを返戻
		return Text.starts_with('+') || NodeKind::RangeOperator.Contains(Text);
	};
	const auto ArgumentTop = [&Src](TSNode Top) -> TSNode {
		// 所有する最上位の実引数節点への遡及
		for(
			// 一段上の候補節点
			TSNode Up = ts_node_parent(Top);
			!ts_node_is_null(Up) && NodeKind::RubyArgumentExtent.Contains(Up);
			Up = ts_node_parent(Up)
		) {
			if(
				!ts_node_eq(ts_node_named_child(Up, 0), Top) || NodeKind::RubyLowBinary.Contains(Src.View(TSSource::FieldChild(Up, "operator")))
				// 所有範囲を越える祖先で探索終了
			) break;
			// 実引数所有元を一段上へ更新
			Top = Up;
		}
		// 最上位の実引数節点を返戻
		return Top;
	};
	const auto ArgumentEnd = [&Src, &ArgumentTop](const TSNode Node) -> uint32_t {
		// 最上位引数と所属引数列の取得
		const TSNode Top = ArgumentTop(Node), List = ts_node_parent(Top);
		if(!ts_node_is_null(List) && NodeKind::RubyArgumentSequence.Contains(List)) {
			// 実引数列の最終引数終端を返戻
			return Src.End(ts_node_named_child(List, ts_node_named_child_count(List) - 1));
		}
		// 単独引数の終端を返戻
		return Src.End(Top);
	};
	const auto Wrap = [](std::vector<TextEdit> &Into, const uint32_t From, const uint32_t To, const uint32_t End) -> void {
		// 実引数前への開き括弧挿入
		TextEdit::Push(From, To, "(", Into);
		TextEdit::Push(End, End, ")", Into);
	};
	// Ruby の終端無範囲の次行を空白で繋ぎ，解析器の別文誤読を補修する（閉じ字句は除外）
	if(Src.IsParsed()) {
		std::vector<TextEdit> Folds;
		WalkAst(
			Src.GetRoot(),
			// 二項式候補の処理
			[&](const TSNode Node) -> void {
				// 条件成立時の返戻
				if(std::string_view(ts_node_type(Node)) != "range") return;
				// 式末尾の演算子節点
				const TSNode Operator = ts_node_child(Node, ts_node_child_count(Node) - 1);
				// 条件成立時の返戻
				if(ts_node_is_named(Operator) || !NodeKind::RangeOperator.Contains(Src.View(Operator))) return;
				const uint32_t After = TextEdit::SkipSpRight(Src, Src.End(Operator));
				// 条件成立時の返戻
				if(After >= Src.size() || Src[After] != '\n') return;
				// 次の実引数開始候補
				uint32_t Next = After;
				// 空白を越えて実引数先頭へ移動
				while(Next < Src.size() && (Src[Next] == '\n' || Src[Next] == ' ' || Src[Next] == '\t')) ++Next;
				// 条件成立時の返戻
				if(Next >= Src.size() || std::string_view(")]};#").find(Src[Next]) != std::string_view::npos) return;
				uint32_t WordEnd = Next;
				// 実引数先頭語の終端まで走査
				while(WordEnd < Src.size() && IsIdentifierChar(Src[WordEnd])) ++WordEnd;
				// 条件成立時の返戻
				if(NodeKind::RubyClauseWord.Contains(std::string_view(Src.data() + Next, WordEnd - Next))) return;
				// 演算子後の改行を空白へ置換
				TextEdit::Push(Src.End(Operator), Next, " ", Folds);
			}
		);
		// 実引数補修前の範囲式改行と構文木の整合
		if(!Folds.empty()) TextEdit::Apply(Src, Folds);
	}
	// 本文を誤編集しない様，ヒアドキュメントの開始だけを包んで誤読解消迄再解析
	while(true) {
		const RubyLocals Locals = IndexRubyLocals(Src, NodeKind::RubyCommandArgumentHost);
		std::vector<TextEdit> Edits, Heredoc;
		WalkAst(
			Src.GetRoot(),
			// Ruby 式候補の処理
			[&](const TSNode Node) -> void {
				// 候補節点の型
				const std::string_view Type(ts_node_type(Node));
				// 条件成立時の返戻
				if(!NodeKind::RubyCommandArgumentHost.Contains(Type)) return;
				// 括弧付実引数を持つ候補の場合
				if(NodeKind::RubyArgumentListHost.Contains(Type)) {
					const bool IsKeyword = NodeKind::RubyArgumentKeyword.Contains(Type);
					const TSNode Args = Type == "call" ? TSSource::FieldChild(Node, "arguments") : ts_node_named_child(Node, 0);
					// 条件成立時の返戻
					if(ts_node_is_null(Args) || std::string_view(ts_node_type(Args)) != "argument_list" || !ts_node_named_child_count(Args)) return;
					// 実引数列の子数と範囲
					const uint32_t Count = ts_node_named_child_count(Args), Begin = Src.Start(Args), End = Src.End(Args);
					const TSNode First = ts_node_named_child(Args, 0);
					// 明示括弧を持つ実引数列の場合
					if(Src.Start(First) != Begin) {
						// return の最初の実引数の括弧を外し，曖昧な後続は共に包む（改行付は保持）
						if(!IsKeyword || Count != 1 || std::memchr(Src.data() + Begin, '\n', End - Begin)) return;
						// 括弧除去後も先頭が曖昧でない場合
						if(!LeadsAmbiguously(First)) {
							TextEdit::Push(Begin, Begin + 1, " ", Edits);
							TextEdit::Push(End - 1, End, "", Edits);
							// 外側の呼出範囲を括弧で保護
						} else if(const TSNode Top = ArgumentTop(Node); !ts_node_eq(Top, Node)) Wrap(Edits, Begin, Begin, Src.End(Top));
						// 終了
						return;
					}
					const TSNode Method = Type == "call" ? TSSource::FieldChild(Node, "method") : TSNode{};
					const TSNode Block = Type == "call" ? TSSource::FieldChild(Node, "block") : TSNode{};
					const bool IsLocalHead = !ts_node_is_null(Method) && ts_node_is_null(TSSource::FieldChild(Node, "receiver")) &&
					std::string_view(ts_node_type(Method)) == "identifier" && Locals.IsDefined(Node, Src.Start(Node), Src.View(Method));
					// 親の子の１回走査による前兄弟の取得（根からの引直の二乗費用を回避）
					TSNode Name {}, Prev {};
					ForEachChild(
						Node,
						// 実引数直前の呼出名を探索
						[&](const TSNode Child) -> bool {
							// 実引数到達時の直前子記録と現在子への更新
							if(ts_node_eq(Child, Args)) Name = Prev;
							Prev = Child;
							// 実引数の並びへ届いたら走査を打ち切る事の返戻
							return !ts_node_eq(Child, Args);
						}
					);
					// 直前の子が無い場合は，空節点の終端 0 から原稿を潰さない様に除外
					if(ts_node_is_null(Name)) return;
					if(
						Src[Begin] == '(' && (IsLocalHead || LeadsAmbiguously(First)) || !ts_node_is_null(Block) && Src[Src.Start(Block)] == '{' &&
						NodeKind::GroupingParen.Contains(ts_node_named_child(Args, Count - 1))
						// 裸の実引数列を括弧で保護
					) Wrap(Edits, Src.End(Name), Begin, End);
					// return 前置でのラベル誤認防止用空白の挿入
					else if(Src[Begin] == ':' && !ts_node_is_null(Name) && Src.End(Name) == Begin) TextEdit::Push(Begin, Begin, " ", Edits);
					// 終了
					return;
				}
				const TSNode Head = ts_node_named_child(Node, 0);
				TSNode Next {};
				// 二項式以外の後続子を探索
				if(Type != "binary" && !ts_node_is_null(Head)) {
					// 先頭通過済の印
					bool IsAfterHead = false;
					ForEachChild(
						Node,
						// 各子の先頭相対位置を確認
						[&](const TSNode Child) -> void {
							// 先頭通過状態と直後の子の記録
							if(IsAfterHead && ts_node_is_null(Next)) Next = Child;
							if(ts_node_eq(Child, Head)) IsAfterHead = true;
						}
					);
				}
				const TSNode Operator = Type == "binary" ? TSSource::FieldChild(Node, "operator") : Next;
				// 条件成立時の返戻
				if(ts_node_is_null(Head) || ts_node_is_null(Operator)) return;
				// 先頭型と演算子の字面
				const std::string_view HeadType(ts_node_type(Head)), OperatorText = Src.View(Operator);
				// 実引数開始キーワードの場合
				if(NodeKind::RubyArgumentKeyword.Contains(HeadType)) {
					// return 後の実引数の補修（+・範囲・数値始まりは括弧化し，他は符号を密着）
					if(ts_node_named_child_count(Head)) return;
					if(
						const TSNode Right = Type == "binary" ? TSSource::FieldChild(Node, "right") : TSNode{};
						OperatorText == "+" || Type == "range" || OperatorText == "-" && !Src.AttachesRubySign(Right)
						// 曖昧なキーワード引数を括弧で保護
					) Wrap(Edits, Src.End(Head), Src.Start(Operator), ArgumentEnd(Node));
					// Ruby 実引数開始演算子の場合
					else if(NodeKind::RubyArgumentStart.Contains(OperatorText)) {
						// 呼出名と演算子の間を単一空白化
						TextEdit::Push(Src.End(Head), Src.Start(Operator), " ", Edits);
						// 演算子と右辺を密着
						if(!ts_node_is_null(Right)) TextEdit::Push(Src.End(Operator), Src.Start(Right), "", Edits);
					}
					// 終了
					return;
				}
				// 名前の後に空白が有り，演算子の直後に空白が無い（添字の `[` は前の空白だけで実引数に為る）
				if(
					!NodeKind::RubyArgumentStart.Contains(OperatorText) || !IsBlank(Src.Start(Operator) - 1) ||
					Type == "binary" && IsBlank(Src.End(Operator))
					// 条件成立時の返戻
				) return;
				// 実引数を取るのは局所変数でない名前か，実引数の無いメソッドの呼出 (`foo.bar +1`)・`yield`・`super`
				if(
					const bool IsMethod = HeadType == "identifier" ? !Locals.IsDefined(Node, Src.Start(Node), Src.View(Head)) : HeadType == "call" ?
					ts_node_is_null(TSSource::FieldChild(Head, "arguments")) && ts_node_is_null(TSSource::FieldChild(Head, "block")) :
					HeadType == "super" || HeadType == "yield" && !ts_node_named_child_count(Head);
					!IsMethod
					// 条件成立時の返戻
				) return;
				// シフト以外の曖昧な実引数を括弧で保護
				if(OperatorText != "<<") Wrap(Edits, Src.End(Head), Src.Start(Operator), ArgumentEnd(Node));
				// ヒアドキュメントを伴うシフト式を別編集へ収集
				else if(Heredoc.empty()) Wrap(Heredoc, Src.End(Head), Src.Start(Operator), Src.End(ArgumentTop(Node)));
			}
		);
		// ヒアドキュメントの誤読が無くなってから通常の編集を適用
		if(Heredoc.empty()) {
			TextEdit::Apply(Src, Edits);
			// 終了
			return;
		}
		// 本文の範囲を読み直してから残る実引数を判定
		TextEdit::Apply(Src, Heredoc);
	}
}

/**
 * Kotlin の改行を跨ぐ継ぎ目の区切り編集の収集関数
 * Kotlin で文境界を跨ぐ誤結合を防ぐセミコロン補完関数
 * @param Src 整形対象ソース
 * @param Node 呼出の接尾辞・参照・返戻のノード
 * @param Edits 編集列追加先
 */
void EditPass::CollectKotlinJointEdit(const TSSource &Src, const TSNode Node, std::vector<TextEdit> &Edits) {
	uint32_t Joint = Src.Start(Node);
	const std::string_view Type(ts_node_type(Node));
	TSNode Lead {};
	// 改行後の `.` / `?.` は連鎖を継続し，`::` だけは別文の参照
	if(Type == "navigation_suffix" && Src.View(ts_node_child(Node, 0)) != "::") return;
	// 呼出可能参照の場合
	else if(Type == "callable_reference") {
		// 受け手の無い参照と直前式の非接続
		TSNode Colons {};
		bool IsReceiver = false;
		ForEachChild(
			Node,
			// 呼出可能参照の各子を走査
			[&](const TSNode Child) -> bool {
				// `::` トークン又は名前付受け手の検出
				if(!ts_node_is_named(Child) && Src.View(Child) == "::") {
					Colons = Child;
					// `::` の発見で打ち切る事の返戻
					return false;
				}
				// 名前付の受け手通過を記録
				IsReceiver = IsReceiver || ts_node_is_named(Child);
				// 次の子へ進む事の返戻
				return true;
			}
		);
		// 受け手の無い参照の返戻
		if(!IsReceiver || ts_node_is_null(Colons)) return;
		// 継ぎ目を `::` の開始へ更新
		Joint = Src.Start(Colons);
		// ジャンプ式の場合
	} else if(Type == "jump_expression") {
		const std::string_view Keyword = Src.View(ts_node_child(Node, 0));
		const TSNode Value = ts_node_named_child(Node, ts_node_named_child_count(Node) - 1);
		// 値を返さない事の返戻
		if(Keyword != "return" && Keyword != "return@" || ts_node_is_null(Value) || std::string_view(ts_node_type(Value)) == "label") {
			// 終了
			return;
		}
		// 継ぎ目を返戻値の開始へ更新
		Joint = Src.Start(Value);
		// 返戻値を先頭式候補へ設定
		Lead = Value;
		// 中置式の場合
	} else if(Type == "infix_expression") {
		// Kotlin の改行後の単独呼出の文分離（裸の右辺は元から文でない為に除外）
		const TSNode Name = ts_node_named_child(Node, 1), Right = ts_node_named_child(Node, 2);
		// 呼出の形でない物の返戻
		if(ts_node_is_null(Name) || ts_node_is_null(Right) || Src[Src.Start(Right)] != '(' && Src[Src.Start(Right)] != '{') return;
		// 継ぎ目を中置演算子名へ更新
		Joint = Src.Start(Name);
	} else {
		const TSNode Arguments = ts_node_named_child(Node, 0);
		if(
			ts_node_is_null(Arguments) || std::string_view(ts_node_type(Arguments)) != "value_arguments" ||
			ts_node_named_child_count(Arguments) != 1 || ts_node_child_count(ts_node_named_child(Arguments, 0)) != 1
			// 条件成立時の返戻
		) return;
		// 実引数を先頭式候補へ設定
		Lead = Arguments;
	}
	// 継ぎ目の空白遡及による文末改行の確認
	bool HasNewline = false;
	// 継ぎ目前の空白を後退走査
	for(; Joint && (Src[Joint - 1] == ' ' || Src[Joint - 1] == '\t' || Src[Joint - 1] == '\n'); --Joint) {
		// 改行の発見を記録
		if(Src[Joint - 1] == '\n') HasNewline = true;
	}
	// 条件成立時の返戻
	if(!HasNewline) return;
	// 文の並びだけで改行を区切にする（実引数・条件・添字等の括弧内と when の条件は除外）
	for(TSNode Cur = Node, Parent = ts_node_parent(Node); !ts_node_is_null(Parent); Cur = Parent, Parent = ts_node_parent(Parent)) {
		const std::string_view Type = ts_node_type(Parent);
		// 文境界の祖先で探索終了
		if(NodeKind::KotlinStatementContext.Contains(Type)) break;
		// 条件成立時の返戻
		if(Type == "when_condition") return;
		int Depth = 0;
		const uint32_t CurStart = Src.Start(Cur);
		ForEachChild(
			Parent,
			// 対象子までの括弧深さ集計
			[&Src, &Depth, CurStart](const TSNode Child) -> bool {
				// Cur に達した事の返戻
				if(Src.Start(Child) >= CurStart) return false;
				// 括弧字句に応じた深さ更新
				if(
					// 現在子の無名トークン
					const std::string_view Token = ts_node_is_named(Child) ? std::string_view{} : Src.View(Child);
					Token == "(" || Token == "["
					// 開き括弧で深さを加算
				) ++Depth;
				// 閉じ括弧で深さを減算
				else if(Token == ")" || Token == "]") --Depth;
				// 次の子へ進む事の返戻
				return true;
			}
		);
		// 条件成立時の返戻
		if(Depth > 0) return;
	}
	TextEdit::Push(Joint, Joint, ";", Edits);
	// tree-sitter-kotlin が読めない文頭の冗長括弧の除去
	while(!ts_node_is_null(Lead) && ts_node_child_count(Lead) && Src.View(ts_node_child(Lead, 0)) != "(") {
		// 先頭子へ移動
		Lead = ts_node_child(Lead, 0);
	}
	// 条件成立時の返戻
	if(ts_node_is_null(Lead) || ts_node_named_child_count(Lead) != 1) return;
	// 括弧省略候補の実引数名
	TSNode Name = ts_node_named_child(Lead, 0);
	// 値引数包装を剥離
	if(std::string_view(ts_node_type(Name)) == "value_argument") Name = ts_node_named_child(Name, 0);
	// 単純識別子の単独実引数の場合
	if(!ts_node_is_null(Name) && std::string_view(ts_node_type(Name)) == "simple_identifier") {
		const TSNode Open = ts_node_child(Lead, 0), Close = ts_node_child(Lead, ts_node_child_count(Lead) - 1);
		TextEdit::Push(Src.Start(Open), Src.End(Open), "", Edits);
		TextEdit::Push(Src.Start(Close), Src.End(Close), "", Edits);
	}
	// 終了
	return;
}

/**
 * Kotlin の改行を跨ぐ継ぎ目の区切り関数
 * tree-sitter-kotlin は改行の後の呼出・添字・参照を前の式へ繋ぐが，Kotlin は改行で文を終える為，継ぎ目の前へ `;` を補って読み直させる
 * 他の編集（括弧除去等）が誤って繋いだ木を元に判断すると，区切った後の巡で結果が変わり冪等性が崩れる為，其れ等より前に単独で行う
 * @param Src ソースコード
 */
void EditPass::ApplyKotlinJoints(TSSource &Src) {
	// Kotlin 継ぎ目の編集列
	std::vector<TextEdit> Edits;
	WalkAst(
		Src.GetRoot(),
		// Kotlin 節点候補の処理
		[&](const TSNode Node) -> void {
			// 継ぎ目対象接尾辞の編集収集
			if(NodeKind::KotlinJointSuffix.Contains(Node)) CollectKotlinJointEdit(Src, Node, Edits);
		}
	);
	TextEdit::Apply(Src, Edits);
	// 終了
	return;
}

/**
 * Kotlin の自動の `;` の補修関数
 * @param Src ソースコード
 */
void EditPass::ApplyKotlinSemicolons(TSSource &Src) {
	// Kotlin 原文への参照
	const std::string &Source = Src;
	const bool HasBlockComment = Source.find("/*") != std::string::npos;
	// 囲みコメントも解析の誤りも無いソースの終了
	if(!HasBlockComment && !ts_node_has_error(Src.GetRoot())) return;
	std::vector<uint32_t> Inserts;
	uint32_t PrevEnd = static_cast<uint32_t>(Source.size()), CodeEnd = PrevEnd;
	WalkChildrenCursor(
		Src.GetRoot(),
		// Kotlin 節点の処理
		[&](const TSNode Node) -> bool {
			// 文字列は閉じの引用符が字句に為らず，中の字句の終わりが文字列の中に来る為，全体を１つの字句としての取扱
			if(std::string_view(ts_node_type(Node)) != "string_literal" && ts_node_child_count(Node)) {
				// 破損したクラス本体の場合
				if(ts_node_has_error(Node) && NodeKind::ClassBodyContainer.Contains(Node)) {
					// クラス本体の閉じトークン
					const TSNode Close = ts_node_child(Node, ts_node_child_count(Node) - 1);
					TSNode Last = ts_node_prev_sibling(Close);
					// 末尾コメントを越えて最後のコードへ移動
					while(!ts_node_is_null(Last) && NodeKind::Comment.Contains(Last)) Last = ts_node_prev_sibling(Last);
					// 健全な成員末尾だけの区切補完（改行と ; が無い時，誤った成員は保持）
					if(
						!ts_node_is_null(Last) && Src.View(Close) == "}" && Src.View(Last) != "{" &&
						std::string_view(Source).substr(Src.End(Last), Src.Start(Close) - Src.End(Last)).find_first_of(";\n") ==
						std::string_view::npos && !HasChildOf(
							Node,
							[](const TSNode Child) -> bool {
								const auto IsBroken = [](const TSNode Member) -> bool {
									// 成員でない名前付の子か，誤りを含む成員かの返戻
									return ts_node_is_named(Member) && !NodeKind::Comment.Contains(Member) &&
									(!NodeKind::KotlinClassMember.Contains(Member) || ts_node_has_error(Member));
								};
								// 健全な成員を包まない `ERROR` か，壊れた子かの返戻
								return std::string_view(ts_node_type(Child)) == "ERROR" ?
								!ts_node_named_child_count(Child) || HasChildOf(Child, IsBroken) :
								IsBroken(Child);
							}
						)
					) {
						// 破損メンバ後の挿入位置を記録
						Inserts.push_back(Src.End(Last));
					}
				}
				// 字句でない節点は子へ降りる事の返戻
				return true;
			}
			// 隙間の後のコメント節点の確認（幅０の節点では本文が隙間に残る）
			const uint32_t Start = ts_node_start_byte(Node);
			const bool IsComment = NodeKind::Comment.Contains(Node);
			if(
				HasBlockComment && PrevEnd < Start && !IsComment &&
				std::string_view(Source).substr(PrevEnd, Start - PrevEnd).find("/*") != std::string_view::npos
				// コメント直前のコード終端を挿入候補化
			) Inserts.push_back(CodeEnd);
			PrevEnd = ts_node_end_byte(Node);
			// コード節点ならコード終端も更新
			if(!IsComment) CodeEnd = PrevEnd;
			// 字句の終了
			return false;
		}
	);
	// 条件成立時の返戻
	if(Inserts.empty()) return;
	std::sort(Inserts.begin(), Inserts.end());
	// 重複した挿入位置を除去
	Inserts.erase(std::unique(Inserts.begin(), Inserts.end()), Inserts.end());
	// セミコロン挿入編集列
	std::vector<TextEdit> Edits;
	// 挿入位置数の領域を予約
	Edits.reserve(Inserts.size());
	// 各候補位置へセミコロンを挿入
	for(const uint32_t Pos : Inserts) Edits.push_back({ Pos, Pos, ";" });
	TextEdit::Apply(Src, Edits);
	// 補完後も読めない区間からの残留 `;` の除去
	if(!Src.IsParsed() || !ts_node_has_error(Src.GetRoot())) return;
	// 誤りを増した挿入の取消編集列
	std::vector<TextEdit> Undo;
	// 各挿入位置の再解析結果を確認
	for(size_t Idx = 0; Idx < Inserts.size(); ++Idx) {
		const uint32_t Pos = Inserts[Idx] + static_cast<uint32_t>(Idx);
		for(
			// 挿入位置を含む最小節点
			TSNode Node = ts_node_descendant_for_byte_range(Src.GetRoot(), Pos, Pos + 1);
			!ts_node_is_null(Node);
			Node = ts_node_parent(Node)
		) {
			// `ERROR` 以外の祖先なら次候補へ移動
			if(std::string_view(ts_node_type(Node)) != "ERROR") continue;
			// 誤りを生んだセミコロンの除去編集
			Undo.push_back({ Pos, Pos + 1, "" });
			// 該当位置の祖先探索終了
			break;
		}
	}
	TextEdit::Apply(Src, Undo);
	// 終了
	return;
}

/**
 * JS/TS の改行を跨ぐ継ぎ目の読みの統一関数
 * @param Src ソースコード
 */
void EditPass::ApplyJsJoints(TSSource &Src) {
	// 対象字句の有無と編集状態の準備
	const std::string &Source = Src;
	const bool HasLet = Source.find("let") != std::string::npos;
	// 条件成立時の返戻
	if(!HasLet && Source.find("class") == std::string::npos) return;
	// JS 継ぎ目補修の編集列
	std::vector<TextEdit> Edits;
	std::string Moved, InlineText;
	const auto Collect = [&](const TSNode Comment) -> void {
		// コメント種別に応じた改行又は空白付の蓄積
		if(const std::string_view Text = Src.View(Comment); Text.starts_with("//") || Text.find('\n') != std::string_view::npos) {
			Moved.append(Text).push_back('\n');
		} else InlineText.append(Text).push_back(' ');
	};
	const auto Join = [&](const TSNode Head, const TSNode Word, const TSNode Next) -> void {
		// 独立行コメントを見出前へ移動
		if(!Moved.empty()) TextEdit::Push(Src.Start(Head), Src.Start(Head), std::move(Moved), Edits);
		// 語と次節点の間を行内本文へ置換
		TextEdit::Push(Src.End(Word), Src.Start(Next), std::move(InlineText), Edits);
	};
	WalkAst(
		Src.GetRoot(),
		// JS 節点候補の処理
		[&](const TSNode Node) -> void {
			const std::string_view Type = ts_node_type(Node);
			// 前候補のコメント蓄積状態を初期化
			Moved.clear();
			InlineText = " ";
			// クラス本体の場合
			if(Type == "class_body") {
				ForEachNamedChild(
					Node,
					// クラスメンバ候補の処理
					[&](const TSNode Member) -> void {
						// 条件成立時の返戻
						if(ts_node_is_extra(Member)) return;
						// 改行前の修飾語を同名の欄へ補修（解析器が ; を読める様，名前を引用符で包む）
						const uint32_t Count = ts_node_child_count(Member);
						// メンバ末尾より前の子を走査
						for(uint32_t Index = 0; Index + 1 < Count; ++Index) {
							const TSNode Modifier = ts_node_child(Member, Index);
							const std::string_view Word = Src.View(Modifier);
							if(
								// 修飾子直後の間隙位置
								const uint32_t Gap = Src.End(Modifier);
								NodeKind::JsSeparatingModifier.Contains(Word) &&
								std::memchr(Source.data() + Gap, '\n', Src.Start(ts_node_child(Member, Index + 1)) - Gap)
								// 文字列と誤読された修飾子を復元
							) TextEdit::Push(Src.Start(Modifier), Gap, "\"" + std::string(Word) + "\";", Edits);
						}
						// 後続の成員へ掛かる修飾語だけの欄 (`static\nx = 1`) を後続への接続
						if(!NodeKind::JsJoiningModifier.Contains(Src.View(Member))) return;
						TSNode Next = ts_node_next_sibling(Member);
						// 間のコメントを回収しつつ次節点へ移動
						for(; !ts_node_is_null(Next) && ts_node_is_extra(Next); Next = ts_node_next_sibling(Next)) Collect(Next);
						// 次の名前付節点へ修飾子を接続
						if(!ts_node_is_null(Next) && ts_node_is_named(Next)) Join(Member, Member, Next);
						// 回収済の独立行コメントを破棄
						Moved.clear();
						// 行内間隙を次候補用に初期化
						InlineText = " ";
					}
				);
				// クラスの本体の子の走査の終了
				return;
			}
			// 条件成立時の返戻
			if(!HasLet || Type != "expression_statement") return;
			// 対象節点先頭のキーワード
			const TSNode Word = ts_node_child(Node, 0);
			// 条件成立時の返戻
			if(std::string_view(ts_node_type(Word)) != "identifier" || Src.View(Word) != "let") return;
			bool IsClosed = false;
			// キーワード後の兄弟走査
			for(TSNode Child = ts_node_next_sibling(Word); !ts_node_is_null(Child); Child = ts_node_next_sibling(Child)) {
				// 間のコメントを回収
				if(ts_node_is_extra(Child)) Collect(Child);
				// コードトークン通過を記録
				else IsClosed = true;
			}
			// 節点後の兄弟候補
			TSNode Next = ts_node_next_sibling(Node);
			// 間のコメントを回収しつつ次節点へ移動
			for(; !ts_node_is_null(Next) && ts_node_is_extra(Next); Next = ts_node_next_sibling(Next)) Collect(Next);
			// 閉じた let 文と後続の無い文の結合除外
			if(IsClosed || ts_node_is_null(Next)) return;
			TSNode Lead = Next;
			// 後続節点の最左葉へ降下
			while(ts_node_child_count(Lead)) Lead = ts_node_child(Lead, 0);
			// 識別子又は物体リテラル開始の場合
			if(const std::string_view LeadType = ts_node_type(Lead); ts_node_is_named(Lead) ? LeadType == "identifier" : LeadType == "{") {
				// キーワードと後続節点を接続
				Join(Node, Word, Next);
			}
		}
	);
	// JS 継ぎ目編集の適用
	TextEdit::Apply(Src, Edits);
	// 終了
	return;
}

/**
 * C# のキャストと誤読した二項式の読みの統一関数
 * @param Src ソースコード
 */
void EditPass::ApplyCSharpMisreadCasts(TSSource &Src) {
	const std::string_view Source = Src;
	bool Candidate = false;
	// 閉じ括弧後の演算子候補を検索
	for(size_t Pos = Source.find(')'); Pos != std::string_view::npos && !Candidate; Pos = Source.find(')', Pos + 1)) {
		// 閉じ括弧直後の走査位置
		size_t Next = Pos + 1;
		// 空白とコメントを越える走査
		while(Next < Source.size()) {
			// 空白文字を読飛ばし
			if(Source[Next] == ' ' || Source[Next] == '\t' || Source[Next] == '\n') ++Next;
			// 囲みコメントを読飛ばし
			else if(Source.substr(Next, 2) == "/*") Next = std::min(Source.find("*/", Next + 2), Source.size() - 2) + 2;
			else if(Source.substr(Next, 2) == "//") Next = std::min(Source.find('\n', Next), Source.size());
			// 最初のコード字句で探索終了
			else break;
		}
		// 前置演算子候補を記録
		Candidate = Next < Source.size() && std::string_view("-+*&^").find(Source[Next]) != std::string_view::npos;
	}
	// 誤読するキャストの無い場合の終了
	if(!Candidate) return;
	std::vector<TextEdit> Edits;
	WalkAst(
		Src.GetRoot(),
		// C# cast 式候補の処理
		[&](const TSNode Node) -> void {
			// 条件成立時の返戻
			if(std::string_view(ts_node_type(Node)) != "cast_expression") return;
			const TSNode Type = TSSource::FieldChild(Node, "type"), Value = TSSource::FieldChild(Node, "value");
			// 型又は前置単項式を欠く候補を除外
			if(ts_node_is_null(Type) || ts_node_is_null(Value) || std::string_view(ts_node_type(Value)) != "prefix_unary_expression") {
				// 終了
				return;
			}
			// 添字の付いた名前（大きさを持つ配列の型に読まれた `d[0]`）
			if(
				const bool IsIndexed = std::string_view(ts_node_type(Type)) == "array_type" && HasDescendantOf(
					Type,
					// 配列階数指定子の探索関数
					[](const TSNode Descendant) -> bool {
						// 要素を持つ配列階数指定子かを返戻
						return std::string_view(ts_node_type(Descendant)) == "array_rank_specifier" && ts_node_named_child_count(Descendant);
					}
				); !IsIndexed && (!NodeKind::CsNameType.Contains(Type) || Src.View(Type).back() == '>') // 条件成立時の返戻
			) return;
			// 値の前置の演算子が二項の演算子を兼ねなければ，本当のキャストの為に終了
			if(
				// 単項演算子の字面
				const std::string_view Operator = Src.View(ts_node_child(Value, 0));
				Operator.size() != 1 || std::string_view("-+*&^").find(Operator) == std::string_view::npos
				// 条件成立時の返戻
			) return;
			TSNode Close = ts_node_next_sibling(Type);
			// 名前付子を越えて閉じ括弧へ移動
			while(!ts_node_is_null(Close) && ts_node_is_named(Close)) Close = ts_node_next_sibling(Close);
			// 条件成立時の返戻
			if(ts_node_is_null(Close) || Src.View(Close) != ")") return;
			// cast 式の開き括弧
			const TSNode Open = ts_node_child(Node, 0);
			TextEdit::Push(Src.Start(Open), Src.End(Open), "", Edits);
			TextEdit::Push(Src.Start(Close), Src.End(Close), "", Edits);
		}
	);
	TextEdit::Apply(Src, Edits);
	// 終了
	return;
}

/**
 * 演算子を成す記号の判定関数（記号同士が接すると１字句へ繋がり，別の演算子に読まれる）
 * @param Char 文字
 * @return 演算子を成す記号なら true
 */
bool EditPass::IsOperatorChar(const char Char) {
	// 演算子を成す記号かの返戻
	return std::string_view("!%&*+-/:<=>?^|~").find(Char) != std::string_view::npos;
}

/**
 * Swift の前置の演算子の呼出と読んだ括弧の実引数の並びの取得関数
 * tree-sitter-swift は前置の演算子の後の括弧 (`-(y)` / `!(y)`) を演算子の呼出と読む
 * 否定の `!` は名前付の `bang` の節点に為り，中置の式の後の前置 (`x + -(y)`) は其の中置の式の末尾の字句に為る（`(x + -)(y)` と読む）
 * @param Src ソースコード
 * @param Node 対象ノード
 * @return 前置の演算子の被演算子を包む括弧と読んだ実引数の並び（其の形でなければ null）
 */
TSNode EditPass::SwiftPrefixArguments(const TSSource &Src, const TSNode Node) {
	// 前置の演算子の呼出の形でない事の返戻
	if(ts_node_child_count(Node) != 2 || std::string_view(ts_node_type(Node)) != "call_expression") return {};
	// 呼出側から前置演算子候補を抽出
	// 呼出側からの実際の前置演算子の取得
	TSNode Prefix = ts_node_child(Node, 0);
	if(ts_node_is_named(Prefix) && std::string_view(ts_node_type(Prefix)) != "bang") {
		// 中置式末尾の演算子候補への移動
		const uint32_t Count = ts_node_child_count(Prefix);
		Prefix = Count > 1 ? ts_node_child(Prefix, Count - 1) : TSNode{};
		// 末尾が演算子の字句でない事の返戻
		if(ts_node_is_null(Prefix) || ts_node_is_named(Prefix)) return {};
	}
	// 呼出接尾辞と実引数列の検査
	const TSNode Suffix = ts_node_child(Node, 1);
	if(
		!IsOperatorChar(Src[Src.Start(Prefix)]) || std::string_view(ts_node_type(Suffix)) != "call_suffix" ||
		ts_node_named_child_count(Suffix) != 1
		// 前置の演算子と実引数の並びの組でない事の返戻
	) return {};
	// 実引数節点の取得
	const TSNode Arguments = ts_node_named_child(Suffix, 0);
	// 実引数の並びの返戻
	return std::string_view(ts_node_type(Arguments)) == "value_arguments" ? Arguments : TSNode{};
}

/**
 * 見出の本体の始まりと紛れる式の判定関数
 * 制御見出で本体の波括弧と誤読される式の包含判定関数
 * 実引数・配列・添字・文字列等の区切りの中と，関数・クロージャの本体の中の物は紛れない（要素１つの括弧は外れ得る為に透過して見る）
 * @param Src ソースコード
 * @param Node 対象ノード
 * @param Language 対象言語
 * @return 紛れる式を持つなら true
 */
bool EditPass::HasBodyLikeExpression(const TSSource &Src, const TSNode Node, const Lang Language) {
	const auto BodyLike = [Language](const TSNode Target) -> bool {
		const std::string_view Type = ts_node_type(Target);
		// 言語毎の紛れる式かの返戻
		return Language == Lang::Go ?
		Type == "composite_literal" && NodeKind::GoTypeName.Contains(TSSource::FieldChild(Target, "type")) :
		Language == Lang::Rust ? Type == "struct_expression" : Type == "lambda_literal";
	};
	// 紛れる式其の物の返戻
	if(BodyLike(Node)) return true;
	// 子孫探索状態と Swift 括弧透過候補の準備
	bool IsFound = false;
	// Swift の前置の演算子の被演算子を包む括弧 (`!(x)`) は，演算子の呼出の実引数の並びと読まれても外れ得る為に透過
	std::vector<const void *> PrefixArguments;
	const auto NotePrefix = [&](const TSNode Call) -> void {
		if(const TSNode Arguments = SwiftPrefixArguments(Src, Call); !ts_node_is_null(Arguments)) {
			PrefixArguments.push_back(Arguments.id);
		}
	};
	// 根自身の前置呼出の括弧透過候補への追加
	if(Language == Lang::Swift) NotePrefix(Node);
	WalkChildrenCursor(
		Node,
		[&](const TSNode Child) -> bool {
			// 見付けた後に降りない事の返戻
			if(IsFound) return false;
			// 紛れる式の検出と探索打切
			if(BodyLike(Child)) {
				IsFound = true;
				// 見付けた為に降りない事の返戻
				return false;
			}
			// 子の型に応じた Swift 括弧候補の記録
			const std::string_view Type = ts_node_type(Child);
			if(Language == Lang::Swift && Type == "call_expression") NotePrefix(Child);
			// 区切り字句を持つ子の降下可否判定
			const TSNode First = ts_node_child(Child, 0);
			// 区切りの括弧・引用符の字句で始まる節点（要素１つの括弧を除く）の中へは降りない事の返戻
			return ts_node_is_null(First) || ts_node_is_named(First) ||
			std::string_view("\"#'`([{").find(Src[Src.Start(First)]) == std::string_view::npos || Type == "parenthesized_expression" ||
			Language == Lang::Swift && ts_node_named_child_count(Child) == 1 &&
			(Type == "tuple_expression" || std::find(PrefixArguments.begin(), PrefixArguments.end(), Child.id) != PrefixArguments.end());
		}
	);
	// 紛れる式を持つかの返戻
	return IsFound;
}

/**
 * Swift の解析器の読違の補正関数
 * 計算量：原稿の長さ N と直した数 K に対し O(N * K)（１つ直す毎に読み直す，K は手間の上限と期限で抑え，超過は見送る）
 * @param Src ソースコード
 * @return コメントの密着した演算子を上限の数迄で直し切れたか
 */
bool EditPass::ApplySwiftMisreads(TSSource &Src) {
	const auto OperatorEnd = [](const std::string_view Text, const uint32_t Start) -> uint32_t {
		// 演算子終端の初期化と連続記号走査
		uint32_t End = Start + 1;
		while(
			End < Text.size() &&
			(std::string_view("/=-+!*%<>&|^~?").find(Text[End]) != std::string_view::npos || Text[End] == '.' && Text[Start] == '.') &&
			Text.compare(End, 2, "//") && Text.compare(End, 2, "/*")
		) ++End;
		// 演算子の字の並びの終わりの返戻
		return End;
	};
	// From より後の演算子内の最初のコメント位置の取得（補修済の前方部分木は飛ばす）
	const auto FirstGluedComment = [](const std::string_view Text, const TSNode Root, const uint32_t From) -> uint32_t {
		// 走査カーソルと発見位置の初期化
		HeldCursor Cursor(Root);
		uint32_t Opener = 0;
		// From 以後の演算子節点の反復探索
		for(bool IsAdvanced = true; IsAdvanced;) {
			if(const TSNode Node = ts_tree_cursor_current_node(&Cursor); std::string_view(ts_node_type(Node)) == "custom_operator") {
				// 演算子内のコメント開始記号の検索
				const std::string_view Operator = Text.substr(ts_node_start_byte(Node), ts_node_end_byte(Node) - ts_node_start_byte(Node));
				size_t At = std::min(Operator.find("//"), Operator.find("/*"));
				if(!At && Operator.starts_with("//")) At = Operator.find_first_not_of('/');
				// 有効な開始位置の記録
				if(At && At != std::string_view::npos) {
					Opener = ts_node_start_byte(Node) + static_cast<uint32_t>(At);
					break;
				}
			}
			// From より後に終わる最初の子へ降り，降りれなければ次の兄弟か祖先の次の兄弟へ進む（無ければ走査を終える）
			if(ts_tree_cursor_goto_first_child_for_byte(&Cursor, From) != -1) continue;
			IsAdvanced = ts_tree_cursor_goto_next_sibling(&Cursor);
			while(!IsAdvanced && ts_tree_cursor_goto_parent(&Cursor)) IsAdvanced = ts_tree_cursor_goto_next_sibling(&Cursor);
		}
		// コメントの始まりの返戻
		return Opener;
	};
	// 密着コメント補修の実行
	if(uint32_t Opener = FirstGluedComment(Src, Src.GetRoot(), 0)) {
		// Swift の局所補修後に１回だけ全体再解析（読直総量 1 MiB を超えれば見送る）
		static constexpr size_t GluedCommentBudget = 1 << 20;
		const std::chrono::steady_clock::time_point Until =
		std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<int64_t>(Src.size()) + 5000000);
		// 局所補修用本文と解析器の準備
		std::string Text = Src;
		TSParser *const Parser = ts_parser_new();
		if(!ts_parser_set_language(Parser, Lang::Get(Lang::Swift).TsLang())) {
			ts_parser_delete(Parser);
			// 文法を設定出来ない場合の失敗返戻
			return false;
		}
		// 編集する構文木と行位置追跡状態の準備
		TSTree *Tree = ts_tree_copy(Src.GetRoot().tree);
		// 直す位置の行（直す位置は前へ進むだけで，挿入は改行を含まない為，前の行頭からの改行を数え足す）
		uint32_t Row = 0;
		size_t Counted = 0;
		// 密着位置毎の空白挿入と差分再解析
		for(size_t Count = 0; Opener && Count * Text.size() < GluedCommentBudget && std::chrono::steady_clock::now() < Until; ++Count) {
			// 演算子と誤読されたコメント開始位置の空白分離
			Text.insert(Opener, 1, ' ');
			const size_t LineStart = Text.rfind('\n', Opener) + 1;
			Row += static_cast<uint32_t>(std::count(Text.begin() + Counted, Text.begin() + LineStart, '\n'));
			Counted = LineStart;
			const TSPoint Point = { Row, static_cast<uint32_t>(Opener - LineStart) };
			const TSInputEdit Edit = { Opener, Opener, Opener + 1, Point, Point, { Row, Point.column + 1 } };
			ts_tree_edit(Tree, &Edit);
			bool IsExpired = false;
			TSTree *const Next = ParseWithDeadline(Parser, Tree, Text, IsExpired);
			ts_tree_delete(Tree);
			Tree = Next;
			// 構文木作成不能時の修正位置保持と見送への移行
			if(!Tree) break;
			Opener = FirstGluedComment(Text, ts_tree_root_node(Tree), Opener);
		}
		// 補修完了状態の判定と解析資源の解放
		const bool IsUnfinished = !Tree || Opener;
		ts_tree_delete(Tree);
		ts_parser_delete(Parser);
		// 上限を超えて直し切れない事の返戻
		if(IsUnfinished) return false;
		// 補修を完了した本文だけをソースへの引渡し
		Src.Assign(std::move(Text));
	}
	// 見出式補正編集列の準備
	std::vector<TextEdit> Edits;
	// 見出式の括弧による包装
	const auto Wrap = [&Edits](const uint32_t Start, const uint32_t End) -> void {
		TextEdit::Push(Start, Start, "(", Edits);
		TextEdit::Push(End, End, ")", Edits);
	};
	const auto Exposed = [&Src, Swift = Lang::Get(Lang::Swift)](const TSNode Expression) -> bool {
		// 要素１つの括弧でなく，紛れる式を持つかの返戻
		return (std::string_view(ts_node_type(Expression)) != "tuple_expression" || ts_node_named_child_count(Expression) != 1) &&
		HasBodyLikeExpression(Src, Expression, Swift);
	};
	// 節点毎の補正（祖先は走査の道筋から受け，`ts_node_parent` で根から降り直さない）
	const auto Visit = [&](const TSNode Node, const std::span<const TSNode> Ancestors) -> void {
		// 節点型による演算子又は見出文の振分
		const std::string_view NodeType = ts_node_type(Node);
		if(NodeType == "custom_operator") {
			// 演算子の開始位置取得
			const uint32_t Start = Src.Start(Node);
			// 密着した `!` / `?` の後置への分離（参照と２字以上の記号列は誤読を防いで保持）
			if(
				Src.Len(Node) < 2 || Src[Start] != '!' && Src[Start] != '?' || Src[Start + 1] == '!' || Src[Start + 1] == '?' || !Start ||
				std::isspace(static_cast<unsigned char>(Src[Start - 1]))
				// 切り離さない演算子の終了
			) return;
			if(
				const TSNode Parent = Ancestors.empty() ? TSNode{} : Ancestors.back();
				ts_node_is_null(Parent) || std::string_view(ts_node_type(Parent)) != "infix_expression" ||
				!ts_node_is_named(ts_node_child(Parent, 0)) || Src.End(ts_node_child(Parent, 0)) != Start
				// 中置の式の左の被演算子へ密着しない演算子の終了
			) return;
			// 演算子前後の空白分離編集
			const uint32_t End = std::max(OperatorEnd(Src, Start), Src.End(Node));
			TextEdit::Push(Start + 1, Start + 1, " ", Edits);
			if(End > Start + 1 && End < Src.size() && !std::isspace(static_cast<unsigned char>(Src[End]))) {
				TextEdit::Push(End, End, " ", Edits);
			}
			// 利用者定義の演算子の終了
			return;
		}
		// 条件成立時の返戻
		if(!NodeKind::SwiftHeaderStatement.Contains(NodeType)) return;
		// 見出文の誤読範囲検出状態の初期化
		uint32_t MisreadEnd = 0;
		if(std::string_view(ts_node_type(ts_node_child(Node, ts_node_child_count(Node) - 1))) != "if_statement") {
			// 親の祖先列を辿る見出文範囲の拡張
			size_t ParentNext = Ancestors.size();
			TSNode Child = Node, Parent = ParentNext ? Ancestors[ParentNext - 1] : TSNode{};
			// 入れ子の if 末尾から外側の見出への遡及
			while(
				!ts_node_is_null(Parent) && std::string_view(ts_node_type(Parent)) == "if_statement" &&
				ts_node_eq(ts_node_child(Parent, ts_node_child_count(Parent) - 1), Child)
			) {
				Child = Parent;
				Parent = --ParentNext ? Ancestors[ParentNext - 1] : TSNode{};
			}
			// 後続クロージャ又は呼出接尾辞に依る誤読終端の検出
			if(
				const TSNode Next = ts_node_is_null(Parent) ? TSNode{} : ts_node_next_named_sibling(Parent);
				!ts_node_is_null(Next) && std::string_view(ts_node_type(Parent)) == "ERROR" && Src.End(Child) == Src.End(Parent) &&
				std::string_view(ts_node_type(Next)) == "lambda_literal"
			) MisreadEnd = Src.End(Parent);
			else if(
				const TSNode Follower = ts_node_next_named_sibling(Child);
				!ts_node_is_null(Follower) && NodeKind::SwiftStrayBlock.Contains(Follower) && Src[Src.Start(Follower)] == '{' &&
				!std::memchr(Src.data() + Src.End(Child), '\n', Src.Start(Follower) - Src.End(Child))
			) MisreadEnd = Src.End(Child);
			// 呼出の先頭側に連なる祖先から後続クロージャを探索
			else while(!ts_node_is_null(Parent) && ts_node_eq(ts_node_child(Parent, 0), Child)) {
				if(
					const TSNode Suffix = ts_node_child(Parent, ts_node_child_count(Parent) - 1);
					std::string_view(ts_node_type(Parent)) == "call_expression" && std::string_view(ts_node_type(Suffix)) == "call_suffix" &&
					ts_node_named_child_count(Suffix) == 1 && std::string_view(ts_node_type(ts_node_named_child(Suffix, 0))) == "lambda_literal"
				) MisreadEnd = Src.End(Child);
				Child = Parent;
				Parent = --ParentNext ? Ancestors[ParentNext - 1] : TSNode{};
			}
		}
		// 見出部の式候補走査
		TSNode Previous = {}, Last = {};
		ForEachChild(
			Node,
			[&](const TSNode Child) -> bool {
				// 子の型と本体到達の判定
				const std::string_view Type = ts_node_type(Child);
				// 本体に達した所で走査を打ち切る事の返戻
				if(ts_node_is_named(Child) ? Type == "else" : Src.View(Child) == "{") return false;
				// 見出語の後に続く式候補の抽出
				if(
					const TSNode Expression = Type == "where_clause" ?
					ts_node_named_child(Child, 1) :
					ts_node_is_named(Child) && !ts_node_is_null(Previous) && !ts_node_is_named(Previous) &&
					NodeKind::SwiftHeaderLead.Contains(Src.View(Previous)) ? Child : TSNode{};
					!ts_node_is_null(Expression)
				) {
					if(!ts_node_is_null(Last) && Exposed(Last)) Wrap(Src.Start(Last), Src.End(Last));
					Last = Expression;
				}
				// 次回用の直前子更新
				Previous = Child;
				// 走査を続ける事の返戻
				return true;
			}
		);
		// 最終見出の誤読本体範囲を含む終端設定
		if(MisreadEnd && !ts_node_is_null(Last)) Wrap(Src.Start(Last), MisreadEnd);
		else if(!ts_node_is_null(Last) && Exposed(Last)) Wrap(Src.Start(Last), Src.End(Last));
	};
	HasDescendantWithAncestorsOf(
		Src.GetRoot(),
		[&Visit](const TSNode Node, const std::span<const TSNode> Ancestors) -> bool {
			Visit(Node, Ancestors);
			// 全ての節点を訪ねる迄走査を続ける事の返戻
			return false;
		}
	);
	// 収集した見出式補正の適用
	TextEdit::Apply(Src, Edits);
	// 補正し切った事の返戻
	return true;
}

/**
 * 前処理の条件を跨ぐ `extern "C"` の波括弧の印への置換関数
 * 条件付コンパイルを跨ぐ `extern "C"` 波括弧の保護関数
 * 対を成す物だけを印の指令（`#pragma <印>O` / `#pragma <印>C`）へ置き換えて解析させ，RestoreLinkageGuards で戻す
 * @param Text 整理中の原文（行継続を繋いだ後，置換は此処へ行う）
 * @param Source 原文（印に使う制御文字を選ぶ）
 * @param Marker 印の格納先（既に選んで有れば其れを使う）
 * @return 置き換えた場合 true
 */
bool EditPass::MaskLinkageGuards(std::string &Text, const std::string &Source, char &Marker) {
	// 字面に無ければ走査を省く事の返戻
	if(Text.find("extern") == std::string::npos) return false;
	// コメントを空白へ置き換えた写し（位置は原文と同じ，コメントの有無で照合を変えない）
	std::string Blank(Text);
	// 字面を保った儘のコメント区間空白化
	for(size_t Pos = 0; Pos < Blank.size(); ++Pos) {
		// 現在字の分類とリテラルの読飛し
		const char Char = Blank[Pos];
		if(Char == '"' || Char == '\'') {
			// 文字列・文字リテラル内のコメント記号の読飛し
			for(++Pos; Pos < Blank.size() && Blank[Pos] != Char && Blank[Pos] != '\n'; ++Pos) if(Blank[Pos] == '\\') ++Pos;
			continue;
		}
		if(Char != '/' || Pos + 1 >= Blank.size() || Blank[Pos + 1] != '/' && Blank[Pos + 1] != '*') continue;
		// コメント終端の決定と範囲の空白化
		const size_t Close = Blank[Pos + 1] == '/' ? Blank.find('\n', Pos) : Blank.find("*/", Pos + 2);
		const size_t End = Close == std::string::npos ? Blank.size() : Blank[Pos + 1] == '/' ? Close : Close + 2;
		for(; Pos < End; ++Pos) if(Blank[Pos] != '\n') Blank[Pos] = ' ';
		--Pos;
	}
	// 空行を除いた各行のインデントを除いた中身（整形で指令の前後に置く空行は並びを変えない）
	std::vector<std::string_view> Lines;
	for(size_t Start = 0; Start < Blank.size();) {
		// 各行の非空白本文抽出
		const size_t End = std::min(Blank.find('\n', Start), Blank.size());
		if(const size_t Head = Blank.find_first_not_of(" \t", Start); Head < End) Lines.emplace_back(Blank.data() + Head, End - Head);
		Start = End + 1;
	}
	const auto Directive = [](const std::string_view Content) -> std::string_view {
		// 指令でない場合の返戻
		if(!Content.starts_with('#')) return {};
		// 指令名候補の開始位置設定
		size_t Start = 1;
		// 指令印後の水平空白読飛し
		while(Start < Content.size() && (Content[Start] == ' ' || Content[Start] == '\t')) ++Start;
		// 指令名の終端探索
		size_t End = Start;
		while(End < Content.size() && IsIdentifierChar(Content[End])) ++End;
		// 指令名の返戻
		return Content.substr(Start, End - Start);
	};
	struct Guard {
		size_t Start; // 置き換える字句の開始位置
		size_t Length; // 置き換える字句の長さ（後続のコメントは残す）
		bool IsOpen; // `extern "C" {` か（偽は `}`）
	};
	// 置換候補列の準備
	std::vector<Guard> Guards;
	// 前処理条件１組に挟まれた連結指定の字句だけを候補化
	for(size_t Index = 1; Index + 1 < Lines.size(); ++Index) {
		// 前後指令と其の間の本体の照合
		const std::string_view Opening = Directive(Lines[Index - 1]), Body = Lines[Index];
		if(!NodeKind::PreprocConditionWord.Contains(Opening) || Directive(Lines[Index + 1]) != "endif") continue;
		// 字句を空白を除いて照合する（コメントは写しで空白に為って居る）
		std::string_view Code = Body;
		while(!Code.empty() && (Code.back() == ' ' || Code.back() == '\t')) Code.remove_suffix(1);
		std::string Compact;
		for(const char Char : Code) if(Char != ' ' && Char != '\t') Compact += Char;
		if(Compact != "extern\"C\"{" && Compact != "}") continue;
		Guards.push_back({ static_cast<size_t>(Body.data() - Blank.data()), Code.size(), Compact != "}" });
	}
	// 候補波括弧の均衡検査
	size_t Depth = 0;
	for(const Guard &Entry : Guards) {
		if(Entry.IsOpen) ++Depth;
		// 開いて居ない閉じが有る場合の失敗返戻
		else if(!Depth--) return false;
	}
	// 対を成さない場合の返戻
	if(Guards.empty() || Depth || !Marker && !(Marker = TextEdit::AbsentControlChar(Source))) return false;
	// 前方位置保持の為の後方字句から印への置換
	for(size_t Index = Guards.size(); Index--;) {
		const Guard &Entry = Guards[Index];
		Text.replace(Entry.Start, Entry.Length, std::string("#pragma ") + Marker + (Entry.IsOpen ? 'O' : 'C'));
	}
	// 置き換えた事の返戻
	return true;
}

/**
 * C / C++ の行継続と前処理指令の整理関数（構文解析より前に原文の字面で行う）
 * 生文字列外の行継続を翻訳段階と同様に除去する関数
 * 囲みコメントの中はコメントの外の意味に関わらない為，字面を保つ
 * 前処理指令の本体は，tree-sitter-c が読み違えるコメントの開きの形を整える（本体のコメントを参照）
 * @param Source 原文（改行は LF へ正規化済）
 * @param Marker 置き換えた制御文字の格納先（置き換えなければ '\0'）
 * @return 整理後の原文（変更が無ければ空）
 */
std::string EditPass::PrepareCSource(const std::string &Source, char &Marker) {
	// 出力状態の初期化
	Marker = '\0';
	const size_t Size = Source.size();
	// 行継続（`\` + 水平空白 + 改行）の長さ（行継続でなければ０）
	const auto SpliceLength = [&Source, Size](const size_t At) -> size_t {
		// 逆斜線でない場合の返戻
		if(Source[At] != '\\') return 0;
		// 行継続候補の水平空白走査
		size_t End = At + 1;
		// 改行直前迄の水平空白読飛し
		while(
			End < Size && (Source[End] == ' ' || Source[End] == '\t' || Source[End] == '\f' || Source[End] == '\v' || Source[End] == '\r')
		) ++End;
		// 改行で終わる場合だけ行継続の長さの返戻
		return End < Size && Source[End] == '\n' ? End - At + 1 : 0;
	};
	const auto NextLogical = [&SpliceLength, Size](size_t At) -> size_t {
		// 連続する行継続の読飛し
		// 次の物理断片への前進
		for(size_t Length; At < Size && (Length = SpliceLength(At));) At += Length;
		// 次の字の位置の返戻
		return At;
	};
	std::string Out;
	Out.reserve(Size);
	bool IsChanged = false, IsLineHead = true, IsInDirective = false, IsHeaderName = false;
	size_t Pos = 0, DirectiveStart = 0, LastToken = 0, DirectiveTokens = 0;
	// 指令の中の囲みコメントの範囲（Out 上）
	std::vector<std::pair<size_t, size_t>> Comments;
	const auto SkipSplices = [&]() -> void {
		// 現在位置から次の論理字句への移動
		if(const size_t Next = NextLogical(Pos); Next != Pos) {
			Pos = Next;
			IsChanged = true;
		}
	};
	const auto MaskLiteral = [&](const size_t From) -> void {
		// リテラル中に現れる囲みコメント開始記号の探索
		for(size_t Found = Out.find("/*", From); Found != std::string::npos; Found = Out.find("/*", Found + 1)) {
			// 条件成立時の返戻
			if(!Marker && !(Marker = TextEdit::AbsentControlChar(Source))) return;
			// 開始記号を解析不能な印へ置換
			Out[Found + 1] = Marker;
			IsChanged = true;
		}
	};
	// 指令途中の囲みコメントを最終字句の後へ移し，跡には区切の空白１個の保持
	const auto EndDirective = [&]() -> void {
		// 指令状態の解除と移動対象数の算出
		IsInDirective = false;
		size_t Moved = 0;
		while(Moved < Comments.size() && Comments[Moved].second < LastToken + 1) ++Moved;
		// 指令コード部分の先行再構成とコメントの末尾移動
		if(Moved) {
			// 指令コードの再構築準備
			std::string Rebuilt;
			size_t Copied = DirectiveStart;
			// コメント跡を区切の空白へ置換
			for(size_t Idx = 0; Idx < Moved; ++Idx) {
				Rebuilt.append(Out, Copied, Comments[Idx].first - Copied);
				Rebuilt += ' ';
				Copied = Comments[Idx].second;
			}
			Rebuilt.append(Out, Copied, LastToken - Copied);
			// 抜き出したコメントを最終字句の後へ移動
			for(size_t Idx = 0; Idx < Moved; ++Idx) {
				Rebuilt += ' ';
				Rebuilt.append(Out, Comments[Idx].first, Comments[Idx].second - Comments[Idx].first);
			}
			Rebuilt.append(Out, LastToken, std::string::npos);
			Out.replace(DirectiveStart, std::string::npos, Rebuilt);
			IsChanged = true;
		}
		Comments.clear();
	};
	// 論理字句単位の原文走査
	while(true) {
		SkipSplices();
		if(Pos >= Size) break;
		// 現在の論理字の取得
		const char Char = Source[Pos];
		if(Char == '\n') {
			// 論理行終端の確定
			if(IsInDirective) EndDirective();
			Out += Char;
			++Pos;
			IsLineHead = true;
			continue;
		}
		if(Char == ' ' || Char == '\t' || Char == '\f' || Char == '\v' || Char == '\r') {
			// 水平空白の逐語複写
			Out += Char;
			++Pos;
			continue;
		}
		// 字句種別判定の準備
		const bool IsAtLineHead = IsLineHead;
		IsLineHead = false;
		// コメント開始記号候補の次の論理字位置取得
		const size_t Second = Char == '/' || Char == '.' ? NextLogical(Pos + 1) : Size;
		// 囲みコメントの抽出
		if(Second < Size && Source[Second] == '*' && Char == '/') {
			// 囲みコメントの出力開始位置記録
			const size_t CommentStart = Out.size();
			if(Second != Pos + 1) IsChanged = true;
			Out += "/*";
			Pos = Second + 1;
			// コメント閉鎖状態の初期化
			bool IsClosed = false;
			// 囲みコメント終端迄の逐語複写
			while(Pos < Size && !IsClosed) {
				if(Source[Pos] == '*') if(const size_t Slash = NextLogical(Pos + 1); Slash < Size && Source[Slash] == '/') {
					if(Slash != Pos + 1) IsChanged = true;
					Out += "*/";
					Pos = Slash + 1;
					IsClosed = true;
					continue;
				}
				Out += Source[Pos++];
			}
			// 指令内で閉じたコメント範囲の記録
			if(IsInDirective && IsClosed) Comments.emplace_back(CommentStart, Out.size());
			// 行頭コメント後の # の指令開始扱い
			IsLineHead = IsAtLineHead && !IsInDirective;
			continue;
		}
		// 行コメント開始の検出
		if(Second < Size && Source[Second] == '/' && Char == '/') {
			// 行コメント：行継続の除去と次の行の本文への結合
			if(Second != Pos + 1) IsChanged = true;
			Out += "//";
			Pos = Second + 1;
			for(SkipSplices(); Pos < Size && Source[Pos] != '\n'; SkipSplices()) Out += Source[Pos++];
			continue;
		}
		// 通常字句の出力開始位置記録
		const size_t TokenStart = Out.size();
		// 論理行の先頭で前処理指令の状態を初期化
		if(IsAtLineHead && Char == '#') {
			IsInDirective = true;
			DirectiveStart = TokenStart;
			DirectiveTokens = 0;
			IsHeaderName = false;
		}
		// 引用形式に応じたリテラル又は見出名の処理
		if(Char == '"' || Char == '\'' || IsHeaderName && Char == '<') {
			// リテラルと見出名からの行継続除去と未終端時の行末打切
			const char Close = Char == '<' ? '>' : Char;
			Out += Char;
			++Pos;
			// 引用符迄のリテラル逐語複写
			for(SkipSplices(); Pos < Size && Source[Pos] != '\n'; SkipSplices()) {
				const char Inner = Source[Pos++];
				Out += Inner;
				if(Inner == Close) break;
				if(Inner != '\\' || Close == '>') continue;
				SkipSplices();
				if(Pos < Size && Source[Pos] != '\n') Out += Source[Pos++];
			}
			// 指令内リテラルに含まれるコメント印の無効化
			if(IsInDirective) MaskLiteral(TokenStart);
		} else if(IsWordChar(Char)) {
			// 識別子字句の収集
			for(SkipSplices(); Pos < Size && IsWordChar(Source[Pos]); SkipSplices()) Out += Source[Pos++];
			const std::string_view Ident(Out.data() + TokenStart, Out.size() - TokenStart);
			// 生文字列（`R"delim(…)delim"`）：翻訳段階２が取り消される為，中身の逐語複写
			if(
				const bool IsRawPrefix = Ident == "R" || Ident == "LR" || Ident == "uR" || Ident == "UR" || Ident == "u8R";
				IsRawPrefix && Pos < Size && Source[Pos] == '"'
			) {
				if(
					const size_t Open = Source.find_first_of("( )\\\t\v\f\n", Pos + 1);
					Open != std::string::npos && Source[Open] == '(' && Open - Pos < 18
				) if(const size_t End = Source.find(")" + Source.substr(Pos + 1, Open - Pos - 1) + "\"", Open); End != std::string::npos) {
					// 生文字列全体の逐語複写
					const size_t Stop = End + Open - Pos + 1;
					Out.append(Source, Pos, Stop - Pos);
					Pos = Stop;
					if(IsInDirective) MaskLiteral(TokenStart);
				}
			}
			// 指令名から次の山括弧を見出名として読むかの決定
			if(IsInDirective && DirectiveTokens == 1) IsHeaderName = Ident == "include" || Ident == "include_next" || Ident == "import";
		} else if(
			std::isdigit(static_cast<unsigned char>(Char)) ||
			Char == '.' && Second < Size && std::isdigit(static_cast<unsigned char>(Source[Second]))
		) {
			// 数値（前処理数）：桁区切りの `'` と指数の符号の包含
			Out += Char;
			++Pos;
			// 前処理数の構成字走査
			for(SkipSplices(); Pos < Size; SkipSplices()) {
				const char Digit = Source[Pos];
				if(Digit == '\'') {
					// 桁区切り後の論理字確認
					const size_t After = NextLogical(Pos + 1);
					if(After == Size || !IsWordChar(Source[After])) break;
					Out += Digit;
					IsChanged = IsChanged || After != Pos + 1;
					Pos = After;
					continue;
				}
				if(!IsWordChar(Digit) && Digit != '.') break;
				Out += Digit;
				++Pos;
				if(Digit != 'e' && Digit != 'E' && Digit != 'p' && Digit != 'P') continue;
				// 指数部の符号収集
				SkipSplices();
				if(Pos < Size && (Source[Pos] == '+' || Source[Pos] == '-')) Out += Source[Pos++];
			}
		} else {
			// 単一記号字句の複写
			Out += Char;
			++Pos;
		}
		if(!IsInDirective) continue;
		// `#` 後の指令名と次字句迄の見出名候補
		if(DirectiveTokens++ > 1) IsHeaderName = false;
		LastToken = Out.size();
	}
	// 末尾の改行が無い指令も確定
	if(IsInDirective) EndDirective();
	// 連結指定保護の適用
	if(MaskLinkageGuards(Out, Source, Marker)) IsChanged = true;
	// 整理後の原文（変更が無ければ空）の返戻
	return IsChanged ? Out : std::string();
}

/**
 * 印へ置き換えた `extern "C"` の波括弧の復元関数
 * @param Text 整形後の原文
 * @param Marker 置換に使った印
 */
void EditPass::RestoreLinkageGuards(std::string &Text, const char Marker) {
	// 置換印の検索字句作成
	const std::string Placeholder = std::string("#pragma ") + Marker;
	// 各置換印の連結指定波括弧への復元
	for(size_t Found = Text.find(Placeholder); Found != std::string::npos; Found = Text.find(Placeholder, Found + 1)) {
		if(const size_t Kind = Found + Placeholder.size(); Kind < Text.size() && (Text[Kind] == 'O' || Text[Kind] == 'C')) {
			Text.replace(Found, Placeholder.size() + 1, Text[Kind] == 'O' ? "extern \"C\" {" : "}");
		}
	}
	// 終了
	return;
}
