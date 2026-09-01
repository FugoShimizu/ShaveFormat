#include "DeclEdit.hpp"
#include "NodeKind.hpp"
#include <algorithm>
#include <string_view>
#include <unordered_map>

/**
 * C# 宣言ラッパーから内部の `variable_declaration` を得る関数
 * 修飾子 (`private` / `static` / `const`) と属性 (`[Obsolete]`) は variable_declaration の前に並び，型の頭の一部として扱う
 * @param Decl 対象の宣言ノード
 * @return 内部の variable_declaration ノード（条件不一致時は引数の Decl を其のまま返戻）
 */
TSNode DeclEdit::UnwrapCSharpDecl(const TSNode Decl) {
	// 包み宣言の名前付子数
	const uint32_t Count = ts_node_named_child_count(Decl);
	// 名前付の子の無い宣言の返戻
	if(!Count) return Decl;
	// 実体の変数宣言
	const TSNode Inner = ts_node_named_child(Decl, Count - 1);
	// 最後の名前付の子が variable_declaration でない宣言の返戻
	if(std::string_view(ts_node_type(Inner)) != "variable_declaration") return Decl;
	// 実体より前が修飾だけか
	bool IsOnlyPrefix = true;
	ForEachNamedChild(
		Decl,
		[&](const TSNode Child) -> bool {
			// IsOnlyPrefix の更新
			IsOnlyPrefix = ts_node_eq(Child, Inner) || NodeKind::CSharpDeclPrefix.Contains(Child);
			// 修飾子・属性の間は走査を続ける事の返戻
			return IsOnlyPrefix && !ts_node_eq(Child, Inner);
		}
	);
	// 前に修飾子・属性だけが並ぶなら内部ノード，其れ以外は宣言其の物の返戻
	return IsOnlyPrefix ? Inner : Decl;
}

/**
 * 宣言ノードの分析関数（関数宣言や構造化束縛は無効化）
 * @param Src ソースコード
 * @param Decl 対象の宣言ノード
 * @param Language 言語
 * @return 分析結果
 */
