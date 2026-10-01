#include "ParseAdapter.hpp"
#include "../Profile/GrammarProfile.hpp"
#include "FormatLimitExceeded.hpp"
#include "ParsedTree.hpp"
#include "TextEdit.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
	// 優先順位に従う二項式の直接の部品
	struct BinaryParts {
		TSNode Left {}; // 左の被演算子
		TSNode Right {}; // 右の被演算子
		int Precedence = 0; // 補修の対象として確定した優先順位（対象外は０）
		bool IsUnknown = false; // 優先順位の分からない利用者定義の演算子か
	};

	// 連鎖の部分式の本文の範囲
	struct OperandSpan {
		uint32_t Start = 0; // 始まり
		uint32_t End = 0; // 終わり
	};

	/**
	 * コメントと補完字句を除いた直接の子の取得関数
	 * 計算量：直接の子の数 N に対し O(N)
	 * @param Src 解析用の本文
	 * @param Node 親の節点
	 * @return 原文順の構文の子
	 */
	std::vector<TSNode> CodeChildren(const ParsedTree &Src, const TSNode Node) {
		// 補完字句とコメントを除いた子の収集
		std::vector<TSNode> Children;
		for(uint32_t Index = 0; Index < ts_node_child_count(Node); ++Index) {
			if(const TSNode Child = ts_node_child(Node, Index); !Src.Is(Child, Core::Role::Comment) && !ts_node_is_missing(Child)) {
				Children.push_back(Child);
			}
		}
		// 構文の子の返戻
		return Children;
	}

	/**
	 * 重ならない区画の追加関数
	 * 計算量：選んだ区画の数 G に対し O(log G)
	 * @param Start 追加する区画の始まり
	 * @param End 追加する区画の終わり
	 * @param Groups 選んだ区画（始まりから終わりへの対応）
	 */
	void AddDisjoint(const uint32_t Start, const uint32_t End, std::map<uint32_t, uint32_t> &Groups) {
		// 既存区画との重なりの検査
		const std::map<uint32_t, uint32_t>::const_iterator Next = Groups.lower_bound(Start);
		// 選んだ区画と重なる区画は次の巡へ残す事の返戻
		if(Next != Groups.end() && Next->first < End || Next != Groups.begin() && std::prev(Next)->second > Start) return;
		// 重ならない区画の追加
		Groups.emplace(Start, End);
		// 終了
		return;
	}

	/**
	 * 解析用の括弧で互いに重ならない部分式を纏めて包む関数
	 * 計算量：区画の数 G と既存の囲みの数 S に対し O(G log S + S) と再解析１回分
	 * @param Src 解析用の本文
	 * @param Selected 包む部分式の範囲（始まりから終わりへの対応）
	 * @throws FormatLimitExceeded 包んだ部分式を再び選ぶ（補修が進まない）場合と，挿入後の解析が期限を超えた場合
	 */
	void WrapExpressions(ParsedTree &Src, const std::map<uint32_t, uint32_t> &Selected) {
		// 既存の囲みとの照合
		const std::vector<std::pair<uint32_t, uint32_t>> &Wrapped = Src.GetSyntheticGroups();
		const auto IsWrapped = [&Wrapped](const std::pair<uint32_t, uint32_t> Group) -> bool {
			const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Found = std::ranges::lower_bound(Wrapped, Group);
			// 同じ開きと閉じの囲みの有無の返戻
			return Found != Wrapped.end() && *Found == Group;
		};
		// 挿入する括弧の編集列の構築
		const std::vector<std::pair<uint32_t, uint32_t>> Groups(Selected.begin(), Selected.end());
		std::vector<TextEdit> Edits;
		Edits.reserve(Groups.size() << 1);
		for(const auto &[Start, End] : Groups) {
			if(Start >= End || End > Src.size() || Start && IsWrapped({ Start - 1, End }) || IsWrapped({ Start, End - 1 })) {
				throw FormatLimitExceeded("a parser binding repair could not preserve the structure", FailureReason::FormatterDefect);
			}
			Edits.push_back({ Start, Start, "(" });
			Edits.push_back({ End, End, ")" });
		}
		// 括弧の挿入と再解析
		TextEdit::Apply(Src, Edits, false, &Groups);
		// 終了
		return;
	}

	/**
	 * 二項式の左右と演算子の取得関数
	 * 計算量：直接の子の数 N と復号資料の数 R に対し O(N + log R)
	 * @param Src 解析用の本文
	 * @param Node 調べる節点
	 * @return 対象の二項式なら其の部品
	 */
	BinaryParts BinaryOf(const ParsedTree &Src, const TSNode Node) {
		// 解析誤りの無い二項式でなければ部品を持たない事の返戻
		if(ts_node_is_null(Node) || !Src.Is(Node, Core::Role::BinaryPrecedenceExpression) || ts_node_has_error(Node)) return {};
		const std::vector<TSNode> Parts = CodeChildren(Src, Node);
		// 左右と演算子の３部品が無ければ対象外の返戻
		if(Parts.size() != 3) return {};
		// 原文の演算子と優先順位の復元
		const LexemeRestoration *const Restoration = Src.RestorationAt(Src.Start(Parts[1]), Src.End(Parts[1]));
		const std::string_view Operator = Restoration ? std::string_view(Restoration->Original) : Src.View(Parts[1]);
		const Core::SyntaxRules &Rules = Src.GetRules();
		const std::unordered_map<std::string_view, int>::const_iterator Found = Rules.OperatorPrecedence.find(Operator);
		const int Precedence =
		Src.GetReading().ReassociatedOperators.contains(Operator) && Found != Rules.OperatorPrecedence.end() ? Found->second : 0;
		// 演算子を葉として持つ二項式の部品の返戻
		return { Parts[0], Parts[2], Precedence, Src.Is(Parts[1], Core::Role::UserDefinedOperator) && !Precedence };
	}

	/**
	 * 優先順位が未知の演算子と，結合を確定する式が隣接するかの判定関数
	 * 計算量：節点数 N，直接の子の最大数 C，復号資料の数 R に対し O(N (C + log R))
	 * @param Src 解析用の本文
	 * @return 解析器の結合を採れない連鎖が有れば true
	 */
	bool HasUnknownChain(const ParsedTree &Src) {
		bool Found = false;
		WalkChildrenCursor(
			Src.GetRoot(),
			[&](const TSNode Node, const TSNode Parent) -> bool {
				const BinaryParts Parts = BinaryOf(Src, Node);
				// 未知の演算子の二項式でない節点の子を辿る事の返戻
				if(!Parts.IsUnknown) return true;
				const auto IsBoundary = [&Src](const TSNode Adjacent) -> bool {
					// 優先順位を決める式又は別の二項式との接続の返戻
					return !ts_node_is_null(Adjacent) &&
					(Src.Is(Adjacent, Core::Role::BinaryPrecedenceExpression) || Src.Is(Adjacent, Core::Role::OperatorPrecedenceBoundary));
				};
				// 三項の真側は `:` で閉じる為，外側との優先順位に依らない
				const bool IsConditionalBoundary =
				Src.Is(Parent, Core::Role::ConditionalOperatorExpression) && !ts_node_eq(Src.FieldChild(Parent, Core::ChildRole::IfTrue), Node);
				Found = IsBoundary(Parts.Left) || IsBoundary(Parts.Right) || IsBoundary(Parent) || IsConditionalBoundary;
				// 連鎖を見付けた後に内側を辿らない事の返戻
				return !Found;
			}
		);
		// 未知の優先順位に依る連鎖の有無の返戻
		return Found;
	}

	/**
	 * 種別だけを読み違える標準の演算子の字面の置換関数
	 * 解析器が既知の同じ種類の二項式として読む字面へ置き換え，元の字面を投影時に戻す印として控える
	 * 計算量：節点数 N と置換の数 K に対し O((N + K) log K) と再解析１回分
	 * @param Src 解析用の本文
	 * @throws FormatLimitExceeded 置換後の解析で種別を正せない場合
	 */
	void RestoreOperatorKinds(ParsedTree &Src) {
		// 演算子の代用字面と原文の収集
		const Core::SyntaxRules &Rules = Src.GetRules();
		const ReadingRules &Reading = Src.GetReading();
		std::vector<TextEdit> Replacements;
		std::vector<LexemeRestoration> Originals;
		WalkChildrenCursor(
			Src.GetRoot(),
			[&](const TSNode Node, const TSNode Parent) -> bool {
				// 利用者定義の演算子でない節点の子を辿る事の返戻
				if(!Src.Is(Node, Core::Role::UserDefinedOperator)) return true;
				const std::string_view Original = Src.View(Node);
				const std::unordered_map<std::string_view, std::string_view>::const_iterator StandIn = Reading.OperatorStandIns.find(Original);
				// 二項式の演算子でない物と代わりの字面を持たない物の子を辿らない事の返戻
				if(!Src.Is(Parent, Core::Role::BinaryPrecedenceExpression) || StandIn == Reading.OperatorStandIns.end()) return false;
				if(!Rules.TokenDefinitions.contains(Original)) {
					throw FormatLimitExceeded("a restorable operator has no grammar token", FailureReason::FormatterDefect);
				}
				TextEdit::Push(Src.Start(Node), Src.End(Node), std::string(StandIn->second), Replacements);
				Originals.push_back({ Src.Start(Node), Src.End(Node), std::string(Original) });
				// 演算子の子は辿らない事の返戻
				return false;
			}
		);
		// 置換の無い本文の終了
		if(Replacements.empty()) return;
		// 元の演算子の原文順への整列
		std::sort(
			Originals.begin(),
			Originals.end(),
			[](const LexemeRestoration &Lhs, const LexemeRestoration &Rhs) -> bool {
				// 原文位置の順の返戻
				return Lhs.Start < Rhs.Start;
			}
		);
		// 代用字面の適用と原文復元位置の登録
		TextEdit::Apply(Src, Replacements);
		int64_t Offset = 0;
		for(const LexemeRestoration &Original : Originals) {
			const std::string_view StandIn = Reading.OperatorStandIns.at(Original.Original);
			const uint32_t Start = static_cast<uint32_t>(static_cast<int64_t>(Original.Start) + Offset);
			Src.AddLexemeRestoration(Start, Start + static_cast<uint32_t>(StandIn.size()), Original.Original);
			Offset += static_cast<int64_t>(StandIn.size()) - (Original.End - Original.Start);
		}
		// 再解析後の代用字面の所在の確認
		std::vector<uint8_t> Resolved(Src.GetLexemeRestorations().size());
		WalkChildrenCursor(
			Src.GetRoot(),
			[&](const TSNode Node) -> bool {
				const LexemeRestoration *const Restoration =
				ts_node_child_count(Node) ? nullptr : Src.RestorationAt(Src.Start(Node), Src.End(Node));
				if(Restoration && Src.Is(Node, Core::Role::UserDefinedOperator)) {
					throw FormatLimitExceeded("a replacement still has an unresolved operator kind", FailureReason::FormatterDefect);
				}
				if(Restoration) Resolved[static_cast<size_t>(Restoration - Src.GetLexemeRestorations().data())] = 1;
				// 置き換えた字句の所在を全て調べる事の返戻
				return true;
			}
		);
		if(std::ranges::find(Resolved, uint8_t{ 0 }) != Resolved.end()) {
			throw FormatLimitExceeded("a replacement token disappeared during reparsing", FailureReason::FormatterDefect);
		}
		// 終了
		return;
	}

	/**
	 * 二項式の全体へ結ばれた前置の演算子の範囲の収集関数
	 * 包む範囲と重ならない前置の式（二項式の右の被演算子の中の物）は同じ巡で集め，包む範囲の内側は包んだ後の巡で調べる
	 * 計算量：節点数 N，直接の子の最大数 C，復号資料の数 R，選んだ区画の数 G に対し O(N (C + log R + log G))
	 * @param Src 解析用の本文
	 * @param Groups 前置の演算子と最も左の被演算子の範囲の格納先
	 */
	void CollectMisboundPrefixes(const ParsedTree &Src, std::map<uint32_t, uint32_t> &Groups) {
		WalkChildrenCursor(
			Src.GetRoot(),
			[&](const TSNode Node) -> bool {
				// 前置の式でない節点の子を辿る事の返戻
				if(!Src.Is(Node, Core::Role::PrefixExpression)) return true;
				const std::vector<TSNode> Parts = CodeChildren(Src, Node);
				const BinaryParts Binary = Parts.size() == 2 ? BinaryOf(Src, Parts[1]) : BinaryParts();
				// 二項式を被演算子に持たない前置の式の子を辿る事の返戻
				if(ts_node_is_null(Binary.Left) || !Binary.Precedence && !Binary.IsUnknown) return true;
				AddDisjoint(Src.Start(Node), Src.End(Binary.Left), Groups);
				// 包む範囲の外の被演算子を同じ巡で調べる事の返戻（包む範囲の内側の前置の式は重なる為に選ばない）
				return true;
			}
		);
		// 終了
		return;
	}

	/**
	 * 連鎖の中で解析器の結合と異なる部分式の収集関数
	 * 標準の優先順位で連鎖を組み直し，解析器の読みに無い部分式の範囲を集める（非結合の連鎖は見送理由を返す）
	 * 計算量：連鎖の演算子の数 M，直接の子の数 C，選んだ区画の数 G に対し O(M (C + log G))
	 * @param Src 解析用の本文
	 * @param Root 連鎖の根
	 * @param Groups 解析器の結合と異なる部分式の格納先
	 * @return 非結合の連鎖の見送理由（組み直せれば理由無）
	 * @throws FormatLimitExceeded 連鎖を被演算子と演算子へ分けれない場合（整形器の不具合）
	 */
	FailureReason CollectMisboundChain(const ParsedTree &Src, const TSNode Root, std::map<uint32_t, uint32_t> &Groups) {
		// 二項式の連鎖から被演算子と演算子の抽出
		const Core::SyntaxRules &Rules = Src.GetRules();
		std::vector<OperandSpan> Operands, Values;
		std::vector<std::pair<TSNode, int>> Operations, Pending;
		for(std::vector<std::pair<TSNode, bool>> Stack { { Root, false } }; !Stack.empty();) {
			const auto [Node, IsExpanded] = Stack.back();
			Stack.pop_back();
			if(const BinaryParts Parts = BinaryOf(Src, Node); !Parts.Precedence) Operands.push_back({ Src.Start(Node), Src.End(Node) });
			else if(IsExpanded) Operations.push_back({ Node, Parts.Precedence });
			else {
				Stack.push_back({ Parts.Right, false });
				Stack.push_back({ Node, true });
				Stack.push_back({ Parts.Left, false });
			}
		}
		if(Operands.size() != Operations.size() + 1) {
			throw FormatLimitExceeded("a parser binding repair could not preserve the structure", FailureReason::FormatterDefect);
		}
		const OperandSpan Whole { Src.Start(Root), Src.End(Root) };
		// 解析器の結合と異なる部分式を記録する簡約
		const auto Reduce = [&]() -> void {
			const TSNode Operation = Pending.back().first;
			Pending.pop_back();
			const OperandSpan Right = Values.back();
			Values.pop_back();
			Values.back().End = Right.End;
			const OperandSpan &Reduced = Values.back();
			const bool IsMisbound = (Src.Start(Operation) != Reduced.Start || Src.End(Operation) != Reduced.End) &&
			(Reduced.Start != Whole.Start || Reduced.End != Whole.End);
			if(IsMisbound) AddDisjoint(Reduced.Start, Reduced.End, Groups);
			// 終了
			return;
		};
		// 優先順位と結合方向に従う連鎖の簡約
		Values.push_back(Operands.front());
		for(size_t Index = 0; Index < Operations.size(); ++Index) {
			const int Precedence = Operations[Index].second;
			while(!Pending.empty()) {
				const int Top = Pending.back().second;
				// 非結合の演算子を括弧無で連鎖させた入力の見送理由の返戻
				if(Top == Precedence && Rules.NonAssociativePrecedences.contains(Precedence)) return FailureReason::NonAssociativeOperators;
				if(Top < Precedence || Top == Precedence && Rules.RightAssociativePrecedences.contains(Precedence)) break;
				Reduce();
			}
			Pending.push_back(Operations[Index]);
			Values.push_back(Operands[Index + 1]);
		}
		while(!Pending.empty()) Reduce();
		// 組み直せる連鎖の返戻
		return {};
	}
}

