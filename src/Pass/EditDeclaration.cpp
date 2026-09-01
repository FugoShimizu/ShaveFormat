#include "Edit.hpp"
#include "../Util/DeclEdit.hpp"
#include "../Util/NodeKind.hpp"
#include "../Util/Parallel.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/**
 * Java の `final` 付与候補が変更識別子集合へ属するかの判定関数
 * @param Names 宣言子識別子のビュー列
 * @param MutatedNames 変更識別子集合
 * @return １つでも含まれれば true
 */
bool EditPass::IsAnyNameMutated(
	const std::vector<std::string_view> &Names,
	const std::unordered_set<std::string_view> &MutatedNames
) {
	// 変更識別子が見付かった場合は真の返戻
	for(const std::string_view Name : Names) if(MutatedNames.contains(Name)) return true;
	// 全宣言名が未変更である事の返戻
	return false;
}

/**
 * Java の関数ローカル変数宣言への `final` 付与編集の収集関数（再代入が無い場合のみ）
 * @param Src ソースコード
 * @param Edits 収集先のエディット一覧
 */
void EditPass::CollectJavaFinalEdits(const TSSource &Src, std::vector<TextEdit> &Edits) {
	// クラスフィールドは外部変更を排除出来ない為，対象外
	const TSNode Root = Src.GetRoot();
	// 未解析の参照が在る場合，束縛と使用回数を証明不能
	if(ts_node_has_error(Root)) return;
	std::unordered_set<std::string> CanonicalNames;
	const auto NameOf = [&](const TSNode Node) -> std::string_view {
		// 原文上の識別子
		const std::string_view Name = Src.View(Node);
		// 結合文字が無い名前の直接返戻
		if(Name.find("\xE2\x80\x8C") == std::string_view::npos && Name.find("\xE2\x80\x8D") == std::string_view::npos) return Name;
		std::string Canonical;
		// UTF-8 の識別子字面を走査
		for(size_t Pos = 0; Pos < Name.size();) {
			// 結合制御文字を読飛ばし
			if(Name.substr(Pos, 3) == "\xE2\x80\x8C" || Name.substr(Pos, 3) == "\xE2\x80\x8D") Pos += 3;
			// 通常の識別子バイトを保持
			else Canonical.push_back(Name[Pos++]);
		}
		// 安定した比較名ビューの返戻
		return *CanonicalNames.insert(std::move(Canonical)).first;
	};
	struct ParameterScope {
		uint32_t End; // 仮引数スコープの終了位置
		std::unordered_set<std::string_view> Names; // スコープ内の仮引数名集合
		bool Inherits; // 外側の仮引数名も参照出来るか
	};
	// 現在位置を包む仮引数スコープ列
	std::vector<ParameterScope> Scopes;
	std::unordered_map<std::string_view, uint32_t> NameCounts;
	const auto IsNonconstant = [&](const TSNode Value) -> bool {
		std::vector<TSNode> Pending { Value };
		// 定数式を反復で深さ優先走査
		while(!Pending.empty()) {
			// 次に調べる式節点
			const TSNode Current = Pending.back();
			// 走査対象から現在節点を除去
			Pending.pop_back();
			// 空節点を読飛ばし
			if(ts_node_is_null(Current)) continue;
			const std::string_view Type = ts_node_type(Current);
			if(Type == "identifier") {
				// 参照された識別子名
				const std::string_view Name = NameOf(Current);
				// 内側から外側への仮引数探索
				for(std::vector<ParameterScope>::const_reverse_iterator Scope = Scopes.crbegin(); Scope != Scopes.crend(); ++Scope) {
					// 仮引数依存の初期値として返戻
					if(Scope->Names.contains(Name)) return true;
					// 非ラムダ境界で外側探索終了
					if(!Scope->Inherits) break;
				}
			}
			// 非定数式である事の返戻
			if(NodeKind::JavaNonConstantExpression.Contains(Type)) return true;
			// 値を作る部分式への非定数性確認の拡張
			if(Type == "binary_expression") {
				// 二項式左辺を走査予約
				Pending.push_back(TSSource::FieldChild(Current, "left"));
				// 二項式右辺を走査予約
				Pending.push_back(TSSource::FieldChild(Current, "right"));
			} else if(Type == "ternary_expression") {
				// 条件式を走査予約
				Pending.push_back(TSSource::FieldChild(Current, "condition"));
				// 真側の値を走査予約
				Pending.push_back(TSSource::FieldChild(Current, "consequence"));
				// 偽側の値を走査予約
				Pending.push_back(TSSource::FieldChild(Current, "alternative"));
			} else if(Type == "unary_expression") Pending.push_back(TSSource::FieldChild(Current, "operand"));
			else if(Type == "cast_expression") Pending.push_back(TSSource::FieldChild(Current, "value"));
			else if(Type == "parenthesized_expression") {
				ForEachNamedChild(
					Current,
					[&](const TSNode Child) -> void {
						Pending.push_back(Child);
					}
				);
			}
		}
		// 仮引数へ依存しない初期値の返戻
		return false;
	};
	std::unordered_set<std::string_view> MutatedNames;
	struct FinalCandidate {
		TSNode Node; // 局所変数宣言節点
		TSNode Modifiers; // 既存の修飾子列
		std::vector<std::string_view> Names; // 宣言された全識別子名
		std::vector<std::string_view> ConstantNames; // 定数式で初期化された識別子名
	};
	// final 付与候補列
	std::vector<FinalCandidate> Candidates;
	WalkAst(
		Root,
		// Java 節点毎の宣言・変更収集
		[&](const TSNode Node) -> void {
			const std::string_view Type = ts_node_type(Node);
			// 走査位置を過ぎた仮引数の有効範囲終了
			while(!Scopes.empty() && Scopes.back().End <= Src.Start(Node)) Scopes.pop_back();
			// 型本体を外側参照の境界として追加
			if(NodeKind::JavaTypeBody.Contains(Type)) Scopes.push_back({ Src.End(Node), {}, false });
			else if(NodeKind::JavaParameterScope.Contains(Type)) {
				// 新しい仮引数スコープ
				ParameterScope Scope { Src.End(Node), {}, Type == "lambda_expression" };
				TSNode Parameters = TSSource::FieldChild(Node, "parameters");
				// 簡略コンストラクタの仮引数のレコード宣言からの補完
				if(Type == "compact_constructor_declaration") {
					if(
						const TSNode Record = ts_node_parent(ts_node_parent(Node));
						!ts_node_is_null(Record) && std::string_view(ts_node_type(Record)) == "record_declaration"
					) Parameters = TSSource::FieldChild(Record, "parameters");
				}
				if(!ts_node_is_null(Parameters)) {
					// 単一ラムダ仮引数を登録
					if(std::string_view(ts_node_type(Parameters)) == "identifier") Scope.Names.insert(NameOf(Parameters));
					else {
						ForEachNamedChild(
							Parameters,
							// 各仮引数名の抽出
							[&](const TSNode Parameter) -> void {
								// 通常仮引数の名前節点
								TSNode Name = TSSource::FieldChild(Parameter, "name");
								if(std::string_view(ts_node_type(Parameter)) == "spread_parameter") {
									// 可変長仮引数の宣言名
									Name = TSSource::FieldChild(FirstNamedChildOfType(Parameter, "variable_declarator"), "name");
								} else if(std::string_view(ts_node_type(Parameter)) == "identifier") Name = Parameter;
								// 仮引数名をスコープへ登録
								if(!ts_node_is_null(Name)) Scope.Names.insert(NameOf(Name));
							}
						);
					}
				}
				// 新しい仮引数スコープを有効化
				Scopes.push_back(std::move(Scope));
			}
			// 同名参照の回数と束縛の変更を別々に記録
			if(Type == "identifier") ++NameCounts[NameOf(Node)];
			if(Type == "update_expression" && ts_node_named_child_count(Node)) {
				TSNode Operand = ts_node_named_child(Node, 0);
				// 外側括弧を剥離
				while(NamedTypeOf(Operand) == "parenthesized_expression") Operand = ts_node_named_child(Operand, 0);
				// 増減された識別子名を記録
				if(NamedTypeOf(Operand) == "identifier") MutatedNames.insert(NameOf(Operand));
			} else if(Type == "assignment_expression" && ts_node_named_child_count(Node)) {
				if(const TSNode Lhs = ts_node_named_child(Node, 0); std::string_view(ts_node_type(Lhs)) == "identifier") {
					// 代入左辺の識別子名を記録
					MutatedNames.insert(NameOf(Lhs));
				}
			} else if(Type == "local_variable_declaration") {
				// 既存の修飾子列
				TSNode Modifiers {};
				std::vector<std::string_view> Names, ConstantNames;
				bool HasFinal = false, IsStructureOk = true;
				ForEachNamedChild(
					Node,
					// 宣言子と修飾子の収集
					[&](const TSNode Child) -> void {
						if(const std::string_view ChildType = ts_node_type(Child); ChildType == "modifiers" && ts_node_is_null(Modifiers)) {
							// 最初の修飾子列を記録
							Modifiers = Child;
							ForEachChild(
								Child,
								[&](const TSNode ModTok) -> bool {
									if(!ts_node_is_named(ModTok) && Src.View(ModTok) == "final") {
										// 既存 final を記録
										HasFinal = true;
										// 既に final 有→付与不要で打切の返戻
										return false;
									}
									// final 未検出の為，走査継続の返戻
									return true;
								}
							);
						} else if(ChildType == "variable_declarator") {
							const TSNode Value = TSSource::FieldChild(Child, "value");
							if(ts_node_is_null(Value)) {
								// 初期値無の宣言を候補外として記録
								IsStructureOk = false;
								// 終了
								return;
							}
							// 宣言子の名前節点
							const TSNode IdNode = TSSource::FieldChild(Child, "name");
							if(std::string_view(ts_node_type(IdNode)) != "identifier") {
								// 単純識別子でない宣言を候補外として記録
								IsStructureOk = false;
								// 終了
								return;
							}
							// 宣言名と定数化の影響を受ける名前の対応付
							Names.push_back(NameOf(IdNode));
							// 定数式の宣言名を記録
							if(!IsNonconstant(Value)) ConstantNames.push_back(Names.back());
						}
					}
				);
				// 全宣言子を調べ終えてから final の候補を確定
				if(!HasFinal && IsStructureOk && !Names.empty()) {
					// final 付与候補を保存
					Candidates.push_back({ Node, Modifiers, std::move(Names), std::move(ConstantNames) });
				}
			}
		}
	);
	// 走査済の MutatedNames に依る final 候補の線形判定（変更対象の宣言子を含む宣言全体を除外）
	for(const FinalCandidate &Cand : Candidates) {
		// 定数変数化は参照先の型・到達可能性・文字列の共有を変える為，未使用と証明出来る物だけ許可
		if(
			IsAnyNameMutated(Cand.Names, MutatedNames) || std::any_of(
				Cand.ConstantNames.begin(),
				Cand.ConstantNames.end(),
				[&](const std::string_view Name) -> bool {
					return NameCounts[Name] != 1;
				}
			)
		) continue;
		// 既存修飾子後又は宣言先頭への final の配置
		if(!ts_node_is_null(Cand.Modifiers)) TextEdit::Push(Src.End(Cand.Modifiers), Src.End(Cand.Modifiers), " final", Edits);
		// 宣言先頭へ final を挿入
		else TextEdit::Push(Src.Start(Cand.Node), Src.Start(Cand.Node), "final ", Edits);
	}
	// 終了
	return;
}

