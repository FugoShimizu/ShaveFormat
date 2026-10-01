#include "DocCommentSynchronizer.hpp"
#include "../Util/CommentNormalizer.hpp"
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 文書化コメントの生成と補正関数（整形仕様 7.4）
	 * 関数の署名と既存の文書を突合し，不足の仮引数・戻値の項目を補い余分を除く（説明文は保持する）
	 * 定義の直前の通常コメントは文書の説明へ統合する
	 * 計算量：節点数 V とコメントの数 K に対し，文書の本文の長さを除いて O(V + K)
	 * @param Syntax 正規化後の構文
	 * @param Comments 錨の節点毎のコメント
	 */
	void DocCommentSynchronizer::Synchronize(const SyntaxModel &Syntax, CommentRepository &Comments) {
		// 文書の記法と項目書式の取得
		const NotationRules &Rules = Syntax.Rules().Notation;
		const DocumentationStyle Dialect = Rules.Documentation;
		// 新しい文書の採用記法
		const CommentOutputForm *const NewForm = Rules.AdoptedFormOf(CommentUse::Document);
		// 新しい文書が通常の説明の記録に為るか（文書文字列はソース側に残り，処理系の読む文書属性は指示コメントに為る為，其の不足は静的検査が報告する，整形仕様 7.4）
		const bool CanCreate = NewForm && VocabularyOf(Dialect, DocDescription::Summary).Policy != DocStylePolicy::SourceOwned &&
		!Rules.DocumentCommentIsAttribute;
		// 新しい文書を書く行形式の記号（囲み形式は空）
		const std::string_view NewMarker = NewDocumentMarker(Rules);
		// 組み直した行形式の文書へ振る群（投影が振った群の後の番号，整形仕様 7.3 の群構成の変更）
		CommentGroupId NextGroup = 0;
		for(const CommentRecord &Record : Comments.GetRecords()) {
			if(Record.Format.Group != NoCommentGroup) NextGroup = std::max(NextGroup, Record.Format.Group + 1);
		}
		// 錨毎の作業中の先行の記録と最後に戻した順（最初に触れた時に器から読む）
		struct LeadingRecords {
			std::vector<CommentRecord> Records; // 先行の記録
			size_t Added = 0; // 最後に戻した順（戻していなければ０）
		};
		std::unordered_map<NodeId, LeadingRecords> LeadingOf;
		size_t AddCount = 0;
		// 錨の作業中の先行の記録
		const auto LeadingAt = [&Comments, &LeadingOf](const NodeId Anchor) -> LeadingRecords & {
			const auto [Found, IsFirst] = LeadingOf.try_emplace(Anchor);
			if(IsFirst) for(CommentRecord &Record : Comments.Get(Anchor)) {
				if(Record.Placement.Side == CommentPlacement::Before) Found->second.Records.push_back(std::move(Record));
			}
			// 錨の先行の記録の返戻
			return Found->second;
		};
		// 錨の先行の記録の取出
		const auto TakeLeading = [&LeadingAt](const NodeId Anchor) -> std::vector<CommentRecord> {
			// 取り出した記録の返戻
			return std::exchange(LeadingAt(Anchor).Records, {});
		};
		// 錨への先行の記録の追加（後の取出の対象にもする）
		const auto AddLeading = [&LeadingAt, &AddCount](const NodeId Anchor, std::vector<CommentRecord> &&Added) -> void {
			LeadingRecords &Target = LeadingAt(Anchor);
			for(CommentRecord &Record : Added) {
				Record.Anchor = Anchor;
				Record.Placement.Side = CommentPlacement::Before;
				Target.Records.push_back(std::move(Record));
			}
			Target.Added = ++AddCount;
		};
		// 文書化対象の定義毎の同期（投影が作った定義を原文順に辿り，入れ子の定義も含む，整形仕様 3.2）
		const std::span<const DocumentationSite> Sites = Comments.GetDocumentationSites();
		// 節点毎の其の節点を頭（文書の錨と其れを先頭に持つ包装）又は見出の中に持つ文書化対象の定義
		const TargetTables Tables = TablesOf(Syntax);
		// 錨の後の独立した通常コメントの内，構文の順で後に続く物（後置へ変えた構文の語は飛ばし，其の構文の後に続く物）が定義の頭・見出の中の物と見出に続く定義の本体は，定義の直前又は見出の中へ置かれる為（整形仕様 7.6），其の定義の先行コメント・見出の中のコメントとして同じ回で同期する（整形仕様 7.4）
		// 空の本体の物と，Go の最上位の空行の前に留める物は除く
		const auto PlacedBefore = [&Syntax, &Tables](const NodeId Anchor) -> NodeId {
			NodeId Up = Anchor, Next = Syntax.FollowingOf(Anchor);
			while(Next != NoNode && !Syntax.IsNamed(Next) && !Syntax.Is(Next, Role::CloserToken) && Syntax.ParentOf(Up) != NoNode) {
				Next = Syntax.FollowingOf(Up = Syntax.ParentOf(Up));
			}
			// 定義へ統合する位置の後続（開きの字句の無い本体は本体の中の為に除く）か，統合しなければ無しの返戻
			return Next != NoNode && (
				Tables.Definitions[Next] != NoNode ||
				Tables.Definitions[Anchor] != NoNode && Syntax.Is(Next, Role::DefinitionBody) && Syntax.IsBracedBody(Next) &&
				Syntax.ParentOf(Next) == Tables.Definitions[Anchor]
			) ? Next : NoNode;
		};
		// 移す記録の番号と新しい錨と其の錨との間の無名の字句の数
		struct MovedRecord {
			size_t Index;
			NodeId Anchor;
			uint16_t TokensBefore;
		};
		std::vector<MovedRecord> Moved;
		const std::span<const CommentRecord> Records = Comments.GetRecords();
		for(size_t Index = 0; Index < Records.size(); ++Index) {
			const CommentRecord &Record = Records[Index];
			if(
				Record.Kind != CommentKind::OwnLine || Record.Role != CommentRole::Presentation || Record.Placement.IsEmptyBody ||
				Record.Placement.IsRemovedCaseBody || Syntax.Is(Syntax.ParentOf(Record.Anchor), Role::AdjacentDocumentSequence)
			) continue;
			if(Record.Placement.Side == CommentPlacement::After) {
				if(const NodeId Next = PlacedBefore(Record.Anchor); Next != NoNode) Moved.push_back({ Index, Next, 0 });
				continue;
			}
			// 定義の本体の最初の子の前の物の内，本体を開く字句より前に在った物（見出と本体の間の物，開きの字句の無い本体は本体の中）は本体の前の物とする
			if(
				const NodeId Body = Syntax.ParentOf(Record.Anchor);
				Record.Placement.Side == CommentPlacement::Before && Syntax.Is(Body, Role::DefinitionBody) && Syntax.IsBracedBody(Body) &&
				Syntax.NamedChild(Body, 0) == Record.Anchor && Record.Placement.TokensBefore >= Syntax.GetNode(Record.Anchor).Index &&
				Tables.Definitions[Syntax.ParentOf(Body)] != NoNode
			) Moved.push_back({ Index, Body, static_cast<uint16_t>(Record.Placement.TokensBefore - Syntax.GetNode(Record.Anchor).Index) });
		}
		// 定義の前へ移す記録の錨と配置関係の更新
		if(!Moved.empty()) {
			std::vector<CommentRecord> Updated(Records.begin(), Records.end());
			for(const MovedRecord &Move : Moved) {
				Updated[Move.Index].Anchor = Move.Anchor;
				Updated[Move.Index].Placement.Side = CommentPlacement::Before;
				Updated[Move.Index].Placement.TokensBefore = Move.TokensBefore;
			}
			Comments.Update(std::move(Updated));
		}
		for(const DocumentationSite &Site : Sites) {
			const NodeId Node = Site.Anchor;
			// 文書を付けない定義と，署名を信用出来ない誤りを含む定義・誤り区画の後の定義（保護した字面の中へ独立行を作れず整形の度に増える）の読飛
			if(
				!SignatureExtractor::IsDocumentable(Syntax, Node) || Syntax.GetNode(Node).Facts.Has(ConstraintKind::ContainsError) ||
				Syntax.Is(Syntax.PrevSibling(Node), Role::Error)
			) continue;
			NodeId Anchor = SignatureExtractor::DocAnchor(Syntax, Node);
			std::vector<CommentRecord> OldLeading = TakeLeading(Anchor);
			// 錨を先頭に持つ包装の先行コメントの探索
			for(NodeId Up = Anchor; OldLeading.empty() && Syntax.ParentOf(Up) != NoNode && !Syntax.GetNode(Up).Index;) {
				Up = Syntax.ParentOf(Up);
				OldLeading = TakeLeading(Up);
				if(!OldLeading.empty()) Anchor = Up;
			}
			// 文書へ取り込めるのは独立コメントだけで，行末・文中のコメントは種類を保つ為に其の儘錨へ戻す
			const std::vector<CommentRecord>::iterator Split = std::stable_partition(
				OldLeading.begin(),
				OldLeading.end(),
				[](const CommentRecord &Comment) -> bool {
					// 独立コメントかの返戻
					return Comment.Kind == CommentKind::OwnLine;
				}
			);
			std::vector<CommentRecord> Others(std::make_move_iterator(Split), std::make_move_iterator(OldLeading.end()));
			OldLeading.erase(Split, OldLeading.end());
			// 其の儘戻す文書
			// 錨の直前の行内の文書化コメント (`/** ... */ int m()`) は行の途中から動かさない
			// Kotlin は前の成員の後に「囲みコメントの行」と「行頭の囲みコメントに続く成員」が並ぶと解析器が読めず，骨組みが其の形を作る為，錨の直前の文中コメントが在れば補わない
			// 項目を持たない godoc は，文書が在れば同期する物が無い
			const bool IsKeptAsIs = std::any_of(
				Others.begin(),
				Others.end(),
				[&Rules](const CommentRecord &Comment) -> bool {
					// 行内の文書化コメントかの返戻
					return IsDocBlock(Rules, CommentRepository::Compose(CommentRepository::OutputFormOf(Comment)));
				}
			) || Rules.InlineCommentPrecludesDocument && std::any_of(
				Others.begin(),
				Others.end(),
				[](const CommentRecord &Comment) -> bool {
					// 錨の直前へ行内で置く文中コメントかの返戻
					return Comment.Kind == CommentKind::Inline;
				}
			) || VocabularyOf(Dialect, DocDescription::Summary).Policy == DocStylePolicy::NameSummary && std::any_of(
				OldLeading.begin(),
				OldLeading.end(),
				[&Rules](const CommentRecord &Comment) -> bool {
					// 文書化コメントかの返戻
					return IsDocComment(Rules, CommentRepository::Compose(CommentRepository::OutputFormOf(Comment)));
				}
			);
			// 錨自身の独立コメントの数（其の後は見出の記録の写し）
			const size_t OwnCount = OldLeading.size();
			// 既存のコメントを其の儘錨へ戻す処理（見出の記録は元の錨に残した儘とし，写しを捨てる）
			const auto RestoreAsIs = [&]() -> void {
				OldLeading.erase(OldLeading.begin() + static_cast<std::ptrdiff_t>(OwnCount), OldLeading.end());
				OldLeading.insert(OldLeading.end(), std::make_move_iterator(Others.begin()), std::make_move_iterator(Others.end()));
				AddLeading(Anchor, std::move(OldLeading));
			};
			// 通常の説明の文書を持つか（指示コメントとソース側に保持した文書は改変しない）
			const bool HasPresentationDocument = std::any_of(
				OldLeading.begin(),
				OldLeading.end(),
				[&Rules](const CommentRecord &Comment) -> bool {
					// 通常の説明の文書化コメントかの返戻
					return Comment.Role == CommentRole::Presentation &&
					IsDocComment(Rules, CommentRepository::Compose(CommentRepository::OutputFormOf(Comment)));
				}
			);
			if(IsKeptAsIs || !CanCreate && !HasPresentationDocument) {
				RestoreAsIs();
				// 既存のコメントを戻して次の定義へ進む
				continue;
			}
			// 見出（前置の属性と本体の手前迄の子）の中の先行の独立した文書と文書へ統合し得る独立コメントの写し（整形仕様 7.4 は行幅・最終字面での判断を禁じる為，構文から決める）
			// 元の記録は同期を確定する迄其の錨に残し，指示コメント等の他の記録は元の錨の儘とする
			const auto IsHeaderTaken = [&Rules](const CommentRecord &Comment) -> bool {
				// 見出から文書の側へ移す記録かの返戻
				return IsDedicatedDocument(Rules, Comment) || IsMergeable(Comment);
			};
			// 前置の属性（兄弟の装飾子等）と本体の手前迄の子と，波括弧で開く本体（本体の前に在る物だけを見る，開きの字句の無い本体の前の物は本体の中）
			std::vector<NodeId> HeaderAnchors, HeaderParts;
			NodeId Body = NoNode;
			for(NodeId Prefix = SignatureExtractor::LeadOf(Syntax, Node); Prefix != Node; Prefix = Syntax.NextNamedSibling(Prefix)) {
				HeaderParts.push_back(Prefix);
			}
			for(const NodeId Child : Syntax.ChildrenOf(Node)) {
				if(EndsHeader(Syntax, Node, Child)) {
					if(Syntax.Is(Child, Role::DefinitionBody)) Body = Child;
					break;
				}
				HeaderParts.push_back(Child);
			}
			// 節点の先行の記録の内，見出から文書の側へ移す物の写し
			const auto TakeHeader = [&](const NodeId Sub) -> void {
				LeadingRecords &Leading = LeadingAt(Sub);
				const size_t Taken = OldLeading.size();
				std::copy_if(Leading.Records.begin(), Leading.Records.end(), std::back_inserter(OldLeading), IsHeaderTaken);
				if(OldLeading.size() > Taken) HeaderAnchors.push_back(Sub);
				Leading.Added = ++AddCount;
			};
			for(const NodeId Part : HeaderParts) {
				WalkNodes(
					Syntax,
					Part,
					[&TakeHeader](const NodeId Sub) -> WalkStep {
						TakeHeader(Sub);
						// 子孫の走査を続ける事の返戻
						return WalkStep::Descend;
					}
				);
			}
			if(Body != NoNode && Syntax.IsBracedBody(Body)) TakeHeader(Body);
			// 独立コメントの原文の最後の通し番号（無ければ最後に並べる）
			size_t LastOrder = OldLeading.empty() ? std::numeric_limits<size_t>::max() : 0;
			for(const CommentRecord &Comment : OldLeading) LastOrder = std::max(LastOrder, Comment.OriginalOrder);
			// 記号と本文を合成した出力の形（文書の記号・記法を規約の文書形式の字面で判定する，整形仕様 7.6）
			std::vector<std::string> Rendered;
			Rendered.reserve(OldLeading.size());
			for(const CommentRecord &Comment : OldLeading) {
				Rendered.push_back(CommentRepository::Compose(CommentRepository::OutputFormOf(Comment)));
			}
			// 記録の文書の位置（定義の前の文書の記法の説明のコメント）に従う群の所属と本文構造（正規化と同じ判定でコード例を読み，補う項目の行で既存の行の読みを変えない，整形仕様 3.3, 7.3）
			std::vector<uint8_t> IsDocuments(OldLeading.size());
			for(size_t Idx = 0; Idx < OldLeading.size(); ++Idx) {
				IsDocuments[Idx] = OldLeading[Idx].Role == CommentRole::Presentation && IsDocComment(Rules, Rendered[Idx]);
			}
			const std::vector<CommentGroupId> Membership = CommentRepository::MembershipOf(OldLeading, IsDocuments);
			const CommentStructure Structure = CommentNormalizer::Inspect(ReadOnlyCommentView(OldLeading, Membership));
			// 行形式の文書の行か（処理系が読む指示コメントは文書へ取り込まない）
			const auto IsDocLineAt = [&OldLeading, &Rendered, &Rules](const size_t Idx) -> bool {
				// 文書の記号を持つ説明のコメントかの返戻
				return OldLeading[Idx].Role != CommentRole::Directive && !DocLineMarker(Rules, Rendered[Idx]).empty();
			};
			// 新しい文書の位置（対象の定義の直前，但し頭部へ置く物を除く後続の宣言へ作用する名前付の指示コメント（`//go:noinline` 等）の前）
			size_t NewAt = OwnCount;
			while(
				NewAt && OldLeading[NewAt - 1].Role == CommentRole::Directive &&
				CommentRepository::IsNamedDirective(OldLeading[NewAt - 1].Notation) && OldLeading[NewAt - 1].Directive &&
				!CommentRepository::IsFileHeadDirective(OldLeading[NewAt - 1].Directive->Definition)
			) --NewAt;
			// 錨自身の最後の文書化コメントブロック，無ければ見出の行から移した最初の物を囲みの文書とする
			size_t DocIdx = OldLeading.size();
			std::vector<std::string_view> OwnTexts;
			OwnTexts.reserve(OwnCount);
			for(size_t Idx = 0; Idx < OwnCount; ++Idx) OwnTexts.emplace_back(Rendered[Idx]);
			if(const size_t Own = LastDocBlockIndex(Rules, OwnTexts); Own < OwnCount) DocIdx = Own;
			// 囲みへ入れれず行形式の儘の文書（本文が囲みを閉じる記号を含む物）も文書の容れ物とする（整形仕様 7.6, 7.7）
			for(size_t Idx = DocIdx < OwnCount ? DocIdx + 1 : 0; Idx < OwnCount; ++Idx) {
				if(CommentRepository::IsBlockLineDocument(OldLeading[Idx])) DocIdx = Idx;
			}
			for(size_t Idx = OwnCount; DocIdx == OldLeading.size() && Idx < OldLeading.size(); ++Idx) {
				if(IsDocBlock(Rules, Rendered[Idx])) DocIdx = Idx;
			}
			if(DocIdx < OldLeading.size() && OldLeading[DocIdx].Role == CommentRole::Directive) {
				RestoreAsIs();
				// 指示コメントの文書を改変せずに次の定義へ進む
				continue;
			}
			// 行形式の文書の行の添字（錨自身の物，見出の行から移した物の順）
			std::vector<size_t> DocLines;
			for(size_t Idx = 0; Idx < OldLeading.size(); ++Idx) if(IsDocLineAt(Idx)) DocLines.push_back(Idx);
			// 埋込文書の前後の空の行は，囲みの文書の開始記号・終端記号だけの行と同じく文書の論理行に含めない
			// 構造として保つ空の行（段落とコード例を分ける行とコード例の中の行）は除かない
			std::vector<bool> IsEdgeBlank = CommentRepository::EmbeddedEdgesOf(OldLeading);
			for(size_t Idx = 0; Idx < OldLeading.size(); ++Idx) {
				if(!IsEdgeBlank[Idx]) continue;
				if(
					const std::span<const uint8_t> KeptBlanks = Structure.GetKeptBlanks(Idx);
					std::ranges::find(KeptBlanks, 1) != KeptBlanks.end()
				) IsEdgeBlank[Idx] = false;
			}
			std::erase_if(
				DocLines,
				[&IsEdgeBlank](const size_t Idx) -> bool {
					// 埋込文書の前後の空の行かの返戻
					return IsEdgeBlank[Idx];
				}
			);
			// 既存の囲みの文書，既存の行形式の文書，新しい文書の書式の順に容れ物を決める
			const bool IsBlock = DocIdx < OldLeading.size() || DocLines.empty() && NewMarker.empty();
			// 錨自身の文書の最初の記録（無ければ新しい文書の位置）
			const std::vector<size_t>::const_iterator OwnEnd = std::lower_bound(DocLines.begin(), DocLines.end(), OwnCount);
			const size_t DocStart = std::min(IsBlock ? DocIdx : OwnEnd != DocLines.begin() ? DocLines.front() : NewAt, NewAt);
			// 区切・指示コメントを挟まず定義へ続く独立コメントの最初（整形仕様 7.4）
			size_t RunStart = NewAt;
			while(RunStart && IsMergeable(OldLeading[RunStart - 1])) --RunStart;
			// 其の中で文書へ統合する通常コメント（文書の手前に区切等が挟まれば統合しない）と，文書より前と後の物の論理行
			std::vector<bool> IsMerged = IsEdgeBlank;
			std::vector<std::string> Before, After;
			for(size_t Idx = RunStart; DocStart >= RunStart && Idx < NewAt; ++Idx) {
				if(IsDocuments[Idx]) continue;
				IsMerged[Idx] = true;
				const std::vector<std::string> Lines = LogicalLines(OldLeading[Idx]);
				std::vector<std::string> &Target = Idx < DocStart ? Before : After;
				Target.insert(Target.end(), Lines.begin(), Lines.end());
			}
			// 錨自身の文書の行の間に統合しない他のコメントが挟まる文書は，同じ種別の順を保てない為に其の儘戻す
			bool IsInterleaved = false;
			for(size_t Idx = DocStart; !IsBlock && OwnEnd != DocLines.begin() && Idx < *(OwnEnd - 1) && !IsInterleaved; ++Idx) {
				IsInterleaved = !IsDocLineAt(Idx) && !IsMerged[Idx];
			}
			if(IsInterleaved) {
				RestoreAsIs();
				// 既存のコメントを戻して次の定義へ進む
				continue;
			}
			// 同期を確定した見出の記録の元の錨からの除去（写しは文書へ統合するか錨の先行コメントへ移す）
			for(const NodeId Sub : HeaderAnchors) std::erase_if(LeadingAt(Sub).Records, IsHeaderTaken);
			const Signature Sig = SignatureExtractor::Extract(Syntax, Node);
			std::vector<CommentRecord> NewLeading;
			// 文書の記法に応じた新しい先行記録の組立
			if(IsBlock) {
				std::vector<std::string> ExistingLines;
				// 文書の説明を残す為，項目の整合前に論理行へ分解
				if(DocIdx < OldLeading.size()) ExistingLines = LogicalLines(OldLeading[DocIdx]);
				size_t MergeAt = MergeDescription(Rules, Sig, Before, After, ExistingLines);
				// 見出の行から移した他の囲みの文書と通常コメントの原文の順の統合（統合した物は復元しない）
				for(size_t Idx = OwnCount; Idx < OldLeading.size(); ++Idx) {
					if(Idx == DocIdx || !IsDocBlock(Rules, Rendered[Idx]) && IsDocuments[Idx]) continue;
					const std::vector<std::string> Lines = LogicalLines(OldLeading[Idx]);
					ExistingLines.insert(ExistingLines.begin() + static_cast<std::ptrdiff_t>(MergeAt), Lines.begin(), Lines.end());
					MergeAt += Lines.size();
					IsMerged[Idx] = true;
				}
				const std::vector<std::string> NewLines = ReconcileLogicalLines(Rules, ExistingLines, Sig);
				// 論理行の本文と，囲みへ入れれず行形式の儘の文書か
				std::string Body;
				for(size_t Idx = 0; Idx < NewLines.size(); ++Idx) Body.append(Idx ? "\n" : "").append(NewLines[Idx]);
				const bool IsLineKept = DocIdx < OldLeading.size() && CommentRepository::IsBlockLineDocument(OldLeading[DocIdx]) &&
				!CommentRepository::CanFrame(Rules, Body);
				// 囲みの文書は開始記号と終端記号を其々独立した行に置く本文（最初と最後の空の論理行）
				std::string Text = IsLineKept ? std::move(Body) : "\n" + Body + (NewLines.empty() ? "" : "\n");
				CommentRecord DocAttach { std::move(Text), { CommentPlacement::Before }, 0 };
				// 既存の文書の入力の記法と正規化の群を保つ（出力記法は整形仕様 7.6 で選ぶ）
				if(DocIdx < OldLeading.size()) {
					DocAttach.Notation = OldLeading[DocIdx].Notation;
					DocAttach.Format.Group = OldLeading[DocIdx].Format.Group;
				} else DocAttach.Notation = NewForm->Notation;
				// 新しい文書の記録は定義の記法の定義を共有する
				CommentRepository::AttachRules(*Site.Rules, DocAttach);
				// 元の文書位置だけを新しい本文へ置換し，文書へ統合した物を除く（既存の文書が無ければ新しい文書の位置）
				const size_t At = DocIdx < OldLeading.size() ? DocIdx : NewAt;
				for(size_t Idx = 0; Idx <= OldLeading.size(); ++Idx) {
					if(Idx == At) NewLeading.push_back(std::move(DocAttach));
					if(Idx < OldLeading.size() && Idx != DocIdx && !IsMerged[Idx]) NewLeading.push_back(std::move(OldLeading[Idx]));
				}
			} else {
				// 文書の記号（既存の文書の最初の行に揃え，無ければ言語の新しい文書の記号）
				const std::string_view Marker = DocLines.empty() ? NewMarker : DocLineMarker(Rules, Rendered[DocLines.front()]);
				// 文書の行の本文と，其の本文毎の元の記録の位置（出現順に取り出す為に逆順に積む）
				std::vector<std::string> SourceTexts;
				SourceTexts.reserve(DocLines.size());
				for(const size_t Idx : DocLines) {
					const std::vector<std::string> Lines = LogicalLines(OldLeading[Idx]);
					std::string &Text = SourceTexts.emplace_back();
					for(size_t Line = 0; Line < Lines.size(); ++Line) Text.append(Line ? "\n" : "").append(Lines[Line]);
				}
				std::unordered_map<std::string_view, std::vector<size_t>> Originals;
				for(size_t Pos = DocLines.size(); Pos--;) Originals[SourceTexts[Pos]].push_back(DocLines[Pos]);
				// 錨自身の文書の論理行へ通常コメントを統合し，見出の行から移した文書の行と通常コメントを原文の順で説明の後へ統合
				std::vector<std::string> ExistingLines(SourceTexts.cbegin(), SourceTexts.cbegin() + (OwnEnd - DocLines.begin()));
				const size_t MergeAt = MergeDescription(Rules, Sig, Before, After, ExistingLines);
				std::vector<std::string> HeaderLines;
				for(size_t Idx = OwnCount; Idx < OldLeading.size(); ++Idx) {
					if(IsEdgeBlank[Idx]) continue;
					const std::vector<std::string> Lines = LogicalLines(OldLeading[Idx]);
					HeaderLines.insert(HeaderLines.end(), Lines.begin(), Lines.end());
					if(!IsDocLineAt(Idx)) IsMerged[Idx] = true;
				}
				ExistingLines.insert(ExistingLines.begin() + static_cast<std::ptrdiff_t>(MergeAt), HeaderLines.begin(), HeaderLines.end());
				const std::vector<std::string> NewLines = ReconcileLogicalLines(Rules, ExistingLines, Sig);
				std::vector<CommentRecord> DocRecords;
				DocRecords.reserve(NewLines.size());
				for(const std::string &Line : NewLines) {
					if(
						const std::unordered_map<std::string_view, std::vector<size_t>>::iterator Iter = Originals.find(Line);
						Iter != Originals.end() && !Iter->second.empty()
					) {
						// 元の記録の本文
						DocRecords.push_back(std::move(OldLeading[Iter->second.back()]));
						DocRecords.back().Text = Line;
						Iter->second.pop_back();
						continue;
					}
					// 既存の論理行は元の記録（記号・続きの行のインデント）の儘，補った論理行と空行だけを文書の記号の記法で書く
					CommentRecord Record { Line, { CommentPlacement::Before }, 0 };
					Record.Notation = CommentRepository::NotationOf(Rules, Marker, false);
					CommentRepository::AttachRules(*Site.Rules, Record);
					DocRecords.push_back(std::move(Record));
				}
				// 既存の行・補った行・統合した行を１つの群とし，正規化・配置が文書全体のコード例を判定する
				for(CommentRecord &Record : DocRecords) Record.Format.Group = NextGroup;
				++NextGroup;
				// 文書の最初の行の位置へ文書を置き（無ければ新しい文書の位置），文書へ統合しない他のコメントは元の順
				for(const size_t Idx : DocLines) IsMerged[Idx] = true;
				const size_t At = DocLines.empty() ? NewAt : DocLines.front();
				for(size_t Idx = 0; Idx <= OldLeading.size(); ++Idx) {
					if(Idx == At) {
						NewLeading.insert(NewLeading.end(), std::make_move_iterator(DocRecords.begin()), std::make_move_iterator(DocRecords.end()));
					}
					if(Idx < OldLeading.size() && !IsMerged[Idx]) NewLeading.push_back(std::move(OldLeading[Idx]));
				}
			}
			// 錨の独立コメントは此の並びの順の儘，同じ行頭へ置く前の錨のコメント（空の本体の末尾の物等）より後へ並べる為，原文の最後の通し番号に揃える
			for(CommentRecord &Comment : NewLeading) Comment.OriginalOrder = LastOrder;
			// 行末・文中のコメントは他の錨のコメントとの原文の順を保つ為，元の通し番号の儘戻す
			NewLeading.insert(NewLeading.end(), std::make_move_iterator(Others.begin()), std::make_move_iterator(Others.end()));
			AddLeading(Anchor, std::move(NewLeading));
		}
		// 触れた錨の先行の記録を最後に戻した順の並びへ移した差替（取り出して戻さなかった記録は除く）
		std::vector<CommentRecord> Kept;
		for(const CommentRecord &Record : Comments.GetRecords()) {
			if(Record.Placement.Side != CommentPlacement::Before || !LeadingOf.contains(Record.Anchor)) Kept.push_back(Record);
		}
		std::vector<LeadingRecords *> Touched;
		Touched.reserve(LeadingOf.size());
		for(auto &[Anchor, Leading] : LeadingOf) Touched.push_back(&Leading);
		std::sort(
			Touched.begin(),
			Touched.end(),
			[](const LeadingRecords *const Lhs, const LeadingRecords *const Rhs) -> bool {
				// 戻した順の比較の返戻
				return Lhs->Added < Rhs->Added;
			}
		);
		for(LeadingRecords *const Leading : Touched) {
			Kept.insert(Kept.end(), std::make_move_iterator(Leading->Records.begin()), std::make_move_iterator(Leading->Records.end()));
		}
		Comments.Update(std::move(Kept));
		// 終了
		return;
	}

	/**
	 * 文書の位置が確定した後の群の所属の確定関数（整形仕様 3.4, 7.6）
	 * 投影が記録した並びを，確定した配置で文書として置く記録に従い，配置の段と同じ定義の群を纏める記法で分ける
	 * 計算量：節点数 V，子の数の和 E，記録の数 C に対し O(V + E + C log C)
	 * @param Code 変更不能なコードの出力
	 * @param Comments 錨の節点毎のコメント
	 */
	void DocCommentSynchronizer::ConfirmGroups(const LaidOutCode &Code, CommentRepository &Comments) {
		const std::span<const CommentRecord> Records = Comments.GetRecords();
		const std::vector<NodeId> Targets = PlacedTargets(Code, Records, DocumentTargets(*Code.GetAnchors().Syntax, Comments));
		std::vector<uint8_t> IsDocuments(Targets.size());
		for(size_t Index = 0; Index < Targets.size(); ++Index) {
			IsDocuments[Index] = Targets[Index] != NoNode && Records[Index].Kind == CommentKind::OwnLine;
		}
		Comments.UpdateGroups(CommentRepository::MembershipOf(Records, IsDocuments));
		// 終了
		return;
	}
} // namespace Core