/**
 * 前置と二項の演算子の結合の補修関数
 * 解析器に正しい結合を読ませる括弧を部分式へ挿し，元の字面に戻す演算子を控える
 * 前置の演算子の誤りを先に巡毎に纏めて包み，無くなってから連鎖の重ならない部分式を巡毎に纏めて包む
 * 包んだ区画の内側の結付は其の巡で確定し，次の巡へ残るのは包んだ区画を含む外側の区画だけの為，巡の数は誤って結ばれた区画の入れ子の深さ（連鎖の優先順位の段数と前置の演算子の入れ子）で抑えられ，連鎖の長さに依らない
 * 計算量：節点数 N，直接の子の最大数 C，復号資料の数 R，巡の数 K に対し O(K N (C + log N + log R)) と再解析Ｋ回分
 * @param Src 解析用の本文
 * @return 非結合又は優先順位不明の連鎖の見送理由，補修出来れば理由無
 * @throws FormatLimitExceeded 括弧又は字句の置換で正しい木を得れない場合
 */
FailureReason ParseAdapter::RepairOperatorBindings(ParsedTree &Src) {
	// 演算子の種別の補修
	RestoreOperatorKinds(Src);
	// 結合が標準に揃う迄の部分式の補修
	while(true) {
		// 優先順位の分からない連鎖の見送理由の返戻
		if(HasUnknownChain(Src)) return FailureReason::UnknownOperatorPrecedence;
		std::map<uint32_t, uint32_t> Groups;
		CollectMisboundPrefixes(Src, Groups);
		if(Groups.empty()) {
			// 各連鎖の根（二項式の被演算子でない二項式）
			std::vector<TSNode> Roots;
			WalkChildrenCursor(
				Src.GetRoot(),
				[&](const TSNode Node, const TSNode Parent) -> bool {
					if(BinaryOf(Src, Node).Precedence && !BinaryOf(Src, Parent).Precedence) Roots.push_back(Node);
					// 括弧の中の別の連鎖も辿る事の返戻
					return true;
				}
			);
			for(const TSNode Root : Roots) {
				// 非結合の連鎖の見送理由の返戻
				if(const FailureReason Reason = CollectMisboundChain(Src, Root, Groups); Reason.Value != FailureReason::None) return Reason;
			}
		}
		// 解析器の結合が全て標準と同じ本文の返戻
		if(Groups.empty()) return {};
		// 誤って結ばれた部分式の囲み
		WrapExpressions(Src, Groups);
	}
}