/**
 * Rust の `let mut x = ...` の `mut` 削除編集の収集関数（変更操作が無い場合のみ）
 * @param Src ソースコード
 * @param Edits 収集先のエディット一覧
 */
void EditPass::CollectRustMutRemoveEdits(const TSSource &Src, std::vector<TextEdit> &Edits) {
	// 字句 `mut` を検索で短絡（不在なら候補０件で確定）
	if(Src.find("mut") == std::string::npos) return;
	const TSNode Root = Src.GetRoot();
	// 未解析の参照や変更操作が在る場合，可変性不要を証明不能
	if(ts_node_has_error(Root)) return;
	// 変更又は可変借用された名前集合
	std::unordered_set<std::string_view> MutatedNames;
	std::vector<TSNode> MacroInvocations, UnicodeIdentifiers;
	const auto ComparisonName = [&Src](const TSNode Node) -> std::string_view {
		std::string_view Name = Src.View(Node);
		// raw 識別子の接頭辞を除去
		if(Name.starts_with("r#")) Name.remove_prefix(2);
		// 比較用識別子名の返戻
		return Name;
	};
	// 最左の名前付子を反復して辿る基底識別子の抽出（長い連鎖の走脈領域を節約）
	const auto BaseIdentifier = [&ComparisonName](TSNode Node) -> std::string_view {
		// フィールド・添字包装を内側へ走査
		while(!ts_node_is_null(Node)) {
			const std::string_view Type = ts_node_type(Node);
			// 基底識別子名の返戻
			if(Type == "identifier") return ComparisonName(Node);
			// 各式経路から最左の名前付子への降下
			if(!NodeKind::RustBaseTraversal.Contains(Type) || !ts_node_named_child_count(Node)) break;
			// 左端の基底候補へ移動
			Node = ts_node_named_child(Node, 0);
		}
		// 基底識別子無の返戻
		return {};
	};
	// 分割代入を含む左辺から，全ての変更対象名を収集する（基底名を持つ部分木の内側へは降りない）
	const auto MarkMutationTargets = [&MutatedNames, &BaseIdentifier](const TSNode Node) -> void {
		// 条件成立時の返戻
		if(ts_node_is_null(Node)) return;
		if(const std::string_view BaseName = BaseIdentifier(Node); !BaseName.empty()) {
			// 単一の基底識別子名を記録
			MutatedNames.insert(BaseName);
			// 基底名を持つ部分木の内側へ降りずに終了
			return;
		}
		// 分配代入部分木の走査
		WalkChildrenCursor(
			Node,
			// 分配代入要素の基底名を収集
			[&](const TSNode Child) -> bool {
				// 名前付でない字句は降りない事の返戻
				if(!ts_node_is_named(Child)) return false;
				// 基底識別子の抽出
				const std::string_view BaseName = BaseIdentifier(Child);
				// 基底名を得れない部分木の内側へ降りる事の返戻
				if(BaseName.empty()) return true;
				// 変更対象の基底名を記録
				MutatedNames.insert(BaseName);
				// 基底名を得た部分木の内側へ降りない事の返戻
				return false;
			}
		);
		// 終了
		return;
	};
	// `ref mut` パターンは照合対象を可変借用
	const auto PatternMutablyBorrows = [&Src](const TSNode Pattern) -> bool {
		// 空パターンの非借用返戻
		if(ts_node_is_null(Pattern)) return false;
		const auto IsRefMut = [&Src](const TSNode Sub) -> bool {
			// 部分パターンの型
			const std::string_view Type = ts_node_type(Sub);
			// 参照でもフィールドでもない節点は対象外の返戻
			if(!NodeKind::RustRefPattern.Contains(Type)) return false;
			// ref トークン有無の収集
			bool HasRef = false;
			ForEachChild(
				Sub,
				// 参照パターンの子トークン走査
				[&](const TSNode Child) -> void {
					// `ref` トークンを記録
					if(!ts_node_is_named(Child) && Src.View(Child) == "ref") HasRef = true;
				}
			);
			// `ref` を持たない事の返戻
			if(!HasRef) return false;
			// 可変指定子の有無の返戻
			return Type == "ref_pattern" ?
			ts_node_named_child_count(Sub) && std::string_view(ts_node_type(ts_node_named_child(Sub, 0))) == "mut_pattern" :
			HasChildOf(
				Sub,
				// mutable_specifier 子の探索
				[](const TSNode Child) -> bool {
					// mut 指定子かの返戻
					return std::string_view(ts_node_type(Child)) == "mutable_specifier";
				}
			);
		};
		// 全部分パターンの可変借用判定
		// パターン全体の可変借用有無の返戻
		return IsRefMut(Pattern) || HasDescendantOf(
			Pattern,
			// 各部分パターンの可変参照判定
			[&IsRefMut](const TSNode Sub) -> bool {
				// ref mut 部分の有無を返戻
				return IsRefMut(Sub);
			}
		);
	};
	// 変更名と let mut 候補を１回の深さ優先走査で収集し，走査後に照合
	struct RustMutCandidate {
		TSNode Tok; // 除去候補の mut トークン
		TSNode IdNode; // 候補を識別する宣言名
		TSNode Declaration; // mut を含む let 宣言
		TSNode Scope; // 宣言を包むブロックスコープ
		bool HasUnexpandedAttribute; // 展開前属性に隣接するか
	};
	// mut 除去候補列
	std::vector<RustMutCandidate> Candidates;
	std::unordered_map<const void *, bool> AttributeProtection;
	std::vector<const void *> AttributePath;
	const auto HasUnexpandedAttribute = [&AttributeProtection, &AttributePath](TSNode Node) -> bool {
		// 祖先の状態を共有した今回の探索経路の再構成
		AttributePath.clear();
		bool IsProtected = false;
		// 宣言から根への祖先走査
		for(; !ts_node_is_null(Node); Node = ts_node_parent(Node)) {
			if(
				const std::unordered_map<const void *, bool>::const_iterator Cached = AttributeProtection.find(Node.id);
				Cached != AttributeProtection.end()
			) {
				// 既知の保護状態を採用
				IsProtected = Cached->second;
				break;
			}
			// 未確認祖先の記録と同一結果の後配布
			AttributePath.push_back(Node.id);
			// 直前の属性・コメント列を走査
			for(TSNode Prev = ts_node_prev_named_sibling(Node); !ts_node_is_null(Prev); Prev = ts_node_prev_named_sibling(Prev)) {
				const std::string_view PrevType = ts_node_type(Prev);
				if(PrevType == "attribute_item") {
					// 外部属性に依る保護を記録
					IsProtected = true;
					break;
				}
				if(!NodeKind::Comment.Contains(PrevType)) break;
			}
			// 先行属性が無い場合の本体内属性の確認
			if(!IsProtected) {
				IsProtected = HasChildOf(
					Node,
					// 内部属性の探索
					[](const TSNode Child) -> bool {
						// 内部属性かの返戻
						return std::string_view(ts_node_type(Child)) == "inner_attribute_item";
					}
				);
			}
			// 最初の保護属性で祖先探索終了
			if(IsProtected) break;
		}
		// 遡った節点の判定を控え，以降の問合せを高速化
		for(const void *const NodeId : AttributePath) AttributeProtection.emplace(NodeId, IsProtected);
		// 属性に依る保護有無の返戻
		return IsProtected;
	};
	WalkAst(
		Root,
		// Rust 節点毎の変更・候補収集
		[&](const TSNode Node) -> void {
			const std::string_view Type = ts_node_type(Node);
			if(Type == "identifier") if(
				const std::string_view Name = Src.View(Node); std::any_of(
					Name.begin(),
					Name.end(),
					[](const char Char) -> bool {
						return static_cast<unsigned char>(Char) > 0X7F;
					}
				) // 非 ASCII 識別子を保護対象へ追加
			) UnicodeIdentifiers.push_back(Node);
			// 後段で有効範囲と照合する未展開マクロの記録
			if(Type == "macro_invocation") MacroInvocations.push_back(Node);
			if(NodeKind::RustAssignmentLike.Contains(Type) && ts_node_named_child_count(Node)) {
				// 代入左辺の基底名を変更済に記録
				MarkMutationTargets(TSSource::FieldChild(Node, "left"));
			} else if(Type == "reference_expression") {
				// `&mut x` 検出
				bool HasMut = false;
				TSNode Operand {};
				ForEachNamedChild(
					Node,
					// 参照式の指定子と被演算子を収集
					[&](const TSNode Child) -> void {
						// mut 指定子と最初の被演算子の記録
						if(std::string_view(ts_node_type(Child)) == "mutable_specifier") HasMut = true;
						else if(ts_node_is_null(Operand)) Operand = Child;
					}
				);
				if(HasMut && !ts_node_is_null(Operand)) {
					// 可変借用された基底名を記録
					if(const std::string_view BaseName = BaseIdentifier(Operand); !BaseName.empty()) MutatedNames.insert(BaseName);
				}
			} else if(Type == "call_expression" && ts_node_named_child_count(Node)) {
				// メソッドと FnMut の呼出は受取側を変更し得る為，関数部の基底識別子を保持
				if(const std::string_view BaseName = BaseIdentifier(ts_node_named_child(Node, 0)); !BaseName.empty()) {
					// 可変受け手の可能性を変更済として記録
					MutatedNames.insert(BaseName);
				}
			} else if(NodeKind::RustLetBinding.Contains(Type)) {
				const TSNode Pattern = TSSource::FieldChild(Node, "pattern"), Value = TSSource::FieldChild(Node, "value");
				const std::string_view PatternType = ts_node_is_null(Pattern) ? "" : ts_node_type(Pattern);
				const std::string_view ValueType = ts_node_is_null(Value) ? "" : ts_node_type(Value);
				// ref mut の束縛では初期値の側を可変として保護
				if(PatternMutablyBorrows(Pattern)) MarkMutationTargets(Value);
				// 条件内束縛の mut 削除候補からの除外
				if(Type == "let_condition") return;
				// mut 指定子と宣言識別子
				TSNode MutSpec {}, IdNode {};
				ForEachNamedChild(
					Node,
					// 最初の mut 指定子を探索
					[&MutSpec](const TSNode Child) -> void {
						// mut トークンを記録
						if(std::string_view(ts_node_type(Child)) == "mutable_specifier" && ts_node_is_null(MutSpec)) MutSpec = Child;
					}
				);
				// 通常束縛名と @ パターン捕捉名の取得
				if(PatternType == "identifier") IdNode = Pattern;
				else if(PatternType == "captured_pattern" && ts_node_named_child_count(Pattern)) {
					if(const TSNode Captured = ts_node_named_child(Pattern, 0); std::string_view(ts_node_type(Captured)) == "identifier") {
						// 捕捉パターン内の宣言名を採用
						IdNode = Captured;
					}
				}
				// クロージャの可変呼出要否を型情報なしでは確定出来ない為，保持
				if(ValueType != "closure_expression" && !ts_node_is_null(MutSpec) && !ts_node_is_null(IdNode)) {
					TSNode Scope = Node;
					// 最寄りブロックまで祖先を走査
					while(!ts_node_is_null(Scope) && std::string_view(ts_node_type(Scope)) != "block") Scope = ts_node_parent(Scope);
					// mut 除去候補を保存
					Candidates.push_back({ MutSpec, IdNode, Node, Scope, HasUnexpandedAttribute(Node) });
				}
			} else if(Type == "match_expression") {
				// match の腕列
				const TSNode Body = TSSource::FieldChild(Node, "body");
				bool MutablyBorrows = false;
				ForEachNamedChild(
					Body,
					// 各 match 腕のパターンを検査
					[&](const TSNode Arm) -> void {
						// 可変借用パターンを記録
						if(PatternMutablyBorrows(TSSource::FieldChild(Arm, "pattern"))) MutablyBorrows = true;
					}
				);
				// １つでも可変借用する枝が有れば照合対象を保護
				if(MutablyBorrows) MarkMutationTargets(TSSource::FieldChild(Node, "value"));
			}
		}
	);
	const auto FirstAtOrAfter =
	// 指定位置以後の最初の節点検索関数
	[&Src](const std::vector<TSNode> &Nodes, const uint32_t Position) -> std::vector<TSNode>::const_iterator {
		// 指定位置以降の先頭要素の返戻
		return std::lower_bound(
			Nodes.begin(),
			Nodes.end(),
			Position,
			// 節点開始位置と検索位置の比較
			[&Src](const TSNode Node, const uint32_t Byte) -> bool {
				// 検索位置より前の節点かを返戻
				return Src.Start(Node) < Byte;
			}
		);
	};
	// 走査済の位置順の不確定要素を二分探索し，mut 削除の判定：次の識別子迄の空白も除く為，単なるキーワード置換と非共通化
	for(const RustMutCandidate &Cand : Candidates) {
		// 有効範囲や属性の影響を確定出来ない束縛を除外
		if(ts_node_is_null(Cand.Scope) || Cand.HasUnexpandedAttribute) continue;
		// 未展開マクロより前の局所束縛と，同じスコープに NFC 正規化を要する識別子が有る束縛の保持（再代入を否定出来ない為）
		if(
			const std::vector<TSNode>::const_iterator Macro = FirstAtOrAfter(MacroInvocations, Src.End(Cand.Declaration));
			Macro != MacroInvocations.end() && Src.End(*Macro) <= Src.End(Cand.Scope)
		) continue;
		if(
			// スコープ内の最初の非 ASCII 識別子
			const std::vector<TSNode>::const_iterator Unicode = FirstAtOrAfter(UnicodeIdentifiers, Src.Start(Cand.Scope));
			Unicode != UnicodeIdentifiers.end() && Src.End(*Unicode) <= Src.End(Cand.Scope)
		) continue;
		// 変更の無い束縛から mut と直後の空白の除外
		if(!MutatedNames.contains(ComparisonName(Cand.IdNode))) TextEdit::Push(Src.Start(Cand.Tok), Src.Start(Cand.IdNode), "", Edits);
	}
	// 終了
	return;
}