DeclEdit::DeclSummary DeclEdit::AnalyzeDecl(const TSSource &Src, const TSNode Decl, const Lang Language) {
	// 解析結果の初期化
	DeclSummary Result {};
	Result.Start = ts_node_start_byte(Decl);
	Result.End = ts_node_end_byte(Decl);
	// 範囲照合する原稿
	const std::string &Source = Src;
	// 範囲不正又は末尾がセミコロンでない場合の返戻
	if(Result.End > Source.size() || Result.Start >= Result.End || Source[Result.End - 1] != ';') return Result;
	// Result.SemicolonStart の更新
	Result.SemicolonStart = Result.End - 1;
	// JS/TS: 先頭子トークン（let や const や var）を型接頭辞と看做し，variable_declarator のみを宣言子として扱う
	if(Language.IsJsTs()) {
		// 子ノードが無い場合の返戻
		if(!ts_node_child_count(Decl)) return Result;
		// 型接頭部の先頭子
		const TSNode FirstChild = ts_node_child(Decl, 0);
		// 先頭子が宣言キーワードでない場合の返戻
		if(!NodeKind::JsVarDeclKeyword.Contains(FirstChild)) return Result;
		// Result.TypePrefixEnd の更新
		Result.TypePrefixEnd = ts_node_end_byte(FirstChild);
		// 番兵を用いた範囲の最小値・最大値への集約
		uint32_t FirstDeclStart = Result.SemicolonStart, LastDeclEnd = Result.Start;
		// 宣言子を一つ以上見付けた印
		bool HasDeclarator = false;
		ForEachNamedChild(
			Decl,
			[&](const TSNode Child) -> void {
				// 宣言子でない子の除外
				if(std::string_view(ts_node_type(Child)) != "variable_declarator") return;
				// 宣言子先頭の更新
				if(const uint32_t ChildStart = ts_node_start_byte(Child); ChildStart < FirstDeclStart) FirstDeclStart = ChildStart;
				// 宣言子終端の更新
				if(const uint32_t ChildEnd = ts_node_end_byte(Child); ChildEnd > LastDeclEnd) LastDeclEnd = ChildEnd;
				// 宣言子有の記録
				HasDeclarator = true;
			}
		);
		// 宣言子が無い場合の返戻
		if(!HasDeclarator) return Result;
		// 宣言子範囲と成功印の確定
		Result.FirstDeclaratorStart = FirstDeclStart;
		Result.LastDeclaratorEnd = LastDeclEnd;
		Result.IsValid = true;
		// 分析結果の返戻
		return Result;
	}
	// C# の宣言包装を降り，内側の variable_declaration を解析
	const TSNode AnalyzeNode = Language == Lang::CSharp ? UnwrapCSharpDecl(Decl) : Decl;
	// C# の先頭 identifier は型として扱い，宣言子への誤分類による無限分割を防止
	const bool IsCSharpVarDecl = Language == Lang::CSharp && !ts_node_is_null(AnalyzeNode) &&
	std::string_view(ts_node_type(AnalyzeNode)) == "variable_declaration";
	// C# の共通型
	const TSNode CSharpTypeNode = IsCSharpVarDecl ? ts_node_named_child(AnalyzeNode, 0) : TSNode{};
	// 宣言部分の境界
	uint32_t FirstDeclStart = Result.SemicolonStart, LastDeclEnd = Result.Start, LastTypeEnd = Result.Start;
	// 分割可否の根拠
	bool HasDeclarator = false, HasFunc = false, HasUnknown = false;
	ForEachNamedChild(
		AnalyzeNode,
		[&](const TSNode Child) -> void {
			// 宣言子の構成要素の種別と範囲の取得
			const std::string_view ChildType(ts_node_type(Child));
			// 現在子の原文範囲
			const uint32_t ChildStart = ts_node_start_byte(Child), ChildEnd = ts_node_end_byte(Child);
			if(ChildType == "function_declarator") {
				// HasFunc の更新
				HasFunc = true;
				// 終了
				return;
			}
			// C# variable_declaration の先頭の identifier は型として扱う
			if(IsCSharpVarDecl && ts_node_eq(Child, CSharpTypeNode) && ChildType == "identifier") {
				if(ChildEnd > LastTypeEnd) LastTypeEnd = ChildEnd;
				// 型扱い識別子を処理した場合の終了
				return;
			}
			if(NodeKind::DeclDeclaratorChild.Contains(ChildType)) {
				if(ChildStart < FirstDeclStart) FirstDeclStart = ChildStart;
				if(ChildEnd > LastDeclEnd) LastDeclEnd = ChildEnd;
				HasDeclarator = true;
				// 宣言子の部分木内に function_declarator が有れば関数宣言として除外
				if(!HasFunc) {
					HasFunc = HasDescendantOf(
						Child,
						[](const TSNode Sub) -> bool {
							// 関数宣言子を含む子孫の判定
							return std::string_view(ts_node_type(Sub)) == "function_declarator";
						}
					);
				}
				// 終了
				return;
			}
			if(NodeKind::DeclTypeChild.Contains(ChildType)) {
				// 再定義・型同一性・記号の適用先を変えない様，型定義と宣言子マクロは対象外
				if(!ts_node_is_null(TSSource::FieldChild(Child, "body")) || Src.MentionsDeclaratorMacro(Child)) HasUnknown = true;
				if(ChildEnd > LastTypeEnd) LastTypeEnd = ChildEnd;
				// 型の子の場合の終了
				return;
			}
			// 平坦な初期値迄の範囲拡張（子は原文順で，既存宣言子より前へ戻らない）
			if(!HasDeclarator) HasUnknown = true;
			// LastDeclEnd の更新
			else if(ChildEnd > LastDeclEnd) LastDeclEnd = ChildEnd;
		}
	);
	// 関数宣言・宣言子無・未知の子が有る場合の返戻
	if(HasFunc || !HasDeclarator || HasUnknown) return Result;
	Result.TypePrefixEnd = LastTypeEnd;
	Result.FirstDeclaratorStart = FirstDeclStart;
	Result.LastDeclaratorEnd = LastDeclEnd;
	Result.IsValid = true;
	// 分析結果の返戻
	return Result;
}

/**
 * 宣言の宣言子の収集関数
 * @param Node 宣言ノード
 * @param TypePrefixEnd 型接頭辞の終端（此れより前の子は宣言子ではない）
 * @param Language 対象言語
 * @param Out 宣言子の格納先（先に空にする）
 */
void DeclEdit::CollectDeclarators(
	const TSNode Node,
	const uint32_t TypePrefixEnd,
	const Lang Language,
	std::vector<TSNode> &Out
) {
	Out.clear();
	// C# の型以後の子を収集し，C++ の平坦な初期値は `=` 〜 `,` 毎除外
	bool IsInInitializer = false;
	ForEachChild(
		Language == Lang::CSharp ? UnwrapCSharpDecl(Node) : Node,
		[&](const TSNode Child) -> void {
			// 初期値境界と宣言子範囲の収集
			if(!ts_node_is_named(Child)) {
				// 初期値区間の切替
				if(const std::string_view Token = ts_node_type(Child); Token == "=") IsInInitializer = true;
				else if(Token == ",") IsInInitializer = false;
				// 字句を読み終えた事の返戻
				return;
			}
			// 型接頭辞範囲内の識別子等は宣言子ではない為，除外
			if(IsInInitializer || !NodeKind::DeclDeclaratorChild.Contains(Child) || ts_node_start_byte(Child) < TypePrefixEnd) return;
			// 宣言子候補の収集
			Out.push_back(Child);
		}
	);
	// 終了
	return;
}