/**
 * 構築の被呼出側と型引数の結合の補修関数
 * 解析器が実引数の無い構築の外へ結んだ後置の式を被呼出側に含め，型引数より前の単項演算子は外へ出す
 * 後置の式の連鎖は最初の呼出迄を一度に被呼出側へ含め，互いに重ならない箇所を巡毎に纏めて包む
 * 包んだ構築の内側は次の巡で読み直す為，巡の数は構築の入れ子の深さで抑えられ，連鎖の長さに依らない
 * 計算量：節点数 N，巡の数 K に対し O(K N) と再解析Ｋ回分
 * @param Src 解析用の本文
 * @throws FormatLimitExceeded 包んだ部分式を再び選ぶ場合と，挿入後の解析が期限を超えた場合
 */
void ParseAdapter::RepairConstructionBindings(ParsedTree &Src) {
	// 構築の外へ結ばれ得る後置の式か
	const auto IsPostfix = [&Src](const TSNode Node) -> bool {
		// 非空の表明・成員・添字・呼出の式かの返戻
		return Src.Is(Node, Core::Role::CallExpression) || Src.Is(Node, Core::Role::MemberExpression) ||
		Src.Is(Node, Core::Role::NonNullExpression) || Src.Is(Node, Core::Role::SubscriptExpressionNode);
	};
	// 構築の結合が揃う迄の部分式の補修
	while(true) {
		std::map<uint32_t, uint32_t> Groups;
		WalkChildrenCursor(
			Src.GetRoot(),
			[&](const TSNode Node) -> bool {
				const TSNode First = ts_node_named_child(Node, 0);
				if(Src.Is(Node, Core::Role::Instantiation) && Src.Is(First, Core::Role::UnaryOperation)) {
					// 型引数より前の単項演算子の被演算子と型引数の範囲
					if(const uint32_t Count = ts_node_named_child_count(First)) {
						AddDisjoint(Src.Start(ts_node_named_child(First, Count - 1)), Src.End(Node), Groups);
					}
					// 具体化の子を辿らない事の返戻
					return false;
				}
				// 実引数の無い構築を後置の式が包まない節点の子を辿る事の返戻
				if(
					!Src.Is(First, Core::Role::Construction) || !ts_node_is_null(Src.FieldChild(First, Core::ChildRole::Arguments)) ||
					!IsPostfix(Node)
				) return true;
				// 構築の外へ結ばれた後置の式の連鎖の最も外の式（最初の呼出で止め，其の実引数は構築の実引数）
				TSNode Top = Node;
				while(!Src.Is(Top, Core::Role::CallExpression)) {
					const TSNode Up = ts_node_parent(Top);
					if(!IsPostfix(Up) || !ts_node_eq(ts_node_named_child(Up, 0), Top)) break;
					Top = Up;
				}
				const TSNode Constructor = ts_node_named_child(First, 0), Arguments = Src.FieldChild(Top, Core::ChildRole::Arguments);
				// 被構築の式から後置の式の連鎖の終わり（呼出は実引数の前）迄
				if(!ts_node_is_null(Constructor)) {
					AddDisjoint(Src.Start(Constructor), ts_node_is_null(Arguments) ? Src.End(Top) : Src.Start(Arguments), Groups);
				}
				// 構築の中を辿らない事の返戻
				return false;
			}
		);
		// 補修の要る結合が無くなった事の終了
		if(Groups.empty()) return;
		// 誤って結ばれた部分式の囲み
		WrapExpressions(Src, Groups);
	}
}