/**
 * Kotlin の関数ローカル `var` 宣言の `val` 置換編集の収集関数（再代入が無い場合のみ）
 * @param Src ソースコード
 * @param Edits 収集先のエディット一覧
 */
void EditPass::CollectVarToValEdits(const TSSource &Src, std::vector<TextEdit> &Edits) {
	// 字句 `var` を検索で短絡（不在なら候補０件で確定）
	if(Src.find("var") == std::string::npos) return;
	// 構文木と収集領域の初期化
	const TSNode Root = Src.GetRoot();
	// 解析失敗部分全体の読飛し
	if(ts_node_has_error(Root)) return;
	// 再束縛名と var 候補の同時収集（フィールド・添字経由の変更は除外）
	std::unordered_set<std::string_view> MutatedNames;
	std::vector<KeywordSwapCandidate> Candidates;
	const auto NameOf = [&Src](const TSNode Node) -> std::string_view {
		std::string_view Name = Src.View(Node);
		// 逆引用符を除去
		if(Name.size() > 1 && Name.front() == '`' && Name.back() == '`') Name = Name.substr(1, Name.size() - 2);
		// 比較用識別子名の返戻
		return Name;
	};
	// Kotlin の object を lambda と誤読した内部宣言の除外（外部から変更可能な為）
	const auto InMisreadObjectBody = [](const TSNode Node) -> bool {
		// ラムダ祖先までの走査
		for(TSNode Cur = ts_node_parent(Node); !ts_node_is_null(Cur); Cur = ts_node_parent(Cur)) {
			// ラムダ以外の祖先を読飛ばし
			if(std::string_view(ts_node_type(Cur)) != "lambda_literal") continue;
			TSNode Host = ts_node_parent(Cur);
			if(!ts_node_is_null(Host) && std::string_view(ts_node_type(Host)) == "annotated_lambda") {
				// 注釈包装の外側へ移動
				Host = ts_node_parent(ts_node_parent(Host));
			}
			// 所有式の先頭節点
			const TSNode Head = ts_node_is_null(Host) ? TSNode{} : ts_node_named_child(Host, 0);
			// 物体リテラル誤読かを返戻
			return !ts_node_is_null(Head) && std::string_view(ts_node_type(Head)) == "object_literal";
		}
		// 対象ラムダを持たない事の返戻
		return false;
	};
	const auto MarkOperand = [&NameOf, &MutatedNames](TSNode Operand) -> void {
		// 代入包装を内側へ走査
		while(!ts_node_is_null(Operand) && ts_node_named_child_count(Operand) == 1) {
			if(!NodeKind::KotlinAssignableWrapper.Contains(ts_node_type(Operand))) break;
			Operand = ts_node_named_child(Operand, 0);
		}
		if(!ts_node_is_null(Operand) && std::string_view(ts_node_type(Operand)) == "simple_identifier") {
			// 変更された単純識別子名を記録
			MutatedNames.insert(NameOf(Operand));
		}
	};
	WalkAst(
		Root,
		// Kotlin 節点毎の変更・候補収集
		[&](const TSNode Node) -> void {
			// 現在節点の型
			const std::string_view Type = ts_node_type(Node);
			// 代入と `++` `--` の被演算子を変更対象として記録
			if(
				ts_node_named_child_count(Node) && (
					Type == "assignment" || NodeKind::RangeOrUnaryExpression.Contains(Type) && HasChildOf(
						Node,
						// 増減演算子トークンの探索
						[&Src](const TSNode Child) -> bool {
							// 名前付の子は区切りでない事の返戻
							if(ts_node_is_named(Child)) return false;
							// 演算子字句の取得
							const std::string_view Tok = Src.View(Child);
							// 増減演算子字句かの返戻
							return Tok == "++" || Tok == "--";
						}
					)
				)
			) MarkOperand(ts_node_named_child(Node, 0));
			else if(Type == "property_declaration") {
				// 外部変更を否定出来ないクラスメンバを除き，親が statements の関数内宣言だけを候補化
				if(
					// プロパティ宣言の所有節点
					const TSNode Parent = ts_node_parent(Node);
					ts_node_is_null(Parent) || std::string_view(ts_node_type(Parent)) != "statements" || InMisreadObjectBody(Node)
					// 条件成立時の返戻
				) return;
				TSNode BindKind {}, VarDecl {};
				bool IsLateInit = false;
				ForEachNamedChild(
					Node,
					// 宣言種別・変数宣言・修飾子の収集
					[&](const TSNode Child) -> void {
						if(const std::string_view ChildType = ts_node_type(Child); ChildType == "binding_pattern_kind" && ts_node_is_null(BindKind)) {
							// 最初の束縛種別節点を記録
							BindKind = Child;
							// 最初の変数宣言を記録
						} else if(ChildType == "variable_declaration" && ts_node_is_null(VarDecl)) VarDecl = Child;
						else if(ChildType == "modifiers") {
							ForEachNamedChild(
								Child,
								// lateinit 修飾子の探索
								[&Src, &IsLateInit](const TSNode Modifier) -> void {
									// lateinit の有無を累積
									IsLateInit = IsLateInit || Src.View(Modifier) == "lateinit";
								}
							);
						}
					}
				);
				// val に変えれない宣言と構造不足の宣言を除外
				if(IsLateInit || ts_node_is_null(BindKind) || ts_node_is_null(VarDecl) || !ts_node_child_count(BindKind)) return;
				// 置換対象の var 字句と単純な束縛名を確定
				const TSNode VarTok = ts_node_child(BindKind, 0);
				// 条件成立時の返戻
				if(ts_node_is_named(VarTok) || Src.View(VarTok) != "var" || !ts_node_named_child_count(VarDecl)) return;
				if(const TSNode IdNode = ts_node_named_child(VarDecl, 0); std::string_view(ts_node_type(IdNode)) == "simple_identifier") {
					// var 置換候補を保存
					Candidates.push_back({ VarTok, IdNode });
				}
			}
		}
	);
	// 走査済の MutatedNames に依る var → val 候補の線形判定
	for(const KeywordSwapCandidate &Cand : Candidates) {
		// 未変更の var を val へ置換
		if(!MutatedNames.contains(NameOf(Cand.IdNode))) TextEdit::Push(Src.Start(Cand.Tok), Src.End(Cand.Tok), "val", Edits);
	}
	// 終了
	return;
}