/**
 * 範囲内付加コメントの収集関数（WalkRoot 子孫の各範囲内コメントを PostEditAttach 化し Src 側から剥がす）
 * @param Src 編集対象ソース
 * @param WalkRoot 走査開始ノード
 * @param Ranges 取込対象バイト範囲の並び（開始の昇順に並んだ重ならない半開区間）
 * @param OldStartByte AdjustPos の基点（編集の StartPos）
 * @param CombinedStarts 編集後 NewText 内の各範囲の開始位置（Ranges と同じ順）
 * @param Out 生成された PostEditAttach の追加先
 */
void DeclEdit::CollectSubAttachments(
	TSSource &Src,
	const TSNode WalkRoot,
	const std::span<const std::pair<uint32_t, uint32_t>> Ranges,
	const uint32_t OldStartByte,
	const std::span<const uint32_t> CombinedStarts,
	std::vector<PostEditAttach> &Out
) {
	// 紐付の無い部分木は省略し，其の他は１度走査して各節点の開始位置を含む範囲を二分探索（範囲毎の再走査を回避）
	if(Ranges.empty() || !Src.HasAttachedCommentInRange(Ranges.front().first, Ranges.back().second)) return;
	// 付加コメントを持つ子孫の収集
	WalkAst(
		WalkRoot,
		[&](const TSNode SubNode) -> void {
			// 根又はコメントを持たない節点の除外
			if(ts_node_eq(SubNode, WalkRoot) || Src.GetLeading(SubNode).empty() && Src.GetTrailing(SubNode).empty()) return;
			// 子範囲の開始位置
			const uint32_t SubStart = ts_node_start_byte(SubNode);
			// 始まりが節点の始まり以下の最後の範囲
			const size_t Index = static_cast<size_t>(
				std::upper_bound(
					Ranges.begin(),
					Ranges.end(),
					SubStart,
					[](const uint32_t Pos, const std::pair<uint32_t, uint32_t> &Range) -> bool {
						// 範囲が位置より後から始まるかの返戻
						return Pos < Range.first;
					}
				) - Ranges.begin()
			);
			// 収集範囲外の節点の除外
			if(!Index || SubStart >= Ranges[Index - 1].second) return;
			// 移動前後の基点
			const uint32_t RangeStart = Ranges[Index - 1].first, CombinedStart = CombinedStarts[Index - 1];
			// 紐付コメントの取出と終端位置の保存（FindAnchor で式連鎖の内側を優先）
			Out.push_back(
				{
					OldStartByte,
					CombinedStart + (SubStart - RangeStart),
					ts_node_type(SubNode),
					Src.TakeLeading(SubNode),
					Src.TakeTrailing(SubNode),
					CombinedStart + (ts_node_end_byte(SubNode) - RangeStart)
				}
			);
		}
	);
	// 終了
	return;
}

/**
 * 宣言の内側の付随コメントの取出関数
 * 内側へ紐付いたコメントを宣言へ移して編集後も保持する
 * @param Src 編集対象ソース
 * @param Decl 宣言ノード
 * @param Language 対象言語
 * @param Leading 宣言の先行コメント（内側の物を後へ加える）
 * @param Trailing 宣言の末尾コメント（内側の物を前へ加える）
 */
void DeclEdit::TakeInnerDeclAttachments(
	TSSource &Src,
	const TSNode Decl,
	const Lang Language,
	std::vector<CommentAttach> &Leading,
	std::vector<CommentAttach> &Trailing
) {
	// 宣言子の並びを包まない言語の終了
	if(Language != Lang::CSharp) return;
	// 添付を移す内側宣言
	const TSNode Inner = FirstNamedChildOfType(Decl, "variable_declaration");
	// 内側の宣言子の並びが無い宣言の終了
	if(ts_node_is_null(Inner)) return;
	std::vector<CommentAttach> InnerLeading = Src.TakeLeading(Inner), InnerTrailing = Src.TakeTrailing(Inner);
	// 内側宣言の先行コメントの統合
	Leading.insert(Leading.end(), std::make_move_iterator(InnerLeading.begin()), std::make_move_iterator(InnerLeading.end()));
	// 外側宣言の後続コメントの統合
	InnerTrailing.insert(InnerTrailing.end(), std::make_move_iterator(Trailing.begin()), std::make_move_iterator(Trailing.end()));
	// 内側宣言へ集約した後続コメント
	Trailing = std::move(InnerTrailing);
	// 終了
	return;
}

/**
 * 独立した行の先行コメントの取出関数
 * 分割で新しい宣言の最初に来た宣言子の先行コメントの内，原文で独立した行に在った物は，宣言の前へ置く新しい宣言の先行コメントにする
 * 宣言子の前（型の後の行の途中）へ置くと，Java 等の文書化コメントが宣言に付かず，統合と分割を経る次の巡で置場も変わる
 * @param Leads 宣言子の先行コメント（取り出した物は除く）
 * @return 取り出したコメント（先頭から続く独立した行のコメント）
 */
