#include "LineBreaker.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include "../Util/TextMetrics.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 平坦時の文字数の取得関数（行分割仕様 3.2）
	 * 選択可能な境界と展開時だけ出す断片を平坦形で数え，最初の必須改行・空行又は改行を含む逐語の最初の行迄を測る
	 * 計算量：辿る要素数 N と測る字面の長さ T に対し O(N + T)
	 * @param IR 中間表現
	 * @param Id 要素
	 * @param HasBreak 必須改行・空行又は改行を含む逐語を含むかの格納先（真なら其の後を数えない）
	 * @param Work 字句を再走査したバイト数と作業単位の記録先
	 * @return 平坦時の文字数
	 */
	size_t LineBreaker::FlatWidth(const FormatIR &IR, const FormatItemId Id, bool &HasBreak, WorkMeter &Work) {
		// 子を巡る作業枠と幅の準備
		struct Frame {
			FormatItemId Id;
			FormatRange Children = {};
			uint32_t Next = 0;
			size_t Width = 0;
			bool Entered = false;
		};
		std::vector<Frame> Pending { { Id } };
		size_t Result = 0;
		// 平坦時の幅と必須改行の集計
		while(!Pending.empty()) {
			Frame &Current = Pending.back();
			if(!Current.Entered) {
				Current.Entered = true;
				if(!HasBreak) {
					const FormatItem &Item = IR.Items[Current.Id];
					const std::string_view Text = IR.TextOf(Item);
					if(!Text.empty()) {
						Work.Charge(Text.size());
						Work.LexemeRescanBytes += Text.size();
					}
					switch(Item.Kind) {
					case FormatItemKind::LineEdgeSpace:
					case FormatItemKind::Token:
					case FormatItemKind::Space:
					case FormatItemKind::LexicalSeparator:
					case FormatItemKind::SoftBreak:
						Current.Width = TextMetrics::CountCharacters(Text);
						break;
					case FormatItemKind::Verbatim:
						{
							const size_t Newline = Text.find('\n');
							HasBreak = Newline != std::string_view::npos;
							Current.Width = TextMetrics::CountCharacters(Text.substr(0, Newline));
						}
						break;
					case FormatItemKind::HardBreak:
					case FormatItemKind::BlankLine:
					case FormatItemKind::OwnedRegion:
						HasBreak = true;
						break;
					case FormatItemKind::IfBroken:
						break;
					default:
						Current.Children = Item.Children;
						break;
					}
				}
			}
			if(Current.Next < Current.Children.Size) {
				Pending.push_back({ IR.Links[Current.Children.Begin + Current.Next++] });
				continue;
			}
			Result = Current.Width;
			Pending.pop_back();
			if(!Pending.empty()) Pending.back().Width += Result;
		}
		// 子の文字数の和の返戻
		return Result;
	}

	/**
	 * 共有する部分の写しの取得関数
	 * 区分した２つ目以降の宣言で繰り返す部分の錨を外し，群には新しい番号を振って折返の選択肢を保つ
	 * 計算量：写す要素数 N に対し O(N)
	 * @param IR 中間表現（写しを追加する）
	 * @param Id 最初の宣言の共有する部分
	 * @param Groups 元の群番号から此の出力宣言の群番号への対応
	 * @param NextGroup 新しい群番号の割当先
	 * @return 錨を重複させず選択肢を保つ写し
	 * @throws FormatLimitExceeded 群の番号を割り当てれない場合，又は複写元の展開依存先が先行しない場合
	 */
	FormatItemId LineBreaker::SharedCopy(
		FormatIR &IR,
		const FormatItemId Id,
		std::unordered_map<uint32_t, uint32_t> &Groups,
		uint32_t &NextGroup
	) {
		// 共有部分を後行で複写する作業枠の準備
		struct Frame {
			FormatItemId Id;
			FormatItem Copy = {};
			std::vector<FormatItemId> Children = {};
			uint32_t Next = 0;
			bool Entered = false;
		};
		std::vector<Frame> Pending { { Id } };
		FormatItemId Result = NoItem;
		// 子の複写と群番号及び詳細情報の付替
		while(!Pending.empty()) {
			Frame &Current = Pending.back();
			if(!Current.Entered) {
				Current.Entered = true;
				Current.Copy = IR.Items[Current.Id];
				// 群番号と錨の複写用の変換
				FormatItem &Copy = Current.Copy;
				switch(Copy.Kind) {
				case FormatItemKind::Group:
					{
						if(const std::unordered_map<uint32_t, uint32_t>::const_iterator Found = Groups.find(Copy.Group); Found == Groups.end()) {
							if(NextGroup == NoGroup) throw FormatLimitExceeded("the format IR has too many groups", FailureReason::FormatterDefect);
							Groups.emplace(Copy.Group, NextGroup);
							Copy.Group = NextGroup++;
						} else Copy.Group = Found->second;
					}
					break;
				case FormatItemKind::AnchorMark:
					Copy.Kind = FormatItemKind::Sequence;
					Copy.Node = NoNode;
					break;
				case FormatItemKind::IfBroken:
					if(const std::unordered_map<uint32_t, uint32_t>::const_iterator Found = Groups.find(Copy.Group); Found != Groups.end()) {
						Copy.Group = Found->second;
					}
					break;
				default:
					// 写した文の終端と区切りは規則から生成した同じ種類の字句とし，出力の錨は元の要素だけが持つ
					if(Copy.Kind == FormatItemKind::Token && Copy.Node != NoNode && Copy.Generated == GeneratedToken::None) {
						if(IR.NormalizedSyntax->IsTokenRole(Copy.Node, Role::StatementTerminatorToken)) {
							Copy.Generated = GeneratedToken::StatementTerminator;
						} else if(IR.NormalizedSyntax->IsTokenRole(Copy.Node, Role::CommaToken)) Copy.Generated = GeneratedToken::ListSeparator;
					}
					Copy.Node = NoNode;
					break;
				}
				// 写しの追加で子の並びの領域が動く為，番号で読む
				Current.Children.reserve(Copy.Children.Size);
			}
			if(Current.Next < Current.Copy.Children.Size) {
				Pending.push_back({ IR.Links[Current.Copy.Children.Begin + Current.Next++] });
				continue;
			}
			Current.Copy.Children = IR.Link(Current.Children);
			// 属性は写し毎に持ち，出現の記録が元の要素へ及ばない様にする
			const uint32_t Detail = Current.Copy.Detail;
			Current.Copy.Detail = NoDetail;
			Result = IR.Add(Current.Copy);
			if(Detail != NoDetail) {
				const FormatItemDetail Original = IR.DetailOf(Detail);
				IR.EditDetail(Result) = Original;
				if(Original.ExpansionDependency != NoGroup) {
					const std::unordered_map<uint32_t, uint32_t>::const_iterator Found = Groups.find(Original.ExpansionDependency);
					if(Found == Groups.end()) {
						throw FormatLimitExceeded("a copied expansion dependency has no preceding group", FailureReason::FormatterDefect);
					}
					IR.EditDetail(Result).ExpansionDependency = Found->second;
				}
			}
			Pending.pop_back();
			if(!Pending.empty()) Pending.back().Children.push_back(Result);
		}
		// 写しの返戻
		return Result;
	}

	/**
	 * 複写した共有部分が属する出力宣言の記録関数
	 * 同じ構文節点に由来する選択を，出力上の各行へ対応付ける
	 * 計算量：辿る要素数 N に対し O(N)
	 * @param IR 中間表現
	 * @param Id 共有部分の要素
	 * @param Declaration 元の統合宣言
	 * @param Row 出力宣言の番号
	 */
	void LineBreaker::MarkDeclarationOccurrence(FormatIR &IR, const FormatItemId Id, const NodeId Declaration, const uint32_t Row) {
		struct Frame {
			FormatItemId Id;
			uint32_t Next = 0;
			bool Entered = false;
		};
		std::vector<Frame> Pending { { Id } };
		while(!Pending.empty()) {
			Frame &Current = Pending.back();
			if(!Current.Entered) {
				Current.Entered = true;
				FormatItemDetail &Detail = IR.EditDetail(Current.Id);
				Detail.DeclarationOwner = Declaration;
				Detail.DeclarationRow = Row;
			}
			const FormatRange Children = IR.Items[Current.Id].Children;
			if(Current.Next == Children.Size) {
				Pending.pop_back();
				continue;
			}
			Pending.push_back({ IR.Links[Children.Begin + Current.Next++] });
		}
		// 終了
		return;
	}

	/**
	 * １つの統合宣言の区分関数（行分割仕様 3.1 の２，4.1）
	 * 宣言子の順を保ち，共有する部分と各宣言子を平坦形で測って行幅内へ収まる最長の連続範囲を１つの宣言に纏める
	 * 必須改行又は空行を含む宣言子は其の構造を保って他の宣言子と纏めず，行幅０では分けない
	 * 計算量：当該宣言の要素数 N と測る字面の長さ T と出力区分数 R に対し O(R * (N + T))
	 * @param IR 中間表現（区分した並びへ書き換える）
	 * @param Id 区分する要素
	 * @param Partitions 確定した出力区分の格納先
	 * @param NextGroup 新しく写した共有部分の群番号の割当先
	 * @param Work 行分割の作業単位
	 * @throws FormatLimitExceeded 宣言子又は元の節点を持たない場合（中間表現の不具合）
	 */
	void LineBreaker::PartitionDeclaration(
		FormatIR &IR,
		const FormatItemId Id,
		std::vector<DeclarationPartition> &Partitions,
		uint32_t &NextGroup,
		WorkMeter &Work
	) const {
		// 統合宣言以外の要素の終了
		if(IR.Items[Id].Kind != FormatItemKind::DeclarationSplit) return;
		const NodeId Node = IR.Items[Id].Node;
		const std::span<const FormatItemId> Links = IR.ChildrenOf(IR.Items[Id]);
		const std::vector<FormatItemId> Parts(Links.begin(), Links.end());
		if(Node == NoNode || Parts.size() < 6) {
			throw FormatLimitExceeded("the format IR has an invalid declaration split", FailureReason::FormatterDefect);
		}
		const size_t Count = Parts.size() - 5;
		// 宣言子毎の名前と初期化子
		std::vector<FormatItemId> Names(Count), Values(Count);
		for(size_t Index = 0; Index < Count; ++Index) {
			const std::span<const FormatItemId> Unit = IR.ChildrenOf(IR.Items[Parts[5 + Index]]);
			if(Unit.size() != 2) {
				throw FormatLimitExceeded("the format IR has an invalid declaration split", FailureReason::FormatterDefect);
			}
			Names[Index] = Unit[0];
			Values[Index] = Unit[1];
		}
		DeclarationPartition Partition;
		Partition.Node = Node;
		// 共有する部分の文字数と，宣言子毎の名前と初期化子の文字数・必須改行の有無
		bool IsSharedBroken = false;
		const size_t Shared = FlatWidth(IR, Parts[0], IsSharedBroken, Work) + FlatWidth(IR, Parts[1], IsSharedBroken, Work) +
		FlatWidth(IR, Parts[2], IsSharedBroken, Work);
		const bool HasValues = IR.Items[Values[0]].Children.Size || IR.Items[Values[0]].Kind != FormatItemKind::Sequence;
		std::vector<size_t> Widths(Count);
		std::vector<uint8_t> IsBroken(Count);
		for(size_t Index = 0; Index < Count; ++Index) {
			bool HasBreak = false;
			Widths[Index] = FlatWidth(IR, Names[Index], HasBreak, Work) + FlatWidth(IR, Values[Index], HasBreak, Work);
			IsBroken[Index] = HasBreak;
		}
		// 中間表現が与える宣言子の間の区切りと宣言の間の必須改行
		const FormatItemId Separator = Parts[3], Break = Parts[4];
		// 宣言子を加える毎に名前の並びと初期化子の並びへ置く区切りの文字数
		bool IsSeparatorBroken = false;
		const size_t SeparatorWidth = FlatWidth(IR, Separator, IsSeparatorBroken, Work) * (HasValues ? 2 : 1);
		std::vector<FormatItemId> Rows;
		// 行幅に収まる宣言子の範囲の選択と行の組立
		for(size_t Begin = 0, End = 0; Begin < Count; Begin = End) {
			// 行幅内へ収まる最長の連続範囲
			size_t Width = Shared + Widths[Begin];
			for(End = Begin + 1; End < Count && MaxChars && !IsBroken[End - 1] && !IsBroken[End]; ++End) {
				const size_t Next = Width + Widths[End] + SeparatorWidth;
				if(Next > MaxChars) break;
				Width = Next;
			}
			if(!MaxChars) End = Count;
			Partition.Rows.push_back({ Begin, End });
			if(Partition.Rows.size() >= NoGroup) {
				throw FormatLimitExceeded("the format IR has too many declaration rows", FailureReason::FormatterDefect);
			}
			std::unordered_map<uint32_t, uint32_t> CopiedGroups;
			const auto AppendShared = [&](const FormatItemId Source) -> void {
				const FormatItemId Shared = Begin ? SharedCopy(IR, Source, CopiedGroups, NextGroup) : Source;
				MarkDeclarationOccurrence(IR, Shared, Node, static_cast<uint32_t>(Partition.Rows.size() - 1));
				Rows.push_back(Shared);
			};
			if(Begin) Rows.push_back(Break);
			AppendShared(Parts[0]);
			for(size_t Index = Begin; Index < End; ++Index) {
				if(Index > Begin) Rows.push_back(Separator);
				Rows.push_back(Names[Index]);
			}
			AppendShared(Parts[1]);
			for(size_t Index = Begin; Index < End; ++Index) {
				if(Index > Begin && HasValues) Rows.push_back(Separator);
				Rows.push_back(Values[Index]);
			}
			AppendShared(Parts[2]);
		}
		FormatItem Replaced;
		Replaced.Children = IR.Link(Rows);
		IR.Items[Id] = Replaced;
		Partitions.push_back(std::move(Partition));
		// 終了
		return;
	}

	/**
	 * 統合宣言を子から親へ区分する関数
	 * 左の部分木を終えてから右へ進み，元の子の範囲を各段で保持する
	 * 計算量：走査する要素数 N と区分処理の総費用 P に対し O(N + P)
	 * @param IR 中間表現（区分した並びへ書き換える）
	 * @param Id 区分する要素（子の統合宣言も区分する）
	 * @param Partitions 確定した出力区分の格納先
	 * @param NextGroup 新しく写した共有部分の群番号の割当先
	 * @param Work 行分割の作業単位
	 * @throws FormatLimitExceeded 宣言子又は元の節点を持たない場合（中間表現の不具合）
	 */
	void LineBreaker::PartitionDeclarations(
		FormatIR &IR,
		const FormatItemId Id,
		std::vector<DeclarationPartition> &Partitions,
		uint32_t &NextGroup,
		WorkMeter &Work
	) const {
		struct Frame {
			FormatItemId Id;
			FormatRange Children;
			uint32_t Next = 0;
		};
		std::vector<Frame> Pending { { Id, IR.Items[Id].Children } };
		while(!Pending.empty()) {
			Frame &Current = Pending.back();
			if(Current.Next < Current.Children.Size) {
				const FormatItemId Child = IR.Links[Current.Children.Begin + Current.Next++];
				Pending.push_back({ Child, IR.Items[Child].Children });
				continue;
			}
			const FormatItemId CurrentId = Current.Id;
			Pending.pop_back();
			PartitionDeclaration(IR, CurrentId, Partitions, NextGroup, Work);
		}
		// 終了
		return;
	}

	/**
	 * 中間表現の線形化関数
	 * 群を子より後に閉じて番号を振り，選択可能な境界を最も内の群へ所属させる（行分割仕様 2.1, 3.1）
	 * 計算量：辿る要素数 N と測る字面の長さ T に対し最悪 O(N² + T)
	 * @param IR 中間表現
	 * @param Id 線形化する要素
	 * @param Condition 展開時だけ出す断片の従う群（構築時の番号，無ければ無し）
	 * @param State 線形化の途中の状態
	 * @throws FormatLimitExceeded 条件付断片の入れ子，未区分の宣言，又は節点の無い構文操作が有る場合（中間表現の不具合）
	 */
	void LineBreaker::Linearize(const FormatIR &IR, const FormatItemId Id, const uint32_t Condition, Linearizer &State) {
		// 要素の入れ子を巡る作業枠の準備
		struct Frame {
			FormatItemId Id;
			uint32_t Condition;
			uint32_t Next = 0;
			bool Entered = false;
		};
		std::vector<Frame> Pending { { Id, Condition } };
		while(!Pending.empty()) {
			Frame &Current = Pending.back();
			const FormatItem &Item = IR.Items[Current.Id];
			// 要素の追加（字面を持つ要素は幅を測る）
			const auto Push = [&State, &IR, &Item, &Current](const AtomKind Kind, const NodeId Node) -> void {
				Atom Piece;
				Piece.Kind = Kind;
				Piece.Generated = Item.Generated;
				Piece.Text = Item.Text;
				Piece.Condition = Current.Condition;
				Piece.Node = Node;
				Piece.Detail = Item.Detail;
				const std::string_view Text = IR.TextOf(Item);
				if(Kind == AtomKind::Verbatim) {
					const size_t FirstEnd = Text.find('\n');
					Piece.HasNewline = FirstEnd != std::string_view::npos;
					Piece.Width = static_cast<uint32_t>(TextMetrics::CountCharacters(Text.substr(0, FirstEnd)));
					Piece.TailWidth =
					Piece.HasNewline ? static_cast<uint32_t>(TextMetrics::CountCharacters(Text.substr(Text.rfind('\n') + 1))) : Piece.Width;
				} else Piece.Width = static_cast<uint32_t>(TextMetrics::CountCharacters(Text));
				if(Kind == AtomKind::Soft && !State.Enclosing.empty()) Piece.Group = State.Enclosing.back();
				State.Atoms.push_back(Piece);
			};
			if(!Current.Entered) {
				Current.Entered = true;
				// 要素の種類に応じた原子と群の開始の記録
				switch(Item.Kind) {
				case FormatItemKind::Sequence:
					break;
				case FormatItemKind::LineEdgeSpace:
					{
						Push(AtomKind::EdgeSpace, Item.Node);
						// 行端の表現は子の字句列
						const FormatItem &Edge = IR.Items[IR.ChildrenOf(Item).front()];
						State.Atoms.back().EdgeText = Edge.Text;
						State.Atoms.back().TailWidth = static_cast<uint32_t>(TextMetrics::CountCharacters(IR.TextOf(Edge)));
					}
					break;
				case FormatItemKind::Token:
				case FormatItemKind::Space:
					Push(AtomKind::Text, Item.Node);
					break;
				case FormatItemKind::LexicalSeparator:
					Push(AtomKind::Separator, NoNode);
					break;
				case FormatItemKind::Verbatim:
					Push(AtomKind::Verbatim, Item.Node);
					break;
				case FormatItemKind::OwnedRegion:
					Push(AtomKind::OwnedRegion, Item.Node);
					break;
				case FormatItemKind::SoftBreak:
					Push(AtomKind::Soft, NoNode);
					break;
				case FormatItemKind::HardBreak:
					Push(AtomKind::Hard, NoNode);
					break;
				case FormatItemKind::BlankLine:
					Push(AtomKind::Blank, IR.DetailOf(Item.Detail).Before);
					break;
				case FormatItemKind::Group:
					if(IR.DetailOf(Item.Detail).ExpansionDependency != NoGroup) {
						State.ExpansionDependencies[Item.Group] = IR.DetailOf(Item.Detail).ExpansionDependency;
					}
					State.Enclosing.push_back(Item.Group);
					break;
				case FormatItemKind::IfBroken:
					if(Current.Condition != NoGroup) {
						throw FormatLimitExceeded("conditional fragments are nested in the format IR", FailureReason::FormatterDefect);
					}
					break;
				case FormatItemKind::IndentScope:
					Push(AtomKind::IndentBegin, NoNode);
					break;
				case FormatItemKind::AnchorMark:
					Push(AtomKind::AnchorBegin, Item.Node);
					break;
				case FormatItemKind::DeclarationSplit:
					throw FormatLimitExceeded("an unpartitioned declaration reaches line breaking", FailureReason::FormatterDefect);
				}
			}
			// 子の条件付断片の巡回
			switch(Item.Kind) {
			case FormatItemKind::Sequence:
			case FormatItemKind::Group:
			case FormatItemKind::IfBroken:
			case FormatItemKind::IndentScope:
			case FormatItemKind::AnchorMark:
				if(Current.Next < Item.Children.Size) {
					const uint32_t ChildCondition = Item.Kind == FormatItemKind::IfBroken ? Item.Group : Current.Condition;
					Pending.push_back({ IR.Links[Item.Children.Begin + Current.Next++], ChildCondition });
					continue;
				}
				break;
			}
			// 群と範囲の終了の記録
			switch(Item.Kind) {
			case FormatItemKind::Group:
				State.Enclosing.pop_back();
				State.Order[Item.Group] = State.Closed++;
				if(Item.Operation && Item.Node == NoNode) {
					throw FormatLimitExceeded("a selected form has no syntax node", FailureReason::FormatterDefect);
				}
				{
					SelectedForm &Form = State.Forms[Item.Group];
					if(Item.Node != NoNode && std::find(Form.Nodes.begin(), Form.Nodes.end(), Item.Node) == Form.Nodes.end()) {
						Form.Nodes.push_back(Item.Node);
					}
					if(Item.Operation) {
						const FormatItemDetail &Detail = IR.DetailOf(Item.Detail);
						Form.Effects.push_back({ Item.Node, *Item.Operation, Detail.DeclarationOwner, Detail.DeclarationRow });
					}
				}
				break;
			case FormatItemKind::IndentScope:
				Push(AtomKind::IndentEnd, NoNode);
				break;
			case FormatItemKind::AnchorMark:
				Push(AtomKind::AnchorEnd, Item.Node);
				break;
			}
			Pending.pop_back();
		}
		// 終了
		return;
	}

	/**
	 * 断片を測定区間へ含めるかの判定関数（行分割仕様 3.4）
	 * 評価中の群に従う断片は平坦形として除き，他の群に従う断片は此の巡の状態を用いる
	 * @param Piece 線形化した要素
	 * @param Evaluated 評価中の群
	 * @param Assumed 此の巡で展開と仮定した群
	 * @return 測定区間へ含めるなら true
	 */
	bool LineBreaker::IsVisible(const Atom &Piece, const uint32_t Evaluated, const std::vector<uint8_t> &Assumed) {
		// 条件の無い断片と，従う群の此の巡の状態の返戻
		return Piece.Condition == NoGroup || Piece.Condition != Evaluated && Assumed[Piece.Condition];
	}

	/**
	 * 測定区間の境界かの判定関数（行分割仕様 3.2）
	 * 必須改行・空行・改行を含む逐語と，評価中でない群の内で平坦と決まって居ない群の選択可能な境界を境界とする
	 * @param Piece 線形化した要素
	 * @param Evaluated 評価中の群
	 * @param States 群の状態
	 * @return 境界なら true
	 */
	bool LineBreaker::IsBoundary(const Atom &Piece, const uint32_t Evaluated, const std::vector<GroupState> &States) {
		switch(Piece.Kind) {
		case AtomKind::Hard:
		case AtomKind::Blank:
		case AtomKind::OwnedRegion:
			// 必須改行と空行と行を跨ぐ所有区画の返戻
			return true;
		case AtomKind::Verbatim:
			// 改行を含む逐語の返戻
			return Piece.HasNewline;
		case AtomKind::Soft:
			// 未評価又は展開と決まった他の群の境界の返戻
			return Piece.Group != NoGroup && Piece.Group != Evaluated && States[Piece.Group] != GroupState::Flat;
		default:
			// 幅を持たない印と字面の返戻
			return false;
		}
	}

	/**
	 * 表示上の空白が行端へ接するかの判定関数
	 * @param Atoms 線形化した要素
	 * @param Position 空白の位置
	 * @param Direction 探索方向（前は負１，後は１）
	 * @param Evaluated 平坦化を試す群（出力時は無し）
	 * @param States 群の選択
	 * @param Assumed 条件付断片の選択
	 * @param Trace 参照した位置の区間（省略時は記録しない）
	 * @return 最寄の可視の境界が改行か出力の端なら true
	 */
	bool LineBreaker::IsLineEdge(
		const std::vector<Atom> &Atoms,
		const size_t Position,
		const int Direction,
		const uint32_t Evaluated,
		const std::vector<GroupState> &States,
		const std::vector<uint8_t> &Assumed,
		EvaluationTrace *const Trace
	) {
		for(
			std::ptrdiff_t Index = static_cast<std::ptrdiff_t>(Position) + Direction;
			Index >= 0 && Index < static_cast<std::ptrdiff_t>(Atoms.size());
			Index += Direction
		) {
			if(Trace) {
				Trace->Begin = std::min(Trace->Begin, static_cast<size_t>(Index));
				Trace->End = std::max(Trace->End, static_cast<size_t>(Index));
			}
			const Atom &Neighbor = Atoms[static_cast<size_t>(Index)];
			if(!IsVisible(Neighbor, Evaluated, Assumed)) continue;
			if(IsBoundary(Neighbor, Evaluated, States)) {
				// 行の端に達した事の返戻
				return Neighbor.Kind != AtomKind::Verbatim || !(Direction < 0 ? Neighbor.TailWidth : Neighbor.Width);
			}
			// 同じ行の内容へ接する事の返戻
			if(
				(
					Neighbor.Kind == AtomKind::Text || Neighbor.Kind == AtomKind::Verbatim || Neighbor.Kind == AtomKind::EdgeSpace ||
					Neighbor.Kind == AtomKind::Soft || Neighbor.Kind == AtomKind::Separator
				) && Neighbor.Width
			) return false;
		}
		// 出力の端の返戻
		return true;
	}

	/**
	 * 行分割の作業単位の計上関数（整形仕様 9.2）
	 * @param Amount 計上する作業単位
	 * @throws FormatLimitExceeded 初回評価と再評価の合計が上限を超える場合（規定した資源上限の見送）
	 */
	void LineBreaker::WorkMeter::Charge(const size_t Amount) {
		// 上限超過の見送
		if(Amount > Limit - Used) throw FormatLimitExceeded("line wrapping exceeds formatting limit");
		Used += Amount;
		// 終了
		return;
	}

	/**
	 * 境界の採否の更新関数
	 * 計算量：要素数 N に対し O(log N)
	 * @param Position 境界の位置
	 * @param Counted 境界として数えるか
	 */
	void LineBreaker::BoundaryCounter::Set(const size_t Position, const bool Counted) {
		// 採否が同じ場合の終了
		if(IsCounted[Position] == Counted) return;
		IsCounted[Position] = Counted;
		for(size_t Index = Position + 1; Index < Tree.size(); Index += Index & ~Index + 1) {
			if(Counted) ++Tree[Index];
			else --Tree[Index];
		}
		// 終了
		return;
	}

	/**
	 * 位置より前の境界の数の取得関数
	 * 計算量：要素数 N に対し O(log N)
	 * @param End 数える範囲の終わり（此の位置を含まない）
	 * @return 先頭から位置の手前迄の境界の数
	 */
	uint32_t LineBreaker::BoundaryCounter::CountBefore(const size_t End) const {
		uint32_t Count = 0;
		for(size_t Index = End; Index; Index -= Index & ~Index + 1) Count += Tree[Index];
		// 境界の数の返戻
		return Count;
	}

	/**
	 * 指定した順位の境界の位置の取得関数
	 * 計算量：要素数 N に対し O(log N)
	 * @param Rank 先頭を０とする境界の順位
	 * @return 境界の位置
	 */
	size_t LineBreaker::BoundaryCounter::Select(const uint32_t Rank) const {
		size_t Position = 0, Step = 1;
		while(Step <= Tree.size() >> 1) Step <<= 1;
		uint32_t Remaining = Rank + 1;
		for(; Step; Step >>= 1) if(Position + Step < Tree.size() && Tree[Position + Step] < Remaining) {
			Remaining -= Tree[Position + Step];
			Position += Step;
		}
		// 境界の位置の返戻
		return Position;
	}

	/**
	 * コンストラクタ（此の巡で見える境界を数える）
	 * 必須改行・空行・改行を含む逐語と，群に属する選択可能な境界の内，此の巡で見えて平坦と決まって居ない物を数える
	 * 計算量：要素数 N に対し O(N)
	 * @param Atoms 線形化した要素
	 * @param Assumed 此の巡で展開と仮定した群
	 * @param States 群の状態
	 */
	LineBreaker::BoundaryCounter::BoundaryCounter(
		const std::vector<Atom> &Atoms,
		const std::vector<uint8_t> &Assumed,
		const std::vector<GroupState> &States
	) : Tree(Atoms.size() + 1, 0), IsCounted(Atoms.size(), 0) {
		for(size_t Position = 0; Position < Atoms.size(); ++Position) {
			const Atom &Piece = Atoms[Position];
			IsCounted[Position] = IsVisible(Piece, NoGroup, Assumed) && IsBoundary(Piece, NoGroup, States);
			Tree[Position + 1] += IsCounted[Position];
			// 線形の構築（親の節へ部分和を送る）
			if(const size_t Parent = Position + 1 + (Position + 1 & ~Position); Parent < Tree.size()) Tree[Parent] += Tree[Position + 1];
		}
		// 終了
		return;
	}

	/**
	 * 可視の字句の幅と残存境界の索引の構築関数
	 * 計算量：要素数 N に対し O(N)
	 * @param Atoms 線形化した要素
	 * @param Assumed 此の巡で展開と仮定した群
	 * @param States 群の状態
	 */
	LineBreaker::MeasureIndex::MeasureIndex(
		const std::vector<Atom> &Atoms,
		const std::vector<uint8_t> &Assumed,
		const std::vector<GroupState> &States
	) : Boundaries(Atoms, Assumed, States), Tree(Atoms.size() + 1, 0), Values(Atoms.size(), 0) {
		for(size_t Position = 0; Position < Atoms.size(); ++Position) {
			const Atom &Piece = Atoms[Position];
			const bool HasWidth = Piece.Kind == AtomKind::Text || Piece.Kind == AtomKind::Separator || Piece.Kind == AtomKind::Soft ||
			Piece.Kind == AtomKind::Verbatim;
			Values[Position] = IsVisible(Piece, NoGroup, Assumed) && HasWidth ? Piece.Width : 0;
			Tree[Position + 1] += Values[Position];
			if(const size_t Parent = Position + 1 + (Position + 1 & ~Position); Parent < Tree.size()) Tree[Parent] += Tree[Position + 1];
		}
		// 終了
		return;
	}

	/**
	 * 要素の可視性と境界の採否の更新関数
	 * 計算量：要素数 N に対し O(log N)
	 * @param Atoms 線形化した要素
	 * @param Position 更新する要素の位置
	 * @param Visible 要素を表示するか
	 * @param Boundary 境界を残すか
	 */
	void LineBreaker::MeasureIndex::Set(
		const std::vector<Atom> &Atoms,
		const size_t Position,
		const bool Visible,
		const bool Boundary
	) {
		const Atom &Piece = Atoms[Position];
		const bool HasWidth = Piece.Kind == AtomKind::Text || Piece.Kind == AtomKind::Separator || Piece.Kind == AtomKind::Soft ||
		Piece.Kind == AtomKind::Verbatim;
		if(const uint32_t Width = Visible && HasWidth ? Piece.Width : 0; Values[Position] != Width) {
			for(size_t At = Position + 1; At < Tree.size(); At += At & ~At + 1) {
				if(Width > Values[Position]) Tree[At] += Width - Values[Position];
				else Tree[At] -= Values[Position] - Width;
			}
			Values[Position] = Width;
		}
		Boundaries.Set(Position, Visible && Boundary);
		// 終了
		return;
	}

	/**
	 * 位置より前の可視字句の幅の取得関数
	 * 計算量：要素数 N に対し O(log N)
	 * @param End 測る範囲の終わり
	 * @return 先頭から位置の手前迄の幅
	 */
	size_t LineBreaker::MeasureIndex::WidthBefore(const size_t End) const {
		size_t Width = 0;
		for(size_t At = End; At; At -= At & ~At + 1) Width += Tree[At];
		// 幅の返戻
		return Width;
	}

	/**
	 * 群の全候補を除けるかの評価関数（行分割仕様 3.3）
	 * 全候補を一時的に除き，候補が同じ測定区間へ収まって幅が上限以下なら除ける
	 * 計算量：要素数 N と候補・条件付断片の数 K に対し索引が有れば O((K + 1) log N)，無ければ O(N)
	 * @param Atoms 線形化した要素
	 * @param Candidates 評価する群の選択可能な境界の位置
	 * @param Conditionals 評価する群に従う断片の位置（索引又は区間和から除く）
	 * @param Evaluated 評価する群
	 * @param States 群の状態
	 * @param Assumed 此の巡で展開と仮定した群
	 * @param Counter 此の巡の状態の境界の数の区間和（無ければ候補の間を走査する）
	 * @param Index 可視字句の幅と境界の索引（行端空白が有る場合は無し）
	 * @param Work 行分割の作業単位（評価と走査した要素を計上する）
	 * @param Trace 参照した幅と境界の記録先（省略時は記録しない）
	 * @return 群の折返を除けるなら true
	 */
	bool LineBreaker::CanFlatten(
		const std::vector<Atom> &Atoms,
		const std::vector<size_t> &Candidates,
		const std::span<const size_t> Conditionals,
		const uint32_t Evaluated,
		const std::vector<GroupState> &States,
		const std::vector<uint8_t> &Assumed,
		const BoundaryCounter *const Counter,
		MeasureIndex *const Index,
		WorkMeter &Work,
		EvaluationTrace *const Trace
	) const {
		// 群の１回の評価を１作業単位とする（整形仕様 9.2）
		Work.Charge(1);
		++Work.Evaluations;
		// 候補の無い群の返戻
		if(Candidates.empty()) return true;
		const size_t First = Candidates.front(), Last = Candidates.back();
		if(Trace) *Trace = { First, Last };
		// 幅と境界の索引による候補の一時除去と区間の測定
		if(Index) {
			// 評価中の群の条件付断片と候補だけを一時的に境界から除く
			for(const size_t Position : Conditionals) Index->Set(Atoms, Position, false, false);
			for(const size_t Position : Candidates) Index->Set(Atoms, Position, IsVisible(Atoms[Position], Evaluated, Assumed), false);
			Work.Charge(Candidates.size() + Conditionals.size());
			const BoundaryCounter &Boundaries = Index->Boundaries;
			const uint32_t BeforeFirst = Boundaries.CountBefore(First), BeforeLast = Boundaries.CountBefore(Last + 1);
			bool Fits = BeforeFirst == BeforeLast;
			Work.Charge(2);
			if(Fits && MaxChars) {
				const size_t Left = BeforeFirst ? Boundaries.Select(BeforeFirst - 1) : Atoms.size();
				if(BeforeFirst) Work.Charge(1);
				const uint32_t Total = Boundaries.CountBefore(Atoms.size());
				Work.Charge(1);
				const size_t Right = BeforeLast < Total ? Boundaries.Select(BeforeLast) : Atoms.size();
				if(Trace) {
					Trace->Begin = Left == Atoms.size() ? 0 : Left;
					Trace->End = Right == Atoms.size() ? Atoms.size() - 1 : Right;
				}
				if(BeforeLast < Total) Work.Charge(1);
				size_t Width = Index->WidthBefore(Right) - Index->WidthBefore(Left == Atoms.size() ? 0 : Left + 1);
				Work.Charge(2);
				if(Left != Atoms.size() && Atoms[Left].Kind == AtomKind::Verbatim) Width += Atoms[Left].TailWidth;
				if(Right != Atoms.size() && Atoms[Right].Kind == AtomKind::Verbatim) Width += Atoms[Right].Width;
				Fits = Width <= MaxChars;
			}
			for(const size_t Position : Conditionals) {
				const Atom &Piece = Atoms[Position];
				Index->Set(Atoms, Position, IsVisible(Piece, NoGroup, Assumed), IsBoundary(Piece, NoGroup, States));
			}
			for(const size_t Position : Candidates) {
				const Atom &Piece = Atoms[Position];
				Index->Set(Atoms, Position, IsVisible(Piece, NoGroup, Assumed), IsBoundary(Piece, NoGroup, States));
			}
			Work.Charge(Candidates.size() + Conditionals.size());
			// 全候補を除いた区間が幅内に収まる事の返戻
			return Fits;
		}
		// 境界の索引又は直接走査による候補間の確認
		if(Counter) {
			// 候補の間の境界の数（評価する群の候補と，評価する群に従い平坦形では見えない断片を除く）
			uint32_t Inner = Last > First ? Counter->CountBefore(Last) - Counter->CountBefore(First + 1) : 0;
			for(size_t Index = 1; Index + 1 < Candidates.size(); ++Index) Inner -= Counter->IsCounted[Candidates[Index]];
			for(const size_t Index : Conditionals) if(
				const bool IsOwnCandidate = Atoms[Index].Kind == AtomKind::Soft && Atoms[Index].Group == Evaluated;
				First < Index && Index < Last && !IsOwnCandidate
			) Inner -= Counter->IsCounted[Index];
			// 区間和の参照と除いた要素の計上
			Work.Charge(Candidates.size() + Conditionals.size());
			// 候補が複数の測定区間へ分かれる事の返戻
			if(Inner) return false;
		} else for(size_t Index = First + 1; Index < Last; ++Index) {
			Work.Charge(1);
			const Atom &Piece = Atoms[Index];
			// 候補が複数の測定区間へ分かれる事の返戻
			if(IsVisible(Piece, Evaluated, Assumed) && IsBoundary(Piece, Evaluated, States)) return false;
		}
		// 行幅０の平坦形の返戻
		if(!MaxChars) return true;
		size_t Width = 0;
		// 境界でない要素の文字数（字面・平坦な境界・１行の逐語）
		const auto Contribution = [&](const Atom &Piece) -> size_t {
			if(Piece.Kind == AtomKind::EdgeSpace) {
				const size_t Position = static_cast<size_t>(&Piece - Atoms.data());
				// 行端で選ぶ空白の式を含めた幅の返戻
				return IsLineEdge(Atoms, Position, -1, Evaluated, States, Assumed, Trace) ||
				IsLineEdge(Atoms, Position, 1, Evaluated, States, Assumed, Trace) ? Piece.TailWidth : Piece.Width;
			}
			// 幅を持つ要素の文字数の返戻
			return Piece.Kind == AtomKind::Text || Piece.Kind == AtomKind::Separator || Piece.Kind == AtomKind::Soft ||
			Piece.Kind == AtomKind::Verbatim || Piece.Kind == AtomKind::EdgeSpace ? Piece.Width : 0;
		};
		for(size_t Index = First; Index <= Last && Width <= MaxChars; ++Index) {
			if(Trace) Trace->End = std::max(Trace->End, Index);
			Work.Charge(1);
			if(IsVisible(Atoms[Index], Evaluated, Assumed)) Width += Contribution(Atoms[Index]);
		}
		// 左の境界迄の文字数
		for(size_t Index = First; Index-- > 0 && Width <= MaxChars;) {
			if(Trace) Trace->Begin = std::min(Trace->Begin, Index);
			Work.Charge(1);
			const Atom &Piece = Atoms[Index];
			if(!IsVisible(Piece, Evaluated, Assumed)) continue;
			if(IsBoundary(Piece, Evaluated, States)) {
				if(Piece.Kind == AtomKind::Verbatim) Width += Piece.TailWidth;
				break;
			}
			Width += Contribution(Piece);
		}
		// 右の境界迄の文字数
		for(size_t Index = Last + 1; Index < Atoms.size() && Width <= MaxChars; ++Index) {
			if(Trace) Trace->End = std::max(Trace->End, Index);
			Work.Charge(1);
			const Atom &Piece = Atoms[Index];
			if(!IsVisible(Piece, Evaluated, Assumed)) continue;
			if(IsBoundary(Piece, Evaluated, States)) {
				if(Piece.Kind == AtomKind::Verbatim) Width += Piece.Width;
				break;
			}
			Width += Contribution(Piece);
		}
		// 上限以下なら除ける事の返戻（上限と等しい場合は折らない）
		return Width <= MaxChars;
	}

	/**
	 * 制御本体の同行形・見出の後の包装形・見出を折った同行形を此の順で選ぶ関数
	 * 試行する群の境界と条件付の波括弧を同じ状態へ揃えてから測る
	 * 計算量：要素数 N，見出の数 H と試行中に参照する候補・条件付断片の累計 K に対し索引が有れば O((H + K) log N)
	 * 索引が無い場合は要素数 N に比例する測定区間の走査を各試行で行う
	 * @param Atoms 線形化した要素
	 * @param Candidates 群毎の選択可能な境界
	 * @param ConditionalsOf 群毎の条件付断片
	 * @param Choice 本体と見出の群
	 * @param States 此の巡に選んだ群の状態（選択時に更新する）
	 * @param Conditions 此の巡に採用する条件付断片（選択時に更新する）
	 * @param Index 可視字句の幅と境界の索引
	 * @param Work 行分割の作業単位
	 */
	void LineBreaker::SelectBraceBody(
		const std::vector<Atom> &Atoms,
		const std::vector<std::vector<size_t>> &Candidates,
		const std::vector<std::vector<size_t>> &ConditionalsOf,
		const BraceChoice &Choice,
		std::vector<GroupState> &States,
		std::vector<uint8_t> &Conditions,
		MeasureIndex *const Index,
		WorkMeter &Work
	) const {
		// 見出の群の無い選択と行幅の無制限の場合の終了
		if(Choice.Headers.empty() || !MaxChars) return;
		// 本体と見出の元の選択の保存
		const GroupState OriginalBody = States[Choice.Body];
		std::vector<GroupState> OriginalHeaders;
		OriginalHeaders.reserve(Choice.Headers.size());
		for(const uint32_t Header : Choice.Headers) OriginalHeaders.push_back(States[Header]);
		// 群の選択と幅の索引の同期
		const auto Set =
		[&Atoms, &Candidates, &ConditionalsOf, &States, &Conditions, Index, &Work](const uint32_t Group, const GroupState Value)
		-> void {
			States[Group] = Value;
			Conditions[Group] = Value == GroupState::Broken;
			if(Index) {
				for(const size_t Position : Candidates[Group]) {
					const Atom &Piece = Atoms[Position];
					Index->Set(Atoms, Position, IsVisible(Piece, NoGroup, Conditions), IsBoundary(Piece, NoGroup, States));
				}
				for(const size_t Position : ConditionalsOf[Group]) {
					const Atom &Piece = Atoms[Position];
					Index->Set(Atoms, Position, IsVisible(Piece, NoGroup, Conditions), IsBoundary(Piece, NoGroup, States));
				}
				Work.Charge(Candidates[Group].size() + ConditionalsOf[Group].size());
			}
		};
		// 試行前の選択の復元
		const auto Restore = [&]() -> void {
			Set(Choice.Body, OriginalBody);
			for(size_t Index = 0; Index < Choice.Headers.size(); ++Index) Set(Choice.Headers[Index], OriginalHeaders[Index]);
		};
		// 本体と見出の選択の幅との整合判定
		const auto IsConsistent = [&]() -> bool {
			for(const uint32_t Header : Choice.Headers) {
				const bool IsFlat =
				CanFlatten(Atoms, Candidates[Header], ConditionalsOf[Header], Header, States, Conditions, nullptr, Index, Work);
				// 見出の選択と実際に幅へ収まる形が食違う場合の返戻
				if(States[Header] == GroupState::Flat != IsFlat) return false;
			}
			// 本体の同行形の選択が，見出を折った時に同じ測定区間で同行に収まるかと一致するかの返戻
			return States[Choice.Body] == GroupState::Flat ==
			CanFlatten(Atoms, Candidates[Choice.Body], ConditionalsOf[Choice.Body], Choice.Body, States, Conditions, nullptr, Index, Work);
		};
		// 本体と見出の組合せの試行
		const auto Select = [&](const GroupState Body, const bool IsHeaderFlat) -> bool {
			Restore();
			Set(Choice.Body, Body);
			for(const uint32_t Header : Choice.Headers) Set(Header, IsHeaderFlat ? GroupState::Flat : GroupState::Pending);
			if(!IsHeaderFlat) for(const uint32_t Header : Choice.Headers) {
				Set(
					Header,
					CanFlatten(Atoms, Candidates[Header], ConditionalsOf[Header], Header, States, Conditions, nullptr, Index, Work) ?
					GroupState::Flat :
					GroupState::Broken
				);
			}
			// 本体と見出を組合せた選択の整合性の返戻
			if(!IsConsistent()) return false;
			// 此の組合せを採用出来る事の返戻
			return true;
		};
		// 同行形を選べた場合の終了
		if(Select(GroupState::Flat, true)) return;
		// 見出に波括弧を続ける形を選べた場合の終了
		if(Select(GroupState::Broken, true)) return;
		// 見出を折った同行形を選べた場合の終了
		if(Select(GroupState::Flat, false)) return;
		// 両方の展開形を選べなければ元の状態へ戻す
		if(!Select(GroupState::Broken, false)) Restore();
		// 終了
		return;
	}

	/**
	 * 選択の解決関数（整形仕様 6.4，行分割仕様第３章）
	 * 統合宣言を先に区分し，群を後行順に評価し，条件付の断片の状態が１巡前と一致する迄巡り直す
	 * 計算量：要素数 N，巡の数 R，実際の再評価回数 E と走査する候補・条件付断片の累計 K に対し O(R * N log N + (E + K) log N)
	 * 行端空白を含む場合の幅測定は測定区間の延べ要素数に比例する
	 * @param IR 幅非依存の中間表現（統合宣言を区分した形へ書き換える写し）
	 * @param ShouldReuse 依存関係に依る評価結果と索引を再利用するか
	 * @param Measured 実際の作業量の記録先（不要なら nullptr）
	 * @param FingerprintMask 比較試験で衝突を強制する要約値の印（製品では全ビット）
	 * @return 全ての選択を解決した中間表現
	 * @throws FormatLimitExceeded 中間表現が群の無い断片を参照する場合と，巡が以前の状態へ戻り収束しない場合（整形器の不具合），作業単位が上限を超える場合（資源上限の見送）
	 */
	BrokenIR LineBreaker::ResolveWithMode(
		FormatIR IR,
		const bool ShouldReuse,
		WorkMeter *const Measured,
		const size_t FingerprintMask
	) const {
		// 初回評価と再評価の作業単位（上限は正規化と同じ入力の基準量から定めた値）
		WorkMeter Work { 0, IR.NormalizedSyntax ? IR.NormalizedSyntax->GetWorkLimit() : std::numeric_limits<size_t>::max() };
		// 区分後に写す共有部分へ，既存の群と重ならない番号を割り当てる
		uint32_t NextGroup = 0;
		std::vector<FormatItemId> Pending { IR.Root };
		while(!Pending.empty()) {
			const FormatItem &Current = IR.Items[Pending.back()];
			Pending.pop_back();
			if(Current.Kind == FormatItemKind::Group) {
				if(Current.Group == NoGroup) {
					throw FormatLimitExceeded("the format IR has a group without a number", FailureReason::FormatterDefect);
				}
				NextGroup = std::max(NextGroup, Current.Group + 1);
			}
			const std::span<const FormatItemId> Children = IR.ChildrenOf(Current);
			Pending.insert(Pending.end(), Children.begin(), Children.end());
		}
		std::vector<DeclarationPartition> Partitions;
		PartitionDeclarations(IR, IR.Root, Partitions, NextGroup, Work);
		Linearizer State;
		// 線形化は要素毎に高々２つの原子を作る為，其の上限を予約して伸長の度の再確保を避ける
		State.Atoms.reserve(IR.Items.size() << 1);
		Linearize(IR, IR.Root, NoGroup, State);
		// 線形化した要素と子の並びは以後使わない為，評価の間の記憶域を空ける
		IR.Items = std::vector<FormatItem>();
		IR.Links = std::vector<FormatItemId>();
		std::vector<Atom> &Atoms = State.Atoms;
		const uint32_t GroupCount = static_cast<uint32_t>(State.Order.size());
		// 閉じの順を詰めた後行順の番号（共有する群は全ての要素の中の群より後に評価する）
		std::vector<std::pair<uint32_t, uint32_t>> ByClose;
		ByClose.reserve(GroupCount);
		for(const auto &[Group, Close] : State.Order) ByClose.emplace_back(Close, Group);
		std::sort(ByClose.begin(), ByClose.end());
		for(uint32_t Rank = 0; Rank < GroupCount; ++Rank) State.Order[ByClose[Rank].second] = Rank;
		// 群の番号を後行順へ振り直し，群毎の候補を集める
		std::vector<std::vector<size_t>> Candidates(GroupCount);
		// 群毎の従う断片の位置
		std::vector<std::vector<size_t>> ConditionalsOf(GroupCount);
		std::vector<size_t> VariablePositions;
		bool HasConditions = false;
		for(size_t Index = 0; Index < Atoms.size(); ++Index) {
			Atom &Piece = Atoms[Index];
			for(uint32_t *const Reference : { &Piece.Group, &Piece.Condition }) {
				if(*Reference == NoGroup) continue;
				const std::unordered_map<uint32_t, uint32_t>::const_iterator Found = State.Order.find(*Reference);
				if(Found == State.Order.end()) {
					throw FormatLimitExceeded("the format IR refers to an unknown group", FailureReason::FormatterDefect);
				}
				*Reference = Found->second;
			}
			HasConditions = HasConditions || Piece.Condition != NoGroup;
			if(Piece.Kind == AtomKind::Soft && Piece.Group != NoGroup) Candidates[Piece.Group].push_back(Index);
			if(Piece.Condition != NoGroup) ConditionalsOf[Piece.Condition].push_back(Index);
			if(Piece.Group != NoGroup || Piece.Condition != NoGroup) VariablePositions.push_back(Index);
		}
		// 本体の群を構文上の親と錨の出力区間で見出の分割群へ結び付ける
		struct BodyPosition {
			uint32_t Group;
			NodeId Parent;
			size_t Open;
			size_t End;
		};
		// 錨の出力区間は波括弧を選ぶ本体と其の構文上の親だけを控える（出力に現れない錨は未配置の儘）
		constexpr size_t Unplaced = std::numeric_limits<size_t>::max();
		std::unordered_map<NodeId, std::pair<size_t, size_t>> Anchors;
		if(IR.NormalizedSyntax) for(const auto &[Group, Form] : State.Forms) for(const SelectedFormEffect &Effect : Form.Effects) {
			if(Effect.Operation != FormOperation::BraceBody) continue;
			for(const NodeId Node : { Effect.Node, IR.NormalizedSyntax->ParentOf(Effect.Node) }) {
				Anchors.try_emplace(Node, Unplaced, Unplaced);
			}
		}
		if(!Anchors.empty()) for(size_t Index = 0; Index < Atoms.size(); ++Index) {
			const Atom &Piece = Atoms[Index];
			const bool IsAnchor =
			Piece.Kind == AtomKind::AnchorBegin || Piece.Kind == AtomKind::AnchorEnd || Piece.Kind == AtomKind::Text ||
			Piece.Kind == AtomKind::Verbatim || Piece.Kind == AtomKind::OwnedRegion;
			if(!IsAnchor || Piece.Node == NoNode) continue;
			const std::unordered_map<NodeId, std::pair<size_t, size_t>>::iterator Found = Anchors.find(Piece.Node);
			if(Found == Anchors.end()) continue;
			if(Piece.Kind == AtomKind::AnchorBegin) Found->second.first = Index;
			else if(Piece.Kind == AtomKind::AnchorEnd) Found->second.second = Index;
			else Found->second = { Index, Index };
		}
		std::vector<uint8_t> IsBraceGroup(GroupCount, 0);
		// 波括弧を選ぶ本体の群と出力区間の対応付け
		std::vector<BodyPosition> Bodies;
		if(IR.NormalizedSyntax) for(uint32_t Group = 0; Group < GroupCount; ++Group) {
			for(const SelectedFormEffect &Effect : State.Forms.at(ByClose[Group].second).Effects) {
				if(Effect.Operation != FormOperation::BraceBody) continue;
				IsBraceGroup[Group] = 1;
				const NodeId Parent = IR.NormalizedSyntax->ParentOf(Effect.Node);
				const std::unordered_map<NodeId, std::pair<size_t, size_t>>::const_iterator ParentRange = Anchors.find(Parent);
				const std::unordered_map<NodeId, std::pair<size_t, size_t>>::const_iterator BodyRange = Anchors.find(Effect.Node);
				if(ParentRange->second.first == Unplaced || BodyRange->second.first == Unplaced) continue;
				const size_t Start = ParentRange->second.first, BodyStart = BodyRange->second.first;
				for(size_t Index = BodyStart; Index-- > Start;) if(
					Atoms[Index].Condition == Group && Atoms[Index].Kind == AtomKind::Text && (
						Atoms[Index].Generated == GeneratedToken::BlockOpen ||
						Atoms[Index].Node != NoNode && IR.NormalizedSyntax->Is(Atoms[Index].Node, Role::OpeningBraceToken)
					)
				) {
					Bodies.push_back({ Group, Parent, Index, BodyRange->second.second });
					break;
				}
			}
		}
		// 本体の手前に在る見出の群の探索
		std::vector<std::pair<size_t, uint32_t>> HeaderStarts;
		for(uint32_t Group = 0; Group < GroupCount; ++Group) {
			if(!IsBraceGroup[Group] && !Candidates[Group].empty()) HeaderStarts.emplace_back(Candidates[Group].front(), Group);
		}
		std::sort(HeaderStarts.begin(), HeaderStarts.end());
		std::sort(
			Bodies.begin(),
			Bodies.end(),
			[](const BodyPosition &Left, const BodyPosition &Right) -> bool {
				return Left.Open < Right.Open;
			}
		);
		// 各本体を其の手前の見出の群へ対応付ける
		std::unordered_map<NodeId, size_t> LastBodyEnd;
		std::vector<BraceChoice> ByBody(GroupCount);
		for(const BodyPosition &Body : Bodies) {
			const size_t ParentStart = Anchors.at(Body.Parent).first;
			const std::unordered_map<NodeId, size_t>::const_iterator Previous = LastBodyEnd.find(Body.Parent);
			const size_t HeaderStart = Previous == LastBodyEnd.end() ? ParentStart : std::max(ParentStart, Previous->second + 1);
			std::vector<uint32_t> &Headers = ByBody[Body.Group].Headers;
			for(
				std::vector<std::pair<size_t, uint32_t>>::const_iterator Found =
				std::lower_bound(HeaderStarts.cbegin(), HeaderStarts.cend(), std::pair<size_t, uint32_t>{ HeaderStart, 0 });
				Found != HeaderStarts.cend() && Found->first < Body.Open;
				++Found
			) if(const uint32_t Header = Found->second; Header < Body.Group && Candidates[Header].back() < Body.Open) {
				Headers.push_back(Header);
			}
			LastBodyEnd.insert_or_assign(Body.Parent, Body.End);
		}
		std::vector<BraceChoice> BraceChoices;
		for(uint32_t Group = 0; Group < GroupCount; ++Group) {
			std::vector<uint32_t> &Headers = ByBody[Group].Headers;
			std::sort(Headers.begin(), Headers.end());
			Headers.erase(std::unique(Headers.begin(), Headers.end()), Headers.end());
			if(Headers.empty()) continue;
			ByBody[Group].Body = Group;
			BraceChoices.push_back(std::move(ByBody[Group]));
		}
		// 展開依存先と幅測定の方式の決定
		std::vector<uint32_t> Dependencies(GroupCount, NoGroup);
		for(const auto &[Group, Dependency] : State.ExpansionDependencies) {
			Dependencies[State.Order.at(Group)] = State.Order.at(Dependency);
		}
		bool UseScan = !MaxChars;
		if(!UseScan) for(const Atom &Piece : Atoms) {
			Work.Charge(1);
			if(Piece.Kind == AtomKind::EdgeSpace) {
				UseScan = true;
				break;
			}
		}
		// 巡毎の評価（条件付の断片の状態を固定し，全ての群を未評価から後行順に評価する）
		std::vector<uint8_t> Assumed(GroupCount, 0);
		// 巡毎の状態の控え（前の巡から状態の変わった群と，展開した群の要約値）
		// （状態は比べる時だけ差分から組み立てる）
		struct RoundState {
			std::vector<uint32_t> Flipped; // 前の巡から状態の変わった群
			size_t Hash; // 展開した群の要約値
		};
		// 群の状態の要約値の計算
		const auto Fingerprint = [FingerprintMask](const uint32_t Group) -> size_t {
			size_t Value = static_cast<size_t>(Group) + 1;
			Value ^= Value >> 16;
			Value *= static_cast<size_t>(0X7FEB352D);
			Value ^= Value >> 15;
			Value *= static_cast<size_t>(0X846CA68B);
			Value ^= Value >> 16;
			// 群の状態を要約する値の返戻
			return Value & FingerprintMask;
		};
		std::vector<RoundState> Rounds { { {}, 0 } };
		std::unordered_multimap<size_t, size_t> RoundsByHash { { 0, 0 } };
		std::vector<GroupState> States(GroupCount, GroupState::Pending);
		std::vector<EvaluationRecord> Records(GroupCount);
		std::vector<uint32_t> ChangedAssumptions;
		std::vector<uint8_t> ChangedFlags(GroupCount, 0);
		std::optional<BoundaryCounter> Counter;
		std::optional<MeasureIndex> Index;
		while(true) {
			// 巡の開始時の状態と測定索引の準備
			if(!ShouldReuse) std::fill(States.begin(), States.end(), GroupState::Pending);
			if(ShouldReuse && !Counter && !Index) {
				Work.Charge(Atoms.size());
				if(!UseScan) Work.Charge(Atoms.size());
				if(UseScan) Counter.emplace(Atoms, Assumed, States);
				else Index.emplace(Atoms, Assumed, States);
			}
			// 残存境界と幅の要約は参照区間内の要素から決まり，区間外の変更では変わらない
			// 自身の条件付断片は評価時に除く為，変更位置の総数から其の群の位置を差し引く
			std::vector<size_t> ChangedConditionalPositions;
			for(const uint32_t Group : ChangedAssumptions) {
				Work.Charge(1);
				for(const size_t Position : ConditionalsOf[Group]) {
					ChangedConditionalPositions.push_back(Position);
					Work.Charge(1);
				}
			}
			const auto ChargingLess = [&Work](const auto Left, const auto Right) -> bool {
				Work.Charge(1);
				return Left < Right;
			};
			std::sort(ChangedConditionalPositions.begin(), ChangedConditionalPositions.end(), ChargingLess);
			// 変更位置の二分探索
			const auto CountBefore = [&Work, &ChargingLess](const std::vector<size_t> &Positions, const size_t Limit) -> size_t {
				Work.Charge(1);
				// 指定位置より前の断片の数の返戻
				return static_cast<size_t>(std::ranges::lower_bound(Positions, Limit, ChargingLess) - Positions.begin());
			};
			// 前の巡の変更から再評価する群の選択
			std::set<uint32_t, decltype(ChargingLess)> WorkQueue(ChargingLess);
			if(!ShouldReuse) for(uint32_t Group = 0; Group < GroupCount; ++Group) {
				Work.Charge(1);
				WorkQueue.insert(Group);
			} else for(uint32_t Group = 0; Group < GroupCount; ++Group) {
				const EvaluationRecord &Record = Records[Group];
				Work.Charge(1);
				if(Record.IsValid && (Candidates[Group].empty() || Record.FirstVariable == Record.EndVariable)) continue;
				if(!Record.IsValid) {
					WorkQueue.insert(Group);
					continue;
				}
				const size_t ChangedCount =
				CountBefore(ChangedConditionalPositions, Record.Trace.End + 1) - CountBefore(ChangedConditionalPositions, Record.Trace.Begin);
				const size_t OwnCount = ChangedFlags[Group] ?
				CountBefore(ConditionalsOf[Group], Record.Trace.End + 1) - CountBefore(ConditionalsOf[Group], Record.Trace.Begin) :
				0;
				if(ChangedCount > OwnCount) WorkQueue.insert(Group);
			}
			// 後行順の群の評価と残存境界の更新
			std::set<size_t, decltype(ChargingLess)> ChangedPositions(ChargingLess);
			for(uint32_t Group = 0; Group < GroupCount; ++Group) {
				EvaluationRecord &Record = Records[Group];
				const std::set<size_t, decltype(ChargingLess)>::iterator Changed = ChangedPositions.lower_bound(Record.Trace.Begin);
				Work.Charge(1);
				if(Record.FirstVariable != Record.EndVariable && Changed != ChangedPositions.end() && *Changed <= Record.Trace.End) {
					WorkQueue.insert(Group);
				}
				bool IsFlat = Record.Result == GroupState::Flat;
				if(WorkQueue.erase(Group)) {
					const GroupState Previous = Record.Result;
					IsFlat = CanFlatten(
						Atoms,
						Candidates[Group],
						ConditionalsOf[Group],
						Group,
						States,
						Assumed,
						Counter ? &*Counter : nullptr,
						Index ? &*Index : nullptr,
						Work,
						ShouldReuse ? &Record.Trace : nullptr
					);
					if(ShouldReuse) {
						Record.FirstVariable = CountBefore(VariablePositions, Record.Trace.Begin);
						Record.EndVariable = CountBefore(VariablePositions, Record.Trace.End + 1);
						Record.IsValid = true;
						Record.Result = IsFlat ? GroupState::Flat : GroupState::Broken;
						if(Previous != GroupState::Pending && Previous != Record.Result) {
							Work.Charge(Candidates[Group].size());
							ChangedPositions.insert(Candidates[Group].begin(), Candidates[Group].end());
						}
					}
				} else Work.Charge(1);
				States[Group] = IsFlat ? GroupState::Flat : GroupState::Broken;
				if(IsFlat) for(const size_t Position : Candidates[Group]) {
					if(Counter) Counter->Set(Position, false);
					else if(Index) Index->Boundaries.Set(Position, false);
					if(ShouldReuse) Work.Charge(1);
				}
			}
			// 包装記号と見出の候補を同じ状態で測り，後続の本体へ選択を直ちに反映する
			if(!BraceChoices.empty()) {
				std::vector<uint8_t> CurrentConditions(GroupCount);
				for(uint32_t Group = 0; Group < GroupCount; ++Group) CurrentConditions[Group] = States[Group] == GroupState::Broken;
				if(ShouldReuse && !UseScan) for(uint32_t Group = 0; Group < GroupCount; ++Group) {
					if(CurrentConditions[Group] != Assumed[Group]) for(const size_t Position : ConditionalsOf[Group]) {
						const Atom &Piece = Atoms[Position];
						Index->Set(Atoms, Position, IsVisible(Piece, NoGroup, CurrentConditions), IsBoundary(Piece, NoGroup, States));
						Work.Charge(1);
					}
				}
				for(const BraceChoice &Choice : BraceChoices) {
					SelectBraceBody(
						Atoms,
						Candidates,
						ConditionalsOf,
						Choice,
						States,
						CurrentConditions,
						ShouldReuse && Index ? &*Index : nullptr,
						Work
					);
				}
			}
			// 先行する群への展開依存の反映
			for(uint32_t Group = 0; Group < GroupCount; ++Group) {
				if(Dependencies[Group] != NoGroup && States[Dependencies[Group]] == GroupState::Broken) States[Group] = GroupState::Broken;
			}
			// 条件付の断片の無い中間表現は１巡で確定する
			if(!HasConditions) break;
			std::vector<uint8_t> Next(GroupCount);
			for(uint32_t Group = 0; Group < GroupCount; ++Group) Next[Group] = States[Group] == GroupState::Broken;
			// 次の巡の状態の構築と前の巡との比較
			Work.Charge(GroupCount);
			if(Next == Assumed) break;
			RoundState Round { {}, Rounds.back().Hash };
			for(uint32_t Group = 0; Group < GroupCount; ++Group) if(Next[Group] != Assumed[Group]) {
				Round.Flipped.push_back(Group);
				Round.Hash ^= Fingerprint(Group);
			}
			Work.Charge(GroupCount);
			const auto [First, Last] = RoundsByHash.equal_range(Round.Hash);
			for(std::unordered_multimap<size_t, size_t>::const_iterator Found = First; Found != Last; ++Found) {
				// 要約値の同じ巡の状態を最初の巡からの差分で組み立てて比べる
				std::vector<uint8_t> Earlier(GroupCount, 0);
				Work.Charge(GroupCount);
				for(size_t At = 1; At <= Found->second; ++At) {
					Work.Charge(Rounds[At].Flipped.size());
					for(const uint32_t Group : Rounds[At].Flipped) Earlier[Group] ^= 1;
				}
				if(Earlier == Next) throw FormatLimitExceeded("line breaking does not converge", FailureReason::FormatterDefect);
			}
			RoundsByHash.emplace(Round.Hash, Rounds.size());
			Rounds.push_back(std::move(Round));
			Assumed = std::move(Next);
			if(ShouldReuse) {
				// 次の巡に向けた変更群と索引の再設定
				ChangedAssumptions = Rounds.back().Flipped;
				std::fill(ChangedFlags.begin(), ChangedFlags.end(), 0);
				Work.Charge(GroupCount);
				for(const uint32_t Group : ChangedAssumptions) {
					ChangedFlags[Group] = 1;
					Work.Charge(1);
				}
				std::vector<uint32_t> ResetGroups;
				for(uint32_t Group = 0; Group < GroupCount; ++Group) {
					if(Records[Group].Result == GroupState::Flat || States[Group] == GroupState::Flat) ResetGroups.push_back(Group);
				}
				std::fill(States.begin(), States.end(), GroupState::Pending);
				for(const uint32_t Group : ResetGroups) for(const size_t Position : Candidates[Group]) {
					const Atom &Piece = Atoms[Position];
					if(Counter) Counter->Set(Position, IsVisible(Piece, NoGroup, Assumed) && IsBoundary(Piece, NoGroup, States));
					else Index->Set(Atoms, Position, IsVisible(Piece, NoGroup, Assumed), IsBoundary(Piece, NoGroup, States));
					Work.Charge(1);
				}
				const bool HasBraceIndex = !BraceChoices.empty() && Index.has_value();
				for(uint32_t Group = 0; Group < GroupCount; ++Group) {
					Work.Charge(1);
					if(HasBraceIndex || ChangedFlags[Group]) for(const size_t Position : ConditionalsOf[Group]) {
						const Atom &Piece = Atoms[Position];
						if(Counter) Counter->Set(Position, IsVisible(Piece, NoGroup, Assumed) && IsBoundary(Piece, NoGroup, States));
						else Index->Set(Atoms, Position, IsVisible(Piece, NoGroup, Assumed), IsBoundary(Piece, NoGroup, States));
						Work.Charge(1);
					}
				}
			}
		}
		// 選択した群と宣言の区分の期待結果の確定
		BrokenIR Result;
		Result.Items.reserve(Atoms.size());
		Result.Expectation.NormalizedSyntax = IR.NormalizedSyntax;
		Result.Expectation.DeclarationPartitions = std::move(Partitions);
		Result.Expectation.SelectedForms.resize(GroupCount);
		for(uint32_t Group = 0; Group < GroupCount; ++Group) {
			Result.Expectation.SelectedForms[Group] = std::move(State.Forms.at(ByClose[Group].second));
			Result.Expectation.SelectedForms[Group].IsBroken = States[Group] == GroupState::Broken;
		}
		// 解決済の要素の追加
		const auto Emit = [&Result](const ResolvedKind Kind, const FormatRange Text, const NodeId Node) -> void {
			ResolvedItem Item;
			Item.Kind = Kind;
			Item.Text = Text;
			Item.Node = Node;
			Result.Items.push_back(Item);
		};
		// 確定した境界と字句の出力
		for(const Atom &Piece : Atoms) {
			// 展開しない群に従う断片の読飛
			if(Piece.Condition != NoGroup && States[Piece.Condition] != GroupState::Broken) continue;
			const FormatItemDetail &Detail = IR.DetailOf(Piece.Detail);
			if(Detail.PunctuationOwner != NoNode && Piece.Generated == GeneratedToken::ListSeparator) {
				const SelectedFormEffect Effect {
					Detail.PunctuationOwner,
					FormOperation::TrailingComma,
					Detail.DeclarationOwner,
					Detail.DeclarationRow
				};
				if(Piece.Condition == NoGroup) {
					// 必須展開の容器には幅で選ぶ群が無いが，採用した句読点は同じ形式一覧へ載せる
					SelectedForm Form;
					Form.IsBroken = 1;
					Form.Nodes.push_back(Detail.PunctuationOwner);
					Form.Effects.push_back(Effect);
					Result.Expectation.SelectedForms.push_back(std::move(Form));
				} else Result.Expectation.SelectedForms[Piece.Condition].Effects.push_back(Effect);
			}
			switch(Piece.Kind) {
			case AtomKind::EdgeSpace:
				{
					const FormatRange Text = IsLineEdge(Atoms, static_cast<size_t>(&Piece - Atoms.data()), -1, NoGroup, States, Assumed) ||
					IsLineEdge(Atoms, static_cast<size_t>(&Piece - Atoms.data()), 1, NoGroup, States, Assumed) ? Piece.EdgeText : Piece.Text;
					Emit(ResolvedKind::Text, Text, Piece.Node);
				}
				break;
			case AtomKind::Text:
				if(Piece.Text.Size) {
					Emit(ResolvedKind::Text, Piece.Text, Piece.Node);
					Result.Items.back().Generated = Piece.Generated;
				}
				break;
			case AtomKind::Separator:
				Emit(ResolvedKind::Separator, Piece.Text, NoNode);
				break;
			case AtomKind::Soft:
				if(Piece.Group != NoGroup && States[Piece.Group] == GroupState::Broken) Emit(ResolvedKind::Newline, {}, NoNode);
				else if(Piece.Text.Size) Emit(ResolvedKind::Text, Piece.Text, NoNode);
				break;
			case AtomKind::Hard:
				Emit(ResolvedKind::Newline, {}, NoNode);
				break;
			case AtomKind::Blank:
				Emit(ResolvedKind::BlankLine, {}, Piece.Node);
				Result.Items.back().Before = Piece.Node;
				Result.Items.back().After = Detail.After;
				Result.Items.back().IsForced = Detail.IsForced;
				break;
			case AtomKind::Verbatim:
				Emit(ResolvedKind::Verbatim, Piece.Text, Piece.Node);
				break;
			case AtomKind::OwnedRegion:
				Emit(ResolvedKind::OwnedRegion, {}, Piece.Node);
				break;
			case AtomKind::IndentBegin:
				Emit(ResolvedKind::IndentBegin, {}, NoNode);
				break;
			case AtomKind::IndentEnd:
				Emit(ResolvedKind::IndentEnd, {}, NoNode);
				break;
			case AtomKind::AnchorBegin:
				Emit(ResolvedKind::AnchorBegin, {}, Piece.Node);
				break;
			case AtomKind::AnchorEnd:
				Emit(ResolvedKind::AnchorEnd, {}, Piece.Node);
				break;
			}
		}
		Result.Texts = std::move(IR.Texts);
		if(Measured) *Measured = Work;
		// 解決済の中間表現の返戻
		return Result;
	}

	/**
	 * 選択の解決関数
	 * @param IR 幅非依存の中間表現
	 * @return 全ての選択を解決した中間表現
	 * @throws FormatLimitExceeded 中間表現の不整合，循環，又は作業上限超過の場合
	 */
	BrokenIR LineBreaker::Resolve(FormatIR IR) const {
		// 依存関係に依る再利用を使った結果の返戻
		return ResolveWithMode(std::move(IR), true, nullptr);
	}

	/**
	 * コンストラクタ（行幅を受け取る唯一の口）
	 * @param Chars １行の最大文字数（０は無制限）
	 */
	LineBreaker::LineBreaker(const size_t Chars) : MaxChars(Chars) {}
} // namespace Core