/**
 * JS/TS の `let` 宣言の `const` 置換編集の収集関数（再代入が無い場合のみ）
 * @param Src ソースコード
 * @param Edits 収集先のエディット一覧
 */
void EditPass::CollectLetToConstEdits(const TSSource &Src, std::vector<TextEdit> &Edits) {
	// 字句 `let` を検索で短絡（不在なら候補０件で確定）
	if(Src.find("let") == std::string::npos) return;
	const TSNode Root = Src.GetRoot();
	// 解析失敗部分全体の再代入収集からの除外
	if(ts_node_has_error(Root)) return;
	// 直接 eval と識別子の別表記は，構文木上の字面だけでは再束縛を確定不能
	if(
		HasDescendantOf(
			Root,
			// eval と逃避識別子の探索
			[&Src](const TSNode Node) {
				// 現在節点の型
				const std::string_view Type = ts_node_type(Node);
				// 逃避を含む識別子は未展開の Unicode 名で保持する事の返戻
				if(Type == "identifier" && Src.View(Node).find('\\') != std::string_view::npos) return true;
				// 呼出以外は判定対象外の返戻
				if(Type != "call_expression") return false;
				// 呼出対象の取得
				TSNode Function = TSSource::FieldChild(Node, "function");
				// 単一括弧包装の反復除去
				while(
					!ts_node_is_null(Function) && std::string_view(ts_node_type(Function)) == "parenthesized_expression" &&
					ts_node_named_child_count(Function) == 1
				) Function = ts_node_named_child(Function, 0);
				// 直接 eval 呼出かを返戻
				return !ts_node_is_null(Function) && Src.View(Function) == "eval";
			}
		)
	) return; // 直接 eval を含むスクリプトの束縛保持の為の返戻
	// 古典スクリプト最上位束縛の保持
	std::unordered_set<const void *> ScriptTopLevel;
	if(
		!HasChildOf(
			Root,
			// module 文の探索
			[](const TSNode Child) {
				// module 文かを返戻
				return NodeKind::JsModuleStatement.Contains(Child);
			}
		)
	) {
		ForEachNamedChild(
			Root,
			// script 直下の let 宣言収集
			[&ScriptTopLevel](const TSNode Child) {
				// 最上位宣言を記録
				if(std::string_view(ts_node_type(Child)) == "lexical_declaration") ScriptTopLevel.insert(Child.id);
			}
		);
	}
	// 変更名と置換候補の収集領域初期化
	std::unordered_set<std::string_view> MutatedNames;
	std::vector<KeywordSwapCandidate> Candidates;
	const auto CollectIdsInto = [&MutatedNames, &Src](const TSNode Pattern) {
		// パターン節点の訪問関数
		const auto Visit = [&](const TSNode Node) {
			const std::string_view Type = ts_node_type(Node);
			// 代入される束縛名を記録
			if(NodeKind::JsBindingName.Contains(Type)) MutatedNames.insert(Src.View(Node));
			// メンバと添字への代入では受取側の変数は再束縛されない事の返戻
			return !NodeKind::JsMemberTarget.Contains(Type);
		};
		// 包装パターンの子孫から束縛名を収集
		if(Visit(Pattern)) WalkChildrenCursor(Pattern, Visit);
	};
	WalkAst(
		Root,
		// JS/TS 節点毎の変更・候補収集
		[&](const TSNode Node) {
			// 現在節点の型
			const std::string_view Type = ts_node_type(Node);
			if((Type == "update_expression" || NodeKind::AssignmentExpression.Contains(Type)) && ts_node_named_child_count(Node)) {
				// 代入又は増減対象名を変更済に記録
				CollectIdsInto(ts_node_named_child(Node, 0));
			} else if(Type == "lexical_declaration") {
				// 全ての宣言子が単純識別子＋初期化子付の `let x = ..., y = ...` のみ変換候補（宣言子毎に候補を並べ，何れも再代入されなければ変換する）
				if(ts_node_child_count(Node) && !ScriptTopLevel.contains(Node.id)) {
					const TSNode FirstChild = ts_node_child(Node, 0);
					const TSNode Export = ts_node_parent(Node);
					if(
						!ts_node_is_named(FirstChild) && Src.View(FirstChild) == "let" &&
						!(std::string_view(ts_node_type(Export)) == "export_statement" && !ts_node_eq(ts_node_parent(Export), Root))
					) {
						const size_t Begin = Candidates.size();
						const bool HasOther = HasChildOf(
							Node,
							// 各宣言子の const 化条件確認
							[&](const TSNode Decl) {
								// 字句 (`let` / `,` / `;`) は読み飛ばす事の返戻
								if(!ts_node_is_named(Decl)) return false;
								// 宣言子構造の検査
								const TSNode IdNode = ts_node_named_child(Decl, 0);
								if(
									std::string_view(ts_node_type(Decl)) != "variable_declarator" || ts_node_is_null(TSSource::FieldChild(Decl, "value")) ||
									std::string_view(ts_node_type(IdNode)) != "identifier"
									// 変換出来ない宣言子の返戻
								) return true;
								// 同じ let の宣言子候補を追加
								Candidates.push_back({ FirstChild, IdNode });
								// 次の宣言子へ進む事の返戻
								return false;
							}
						);
						// 一部を変換出来ない宣言の候補を全取消
						if(HasOther) Candidates.resize(Begin);
					}
				}
			} else if(Type == "for_in_statement") {
				// `for(let x of arr)`/`for(let x in obj)` の `let` も対象
				TSNode LetTok {}, IdNode {};
				ForEachChild(
					Node,
					// for 見出から let 宣言を抽出
					[&](const TSNode Child) -> bool {
						// let トークンを記録
						if(!ts_node_is_named(Child) && Src.View(Child) == "let") LetTok = Child;
						else if(
							!ts_node_is_null(LetTok) && ts_node_is_named(Child) && std::string_view(ts_node_type(Child)) == "identifier" &&
							ts_node_is_null(IdNode)
						) {
							// 反復変数の識別子を記録
							IdNode = Child;
							// 同名識別子（使用）を発見→走査継続不要の返戻
							return false;
						}
						// 識別子参照無の為，兄弟へ走査継続の返戻
						return true;
					}
				);
				// for 見出の let を候補へ追加
				if(!ts_node_is_null(LetTok) && !ts_node_is_null(IdNode)) Candidates.push_back({ LetTok, IdNode });
				else {
					// 宣言を伴わない反復変数と分割代入先の反復毎の再束縛
					const TSNode Left = TSSource::FieldChild(Node, "left");
					// 反復毎の再束縛名を記録
					if(ts_node_is_null(TSSource::FieldChild(Node, "kind")) && !ts_node_is_null(Left)) CollectIdsInto(Left);
				}
			}
		}
	);
	// let → const 化可否判定（同じ `let` の宣言子の候補は並んで居り，何れも再代入されない時だけ変換する）
	for(size_t Idx = 0; Idx < Candidates.size();) {
		const TSNode Tok = Candidates[Idx].Tok;
		bool IsConstant = true;
		for(; Idx < Candidates.size() && ts_node_eq(Candidates[Idx].Tok, Tok); ++Idx) {
			// 宣言子毎の未変更状態を累積
			IsConstant = IsConstant && !MutatedNames.contains(Src.View(Candidates[Idx].IdNode));
		}
		// 全宣言子未変更の let を const へ置換
		if(IsConstant) TextEdit::Push(Src.Start(Tok), Src.End(Tok), "const", Edits);
	}
	// 終了
	return;
}