std::vector<CommentAttach> DeclEdit::TakeOwnLineLeads(std::vector<CommentAttach> &Leads) {
	// OwnLineEnd の初期化
	const std::vector<CommentAttach>::iterator OwnLineEnd = std::find_if(
		Leads.begin(),
		Leads.end(),
		[](const CommentAttach &Comment) -> bool {
			// 原文で行の途中に在ったコメントかの返戻
			return !Comment.IsStandalone;
		}
	);
	std::vector<CommentAttach> Taken(std::make_move_iterator(Leads.begin()), std::make_move_iterator(OwnLineEnd));
	// Leads からの要素除去
	Leads.erase(Leads.begin(), OwnLineEnd);
	// 取り出したコメントの返戻
	return Taken;
}

/**
 * 編集の整列順序の判定関数
 * @param Lhs 比較の左辺
 * @param Rhs 比較の右辺
 * @return Lhs を先に並べるべき場合に true（StartPos 昇順，同一 Start なら範囲が広い＝ End が大きい方を先に）
 */
bool DeclEdit::IsEditBefore(const TextEdit &Lhs, const TextEdit &Rhs) {
	// 整列順序の判定結果の返戻
	return Lhs.StartPos == Rhs.StartPos ? Lhs.EndPos > Rhs.EndPos : Lhs.StartPos < Rhs.StartPos;
}

/**
 * テキスト置換のみの編集適用関数（ts_tree_edit を介さず全体再構文解析経路で構文木を再構築）
 * @param Src 対象ソース
 * @param Edits 呼出元で整列・重複除去済の編集列
 */
void DeclEdit::ApplyEditsNoTreeEdit(TSSource &Src, std::vector<TextEdit> &Edits) {
	// 木の移動後の枝刈りで紐付を落とさない様，開始位置索引を更新しない増分編集は不使用
	if(Edits.empty()) return;
	// 編集前の原稿
	const std::string &Source = Src;
	// 木編集を伴わない置換結果
	std::string Result;
	// Result の領域確保
	Result.reserve(Source.size());
	// 未複写部分の先頭
	uint32_t Pos = 0;
	for(const TextEdit &Edit : Edits) {
		if(Edit.StartPos < Pos || Edit.StartPos > Edit.EndPos || Edit.StartPos == Edit.EndPos && Edit.NewText.empty()) continue;
		if(const std::string_view Cur(Source.data() + Edit.StartPos, Edit.EndPos - Edit.StartPos); Cur == Edit.NewText) continue;
		// Result への本文追加
		Result.append(Source.data() + Pos, Edit.StartPos - Pos);
		Result.append(Edit.NewText);
		// Pos の更新
		Pos = Edit.EndPos;
	}
	// 錨は必ず先頭より後の為，コメントを先頭へ飛ばす写像破損の徴候（Pos が０の儘の編集）は不採用
	if(!Pos) return;
	Result.append(Source.data() + Pos, Source.size() - Pos);
	// ts_tree_edit を呼ばないので次回の再構文解析は全体構文解析となり AttachedStartBytes との整合が保たれる
	Src.Assign(std::move(Result));
	// 終了
	return;
}

/**
 * 編集適用後バイト位置基準の付加情報再束縛関数（編集範囲外の既存付加情報をスナップショット化し再束縛）
 * @param Src 対象ソース
 * @param Edits 適用する編集列（本関数内で破壊的に消費される）
 * @param PostAttaches 再束縛するマージ由来の付加情報（紐付は移動で消費される）
 */
