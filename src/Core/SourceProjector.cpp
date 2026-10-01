#include "SourceProjector.hpp"
#include "../Profile/GrammarProfile.hpp"
#include "../Util/CharClass.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include "../Util/ParseAdapter.hpp"
#include "../Util/SyntaxParser.hpp"
#include "../Util/TextEdit.hpp"
#include "DocCommentSynchronizer.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace Core {
	/**
	 * 出所の解析器節点の取得関数
	 * 計算量：O(1)
	 * @param Source 投影した節点の出所
	 * @param ValidationNodes 検証資料に含む解析器節点
	 * @param ExtraNodes 検証資料と異なる投影節点
	 * @return 出所の解析器節点（隠れた字句では空）
	 */
	TSNode SourceProjector::SourceNode(
		const Origin &Source,
		const std::span<const TSNode> ValidationNodes,
		const std::span<const TSNode> ExtraNodes
	) {
		// 隠れた字句の空の節点の返戻
		if(Source.NodeIndex == NoNode) return {};
		// 検証資料と異なる投影節点の返戻
		if(Source.NodeIndex & ExtraNodeBit) return ExtraNodes[Source.NodeIndex & ~ExtraNodeBit];
		// 検証資料に含む節点の返戻
		return ValidationNodes[Source.NodeIndex];
	}

	/**
	 * 文書の表示境界の投影関数
	 * 計算量：節点数 V，子の総数 E，読む字句の長さ B に対し O(V + E + B)
	 * @param Src 解析済の文書
	 * @param Sources 節点の原文の出所
	 * @param ValidationNodes 検証資料に含む解析器節点
	 * @param ExtraNodes 検証資料と異なる投影節点
	 * @param Syntax 表示の空白と逐語内容を記録する構文
	 */
	void SourceProjector::RecordMarkupFacts(
		const ::ParsedTree &Src,
		const std::vector<Origin> &Sources,
		const std::span<const TSNode> ValidationNodes,
		const std::span<const TSNode> ExtraNodes,
		SyntaxModel &Syntax
	) {
		// 親から継承する空白保持・外来内容・XML の契約の文脈
		std::vector<uint8_t> Traits(Syntax.NodeCount()), Preserved(Syntax.NodeCount()), Foreign(Syntax.NodeCount());
		std::vector<uint8_t> Xml(Syntax.NodeCount());
		const GrammarProfile &Profile = Src.GetProfile();
		const ReadingRules &Reading = Profile.GetReading();
		const KindDefinition *const OpenTag = Profile.KindWithRole(Role::StartTag);
		const KindDefinition *const CloseToken = Profile.KindWithRole(Role::GreaterThanToken);
		const std::string_view CloseSpelling = Profile.SymbolName(Profile.SymbolWithRole(Role::GreaterThanToken)); // 字句の節点名は其の字面
		// 要素の特性と空白保持の継承
		for(NodeId Id = 0; Id < Syntax.NodeCount(); ++Id) {
			if(const SyntaxNode &Node = Syntax.Nodes[Id]; Node.Parent != NoNode) {
				Preserved[Id] = Preserved[Node.Parent];
				Foreign[Id] = Foreign[Node.Parent];
				Xml[Id] = Xml[Node.Parent];
			}
			if(!Syntax.Is(Id, Role::MarkupContainer)) continue;
			const NodeId Tag = Syntax.Child(Id, 0), Name = Syntax.NamedChild(Tag, 0);
			std::string Lower = Name == NoNode ? std::string() : Syntax.Text(Name);
			ToLowerAscii(Lower);
			Traits[Id] = GrammarProfile::MarkupTraits(Lower);
			Foreign[Id] = Foreign[Id] || Traits[Id] & GrammarProfile::Foreign;
			for(const NodeId Attribute : Syntax.ChildrenOf(Tag)) {
				const NodeId AttributeName = Syntax.NamedChild(Attribute, 0);
				std::string Value = Syntax.Text(Syntax.NamedChild(Attribute, 1));
				if(Value.size() > 1 && (Value.front() == '\'' || Value.front() == '"')) Value = Value.substr(1, Value.size() - 2);
				if(Profile.PreservesMarkupWhitespace(Syntax.Text(AttributeName), Value)) Traits[Id] |= GrammarProfile::Preserve;
				if(Syntax.IsWord(AttributeName, Word::NamespaceAttribute) && Value == Reading.NativeNamespaceValue) Xml[Id] = true; // 表示の規則は HTML の儘とする XML の契約
			}
			Preserved[Id] = Preserved[Id] || Traits[Id] & GrammarProfile::Preserve;
			// 空要素のタグの正規化
			if(!Foreign[Id] && !Xml[Id] && Traits[Id] & GrammarProfile::Void && Syntax.Is(Tag, Role::SelfClosingTag)) {
				Syntax.Nodes[Tag].Kind = { OpenTag };
				SyntaxNode &Close = Syntax.Nodes[Syntax.Child(Tag, Syntax.ChildCount(Tag) - 1)];
				Close.Kind = { CloseToken };
				Close.Lexeme = CloseSpelling;
			}
		}
		// 暗黙に閉じる要素が末尾に取り込んだ空白を親の表示境界へ帰属
		std::vector<uint32_t> Ends(Syntax.NodeCount());
		for(NodeId Id = Syntax.NodeCount(); Id-- > 0;) {
			const SyntaxNode &Node = Syntax.GetNode(Id);
			Ends[Id] = Sources[Id].End;
			if(!Preserved[Id] && Syntax.Is(Id, Role::MarkupContainer) && !Node.Children.empty()) Ends[Id] = Ends[Node.Children.back()];
		}
		// 子の境界を表示上の空白と構造上の区切りへ変換
		for(NodeId Id = 0; Id < Syntax.NodeCount(); ++Id) {
			SyntaxNode &Node = Syntax.Nodes[Id];
			if(!Syntax.Is(Id, Role::MarkupContainer)) continue;
			Node.Facts.Boundaries.Edit().clear();
			const bool IsRoot = Node.Parent == NoNode;
			for(uint32_t Index = 1; Index <= Node.Children.size(); ++Index) {
				const NodeId Left = Node.Children[Index - 1], Right = Index < Node.Children.size() ? Node.Children[Index] : NoNode;
				const uint32_t From = Preserved[Id] ? Sources[Left].End : Ends[Left];
				const uint32_t To = Right == NoNode ? Sources[Id].End : Sources[Right].Start;
				const std::string_view Gap = From < To ? std::string_view(Src).substr(From, To - From) : std::string_view();
				if(Right == NoNode && (!Preserved[Id] || IsRoot || Syntax.Is(Left, Role::MarkupClose))) continue;
				if(Preserved[Id] || Gap.find_first_of("\v\f") != std::string_view::npos) {
					Node.Facts.Boundaries.Edit().push_back({ Index, BoundaryKind::Preserved, std::string(Gap), PreservedSource::LiteralContent });
					continue;
				}
				const bool IsEdge = Syntax.Is(Left, Role::MarkupOpen) || Syntax.Is(Right, Role::MarkupClose);
				const bool HasText = Syntax.Is(Left, Role::MarkupText) || Syntax.Is(Right, Role::MarkupText);
				const bool IsBlock = Traits[Left] & GrammarProfile::Block || Right != NoNode && Traits[Right] & GrammarProfile::Block;
				const bool IsHidden = Syntax.Is(Left, Role::CommentNode) || Syntax.Is(Right, Role::CommentNode);
				const bool IsGraphic = Foreign[Id] && !(Traits[Id] & GrammarProfile::Text);
				const bool IsHead = Syntax.IsWord(Syntax.NamedChild(Syntax.Child(Id, 0), 0), Word::MetadataElement);
				BoundaryKind Boundary = BoundaryKind::NoBreak;
				if(
					IsRoot && (IsBlock || IsHidden || Syntax.Is(Left, Role::DocumentTypeDeclaration)) ||
					!HasText && (IsBlock || IsGraphic || IsHead) && !IsEdge
				) Boundary = BoundaryKind::Separated;
				else if(IsEdge && !(Traits[Id] & GrammarProfile::Void) && Traits[Id] & GrammarProfile::Block) {
					Boundary = BoundaryKind::Separated;
				} else if(IsEdge && !HasText && !Gap.empty() && Syntax.Is(Left, Role::MarkupOpen) && Syntax.Is(Right, Role::MarkupClose)) {
					Boundary = BoundaryKind::RequiredSpace;
				} else if(HasText || IsEdge && Traits[Id] & GrammarProfile::Void || Right == NoNode) {
					Boundary = Gap.empty() ? BoundaryKind::NoBreak : BoundaryKind::RequiredSpace;
				} else if(IsEdge) Boundary = BoundaryKind::Separated;
				else if(!IsHidden && !Gap.empty()) Boundary = BoundaryKind::Spaced;
				else if(IsHidden) {
					// コメントを越えた最寄の子（コメントだけが続く端は容器の端）
					uint32_t Previous = Index - 1, Next = Index;
					while(Previous && Syntax.Is(Node.Children[Previous], Role::CommentNode)) --Previous;
					while(Next + 1 < Node.Children.size() && Syntax.Is(Node.Children[Next], Role::CommentNode)) ++Next;
					// 行内の内容に接するか（ブロックでない容器の端は外の行内の内容へ続き得る）
					const auto IsInlineSide = [&](const NodeId Side) -> bool {
						// 容器の端は容器がブロックでない事，其の他は行を区切る要素とコメントでない事の返戻
						return Syntax.Is(Side, Role::CommentNode) || Syntax.Is(Side, Role::MarkupOpen) || Syntax.Is(Side, Role::MarkupClose) ?
						!(Traits[Id] & GrammarProfile::Block) :
						!(Traits[Side] & GrammarProfile::Block);
					};
					// 両側が行内の内容なら表示される空白の有無を保ち，ブロックの端・行を区切る要素に接する境界は空白に依らず表示されない区切とする（整形仕様 6.6）
					if(!IsInlineSide(Node.Children[Previous]) || !IsInlineSide(Node.Children[Next])) Boundary = BoundaryKind::Separated;
					else if(!Gap.empty()) Boundary = BoundaryKind::Spaced;
				}
				Node.Facts.Boundaries.Edit().push_back({ Index, Boundary });
			}
		}
		// 独立したコードとして整形した埋込の出力を所有区画とし，親の字句にしない（整形仕様 3.2, 4.4）
		for(NodeId Id = 0, Count = static_cast<NodeId>(Sources.size()); Id < Count; ++Id) {
			std::shared_ptr<const OwnedRegion> Region =
			Syntax.Is(Id, Role::RawTextNode) ? Src.OwnedRegionOf(SourceNode(Sources[Id], ValidationNodes, ExtraNodes)) : nullptr;
			if(!Region) continue;
			// 字句を持たない区画もコメントの配置計画を出力の組立へ渡す
			Syntax.Nodes[Id].Lexeme.clear();
			Syntax.Regions.push_back(std::move(Region));
			Syntax.Nodes[Id].Facts.RegionIndex = static_cast<uint32_t>(Syntax.Regions.size());
		}
		// 終了
		return;
	}

	/**
	 * 識別子の書式文字の判定関数
	 * @param Point 符号点
	 * @return Unicode 17.0.0 の一般カテゴリ `Cf` なら true
	 */
	bool SourceProjector::IsFormatCharacter(const uint32_t Point) {
		// 一般カテゴリ `Cf` の範囲
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
		// 範囲に入る事の返戻
		for(const uint32_t (&Range)[2] : Ranges) if(Point >= Range[0] && Point <= Range[1]) return true;
		// `Cf` でない事の返戻
		return false;
	}

	/**
	 * 識別子の中の Unicode の逃避の復号関数
	 * `\u` と４桁（`u` の繰返を含む），`\u{…}`，`\U` と８桁を読む
	 * @param Reading 入力の読取規則
	 * @param Name 名前の字面
	 * @param Pos 逃避の始まり（成功時は逃避の後へ進む）
	 * @param Point 復号した符号点の格納先
	 * @return 識別子の字として解釈出来る逃避なら true
	 */
	bool SourceProjector::DecodeIdentifierEscape(
		const ReadingRules &Reading,
		const std::string_view Name,
		size_t &Pos,
		uint32_t &Point
	) {
		// 逃避の記法と桁数の判定
		size_t At = Pos + 1;
		const bool IsShort = At < Name.size() && Name[At] == 'u';
		// 逃避の印でない事の返戻
		if(!IsShort && (At >= Name.size() || Name[At] != 'U')) return false;
		while(At < Name.size() && Name[At] == Name[Pos + 1]) ++At;
		const bool IsBraced = IsShort && At < Name.size() && Name[At] == '{';
		const size_t First = At + IsBraced, Last = IsBraced ? Name.find('}', First) : First + (IsShort ? 4 : 8);
		// 桁の無い逃避と足りない逃避の返戻
		if(Last == std::string_view::npos || Last > Name.size() || Last == First) return false;
		if(
			const std::from_chars_result Parsed = std::from_chars(Name.data() + First, Name.data() + Last, Point, 16);
			Parsed.ec != std::errc{} || Parsed.ptr != Name.data() + Last || Point > 0X10FFFF || Point > 0XD7FF && Point < 0XE000
		// Unicode のスカラー値でない逃避の返戻
		) return false;
		// 基本文字（`$` `@` `` ` `` を除く `U+00A0` 未満）を指せない文法の返戻
		if(Reading.RestrictsBasicIdentifierEscapes && Point < 0XA0 && Point != '$' && Point != '@' && Point != '`') return false;
		Pos = Last + IsBraced;
		// 復号出来た事の返戻
		return true;
	}

	/**
	 * 識別子の比較用の名前の取得関数
	 * 生の接頭辞・引用記号・ラベルの定義の記号を除き，逃避を復号し，比較で無視する字を除く（解釈出来ない名前は字面の儘）
	 * 計算量：名前の長さ L に対し O(L)
	 * @param Reading 入力の読取規則
	 * @param Name 名前の字面
	 * @param IsLabel ラベルの字句か
	 * @return 比較用の名前
	 */
	std::string SourceProjector::ComparisonNameOf(const ReadingRules &Reading, std::string_view Name, const bool IsLabel) {
		// 比較用の名前から外形の記号を除去
		const std::string_view Raw = Name;
		if(IsLabel && !Reading.LabelDefinitionSuffix.empty() && Name.ends_with(Reading.LabelDefinitionSuffix)) {
			Name.remove_suffix(Reading.LabelDefinitionSuffix.size());
		}
		if(!Reading.IdentifierRawPrefix.empty() && Name.starts_with(Reading.IdentifierRawPrefix)) {
			Name.remove_prefix(Reading.IdentifierRawPrefix.size());
		}
		if(
			const std::string_view Quote = Reading.IdentifierQuote;
			!Quote.empty() && Name.size() > Quote.size() && Name.starts_with(Quote) && Name.ends_with(Quote)
		) Name = Name.substr(Quote.size(), Name.size() - (Quote.size() << 1));
		// 逃避の復号と書式文字の除去
		std::string Result;
		Result.reserve(Name.size());
		for(size_t Pos = 0; Pos < Name.size();) {
			const size_t Start = Pos;
			uint32_t Point = 0;
			const bool IsEscape = Reading.DecodesIdentifierEscapes && Name[Pos] == '\\';
			if(IsEscape ? !DecodeIdentifierEscape(Reading, Name, Pos, Point) : !TextEdit::DecodeUtf8(Name, Pos, Point)) {
				// 解釈出来ない逃避と不正な UTF-8 を持つ名前の字面の返戻
				return std::string(Raw);
			}
			if(Reading.IgnoresIdentifierFormatCharacters && IsFormatCharacter(Point)) {
				// 書式文字で始まる名前（識別子に為れない）の字面の返戻
				if(!Start) return std::string(Raw);
				continue;
			}
			if(IsEscape) TextEdit::EncodeUtf8(Point, Result);
			else Result.append(Name.substr(Start, Pos - Start));
		}
		// 比較用の名前の返戻
		return Result;
	}

	/**
	 * 閉じ括弧の内側へ読まれた逐語本文の結付の復元関数
	 * 本文から最初の閉じ字句迄に別の字句が無ければ，閉じの後に字句が続いても開始を含む文（語で閉じる塊の見出の中の物は其の見出の部品）の後へ移す
	 * 同じ文の中に移せない本文が在れば，其の本文が括弧の中を複数行にする為に，其の文の本文は移さない
	 * 計算量：節点数 V，本文数 B，調べる部分木と経路の大きさの和 A，移動先の子の最大数 C に対し O(V + B log B + A + BC)
	 * @param Sources 節点の原文の出所
	 * @param Syntax 投影した構文
	 */
	void SourceProjector::RebindDeferredLiteralBodies(const std::vector<Origin> &Sources, SyntaxModel &Syntax) {
		// 括弧の中の本文と，其の開始を含む文と文の並び，文の後へ移せるか
		struct Candidate {
			NodeId Body;
			NodeId Statement;
			NodeId Sequence;
			bool IsMovable;
		};
		// 逐語本文の原文順の収集
		std::vector<NodeId> Bodies;
		for(NodeId Id = 0; Id < Syntax.NodeCount(); ++Id) if(Syntax.Is(Id, Role::DeferredLiteralBody)) Bodies.push_back(Id);
		std::ranges::sort(
			Bodies,
			[&Sources](const NodeId Left, const NodeId Right) -> bool {
				// 本文の原文上の順の返戻
				return Sources[Left].Start < Sources[Right].Start;
			}
		);
		// 本文毎の移動可否と移動先の判定
		std::vector<Candidate> Candidates;
		std::unordered_set<NodeId> Blocked;
		for(const NodeId Body : Bodies) {
			const NodeId Brackets = Syntax.ParentOf(Body);
			if(
				Brackets == NoNode || Syntax.Is(Brackets, Role::StatementSequence) || Syntax.Is(Brackets, Role::StatementHost) || !(
					Syntax.HasTokenRoleChild(Brackets, Role::OpeningParenToken) || Syntax.HasTokenRoleChild(Brackets, Role::OpeningBracketToken) ||
					Syntax.HasTokenRoleChild(Brackets, Role::OpeningBraceToken)
				)
			) continue;
			bool HasStart = false;
			std::vector<NodeId> Pending { Brackets };
			while(!Pending.empty()) {
				const NodeId Part = Pending.back();
				Pending.pop_back();
				if(Syntax.Is(Part, Role::DeferredLiteralStart) && Sources[Part].Start < Sources[Body].Start) HasStart = true;
				for(const NodeId Child : Syntax.ChildrenOf(Part)) Pending.push_back(Child);
			}
			NodeId Within = Body, Statement = NoNode, Sequence = NoNode;
			bool HasClearCloser = HasStart, HasCloser = false;
			for(NodeId Parent = Brackets; Parent != NoNode; Parent = Syntax.ParentOf(Parent)) {
				// 文の並びの中の文の後，又は語で閉じる塊の見出の部品の後（閉じ括弧を同じ行に書いた時に解析器が置く所）
				if(
					Syntax.Is(Parent, Role::StatementSequence) || Syntax.Is(Parent, Role::StatementHost) || Syntax.Is(Parent, Role::KeywordBlock)
				) {
					Statement = Within;
					Sequence = Parent;
					break;
				}
				const std::span<const NodeId> Children = Syntax.ChildrenOf(Parent);
				// 最初の閉じ迄に別の字句が無いかを確かめ，閉じより後の式の字句は許す
				for(uint32_t Index = Syntax.GetNode(Within).Index + 1; HasClearCloser && !HasCloser && Index < Children.size(); ++Index) {
					const NodeId After = Children[Index];
					const bool IsCloser =
					!Syntax.IsNamed(After) && Syntax.IsTokenRole(After, Role::ClosingBracketLead) && Syntax.IsTokenRole(After, Role::CloserToken);
					HasClearCloser = Syntax.Is(After, Role::DeferredLiteralBody) || IsCloser;
					HasCloser = HasCloser || IsCloser;
				}
				Within = Parent;
			}
			if(Sequence == NoNode) continue;
			Candidates.push_back({ Body, Statement, Sequence, HasClearCloser && HasCloser });
			if(!Candidates.back().IsMovable) Blocked.insert(Statement);
		}
		// 移動可能な本文の結付変更
		for(const auto &[Body, Statement, Sequence, IsMovable] : Candidates) {
			if(!IsMovable || Blocked.contains(Statement)) continue;
			uint32_t Index = Syntax.GetNode(Statement).Index + 1;
			while(Index < Syntax.ChildCount(Sequence) && Syntax.Is(Syntax.Child(Sequence, Index), Role::DeferredLiteralBody)) ++Index;
			RewriteStep Move { RewriteKind::Move, Body };
			Move.Destination = Sequence;
			Move.Index = Index;
			Move.DestinationFields = 0;
			RewritePlan Plan;
			Plan.Steps.push_back(std::move(Move));
			Syntax.Apply(Plan);
		}
		// 終了
		return;
	}

	/**
	 * 閉じタグと枝の所属に関する解析器の補修関数
	 * 閉じタグが終える空の本体を明示し，後続の地の文と文を外へ戻す
	 * 開いた条件分岐に続く枝は最も近い未完の分岐へ結ぶ
	 * @param Reading 入力の読取規則
	 * @param Syntax 投影した構文
	 */
	void SourceProjector::RebindEmbeddedBranches(const ReadingRules &Reading, SyntaxModel &Syntax) {
		// 閉じタグで終わる本体の明示
		const uint64_t BodyField = uint64_t{ 1 } << static_cast<uint8_t>(ChildRole::Body);
		for(NodeId Id = 0; Id < Syntax.NodeCount(); ++Id) {
			if(!Syntax.Is(Id, Role::BodyOwner) && !Syntax.Is(Id, Role::If) && !Syntax.Is(Id, Role::ElseIfClause)) continue;
			if(Syntax.HasTokenRoleChild(Id, Role::ColonToken)) continue;
			const NodeId Body = Syntax.FieldChild(Id, ChildRole::Body);
			NodeId Boundary = NoNode;
			for(const NodeId Child : Syntax.ChildrenOf(Id)) {
				if(Child == Body) break;
				if(Syntax.Is(Child, Role::CodeBoundary)) {
					Boundary = Child;
					break;
				}
			}
			if(Boundary == NoNode || Body == NoNode) continue;
			NodeId Owner = Id;
			while(Syntax.Is(Owner, Role::ElseClause) || Syntax.Is(Owner, Role::ElseIfClause) || Syntax.HasField(Owner, ChildRole::Body)) {
				Owner = Syntax.ParentOf(Owner);
			}
			const NodeId Parent = Syntax.ParentOf(Owner);
			if(Parent == NoNode) continue;
			// 本体の移動と空の文の挿入
			RewritePlan Plan;
			for(const NodeId Child : { Boundary, Body }) {
				RewriteStep Move { RewriteKind::Move, Child };
				Move.Destination = Parent;
				Move.Index = Syntax.GetNode(Owner).Index + (Child == Boundary ? 1 : 2);
				Move.DestinationFields = 0;
				Plan.Steps.push_back(std::move(Move));
			}
			RewriteStep Empty { RewriteKind::Insert, Id };
			Empty.Destination = Id;
			Empty.Index = Syntax.GetNode(Boundary).Index;
			Empty.Inserted = {
				{},
				true,
				{ { std::string(Syntax.Rules().Spelling(Word::StatementEnd)) } },
				BodyField,
				NoNode,
				{},
				Reading.EmptyStatementDefinition
			};
			Plan.Steps.push_back(std::move(Empty));
			Syntax.Apply(Plan);
		}
		// 最も内側の開いた枝（明示した塊を境界とし，最後の枝の本体の連鎖を内へ辿る，整形仕様 9.2）
		const auto FindOpen = [&Syntax](NodeId Id) -> NodeId {
			NodeId Open = NoNode;
			while(Syntax.Is(Id, Role::If) || Syntax.Is(Id, Role::ElseIfClause)) {
				NodeId LastBranch = NoNode;
				for(const NodeId Child : Syntax.ChildrenOf(Id)) {
					if(Syntax.Is(Child, Role::ElseClause) || Syntax.Is(Child, Role::ElseIfClause)) LastBranch = Child;
				}
				// 最後の else を持たない分岐
				if(!Syntax.Is(LastBranch, Role::ElseClause)) Open = Id;
				Id = Syntax.FieldChild(LastBranch == NoNode ? Id : LastBranch, ChildRole::Body);
			}
			// 最も内側の開いた分岐の返戻
			return Open;
		};
		// 後続枝の未完の分岐への結付
		for(bool Changed = true; Changed;) {
			Changed = false;
			for(NodeId Id = 0; Id < Syntax.NodeCount(); ++Id) {
				if(!Syntax.Is(Id, Role::If)) continue;
				std::vector<NodeId> Branches;
				for(const NodeId Child : Syntax.ChildrenOf(Id)) {
					if(Syntax.Is(Child, Role::ElseClause) || Syntax.Is(Child, Role::ElseIfClause)) Branches.push_back(Child);
				}
				for(const NodeId Branch : Branches) {
					const NodeId Target = FindOpen(Syntax.FieldChild(Id, ChildRole::Body));
					if(Target == NoNode) break;
					RewriteStep Move { RewriteKind::Move, Branch };
					Move.Destination = Target;
					Move.Index = Syntax.ChildCount(Target);
					RewritePlan Plan;
					Plan.Steps.push_back(std::move(Move));
					Syntax.Apply(Plan);
					Changed = true;
				}
			}
		}
		// 終了
		return;
	}

	/**
	 * 片側だけ空白の有る Swift の利用者定義演算子を持つかの判定関数
	 * Swift は演算子の前後の空白の有無で前置・後置・中置を見分け（`a^^` の後に改行が続けば後置），両側に空白を置くと中置に変わる
	 * 解析器は宣言を知らず中置の式と読む為（`a^^↵print(b)` を `a ^^ print(b)`），片側だけ空白の有る利用者定義演算子の式は原文の儘写す
	 * @param Src ソースコード
	 * @param Node 判定対象のノード
	 * @return 片側だけ空白の有る利用者定義演算子を直接の子に持つ Swift の中置の式なら true
	 */
	bool SourceProjector::HoldsLopsidedOperator(const ::ParsedTree &Src, const TSNode Node) {
		// Swift の中置の式でない場合の返戻
		if(!Src.Is(Node, Role::SpacingSensitiveInfix)) return false;
		// 利用者定義演算子の前後の空白の有無が揃わないかの返戻
		return HasChildOf(
			Node,
			[&Src](const TSNode Child) -> bool {
				// 演算子でない子の返戻
				if(!Src.Is(Child, Role::UserDefinedOperator)) return false;
				// 対象直前の状態列
				const TSNode Before = ts_node_prev_sibling(Child), After = ts_node_next_sibling(Child);
				// 片側の兄弟を欠く演算子は隣接を比べれない事の返戻
				if(ts_node_is_null(Before) || ts_node_is_null(After)) return false;
				// 左右の隣接（空白を挟まずに続くか）
				const bool IsLeftAttached = Src.End(Before) == Src.Start(Child), IsRightAttached = Src.End(Child) == Src.Start(After);
				// 片側だけ空白の有る演算子かの返戻
				return IsLeftAttached != IsRightAttached;
			}
		);
	}

	/**
	 * 錨へ紐付けたコメントを除いた子の数の取得関数（整形仕様 7.1）
	 * 分離したコメントは原文の木の子に残る為，除いた跡へ字句の融合を防ぐ CSS の区切記法を置く物だけを子に数える
	 * @param Src 解析結果
	 * @param Separated 投影の間だけ持つコメントの分離の結果
	 * @param Node 解析器の節点
	 * @param IsNamedOnly 名前付の子だけを数えるか
	 * @param First 最初に数えた子の格納先（不要なら nullptr）
	 * @return 数えた子の数
	 */
	uint32_t SourceProjector::CountCodeChildren(
		const ::ParsedTree &Src,
		const CommentSeparation &Separated,
		const TSNode Node,
		const bool IsNamedOnly,
		TSNode *const First
	) {
		// 分離したコメントを除く子の計数
		uint32_t Count = 0;
		ForEachChild(
			Node,
			[&](const TSNode Child) -> void {
				// 名前無の子と，跡へ区切記法を置かない分離したコメントを読飛ばす場合の終了
				if(
					IsNamedOnly && !ts_node_is_named(Child) || Separated.IsSeparatedComment(Src, Child) && !Separated.IsNotationSeparated(Child)
				) return;
				if(!Count && First) *First = Child;
				++Count;
			}
		);
		// 数えた子の数の返戻
		return Count;
	}

	/**
	 * 子へ降りず字面を保つ節点かの判定関数（整形仕様 4.4, 4.6）
	 * 値の内容・読めなかった区画・字面が値に為るマクロの実引数・処理系で読みの分かれる本文等は，字句へ分けずに一つの葉として保つ
	 * @param Src コメントを分離した解析結果
	 * @param Separated 投影の間だけ持つコメントの分離の結果
	 * @param Node 解析器の節点（子を持つ物）
	 * @return 子へ降りない節点なら true
	 */
	bool SourceProjector::IsVerbatimNode(const ::ParsedTree &Src, const CommentSeparation &Separated, const TSNode Node) {
		if(Src.Is(Node, Role::TypeAliasStatement)) if(
			const TSNode Left = Src.FieldChild(Node, ChildRole::Left);
			Src.Is(ts_node_named_child(Left, 0), Role::Identifier) || Src.Is(ts_node_named_child(Left, 0), Role::AppliedGenericType)
		// 型の別名の部品へ降りる事の返戻
		) return false;
		// 補間内の引用符と表示対象の字面を処理系の版を問わず読める形で保つ事の返戻
		if(Src.Is(Node, Role::VerbatimQuotedString)) return true;
		// 子を持つ無名の節点（別名で包んだ字句）は字句として保つ事の返戻
		if(!ts_node_is_named(Node)) return true;
		const RoleSet &Roles = Src.GetProfile().KindOf(ts_node_symbol(Node)).Roles;
		// 節点の役割の有無
		const auto Has = [&Roles](const Role Wanted) -> bool {
			// 役割の有無の返戻
			return Roles.test(static_cast<size_t>(Wanted));
		};
		// 文書の要素は本文とタグの境界を個別に投影する事の返戻
		if(Has(Role::MarkupContainer)) return false;
		// Rust の定義見出を通常の構文へ投影し，字句列の内部は原文の間隙を保つ（整形仕様 4.3, 4.4）
		const bool IsMacroBody = Src.Is(Node, Role::MacroRuleDefinition) || Has(Role::PreservedTokenSequence);
		// マクロの構文を子へ投影する事の返戻
		if(IsMacroBody) return false;
		// 埋込式と埋込式を持つ文字列は，葉の集合に在るが埋込式を正規化する為に降りる（整形仕様 4.4）
		const bool IsInterpolated =
		Has(Role::StringInterpolation) || (Has(Role::SpaceSkip) || Has(Role::SourceLeaf) || Has(Role::VerbatimSubtree)) && HasChildOf(
			Node,
			[&Src](const TSNode Child) -> bool {
				// 埋込式の子かの返戻
				return Src.Is(Child, Role::StringInterpolation);
			}
		);
		// 誤り区画は子の字句と其の間隙を保つ境界として投影する為に降りる（整形仕様 4.6）
		// 字面を写す種別の返戻
		if(
			Has(Role::SourceCopiedNode) && !Has(Role::Error) || Src.Is(Node, Role::DocumentTypeDeclaration) || Has(Role::ImportLeaf) ||
			Src.Is(Node, Role::ElementText) ||
			Has(Role::SpaceSkip) && !Has(Role::EqualsTight) && !Has(Role::EqualsSpaced) && !Has(Role::NormColon) && !IsInterpolated ||
			Has(Role::SourceLeaf) && (Src.GetReading().HasOuterText || !Src.Is(Node, Role::QualifiedNameNode)) &&
			!Has(Role::AngleBracketList) && !IsInterpolated
		) return true;
		// 前処理指令は指令の語・名前・仮引数・本体の字句を構文として投影し，解析の為に印へ置き換えた連結指定は一つの葉として復号する事の返戻（整形仕様 4.2，字句間空白仕様 5.4）
		if(Has(Role::Preprocessor)) return ParseAdapter::IsLinkageGuard(Src, Node);
		// 整形出来ない構文と字面が値に為る区画の返戻
		if(
			Has(Role::VerbatimSubtree) && !Src.Is(Node, Role::TemplateInstantiationType) && !IsInterpolated ||
			Src.GetReading().HasPreprocessorMacros && Src.StringizesArguments(Node) || HoldsLopsidedOperator(Src, Node) ||
			Src.GetRules().IsWhitespaceSemantic && Src.IsCssUrlArguments(Node)
		) return true;
		TSNode Only {};
		const uint32_t ChildCount = CountCodeChildren(Src, Separated, Node, false, &Only);
		// 空白が意味を持つ文法の単位付の数値で，子が複数の物の返戻
		if(Src.GetRules().IsWhitespaceSemantic && ChildCount > 1 && Has(Role::DimensionNumber)) return true;
		// 子の外に空白以外の字面を持つ唯一の子（解析器が字句を節点へ出さなかった形）の返戻（根は子の並びを組む為に除く）
		if(ChildCount == 1 && !IsTreeRoot(Node)) {
			const std::string Before = Separated.SeparatedText(Src, ts_node_start_byte(Node), ts_node_start_byte(Only));
			const std::string After = Separated.SeparatedText(Src, ts_node_end_byte(Only), ts_node_end_byte(Node));
			// 空白・文を終える隠れた区切記号（文境界で字句にしない）・行継続の記号以外の字面を持つか
			const auto HasToken = [&Src](std::string_view Text) -> bool {
				const std::string_view Terminator = Src.GetRules().Spelling(Word::StatementEnd);
				const std::string_view Continuation = Src.GetRules().Spelling(Word::LineContinuation);
				while(!Text.empty()) {
					if(std::string_view(" \t\r\n").find(Text.front()) != std::string_view::npos) Text.remove_prefix(1);
					else if(!Terminator.empty() && Text.starts_with(Terminator)) Text.remove_prefix(Terminator.size());
					else if(!Continuation.empty() && Text.starts_with(Continuation)) Text.remove_prefix(Continuation.size());
					// 字句の字面を持つ事の返戻
					else return true;
				}
				// 字句の字面を持たない事の返戻
				return false;
			};
			// 子の外に文境界以外の字句を持つ唯一の子かの返戻
			return !Has(Role::MarkupElementNode) && (HasToken(Before) || HasToken(After));
		}
		// 子へ降りる節点の返戻
		return false;
	}

	/**
	 * プリプロセッサ指令判定関数
	 * @param Raw 判定対象テキスト
	 * @param Pos 判定開始位置
	 * @return `#` の直後（空白を挟んでも良い）に指令名が有れば true
	 */
	bool SourceProjector::IsPreprocDirectiveAt(const std::string_view Raw, const size_t Pos) {
		// `#` で始まらない事の返戻
		if(Pos >= Raw.size() || Raw[Pos] != '#') return false;
		// 対象範囲の開始位置
		size_t Start = Pos + 1;
		while(Start < Raw.size() && (Raw[Start] == ' ' || Raw[Start] == '\t')) ++Start;
		// 指令名は処理系の拡張（`#import` / `#include_next` / `#ident` 等）も有る為，名前の種類を限らない事の返戻
		return Start < Raw.size() && std::isalpha(static_cast<unsigned char>(Raw[Start]));
	}

	/**
	 * CSS の `url(...)` の引数の並びの字面の取得関数
	 * CSS の URL は丸括弧の内側だけ詰め，参照先の字面を保つ
	 * @param Arguments 復号した引数の並びの字面
	 * @return 丸括弧の内側の両端の余白を詰めた字面
	 */
	std::string SourceProjector::CssUrlArgumentsText(const std::string_view Arguments) {
		// 参照先を変えずに詰める丸括弧内の範囲
		std::string_view Text = Arguments;
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
	 * 字面を保つ節点の字面の取得関数（整形仕様 4.4）
	 * 解析の為に印へ置き換えた連結指定は其の字句だけ，CSS の `url()` は引数の参照先の字面を保つ
	 * @param Src コメントを分離した解析結果
	 * @param Separated 投影の間だけ持つコメントの分離の結果
	 * @param Node 解析器の節点
	 * @return 葉の字面
	 */
	std::string SourceProjector::VerbatimText(const ::ParsedTree &Src, const CommentSeparation &Separated, const TSNode Node) {
		if(ParseAdapter::IsLinkageGuard(Src, Node)) {
			// 解析の為に印へ置き換えた連結指定の字句の返戻（指令を終える改行を含めない）
			return Separated.DecodedText(Src, ts_node_start_byte(Node), Src.End(ts_node_child(Node, ts_node_child_count(Node) - 1)));
		}
		// CSS の `url()` の引数の返戻
		if(Src.GetRules().IsWhitespaceSemantic && ts_node_child_count(Node) && Src.IsCssUrlArguments(Node)) {
			// URL の引数の字面の返戻
			return CssUrlArgumentsText(ParseAdapter::DecodeLexeme(Src, std::string(Src.View(Node)), Src.Start(Node)));
		}
		// 錨へ紐付けたコメントを除き，断片毎に復号した原文の字面の返戻
		return Separated.DecodedText(Src, ts_node_start_byte(Node), ts_node_end_byte(Node));
	}

	/**
	 * コメントを除いた子の取得関数（整形仕様 1.2）
	 * 解析木の子に残るコメントの節点を除き，読違の判定が文中のコメントの有無で変わらない様にする
	 * @param Src 解析結果
	 * @param Node 節点
	 * @return コメントでない子の並び
	 */
	std::vector<TSNode> SourceProjector::CodeChildrenOf(const ::ParsedTree &Src, const TSNode Node) {
		// コメントでない子の収集
		std::vector<TSNode> Children;
		for(uint32_t Index = 0; Index < ts_node_child_count(Node); ++Index) {
			if(const TSNode Child = ts_node_child(Node, Index); !Src.Is(Child, Role::Comment)) Children.push_back(Child);
		}
		// コメントでない子の並びの返戻
		return Children;
	}

	/**
	 * 山括弧誤解析の一括判定関数（C++ の `b < c && d > e`・C# / TS の `a < b, c >> d` が型引数リストへ化けた形かを構造で見分ける）
	 * @param Src ソースコード
	 * @return 実体化の節点と型引数リスト毎の誤解析の判定（投影の間だけ持つ）
	 */
	std::unordered_map<const void *, uint8_t> SourceProjector::PrepareTemplateContexts(const ::ParsedTree &Src) {
		// 節点別の判定控え
		std::unordered_map<const void *, uint8_t> Memo;
		// 型引数を誤読し得ない言語と，型引数リストを持ち得ない（`<` の無い）ソースの空の判定の返戻
		if(!Src.GetReading().MayMisreadTemplateArguments || Src.CodeText().find('<') == std::string_view::npos) return Memo;
		// 型引数の誤読条件の判定
		const auto Misparsed =
		[&Src](const TSNode List, const TSNode Template, const TSNode After, const TSNode Outer, const bool IsExpression) -> bool {
			// 閉じの後に無名の字句が続く型引数の判定
			if(!ts_node_is_null(After) && !ts_node_is_named(After)) {
				// 解析器が型引数の閉じへ割った >>・>= 等は元字句を保ち，正当なネストの >> は二項式の外なので除外
				// 字句を割った誤解析である事の返戻
				if(
					ts_node_end_byte(Template) == ts_node_start_byte(After) &&
					(Src.Is(After, Role::AngleCloseToken) || Src.Is(After, Role::AssignmentToken)) && !ts_node_is_null(Outer) &&
					(Src.Is(Outer, Role::BinaryOperation) || Src.Is(Outer, Role::AssignmentOperation))
				) return true;
				// 閉じの後に `::` が続く型引数は，比較の被演算子に為れない字句が続く為に正当なテンプレートである事の返戻
				if(Src.Is(After, Role::DoubleColonToken)) return false;
			}
			// 空の実引数が閉じの後に続く型引数を正当とする事の返戻
			if(!ts_node_is_null(After) && Src.Is(After, Role::ArgumentSequence) && !ts_node_named_child_count(After)) return false;
			// 括弧内の節点
			const TSNode Inner = ts_node_named_child(List, 0);
			// 初期化子側の二項式を誤解析とする事の返戻（宣言子側の実体化は正当なテンプレート，f<N + 1> と b < c && d >> e）
			return !ts_node_is_null(Inner) && Src.Is(Inner, Role::BinaryOperation) && IsExpression;
		};
		// 実体化の節点の最初の型引数リスト（無ければ空）
		const auto ListOf = [&Src](const TSNode Template) -> TSNode {
			// 型引数リストの節点
			TSNode List {};
			ForEachNamedChild(
				Template,
				[&Src, &List](const TSNode Child) -> bool {
					// 山括弧の並び以外を読み飛ばす事の返戻
					if(!Src.Is(Child, Role::AngleBracketList)) return true;
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
			TSNode Outer; // 対象を包む節点
			bool IsExpression; // 式側の文脈か
		};
		// 深さ三乗を避ける構文木の単一先行順走査
		// 構文木の根節点
		std::vector<Frame> Frames { { Src.GetRoot(), TSNode{}, TSNode{}, true } };
		std::vector<TSNode> Kids;
		while(!Frames.empty()) {
			// 現在処理する走査枠
			const Frame Top = Frames.back();
			// 構文木走査の作業列からの末尾要素取出
			Frames.pop_back();
			// 走査用のコメントでない子節点列（直後の字句の判定を文中のコメントの有無で変えない，整形仕様 1.2）
			Kids = CodeChildrenOf(Src, Top.Node);
			// 子の文脈：初期化子を持つ宣言子の初期化子は式の側
			const bool IsInitDeclarator = Src.Is(Top.Node, Role::InitDeclarator);
			const TSNode Value = IsInitDeclarator ? Src.FieldChild(Top.Node, ChildRole::Value) : TSNode{};
			const bool IsHost = Src.Is(Top.Node, Role::MisparsedTemplateHost);
			for(size_t Idx = 0; Idx < Kids.size(); ++Idx) {
				// 走査中の子節点
				const TSNode Child = Kids[Idx], After = Idx + 1 < Kids.size() ? Kids[Idx + 1] : TSNode{};
				const bool IsExpression = IsInitDeclarator ?
				ts_node_eq(Child, Value) :
				!Src.Is(Top.Node, Role::TypedDeclaration) && (Top.IsExpression || Src.Is(Top.Node, Role::TemplateScanBoundary));
				if(Src.Is(Child, Role::MisparsedTemplateHost)) {
					// 実体化の節点と其の型引数リストは同一判定
					if(const TSNode List = ListOf(Child); !ts_node_is_null(List)) {
						// 節点別の判定控えの確定
						Memo[Child.id] = Memo[List.id] = Misparsed(List, Child, After, Top.Node, IsExpression);
					}
				} else if(!IsHost && Src.Is(Child, Role::AngleBracketList)) {
					// 節点別の判定控えの確定
					Memo[Child.id] = Misparsed(Child, Top.Node, Top.After, Top.Outer, IsExpression);
				}
				if(ts_node_child_count(Child)) Frames.push_back({ Child, After, Top.Node, IsExpression });
			}
		}
		// 誤解析の判定の返戻
		return Memo;
	}

	/**
	 * 山括弧誤解析判定関数
	 * @param Misreads `PrepareTemplateContexts` が控えた判定
	 * @param Node 実体化の節点又は型引数リスト
	 * @return 誤解析なら true（型引数リストが二項式を直に持つ形，型・リテラルを取る正当な実体化は false）
	 */
	bool SourceProjector::IsMisparsedTemplate(const std::unordered_map<const void *, uint8_t> &Misreads, const TSNode Node) {
		const std::unordered_map<const void *, uint8_t>::const_iterator Hit = Misreads.find(Node.id);
		// 控えた判定（型引数リストを持たない節点は誤解析でない）の返戻
		return Hit != Misreads.end() && Hit->second;
	}

	/**
	 * C# の宣言式と誤読した乗算かの判定関数
	 * C# 解析器が実引数の乗算を宣言式と誤読したかを判定する
	 * @param Src ソースコード
	 * @param PointerType 宣言式の型に読まれたポインタ型
	 * @return 乗算と読む位置なら true
	 */
	bool SourceProjector::ReadsAsCsProduct(const ::ParsedTree &Src, const TSNode PointerType) {
		// 宣言式の節点
		const TSNode Declaration = ts_node_parent(PointerType), Argument = ts_node_parent(Declaration);
		// 実引数の宣言式でない又は `out` の実引数の宣言は，本当の宣言の返戻
		if(
			!Src.Is(Declaration, Role::DeclarationExpression) || !Src.Is(Argument, Role::Argument) ||
			HasUnnamedTokenChild(Src, Argument, Src.GetRules().Words[static_cast<size_t>(Word::OutputArgument)].front())
		) return false;
		// 組のネストを外側へ遡り，分解の代入の左辺と foreach の分解の変数は宣言と解釈
		TSNode Tuple = ts_node_parent(Argument);
		while(Src.Is(Tuple, Role::TupleExpression)) {
			// 対象を保持する節点
			const TSNode Host = ts_node_parent(Tuple);
			if(Src.Is(Host, Role::Argument)) {
				// タプル候補の節点の確定
				Tuple = ts_node_parent(Host);
				continue;
			}
			// 分解の代入の左辺・foreach の分解の変数でなければ乗算の返戻
			return !(
				Src.Is(Host, Role::Assignment) && ts_node_eq(Src.FieldChild(Host, ChildRole::Left), Tuple) ||
				Src.Is(Host, Role::ForeachStatement)
			);
		}
		// 呼出の実引数は乗算の返戻
		return true;
	}

	/**
	 * 型を定義する容器かの判定関数（整形仕様 4.2）
	 * 名前空間を除く型の定義（クラス・構造体・共用体）を，基底の成員が名前を隠し得る範囲と型の名前の宣言の判定で共有する
	 * @param Src 解析結果
	 * @param Node 節点
	 * @return 名前空間でない型を定義する容器なら true
	 */
	bool SourceProjector::IsTypeScope(const ::ParsedTree &Src, const TSNode Node) {
		// 名前空間を除く型の定義の容器かの返戻
		return Src.Is(Node, Role::ClassLike) && !Src.Is(Node, Role::NamespaceDefinition);
	}

	/**
	 * 比較と誤読した総称呼出の判定関数
	 * TypeScript は `<` から `>` までが型として読め，`>` の次の字句が `(` の時に総称呼出として確定する
	 * 解析器は此の形を二項式のネストとして読む為，投影の役割を此処で正す（整形仕様 6.6）
	 * 子はコメントを除いて数え，文中のコメントの有無で判定を変えない（整形仕様 1.2）
	 * @param Src 解析結果
	 * @param Node 判定する節点
	 * @return 誤読した総称呼出の外側の二項式なら true
	 */
	bool SourceProjector::IsMisparsedGenericCall(const ::ParsedTree &Src, const TSNode Node) {
		// 型引数を持たない言語と二項式でない節点の返戻
		if(!Src.GetReading().MayMisreadGenericCalls || ts_node_is_null(Node) || !Src.Is(Node, Role::BinaryOperation)) return false;
		const std::vector<TSNode> Parts = CodeChildrenOf(Src, Node);
		// 閉じの山括弧の三つ組でない節点の返戻
		if(Parts.size() != 3 || !Src.Is(Parts[1], Role::GreaterThanToken)) return false;
		// 閉じの後に続く部分の最左の字句（呼出・メンバ参照へ続く形も総称呼出の確定条件を満たす）
		TSNode Leftmost = Parts[2];
		for(std::vector<TSNode> Children = CodeChildrenOf(Src, Leftmost); !Children.empty(); Children = CodeChildrenOf(Src, Leftmost)) {
			Leftmost = Children.front();
		}
		// 閉じの後が実引数の括弧で始まらない場合の返戻
		if(!Src.Is(Leftmost, Role::OpeningParenToken)) return false;
		const std::vector<TSNode> Left = CodeChildrenOf(Src, Parts[0]);
		// 左が `<` を演算子とする二項式で在る事の返戻
		return Src.Is(Parts[0], Role::BinaryOperation) && Left.size() == 3 && Src.Is(Left[1], Role::LessThanToken);
	}

	/**
	 * 修飾を含む名前の並びの取得関数（整形仕様 4.2）
	 * 名前・型の名前・名前空間の名前の字句と修飾の `::` だけが交互に並ぶ節点の名前を原文の順に並べる（コメントは読み飛ばす）
	 * @param Src 解析結果
	 * @param Node 名前の節点（修飾付の名前又は入れ子の名前空間の名前を含む）
	 * @return 名前の並び（其の他の字句を含む物と `::` で始まる又は終わる物は空）
	 */
	std::vector<std::string> SourceProjector::NamesOf(const ::ParsedTree &Src, const TSNode Node) {
		// 名前と区切りの順序の検査
		std::vector<std::string> Names;
		// 次に名前を置く位置か（最初と区切りの後）
		bool IsSeparated = true;
		for(std::vector<TSNode> Pending { Node }; !Pending.empty();) {
			const TSNode Current = Pending.back();
			Pending.pop_back();
			if(Src.Is(Current, Role::Comment)) continue;
			if(const uint32_t Count = ts_node_child_count(Current)) {
				for(uint32_t Index = Count; Index--;) Pending.push_back(ts_node_child(Current, Index));
				continue;
			}
			const bool IsName =
			Src.Is(Current, Role::Identifier) || Src.Is(Current, Role::TypeName) || Src.Is(Current, Role::NamespaceName);
			// 名前と区切りが交互に並ばない字句の空の並びの返戻
			if(IsName ? !IsSeparated : !Src.Is(Current, Role::DoubleColonToken) || IsSeparated) return {};
			if(IsName) Names.emplace_back(Src.View(Current));
			IsSeparated = !IsName;
		}
		// 区切りで終わる並びは空，其の他は名前の並びの返戻
		return IsSeparated ? std::vector<std::string>() : Names;
	}

	/**
	 * 比較と誤読した型引数を持つ型の宣言の型と宣言子の取得関数（整形仕様 4.2）
	 * 結果を捨てる式文の全体が「名前 `<` 型引数を右端に持つ名前の並び `>` 名前」だけから成る形の，先頭の名前と宣言子を返す
	 * 型引数の部分は名前・型の名前・名前空間の名前・基本型と `::` `<` `>` `,` だけから成り，比較・演算・呼出・値を含まない（呼出・多重定義演算を含む式は読み替えない）
	 * 子はコメントを除いて数え，文中のコメントの有無で判定を変えない（整形仕様 1.2）
	 * @param Src 解析結果
	 * @param Node 外側の二項式の候補
	 * @return 先頭の名前と宣言子（形が違えば無し）
	 */
	std::optional<std::pair<TSNode, TSNode>> SourceProjector::MisreadDeclarationParts(const ::ParsedTree &Src, const TSNode Node) {
		// 型の宣言を読み違えない言語の返戻
		if(!Src.GetReading().MayMisreadTemplateDeclarations) return std::nullopt;
		// 二項式でない節点の返戻
		if(ts_node_is_null(Node) || !Src.Is(Node, Role::BinaryOperation)) return std::nullopt;
		const std::vector<TSNode> Parts = CodeChildrenOf(Src, Node);
		// 閉じの山括弧の三つ組でない節点の返戻
		if(Parts.size() != 3 || !Src.Is(Parts[1], Role::GreaterThanToken)) return std::nullopt;
		const TSNode Statement = ts_node_parent(Node);
		const std::vector<TSNode> StatementParts = CodeChildrenOf(Src, Statement), Left = CodeChildrenOf(Src, Parts[0]);
		// 式文の全体でない二項式と，左が開きの山括弧の三つ組でない物と，宣言子が名前でない物（呼出を含む物）の返戻
		if(
			!Src.Is(Statement, Role::ExpressionStatement) || StatementParts.empty() || !ts_node_eq(StatementParts.front(), Node) ||
			!Src.Is(Parts[0], Role::BinaryOperation) || Left.size() != 3 || !Src.Is(Left[1], Role::LessThanToken) ||
			!Src.Is(Parts[2], Role::Identifier)
		) return std::nullopt;
		// 名前でない左端の返戻
		if(NamesOf(Src, Left[0]).empty()) return std::nullopt;
		// 型引数の並びを持つ右端（修飾付の名前は最後の名前付の子を辿る）
		TSNode Arguments = Left[2];
		while(!Src.Is(Arguments, Role::MisparsedTemplateHost) && ts_node_named_child_count(Arguments)) {
			Arguments = ts_node_named_child(Arguments, ts_node_named_child_count(Arguments) - 1);
		}
		// 型引数の並びを持たない右端の返戻
		if(!Src.Is(Arguments, Role::MisparsedTemplateHost)) return std::nullopt;
		for(std::vector<TSNode> Pending { Left[2] }; !Pending.empty();) {
			const TSNode Current = Pending.back();
			Pending.pop_back();
			// 比較・演算を含む部分の返戻
			if(Src.Is(Current, Role::BinaryOperation)) return std::nullopt;
			if(const uint32_t Count = ts_node_child_count(Current)) {
				for(uint32_t Index = 0; Index < Count; ++Index) Pending.push_back(ts_node_child(Current, Index));
				continue;
			}
			// 名前・基本型と区切り・山括弧以外の字句（値・演算子・括弧）の返戻（コメントは読み飛ばす）
			if(
				!Src.Is(Current, Role::Comment) && !Src.Is(Current, Role::Identifier) && !Src.Is(Current, Role::TypeName) &&
				!Src.Is(Current, Role::NamespaceName) && !Src.Is(Current, Role::PrimitiveType) && !Src.Is(Current, Role::DoubleColonToken) &&
				!Src.Is(Current, Role::LessThanToken) && !Src.Is(Current, Role::GreaterThanToken) && !Src.Is(Current, Role::CommaToken)
			) return std::nullopt;
		}
		// 先頭の名前と宣言子の返戻
		return std::pair(Left[0], Parts[2]);
	}

	/**
	 * 型引数を持つ型の宣言と確定する名前の束縛の取得関数（整形仕様 4.2）
	 * 名前空間の範囲で宣言したクラス・別名のテンプレートの名前と其の宣言の位置と，最も外の名前空間の名前を集める
	 * 其れ等を隠し得る名前（識別子・成員名，其の他の型・内側又は無名の名前空間の中の名前空間・名前空間の別名・型の別名・型引数の宣言名）と，他の範囲の名前を取り込む using の有無を控える
	 * 比較と誤読した宣言の型の部分の名前は型の名前の参照として隠す名前に数えず，宣言子は隠す名前に数える
	 * 名前空間の名前は表に一度だけ置き，走査の枠は其の番号を持つ
	 * 計算量：節点数 V と名前の長さの和 L に対し O(V + L)
	 * @param Src 解析結果
	 * @return 名前の束縛
	 */
	SourceProjector::TemplateBindings SourceProjector::TemplateBindingsOf(const ::ParsedTree &Src) {
		TemplateBindings Bindings;
		// 走査する節点と，其の名前空間の名前の番号と，名前空間の範囲か，名前空間の本体か，名前で辿れる名前空間の中か，名前空間の範囲のテンプレートが包む宣言か，型の名前を宣言する部分の中か
		struct Frame {
			TSNode Node;
			uint32_t Path;
			bool IsNamespaceScope;
			bool IsNamespaceBody;
			bool IsQualifiable;
			bool IsTemplated;
			bool IsDeclaringTypes;
		};
		// 名前空間の名前の表（`::` で繋ぐ，最初は最も外の範囲）
		std::vector<std::string> Paths(1);
		// 名前空間の名前と名前を繋いだ名前
		const auto Join = [](const std::string &Path, const std::string_view Name) -> std::string {
			// 繋いだ名前の返戻
			return Path.empty() ? std::string(Name) : Path + "::" + std::string(Name);
		};
		// 名前空間と宣言名の束縛の走査
		const TSNode Root = Src.GetRoot();
		for(std::vector<Frame> Pending { { Root, 0, false, false, true, false, false } }; !Pending.empty();) {
			const Frame Top = Pending.back();
			Pending.pop_back();
			const TSNode Node = Top.Node;
			if(Src.Is(Node, Role::ScopeImport)) Bindings.IsOpen = true;
			if(const std::optional<std::pair<TSNode, TSNode>> Parts = MisreadDeclarationParts(Src, Node)) {
				// 比較と誤読した宣言は宣言子だけを隠す名前とする
				Bindings.Shadows.emplace(Src.View(Parts->second));
				continue;
			}
			const uint32_t Count = ts_node_child_count(Node);
			if(!Count) {
				if(Src.Is(Node, Role::Identifier) || Src.Is(Node, Role::FieldName) || Top.IsDeclaringTypes && Src.Is(Node, Role::TypeName)) {
					Bindings.Shadows.emplace(Src.View(Node));
				}
				continue;
			}
			const TSNode Name = Src.FieldChild(Node, ChildRole::Name);
			const bool IsNamedType = !ts_node_is_null(Name) && Src.Is(Name, Role::TypeName);
			const bool IsTemplateType = IsTypeScope(Src, Node) || Src.Is(Node, Role::AliasDeclaration);
			const bool IsNamespaceDefinition = Src.Is(Node, Role::NamespaceDefinition);
			const bool IsTypeDefinition = Src.Is(Node, Role::TypeDefinition);
			// 名前空間の範囲のテンプレートの宣言名と位置（同じ名前は最も前の宣言）
			if(IsTemplateType && IsNamedType && Top.IsTemplated) {
				const auto [Found, IsNew] = Bindings.Templates.try_emplace(Join(Paths[Top.Path], Src.View(Name)), Src.Start(Node));
				if(!IsNew) Found->second = std::min(Found->second, Src.Start(Node));
			} else if(
				// 其の他の型・名前空間の別名の宣言名
				(IsTemplateType || Src.Is(Node, Role::EnumSpecifier)) && IsNamedType ||
				Src.Is(Node, Role::NamespaceAliasDefinition) && !ts_node_is_null(Name)
			) Bindings.Shadows.emplace(Src.View(Name));
			uint32_t Path = Top.Path;
			bool IsQualifiable = Top.IsQualifiable;
			if(IsNamespaceDefinition) {
				const std::vector<std::string> Names = ts_node_is_null(Name) ? std::vector<std::string>() : NamesOf(Src, Name);
				// 無名の名前空間の中は名前で辿れない
				if(Names.empty()) IsQualifiable = false;
				std::string Joined = Paths[Top.Path];
				for(const std::string &Part : Names) {
					// 最も外の範囲の名前空間の名前と，内側又は無名の名前空間の中の名前空間の隠し得る名前
					if(Joined.empty() && Top.IsNamespaceScope && IsQualifiable && &Part == &Names.front()) Bindings.Namespaces.insert(Part);
					else Bindings.Shadows.insert(Part);
					Joined = Join(Joined, Part);
				}
				if(!Names.empty()) {
					Path = static_cast<uint32_t>(Paths.size());
					Paths.push_back(std::move(Joined));
				}
			}
			const TSNode Body = IsNamespaceDefinition ? Src.FieldChild(Node, ChildRole::Body) : TSNode{};
			const TSNode AliasedType = IsTypeDefinition ? Src.FieldChild(Node, ChildRole::Type) : TSNode{};
			const bool IsTemplate = Src.Is(Node, Role::TypeParameterScope);
			for(uint32_t Index = Count; Index--;) {
				const TSNode Child = ts_node_child(Node, Index);
				const bool IsParameters = IsTemplate && Src.Is(Child, Role::AngleBracketList);
				Pending.push_back(
					{
						Child,
						Path,
						ts_node_eq(Node, Root) || Top.IsNamespaceBody,
						!ts_node_is_null(Body) && ts_node_eq(Child, Body),
						IsQualifiable,
						IsTemplate && Top.IsNamespaceScope && IsQualifiable && !IsParameters,
						Top.IsDeclaringTypes || IsParameters || IsTypeDefinition && !ts_node_eq(Child, AliasedType)
					}
				);
			}
		}
		// 名前の束縛の返戻
		return Bindings;
	}

	/**
	 * 比較と誤読した型引数を持つ型の宣言の判定関数（整形仕様 4.2）
	 * C++ の解析器は `ns::T< ns::T<ns::T<x>> > Name;` 等の宣言を，比較を連ねた式文として読む事が有る
	 * 先頭の名前が，使う位置より前に同じファイルの名前空間の範囲で宣言したクラス・別名のテンプレートへ束縛され，`<` が型引数の並びを開いて文全体が宣言と確定する時だけ，投影の役割を此処で正す
	 * 比較の結果を捨てる事だけを根拠にせず，名前を隠し得る宣言・参照，他の範囲の名前の取込，基底の成員が名前を隠し得るクラスの範囲と，未確定の名前は読み替えない
	 * @param Src 解析結果
	 * @param Node 判定する節点
	 * @param Bindings 名前の束縛（最初の候補で求めて控える）
	 * @return 誤読した宣言の外側の二項式なら true
	 */
	bool SourceProjector::IsMisparsedTemplateDeclaration(
		const ::ParsedTree &Src,
		const TSNode Node,
		std::optional<TemplateBindings> &Bindings
	) {
		const std::optional<std::pair<TSNode, TSNode>> Parts = MisreadDeclarationParts(Src, Node);
		// 宣言の読違の形でない節点の返戻
		if(!Parts) return false;
		if(!Bindings) Bindings.emplace(TemplateBindingsOf(Src));
		const std::vector<std::string> Names = NamesOf(Src, Parts->first);
		// 他の範囲の名前を取り込む原文と，テンプレートの名前を隠し得る名前を持つ原文の返戻
		if(Bindings->IsOpen || Bindings->Shadows.contains(Names.back())) return false;
		// 使う位置を囲む名前で辿れる名前空間の名前（外から順，無名の名前空間より内側は辿れない為に除く）
		std::vector<std::string> Scope;
		for(TSNode Up = ts_node_parent(Node); !ts_node_is_null(Up); Up = ts_node_parent(Up)) {
			// 基底の成員が名前を隠し得るクラスの範囲の返戻
			if(IsTypeScope(Src, Up)) return false;
			if(Src.Is(Up, Role::FunctionDefinition)) {
				TSNode Declarator = Src.FieldChild(Up, ChildRole::Declarator);
				while(!ts_node_is_null(Declarator) && !ts_node_is_null(Src.FieldChild(Declarator, ChildRole::Declarator))) {
					Declarator = Src.FieldChild(Declarator, ChildRole::Declarator);
				}
				// クラスの外で定義した成員関数の返戻
				if(!ts_node_is_null(Declarator) && Src.Is(Declarator, Role::QualifiedIdentifier)) return false;
			}
			if(!Src.Is(Up, Role::NamespaceDefinition)) continue;
			const TSNode Name = Src.FieldChild(Up, ChildRole::Name);
			if(
				const std::vector<std::string> Enclosing = ts_node_is_null(Name) ? std::vector<std::string>() : NamesOf(Src, Name);
				Enclosing.empty()
			) Scope.clear();
			else Scope.insert(Scope.begin(), Enclosing.begin(), Enclosing.end());
		}
		std::string Joined;
		for(const std::string &Part : Names) Joined += Joined.empty() ? Part : "::" + Part;
		// 使う位置より前に宣言したテンプレートか
		const auto IsDeclaredBefore = [&Bindings, &Src, Node](const std::string &Name) -> bool {
			const std::unordered_map<std::string, uint32_t>::const_iterator Found = Bindings->Templates.find(Name);
			// 前の宣言の有無の返戻
			return Found != Bindings->Templates.end() && Found->second < Src.Start(Node);
		};
		if(Names.size() > 1) {
			// 修飾付の名前は，最初の修飾を隠し得る名前が無く最も外の名前空間から辿れるテンプレートかの返戻
			return Bindings->Namespaces.contains(Names.front()) && !Bindings->Shadows.contains(Names.front()) && IsDeclaredBefore(Joined);
		}
		// 修飾の無い名前は，使う位置を囲む名前空間の何れかで宣言したテンプレートかの返戻
		if(IsDeclaredBefore(Joined)) return true;
		std::string Path;
		for(const std::string &Part : Scope) {
			Path += Path.empty() ? Part : "::" + Part;
			// 囲む名前空間で宣言したテンプレートの返戻
			if(IsDeclaredBefore(Path + "::" + Joined)) return true;
		}
		// 未確定の名前の返戻
		return false;
	}

	/**
	 * 原文の隣接が読みを決める境界かの判定関数（整形仕様 4.2，字句間空白仕様 4.2）
	 * 空白の有無で演算子の役割・実引数の読み・注釈の適用先・値の字句が変わる境界だけを記録し，其の他の原文の空白は捨てる
	 * 空白の追加が意味を変える記法は字句の間の空白の有無が値の区切り・子孫結合子を成す為，全ての子の境界を記録する（区切り字句の周りの空白は字句間空白の規則が決める）
	 * @param Syntax 構文モデル
	 * @param Parent 親の節点
	 * @param Next 境界の後の子
	 * @return 原文の隣接を構文事実として記録するなら true
	 */
	bool SourceProjector::KeepsAdjacency(const SyntaxModel &Syntax, const NodeId Parent, const NodeId Next) {
		// 隣接が意味を持つ境界かの返戻
		return Syntax.Rules().IsWhitespaceSemantic || Syntax.Is(Parent, Role::AdjacencySensitiveInfix) ||
		Syntax.Is(Parent, Role::AdjacencyRecordingHost) || Syntax.Is(Next, Role::AdjacencyRecordingOperand);
	}

	/**
	 * 子の間の隠れた字句の範囲の取得関数（整形仕様 4.4）
	 * 文法が子に出さず子の間の原文にだけ残る字句（C# の修飾子付の暗黙型のラムダの仮引数 `(ref x) => x` の `(` 等）を，空白とコメントを除いて切り出す
	 * コメントの前置区切り（全角スペースを含む）はコメントの配置が扱う為，字句に含めない（整形仕様 7.6）
	 * @param Src コメントを分離した解析結果
	 * @param From 間隙の開始位置
	 * @param To 間隙の終了位置
	 * @param Gaps 間隙から除くコメントの範囲と解析用の括弧の位置
	 * @return 隠れた字句毎の原文の範囲
	 */
	std::vector<std::pair<uint32_t, uint32_t>> SourceProjector::HiddenTokens(
		const ::ParsedTree &Src,
		const uint32_t From,
		const uint32_t To,
		const GapExclusions &Gaps
	) {
		// 間隙から除くコメントと読取規則の準備
		std::vector<std::pair<uint32_t, uint32_t>> Tokens;
		const std::vector<std::pair<uint32_t, uint32_t>> &CommentRanges = Gaps.CommentRanges;
		const ReadingRules &Reading = Src.GetReading();
		std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Comment =
		std::lower_bound(CommentRanges.begin(), CommentRanges.end(), std::pair<uint32_t, uint32_t>(From, 0));
		const auto IsSynthetic = [&Gaps](const uint32_t Position) -> bool {
			// 解析用の括弧の位置かの返戻
			return std::binary_search(Gaps.SyntheticPositions.begin(), Gaps.SyntheticPositions.end(), Position);
		};
		// 次のコメントの前置区切りの始まり
		uint32_t Lead = Comment == CommentRanges.end() ? To : TextEdit::SeparatorBefore(Src, Comment->first, Reading).first;
		for(uint32_t Cursor = From; Cursor < To;) {
			if(Comment != CommentRanges.end() && Lead <= Cursor) {
				// 前置区切りとコメントの読飛
				Cursor = std::max(Cursor, Comment->second);
				Lead = ++Comment == CommentRanges.end() ? To : TextEdit::SeparatorBefore(Src, Comment->first, Reading).first;
				continue;
			}
			if(IsSynthetic(Cursor)) {
				++Cursor;
				continue;
			}
			if(const size_t Width = TextEdit::IntertokenSpaceAt(Src, Cursor, Reading)) {
				Cursor += static_cast<uint32_t>(Width);
				continue;
			}
			// 空白かコメントの前置区切りの前迄の字句
			const uint32_t Start = Cursor, Stop = std::min(To, Lead);
			while(Cursor < Stop && !IsSynthetic(Cursor) && !TextEdit::IntertokenSpaceAt(Src, Cursor, Reading)) ++Cursor;
			Tokens.emplace_back(Start, Cursor);
		}
		// 隠れた字句の範囲の返戻
		return Tokens;
	}

	/**
	 * 文法が子に出さない語の種別の取得関数（整形仕様 4.2）
	 * @param Src コメントを分離した解析結果
	 * @param Host 語を子の間に持つ節点
	 * @return 節点が子に出さない語の種別（其の様な語を持たない節点は無し）
	 */
	const KindDefinition *SourceProjector::HiddenLexemeKind(const ::ParsedTree &Src, const TSNode Host) {
		const std::unordered_map<std::string_view, const KindDefinition *> &Kinds = Src.GetReading().HiddenLexemeKinds;
		const std::unordered_map<std::string_view, const KindDefinition *>::const_iterator Found =
		Kinds.find(Src.GetProfile().SymbolName(ts_node_symbol(Host)));
		// 語の種別の返戻
		return Found == Kinds.end() ? nullptr : Found->second;
	}

	/**
	 * 字句の節点にする隠れた字面かの判定関数（整形仕様 4.4）
	 * @param Src コメントを分離した解析結果
	 * @param Host 字面を子の間に持つ節点
	 * @param Text 隠れた字面
	 * @return 文を終える区切記号（文境界）でも行継続の記号でもない，文法の字句又は文法が子に出さない語なら true
	 */
	bool SourceProjector::IsTokenizedHidden(const ::ParsedTree &Src, const TSNode Host, const std::string_view Text) {
		const SyntaxRules &Rules = Src.GetRules();
		// 文境界でない文法の字句か文法が子に出さない語かの返戻
		return !Rules.IsSpelling(Text, Word::StatementEnd) && !Rules.IsSpelling(Text, Word::LineContinuation) &&
		(Src.GetProfile().TokenSymbol(Text) || HiddenLexemeKind(Src, Host));
	}

	/**
	 * 字句にしない隠れた字面を持つ間隙かの判定関数（整形仕様 4.4）
	 * 隠れた字句の内，文を終える区切記号（文境界）・行継続の記号と文法の字句（字句の節点にする物）を除いた字面（文字列の引用符等）を持つかを見る
	 * @param Src コメントを分離した解析結果
	 * @param Host 間隙を子の間に持つ節点
	 * @param From 間隙の開始位置
	 * @param To 間隙の終了位置
	 * @param Gaps 間隙から除くコメントの範囲と解析用の括弧の位置
	 * @return 原文の間隙として保つ字面を持てば true
	 */
	bool SourceProjector::HasOpaqueHidden(
		const ::ParsedTree &Src,
		const TSNode Host,
		const uint32_t From,
		const uint32_t To,
		const GapExclusions &Gaps
	) {
		const std::vector<std::pair<uint32_t, uint32_t>> Pieces = HiddenTokens(Src, From, To, Gaps);
		// 文境界でも文法の字句でもない字面の有無の返戻
		return std::any_of(
			Pieces.begin(),
			Pieces.end(),
			[&](const std::pair<uint32_t, uint32_t> &Range) -> bool {
				const std::string_view Text = std::string_view(Src).substr(Range.first, Range.second - Range.first);
				const SyntaxRules &Rules = Src.GetRules();
				// 文境界でも字句でもない字面かの返戻
				return !Rules.IsSpelling(Text, Word::StatementEnd) && !Rules.IsSpelling(Text, Word::LineContinuation) &&
				!Src.GetProfile().TokenSymbol(Text) && !HiddenLexemeKind(Src, Host);
			}
		);
	}

	/**
	 * コメントを除いた間隙の取得関数（整形仕様 4.4）
	 * 間隙からコメントを除き，コメントを挟んだ空白は改行の数だけを保つ（独立コメントの行の改行は数えず，改行の無い間隙は最後のコメントの後の空白を保つ）
	 * @param Src コメントを分離した解析結果
	 * @param Separated 投影の間だけ持つコメントの分離の結果
	 * @param From 間隙の開始位置
	 * @param To 間隙の終了位置
	 * @param CommentRanges 原文のコメントの範囲（原文順）
	 * @return コメントを除いた間隙（コメントを含まなければ原文の儘）
	 */
	std::string SourceProjector::CommentFreeGap(
		const ::ParsedTree &Src,
		const CommentSeparation &Separated,
		const uint32_t From,
		const uint32_t To,
		const std::vector<std::pair<uint32_t, uint32_t>> &CommentRanges
	) {
		// コメントで分けた空白の区間毎の改行の数と，最後の区間が空でないか
		uint32_t Newlines = 0, Previous = 0;
		bool HasSpace = false, HasComment = false;
		uint32_t Cursor = From;
		for(
			std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Comment =
			std::lower_bound(CommentRanges.begin(), CommentRanges.end(), std::pair<uint32_t, uint32_t>(From, 0));
			Cursor < To;
			++Comment
		) {
			// 錨へ紐付けたコメントは分離の記録で除き，字面を保つ区画に残るコメントだけで区切る
			while(Comment != CommentRanges.end() && Comment->first < To && Separated.IsSeparatedAt(Comment->first)) ++Comment;
			const uint32_t End = Comment != CommentRanges.end() && Comment->first < To ? Comment->first : To;
			const std::string Space = Separated.SeparatedText(Src, Cursor, End);
			const uint32_t Count = static_cast<uint32_t>(std::count(Space.begin(), Space.end(), '\n'));
			// 独立コメントの前後の改行は１つと数える
			Newlines += Count - (Previous && Count ? 1 : 0);
			Previous = Count;
			HasSpace = !Space.empty();
			if(End == To) break;
			HasComment = true;
			Cursor = std::min(To, Comment->second);
		}
		// コメントを含まない間隙の原文と，コメントを除いて改行か（配置の段が錨とコメントの間の空白を置く為）最後のコメントの後の空白へ畳んだ間隙の返戻
		return !HasComment ?
		Separated.SeparatedText(Src, From, To) :
		Newlines ? std::string(Newlines, '\n') : HasSpace ? std::string(" ") : std::string();
	}

	/**
	 * Go の短い宣言が同じ有効範囲の既存変数へ代入するかの記録関数
	 * 文の並びに導入済の名前と関数本体の仮引数を追い，確認出来ない位置は再代入の可能性を残す
	 * @param Syntax 投影済の構文モデル
	 */
	void SourceProjector::RecordGoBindingFacts(SyntaxModel &Syntax) {
		// 文の並び毎の束縛追跡
		for(NodeId List = 0; List < Syntax.Nodes.size(); ++List) {
			if(!Syntax.Is(List, Role::PlainStatementSequence)) continue;
			std::unordered_set<std::string> BoundNames;
			const auto AddNames = [&Syntax, &BoundNames](const NodeId Parent) -> void {
				for(const NodeId Name : Syntax.ChildrenOf(Parent)) {
					if(!Syntax.Is(Name, Role::Identifier)) continue;
					if(const SyntaxNode &Named = Syntax.GetNode(Name); !Named.Facts.DiscardsBinding) BoundNames.insert(Named.Lexeme);
				}
			};
			// 関数の仮引数の束縛
			if(
				const NodeId Block = Syntax.ParentOf(List), Function = Syntax.ParentOf(Block);
				Syntax.Is(Block, Role::LexicalBlock) && (
					Syntax.Is(Function, Role::FunctionDeclarationNode) || Syntax.Is(Function, Role::MethodDeclaration) ||
					Syntax.Is(Function, Role::FunctionLiteral)
				)
			) for(const NodeId Child : Syntax.ChildrenOf(Function)) {
				if(!Syntax.Is(Child, Role::DeclaratorParameters)) continue;
				WalkNodes(
					Syntax,
					Child,
					[&Syntax, &AddNames](const NodeId Parameter) -> WalkStep {
						// 通常と可変長の仮引数だけを収集する
						if(!Syntax.Is(Parameter, Role::ParameterDeclaration) && !Syntax.Is(Parameter, Role::VariadicParameterDeclaration)) {
							// 仮引数でない節点は更に辿る事の返戻
							return WalkStep::Descend;
						}
						AddNames(Parameter);
						// 仮引数の型の中へ降りない事の返戻
						return WalkStep::Skip;
					}
				);
			}
			// case 節の受信名と別名の束縛
			if(const NodeId Case = Syntax.ParentOf(List); Syntax.Is(Case, Role::CommunicationCase)) {
				if(
					const NodeId Communication = Syntax.FieldChild(Case, ChildRole::Communication);
					Syntax.Is(Communication, Role::ReceiveStatement)
				) {
					const bool Declares = std::ranges::any_of(
						Syntax.ChildrenOf(Communication),
						[&Syntax](const NodeId Child) -> bool {
							// 短い宣言の記号かの返戻
							return Syntax.Is(Child, Role::ShortAssignmentToken);
						}
					);
					if(Declares) AddNames(Syntax.FieldChild(Communication, ChildRole::Left));
				}
			} else if(
				(Syntax.Is(Case, Role::TypeCase) || Syntax.Is(Case, Role::DefaultCase)) && Syntax.Is(Syntax.ParentOf(Case), Role::TypeSwitch)
			) AddNames(Syntax.FieldChild(Syntax.ParentOf(Case), ChildRole::Alias));
			// 文を順に読んだ再代入の判定
			for(const NodeId Child : Syntax.ChildrenOf(List)) {
				NodeId Statement = Child;
				while(Syntax.Is(Statement, Role::LabeledStatement)) {
					NodeId Following = NoNode;
					for(const NodeId Part : Syntax.ChildrenOf(Statement)) {
						if(Syntax.IsNamed(Part) && !Syntax.Is(Part, Role::LabelName)) Following = Part;
					}
					Statement = Following;
				}
				if(Syntax.Is(Statement, Role::ShortVariableDeclaration)) {
					const NodeId Left = Syntax.FieldChild(Statement, ChildRole::Left);
					bool HasName = false, IsKnown = Left != NoNode, Reassigns = false;
					for(const NodeId Name : Syntax.ChildrenOf(Left)) {
						if(!Syntax.IsNamed(Name)) continue;
						if(!Syntax.Is(Name, Role::Identifier)) {
							IsKnown = false;
							continue;
						}
						const SyntaxNode &Named = Syntax.GetNode(Name);
						if(Named.Facts.DiscardsBinding) continue;
						HasName = true;
						Reassigns = Reassigns || BoundNames.contains(Named.Lexeme);
					}
					Syntax.Nodes[Statement].Facts.MayReassignExistingBinding = !IsKnown || !HasName || Reassigns;
					AddNames(Left);
				} else if(Syntax.Is(Statement, Role::VariableDeclarationNode) || Syntax.Is(Statement, Role::ConstantDeclarationNode)) {
					WalkNodes(
						Syntax,
						Statement,
						[&Syntax, &AddNames](const NodeId Spec) -> WalkStep {
							// 宣言子以外の節点は更に辿る事の返戻
							if(!Syntax.Is(Spec, Role::VariableSpecification) && !Syntax.Is(Spec, Role::ConstantSpecification)) return WalkStep::Descend;
							AddNames(Spec);
							// 初期化子の中へ降りない事の返戻
							return WalkStep::Skip;
						}
					);
				}
			}
		}
		// 終了
		return;
	}

	/**
	 * 構文事実の記録関数（整形仕様 3.2, 4.2）
	 * 解析器の読違（比較を型引数・総称呼出と読んだ山括弧，C# の乗算を型と読んだ記号）と，隣接が読みを決める境界を節点へ記録する
	 * 計算量：節点数 V に対し O(V)
	 * @param Src コメントを分離した解析結果
	 * @param Separated 投影の間だけ持つコメントの分離の結果
	 * @param Gaps 間隙から除くコメントの範囲と解析用の括弧の位置
	 * @param Sources 投影した節点毎の原文の出所
	 * @param ValidationNodes 検証資料に含む解析器節点
	 * @param ExtraNodes 検証資料と異なる投影節点
	 * @param SeparatedBefore 投影した節点毎の，区切記法の後の最初の字句か
	 * @param Syntax 構文事実を記録する節点表
	 */
	void SourceProjector::RecordFacts(
		const ::ParsedTree &Src,
		const CommentSeparation &Separated,
		const GapExclusions &Gaps,
		const std::vector<Origin> &Sources,
		const std::span<const TSNode> ValidationNodes,
		const std::span<const TSNode> ExtraNodes,
		const std::vector<uint8_t> &SeparatedBefore,
		SyntaxModel &Syntax
	) {
		// 読違判定と読取規則の取得
		const std::unordered_map<const void *, uint8_t> TemplateMisreads = PrepareTemplateContexts(Src);
		const SyntaxRules &Rules = Src.GetRules();
		const ReadingRules &Reading = Src.GetReading();
		// 型引数を持つ型の宣言と確定する名前の束縛（宣言の読違の形が在る時に初めて求める）
		std::optional<TemplateBindings> NameBindings;
		const std::vector<std::pair<uint32_t, uint32_t>> &CommentRanges = Gaps.CommentRanges;
		// 解析器だけに見せた区切りと錨へ紐付けたコメントは，原文から写す保持境界の字句ではない
		std::vector<uint32_t> Inserted;
		Inserted.reserve(Src.GetInsertedBreaks().size() + Gaps.SyntheticPositions.size());
		std::set_union(
			Src.GetInsertedBreaks().begin(),
			Src.GetInsertedBreaks().end(),
			Gaps.SyntheticPositions.begin(),
			Gaps.SyntheticPositions.end(),
			std::back_inserter(Inserted)
		);
		const auto OriginalGap = [&Src, &Separated, &Inserted](const uint32_t From, const uint32_t To) -> std::string {
			// 重なる節点の間には写す字面が無い事の返戻
			if(To <= From) return {};
			std::string Gap;
			Gap.reserve(To - From);
			uint32_t Cursor = From;
			for(
				std::vector<uint32_t>::const_iterator It = std::lower_bound(Inserted.begin(), Inserted.end(), From);
				It != Inserted.end() && *It < To;
				++It
			) {
				Gap.append(Separated.SeparatedText(Src, Cursor, *It));
				Cursor = *It + 1;
			}
			Gap.append(Separated.SeparatedText(Src, Cursor, To));
			// 原文に在った字句だけの返戻
			return Gap;
		};
		// 節点毎の最初と最後の字句（識別子は先行順の為，後ろから決める）と，誤り区画の中の節点か（親から決める）
		// 最後の字句の終わりを間隙の始まりとする（Go の文の並び等は範囲に文を終える改行を含む）
		std::vector<NodeId> FirstLeaf(Sources.size()), LastLeaf(Sources.size());
		for(NodeId Id = static_cast<NodeId>(Sources.size()); Id--;) {
			const std::vector<NodeId> &Children = Syntax.Nodes[Id].Children;
			FirstLeaf[Id] = Children.empty() ? Id : FirstLeaf[Children.front()];
			LastLeaf[Id] = Children.empty() ? Id : LastLeaf[Children.back()];
		}
		// C 系の読めなかった前処理指令の物理行（行継続を含む，先行順の為に開始位置の昇順）
		std::vector<std::pair<uint32_t, uint32_t>> DirectiveLines;
		if(Rules.HasLineDirectives) for(NodeId Id = 0; Id < Sources.size(); ++Id) {
			if(!Syntax.Is(Id, Role::Error) || Sources[Id].Start >= Src.size() || Src[Sources[Id].Start] != '#') continue;
			// 前処理指令で始まる誤り区画の記録
			if(IsPreprocDirectiveAt(Src, Sources[Id].Start)) Syntax.Nodes[Id].Facts.Add(ConstraintKind::DirectiveZone);
			size_t LineEnd = Sources[Id].End;
			while((LineEnd = Src.find('\n', LineEnd)) != std::string::npos && LineEnd && Src[LineEnd - 1] == '\\') ++LineEnd;
			DirectiveLines.emplace_back(Sources[Id].Start, static_cast<uint32_t>(std::min(LineEnd, Src.size())));
		}
		// 読めなかった指令の後に同じ物理行で始まる節点か（指令の残りの字句は区画と共に原文の間隙を保つ）
		const auto IsDirectiveTail = [&DirectiveLines](const uint32_t Start) -> bool {
			const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator After = std::lower_bound(
				DirectiveLines.begin(),
				DirectiveLines.end(),
				Start,
				[](const std::pair<uint32_t, uint32_t> &Line, const uint32_t Position) -> bool {
					// 指令が位置より前に始まるかの返戻
					return Line.first < Position;
				}
			);
			// 直前に始まる指令の行の中かの返戻
			return After != DirectiveLines.begin() && Start < std::prev(After)->second;
		};
		// 誤り区画の中の節点か，Rust のマクロの字句列の中の節点か（親から決める）
		std::vector<uint8_t> IsInErrorZone(Sources.size(), 0), IsInMacroTokens(Sources.size(), 0), IsImportNamePart(Sources.size(), 0);
		for(NodeId Id = 0; Id < Sources.size(); ++Id) {
			const NodeId Parent = Syntax.Nodes[Id].Parent;
			IsInErrorZone[Id] = Syntax.Is(Id, Role::Error) || Parent != NoNode && IsInErrorZone[Parent] ||
			!DirectiveLines.empty() && IsDirectiveTail(Sources[Id].Start);
			IsInMacroTokens[Id] = Syntax.Is(Id, Role::PreservedTokenSequence) || Parent != NoNode && IsInMacroTokens[Parent];
			IsImportNamePart[Id] =
			Reading.KeepsImportNamesTogether && (Syntax.Is(Id, Role::Identifier) || Syntax.Is(Id, Role::ScopedIdentifier)) &&
			Parent != NoNode && (Syntax.Is(Parent, Role::ImportDeclarationNode) || IsImportNamePart[Parent]);
		}
		// 投影後に残る誤り（修復しなかった破損・字面を保つ区画の中の誤り・JSON の末尾カンマ以外で投影しなかった補完の字句）を祖先へ伝える
		std::vector<uint8_t> HasError(Sources.size(), 0);
		for(NodeId Id = static_cast<NodeId>(Sources.size()); Id--;) {
			const TSNode Source = SourceNode(Sources[Id], ValidationNodes, ExtraNodes);
			// 隠れた字句は誤りを持たない
			if(ts_node_is_null(Source)) continue;
			bool IsOwn = ts_node_is_error(Source) || Syntax.Nodes[Id].Facts.Has(ConstraintKind::Verbatim) && ts_node_has_error(Source);
			// 補完の字句の子は誤りの旗を立てる為，誤りを持つ節点の子だけを一度ずつ読む
			if(!IsOwn && ts_node_has_error(Source)) {
				ForEachChild(
					Source,
					[&](const TSNode Child) -> bool {
						IsOwn = ts_node_is_missing(Child) &&
						(!Reading.ToleratesTrailingCommaError || ts_node_is_null(SyntaxParser::JsonTrailingCommaToken(Src, Child)));
						// 補完の字句を見付ける迄読み続ける事の返戻
						return !IsOwn;
					}
				);
			}
			HasError[Id] = HasError[Id] || IsOwn;
			if(HasError[Id] && Syntax.Nodes[Id].Parent != NoNode) HasError[Syntax.Nodes[Id].Parent] = 1;
		}
		// 読めなかった字句を含む前処理指令の条件は指令の行の字面を保つ（見出名の `<` 等を演算子として整えない）
		if(Rules.HasLineDirectives) for(NodeId Id = 0; Id < Sources.size(); ++Id) {
			const NodeId Parent = Syntax.Nodes[Id].Parent;
			IsInErrorZone[Id] = IsInErrorZone[Id] || Parent != NoNode && (
				IsInErrorZone[Parent] ||
				HasError[Id] && Syntax.Is(Parent, Role::Preprocessor) && Syntax.FieldChild(Parent, ChildRole::Condition) == Id
			);
		}
		// 宣言直下の誤りが結付先を曖昧にする範囲の字句境界の保護
		std::vector<uint8_t> IsInBrokenDeclaration(Sources.size(), 0);
		for(NodeId Id = 0; Id < Sources.size(); ++Id) {
			const NodeId Parent = Syntax.ParentOf(Id);
			IsInBrokenDeclaration[Id] = Rules.SplitBindingsBeforeConst && (
				HasError[Id] &&
				(Syntax.Is(Id, Role::Declaration) || Syntax.Is(Id, Role::TypeMemberBody) || Syntax.Is(Id, Role::FunctionDeclarationNode)) &&
				std::any_of(
					Syntax.ChildrenOf(Id).begin(),
					Syntax.ChildrenOf(Id).end(),
					[&](const NodeId Child) {
						return Syntax.Is(Child, Role::Error);
					}
				) || Parent != NoNode && IsInBrokenDeclaration[Parent]
			);
		}
		// 節点毎の構文事実の記録
		for(NodeId Id = 0; Id < Sources.size(); ++Id) {
			SyntaxNode &Node = Syntax.Nodes[Id];
			const TSNode Source = SourceNode(Sources[Id], ValidationNodes, ExtraNodes);
			// 隠れた字句は読違も子の境界も持たない
			if(ts_node_is_null(Source)) continue;
			Node.Facts.DiscardsBinding = Syntax.Is(Id, Role::Identifier) && Syntax.Rules().IsSpelling(Node.Lexeme, Word::DiscardName);
			Node.Facts.MayReassignExistingBinding = Syntax.Is(Id, Role::ReassigningShortDeclaration);
			Node.Facts.AbsorbsLeadingComment = Reading.HasCommentAbsorbingDeclarations && Src.AbsorbsGoComment(Source);
			// C と C からも読むヘッダの繰返は <stdbool.h> の取込の後だけ真偽値の名前を綴れる
			if(Rules.BooleanNameNeedsDeclaration && (Syntax.Is(Id, Role::Loop) || Syntax.Is(Id, Role::BooleanInitializerDeclaration))) {
				Node.Facts.HasBooleanNames = Src.HasBoolNamesAt(Sources[Id].Start);
			}
			if(HasError[Id]) Node.Facts.Add(ConstraintKind::ContainsError);
			if(Src.IsAmbiguousDeclaration(Source)) Node.Facts.Add(ConstraintKind::AmbiguousDeclaration);
			if(IsImportNamePart[Id] || Syntax.Is(Id, Role::SpacelessTypeName)) Node.Facts.Add(ConstraintKind::NoOptionalBreak);
			// 文へ展開するマクロの呼出の文
			if(Reading.HasPreprocessorMacros && Src.ExpandsToStatements(Source)) Node.Facts.Add(ConstraintKind::StatementMacro);
			// 置換本体を持つマクロの定義
			if(
				Syntax.Is(Id, Role::MacroDefinition) && HasChildOf(
					Source,
					[&Src](const TSNode Child) -> bool {
						// マクロの本体の子かの返戻
						return Src.Is(Child, Role::MacroBody);
					}
				)
			) Node.Facts.Add(ConstraintKind::ValuedMacro);
			// 宣言の型の宣言子の記号へ展開するマクロ
			if(
				Reading.HasPreprocessorMacros && Node.Parent != NoNode && Syntax.Is(Node.Parent, Role::Declaration) &&
				Src.MentionsDeclaratorMacro(Source)
			) Node.Facts.Add(ConstraintKind::DeclaratorMacro);
			// 後の原文をデータとして読む PHP の `__halt_compiler` の文（関数名の大小を区別しない，入力側の判定と同じ関数で決める）
			if(Reading.HasHaltedData && ParseAdapter::IsHaltCall(Src, Source)) Node.Facts.Add(ConstraintKind::EndsCode);
			// 前処理の条件の塊を挟んで型の名前と読まれた else（処理系は前の if へ繋げる）
			if(
				Rules.HasLineDirectives && Syntax.Is(Id, Role::Declaration) && !Node.Children.empty() &&
				Syntax.IsWord(Node.Children.front(), Word::AlternateBranch)
			) {
				// else の前の文（条件の塊は越え，塊の最初の文は塊の前の文へ遡る）
				NodeId Anchor = Id, Before = NoNode;
				while(Before == NoNode && Anchor != NoNode) {
					const NodeId Up = Syntax.Nodes[Anchor].Parent;
					Before = Syntax.PrevNamedSibling(Anchor);
					while(
						Before != NoNode && (
							Syntax.Is(Before, Role::PreprocessorBranch) || Syntax.Is(Up, Role::PreprocessorBranch) &&
							(Syntax.FieldChild(Up, ChildRole::Condition) == Before || Syntax.FieldChild(Up, ChildRole::Name) == Before)
						)
					) Before = Syntax.PrevNamedSibling(Before);
					Anchor = Syntax.Is(Up, Role::PreprocessorBranch) ? Up : NoNode;
				}
				// else の結付が波括弧の増減で変わる右端の文の並び
				if(Syntax.Is(Before, Role::If)) for(NodeId Edge = Before; Edge != NoNode; Edge = Syntax.LastNamedChild(Edge)) {
					Syntax.Nodes[Edge].Facts.Add(ConstraintKind::DetachedElse);
				}
			}
			if(IsMisparsedTemplate(TemplateMisreads, Source)) Node.Facts.Add(ConstraintKind::MisparsedTemplate);
			if(IsMisparsedGenericCall(Src, Source)) Node.Facts.Add(ConstraintKind::MisparsedGenericCall);
			if(IsMisparsedTemplateDeclaration(Src, Source, NameBindings)) Node.Facts.Add(ConstraintKind::MisparsedTemplateDeclaration);
			if(Syntax.Is(Id, Role::ProductAmbiguousPointerType) && ReadsAsCsProduct(Src, Source)) {
				Node.Facts.Add(ConstraintKind::MisreadProduct);
			}
			if(Syntax.Is(Id, Role::MacroGuardedParentheses)) {
				const NodeId Parent = Node.Parent, Inner = Syntax.NamedChild(Id, 0);
				// マクロ名の形の名前か
				const auto IsMacroStyle = [](const std::string_view Candidate) -> bool {
					// 大文字・下線・数字だけの名前かの返戻
					return !Candidate.empty() && std::all_of(
						Candidate.begin(),
						Candidate.end(),
						[](const char Char) -> bool {
							// マクロ名に使う文字かの返戻
							return Char >= 'A' && Char <= 'Z' || Char == '_' || Char >= '0' && Char <= '9';
						}
					);
				};
				const std::string_view Name =
				Syntax.Is(Inner, Role::Identifier) ? Src.View(SourceNode(Sources[Inner], ValidationNodes, ExtraNodes)) : std::string_view();
				const bool IsMacroStyleName = IsMacroStyle(Name);
				// 実引数の端に在る括弧の最も内の実引数の並び（端を離れれば無し）
				NodeId Arguments = NoNode;
				for(NodeId Host = Parent; Host != NoNode && Arguments == NoNode; Host = Syntax.Nodes[Host].Parent) {
					if(Syntax.Is(Host, Role::ArgumentSequence)) Arguments = Host;
					else if(Sources[Host].Start != Sources[Id].Start && Sources[Host].End != Sources[Id].End) break;
				}
				const NodeId Call = Arguments == NoNode ? NoNode : Syntax.Nodes[Arguments].Parent;
				const NodeId Callee = Syntax.Is(Call, Role::CallExpression) ? Syntax.NamedChild(Call, 0) : NoNode;
				const std::string_view CalleeName = Syntax.Is(Callee, Role::Identifier) ? Syntax.Nodes[Callee].Lexeme : std::string_view();
				// 宣言の無い大文字の名前か関数形式マクロの呼出の実引数の端の括弧は展開後の結合を保つ
				const bool IsMacroArgumentEdge = !CalleeName.empty() &&
				(IsMacroStyle(CalleeName) && !Src.IsDeclaredName(CalleeName, Sources[Callee].Start) || Src.IsFunctionMacro(CalleeName));
				if(
					IsMacroArgumentEdge || !Syntax.Is(Parent, Role::ReturningStatement) && (
						Src.MentionsExpressionMacro(Source) || IsMacroStyleName && (
							!Src.IsDeclaredName(Name, Src.Start(SourceNode(Sources[Inner], ValidationNodes, ExtraNodes))) ||
							Syntax.NamedChild(Parent, 0) == Id &&
							(Syntax.Is(Parent, Role::PostfixExpressionHost) || Syntax.Is(Parent, Role::PostfixOrUpdateExpression))
						)
					)
				) Node.Facts.Add(ConstraintKind::MacroSensitiveParen);
			}
			// 制御構文の中で誤り区画の後に続くか，埋込式を持つ文字列か
			bool IsAfterControlError = false;
			const bool IsInterpolated = std::any_of(
				Node.Children.begin(),
				Node.Children.end(),
				[&Syntax](const NodeId Child) -> bool {
					// 埋込式の子かの返戻
					return Syntax.Is(Child, Role::StringInterpolation);
				}
			);
			const bool IsLiteralContent = IsInterpolated || Syntax.Is(Id, Role::StringLikeAll) || Syntax.Is(Id, Role::InterpolationFormat);
			// 誤りの無い節点の頭と尾に残る文法の字句に無い字面（文字列の引用符等）は原文の間隙として保つ
			if(!Node.Children.empty() && !IsInErrorZone[Id] && !HasError[Id]) for(
				const auto [Edge, From, To] :
				{
					std::tuple(0U, Sources[Id].Start, Sources[Node.Children.front()].Start),
					std::tuple(static_cast<uint32_t>(Node.Children.size()), Sources[Node.Children.back()].End, Sources[Id].End)
				}
			) if(From < To && HasOpaqueHidden(Src, Source, From, To, Gaps)) {
				Node.Facts.Boundaries.Edit().push_back(
					{
						Edge,
						BoundaryKind::Preserved,
						OriginalGap(From, To),
						IsInMacroTokens[Id] ?
						PreservedSource::MacroTokens :
						IsLiteralContent ? PreservedSource::LiteralContent : PreservedSource::HiddenContent
					}
				);
			}
			// マクロの本体と行の前処理指令か
			const bool IsMacroBody = Src.Is(Source, Role::MacroBody), IsDirectiveLine = Syntax.Is(Id, Role::PreprocessorLine);
			for(uint32_t Index = 1; Index < Node.Children.size(); ++Index) {
				const NodeId Previous = Node.Children[Index - 1], Next = Node.Children[Index];
				// 指令の語の後・マクロの本体の前・本体の字句の間は空白１個，関数形式のマクロの名前と仮引数の並びは密着（字句間空白仕様 5.4）
				if(IsMacroBody || IsDirectiveLine) {
					if(IsDirectiveLine && Syntax.FieldChild(Id, ChildRole::Parameters) == Next) {
						Node.Facts.Boundaries.Edit().push_back({ Index, BoundaryKind::NoInlineSpace });
						continue;
					}
					// 指令の行に字句として残す行末コメント（解析用の印で覆った物を除く）の前置区切りは，原文のタブの数だけのタブか半角スペース１つ（整形仕様 7.1, 7.6）
					if(
						const std::string_view Raw = std::string_view(Src).substr(Sources[Next].Start, Sources[Next].End - Sources[Next].Start);
						(
							Raw.starts_with(CommentRepository::OpenerOf(CommentNotation::SlashLine)) ||
							Raw.starts_with(CommentRepository::OpenerOf(CommentNotation::Block))
						) && Src.ClassifyComment(Src.GetCommentSpans(), Sources[Next].Start, Sources[Next].End) == CommentKind::Trailing
					) {
						const size_t Tabs = TextEdit::SeparatorBefore(Src, Sources[Next].Start, Reading).second;
						Node.Facts.Boundaries.Edit().push_back(
							{ Index, BoundaryKind::Preserved, Tabs ? std::string(Tabs, '\t') : " ", PreservedSource::MacroTokens }
						);
						continue;
					}
					if(IsMacroBody || Index == 1 || Src.Is(SourceNode(Sources[Next], ValidationNodes, ExtraNodes), Role::MacroBody)) {
						Node.Facts.Boundaries.Edit().push_back({ Index, BoundaryKind::RequiredSpace });
						continue;
					}
				}
				const uint32_t GapStart = Sources[LastLeaf[Previous]].End, GapEnd = Sources[Next].Start;
				const std::string Gap =
				Src.GetSyntheticGroups().empty() ? Separated.SeparatedText(Src, GapStart, GapEnd) : OriginalGap(GapStart, GapEnd);
				IsAfterControlError = IsAfterControlError || Syntax.Is(Previous, Role::Error) && Syntax.Is(Id, Role::Control);
				if(Reading.HasKeywordClosedBlocks && (Syntax.Is(Previous, Role::CommentNode) || Syntax.Is(Next, Role::CommentNode))) {
					Node.Facts.Boundaries.Edit().push_back({ Index, BoundaryKind::Preserved, Gap, PreservedSource::LiteralContent });
					continue;
				}
				// 誤り区画・読めなかった指令の行の中と其れに接する境界，誤りを含む節点の読み飛ばした字句を挟む境界，Go の誤りを含む子の後の字句の前は原文の間隙を保つ
				if(
					IsInErrorZone[Id] || IsInErrorZone[Previous] || IsInBrokenDeclaration[Id] && !Syntax.Is(Id, Role::StatementHost) ||
					Syntax.Is(Previous, Role::Error) || Syntax.Is(Next, Role::Error) || IsAfterControlError ||
					HasError[Id] && OriginalGap(GapStart, GapEnd).find_first_not_of(" \t\r\n") != std::string::npos ||
					Rules.HasAutomaticLineTerminator && HasError[Previous] && !Syntax.Nodes[Next].Kind.IsNamed()
				) {
					Node.Facts.Boundaries.Edit().push_back(
						{
							Index,
							BoundaryKind::Preserved,
							OriginalGap(GapStart, GapEnd),
							IsInMacroTokens[Id] ? PreservedSource::MacroTokens : PreservedSource::ErrorBoundary
						}
					);
					continue;
				}
				// 埋込式を持つ文字列の本文の境界と C# の書式の後の境界（間隙が文字列の値），Rust のマクロの字句列の境界（本文の配置を保つ）と，文法の字句に無い字面を挟む境界は，コメントを除いた原文の間隙を保つ
				if(
					IsInterpolated || Syntax.Is(Previous, Role::InterpolationFormat) || IsInMacroTokens[Id] ||
					HasOpaqueHidden(Src, Source, GapStart, GapEnd, Gaps)
				) {
					std::string Preserved = CommentFreeGap(Src, Separated, GapStart, GapEnd, CommentRanges);
					if(Preserved == Gap) Preserved = OriginalGap(GapStart, GapEnd);
					Node.Facts.Boundaries.Edit().push_back(
						{
							Index,
							BoundaryKind::Preserved,
							std::move(Preserved),
							IsInMacroTokens[Id] ? PreservedSource::MacroTokens : IsLiteralContent || Syntax.Is(Previous, Role::InterpolationFormat) ?
							PreservedSource::LiteralContent :
							PreservedSource::HiddenContent
						}
					);
					continue;
				}
				if(
					Syntax.Is(Id, Role::NameParts) ||
					Syntax.Is(Id, Role::AnnotationParts) && !(Syntax.Is(Previous, Role::UserType) && Syntax.Is(Next, Role::UserType))
				) {
					// 名前と注釈の部品を同行で密着させる境界の記録
					Node.Facts.Boundaries.Edit().push_back({ Index, BoundaryKind::NoInlineSpace });
					continue;
				}
				if(
					Syntax.Is(Id, Role::MacroRuleDefinition) && (
						Syntax.Is(Previous, Role::MacroRulesToken) || Syntax.Nodes[Previous].Kind.IsNamed() && !Syntax.Nodes[Next].Kind.IsNamed() &&
						(Syntax.Is(Next, Role::OpenBrace) || Syntax.Is(Next, Role::OpeningParenToken) || Syntax.Is(Next, Role::OpeningBracketToken))
					)
				) {
					// 定義の語・名前・本体の開きの間に要る空白を投影する
					Node.Facts.Boundaries.Edit().push_back({ Index, BoundaryKind::RequiredSpace });
					continue;
				}
				if(Syntax.Is(Id, Role::MacroRuleDefinition)) {
					// 見出以外は同じ行に置く時だけ間に空白を置かず，規則の前後の改行を許す
					Node.Facts.Boundaries.Edit().push_back({ Index, BoundaryKind::NoInlineSpace });
					continue;
				}
				if(!KeepsAdjacency(Syntax, Id, Next)) continue;
				// 区切記法を挟む境界と，空白無で接する境界と，其の他の境界の記録
				const BoundaryKind Kind =
				SeparatedBefore[FirstLeaf[Next]] ? BoundaryKind::Separated : Gap.empty() ? BoundaryKind::Glued : BoundaryKind::Spaced;
				Node.Facts.Boundaries.Edit().push_back({ Index, Kind });
			}
			// 両端と内部の境界を子の番号順に揃え，組立時の探索を二分探索にする
			if(!Node.Facts.Boundaries.View().empty()) {
				std::vector<SemanticBoundary> &Boundaries = Node.Facts.Boundaries.Edit();
				std::sort(
					Boundaries.begin(),
					Boundaries.end(),
					[](const SemanticBoundary &Left, const SemanticBoundary &Right) -> bool {
						// 子の番号の大小の返戻
						return Left.Before < Right.Before;
					}
				);
			}
		}
		if(Reading.HasShortVariableDeclarations) RecordGoBindingFacts(Syntax);
		// 終了
		return;
	}

	/**
	 * Swift の後置の式の結付の補修関数（整形仕様 4.2, 4.3）
	 * tree-sitter-swift は加減・乗除・前置の式の後の呼出・添字・後置の演算子を其の式全体へ結び付ける（`a + g()` を `(a + g)()` と読む）為，後置の式を最も右の被演算子へ結び直す（字句の順は変えず，節点の親子と場だけを組み替える）
	 * 計算量：節点数 N と組替の回数 K に対し O(N + K)
	 * @param Syntax 構文事実を記録した構文モデル
	 */
	void SourceProjector::RebindSwiftPostfix(SyntaxModel &Syntax) {
		// 子を親より先に並べた節点の順（内側の組替の後に外側を組み替える）
		std::vector<NodeId> Order, Stack { Syntax.Root };
		while(!Stack.empty()) {
			const NodeId Id = Stack.back();
			Stack.pop_back();
			Order.push_back(Id);
			Stack.insert(Stack.end(), Syntax.Nodes[Id].Children.begin(), Syntax.Nodes[Id].Children.end());
		}
		// 後置の式の被演算子への結付
		for(std::vector<NodeId>::reverse_iterator Post = Order.rbegin(); Post != Order.rend(); ++Post) {
			while(Syntax.Is(*Post, Role::PostfixBindingExpression) && !Syntax.Nodes[*Post].Children.empty()) {
				const NodeId Left = Syntax.Nodes[*Post].Children.front();
				// 誤読の形でない後置の式の終了
				if(
					!Syntax.Is(Left, Role::PostfixBindingOperand) || Syntax.Nodes[Left].Children.size() < 2 ||
					Syntax.Nodes[*Post].Facts.Has(ConstraintKind::ContainsError)
				) break;
				const NodeId Right = Syntax.Nodes[Left].Children.back();
				SyntaxNode &Postfix = Syntax.Nodes[*Post], &Operand = Syntax.Nodes[Left], &Last = Syntax.Nodes[Right];
				const uint64_t PostfixFields = Postfix.Fields, OperandFields = Operand.Fields, LastFields = Last.Fields;
				const NodeId Parent = Postfix.Parent;
				const uint32_t Index = Postfix.Index, LastIndex = Last.Index;
				// 被演算子の式を後置の式の位置へ移す
				Operand.Parent = Parent;
				Operand.Index = Index;
				Operand.Fields = PostfixFields;
				if(Parent == NoNode) Syntax.Root = Left;
				else Syntax.Nodes[Parent].Children[Index] = Left;
				// 後置の式を最も右の被演算子の位置へ移す
				Operand.Children[LastIndex] = *Post;
				Postfix.Parent = Left;
				Postfix.Index = LastIndex;
				Postfix.Fields = LastFields;
				// 最も右の被演算子を後置の式の最初の子にする
				Postfix.Children.front() = Right;
				Last.Parent = *Post;
				Last.Index = 0;
				Last.Fields = OperandFields;
			}
		}
		// 終了
		return;
	}

	/**
	 * 空白を除きコメントを印とした字面の追記関数
	 * @param Src 解析済のソース
	 * @param Start 追記する範囲の始まり
	 * @param End 追記する範囲の終わり
	 * @param Comments コメントの範囲の位置順の並び (`CommentRanges`)
	 * @param Out 追記先
	 */
	void SourceProjector::AppendCode(
		const ::ParsedTree &Src,
		uint32_t Start,
		const uint32_t End,
		const std::vector<std::pair<uint32_t, uint32_t>> &Comments,
		std::string &Out
	) {
		// 範囲の始まり以後に終わる最初のコメント
		std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Comment = std::upper_bound(
			Comments.begin(),
			Comments.end(),
			Start,
			[](const uint32_t Pos, const std::pair<uint32_t, uint32_t> &Range) -> bool {
				// コメントの終わりとの比較結果の返戻
				return Pos < Range.second;
			}
		);
		// 解析用の区切りを除く字面の追記
		const std::vector<uint32_t> &Inserted = Src.GetInsertedBreaks();
		std::vector<uint32_t>::const_iterator Semicolon = std::lower_bound(Inserted.begin(), Inserted.end(), Start);
		while(Start < End) {
			while(Semicolon != Inserted.end() && *Semicolon < Start) ++Semicolon;
			if(Comment != Comments.end() && Comment->first <= Start) {
				// 並ぶコメントを１つの印に纏めた位置の追記（本文の正規化・句点での分割に依らず，部分整形の入力は NUL を含まない）
				if(Out.empty() || Out.back() != '\0') Out.push_back('\0');
				Start = Comment++->second;
				continue;
			}
			if(Semicolon != Inserted.end() && *Semicolon == Start) {
				++Start;
				++Semicolon;
				continue;
			}
			// 空白以外の字の追記
			if(const char Char = Src[Start++]; !std::isspace(static_cast<unsigned char>(Char))) Out.push_back(Char);
		}
		// 終了
		return;
	}

	/**
	 * 字面の出現数の一括計数関数
	 * 印（コメントの位置）は整形が差し込む物（文書化コメントの雛形等）も有る為，印を除いた字で数える
	 * 計算量：字面の全体の長さ N と数える字面の長さの和 L に対し O(N + L)（遷移の表の引きを O(1) と看做す）
	 * @param Code 空白を除きコメントを印とした字面の全体
	 * @param Texts 数える字面の並び
	 * @return 字面毎の出現数（重なる出現も数え，印だけの字面は０）
	 */
	std::vector<uint32_t> SourceProjector::CountZones(const std::string_view Code, const std::vector<std::string_view> &Texts) {
		// 全ての字面を１回の走査で数える Aho–Corasick 法の，状態と字を鍵にした遷移，状態毎の子，失敗の遷移，字面の終わりの状態
		std::unordered_map<uint64_t, uint32_t> Goto;
		std::vector<std::vector<std::pair<unsigned char, uint32_t>>> Children(1);
		std::vector<uint32_t> Fail(1, 0), Terminal;
		Terminal.reserve(Texts.size());
		const auto Edge = [&Goto](const uint32_t State, const unsigned char Char) -> uint32_t {
			const std::unordered_map<uint64_t, uint32_t>::const_iterator Iter = Goto.find(static_cast<uint64_t>(State) << 8 | Char);
			// 遷移先の返戻（無ければ根の０）
			return Iter == Goto.end() ? 0 : Iter->second;
		};
		// 字面の木の構築
		for(const std::string_view Text : Texts) {
			uint32_t State = 0;
			for(const char Char : Text) {
				// 印の読み飛ばし
				if(!Char) continue;
				const unsigned char Byte = static_cast<unsigned char>(Char);
				const auto [Iter, IsNew] = Goto.try_emplace(static_cast<uint64_t>(State) << 8 | Byte, static_cast<uint32_t>(Fail.size()));
				if(IsNew) {
					Children[State].push_back({ Byte, Iter->second });
					Children.emplace_back();
					Fail.push_back(0);
				}
				State = Iter->second;
			}
			Terminal.push_back(State);
		}
		// 幅優先の順と失敗の遷移の算出
		std::vector<uint32_t> Order;
		Order.reserve(Fail.size());
		for(const auto &[Byte, Child] : Children[0]) Order.push_back(Child);
		for(size_t Head = 0; Head < Order.size(); ++Head) {
			const uint32_t State = Order[Head];
			for(const auto &[Byte, Child] : Children[State]) {
				uint32_t Back = Fail[State];
				// 同じ字で進める最長の接尾辞の探索
				while(Back && !Edge(Back, Byte)) Back = Fail[Back];
				Fail[Child] = Edge(Back, Byte);
				Order.push_back(Child);
			}
		}
		// 字面全体の走査と状態毎の到達数
		std::vector<uint32_t> Hits(Fail.size(), 0);
		uint32_t State = 0;
		for(const char Char : Code) {
			// 印の読み飛ばし
			if(!Char) continue;
			const unsigned char Byte = static_cast<unsigned char>(Char);
			while(State && !Edge(State, Byte)) State = Fail[State];
			State = Edge(State, Byte);
			++Hits[State];
		}
		// 失敗の遷移の先（接尾辞の字面）への到達数の累積
		for(size_t Idx = Order.size(); Idx--;) Hits[Fail[Order[Idx]]] += Hits[Order[Idx]];
		// 状態毎の到達数から字面毎の出現数への変換
		std::vector<uint32_t> Counts;
		Counts.reserve(Terminal.size());
		for(const uint32_t End : Terminal) Counts.push_back(End ? Hits[End] : 0);
		// 字面毎の出現数の返戻
		return Counts;
	}

	/**
	 * 位置を観測する構文の位置の収集関数（整形仕様 4.4）
	 * 位置を実行時の値として読む構文を文法の定義から拾い，通常の診断座標と区別する（呼出側の位置を観測する仮引数の属性は，其の関数の呼出の位置を拾う）
	 * 字面が似て居るだけの文字列・コメントは拾わない（コメントは投影で本文から除かれて居る）
	 * 計算量：節点数 V と観測する構文の定義の数 D に対し O(V×D)
	 * @param Src 解析済のソース
	 * @param Profile 対象の文法
	 * @param BeforeEdits 補修前の本文（補修後の木を読む時だけ指定する）
	 * @param Nodes 投影の節点の走査で記録した原文順の節点
	 * @return 観測する構文の行と桁（桁を観測しない物は０）の原文順の並び
	 */
	std::vector<std::pair<uint32_t, uint32_t>> SourceProjector::CollectObservedPositions(
		const ::ParsedTree &Src,
		const GrammarProfile &Profile,
		const std::string_view BeforeEdits,
		const std::span<const TSNode> Nodes
	) {
		std::vector<std::pair<uint32_t, uint32_t>> Positions;
		const std::span<const SourceObservation> Observations = Profile.GetObservations();
		// 観測する構文を持たない言語の返戻
		if(!Src.IsParsed() || Observations.empty()) return Positions;
		// 補修前の行頭位置の収集
		std::vector<uint32_t> LineStarts;
		if(!BeforeEdits.empty()) {
			LineStarts.push_back(0);
			for(size_t Pos = BeforeEdits.find('\n'); Pos != std::string_view::npos; Pos = BeforeEdits.find('\n', Pos + 1)) {
				LineStarts.push_back(static_cast<uint32_t>(Pos + 1));
			}
		}
		const auto PositionOf = [&Src, &LineStarts](const TSNode Node) -> TSPoint {
			// 補修前の本文を持たない木の位置の返戻
			if(LineStarts.empty()) return ts_node_start_point(Node);
			const uint32_t Offset = Src.OffsetBeforeEdits(ts_node_start_byte(Node));
			const uint32_t Row =
			static_cast<uint32_t>(std::upper_bound(LineStarts.begin(), LineStarts.end(), Offset) - LineStarts.begin() - 1);
			// 補修前の位置の返戻
			return { Row, Offset - LineStarts[Row] };
		};
		// 節点の名前の部分の綴り（最初の名前付の子，無ければ節点の字面）
		const auto NameOf = [&Src](const TSNode Node) -> std::string_view {
			const TSNode Named = ts_node_named_child(Node, 0);
			// 名前の綴りの返戻
			return Src.View(ts_node_is_null(Named) ? Node : Named);
		};
		// 綴りの末尾が区切りの位置で名前と一致するか
		const auto EndsWithName = [](const std::string_view Text, const std::string_view Name, const bool IsCaseInsensitive) -> bool {
			// 名前より短い綴りの返戻
			if(Text.size() < Name.size()) return false;
			const std::string_view Tail = Text.substr(Text.size() - Name.size());
			const size_t Head = Text.size() - Name.size();
			// 区切りの位置での一致の返戻
			return (!Head || Text[Head - 1] == '.' || Text[Head - 1] == ':') && std::equal(
				Tail.begin(),
				Tail.end(),
				Name.begin(),
				Name.end(),
				[IsCaseInsensitive](const char Left, const char Right) -> bool {
					// 大小を揃えた字の一致の返戻
					return IsCaseInsensitive ?
					std::tolower(static_cast<unsigned char>(Left)) == std::tolower(static_cast<unsigned char>(Right)) :
					Left == Right;
				}
			);
		};
		// 呼出側の位置を観測する関数の名前と，其の候補の呼出
		std::vector<std::string_view> ObservedCallees;
		std::vector<TSNode> Calls;
		uint32_t LiteralEnd = 0;
		// 投影中に記録した原文順の節点から観測する構文を拾う
		for(const TSNode Node : Nodes) {
			if(Src.Start(Node) < LiteralEnd) continue;
			if(Src.Is(Node, Role::LiteralInterior)) {
				LiteralEnd = Src.End(Node);
				continue;
			}
			const TSSymbol Symbol = ts_node_symbol(Node);
			std::optional<std::string_view> Name;
			for(const SourceObservation &Observation : Observations) {
				if(Observation.CallerSymbol && *Observation.CallerSymbol == Symbol) Calls.push_back(Node);
				if(Observation.Symbol != Symbol) continue;
				if(!Name) Name = NameOf(Node);
				if(!EndsWithName(*Name, Observation.Name, Observation.IsCaseInsensitive)) continue;
				const TSPoint Start = PositionOf(Node);
				if(!Observation.CallerSymbol) {
					Positions.emplace_back(Start.row, Observation.ObservesColumn ? Start.column + 1 : 0);
					continue;
				}
				// 仮引数の属性を持つ関数（仮引数の並びを持つ最も内の祖先）の名前
				TSNode Function = ts_node_parent(Node);
				while(!ts_node_is_null(Function) && ts_node_is_null(Src.FieldChild(Function, ChildRole::ParametersField))) {
					Function = ts_node_parent(Function);
				}
				if(!ts_node_is_null(Function)) ObservedCallees.push_back(Src.View(Src.FieldChild(Function, ChildRole::Name)));
			}
		}
		// 観測する関数の呼出の行（型引数を除いた被呼出側の名前で照合する）
		for(const TSNode Call : Calls) {
			std::string_view Callee = Src.View(Src.FieldChild(Call, ChildRole::FunctionField));
			Callee = Callee.substr(0, Callee.find('<'));
			if(
				std::any_of(
					ObservedCallees.begin(),
					ObservedCallees.end(),
					[&](const std::string_view Name) -> bool {
						// 名前の一致の返戻
						return !Name.empty() && EndsWithName(Callee, Name, false);
					}
				)
			) Positions.emplace_back(PositionOf(Call).row, 0);
		}
		// 観測位置の原文順への整列
		std::sort(Positions.begin(), Positions.end());
		// 観測する構文の位置の返戻
		return Positions;
	}

	/**
	 * 親の表から節点の親を取得する関数
	 * 計算量：根と表に有る節点には O(1)，其れ以外には木の幅 C に対し O(C)
	 * @param Parents 一度の木の走査で作った親の表
	 * @param Node 対象の節点
	 * @param Root 木の根
	 * @return 親の節点（根には空の節点）
	 */
	static TSNode CachedParent(const std::unordered_map<const void *, TSNode> &Parents, const TSNode Node, const TSNode Root) {
		// 根の親の無い事の返戻
		if(ts_node_eq(Node, Root)) return TSNode{};
		const std::unordered_map<const void *, TSNode>::const_iterator Found = Parents.find(Node.id);
		// 親の節点の返戻
		return Found == Parents.end() ? ts_node_parent(Node) : Found->second;
	}

	/**
	 * 構文誤りの周りの字面の収集関数
	 * 部分整形は解析器が読めなかった範囲 (`ERROR`) の周りを誤読した木で編集し得る（区切りの補完・隣の括弧の除去・コメントの差込）
	 * 其の範囲と前後に接する字（空白を除き，コメントは位置の印とする）を集め，整形後も同じ並びで残るかを確かめる (`CountLostErrorZones`)
	 * 計算量：入力の長さ N，節点の数 V，誤りの祖先の長さの和 A に対し O(N log N + V + A)
	 * @param Src 解析済の入力（コメントを含む）
	 * @param Nodes 投影の節点の走査で記録した原文順の節点
	 * @param Parents 木を一度辿って集めた節点毎の親
	 * @return 構文誤り毎の空白を除きコメントを印とした字面と入力の全体での其の出現数の位置順の並び（重なる範囲は纏める）
	 */
	std::vector<ValidationContext::ErrorZone> SourceProjector::CollectErrorZones(
		const ::ParsedTree &Src,
		const std::span<const TSNode> Nodes,
		const std::unordered_map<const void *, TSNode> &Parents
	) {
		std::vector<ValidationContext::ErrorZone> Zones;
		// 解析木の無い入力の返戻
		if(!Src.IsParsed()) return Zones;
		const TSNode Root = Src.GetRoot();
		// 構文誤りの無い入力の返戻
		if(!ts_node_has_error(Root)) return Zones;
		// 構文誤りの照合に使うコメント範囲
		std::vector<std::pair<uint32_t, uint32_t>> Comments;
		for(const TSNode Node : Nodes) {
			if(!ts_node_eq(Node, Root) && Src.Is(Node, Role::Comment)) Comments.push_back({ Src.Start(Node), Src.End(Node) });
		}
		std::sort(Comments.begin(), Comments.end());
		Comments.erase(std::unique(Comments.begin(), Comments.end()), Comments.end());
		const uint32_t Size = static_cast<uint32_t>(Src.CodeText().size());
		// 位置を含むコメントの範囲（無ければ末尾）
		const auto CommentAt = [&Comments](const uint32_t Pos) -> std::vector<std::pair<uint32_t, uint32_t>>::const_iterator {
			const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Hit = std::upper_bound(
				Comments.begin(),
				Comments.end(),
				Pos,
				[](const uint32_t Target, const std::pair<uint32_t, uint32_t> &Range) -> bool {
					// コメントの終わりとの比較結果の返戻
					return Target < Range.second;
				}
			);
			// 位置を含む範囲の返戻
			return Hit != Comments.end() && Hit->first <= Pos ? Hit : Comments.end();
		};
		std::vector<std::pair<uint32_t, uint32_t>> Ranges;
		// 最も外側の構文誤りの範囲の記録（構文誤りの内側へは降りない）
		const auto AddZone = [&](const TSNode Node, const std::span<const TSNode> Ancestors) -> void {
			if(SyntaxParser::IsBrokenNode(Src, Node, Ancestors)) {
				// 前に接する字の位置（空白とコメントを越える）
				uint32_t Before = Src.Start(Node);
				while(Before) {
					if(std::isspace(static_cast<unsigned char>(Src[Before - 1]))) --Before;
					else if(
						const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Comment = CommentAt(Before - 1);
						Comment != Comments.end()
					) Before = Comment->first;
					else {
						--Before;
						break;
					}
				}
				// 後に接する字の終わり（空白とコメントを越える）
				uint32_t After = Src.End(Node);
				while(After < Size) {
					if(std::isspace(static_cast<unsigned char>(Src[After]))) ++After;
					else if(
						const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Comment = CommentAt(After);
						Comment != Comments.end()
					) After = Comment->second;
					else {
						++After;
						break;
					}
				}
				// 重なる範囲の纏め
				if(!Ranges.empty() && Before <= Ranges.back().second) Ranges.back().second = std::max(Ranges.back().second, After);
				else Ranges.push_back({ Before, After });
			}
			// 終了
			return;
		};
		// 最も外側の構文誤りの収集
		TSNode SkippedError {};
		for(const TSNode Node : Nodes) {
			if(!Src.Is(Node, Role::Error)) continue;
			bool IsInsideError = false;
			std::vector<TSNode> Ancestors;
			for(TSNode Parent = CachedParent(Parents, Node, Root); !ts_node_is_null(Parent); Parent = CachedParent(Parents, Parent, Root)) {
				if(ts_node_eq(Parent, SkippedError)) {
					IsInsideError = true;
					break;
				}
				Ancestors.push_back(Parent);
			}
			if(IsInsideError) continue;
			std::reverse(Ancestors.begin(), Ancestors.end());
			AddZone(Node, Ancestors);
			SkippedError = Node;
		}
		// 出現数を数える入力の全体の字面（JSX の本文の端へ置いた私用領域の文字の印は完成候補の字面と同じ空白へ戻す）
		const bool IsJsxMarked = !Src.GetJsxBlankMark(' ').starts_with('&');
		std::string Code;
		AppendCode(Src, 0, Size, Comments, Code);
		if(IsJsxMarked) Code = ParseAdapter::DecodeJsxBlanks(Src, std::move(Code));
		// 範囲毎の字面の抽出（照合する字の無い範囲は除く）
		for(const auto &[Start, End] : Ranges) {
			std::string Text;
			AppendCode(Src, Start, End, Comments, Text);
			if(IsJsxMarked) Text = ParseAdapter::DecodeJsxBlanks(Src, std::move(Text));
			if(!Text.empty()) Zones.push_back({ std::move(Text), 0 });
		}
		// 入力の全体での出現数の一括計数
		std::vector<std::string_view> Texts;
		Texts.reserve(Zones.size());
		for(const ValidationContext::ErrorZone &Zone : Zones) Texts.push_back(Zone.Text);
		const std::vector<uint32_t> Counts = CountZones(Code, Texts);
		for(size_t Idx = 0; Idx < Zones.size(); ++Idx) Zones[Idx].Count = Counts[Idx];
		// 字面の並びの返戻
		return Zones;
	}

	/**
	 * 意味上の区別の収集関数（整形仕様 3.2, 8.1）
	 * 関数・ラムダの種類（束縛・構築呼出の可否が異なる関数式とアロー関数等）と，投影が組み替える前の解析器の結付から言語の規則で定まる結付を集める
	 * 結付は，後置の式を最も右の被演算子へ結ぶ規則で定まる前置・二項の演算子の右の被演算子の範囲の名前の字句の数と，閉じタグが文を終えて空に為る制御構文の本体で，組替の誤りを完成候補の解析木と照合する（整形で変わる括弧・字句の数に依らない量で表す）
	 * 誤りを含む構文は誤り区画の照合に委ねて除く
	 * 計算量：節点数 V と被演算子の連鎖の長さ L に対し O(V (L + log V))
	 * @param Src 解析済の原稿
	 * @param Nodes 投影の節点の走査で記録した原文順の節点
	 * @return 関数の種類（始まりの順），演算子の右の被演算子（演算子の順），制御構文の本体（始まりの順）を続けた並び
	 */
	std::vector<ValidationContext::SemanticCheck> SourceProjector::CollectSemanticChecks(
		const ::ParsedTree &Src,
		const std::span<const TSNode> Nodes
	) {
		std::vector<ValidationContext::SemanticCheck> Checks;
		// 解析して居ない原稿の空の並びの返戻
		if(!Src.IsParsed()) return Checks;
		const ReadingRules &Reading = Src.GetReading();
		// 名前の字句の始まり（原文順）
		std::vector<uint32_t> Names;
		for(const TSNode Node : Nodes) if(Src.Is(Node, Role::NameToken)) Names.push_back(Src.Start(Node));
		std::sort(Names.begin(), Names.end());
		// コメントを除く最後の子
		const auto LastCodeChild = [&Src](const TSNode Node) -> TSNode {
			for(uint32_t Index = ts_node_child_count(Node); Index--;) {
				// コメントでない子の返戻
				if(const TSNode Child = ts_node_child(Node, Index); !Src.Is(Child, Role::Comment)) return Child;
			}
			// 子の無い事の返戻
			return TSNode{};
		};
		// 範囲の名前の字句の数
		const auto CountNames = [&Names](const uint32_t Start, const uint32_t End) -> uint32_t {
			const std::vector<uint32_t>::const_iterator From = std::lower_bound(Names.begin(), Names.end(), Start);
			// 範囲の始まりから終わりの前迄の数の返戻
			return static_cast<uint32_t>(std::lower_bound(From, Names.cend(), End) - From);
		};
		// 後置の式を結び直す前置・二項の式か
		const auto IsOperation = [&Src](const TSNode Node) -> bool {
			// 被演算子を持つ前置・二項の式かの返戻
			return Src.Is(Node, Role::PostfixBindingOperand) && ts_node_child_count(Node) > 1;
		};
		// 前置・二項の式毎の右の被演算子の意味上の終わり（後置の式が最も右の被演算子へ結び付くと其の後置の式の終わり迄）
		std::unordered_map<const void *, uint32_t> OperandEnds;
		// 後置の式を結び直した後に其の位置を占める前置・二項の式（内側の後置の式から順に定まる）
		std::unordered_map<const void *, TSNode> Replacing;
		if(Reading.MisbindsPostfixOperand) for(const TSNode Node : std::views::reverse(Nodes)) {
			if(ts_node_has_error(Node) || !Src.Is(Node, Role::PostfixBindingExpression)) continue;
			TSNode Operand = ts_node_child(Node, 0);
			if(
				const std::unordered_map<const void *, TSNode>::const_iterator Found = Replacing.find(Operand.id);
				Found != Replacing.end()
			) Operand = Found->second;
			if(!IsOperation(Operand)) continue;
			Replacing.emplace(Node.id, Operand);
			for(TSNode Operation = Operand; IsOperation(Operation); Operation = LastCodeChild(Operation)) {
				uint32_t &End = OperandEnds.try_emplace(Operation.id, Src.End(Operation)).first->second;
				End = std::max(End, Src.End(Node));
			}
		}
		// 種類毎の区別と其の並べ順の鍵
		std::vector<std::pair<uint32_t, ValidationContext::SemanticCheck>> Functions, Operations, Bodies;
		for(const TSNode Node : Nodes) {
			if(ts_node_has_error(Node)) continue;
			if(Src.Is(Node, Role::FunctionScope)) Functions.push_back({ Src.Start(Node), { ts_node_symbol(Node), 0 } });
			if(Reading.MisbindsPostfixOperand && IsOperation(Node)) {
				// 演算子の字句（最初の名前の無い子）の位置と，右の被演算子の意味上の範囲の名前の字句の数
				uint32_t Operator = Src.Start(Node);
				for(uint32_t Index = 0; Index < ts_node_child_count(Node); ++Index) {
					const TSNode Child = ts_node_child(Node, Index);
					if(ts_node_is_named(Child)) continue;
					Operator = Src.Start(Child);
					break;
				}
				const std::unordered_map<const void *, uint32_t>::const_iterator Found = OperandEnds.find(Node.id);
				Operations.push_back(
					{ Operator, { 0, CountNames(Src.Start(LastCodeChild(Node)), Found == OperandEnds.end() ? Src.End(Node) : Found->second) } }
				);
			}
			if(Reading.MisbindsPostfixOperand && Src.Is(Node, Role::OperatorCall) && ts_node_child_count(Node) == 2) {
				// 前置の演算子の後の括弧を演算子の呼出と読んだ式（中置の式の末尾の字句の演算子を含む）は，括弧の中を右の被演算子とする前置の演算
				TSNode Prefix = ts_node_child(Node, 0);
				if(IsOperation(Prefix)) Prefix = LastCodeChild(Prefix);
				if(
					const TSNode Suffix = ts_node_child(Node, 1);
					(!ts_node_is_named(Prefix) || Src.Is(Prefix, Role::ExclamationMark)) && Src.Is(Suffix, Role::CallSuffix) &&
					ts_node_named_child_count(Suffix) == 1
				) Operations.push_back({ Src.Start(Prefix), { 0, CountNames(Src.Start(Suffix), Src.End(Suffix)) } });
			}
			if(
				Reading.MisbindsEmbeddedBranches &&
				(Src.Is(Node, Role::BodyOwner) || Src.Is(Node, Role::If) || Src.Is(Node, Role::ElseIfClause))
			) {
				const TSNode Body = Src.FieldChild(Node, ChildRole::Body);
				if(ts_node_is_null(Body)) continue;
				// 空の文と文を持たない塊の本体と，本体より前の閉じタグが文を終える（コロンで本体を開く代替の構文を除く）本体は空
				bool IsEmpty = Src.Is(Body, Role::EmptyStatement) || Src.Is(Body, Role::CompoundBody) && !HasChildOf(
					Body,
					[&Src](const TSNode Child) -> bool {
						// コメントでない名前付の子かの返戻
						return ts_node_is_named(Child) && !Src.Is(Child, Role::Comment);
					}
				);
				for(uint32_t Index = 0; Index < ts_node_child_count(Node) && !ts_node_eq(ts_node_child(Node, Index), Body); ++Index) {
					if(Src.Is(ts_node_child(Node, Index), Role::ColonToken)) break;
					IsEmpty = IsEmpty || Src.Is(ts_node_child(Node, Index), Role::CodeBoundary);
				}
				Bodies.push_back({ Src.Start(Node), { 0, IsEmpty } });
			}
		}
		// 種類毎の原文順による照合資料の連結
		for(std::vector<std::pair<uint32_t, ValidationContext::SemanticCheck>> *const Kind : { &Functions, &Operations, &Bodies }) {
			std::stable_sort(
				Kind->begin(),
				Kind->end(),
				[](
					const std::pair<uint32_t, ValidationContext::SemanticCheck> &Lhs,
					const std::pair<uint32_t, ValidationContext::SemanticCheck> &Rhs
				) -> bool {
					// 並べ順の鍵の比較の返戻
					return Lhs.first < Rhs.first;
				}
			);
			for(const std::pair<uint32_t, ValidationContext::SemanticCheck> &Entry : *Kind) Checks.push_back(Entry.second);
		}
		// 意味上の区別の並びの返戻
		return Checks;
	}

	/**
	 * 投影の節点からの検証資料の組立関数（整形仕様 3.4, 4.3）
	 * 計算量：親の表と祖先経路の収集は木の節点数 V に対し O(V)（他の資料の組立を除く）
	 * @param Src 受理した解析結果
	 * @param Profile 入力時に確定した文法
	 * @param Input 適応済の入力と補修前の字面
	 * @param Nodes 投影中に記録した節点の原文順の並び
	 * @return 最終検証が照合する資料
	 */
	ValidationContext SourceProjector::CollectContext(
		const ::ParsedTree &Src,
		const GrammarProfile &Profile,
		const ParseInput &Input,
		const std::span<const TSNode> Nodes
	) {
		// 診断に使う親表とコメント範囲の収集
		ValidationContext Context;
		const TSNode Root = Src.GetRoot();
		const bool HasTreeError = ts_node_has_error(Root);
		std::unordered_map<const void *, TSNode> Parents;
		if(HasTreeError) {
			WalkChildrenCursor(
				Root,
				[&Parents](const TSNode Node, const TSNode Parent) -> bool {
					Parents.emplace(Node.id, Parent);
					// 子の走査を続ける事の返戻
					return true;
				}
			);
		}
		std::vector<TSNode> Path;
		for(const TSNode Node : Nodes) {
			if(!ts_node_eq(Node, Root) && Src.Is(Node, Role::Comment)) {
				Context.InputDiagnostics.CommentRanges.push_back({ Src.Start(Node), Src.End(Node) });
			}
			if(!HasTreeError) continue;
			const TSNode Parent = CachedParent(Parents, Node, Root);
			while(!Path.empty() && !ts_node_eq(Path.back(), Parent)) Path.pop_back();
			if(!ts_node_is_null(Parent) && Path.empty()) {
				std::vector<TSNode> Missing;
				for(TSNode Up = Parent; !ts_node_is_null(Up); Up = CachedParent(Parents, Up, Root)) Missing.push_back(Up);
				Path.assign(Missing.rbegin(), Missing.rend());
			}
			if(SyntaxParser::IsBrokenNode(Src, Node, std::span<const TSNode>(Path))) ++Context.InputDiagnostics.BrokenCount;
			Path.push_back(Node);
		}
		std::sort(Context.InputDiagnostics.CommentRanges.begin(), Context.InputDiagnostics.CommentRanges.end());
		Context.InputDiagnostics.CommentRanges.erase(
			std::unique(Context.InputDiagnostics.CommentRanges.begin(), Context.InputDiagnostics.CommentRanges.end()),
			Context.InputDiagnostics.CommentRanges.end()
		);
		Context.InputDiagnostics.HasBrokenNode = Context.InputDiagnostics.BrokenCount;
		if(
			const bool IsVerification = std::holds_alternative<ParseInput::VerificationSource>(Input.Source);
			Src.ContainsInputError() || IsVerification && HasTreeError
		) {
			// 入力の破損又は完成候補の誤りが在る時に区画を記録
			Context.ErrorZones = CollectErrorZones(Src, Nodes, Parents);
			Context.InputDiagnostics.InputBrokenCount = std::min(Input.PreparedBroken, Context.InputDiagnostics.BrokenCount);
		}
		// 意味上の区別と保護する位置の収集
		Context.SemanticChecks.Checks = CollectSemanticChecks(Src, Nodes);
		const ParseInput::VerificationSource *const Verification = std::get_if<ParseInput::VerificationSource>(&Input.Source);
		const std::string_view ObservationText =
		Verification && !Verification->BeforeEdits.empty() ? Verification->BeforeEdits : Input.SourceText();
		Context.ProtectedRegions.ObservedPositions = CollectObservedPositions(Src, Profile, ObservationText, Nodes);
		Context.ProtectedRegions.InputText = Input.SourceText();
		// 解析の為に置いた印（私用領域の文字を選べず文字参照を使った JSX の印は出力の字面として正しい為に除く）
		if(const char Marker = Src.GetAnalysisMark()) Context.ProtectedRegions.AnalysisMarks.emplace_back(1, Marker);
		for(const char Blank : { ' ', '\t' }) if(const std::string_view Mark = Src.GetJsxBlankMark(Blank); !Mark.starts_with('&')) {
			Context.ProtectedRegions.AnalysisMarks.emplace_back(Mark.substr(0, 3));
		}
		// 検証資料の返戻
		return Context;
	}

	/**
	 * コメントを分離した解析結果の節点表への投影関数（整形仕様 3.4, 4.3）
	 * コメントを錨へ紐付けた構文木を，独自の節点表と錨の節点毎のコメントへ写す（錨へ紐付けたコメントの節点は写さない）
	 * 解析器が補った字句 (`MISSING`) は原文に無い為に節点を作らず，文法が子に出さない隠れた字句は無名の字句の節点にする
	 * 計算量：節点数 V とコメントの数 K に対し O(V + K)
	 * @param Src 解析結果（書き換えない）
	 * @param Profile 入力で確定した文法
	 * @param Input 適応済の入力と補修前の字面
	 * @param Separated 投影の間だけ持つコメントの分離の結果（検証の投影では行末コメントの前置区切りを結果へ移す）
	 * @return 節点表・コメント・検証資料
	 * @throws FormatLimitExceeded 節点を作らない字句や字面を保つ区画の中にコメントが紐付いて居る場合（投影の不具合）
	 */
	ProjectionResult SourceProjector::ProjectNodes(
		const ::ParsedTree &Src,
		const GrammarProfile &Profile,
		const ParseInput &Input,
		CommentSeparation &Separated
	) {
		// 投影先の節点表と検証資料の準備
		ProjectionResult Result { SyntaxModel(Profile.GetRules(), Profile.GetChildRoleMask()), CommentRepository(), {} };
		SyntaxModel &Syntax = Result.Syntax;
		// 投影する節点と其の親・子の役割
		struct Frame {
			TSNode Node; // 解析器の節点（隠れた字句は空）
			NodeId Parent; // 投影済の親
			uint64_t Fields; // 親の中の子の役割の集合
			std::pair<uint32_t, uint32_t> Hidden {}; // 隠れた字句の原文の範囲
			TSNode ParentNode {}; // 解析器の親の節点（深さに比例する親の探索を避ける）
			std::vector<TSNode> Groups {}; // 投影せずに内側の子へ紐付を渡す解析用の括弧の包装
			bool ValidationOnly = false; // 投影しない子の検証資料だけを採るか
		};
		// 節点の子孫に紐付いたコメントの有無（節点を作らない部分木へ紐付いた物は置き場を失う）
		const auto HasAttached = [&Separated](const TSNode Node) -> bool {
			// 部分木の範囲に紐付の有るかの返戻
			return Separated.HasAttachedCommentInRange(ts_node_start_byte(Node), ts_node_end_byte(Node) + 1) && (
				!Separated.GetLeading(Node).empty() || !Separated.GetTrailing(Node).empty() || HasDescendantOf(
					Node,
					[&Separated](const TSNode Sub) -> bool {
						// 子孫の紐付の有無の返戻
						return !Separated.GetLeading(Sub).empty() || !Separated.GetTrailing(Sub).empty();
					}
				)
			);
		};
		// 親の中で既に投影した最後の節点（親に無ければ祖先の中で親より前の節点，除く節点は数えない）
		const auto LastProjected = [&Result](NodeId Parent, const NodeId Excluded) -> NodeId {
			for(NodeId Up = Parent; Up != NoNode; Up = Result.Syntax.Nodes[Up].Parent) {
				const std::vector<NodeId> &Siblings = Result.Syntax.Nodes[Up].Children;
				for(std::vector<NodeId>::const_reverse_iterator Found = Siblings.rbegin(); Found != Siblings.rend(); ++Found) {
					// 除く節点でも其の祖先でもない最後の子の返戻
					if(*Found != Excluded && *Found != Parent) return *Found;
				}
				Parent = Up;
			}
			// 前に投影した節点の無い事の返戻
			return NoNode;
		};
		// 節点を作らない又は出力の位置を持たない部分木へ紐付いたコメントの移譲
		// 空の本体の中の独立した物は本体の所有者の空の本体の物とし，其の他は既に投影した直前の節点の後（無ければ親）の物とする
		const auto MoveAttached =
		[&Separated, &Result](const TSNode Node, const NodeId Parent, const NodeId Owner, const NodeId Previous, const bool IsSubtree)
		-> void {
			const auto Transfer = [&](const TSNode Sub) -> void {
				for(const std::vector<CommentRecord> *const Attached : { &Separated.GetLeading(Sub), &Separated.GetTrailing(Sub) }) {
					for(CommentRecord Record : *Attached) {
						if(Owner != NoNode && Record.Kind == CommentKind::OwnLine) {
							Record.Anchor = Owner;
							Record.Placement.Side = CommentPlacement::Before;
							Record.Placement.IsEmptyBody = true;
						} else if(Previous != NoNode) {
							Record.Anchor = Previous;
							Record.Placement.Side = CommentPlacement::After;
							Record.Placement.TokensBefore = 0;
						} else Record.Anchor = Parent;
						Result.Comments.Add(std::move(Record));
					}
				}
			};
			Transfer(Node);
			if(IsSubtree) {
				WalkChildrenCursor(
					Node,
					[&Transfer](const TSNode Sub) -> bool {
						Transfer(Sub);
						// 子孫の走査を続ける事の返戻
						return true;
					}
				);
			}
			// 終了
			return;
		};
		// PHP の本体の範囲（其の外の地の文は走査の後に逐語の子として加え，閉じタグの後の子とコメントは投影しない，整形仕様 4.4）
		const auto [BodyStart, BodyEnd] = ParseAdapter::OuterTextBounds(Src);
		// 字面を保つ区画に残るコメントの範囲（原文順）と解析用の括弧の位置（昇順）
		GapExclusions Gaps { Src.CollectCommentRanges(BodyEnd), {} };
		Gaps.SyntheticPositions.reserve(Src.GetSyntheticGroups().size() << 1);
		for(const auto &[Open, Close] : Src.GetSyntheticGroups()) {
			Gaps.SyntheticPositions.push_back(Open);
			Gaps.SyntheticPositions.push_back(Close);
		}
		std::sort(Gaps.SyntheticPositions.begin(), Gaps.SyntheticPositions.end());
		const std::vector<std::pair<uint32_t, uint32_t>> &CommentRanges = Gaps.CommentRanges;
		// 字面を保つ区画の字面の中のコメントと分離の跡へ置いた区切記法の範囲の記録（字面の変換はコメントの字面と順を保つ）
		const auto RecordKept = [&Src, &Separated, &CommentRanges, &Result](const TSNode Node, const NodeId Id) -> void {
			const uint32_t Start = ts_node_start_byte(Node), End = ts_node_end_byte(Node);
			const std::string &Lexeme = Result.Syntax.Nodes[Id].Lexeme;
			std::vector<std::pair<uint32_t, uint32_t>> Kept;
			// 分離の跡へ置いた区切記法（原文の字面の儘の区画だけが持つ）
			if(Lexeme == Separated.SeparatedText(Src, Start, End, &Kept)) {
				for(const auto &[Begin, Finish] : Kept) Result.Validation.ProtectedRegions.KeptComments.push_back({ Id, Begin, Finish });
			}
			size_t Cursor = 0;
			for(
				std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Range =
				std::lower_bound(CommentRanges.begin(), CommentRanges.end(), std::pair<uint32_t, uint32_t>(Start, 0));
				Range != CommentRanges.end() && Range->second <= End;
				++Range
			) {
				// 錨へ紐付けて字面から除いたコメントの読飛
				if(Separated.IsSeparatedAt(Range->first)) continue;
				const std::string_view Body = std::string_view(Src).substr(Range->first, Range->second - Range->first);
				const size_t At = Lexeme.find(Body, Cursor);
				if(At == std::string::npos) {
					throw FormatLimitExceeded("a comment kept in a verbatim region is lost", FailureReason::FormatterDefect);
				}
				Result.Validation.ProtectedRegions.KeptComments.push_back(
					{ Id, static_cast<uint32_t>(At), static_cast<uint32_t>(At + Body.size()) }
				);
				Cursor = At + Body.size();
			}
		};
		// 投影した節点毎の原文の出所（構文事実の記録に使い，中核へは渡さない）と，区切記法の後の最初の字句か
		std::vector<Origin> Sources;
		std::vector<uint8_t> SeparatedBefore;
		// 投影する節点の数は解析器の節点の数に概ね等しい為，伸長の度の再確保を避ける
		const TSNode RootNode = Src.GetRoot();
		const uint32_t DescendantCount = ts_node_descendant_count(RootNode);
		Sources.reserve(DescendantCount);
		SeparatedBefore.reserve(Sources.capacity());
		bool IsSeparatorPending = false;
		std::vector<Frame> Stack { { RootNode, NoNode, 0 } }, Children;
		std::vector<uint8_t> ConsumedGroups(Src.GetSyntheticGroups().size());
		const size_t HaltedStart = Src.HaltedDataStart();
		const uint32_t CodeEnd = static_cast<uint32_t>(HaltedStart == std::string::npos ? Src.size() : HaltedStart);
		const bool HasOuterTail = BodyEnd < CodeEnd;
		// CSS の宣言の値の語の記号
		const TSSymbol PlainValue = Profile.SymbolWithRole(Role::UnitText);
		const uint32_t FieldCount = ts_language_field_count(Profile.GetLanguage().TsLang());
		// 検証資料を集める節点（原文順，同じ節点を重ねて訪ねた物は最後に除く）
		std::vector<TSNode> ValidationNodes, ExtraNodes;
		ValidationNodes.reserve(DescendantCount);
		const auto RecordNode = [&ValidationNodes](const TSNode Node) -> void {
			if(!ts_node_is_null(Node)) ValidationNodes.push_back(Node);
		};
		const auto RecordDescendants = [&RecordNode](const TSNode Node) -> void {
			HeldCursor Cursor(Node);
			// 子を持たない節点の終了
			if(!ts_tree_cursor_goto_first_child(&Cursor)) return;
			do WalkAst(ts_tree_cursor_current_node(&Cursor), RecordNode);
			while(ts_tree_cursor_goto_next_sibling(&Cursor));
			// 終了
			return;
		};
		// 構文木から節点表への投影
		while(!Stack.empty()) {
			const Frame Top = Stack.back();
			Stack.pop_back();
			if(ts_node_is_null(Top.Node)) {
				// 隠れた字句の無名の字句の節点
				const auto [Start, End] = Top.Hidden;
				// 本体の外の地の文の中の字句の読飛
				if(HasOuterTail && Start >= BodyEnd && ts_node_eq(Top.ParentNode, RootNode)) continue;
				const std::string_view Text = std::string_view(Src).substr(Start, End - Start);
				const TSSymbol Symbol = Profile.TokenSymbol(Text);
				SyntaxNode Hidden;
				Hidden.Kind = { Symbol ? &Profile.KindOf(Symbol) : HiddenLexemeKind(Src, Top.ParentNode) };
				Hidden.Parent = Top.Parent;
				Hidden.Lexeme = ParseAdapter::DecodeLexeme(Src, std::string(Text), Start);
				const NodeId Id = Syntax.AddNode(std::move(Hidden));
				Sources.push_back({ NoNode, Start, End });
				SeparatedBefore.push_back(IsSeparatorPending);
				IsSeparatorPending = false;
				Syntax.Nodes[Id].Index = static_cast<uint32_t>(Syntax.Nodes[Top.Parent].Children.size());
				Syntax.Nodes[Top.Parent].Children.push_back(Id);
				continue;
			}
			const size_t RecordedIndex = ValidationNodes.size();
			RecordNode(Top.Node);
			if(Top.ValidationOnly) {
				RecordDescendants(Top.Node);
				continue;
			}
			// 両側が解析用の囲みである包装は，名前付の子を元の親と場へ直接投影する
			const std::pair<uint32_t, uint32_t> Span { ts_node_start_byte(Top.Node), ts_node_end_byte(Top.Node) - 1 };
			const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Group =
			std::ranges::lower_bound(Src.GetSyntheticGroups(), Span);
			if(
				Group != Src.GetSyntheticGroups().end() && *Group == Span && ts_node_child_count(Top.Node) > 2 &&
				Src.Is(ts_node_child(Top.Node, 0), Role::OpeningParenToken) &&
				Src.Is(ts_node_child(Top.Node, ts_node_child_count(Top.Node) - 1), Role::ClosingParenToken)
			) {
				RecordDescendants(Top.Node);
				ConsumedGroups[static_cast<size_t>(std::distance(Src.GetSyntheticGroups().begin(), Group))] = 1;
				TSNode Inner {};
				for(uint32_t Index = 0; Index < ts_node_named_child_count(Top.Node); ++Index) {
					const TSNode Child = ts_node_named_child(Top.Node, Index);
					if(Src.Is(Child, Role::Comment)) continue;
					if(!ts_node_is_null(Inner)) {
						throw FormatLimitExceeded("a synthetic group has multiple expressions", FailureReason::FormatterDefect);
					}
					Inner = Child;
				}
				if(ts_node_is_null(Inner)) throw FormatLimitExceeded("a synthetic group has no expression", FailureReason::FormatterDefect);
				Frame Next = Top;
				Next.Node = Inner;
				Next.Groups.push_back(Top.Node);
				Next.Groups.push_back(ts_node_child(Top.Node, 0));
				Next.Groups.push_back(ts_node_child(Top.Node, ts_node_child_count(Top.Node) - 1));
				Stack.push_back(std::move(Next));
				continue;
			}
			// 末尾カンマに対応しない解析器が破損として包んだ JSON のカンマは，通常の区切りの字句として写し，包みに紐付いたコメントも其のカンマへ移す（整形仕様 4.2）
			const TSNode Wrapper = Top.Node;
			const TSNode Misread = Profile.GetReading().ToleratesTrailingCommaError && Src.Is(Wrapper, Role::Error) ?
			SyntaxParser::JsonTrailingCommaToken(Src, Wrapper) :
			TSNode{};
			const TSNode Comma = !ts_node_is_null(Misread) && ts_node_eq(ts_node_parent(Misread), Wrapper) ? Misread : TSNode{};
			const TSNode Node = ts_node_is_null(Comma) ? Wrapper : Comma;
			if(!ts_node_is_null(Comma)) RecordDescendants(Wrapper);
			// 本体の外の地の文の中の子（閉じタグの後を解析器がコメント等と読んだ物）の読飛
			if(HasOuterTail && ts_node_start_byte(Node) >= BodyEnd && ts_node_eq(Top.ParentNode, RootNode)) {
				RecordDescendants(Node);
				continue;
			}
			// 錨へ紐付けたコメントは節点にせず，除いた跡で字句が融合する CSS の境界だけ区切記法を置く（整形仕様 6.1, 7.1）
			if(Separated.IsSeparatedComment(Src, Node)) {
				RecordDescendants(Node);
				IsSeparatorPending = IsSeparatorPending || Separated.IsNotationSeparated(Node);
				continue;
			}
			// 空のカスタムプロパティ値を整数と読む偽の節点
			const bool IsEmptyCustomValue = ParseAdapter::IsEmptyCssCustomValue(Src, Node);
			// 解析器が補った字句と，CSS の空値の偽節点，字句の融合を防ぐ空の区切記法は投影しない
			if(
				ts_node_is_missing(Node) || IsEmptyCustomValue ||
				!Src.GetRules().LexicalSeparator.empty() && Src.Is(Node, Role::Comment) && Src.View(Node) == Src.GetRules().LexicalSeparator
			) {
				RecordDescendants(Node);
				if(HasAttached(Node)) {
					throw FormatLimitExceeded("a comment is attached to an unprojected token", FailureReason::FormatterDefect);
				}
				IsSeparatorPending = IsSeparatorPending || !ts_node_is_missing(Node) && !IsEmptyCustomValue;
				continue;
			}
			if(
				Src.GetReading().HasKeywordClosedBlocks && (
					Src.Is(Node, Role::EmptyStatement) || Src.Is(Node, Role::ThenSection) && !CountCodeChildren(Src, Separated, Node, true, nullptr)
				)
			) {
				RecordDescendants(Node);
				// 空の then の本体の所有者は其の親
				MoveAttached(Node, Top.Parent, Src.Is(Node, Role::ThenSection) ? Top.Parent : NoNode, LastProjected(Top.Parent, NoNode), true);
				continue;
			}
			if(Src.GetReading().HasKeywordClosedBlocks && !ts_node_is_named(Node)) if(
				(Src.Is(Node, Role::ConditionBodyDelimiterToken) || Src.Is(Node, Role::IterationBodyDelimiterToken)) &&
				(Src.Is(Top.ParentNode, Role::ThenSection) || Src.Is(Top.ParentNode, Role::IterationSection)) ||
				Src.Is(Node, Role::StatementTerminatorToken) && !Src.Is(Top.ParentNode, Role::BlockParameterList)
			) {
				RecordDescendants(Node);
				// 名前付の子を持たない then / do の本体の語は，本体の所有者の空の本体の物
				const bool IsEmptyBody =
				!Src.Is(Node, Role::StatementTerminatorToken) && !CountCodeChildren(Src, Separated, Top.ParentNode, true, nullptr);
				MoveAttached(Node, Top.Parent, IsEmptyBody ? Syntax.Nodes[Top.Parent].Parent : NoNode, LastProjected(Top.Parent, NoNode), true);
				continue;
			}
			const bool HasChildren = ts_node_child_count(Node), IsVerbatim = HasChildren && IsVerbatimNode(Src, Separated, Node);
			SyntaxNode Projected;
			// CSS の宣言の値の語は，密着したコメントの有無で解析器が名前と値の語に読み分ける為，値の語へ揃える（整形仕様 4.2）
			const bool IsGluedValueName =
			Src.Is(Node, Role::PlainValueWord) && Top.Parent != NoNode && Syntax.Is(Top.Parent, Role::TypedDeclaration);
			const TSSymbol ProjectedSymbol = IsGluedValueName ? PlainValue : ts_node_symbol(Node);
			Projected.Kind = { &Profile.KindOf(ProjectedSymbol) };
			Projected.Facts.Forms = Profile.FormsOf(ProjectedSymbol);
			Projected.Parent = Top.Parent;
			Projected.Fields = Top.Fields;
			// 解析用の印を本来の字句へ復号した字面（整形仕様 4.2）
			if(!HasChildren || IsVerbatim) Projected.Lexeme = VerbatimText(Src, Separated, Node);
			if(!HasChildren && Src.RestorationAt(Src.Start(Node), Src.End(Node))) {
				const std::unordered_map<std::string_view, const KindDefinition *>::const_iterator Found =
				Src.GetRules().TokenDefinitions.find(Projected.Lexeme);
				if(Found == Src.GetRules().TokenDefinitions.end()) {
					throw FormatLimitExceeded("a restored token has no definition", FailureReason::FormatterDefect);
				}
				Projected.Kind = { Found->second };
				const TSSymbol Restored = Profile.TokenSymbol(Projected.Lexeme);
				if(!Restored) throw FormatLimitExceeded("a restored token has no grammar symbol", FailureReason::FormatterDefect);
				Projected.Facts.Forms = Profile.FormsOf(Restored);
			}
			// 本文の ASCII 字下げを正規化し，共通入力段の空白の印を実際の字句へ復号
			if(Projected.Kind.Definition->Roles.test(static_cast<size_t>(Role::CollapsibleText))) {
				if(Projected.Lexeme.find('\n') != std::string::npos) {
					std::string Normalized;
					for(size_t Start = 0; Start <= Projected.Lexeme.size();) {
						const size_t End = Projected.Lexeme.find('\n', Start);
						std::string_view Line = std::string_view(Projected.Lexeme).substr(Start, End == std::string::npos ? End : End - Start);
						if(Start) while(!Line.empty() && (Line.front() == ' ' || Line.front() == '\t')) Line.remove_prefix(1);
						if(End != std::string::npos) while(!Line.empty() && (Line.back() == ' ' || Line.back() == '\t')) Line.remove_suffix(1);
						Normalized += Line;
						if(End == std::string::npos) break;
						Normalized += '\n';
						Start = End + 1;
					}
					Projected.Lexeme = std::move(Normalized);
				}
				Projected.Lexeme = ParseAdapter::DecodeJsxBlanks(Src, std::move(Projected.Lexeme));
			}
			// 前処理指令の語は `#` と名前の間の空白を詰めた綴り（字句間空白仕様 5.4）
			if(
				Top.Parent != NoNode && Syntax.Is(Top.Parent, Role::Preprocessor) && Projected.Lexeme.size() > 1 &&
				Projected.Lexeme.front() == '#' && (Projected.Lexeme[1] == ' ' || Projected.Lexeme[1] == '\t')
			) Projected.Lexeme.erase(1, Projected.Lexeme.find_first_not_of(" \t", 1) - 1);
			// 改行だけから成る字句（前処理指令を終える改行）は後続の空行を含み得る為，字面を１つの改行にして空行を配置へ委ねる
			if(!HasChildren && !Projected.Lexeme.empty() && Projected.Lexeme.find_first_not_of("\r\n") == std::string::npos) {
				Projected.Lexeme = "\n";
			}
			// 分離出来ずにコードとして残るコメント（誤り区画の中等）は空行の照合基準で除く範囲に為る
			const bool IsKeptComment = !HasChildren && Src.Is(Node, Role::Comment);
			if(IsVerbatim) Projected.Facts.Add(ConstraintKind::Verbatim);
			const NodeId Id = Syntax.AddNode(std::move(Projected));
			uint32_t SourceIndex = static_cast<uint32_t>(RecordedIndex);
			if(!ts_node_eq(Node, Top.Node)) {
				SourceIndex = ExtraNodeBit | static_cast<uint32_t>(ExtraNodes.size());
				ExtraNodes.push_back(Node);
			}
			Sources.push_back({ SourceIndex, ts_node_start_byte(Node), ts_node_end_byte(Node) });
			// 末尾のデータ区画は区画を開く印の行から始まる（解析器は印を節点に含めない）
			if(Src.Is(Node, Role::TrailingDataSection)) {
				const std::string_view Code = Src.CodeText();
				const uint32_t Start = Sources.back().Start;
				const size_t MarkEnd = !Start ? std::string_view::npos : Code.rfind('\n', Start - 1);
				const uint32_t LineStart = MarkEnd == std::string_view::npos ? 0 : static_cast<uint32_t>(MarkEnd + 1);
				Syntax.Nodes[Id].Lexeme.insert(0, Code.substr(LineStart, Start - LineStart));
				Sources.back().Start = LineStart;
			}
			if(IsKeptComment) {
				Result.Validation.ProtectedRegions.KeptComments.push_back({ Id, 0, static_cast<uint32_t>(Syntax.Nodes[Id].Lexeme.size()) });
			}
			// マクロの本体は空白で区切られた字句の並び（字句の間は投影する境界が表す，字句間空白仕様 5.4）
			if(Src.Is(Node, Role::MacroBody)) {
				const std::string Body = std::move(Syntax.Nodes[Id].Lexeme);
				Syntax.Nodes[Id].Lexeme.clear();
				// 見出名を引数に取る指令の引数か
				std::string_view Directive = Src.View(ts_node_child(Top.ParentNode, 0));
				Directive.remove_prefix(std::min(Directive.find_first_not_of("# \t"), Directive.size()));
				const std::vector<std::string_view> &HeaderNameDirectives = Src.GetReading().HeaderNameDirectives;
				const bool IsHeaderName = std::ranges::find(HeaderNameDirectives, Directive) != HeaderNameDirectives.end();
				for(const auto &[From, To] : ParseAdapter::MacroBodyTokens(Body, IsHeaderName)) {
					SyntaxNode Token;
					Token.Kind = Syntax.Nodes[Id].Kind;
					Token.Parent = Id;
					Token.Lexeme = Body.substr(From, To - From);
					const NodeId TokenId = Syntax.AddNode(std::move(Token));
					Sources.push_back({ NoNode, Sources[Id].Start + From, Sources[Id].Start + To });
					SeparatedBefore.push_back(false);
					Syntax.Nodes[TokenId].Index = static_cast<uint32_t>(Syntax.Nodes[Id].Children.size());
					Syntax.Nodes[Id].Children.push_back(TokenId);
					// 本体に残るコメント（解析用の印で覆った物は最終字面でも印で覆う為に除く）は空行の照合基準で除く範囲に為る
					const uint32_t At = Sources[TokenId].Start;
					const std::string_view TokenText = std::string_view(Body).substr(From);
					if(
						(
							TokenText.starts_with(CommentRepository::OpenerOf(CommentNotation::SlashLine)) ||
							TokenText.starts_with(CommentRepository::OpenerOf(CommentNotation::Block))
						) && !(Src.GetAnalysisMark() && At + 1 < Src.size() && Src[At + 1] == Src.GetAnalysisMark())
					) Result.Validation.ProtectedRegions.KeptComments.push_back({ TokenId, 0, To - From });
				}
			}
			SeparatedBefore.push_back(IsSeparatorPending && (!HasChildren || IsVerbatim));
			if(!HasChildren || IsVerbatim) IsSeparatorPending = false;
			if(Top.Parent == NoNode) Syntax.Root = Id;
			else {
				Syntax.Nodes[Id].Index = static_cast<uint32_t>(Syntax.Nodes[Top.Parent].Children.size());
				Syntax.Nodes[Top.Parent].Children.push_back(Id);
			}
			// 名前付の子を持たない Ruby の do の本体（組立で語を所有者が出し，錨の出力位置を持たない）へ紐付いたコメントは所有者の物とする
			if(
				Src.GetReading().HasKeywordClosedBlocks && ts_node_is_named(Node) && !CountCodeChildren(Src, Separated, Node, true, nullptr) &&
				Src.Is(Node, Role::IterationSection) && Top.Parent != NoNode
			) MoveAttached(Node, Top.Parent, Top.Parent, LastProjected(Top.Parent, Id), false);
			// 錨の節点を此の節点へ写したコメントの記録
			else for(const TSNode Source : { Wrapper, Comma }) {
				if(ts_node_is_null(Source)) continue;
				for(const std::vector<CommentRecord> *const Attached : { &Separated.GetLeading(Source), &Separated.GetTrailing(Source) }) {
					for(CommentRecord Record : *Attached) {
						Record.Anchor = Id;
						Result.Comments.Add(std::move(Record));
					}
				}
			}
			for(const TSNode GroupNode : Top.Groups) for(
				const std::vector<CommentRecord> *const Attached :
				{ &Separated.GetLeading(GroupNode), &Separated.GetTrailing(GroupNode) }
			) for(CommentRecord Record : *Attached) {
				Record.Anchor = Id;
				Result.Comments.Add(std::move(Record));
			}
			// 子の無い字句の終了
			if(!HasChildren) continue;
			if(IsVerbatim) {
				RecordDescendants(Node);
				RecordKept(Node, Id);
				// 字面を保つ区画の中へ紐付いたコメントは置き場を失う
				ForEachChild(
					Node,
					[&](const TSNode Child) -> void {
						if(HasAttached(Child)) {
							throw FormatLimitExceeded("a comment is attached inside a verbatim region", FailureReason::FormatterDefect);
						}
					}
				);
				continue;
			}
			// 子を原文順に訪ねる為の逆順の積上（誤りを含まない節点は子の間の隠れた字句も子にし，誤りを含む節点の読み飛ばした字句は保つ境界が写す）
			Children.clear();
			const bool FindsHidden = !ts_node_has_error(Node);
			uint32_t From = ts_node_start_byte(Node);
			const auto AddHidden = [&](const uint32_t To) -> void {
				// 隠れた字句を探さない誤りを含む節点の場合の終了
				if(!FindsHidden) return;
				for(const std::pair<uint32_t, uint32_t> &Range : HiddenTokens(Src, From, To, Gaps)) {
					// 文を終える隠れた `;` は文境界で字句にせず（文の並びは文を改行で区切り，改行で繋がる文にだけ `;` を置く），文法の字句に無い字面（文字列の引用符等）は構文事実の保つ間隙として写す
					if(!IsTokenizedHidden(Src, Node, std::string_view(Src).substr(Range.first, Range.second - Range.first))) continue;
					Children.push_back({ TSNode{}, Id, 0, Range, Node });
				}
			};
			HeldCursor Cursor(Node);
			bool HasFields = false;
			for(
				bool IsWalking = ts_tree_cursor_goto_first_child(&Cursor);
				IsWalking;
				IsWalking = ts_tree_cursor_goto_next_sibling(&Cursor)
			) {
				const TSNode Child = ts_tree_cursor_current_node(&Cursor);
				const TSFieldId Field = ts_tree_cursor_current_field_id(&Cursor);
				AddHidden(ts_node_start_byte(Child));
				if(!Src.Is(Child, Role::LineContinuation)) Children.push_back({ Child, Id, Profile.FieldBits(Field), {}, Node });
				else {
					Frame PendingValidation { Child, Id, 0, {}, Node };
					PendingValidation.ValidationOnly = true;
					Children.push_back(std::move(PendingValidation));
				}
				HasFields = HasFields || Field;
				From = std::max(From, ts_node_end_byte(Child));
			}
			AddHidden(ts_node_end_byte(Node));
			// カーソルは隠れた規則の最も内の場だけを返す為，外の規則の場（Swift の戻り値の型等）を場毎の最初の子へ加える
			if(HasFields) for(TSFieldId Field = 1; Field <= FieldCount; ++Field) {
				const TSNode First = ts_node_child_by_field_id(Node, Field);
				if(ts_node_is_null(First)) continue;
				for(Frame &Pending : Children) if(ts_node_eq(Pending.Node, First)) Pending.Fields |= Profile.FieldBits(Field);
			}
			Stack.insert(Stack.end(), Children.rbegin(), Children.rend());
		}
		// 解析用括弧の照合と走査領域の解放
		if(std::ranges::find(ConsumedGroups, uint8_t{ 0 }) != ConsumedGroups.end()) {
			throw FormatLimitExceeded("a synthetic group has no parser wrapper", FailureReason::FormatterDefect);
		}
		std::vector<Frame>().swap(Stack);
		std::vector<Frame>().swap(Children);
		std::vector<uint8_t>().swap(ConsumedGroups);
		// PHP の本体の外の地の文（先頭の空白と閉じタグの後の地の文）を根の逐語の子として保つ（整形仕様 4.4）
		if(BodyStart || HasOuterTail) {
			const auto AddOuterText = [&](const uint32_t Start, const uint32_t End, const bool IsFirst) -> void {
				// 地の文の逐語節点の追加
				SyntaxNode Outer;
				Outer.Kind = { &Profile.GetOuterTextKind() };
				Outer.Parent = Syntax.Root;
				Outer.Lexeme = ParseAdapter::DecodeLexeme(Src, std::string(Src, Start, End - Start), Start);
				Outer.Facts.Add(ConstraintKind::Verbatim);
				const NodeId Id = Syntax.AddNode(std::move(Outer));
				Sources.push_back({ NoNode, Start, End });
				SeparatedBefore.push_back(false);
				std::vector<NodeId> &Children = Syntax.Nodes[Syntax.Root].Children;
				Children.insert(IsFirst ? Children.begin() : Children.end(), Id);
				for(uint32_t Index = 0; Index < Children.size(); ++Index) Syntax.Nodes[Children[Index]].Index = Index;
			};
			if(BodyStart) AddOuterText(0, BodyStart, true);
			if(HasOuterTail) AddOuterText(BodyEnd, CodeEnd, false);
		}
		// 実行停止後のデータは終端を含む原文を根の最後の逐語区画へ写す
		if(HaltedStart != std::string::npos) {
			SyntaxNode Data;
			Data.Kind = { &Profile.GetTrailingDataKind() };
			Data.Parent = Syntax.Root;
			Data.Lexeme.assign(Src, HaltedStart);
			Data.Facts.Add(ConstraintKind::Verbatim);
			const NodeId Id = Syntax.AddNode(std::move(Data));
			Sources.push_back({ NoNode, CodeEnd, static_cast<uint32_t>(Src.size()) });
			SeparatedBefore.push_back(false);
			std::vector<NodeId> &RootChildren = Syntax.Nodes[Syntax.Root].Children;
			Syntax.Nodes[Id].Index = static_cast<uint32_t>(RootChildren.size());
			RootChildren.push_back(Id);
		}
		// 省略可能な節の区切語に依り本体へ取り込まれたヒアドキュメントを，見出直後の兄弟へ揃える
		if(Src.GetReading().HasKeywordClosedBlocks) for(NodeId Id = 0; Id < Syntax.NodeCount(); ++Id) {
			if(!Syntax.Is(Id, Role::KeywordBlock)) continue;
			std::vector<NodeId> &Children = Syntax.Nodes[Id].Children;
			for(size_t Index = 0; Index < Children.size(); ++Index) {
				const NodeId Body = Children[Index];
				if(!Syntax.Is(Body, Role::StatementSequence)) continue;
				std::vector<NodeId> &Parts = Syntax.Nodes[Body].Children;
				size_t Count = 0;
				while(Count < Parts.size() && Syntax.Is(Parts[Count], Role::HeredocBody)) ++Count;
				if(!Count) continue;
				for(size_t Part = 0; Part < Count; ++Part) Syntax.Nodes[Parts[Part]].Parent = Id;
				Children.insert(Children.begin() + Index, Parts.begin(), Parts.begin() + Count);
				Parts.erase(Parts.begin(), Parts.begin() + Count);
				Sources[Body].Start = Parts.empty() ? Sources[Body].End : Sources[Parts.front()].Start;
				for(uint32_t Part = 0; Part < Parts.size(); ++Part) Syntax.Nodes[Parts[Part]].Index = Part;
				for(uint32_t Part = 0; Part < Children.size(); ++Part) Syntax.Nodes[Children[Part]].Index = Part;
				Index += Count;
			}
		}
		{
			// 重ねて訪ねた節点の除去（最初の出現の順と出所の節点を保つ）
			std::vector<uint32_t> Visits;
			Visits.reserve(ValidationNodes.size());
			for(uint32_t Index = 0; Index < ValidationNodes.size(); ++Index) Visits.push_back(Index);
			std::sort(
				Visits.begin(),
				Visits.end(),
				[&ValidationNodes](const uint32_t Left, const uint32_t Right) -> bool {
					// 節点の識別子と最初に訪ねた位置の順の返戻
					return std::pair(ValidationNodes[Left].id, Left) < std::pair(ValidationNodes[Right].id, Right);
				}
			);
			std::vector<uint8_t> IsRepeated(ValidationNodes.size(), 0);
			for(size_t Index = 1; Index < Visits.size(); ++Index) {
				if(ValidationNodes[Visits[Index]].id == ValidationNodes[Visits[Index - 1]].id) IsRepeated[Visits[Index]] = 1;
			}
			std::vector<uint32_t> Remap(ValidationNodes.size());
			uint32_t Kept = 0;
			for(uint32_t Index = 0; Index < ValidationNodes.size(); ++Index) {
				if(IsRepeated[Index]) continue;
				Remap[Index] = Kept++;
			}
			for(Origin &Source : Sources) {
				if(Source.NodeIndex == NoNode || Source.NodeIndex & ExtraNodeBit) continue;
				if(const uint32_t Index = Source.NodeIndex; IsRepeated[Index]) {
					Source.NodeIndex = ExtraNodeBit | static_cast<uint32_t>(ExtraNodes.size());
					ExtraNodes.push_back(ValidationNodes[Index]);
				} else Source.NodeIndex = Remap[Index];
			}
			for(uint32_t Index = 0; Index < ValidationNodes.size(); ++Index) {
				if(!IsRepeated[Index]) ValidationNodes[Remap[Index]] = ValidationNodes[Index];
			}
			ValidationNodes.resize(Kept);
		}
		// 検証資料と構文事実の組立
		ValidationContext Validation = CollectContext(Src, Profile, Input, ValidationNodes);
		// 指示コメントの登録定義（始まりの位置と同じ順，最終字面の記録へ引き継ぐ）
		std::vector<DirectiveDefinitionId> DirectiveDefinitions;
		Validation.ProtectedRegions.Directives =
		CollectDirectives(Src, Separated.Head, ValidationNodes, &Validation.ProtectedRegions.DirectiveStarts, &DirectiveDefinitions);
		Validation.ProtectedRegions.KeptComments = std::move(Result.Validation.ProtectedRegions.KeptComments);
		Result.Validation = std::move(Validation);
		RecordFacts(Src, Separated, Gaps, Sources, ValidationNodes, ExtraNodes, SeparatedBefore, Syntax);
		if(Src.GetReading().HasEmbeddedScripts) RecordMarkupFacts(Src, Sources, ValidationNodes, ExtraNodes, Syntax);
		if(Profile.GetRules().HasDeferredLiteralBodies) RebindDeferredLiteralBodies(Sources, Syntax);
		std::vector<uint8_t>().swap(SeparatedBefore);
		// 文法の定義する解析器の読違の結付の復元（整形仕様 4.3，意味の異なる構文を同じ表現へ投影しない）
		const ReadingRules &Rules = Profile.GetReading();
		if(Rules.MisbindsEmbeddedBranches) RebindEmbeddedBranches(Rules, Syntax);
		if(Rules.MisbindsPostfixOperand) RebindSwiftPostfix(Syntax);
		// 名前・ラベルの字句と名前の位置に在る節点の比較用の名前を，字句の規則に従って投影
		for(NodeId Id = 0; Id < Syntax.NodeCount(); ++Id) {
			if(!Syntax.Is(Id, Role::NameToken) && !Syntax.Is(Id, Role::BindingName) && !Syntax.HasField(Id, ChildRole::Name)) continue;
			std::string Name = ComparisonNameOf(Rules, Syntax.Text(Id), Syntax.Is(Id, Role::Label));
			if(Name.empty()) continue;
			Syntax.ComparisonNames.push_back(std::move(Name));
			Syntax.Nodes[Id].Facts.NameIndex = static_cast<uint32_t>(Syntax.ComparisonNames.size());
		}
		// 入力の文書化対象の定義（コメントの有無に依らず，定義の錨と記法の定義を器へ持たせる，整形仕様 3.2）
		if(!std::holds_alternative<ParseInput::VerificationSource>(Input.Source)) {
			const std::vector<uint8_t> Definitions = DocCommentSynchronizer::DefinitionsOf(Syntax);
			for(NodeId Id = 0; Id < Syntax.NodeCount(); ++Id) {
				if(Definitions[Id]) Result.Comments.AddDocumentationSite({ Id, &Profile.GetRules().Notation });
			}
		}
		// 解析器の記号は検証資料だけへ写し，中核の節点表には残さない
		Result.Validation.SemanticChecks.Symbols.resize(Syntax.NodeCount(), std::numeric_limits<TSSymbol>::max());
		for(NodeId Id = 0; Id < Sources.size(); ++Id) {
			if(const TSNode Source = SourceNode(Sources[Id], ValidationNodes, ExtraNodes); !ts_node_is_null(Source)) {
				Result.Validation.SemanticChecks.Symbols[Id] = ts_node_symbol(Source);
				continue;
			}
			if(Sources[Id].Start > Sources[Id].End || Sources[Id].End > Src.size()) continue;
			const std::string_view Raw = std::string_view(Src).substr(Sources[Id].Start, Sources[Id].End - Sources[Id].Start);
			if(const TSSymbol Symbol = Profile.TokenSymbol(Raw); Symbol && &Profile.KindOf(Symbol) == Syntax.Nodes[Id].Kind.Definition) {
				Result.Validation.SemanticChecks.Symbols[Id] = Symbol;
			}
		}
		std::vector<TSNode>().swap(ValidationNodes);
		std::vector<TSNode>().swap(ExtraNodes);
		if(const ParseInput::VerificationSource *const Verification = std::get_if<ParseInput::VerificationSource>(&Input.Source)) {
			// 完成候補の文書の説明対象と構造（整形仕様 7.4, 8.1，期待コメントから同じ規則で集めた物と検証が比べる）
			Result.Validation.ProtectedRegions.Documents =
			DescribeDocuments(Syntax, Result.Comments.GetRecords(), DocCommentSynchronizer::DocumentTargets(Syntax, Result.Comments));
			// 完成候補の行末コメントの前置区切りの形（期待コメントの記録と検証が照合する，整形仕様 7.6, 8.1）
			Result.Validation.InputDiagnostics.LeadingSeparators = std::move(Separated.LeadingSeparators);
			std::vector<uint32_t> Starts = Result.Validation.ProtectedRegions.DirectiveStarts;
			for(uint32_t &Start : Starts) Start = Src.OffsetBeforeEdits(Start);
			Result.Comments = ProjectOutputComments(
				Profile.GetRules().Notation,
				Verification->BeforeText,
				Verification->Comments,
				Starts,
				DirectiveDefinitions,
				Profile.GetReading()
			);
		}
		// 投影と検証資料の収集を終えた構文の基準量からの構文の正規化と中間表現の構築の作業単位の上限と，構造の段数の上限（整形仕様 9.2）
		Syntax.FixWorkLimit();
		Syntax.CheckNesting();
		if(std::holds_alternative<ParseInput::VerificationSource>(Input.Source)) {
			// 静的検査が読む検証の構文の診断位置の投影（解析用の補修の前の本文の位置，行と桁は本文を持つ呼出側が求める）
			Result.Validation.InputDiagnostics.Positions.resize(Syntax.NodeCount());
			for(NodeId Id = 0; Id < Sources.size(); ++Id) {
				Result.Validation.InputDiagnostics.Positions[Id] =
				{ Src.OffsetBeforeEdits(Sources[Id].Start), Src.OffsetBeforeEdits(Sources[Id].End), {} };
			}
			for(NodeId Id = static_cast<NodeId>(Syntax.NodeCount()); Id-- > Sources.size();) {
				if(const std::span<const NodeId> Children = Syntax.ChildrenOf(Id); !Children.empty()) {
					Result.Validation.InputDiagnostics.Positions[Id] = Result.Validation.InputDiagnostics.Positions[Children.front()];
					Result.Validation.InputDiagnostics.Positions[Id].End = Result.Validation.InputDiagnostics.Positions[Children.back()].End;
				}
			}
		}
		// 投影の結果の返戻
		return Result;
	}

	/**
	 * 構文・コメント・検証資料への投影関数（整形仕様 3.4, 4.3, 7.1）
	 * 原文のコメントを錨へ紐付けて分離し，構文を独自の節点表へ写す
	 * @param Src 解析済の原稿（書き換えない）
	 * @param Profile 入力で確定した文法
	 * @param Input 適応済の入力と補修前の字面
	 * @return 節点表・コメント・検証資料と診断位置
	 * @throws FormatLimitExceeded 分離の対象のコメントの節点が残る場合（整形仕様 1.2 の経路の分離の違反）
	 */
	ProjectionResult SourceProjector::Project(const ::ParsedTree &Src, const GrammarProfile &Profile, const ParseInput &Input) {
		// 原文のコメントの分離
		CommentSeparation Separated = SeparateComments(Src, Input);
		// 経路の分離（整形仕様 1.2）の違反は，検査用の指定の有無に依らず整形器の不具合として失敗させる（整形仕様 10.5）
		if(Src.IsParsed() && CountUnseparatedComments(Src, Separated, ParseAdapter::OuterTextBounds(Src).second)) {
			throw FormatLimitExceeded("comment nodes reach the core passes", FailureReason::FormatterDefect);
		}
		// 構文と同じ解析結果から組み立てた資料の返戻
		return ProjectNodes(Src, Profile, Input, Separated);
	}
} // namespace Core