/**
 * TS/JS の名前付 import 指定子の整列の収集関数（大文字小文字を問わない字順）
 * @param Src ソースコード（紐付の取出で書き換える）
 * @param Edits 編集列追加先
 * @param PostAttaches 並替で移る指定子のコメント引継先
 */
void EditPass::CollectNamedImportSortEdits(
	TSSource &Src,
	std::vector<TextEdit> &Edits,
	std::vector<DeclEdit::PostEditAttach> &PostAttaches
) {
	// 現在 import の指定子列
	std::vector<TSNode> Specifiers;
	std::vector<std::string> Keys;
	std::vector<size_t> Order;
	// 構文木からの名前付 import 収集
	WalkAst(
		Src.GetRoot(),
		// 名前付 import 節点の処理
		[&](const TSNode Node) -> void {
			// 無名字句の型名取得前の除外
			if(!ts_node_is_named(Node) || std::string_view(ts_node_type(Node)) != "named_imports") return;
			// import 毎の並替対象指定子の再収集
			Specifiers.clear();
			ForEachNamedChild(
				Node,
				// import 指定子だけを収集
				[&Specifiers](const TSNode Child) -> void {
					// 指定子を原文順に追加
					if(std::string_view(ts_node_type(Child)) == "import_specifier") Specifiers.push_back(Child);
				}
			);
			// 比較相手の無い import の保持
			if(Specifiers.size() < 2) return;
			// 連続空白を１個へ畳み，小文字化と末尾空白除去で整列キーの作成
			Keys.clear();
			// 指定子数のキー領域を予約
			Keys.reserve(Specifiers.size());
			// 各指定子の整列キー生成
			for(const TSNode Specifier : Specifiers) {
				const std::string_view View = Src.View(Specifier);
				std::string Key;
				// 原文字面長の領域を予約
				Key.reserve(View.size());
				// 先頭空白は前置扱いで読み飛ばす
				bool WasLastSpace = true;
				size_t LastNonSpace = 0;
				// 指定子字面の各文字を正規化
				for(const char Char : View) {
					if(const char Cur = Char == '\t' || Char == '\n' || Char == '\r' ? ' ' : Char; Cur == ' ') {
						// 連続空白の先頭だけを保持
						if(!WasLastSpace) Key += ' ';
						// 空白直後の状態へ更新
						WasLastSpace = true;
					} else {
						// 小文字化した文字を追加
						Key += static_cast<char>(std::tolower(static_cast<unsigned char>(Cur)));
						// 非空白直後の状態へ更新
						WasLastSpace = false;
						// 末尾空白を除く長さを更新
						LastNonSpace = Key.size();
					}
				}
				// 末尾空白を除去
				Key.resize(LastNonSpace);
				// 指定子の整列キーを保存
				Keys.push_back(std::move(Key));
			}
			// キー＋ノード対の全複製を避け，添字列を整列
			Order.resize(Specifiers.size());
			// 原文順の添字で初期化
			for(size_t Idx = 0; Idx < Order.size(); ++Idx) Order[Idx] = Idx;
			std::stable_sort(
				Order.begin(),
				Order.end(),
				[&Keys](const size_t a, const size_t b) -> bool {
					return Keys[a] < Keys[b];
				}
			);
			// 整列後の字面一致時の編集と再紐付の省略
			bool NeedsReorder = false;
			for(size_t Idx = 0; Idx < Specifiers.size(); ++Idx) if(Src.View(Specifiers[Idx]) != Src.View(Specifiers[Order[Idx]])) {
				NeedsReorder = true;
				break;
			}
			// 条件成立時の返戻
			if(!NeedsReorder) return;
			const uint32_t First = Src.Start(Specifiers.front()), Last = Src.End(Specifiers.back());
			std::string Replacement;
			for(size_t Idx = 0; Idx < Order.size(); ++Idx) {
				if(Idx) Replacement.append(", ");
				const uint32_t Offset = static_cast<uint32_t>(Replacement.size());
				const TSNode Moved = Specifiers[Order[Idx]];
				Replacement.append(Src.View(Moved));
				// 指定子の整列に伴うコメントの移送（全文置換で消える錨を引き継ぎ，コメントの有無に依らず整列）
				std::vector<CommentAttach> Leading = Src.TakeLeading(Moved), Trailing = Src.TakeTrailing(Moved);
				if(!Leading.empty() || !Trailing.empty()) {
					PostAttaches.push_back(
						{ First, Offset, "import_specifier", std::move(Leading), std::move(Trailing), static_cast<uint32_t>(Replacement.size()) }
					);
				}
			}
			TextEdit::Push(First, Last, std::move(Replacement), Edits);
		}
	);
	// 終了
	return;
}