void DeclEdit::ApplyByteRebind(TSSource &Src, std::vector<TextEdit> &Edits, std::vector<PostEditAttach> &PostAttaches) {
	// 編集が無い場合の返戻
	if(Edits.empty()) return;
	// 開始昇順・同開始は終端降順で重複除去し，外側の編集に含まれる内側の編集を破棄
	std::sort(Edits.begin(), Edits.end(), IsEditBefore);
	// 残す編集の書込位置
	size_t WriteIdx = 0;
	// 直前に残した編集の終端
	uint32_t LastEnd = 0;
	for(size_t ReadIdx = 0; ReadIdx < Edits.size(); ++ReadIdx) {
		if(Edits[ReadIdx].StartPos < LastEnd) continue;
		// LastEnd の更新
		LastEnd = Edits[ReadIdx].EndPos;
		if(WriteIdx != ReadIdx) Edits[WriteIdx] = std::move(Edits[ReadIdx]);
		// WriteIdx の格納先の準備
		++WriteIdx;
	}
	// Edits の寸法更新
	Edits.resize(WriteIdx);
	// 累積差分を事前計算し（CumulativeDelta[i] = Edits[0..i-1] の差分合計），AdjustPos の線形走査を二分探索化
	std::vector<int64_t> CumulativeDelta(Edits.size() + 1, 0);
	for(size_t Idx = 0; Idx < Edits.size(); ++Idx) {
		CumulativeDelta[Idx + 1] = CumulativeDelta[Idx] + static_cast<int64_t>(Edits[Idx].NewText.size()) -
		static_cast<int64_t>(Edits[Idx].EndPos - Edits[Idx].StartPos);
	}
	// AdjustPos の初期化
	const auto AdjustPos = [&Edits, &CumulativeDelta](const uint32_t OldPos) -> uint32_t {
		// StartPos < OldPos を満たす Edit の件数を lower_bound で取得（計算量 O(log N)）
		const size_t Count = static_cast<size_t>(
			std::lower_bound(
				Edits.begin(),
				Edits.end(),
				OldPos,
				[](const TextEdit &Edit, const uint32_t Val) -> bool {
					// 編集位置を上限にする二分探索の比較
					return Edit.StartPos < Val;
				}
			) - Edits.begin()
		);
		// 編集範囲外の旧バイト位置を編集後の新バイト位置に変換の返戻
		return static_cast<uint32_t>(static_cast<int64_t>(OldPos) + CumulativeDelta[Count]);
	};
	// 編集範囲外の付加情報をスナップショット化し，適用後に再束縛（範囲内はマージで消費済の為，除外）
	struct ByteAttach {
		uint32_t StartByte;
		uint32_t EndByte; // 同一開始バイト且つ同一型のネスト候補を区別する終端
		const char *const NodeType; // 再構文解析後も同一な解析器所有の静的文字列
		std::vector<CommentAttach> Leading;
		std::vector<CommentAttach> Trailing;
	};
	std::vector<ByteAttach> Filtered;
	// 紐付コメントが１件も無ければ全木走査自体を省略（Filtered は空のまま）
	if(Src.HasAttachments()) {
		// 編集範囲外の紐付節点を訪問順に収集（剪定に使う開始位置集合を保つ為，取出は全走査の後）
		std::vector<TSNode> AttachedNodes;
		const auto CollectNode = [&](const TSNode Node) -> void {
			// コメント付節点の編集範囲への包含判定
			if(Src.GetLeading(Node).empty() && Src.GetTrailing(Node).empty()) return;
			// 節点の開始位置取得
			const uint32_t StartByte = ts_node_start_byte(Node);
			// 直前の編集を二分探索し，始点・終点共に内包する節点だけ除外（根等の紐付を保護）
			if(
				const std::vector<TextEdit>::const_iterator Upper = std::upper_bound(
					Edits.begin(),
					Edits.end(),
					StartByte,
					[](const uint32_t Pos, const TextEdit &Edit) -> bool {
						// 対象位置の処理
						return Pos < Edit.StartPos;
					}
				); Upper != Edits.begin() && StartByte < (Upper - 1)->EndPos && ts_node_end_byte(Node) <= (Upper - 1)->EndPos
			) return; // 既存編集範囲へ含まれる節点の返戻
			// 編集範囲内のコメント付節点の記録
			AttachedNodes.push_back(Node);
		};
		CollectNode(Src.GetRoot());
		// 紐付開始位置の無い部分木の枝刈り（終端＋１で零幅錨を含め，数万節点から通常百件程度の紐付だけ探索）
		WalkChildrenCursor(
			Src.GetRoot(),
			[&](const TSNode Node) -> bool {
				// コメントの無い部分木へ降りない事の返戻
				if(!Src.HasAttachedCommentInRange(ts_node_start_byte(Node), ts_node_end_byte(Node) + 1)) return false;
				// 対象節点の収集
				CollectNode(Node);
				// 範囲内に紐付が有る為，内部へ降下の返戻
				return true;
			}
		);
		// 局面 2：紐付コメントを移動で取出（直後の ClearAttachments で破棄される為，複製は不要）
		Filtered.reserve(AttachedNodes.size());
		for(const TSNode Node : AttachedNodes) {
			Filtered.push_back(
				{ ts_node_start_byte(Node), ts_node_end_byte(Node), ts_node_type(Node), Src.TakeLeading(Node), Src.TakeTrailing(Node) }
			);
		}
	}
	// パス基盤の再束縛を抑止しつつ適用
	Src.ClearAttachments();
	ApplyEditsNoTreeEdit(Src, Edits);
	// 編集後の解析失敗時の返戻
	if(!Src.IsParsed()) return;
	// 再束縛位置だけの索引登録と調整位置の再利用
	std::unordered_map<uint32_t, std::vector<TSNode>> ByteToNode;
	ByteToNode.reserve(Filtered.size() + PostAttaches.size());
	for(ByteAttach &Entry : Filtered) {
		Entry.StartByte = AdjustPos(Entry.StartByte);
		Entry.EndByte = AdjustPos(Entry.EndByte);
		ByteToNode.try_emplace(Entry.StartByte);
	}
	for(PostEditAttach &Entry : PostAttaches) {
		Entry.OldStartByte = AdjustPos(Entry.OldStartByte);
		ByteToNode.try_emplace(Entry.OldStartByte + Entry.OffsetInReplacement);
	}
	// 再束縛対象が１件も無ければ全木走査自体を省略（後続の再束縛ループは両方共空で何もしない）
	if(!ByteToNode.empty()) {
		WalkAst(
			Src.GetRoot(),
			[&](const TSNode Node) -> void {
				// 事前登録済のバイト位置のみ候補ノードを蓄積（検索１回で存在判定と挿入先取得を兼用）
				if(
					const std::unordered_map<uint32_t, std::vector<TSNode>>::iterator Iter = ByteToNode.find(ts_node_start_byte(Node));
					Iter != ByteToNode.end()
				) Iter->second.push_back(Node);
			}
		);
	}
	// 編集後の探索根
	const TSNode CurRoot = Src.GetRoot();
	// 同開始・型では終端一致を優先し，不一致なら最初の同型へ退避（式連鎖の内外を区別）
	const auto FindAnchor = [&](const uint32_t NewByte, const uint32_t NewEndByte, const char *const NodeType) -> TSNode {
		// 型の無い錨の空の返戻
		if(!NodeType) return {};
		// 型名はビュー化して候補毎の長さ計算（strlen 相当）を１回に固定する
		const std::string_view WantType(NodeType);
		// 開始位置一致候補の検索
		if(
			const std::unordered_map<uint32_t, std::vector<TSNode>>::const_iterator Iter = ByteToNode.find(NewByte);
			Iter != ByteToNode.end()
		) {
			// 種別不一致時の同範囲節点
			TSNode Fallback {};
			// 型と終端の一致候補の選別
			for(const TSNode Cand : Iter->second) if(std::string_view(ts_node_type(Cand)) == WantType) {
				// 終端も一致する候補の返戻
				if(NewEndByte && ts_node_end_byte(Cand) == NewEndByte) return Cand;
				if(ts_node_is_null(Fallback)) Fallback = Cand;
			}
			// 型だけ一致する候補の返戻
			if(!ts_node_is_null(Fallback)) return Fallback;
		}
		// 探索根への退避
		// コメントだけで根の開始位置が動いても，根の型が一致すれば新しい根へ紐付
		if(std::string_view(ts_node_type(CurRoot)) == WantType) return CurRoot;
		// 該当ノードが見付からない為 null の返戻
		return {};
	};
	// 既存（マージ対象外）の付加情報を再束縛（本ループが Filtered の最終消費者の為，紐付コメントは移動で取り込む）
	for(ByteAttach &Entry : Filtered) {
		const TSNode Anchor = FindAnchor(Entry.StartByte, Entry.EndByte, Entry.NodeType);
		if(ts_node_is_null(Anchor)) continue;
		if(!Entry.Leading.empty()) Src.AddLeading(Anchor, std::move(Entry.Leading));
		if(!Entry.Trailing.empty()) Src.AddTrailing(Anchor, std::move(Entry.Trailing));
	}
	// 統合・分割由来の行頭・行末コメントの再紐付（EndOffsetInReplacement が有れば終端一致を優先し，０なら先頭候補へ退避）
	for(PostEditAttach &Entry : PostAttaches) {
		const TSNode Anchor = FindAnchor(
			Entry.OldStartByte + Entry.OffsetInReplacement,
			Entry.EndOffsetInReplacement ? Entry.OldStartByte + Entry.EndOffsetInReplacement : 0,
			Entry.NodeType
		);
		if(ts_node_is_null(Anchor)) continue;
		if(!Entry.Leading.empty()) Src.AddLeading(Anchor, std::move(Entry.Leading));
		if(!Entry.Trailing.empty()) Src.AddTrailing(Anchor, std::move(Entry.Trailing));
	}
	// 終了
	return;
}