/**
 * 文脈上の語と同じ名前の呼出を其の語の文と読んだ誤りの読直関数
 * 解析器は Python の `type(x).y = v` 等，語 `type` の後に別名の名前以外が続く文を型の別名の文と読む為，語を解析用の括弧で包んで呼出の式と読ませる
 * 計算量：節点数 N に対し O(N) と再解析１回分
 * @param Src 解析用の本文
 * @throws FormatLimitExceeded 挿入後の解析が期限を超えた場合
 */
void ParseAdapter::RepairSoftKeywordCalls(ParsedTree &Src) {
	std::map<uint32_t, uint32_t> Groups;
	WalkChildrenCursor(
		Src.GetRoot(),
		[&](const TSNode Node) -> bool {
			// 型の別名の文でない節点の子を辿る事の返戻
			if(!Src.Is(Node, Core::Role::TypeAlias)) return true;
			// 別名の名前（型の包みの中の名前か，型の仮引数を付けた名前の頭）
			const TSNode Left = Src.FieldChild(Node, Core::ChildRole::Left);
			TSNode Name = ts_node_is_null(Left) ? TSNode{} : ts_node_named_child(Left, 0);
			if(Src.Is(Name, Core::Role::TypeApplication)) Name = ts_node_named_child(Name, 0);
			if(const TSNode Keyword = ts_node_child(Node, 0); !Src.Is(Name, Core::Role::Identifier)) {
				AddDisjoint(Src.Start(Keyword), Src.End(Keyword), Groups);
			}
			// 文の中に別の文は無い為に子を辿らない事の返戻
			return false;
		}
	);
	if(!Groups.empty()) WrapExpressions(Src, Groups);
	// 終了
	return;
}