/**
 * TS/JS の名前付 import 指定子の整列の適用関数（紐付が生きた状態で単独ラウンドとして走らせる）
 * @param Src ソースコード（破壊的に書き換える）
 * @param Language 対象言語
 */
void EditPass::ApplyNamedImportSort(TSSource &Src, const Lang Language) {
	// `import` の字句が無ければ名前付 import は０件で確定する為，全木走査毎省略
	if(!Language.IsJsTs() || !Src.IsParsed() || Src.find("import") == std::string::npos) return;
	std::vector<TextEdit> SortEdits;
	std::vector<DeclEdit::PostEditAttach> SortAttaches;
	// 並替本文とコメントの移動先を同時に収集
	CollectNamedImportSortEdits(Src, SortEdits, SortAttaches);
	// 並替を適用した新しい木へコメントの復元
	DeclEdit::ApplyByteRebind(Src, SortEdits, SortAttaches);
	// 終了
	return;
}

/**
 * 宣言分離スコープ判定関数（トップレベル宣言間の空行挿入と同一の条件）
 * @param Container 判定対象のコンテナノード
 * @return 直下の宣言を統合せず１宣言子毎に分離するスコープか
 */
bool EditPass::IsDeclSplitScope(const TSNode Container) {
	// 前処理包装を除いた実スコープの探索
	TSNode Effective = Container;
	// 前処理・大域包装の透過
	while(!ts_node_is_null(Effective)) {
		if(const std::string_view Type(ts_node_type(Effective)); !NodeKind::PreprocBlock.Contains(Type) && Type != "global_statement") {
			break;
		}
		Effective = ts_node_parent(Effective);
	}
	// ルート（ファイルのトップレベル）又はクラス様本体か否かの返戻
	return !ts_node_is_null(Effective) && (IsTreeRoot(Effective) || NodeKind::ClassBodyContainer.Contains(Effective));
}

/**
 * 宣言マージの関数ローカル部分の編集収集関数（`ApplyByteRebind` は呼出側責任）
 * @param Src ソースコード
 * @param Language 対象言語
 * @param MergeEdits 集めた編集列の追加先
 * @param MergePostAttaches 集めた再紐付情報の追加先
 * @return 分割と再統合を要するか（分離スコープ直下の宣言，又は関数ローカルの複数宣言子を持つ多行宣言が有れば true）
 */