/**
 * 宣言の分割の適用関数
 * 候補の集め方（場所と条件）と副宣言の群の切り方だけを呼出側が決め，置換文字列の組立とコメントの運び・再紐付は共通にする
 * @param Src 対象のソース（破壊的に書き換える）
 * @param Language 対象言語
 * @param Collect 候補の収集関数（反復毎に呼ぶ）
 * @param GroupEnd 副宣言の群の終端を返す関数（空なら宣言子毎に分割）
 * @param KeepsIndent 副宣言の行頭へ原文のインデントを写すか
 * @param AfterApply 反復毎の適用の後の処理（空なら何もしない）
 * @return 分割を１回以上適用したか
 */
bool DeclEdit::SplitDeclarations(
	TSSource &Src,
	const Lang Language,
	const std::function<void(std::vector<SplitCandidate> &)> &Collect,
	const std::function<size_t(const std::vector<size_t> &, const size_t, const size_t)> &GroupEnd,
	const bool KeepsIndent,
	const std::function<void(TSSource &)> &AfterApply
) {
	// 範囲が重なる宣言は最深だけ適用し，再解析後の次巡で外側を処理
	bool IsApplied = false;
	std::vector<SplitCandidate> Cands;
	std::vector<uint8_t> Keep;
	std::vector<size_t> Stack;
	std::vector<TextEdit> Edits;
	std::vector<PostEditAttach> PostAttaches;
	// 各宣言子の範囲（宣言子自身と後に続く初期化の字句）と，宣言子毎のコメント
	struct DeclItem {
		uint32_t Start;
		uint32_t End;
		const char *AnchorType;
		std::vector<CommentAttach> Leading;
		std::vector<CommentAttach> Trailing;
	};
	std::vector<DeclItem> Items;
	std::vector<size_t> Widths;
	// コメントの置場を引く為の範囲と，置換文字列の中の位置（候補毎に空にして使い回す）
	std::vector<std::pair<uint32_t, uint32_t>> ItemRanges;
	std::vector<uint32_t> CombinedStarts;
	// 候補の反復収集
	while(true) {
		Cands.clear();
		Collect(Cands);
		if(Cands.empty()) break;
		// 範囲の重なる（一方が他方を含む）候補は外側を１反復先送りする（候補は構文木の先行順で，祖先が先に並ぶ）
		Keep.assign(Cands.size(), 1);
		Stack.clear();
		for(size_t CandIdx = 0; CandIdx < Cands.size(); ++CandIdx) {
			while(!Stack.empty() && Cands[Stack.back()].Decl.End < Cands[CandIdx].Decl.Start) Stack.pop_back();
			if(!Stack.empty()) Keep[Stack.back()] = 0;
			Stack.push_back(CandIdx);
		}
		// 今回編集の初期化
		Edits.clear();
		PostAttaches.clear();
		// 候補範囲を切り出す原稿
		const std::string &Source = Src;
		// 採用候補毎の置換作成
		for(size_t CandIdx = 0; CandIdx < Cands.size(); ++CandIdx) {
			if(!Keep[CandIdx]) continue;
			const SplitCandidate &Cand = Cands[CandIdx];
			// 宣言の型接頭辞取得
			const std::string_view TypePrefix(Source.data() + Cand.Decl.Start, Cand.Decl.TypePrefixEnd - Cand.Decl.Start);
			// インデントは構文木に現れない為，原文の宣言の行頭迄遡って写す
			const uint32_t LineStart = KeepsIndent ? TextEdit::LineStartOf(Source, Cand.Decl.Start) : Cand.Decl.Start;
			const std::string_view IndentText(Source.data() + LineStart, Cand.Decl.Start - LineStart);
			// 同範囲の親も含め，先行コメントを最初の副宣言の前，末尾を最後の後へ移動
			std::vector<CommentAttach> DeclLeading = Src.TakeLeading(Cand.Node), DeclTrailing = Src.TakeTrailing(Cand.Node);
			TakeInnerDeclAttachments(Src, Cand.Node, Language, DeclLeading, DeclTrailing);
			// 包装宣言のコメント統合
			if(!ts_node_is_null(Cand.Wrapper)) {
				std::vector<CommentAttach> WrapperLeading = Src.TakeLeading(Cand.Wrapper), WrapperTrailing = Src.TakeTrailing(Cand.Wrapper);
				DeclLeading.insert(
					DeclLeading.end(),
					std::make_move_iterator(WrapperLeading.begin()),
					std::make_move_iterator(WrapperLeading.end())
				);
				DeclTrailing.insert(
					DeclTrailing.end(),
					std::make_move_iterator(WrapperTrailing.begin()),
					std::make_move_iterator(WrapperTrailing.end())
				);
			}
			// 平坦な初期値も含め，次の宣言子又は `;` の前のコンマ・空白迄を宣言子範囲化
			const uint32_t DeclEndByte = Cand.Decl.End - 1;
			// 宣言子情報の初期化
			Items.clear();
			Widths.clear();
			Items.reserve(Cand.Declarators.size());
			if(GroupEnd) Widths.reserve(Cand.Declarators.size());
			// 宣言子範囲とコメントの収集
			for(size_t Idx = 0; Idx < Cand.Declarators.size(); ++Idx) {
				const TSNode Declarator = Cand.Declarators[Idx];
				const uint32_t DeclStart = ts_node_start_byte(Declarator);
				const uint32_t NextStart = Idx + 1 < Cand.Declarators.size() ? ts_node_start_byte(Cand.Declarators[Idx + 1]) : DeclEndByte;
				const uint32_t End = TextEdit::SkipCharsLeftBounded(Source, NextStart, DeclStart, " \t\n,");
				DeclItem Item;
				Item.Start = DeclStart;
				Item.End = End;
				Item.AnchorType = ts_node_type(Declarator);
				Item.Leading = Src.TakeLeading(Declarator);
				Item.Trailing = Src.TakeTrailing(Declarator);
				Items.push_back(std::move(Item));
				// 幅は群の切り方が使う時だけ測る
				if(GroupEnd) Widths.push_back(TextEdit::ContentChars(std::string_view(Source.data() + DeclStart, End - DeclStart)));
			}
			// 副宣言の群（半開区間）は呼出側の切り方で決める（コメントの有無・位置では変えず，コメント非依存・冪等を保つ）
			const size_t TypePrefixWidth = GroupEnd ? TextEdit::ContentChars(TypePrefix) : 0;
			// 置換文とコメント位置の作成（型の頭の範囲内のコメントは最初の副宣言の型の頭へ再配置）
			std::string Replacement;
			ItemRanges.assign(1, { Cand.Decl.Start, Cand.Decl.TypePrefixEnd });
			CombinedStarts.assign(1, 0);
			ItemRanges.reserve(Items.size() + 1);
			CombinedStarts.reserve(Items.size() + 1);
			// 置換領域の確保
			Replacement.reserve((TypePrefix.size() + IndentText.size() + 3) * Items.size() + Cand.Decl.End - Cand.Decl.Start);
			// 副宣言群毎の置換組立
			for(size_t Begin = 0; Begin < Items.size();) {
				// 群に入れない最初の宣言子（切り方の指定が無ければ宣言子毎に１つの副宣言にする）
				const size_t End = GroupEnd ? GroupEnd(Widths, TypePrefixWidth, Begin) : Begin + 1;
				if(Begin) {
					Replacement += '\n';
					Replacement.append(IndentText);
				}
				const uint32_t SubLineOffset = static_cast<uint32_t>(Replacement.size());
				// 型接頭辞の書込
				Replacement.append(TypePrefix);
				Replacement += ' ';
				// 最初の副宣言の前へ宣言の先行コメントを置く
				if(!Begin && !DeclLeading.empty()) {
					PostAttaches.push_back({ Cand.Decl.Start, SubLineOffset, Cand.TypeRaw, std::move(DeclLeading), {} });
				}
				// 群内宣言子の連結
				for(size_t Idx = Begin; Idx < End; ++Idx) {
					if(Idx > Begin) Replacement.append(", ");
					const uint32_t DeclCombinedStart = static_cast<uint32_t>(Replacement.size());
					Replacement.append(Source.data() + Items[Idx].Start, Items[Idx].End - Items[Idx].Start);
					// 行内コメントは行内を保ち，後の副宣言の先頭に属する独立行コメントは宣言前へ配置
					std::vector<CommentAttach> Leads = std::move(Items[Idx].Leading), Trails = std::move(Items[Idx].Trailing);
					if(Begin && Idx == Begin) if(std::vector<CommentAttach> OwnLine = TakeOwnLineLeads(Leads); !OwnLine.empty()) {
						PostAttaches.push_back({ Cand.Decl.Start, SubLineOffset, Cand.TypeRaw, std::move(OwnLine), {} });
					}
					if(!Leads.empty() || !Trails.empty()) {
						PostAttaches.push_back({ Cand.Decl.Start, DeclCombinedStart, Items[Idx].AnchorType, std::move(Leads), std::move(Trails) });
					}
					ItemRanges.push_back({ Items[Idx].Start, Items[Idx].End });
					// 統合済宣言の開始位置の記録
					CombinedStarts.push_back(DeclCombinedStart);
				}
				Replacement += ';';
				// 最後の副宣言の後へ宣言の末尾コメントを置く
				if(End == Items.size() && !DeclTrailing.empty()) {
					PostAttaches.push_back({ Cand.Decl.Start, SubLineOffset, Cand.TypeRaw, {}, std::move(DeclTrailing) });
				}
				Begin = End;
			}
			// 宣言子の中（初期化子の関数・ラムダの本体等）のコメントも分割後の位置へ置き直す
			CollectSubAttachments(Src, Cand.Node, ItemRanges, Cand.Decl.Start, CombinedStarts, PostAttaches);
			TextEdit::Push(Cand.Decl.Start, Cand.Decl.End, std::move(Replacement), Edits);
		}
		if(Edits.empty()) break;
		// 収束の判定：全ての編集が原文と同じなら不動点（病的な構文木での無限反復を防ぐ）
		bool IsAnyProgress = false;
		// 原文との差の確認
		for(const TextEdit &Edit : Edits) {
			if(const std::string_view Original(Source.data() + Edit.StartPos, Edit.EndPos - Edit.StartPos); Edit.NewText != Original) {
				IsAnyProgress = true;
				break;
			}
		}
		if(!IsAnyProgress) break;
		// 分割は兄弟の構造を変える為，経路でなくバイト位置でコメントを再紐付する
		ApplyByteRebind(Src, Edits, PostAttaches);
		IsApplied = true;
		if(!Src.IsParsed()) break;
		if(AfterApply) AfterApply(Src);
		if(!Src.IsParsed()) break;
	}
	// 分割を適用したかの返戻
	return IsApplied;
}