bool EditPass::CollectVarDeclMergeEditsImpl(
	TSSource &Src,
	const Lang Language,
	std::vector<TextEdit> &MergeEdits,
	std::vector<DeclEdit::PostEditAttach> &MergePostAttaches
) {
	// ResumeAttachments 後の GetLeading/GetTrailing の取得：宣言の添字変更で付随情報を失わない様，バイト位置で再束縛
	if(!Language.IsBraceLang() || !Src.IsParsed()) return false;
	// 分割状態と宣言子領域の初期化
	bool NeedsSplit = false;
	std::vector<TSNode> Declarators;
	const auto Walker = [&](const TSNode Container, const auto &Self) -> void {
		// 深いネストは走脈領域を使い切る前に，新しい走脈で続きを走査
		const RecursionGuard Guard;
		if(Guard.IsOverflow) {
			Parallel::RunOnFreshStack(
				[&]() -> void {
					Self(Container, Self);
				}
			);
			// 新しい走脈で走査を終えた事の返戻
			return;
		}
		// 個別の文書・アクセス修飾・意味群を保つ為，関数内変数だけを統合
		if(IsDeclSplitScope(Container)) {
			// 分離スコープ直下の再帰走査
			ForEachNamedChild(
				Container,
				[&](const TSNode Child) -> void {
					// 分離スコープ直下の宣言有無に応じた後段分割走査の有効化
					if(NodeKind::DeclLike.Contains(Child)) NeedsSplit = true;
					Self(Child, Self);
				}
			);
			// 終了
			return;
		}
		// 宣言群の収集領域の初期化
		std::vector<DeclEdit::DeclSummary> Group;
		std::vector<TSNode> GroupNodes;
		uint32_t GroupTypeStart = 0, GroupTypeEnd = 0;
		const std::string &Source = Src;
		const auto FlushGroup = [&]() -> void {
			// 単独宣言の統合本文を作らない保持
			if(Group.size() < 2) return;
			// 共通型接頭辞の取得
			const std::string_view TypePrefix(Source.data() + GroupTypeStart, GroupTypeEnd - GroupTypeStart);
			// 前後空白系の切詰は共通補助関数へ委譲
			std::string_view TrimmedType = TypePrefix;
			TextEdit::TrimView(TrimmedType);
			const std::string_view LastToken = [&]() -> std::string_view {
				// 末尾空白の除外
				size_t Stop = TrimmedType.size();
				while(Stop && (TrimmedType[Stop - 1] == ' ' || TrimmedType[Stop - 1] == '\t')) --Stop;
				// 最終単語の開始位置走査
				size_t Begin = Stop;
				while(Begin && TrimmedType[Begin - 1] != ' ' && TrimmedType[Begin - 1] != '\t') --Begin;
				// 末尾空白除去後の最終単語の返戻
				return TrimmedType.substr(Begin, Stop - Begin);
			}();
			// C++ の `auto const` と `decltype(auto)` を含む型の推論対象への追加
			bool IsAutoLike = false;
			for(
				size_t Found = TrimmedType.find("auto");
				Found != std::string_view::npos && !IsAutoLike;
				Found = TrimmedType.find("auto", Found + 1)
			) {
				IsAutoLike = (!Found || !IsIdentifierChar(TrimmedType[Found - 1])) &&
				(Found + 4 >= TrimmedType.size() || !IsIdentifierChar(TrimmedType[Found + 4]));
			}
			IsAutoLike = IsAutoLike && (Language.IsCFamily() || Language == Lang::CSharp);
			if(
				const bool IsVarLike = LastToken == "var" && (Language == Lang::Java || Language == Lang::Kotlin || Language == Lang::CSharp);
				IsAutoLike || IsVarLike
				// 条件成立時の返戻
			) return;
			// C++17 構造化束縛 `auto [a, b] = ...` は単一宣言子のみ許容の為，`[` 始まり宣言子を含む群は統合読飛し
			for(const DeclEdit::DeclSummary &Decl : Group) {
				// 条件成立時の返戻
				if(Decl.FirstDeclaratorStart < Source.size() && Source[Decl.FirstDeclaratorStart] == '[') return;
			}
			// 統合本文の必要容量算出
			size_t CombinedReserve = TypePrefix.size() + 2;
			for(size_t Idx = 0; Idx < Group.size(); ++Idx) {
				const DeclEdit::DeclSummary &Decl = Group[Idx];
				CombinedReserve += Decl.LastDeclaratorEnd + (Idx ? 2 : 0) - Decl.FirstDeclaratorStart;
			}
			// 統合本文とコメント列の初期化
			std::string Combined;
			Combined.reserve(CombinedReserve);
			Combined = TypePrefix;
			std::vector<CommentAttach> MergedLeading, MergedTrailing;
			uint32_t CombinedOffset = static_cast<uint32_t>(TypePrefix.size());
			// 各宣言子の本文とコメントの結合
			for(size_t Idx = 0; Idx < Group.size(); ++Idx) {
				// C/C++ の関数・配列ポインタは型へ密着し，JS/TS の分割代入等は空白の保持
				const char FirstChar = !Idx && Group[Idx].FirstDeclaratorStart < Source.size() ? Source[Group[Idx].FirstDeclaratorStart] : '\0';
				const std::string_view Separator = Idx ? ", " : Language.IsCFamily() && (FirstChar == '(' || FirstChar == '[') ? "" : " ";
				Combined += Separator;
				CombinedOffset += static_cast<uint32_t>(Separator.size());
				const DeclEdit::DeclSummary &Decl = Group[Idx];
				const TSNode DeclNode = GroupNodes[Idx];
				const char *DeclaratorAnchorType = nullptr, *LastDeclaratorType = nullptr;
				uint32_t LastDeclaratorStart = 0;
				// C# の宣言子の包みを解き，コメントの錨の見落としの防止
				ForEachNamedChild(
					Language == Lang::CSharp ? DeclEdit::UnwrapCSharpDecl(DeclNode) : DeclNode,
					[&](const TSNode Child) -> bool {
						// 宣言子型の取得
						const char *const ChildType = ts_node_type(Child);
						// コメント錨に使う宣言子の記録
						if(NodeKind::DeclDeclaratorChild.Contains(std::string_view(ChildType))) {
							if(!DeclaratorAnchorType) DeclaratorAnchorType = ChildType;
							LastDeclaratorType = ChildType;
							LastDeclaratorStart = ts_node_start_byte(Child);
						}
						// 次の兄弟へ走査継続の返戻
						return true;
					}
				);
				const uint32_t DeclCombinedStart = CombinedOffset;
				// 紐付コメントの移動による取得
				std::vector<CommentAttach> OuterLeads = Src.TakeLeading(DeclNode), OuterTrails = Src.TakeTrailing(DeclNode);
				DeclEdit::TakeInnerDeclAttachments(Src, DeclNode, Language, OuterLeads, OuterTrails);
				// 先頭宣言子と其の他で移動先が異なる先行コメントの再付随
				if(Idx && DeclaratorAnchorType && !OuterLeads.empty()) {
					MergePostAttaches.push_back({ Group[0].Start, DeclCombinedStart, DeclaratorAnchorType, std::move(OuterLeads), {} });
				} else {
					MergedLeading.insert(
						MergedLeading.end(),
						std::make_move_iterator(OuterLeads.begin()),
						std::make_move_iterator(OuterLeads.end())
					);
				}
				// 末尾コメントを各宣言の最終宣言子へ再付随（最後の外側だけは統合宣言全体へ）
				if(Idx + 1 != Group.size() && LastDeclaratorType && !OuterTrails.empty()) {
					MergePostAttaches.push_back(
						{
							Group[0].Start,
							DeclCombinedStart + LastDeclaratorStart - Decl.FirstDeclaratorStart,
							LastDeclaratorType,
							{},
							std::move(OuterTrails)
						}
					);
				} else {
					MergedTrailing.insert(
						MergedTrailing.end(),
						std::make_move_iterator(OuterTrails.begin()),
						std::make_move_iterator(OuterTrails.end())
					);
				}
				const std::pair<uint32_t, uint32_t> Ranges[] =
				{ { Decl.Start, Decl.TypePrefixEnd }, { Decl.FirstDeclaratorStart, Decl.LastDeclaratorEnd } };
				const uint32_t CombinedStarts[] = { 0, DeclCombinedStart };
				DeclEdit::CollectSubAttachments(Src, DeclNode, Ranges, Group[0].Start, CombinedStarts, MergePostAttaches);
				Combined.append(Source.data() + Decl.FirstDeclaratorStart, Decl.LastDeclaratorEnd - Decl.FirstDeclaratorStart);
				CombinedOffset += Decl.LastDeclaratorEnd - Decl.FirstDeclaratorStart;
			}
			// 結合した宣言の単一終端による閉鎖
			Combined += ';';
			TextEdit::Push(Group[0].Start, Group.back().End, std::move(Combined), MergeEdits);
			// 群の最外部に属するコメントを統合宣言への復元
			if(!MergedLeading.empty() || !MergedTrailing.empty()) {
				MergePostAttaches.push_back(
					{ Group[0].Start, 0, ts_node_type(GroupNodes[0]), std::move(MergedLeading), std::move(MergedTrailing) }
				);
			}
		};
		// 現在群の確定と次宣言群への切替
		const auto AbortGroup = [&]() -> void {
			// 現在群の確定と状態消去
			FlushGroup();
			Group.clear();
			GroupNodes.clear();
		};
		const auto WalkNestedContainers = [&](const TSNode Node, const auto &SelfNested) -> void {
			// 字面の儘保つマクロの実引数 (`GLSL({int a; int b;})`) の中の宣言は統合すると字面が変わる事の返戻
			if(Language.IsCFamily() && Src.StringizesArguments(Node)) return;
			// 深いネストでの新しい走脈による探索継続
			const RecursionGuard Guard;
			if(Guard.IsOverflow) {
				Parallel::RunOnFreshStack(
					[&]() -> void {
						SelfNested(Node, SelfNested);
					}
				);
				// 新しい走脈で探索を終えた事の返戻
				return;
			}
			// 子コンテナの再帰走査
			ForEachNamedChild(
				Node,
				[&](const TSNode Inner) -> void {
					// 宣言コンテナの振分
					if(NodeKind::DeclContainer.Contains(Inner)) Self(Inner, Self);
					else SelfNested(Inner, SelfNested);
				}
			);
		};
		ForEachNamedChild(
			Container,
			[&](const TSNode Child) -> void {
				// 子節点型の取得
				const std::string_view ChildType(ts_node_type(Child));
				if(NodeKind::DeclContainer.Contains(ChildType)) {
					AbortGroup();
					Self(Child, Self);
					// DeclContainer 再帰後の終了
					return;
				}
				if(!NodeKind::DeclLike.Contains(ChildType)) {
					AbortGroup();
					// expression_statement／関数呼出引数のラムダ本体等の中にもネスト DeclContainer が有り得る為，再帰探索
					WalkNestedContainers(Child, WalkNestedContainers);
					// 非宣言子のネスト探索後の終了
					return;
				}
				// 宣言構造の解析
				const DeclEdit::DeclSummary Decl = DeclEdit::AnalyzeDecl(Src, Child, Language);
				// 構造を信用出来ない `MISSING` / `ERROR` 子孫を持つ宣言の統合除外
				if(!Decl.IsValid || ts_node_has_error(Child)) {
					AbortGroup();
					WalkNestedContainers(Child, WalkNestedContainers);
					// 終了
					return;
				}
				// 多行宣言（ラムダ／アロー関数本体・オブジェクトリテラル等で改行を含む）は統合対象外
				if(std::memchr(Source.data() + Decl.Start, '\n', Decl.End - Decl.Start)) {
					AbortGroup();
					// 多行の複合宣言の分割：単一行の宣言子を前後と統合可能にし，原文の区切に依る群分けと次巡での追加統合の防止
					if(!NeedsSplit && NodeKind::StatementHost.Contains(Container)) {
						DeclEdit::CollectDeclarators(Child, Decl.TypePrefixEnd, Language, Declarators);
						NeedsSplit = Declarators.size() > 1;
					}
					WalkNestedContainers(Child, WalkNestedContainers);
					// 終了
					return;
				}
				// C++ の型引数を省いた型名の初期化付宣言の統合除外（推論結果が異なり得る為）
				if(Language == Lang::Cpp && !ts_node_is_null(FirstNamedChildOfType(Child, "init_declarator"))) {
					TSNode Name = TSSource::FieldChild(Child, "type");
					while(!ts_node_is_null(Name) && std::string_view(ts_node_type(Name)) == "qualified_identifier") {
						Name = TSSource::FieldChild(Name, "name");
					}
					if(!ts_node_is_null(Name) && std::string_view(ts_node_type(Name)) == "type_identifier") {
						AbortGroup();
						WalkNestedContainers(Child, WalkNestedContainers);
						// 推論の可能性の有る宣言を群から外した後の終了
						return;
					}
				}
				// 現在宣言の型接頭辞取得
				const std::string_view CurType(Source.data() + Decl.Start, Decl.TypePrefixEnd - Decl.Start);
				if(!Group.empty()) {
					if(const std::string_view GroupType(Source.data() + GroupTypeStart, GroupTypeEnd - GroupTypeStart); CurType != GroupType) {
						AbortGroup();
					}
				}
				// 新しい群の共通の型範囲を最初の宣言からの決定
				if(Group.empty()) {
					GroupTypeStart = Decl.Start;
					GroupTypeEnd = Decl.TypePrefixEnd;
				}
				// 宣言の範囲とコメントを持つ節点の対応の保持
				Group.push_back(Decl);
				GroupNodes.push_back(Child);
				WalkNestedContainers(Child, WalkNestedContainers);
			}
		);
		// 末尾宣言群の確定
		FlushGroup();
	};
	// 最上位から各有効範囲の統合候補を収集
	Walker(Src.GetRoot(), Walker);
	// 分割と再統合を要するかの返戻（呼出側が分割＋再統合を発火するか判定する）
	return NeedsSplit;
}

/**
 * 不変修飾 (`const` / `final`) の付与の適用関数
 * @param Src ソースコード（破壊的に書き換える）
 * @param Language 対象言語 (Java / JavaScript / TypeScript)
 */
void EditPass::ApplyImmutableQualifiers(TSSource &Src, const Lang Language) {
	// 構文木が無い場合の返戻
	if(!Src.IsParsed()) return;
	// 不変修飾編集の収集領域初期化
	std::vector<TextEdit> Edits;
	// 言語別の修飾編集収集
	if(Language == Lang::Java) CollectJavaFinalEdits(Src, Edits);
	else CollectLetToConstEdits(Src, Edits);
	// 紐付は編集範囲の外に在る為，位置基準の再束縛で運び直す（編集が無ければ再構文解析も起きない）
	std::vector<DeclEdit::PostEditAttach> Attaches;
	DeclEdit::ApplyByteRebind(Src, Edits, Attaches);
	// 終了
	return;
}

/**
 * 宣言統合の分離スコープ分割と再統合の実行関数
 * @param Src ソースコード（破壊的に書き換える）
 * @param Language 対象言語
 * @param HadMergeEdit 本体走査の統合編集が１件以上有ったか（分割不発生との連言で末尾再統合走査の省略判定に使う）
 */
void EditPass::RunVarDeclSplitAndRemerge(TSSource &Src, const Lang Language, const bool HadMergeEdit) {
	// CollectVarDeclMergeEditsImpl が分割と再統合を要する＝真を返した時のみ ApplyVarDeclMerge から呼び出される前提
	if(!Language.IsBraceLang() || !Src.IsParsed()) return;
	// 宣言子毎の分割適用
	const bool IsSplitApplied = DeclEdit::SplitDeclarations(
		Src,
		Language,
		[&Src, Language](std::vector<DeclEdit::SplitCandidate> &Cands) -> void {
			WalkChildrenCursor(
				Src.GetRoot(),
				[&](const TSNode Node, const TSNode Parent) -> bool {
					// 字面の儘保つマクロの実引数の中は分割しない事の返戻
					if(Language.IsCFamily() && Src.StringizesArguments(Node)) return false;
					// 節点型の取得
					const char *const TypeRaw = ts_node_type(Node);
					// 宣言でない節点の子へ降りる事の返戻
					if(!NodeKind::DeclLike.Contains(TypeRaw)) return true;
					// 別スコープの宣言と，多行で統合から外れた関数内宣言だけの再統合用の分割
					const bool IsSplitScope = IsDeclSplitScope(Parent);
					// 分割の対象外の場所の宣言の子へ降りる事の返戻
					if(!IsSplitScope && !NodeKind::StatementHost.Contains(Parent)) return true;
					// 宣言構造の解析
					const DeclEdit::DeclSummary Decl = DeclEdit::AnalyzeDecl(Src, Node, Language);
					// 分割しない宣言の子へ降りる事の返戻
					if(!Decl.IsValid || !IsSplitScope && !std::memchr(Src.data() + Decl.Start, '\n', Decl.End - Decl.Start)) return true;
					// 宣言子列の収集
					std::vector<TSNode> Declarators;
					DeclEdit::CollectDeclarators(Node, Decl.TypePrefixEnd, Language, Declarators);
					// 複数宣言子だけの分割と同一範囲の包装引継
					if(Declarators.size() > 1) {
						const bool IsWrapped =
						ts_node_start_byte(Parent) == ts_node_start_byte(Node) && ts_node_end_byte(Parent) == ts_node_end_byte(Node);
						Cands.push_back({ Node, IsWrapped ? Parent : TSNode{}, Decl, std::move(Declarators), TypeRaw });
					}
					// 宣言の中（初期化子のラムダ等）へも降りる事の返戻
					return true;
				}
			);
		},
		{},
		true,
		{}
	);
	// 分割で生じた単一宣言子へ，従来対象の言語だけ不変修飾を付与
	if(IsSplitApplied && (Language == Lang::Java || Language == Lang::JavaScript || Language == Lang::TypeScript)) {
		ApplyImmutableQualifiers(Src, Language);
	}
	// 分割結果に応じた後処理の選択
	// 統合も分割も無ければ本文は同じで再統合も空に為る為，省略
	if(!HadMergeEdit && !IsSplitApplied) return;
	std::vector<TextEdit> RemergeEdits;
	std::vector<DeclEdit::PostEditAttach> RemergePostAttaches;
	CollectVarDeclMergeEditsImpl(Src, Language, RemergeEdits, RemergePostAttaches);
	// 再統合した位置へコメントを移して処理を確定
	DeclEdit::ApplyByteRebind(Src, RemergeEdits, RemergePostAttaches);
	// 終了
	return;
}

/**
 * 宣言統合の適用関数
 * @param Src 整形対象ソース
 * @param Language 対象言語
 */
void EditPass::ApplyVarDeclMerge(TSSource &Src, const Lang Language) {
	std::vector<TextEdit> MergeEdits;
	std::vector<DeclEdit::PostEditAttach> MergePostAttaches;
	// 更新済の構文木から宣言と紐付を収集し，破棄された編集の再紐付や次巡への統合持越を防止
	const bool NeedsSplit = CollectVarDeclMergeEditsImpl(Src, Language, MergeEdits, MergePostAttaches);
	const bool HadMergeEdit = !MergeEdits.empty();
	DeclEdit::ApplyByteRebind(Src, MergeEdits, MergePostAttaches);
	// 分割候補の検出時のみ分割＋再統合を実行
	if(NeedsSplit) RunVarDeclSplitAndRemerge(Src, Language, HadMergeEdit);
	// 終了
	return;
}
