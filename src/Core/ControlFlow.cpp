#include "ControlFlow.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Core {
	/** ========== 共通判定 ========== */
	/**
	 * 独立した制御フロー境界の判定関数
	 * @param Type 判定する節点
	 * @return 外側の制御フローから分離する境界なら true
	 */
	bool ControlFlow::IsFlowBoundary(const NodeId Type) const {
		// 関数・クラスの本体と Rust の非同期の塊は独立し，Kotlin の inline ラムダからは外側のループへ脱出出来る事の返戻
		return Syntax.Is(Type, Role::FlowBoundary);
	}

	/**
	 * 分岐を全て列挙する切替かの判定関数
	 * 切替文と，本体を持たず全ての分岐を矢印で列挙する Java の切替式を同じ切替として扱い，分岐の中で脱出文を書けない切替の式は含めない
	 * @param Type 判定する節点
	 * @return 全ての分岐を列挙する切替なら true
	 */
	bool ControlFlow::IsArmSwitch(const NodeId Type) const {
		// 切替文と Java の切替式かの返戻
		return Syntax.Is(Type, Role::ArmSwitch);
	}

	/**
	 * 関数内 goto 飛先収集関数
	 * 範囲表の構築は節点数 N と入口数 L に対して O(N + L log L)
	 * @param Body 関数本体
	 * @return 飛先と其れに因る到達候補位置
	 */
	GotoIndex ControlFlow::CollectGotoTargets(const NodeId Body) const {
		// 飛先索引の初期化
		GotoIndex Result;
		Result.Body = Body;
		// goto を持たない言語の空の結果の返戻
		if(!Syntax.Rules().HasLabelJump) return Result;
		// ラベル名別の節点索引
		std::unordered_multimap<std::string, NodeId> Labels;
		std::vector<NodeId> Jumps, Entries;
		WalkNodes(
			Syntax,
			Body,
			[&](const NodeId Node) -> WalkStep {
				if(Syntax.Is(Node, Role::GotoStatement)) Jumps.push_back(Node);
				// 対象の識別名（名前の子を持たない誤り回復の節点は登録しない）
				else if(Syntax.Is(Node, Role::Labeled)) if(const NodeId Name = Syntax.NamedChild(Node, 0); Syntax.Is(Name, Role::NameToken)) {
					Labels.emplace(Syntax.ComparisonName(Name), Node);
				}
				// 制御フローの境界の中へ降りない事の返戻
				return IsFlowBoundary(Node) ? WalkStep::Skip : WalkStep::Descend;
			}
		);
		// 移動先が未解決か
		bool IsUnknown = false;
		for(const NodeId Jump : Jumps) {
			Syntax.Charge(1);
			NodeId Target = NoNode;
			if(
				Syntax.Is(Jump, Role::CaseLabelJump) && (Syntax.HasWordChild(Jump, Word::Case) || Syntax.HasWordChild(Jump, Word::Default))
			) for(NodeId Parent = Syntax.ParentOf(Jump); Parent != NoNode; Parent = Syntax.ParentOf(Parent)) {
				Syntax.Charge(1);
				if(Syntax.Is(Parent, Role::SwitchStatement)) {
					Target = Parent;
					break;
				}
				if(IsFlowBoundary(Parent)) break;
			} else if(const NodeId Name = Syntax.NamedChild(Jump, 0); Syntax.Is(Name, Role::NameToken)) {
				// 担当範囲の終端索引（名前でない計算した飛先は未解決とする）
				const auto [Begin, End] = Labels.equal_range(Syntax.ComparisonName(Name));
				// 採った飛先の有効範囲（C# は goto を含む最も内の有効範囲のラベルを採る）
				NodeId Closest = NoNode;
				for(std::unordered_multimap<std::string, NodeId>::const_iterator Iter = Begin; Iter != End; ++Iter) {
					Syntax.Charge(1);
					// 対象ラベルの節点
					const NodeId Label = Iter->second;
					if(Syntax.Rules().LabelJumpIsScoped) {
						NodeId Scope = Syntax.ParentOf(Label);
						while(Scope != NoNode) {
							Syntax.Charge(1);
							if(Syntax.Is(Scope, Role::ScopeBody)) break;
							Scope = Syntax.ParentOf(Scope);
						}
						if(
							Scope == NoNode || !Syntax.Contains(Scope, Jump) || Target != NoNode && (Scope == Closest || !Syntax.Contains(Closest, Scope))
						) continue;
						Closest = Scope;
					}
					Target = Label;
				}
			}
			// ジャンプ別の到達先索引への登録
			Result.Targets.emplace(Jump, Target);
			if(Target == NoNode) IsUnknown = true;
			else Entries.push_back(Target);
		}
		// 未解決の飛先を持つ関数では，何のラベルも到達不能と断定の回避
		if(IsUnknown) for(const auto &[Name, Label] : Labels) {
			Syntax.Charge(1);
			Entries.push_back(Label);
		}
		// 外部から到達するラベルの重複の除去
		Syntax.Charge(Entries.size());
		std::sort(Entries.begin(), Entries.end());
		Entries.erase(std::unique(Entries.begin(), Entries.end()), Entries.end());
		if(!Entries.empty()) {
			// 関数本体の構文順を１回だけ辿り，各節点の部分木の範囲を記録
			std::vector<std::pair<NodeId, bool>> Stack { { Body, false } };
			size_t Position = 0;
			while(!Stack.empty()) {
				const auto [Node, IsClosing] = Stack.back();
				Stack.pop_back();
				if(IsClosing) {
					Syntax.Charge(1);
					Result.Ranges.at(Node).second = Position;
					continue;
				}
				Syntax.Charge(1);
				Result.Ranges.emplace(Node, std::pair<size_t, size_t>{ Position++, 0 });
				Stack.emplace_back(Node, true);
				const std::span<const NodeId> Children = Syntax.ChildrenOf(Node);
				for(std::span<const NodeId>::reverse_iterator Next = Children.rbegin(); Next != Children.rend(); ++Next) {
					Stack.emplace_back(*Next, false);
				}
			}
			for(const NodeId Entry : Entries) {
				Syntax.Charge(1);
				Result.Entries.push_back(Result.Ranges.at(Entry).first);
			}
			// 入口の構文順への整列
			std::sort(Result.Entries.begin(), Result.Entries.end());
		}
		// 関数内で共有する飛先の返戻
		return Result;
	}

	/**
	 * 節点内への goto 入口有無判定関数
	 * 関数本体内の照会は入口数 L に対して O(log L)
	 * @param Node 検査する節点
	 * @param Gotos 同じ関数内の goto 飛先
	 * @return 外から到達候補となるラベルが有れば true
	 */
	bool ControlFlow::HasGotoEntry(const NodeId Node, const GotoIndex &Gotos) const {
		// 節点の部分木に入口のラベルが含まれるかの返戻
		if(Gotos.Entries.empty()) return false;
		Syntax.Charge(1);
		const std::unordered_map<NodeId, std::pair<size_t, size_t>>::const_iterator Found = Gotos.Ranges.find(Node);
		// 関数本体より外の節点からの照会では本体を含むかの返戻
		if(Found == Gotos.Ranges.end()) return Syntax.Contains(Node, Gotos.Body);
		const auto [Begin, End] = Found->second;
		const std::vector<size_t>::const_iterator Entry = std::lower_bound(
			Gotos.Entries.begin(),
			Gotos.Entries.end(),
			Begin,
			[this](const size_t Position, const size_t Wanted) -> bool {
				Syntax.Charge(1);
				// 入口の位置が部分木の始端より前かの返戻
				return Position < Wanted;
			}
		);
		// 始端以降の入口が無い場合の返戻
		if(Entry == Gotos.Entries.end()) return false;
		Syntax.Charge(1);
		// 最初の入口位置が部分木内に在るかの返戻
		return *Entry < End;
	}

	/**
	 * 先頭の子の取得関数
	 * @param Node 対象ノード
	 * @return 先頭の子（無名の字句を含む）の番号（子が無ければ NoNode）
	 */
	NodeId ControlFlow::FirstChildNode(const NodeId Node) const {
		// 先頭の子の番号の返戻
		return Syntax.Child(Node, 0);
	}

	/**
	 * 関数から戻る文・式かの判定関数
	 * @param Syntax 構文モデル
	 * @param Node 節点
	 * @return 返戻の文・式か，先頭の語が返戻の脱出の文 (Kotlin / Swift) なら true
	 */
	bool ControlFlow::IsReturn(const SyntaxModel &Syntax, const NodeId Node) {
		// 返戻の構文か，脱出の文の先頭の語で区別する返戻かの返戻
		return Syntax.Is(Node, Role::FlowReturn) ||
		Syntax.Is(Node, Role::KeywordJump) && Syntax.IsWord(Syntax.Child(Node, 0), Word::Return);
	}

	/**
	 * 同じ関数に属する指定型の子孫の有無の判定関数
	 * @param Node 検索する節点
	 * @param Desired 判定する役割
	 * @return ネスト関数の本体を除いて指定型の名前付節点が有れば true
	 */
	bool ControlFlow::HasFlowDescendant(const NodeId Node, const Role Desired) const {
		// 同じ関数に属する指定型の節点で止めた走査の結果の返戻
		return WalkNodes(
			Syntax,
			Node,
			[&](const NodeId Current) -> WalkStep {
				// 境界の中へ降りない事の返戻
				if(Current != Node && IsFlowBoundary(Current)) return WalkStep::Skip;
				// 指定型で止める事の返戻
				return Syntax.IsNamed(Current) && Syntax.Is(Current, Desired) ? WalkStep::Stop : WalkStep::Descend;
			}
		);
	}

	/**
	 * 同じ制御フロー内の返戻文包含判定関数
	 * @param Node 検索する節点
	 * @return ネスト関数の本体を除いて返戻文が有れば true
	 */
	bool ControlFlow::HasReturnInFlow(const NodeId Node) const {
		// 同じ関数に属する返戻文で止めた走査の結果の返戻
		return WalkNodes(
			Syntax,
			Node,
			[&](const NodeId Current) -> WalkStep {
				// 境界の中へ降りない事の返戻
				if(IsFlowBoundary(Current)) return WalkStep::Skip;
				// 返戻文で止める事の返戻
				return IsReturn(Syntax, Current) ? WalkStep::Stop : WalkStep::Descend;
			}
		);
	}

	/**
	 * 同じ関数に属する値付の返戻文の有無の判定関数
	 * @param Body 関数本体
	 * @return ネスト関数の本体を除いて値を返す返戻文が有れば true
	 */
	bool ControlFlow::HasValueReturn(const NodeId Body) const {
		// 本体が空ノードの場合の返戻
		if(Body == NoNode) return false;
		// ネスト関数定義の戻り文は当該関数に属さない為，其の部分木へは降りない走査の結果の返戻
		return WalkNodes(
			Syntax,
			Body,
			[&](const NodeId Node) -> WalkStep {
				// 境界の中へ降りない事の返戻
				if(Node != Body && IsFlowBoundary(Node)) return WalkStep::Skip;
				// Ruby の return は実引数の並び，其の他の言語は名前付の子（戻値式）の有無で値付かを判別する事の返戻
				return (
					Syntax.Is(Node, Role::ArgumentReturn) ?
					Syntax.Is(Node, Role::ReturnKeyword) && Syntax.FirstNamedChildWithRole(Node, Role::ArgumentSequence) != NoNode :
					Syntax.Is(Node, Role::FlowReturn) && Syntax.NamedChild(Node, 0) != NoNode
				) ? WalkStep::Stop : WalkStep::Descend;
			}
		);
	}

	/**
	 * 脱出文のラベルの取得関数
	 * @param Node 脱出文
	 * @return 脱出先を名付けるラベル又は PHP の段数（無ければ空）
	 */
	NodeId ControlFlow::ExitLabel(const NodeId Node) const {
		// Rust の脱出は値を伴え，ラベルは専用の節点に限る事の返戻
		if(Syntax.Is(Node, Role::LabeledValueExit)) return Syntax.FirstNamedChildWithRole(Node, Role::JumpLabel);
		// 其の他の言語は最初の名前付子がラベル又は段数である事の返戻
		return Syntax.NamedChild(Node, 0);
	}

	/**
	 * 脱出文の飛先の解決関数
	 * 無名の脱出は祖先の索引から選び，ラベル付の脱出は元の祖先順で照合する
	 * 計算量：引数の字面の長さ L と実際に調べる祖先・先行兄弟の数 A に対し O(L + A)
	 * @param Node 脱出文
	 * @param Continues 繰返の継続か
	 * @param Path 現在の祖先の脱出先
	 * @return 飛先と継続の扱い（未解決なら飛先は無し）
	 */
	std::pair<NodeId, bool> ControlFlow::ResolveExitTarget(const NodeId Node, const bool Continues, const ExitPath &Path) const {
		NodeId Argument = ExitLabel(Node);
		std::string Label;
		uint32_t Level = 1;
		if(Argument != NoNode) {
			if(const SyntaxRules &Rules = Syntax.Rules(); Rules.ExitHasNumericDepth) {
				while(Argument != NoNode && Syntax.Is(Argument, Role::ValueParentheses)) {
					Syntax.Charge(1);
					Argument = Syntax.NamedChild(Argument, 0);
				}
				// 飛先の無い指定の返戻
				if(Argument == NoNode) return { NoNode, false };
				// 段数の記法と進数の解釈
				std::string Digits(Syntax.Text(Argument));
				std::erase(Digits, Rules.DigitSeparator);
				std::string_view Number(Digits);
				int Base = 10;
				if(Number.size() > 1 && Rules.IsSpelling(Number.substr(0, 1), Word::IntegerZero)) {
					const std::string_view Prefix = Number.substr(1, 1);
					const std::array<std::string_view, 3> &Prefixes = Rules.ExitDepthBasePrefixes;
					const bool IsHex = Prefixes[0].find(Prefix) != std::string_view::npos;
					const bool IsBinary = Prefixes[1].find(Prefix) != std::string_view::npos;
					Base = IsHex ? 16 : IsBinary ? 2 : 8;
					if(IsHex || IsBinary || Prefixes[2].find(Prefix) != std::string_view::npos) Number.remove_prefix(2);
				}
				const std::from_chars_result Parsed = std::from_chars(Number.data(), Number.data() + Number.size(), Level, Base);
				// 解析不能な段数の返戻
				if(Parsed.ec != std::errc{} || Parsed.ptr != Number.data() + Number.size() || !Level) return { NoNode, false };
			} else if(Syntax.Is(Argument, Role::NameToken)) Label = Syntax.ComparisonName(Argument);
			// 名前でない値を伴う脱出の飛先を解決しない事の返戻
			else return { NoNode, false };
		}
		if(Label.empty()) {
			const std::vector<NodeId> &Targets = Continues ? Path.ContinueTargets : Path.BreakTargets;
			// 指定段数の飛先を持たない場合の返戻
			if(Level > Targets.size()) return { NoNode, false };
			const NodeId Target = Targets[Targets.size() - Level];
			// 段数で選んだ飛先の返戻
			return { Target, Continues && !Syntax.Is(Target, Role::ContinueExitsSwitch) };
		}
		// ラベルを指定した脱出先の祖先探索
		for(NodeId Parent = Syntax.ParentOf(Node); Parent != NoNode; Parent = Syntax.ParentOf(Parent)) {
			Syntax.Charge(1);
			// 独立した制御の境界の返戻
			if(IsFlowBoundary(Parent)) return { Parent, false };
			if(Syntax.Rules().ExitLabelIsBodyChild) {
				const NodeId Own = Syntax.FirstNamedChildWithRole(Parent, Role::JumpLabel);
				// 本体のラベルに一致する飛先の返戻
				if(Own != NoNode && Syntax.ComparisonName(Own) == Label) return { Parent, Continues };
				continue;
			}
			const bool IsMatchingLabel = Syntax.Is(Parent, Role::LabeledStatement) && Syntax.NamedChildCount(Parent) &&
			Syntax.ComparisonName(Syntax.NamedChild(Parent, 0)) == Label;
			// 最も内側のラベルに一致する飛先の返戻
			if(IsMatchingLabel) return { Parent, Continues };
			if(Syntax.Rules().ExitLabelIsPreviousSibling) {
				for(NodeId Previous = Syntax.PrevNamedSibling(Parent); Previous != NoNode; Previous = Syntax.PrevNamedSibling(Previous)) {
					Syntax.Charge(1);
					if(Syntax.Is(Previous, Role::Comment) || Syntax.Is(Previous, Role::TransparentLabelPrefix)) continue;
					if(!Syntax.Is(Previous, Role::Label)) break;
					// 直前のラベルに一致する飛先の返戻
					if(Syntax.Is(Previous, Role::NameToken) && Syntax.ComparisonName(Previous) == Label) return { Parent, Continues };
				}
			}
		}
		// 未解決のラベルの返戻
		return { NoNode, false };
	}

	/**
	 * 脱出の索引の構築関数
	 * 構文順と深さを記録し，各脱出の飛先を走査中の祖先の索引から一度だけ選ぶ
	 * 計算量：節点数 N とラベル付脱出・goto の飛先探索総量 A に対し O(N + A)
	 * @param Root 走査する関数本体
	 * @param Gotos 同じ関数内の goto 飛先
	 * @param Index 構築する索引
	 */
	void ControlFlow::BuildLoopExitIndex(const NodeId Root, const GotoIndex &Gotos, LoopExitIndex &Index) const {
		Index = {};
		Index.Root = Root;
		struct Pending {
			NodeId Node;
			uint32_t Depth;
			bool IsClosing;
			bool AddedBreak;
			bool AddedContinue;
		};
		std::vector<Pending> Work { { Root, 0, false, false, false } };
		ExitPath Path;
		size_t Position = 0;
		// 構文順の範囲と脱出事象の収集
		while(!Work.empty()) {
			const Pending Current = Work.back();
			Work.pop_back();
			if(Current.IsClosing) {
				Syntax.Charge(1);
				Index.Ranges.at(Current.Node).second = Position;
				Index.Postorder.push_back(Current.Node);
				if(Current.AddedBreak) Path.BreakTargets.pop_back();
				if(Current.AddedContinue) Path.ContinueTargets.pop_back();
				continue;
			}
			Syntax.Charge(1);
			Index.Ranges.emplace(Current.Node, std::pair<size_t, size_t>{ Position++, 0 });
			Index.Depths.emplace(Current.Node, Current.Depth);
			// 脱出文の飛先の記録
			if(Syntax.Is(Current.Node, Role::GotoStatement)) {
				Syntax.Charge(1);
				const std::unordered_map<NodeId, NodeId>::const_iterator Found = Gotos.Targets.find(Current.Node);
				Index.Events.push_back({ Current.Node, Found == Gotos.Targets.end() ? NoNode : Found->second, NoNode, 0, true });
				Index.HasGoto = true;
			} else if(
				const bool IsLoopExit = Syntax.Is(Current.Node, Role::LoopExitStatement);
				IsLoopExit || Syntax.Is(Current.Node, Role::KeywordJump)
			) {
				const NodeId Keyword = FirstChildNode(Current.Node);
				if(
					const bool Continues = Syntax.Is(Keyword, Role::ContinueKeyword);
					IsLoopExit || Continues || Syntax.Is(Keyword, Role::BreakKeyword)
				) {
					Syntax.Charge(1);
					const auto [Target, IsContinuation] = ResolveExitTarget(Current.Node, Continues, Path);
					Index.Events.push_back({ Current.Node, NoNode, Target, 0, false, IsContinuation });
				}
			}
			// 祖先の脱出先の更新
			const bool AddedBreak = Syntax.Is(Current.Node, Role::BreakTarget);
			const bool AddedContinue = AddedBreak && (Syntax.Rules().ContinueCountsSwitch || Syntax.Is(Current.Node, Role::Loop));
			if(AddedBreak) Path.BreakTargets.push_back(Current.Node);
			if(AddedContinue) Path.ContinueTargets.push_back(Current.Node);
			Work.push_back({ Current.Node, Current.Depth, true, AddedBreak, AddedContinue });
			if(IsFlowBoundary(Current.Node)) continue;
			const std::span<const NodeId> Children = Syntax.ChildrenOf(Current.Node);
			for(std::span<const NodeId>::reverse_iterator Child = Children.rbegin(); Child != Children.rend(); ++Child) {
				Work.push_back({ *Child, Current.Depth + 1, false, false, false });
			}
		}
		// 飛先と判定範囲からの脱出の深さの確定
		for(ExitEvent &Event : Index.Events) {
			Syntax.Charge(1);
			if(!Event.IsGoto) {
				NodeId Target = Event.Target;
				if(Event.Continues) while(Syntax.Is(Target, Role::LabeledStatement)) {
					Syntax.Charge(1);
					const uint32_t Count = Syntax.NamedChildCount(Target);
					if(!Count) {
						Target = NoNode;
						break;
					}
					Target = Syntax.NamedChild(Target, Count - 1);
				}
				Event.Depth = Target != NoNode && Index.Ranges.contains(Target) ? Index.Depths.at(Target) + (Event.Continues ? 1 : 0) : 0;
				continue;
			}
			// goto の着地点と共通の祖先の確認
			NodeId Target = Event.Destination;
			while(Target != NoNode && (Syntax.Is(Target, Role::NamedLabelStatement) || Syntax.Is(Target, Role::Comment))) {
				Syntax.Charge(1);
				Target = Syntax.NextNamedSibling(Target);
			}
			while(Target != NoNode && Syntax.Is(Target, Role::LabeledStatement)) {
				Syntax.Charge(1);
				const uint32_t Count = Syntax.NamedChildCount(Target);
				if(!Count) break;
				Target = Syntax.NamedChild(Target, Count - 1);
			}
			if(Event.Destination != NoNode && Index.Ranges.contains(Event.Destination)) {
				const size_t Destination = Index.Ranges.at(Event.Destination).first;
				for(NodeId Up = Event.Node; Up != NoNode; Up = Syntax.ParentOf(Up)) {
					Syntax.Charge(1);
					if(
						const std::unordered_map<NodeId, std::pair<size_t, size_t>>::const_iterator Range = Index.Ranges.find(Up);
						Range != Index.Ranges.end() && Range->second.first <= Destination && Destination < Range->second.second
					) {
						Event.Depth = Index.Depths.at(Up) + 1;
						break;
					}
				}
			}
			if(Target != NoNode && Index.Ranges.contains(Target)) {
				const auto [Begin, End] = Index.Ranges.at(Target);
				if(const size_t Source = Index.Ranges.at(Event.Node).first; Begin <= Source && Source < End) {
					Event.Depth = std::max(Event.Depth, Index.Depths.at(Target) + 1);
				}
			}
		}
		// 索引の構築の終了
		return;
	}

	/**
	 * 対象ループからの脱出有無判定関数
	 * 同じ判定範囲の脱出の最小深さを本体へ集計して再利用する
	 * 計算量：最初の索引は節点数 N と飛先探索総量 A に対し O(N + A)，新しい判定範囲は goto 数 G と深さ D に対し O(N + G×D)，其の後は O(1)
	 * @param Body 検索する本体
	 * @param Loop 対象ループ
	 * @param Gotos 同じ関数内の goto 飛先
	 * @param Index 同じ終端判定で共有する脱出の索引
	 * @param TransferScope 内部転送を認める切替文の判定範囲
	 * @return ループ自身又は其の外側への脱出が有れば true
	 */
	bool ControlFlow::HasLoopExit(
		const NodeId Body,
		const NodeId Loop,
		const GotoIndex &Gotos,
		LoopExitIndex &Index,
		const NodeId TransferScope
	) const {
		// 本体の無い繰返は脱出しない事の返戻
		if(Body == NoNode) return false;
		if(Index.Root == NoNode || !Index.Ranges.contains(Body) || !Index.Ranges.contains(Loop)) {
			const NodeId Root = Gotos.Body != NoNode && Syntax.Contains(Gotos.Body, Loop) ? Gotos.Body : Loop;
			BuildLoopExitIndex(Root, Gotos, Index);
		}
		// 判定範囲毎の脱出深さの集計
		const NodeId Scope = Index.HasGoto ? TransferScope : NoNode;
		const auto [Found, IsNew] = Index.MinimumByScope.try_emplace(Scope);
		if(IsNew) {
			std::unordered_map<NodeId, uint32_t> &Minimum = Found->second;
			for(const ExitEvent &Event : Index.Events) {
				Syntax.Charge(1);
				if(Event.IsGoto && Scope != NoNode && Syntax.Contains(Scope, Event.Destination)) continue;
				const auto [Place, IsInserted] = Minimum.try_emplace(Event.Node, Event.Depth);
				if(!IsInserted) Place->second = std::min(Place->second, Event.Depth);
			}
			for(const NodeId Node : Index.Postorder) {
				Syntax.Charge(1);
				const std::unordered_map<NodeId, uint32_t>::const_iterator Child = Minimum.find(Node);
				if(Child == Minimum.end()) continue;
				const NodeId Parent = Syntax.ParentOf(Node);
				if(Parent == NoNode || !Index.Ranges.contains(Parent)) continue;
				const auto [Place, IsInserted] = Minimum.try_emplace(Parent, Child->second);
				if(!IsInserted) Place->second = std::min(Place->second, Child->second);
			}
		}
		Syntax.Charge(1);
		const std::unordered_map<NodeId, uint32_t>::const_iterator Exit = Found->second.find(Body);
		// 本体の脱出の飛先が対象ループ自身又は其の外に在る事の返戻
		return Exit != Found->second.end() && Exit->second <= Index.Depths.at(Loop);
	}

	/**
	 * 宣言の有効範囲の取得関数（整形仕様 5.2）
	 * 宣言自身か其の祖先の内で最も内側の塊・繰返・捕捉の節・関数・型の本体を有効範囲とする（仮引数は関数，初期化部の宣言は繰返）
	 * @param Declaration 宣言の節点
	 * @return 有効範囲の節点（見付からなければ根）
	 */
	NodeId ControlFlow::ScopeOf(const NodeId Declaration) const {
		NodeId Scope = Declaration;
		while(
			Syntax.ParentOf(Scope) != NoNode && !Syntax.Is(Scope, Role::Block) && !Syntax.Is(Scope, Role::ScopeBody) &&
			!Syntax.Is(Scope, Role::Loop) && !Syntax.Is(Scope, Role::CatchClause) && !Syntax.Is(Scope, Role::SwitchBody) &&
			!Syntax.Is(Scope, Role::FunctionScope) && !Syntax.Is(Scope, Role::ClassBody)
		) {
			Syntax.Charge(1);
			Scope = Syntax.ParentOf(Scope);
		}
		// 有効範囲の節点の返戻
		return Scope;
	}

	/**
	 * 範囲の名前の宣言の取得関数（整形仕様 5.2, 9.2）
	 * 関数は中の全ての宣言を，型の本体と外側の塊は直下の宣言だけ（成員の本体と入れ子の型へ降りない）を集める
	 * 計算量：範囲の節点数 N に対し O(N)
	 * @param Scope 関数，型の本体又は外側の塊
	 * @param IsMemberScope 直下の宣言だけを集めるか
	 * @return 原文順の名前の宣言
	 */
	std::vector<ControlFlow::NameDeclaration> ControlFlow::DeclarationsIn(const NodeId Scope, const bool IsMemberScope) const {
		std::vector<NameDeclaration> Declarations;
		WalkNodes(
			Syntax,
			Scope,
			[&](const NodeId Declarator) -> WalkStep {
				// 型の本体の中の成員の本体と入れ子の型へ降りない事の返戻
				if(
					IsMemberScope && Declarator != Scope && (Syntax.Is(Declarator, Role::DefinitionBody) || Syntax.Is(Declarator, Role::ClassLike))
				) return WalkStep::Skip;
				// 宣言子でない節点は名前の場を探索しない
				if(!Syntax.Is(Declarator, Role::VariableDeclarator)) return WalkStep::Descend;
				const NodeId NameNode = Syntax.FieldChild(Declarator, ChildRole::Name);
				// 識別子を名前とする宣言子だけの照合（名前を持たない束縛は比較用の名前を持たない）
				// 名前を持たない宣言子の子を辿る事の返戻
				if(NameNode == NoNode || !Syntax.Is(NameNode, Role::Identifier)) return WalkStep::Descend;
				// 宣言子を並べる宣言（仮引数・捕捉・拡張 for は自身が宣言）
				NodeId Declaration = Declarator;
				if(Syntax.Is(Declarator, Role::VariableDeclaratorItem)) {
					Declaration = Syntax.ParentOf(Declarator);
					if(Syntax.Is(Declaration, Role::VariableDeclaration)) Declaration = Syntax.ParentOf(Declaration);
				}
				// 直下でない宣言を除いて子へ降りる事の返戻
				if(IsMemberScope && Syntax.ParentOf(Declaration) != Scope) return WalkStep::Descend;
				// 名前より前の定数を宣言する語の照合
				const bool IsConstant = AnyNodeOf(
					Syntax,
					Declaration,
					[&](const NodeId Modifier) -> bool {
						// 定数を宣言する語かの返戻
						return Syntax.Precedes(Modifier, NameNode) && !Syntax.ChildCount(Modifier) &&
						Syntax.IsWord(Modifier, Word::ConstantDeclaration);
					}
				);
				Declarations.push_back({ NameNode, IsMemberScope ? Scope : ScopeOf(Declaration), IsConstant });
				// 宣言の中も辿る事の返戻
				return WalkStep::Descend;
			}
		);
		// 名前の宣言の返戻
		return Declarations;
	}

	/**
	 * 関数から見える名前の表の取得関数（整形仕様 5.2, 9.2）
	 * 関数の中の宣言は有効範囲と共に保ち（条件の位置で見える物だけが外側の同名の定数を隠す），外側は名前を宣言する最も内側の範囲の宣言を採る
	 * 型の本体と外側の塊の表は一度だけ作り，同じ構文の関数毎の判定で共有する
	 * 計算量：関数の節点数 N と，初めて見る外側の範囲の節点数 M に対し O(N + M)
	 * @param Function 関数の節点
	 * @return 関数の中の宣言と，名前毎の最も内側の外側の範囲の同名の宣言に定数の宣言が有るかの表
	 */
	ControlFlow::VisibleNameTable ControlFlow::VisibleNamesOf(const NodeId Function) const {
		VisibleNameTable Names;
		// 関数内の宣言の登録
		for(const NameDeclaration &Declaration : DeclarationsIn(Function, false)) {
			Names.Locals[Syntax.ComparisonName(Declaration.Name)].push_back(Declaration);
		}
		// 外側の範囲の宣言の収集と再利用
		for(NodeId Parent = Syntax.ParentOf(Function); Parent != NoNode; Parent = Syntax.ParentOf(Parent)) {
			Syntax.Charge(1);
			if(!Syntax.Is(Parent, Role::ClassBody) && !Syntax.Is(Parent, Role::ScopeBody)) continue;
			std::unordered_map<NodeId, NameConstants>::iterator Found = MemberNames.find(Parent);
			if(Found == MemberNames.end()) {
				Found = MemberNames.emplace(Parent, NameConstants()).first;
				for(const NameDeclaration &Declaration : DeclarationsIn(Parent, true)) {
					bool &IsConstant = Found->second[Syntax.ComparisonName(Declaration.Name)];
					IsConstant = IsConstant || Declaration.IsConstant;
				}
			}
			// 内側で宣言済の名前を保つ外側の宣言の追加
			for(const auto &[Name, IsConstant] : Found->second) Names.Outer.emplace(Name, IsConstant);
		}
		// 見える名前の表の返戻
		return Names;
	}

	/**
	 * 定数式と評価され得る条件かの判定関数
	 * Java / C# の到達可能性は定数式の値を用いる為，変数でない名前と字句だけの条件を定数の候補とする
	 * 名前は悲観の判定の為に作った関数から見える名前の表で引き，関数の中の宣言は条件より前に在り有効範囲が条件を含む最も内側の物を採る
	 * @param Cond 条件の節点
	 * @return 定数式でないと構文から確定出来なければ true
	 */
	bool ControlFlow::MayBeConstantCondition(const NodeId Cond) const {
		// 定数式でない事を確定出来ないかの返戻
		return !AnyNodeOf(
			Syntax,
			Cond,
			[&](const NodeId Node) -> bool {
				// 呼出・代入・増減・型検査等は定数式でない事の返戻
				if(Syntax.Is(Node, Role::NonConstantExpression)) return true;
				// 名前以外の節点は判定を子へ委ねる事の返戻
				if(!Syntax.Is(Node, Role::Identifier)) return false;
				// 修飾の後の名前は this の欄だけを照合し，其の他は型の定数の候補とする
				if(const NodeId Parent = Syntax.ParentOf(Node); Syntax.Is(Parent, Role::QualifiedMember)) {
					const NodeId Object = Syntax.NamedChild(Parent, 0);
					// 受け手の名前と this の欄の照合結果の返戻
					if(Object != Node && !Syntax.Is(Object, Role::ThisReference)) return false;
				}
				const std::string Name = Syntax.ComparisonName(Node);
				// 条件の位置で見える最も内側の関数の中の宣言
				const NameDeclaration *Visible = nullptr;
				size_t VisibleDepth = std::numeric_limits<size_t>::max();
				if(
					const std::unordered_map<std::string, std::vector<NameDeclaration>>::const_iterator Locals = VisibleNames->Locals.find(Name);
					Locals != VisibleNames->Locals.end()
				) {
					// 名前の祖先毎の深さ（宣言毎に根へ遡らない）
					std::unordered_map<NodeId, size_t> Ancestors;
					size_t Depth = 0;
					for(NodeId Up = Node; Up != NoNode; Up = Syntax.ParentOf(Up)) {
						Syntax.Charge(1);
						Ancestors.emplace(Up, Depth++);
					}
					for(const NameDeclaration &Declaration : Locals->second) {
						const std::unordered_map<NodeId, size_t>::const_iterator Scope = Ancestors.find(Declaration.Scope);
						if(Scope == Ancestors.end() || Scope->second >= VisibleDepth || !Syntax.Precedes(Declaration.Name, Node)) continue;
						Visible = &Declaration;
						VisibleDepth = Scope->second;
					}
				}
				// 見える関数の中の宣言が変数である事の返戻
				if(Visible) return !Visible->IsConstant;
				const NameConstants::const_iterator Found = VisibleNames->Outer.find(Name);
				// 外側の宣言が見付かり定数の宣言を持たない名前は変数である事の返戻
				return Found != VisibleNames->Outer.end() && !Found->second;
			}
		);
	}

	/**
	 * ループ条件の真値リテラル判定関数
	 * @param Cond 判定対象の条件式ノード
	 * @return 真値リテラルなら true
	 */
	bool ControlFlow::IsAlwaysTrueCondition(NodeId Cond) const {
		// 常真判定に不要な条件の包みの除去
		while(Cond != NoNode) {
			Syntax.Charge(1);
			// 真値の字句と，整数の１だけの条件は常に真である事の返戻
			if(
				Syntax.Is(Cond, Role::TrueValue) || Syntax.Is(Cond, Role::IntegerLiteralCandidate) && Syntax.IsWord(Cond, Word::IntegerTruth)
			) return true;
			// 文法の真値の綴りの返戻
			if(Syntax.Is(Cond, Role::Boolean)) return Syntax.IsWord(Cond, Word::True);
			// C# / Kotlin / Swift は真値キーワードを真偽値リテラルノードで包む為，名前無子トークンの型名が `true` なら真
			if(Syntax.Is(Cond, Role::BooleanLiteral)) {
				// 括弧内の節点
				const NodeId Inner = Syntax.Child(Cond, 0);
				// 中身の無い真偽値は常に真でない事の返戻
				if(Inner == NoNode) return false;
				// 内側トークンの真値キーワード該当有無の返戻
				return Syntax.Is(Inner, Role::TrueValue);
			}
			if(Syntax.Is(Cond, Role::CondParenWrap)) {
				Cond = Syntax.NamedChild(Cond, 0);
				continue;
			}
			// 真値以外の条件は無限ループと判定不可の返戻
			return false;
		}
		// 条件が空の為，無限ループ判定不可の返戻
		return false;
	}

	/**
	 * 繰返を続ける条件と看做すかの判定関数（整形仕様 5.2）
	 * 真値の条件に加え，到達可能性を悲観的に判定する時は定数式と評価され得る条件も常に真と看做す
	 * @param Cond 条件の節点
	 * @return 常に真と看做す条件なら true
	 */
	bool ControlFlow::IsAssumedTrueCondition(const NodeId Cond) const {
		// 真値の条件か，悲観の判定で定数と為り得る条件かの返戻
		return IsAlwaysTrueCondition(Cond) || VisibleNames && Cond != NoNode && MayBeConstantCondition(Cond);
	}

	/**
	 * 条件で終わらない繰返かの判定関数
	 * @param Node 判定対象ノード
	 * @param Body 繰返の本体の格納先（繰返でなければ空）
	 * @return 条件が常に真で，本体からの脱出だけが繰返を終える形なら true
	 */
	bool ControlFlow::IsInfiniteLoop(const NodeId Node, NodeId &Body) const {
		Body = Syntax.FieldChild(Node, ChildRole::Body);
		// Rust の無条件の繰返は常に続く事の返戻
		if(Syntax.Is(Node, Role::LoopExpression)) return true;
		if(Syntax.Is(Node, Role::CountedLoop)) {
			if(Syntax.Is(Node, Role::ClauseOrConditionLoop)) {
				// Go は条件の節で条件を省くか，節も条件も持たない形が無条件
				if(const NodeId Clause = Syntax.FirstNamedChildWithRole(Node, Role::ForClause); Clause != NoNode) {
					const NodeId Cond = Syntax.FieldChild(Clause, ChildRole::Condition);
					// 条件の無い節か常真の条件かの返戻
					return Cond == NoNode || IsAssumedTrueCondition(Cond);
				}
				// 範囲の反復は要素が尽きて終わる事の返戻
				if(Syntax.FirstNamedChildWithRole(Node, Role::RangeClause) != NoNode) return false;
				const NodeId First = Syntax.NamedChild(Node, 0);
				// 本体だけの繰返か常真の条件かの返戻
				return First == Body || IsAssumedTrueCondition(First);
			}
			// 条件を省ける C 形式の for は C 系・C#・Java・JS/TS・PHP だけ
			// 其の他の言語の for は要素の反復である事の返戻
			if(!Syntax.Is(Node, Role::OmittableConditionLoop)) return false;
			const NodeId Cond = Syntax.FieldChild(Node, ChildRole::Condition);
			// JS/TS の省いた条件は区切だけの字句又は空文で表す
			const bool IsOmitted = Cond == NoNode || !Syntax.IsNamed(Cond) || Syntax.Is(Cond, Role::EmptyStatement);
			// 省いた条件か常真の条件かの返戻
			return IsOmitted || IsAssumedTrueCondition(Cond);
		}
		const bool IsDoLoop = Syntax.Is(Node, Role::PostConditionLoop);
		// 条件で終わる繰返以外は無条件の繰返でない事の返戻
		if(!IsDoLoop && !Syntax.Is(Node, Role::WhileLoop)) return false;
		NodeId Cond = Syntax.FieldChild(Node, ChildRole::Condition);
		// 場を持たない Kotlin と本体を場で持たない Swift の子の並びからの取得
		if(Cond == NoNode || Body == NoNode) {
			NodeId First = NoNode, Last = NoNode;
			ForEachNamedChildOf(
				Syntax,
				Node,
				[&](const NodeId Child) -> void {
					// 注記の子を最初と最後の子に数えない場合の終了
					if(Syntax.Is(Child, Role::Comment)) return;
					if(First == NoNode) First = Child;
					Last = Child;
				}
			);
			// 後置の条件の繰返は本体が先，其の他は条件が先
			if(Cond == NoNode) Cond = IsDoLoop ? Last : First;
			if(Body == NoNode && First != Last) Body = IsDoLoop ? First : Last;
			// 空の本体は Swift の本体の子を持たない
			if(Body != NoNode && Body == Cond) Body = NoNode;
		}
		// Swift の複数の条件の場は全てが常真である事の要求
		for(const NodeId Child : Syntax.ChildrenOf(Node)) {
			Syntax.Charge(1);
			// 常真でない条件の場が有れば無条件でない事の返戻
			if(Syntax.HasField(Child, ChildRole::Condition) && !IsAssumedTrueCondition(Child)) return false;
		}
		// 条件が常真かの返戻
		return IsAssumedTrueCondition(Cond);
	}

	/**
	 * 戻らない事を言語・標準ライブラリが定める呼出かの判定関数
	 * @param Node 判定対象ノード
	 * @return 処理を終える（又は例外を送出する）事だけが結果である呼出なら true
	 */
	bool ControlFlow::IsNoReturnCall(const NodeId Node) const {
		if(Syntax.Is(Node, Role::MacroCall)) {
			// 戻らないマクロを持つ文法の，戻らない事が定義された名前のマクロかの返戻
			return Syntax.Is(Node, Role::TerminatingMacroCall) &&
			Syntax.IsWord(Syntax.FieldChild(Node, ChildRole::Macro), Word::TerminatingMacro);
		}
		// 呼出でない節点は戻らない呼出でない事の返戻
		if(!Syntax.Is(Node, Role::CallNode)) return false;
		if(!Syntax.Is(Node, Role::MethodInvocation)) {
			const NodeId Callee = Syntax.FieldChild(Node, ChildRole::Function);
			// 受け手と名前を別の場に持たない呼出の，戻らない事が定義された名前の呼出かの返戻
			return Syntax.IsWord(Callee == NoNode ? Syntax.NamedChild(Node, 0) : Callee, Word::TerminatingCall);
		}
		const NodeId Object = Syntax.FieldChild(Node, ChildRole::Object), Method = Syntax.FieldChild(Node, ChildRole::Name);
		// 修飾を受けない呼出は対象外である事の返戻
		if(Object == NoNode || Method == NoNode) return false;
		// 受け手から名前迄の子を繋いだ修飾した名前
		std::string Name;
		bool IsQualifier = false;
		for(const NodeId Child : Syntax.ChildrenOf(Node)) {
			Syntax.Charge(1);
			IsQualifier = IsQualifier || Child == Object;
			if(!IsQualifier) continue;
			Name += Syntax.Text(Child);
			if(Child == Method) break;
		}
		// 戻らない事が定義された名前との一致の返戻
		return Syntax.Rules().IsSpelling(Name, Word::TerminatingCall);
	}

	/**
	 * 終端の判定結果の鍵の生成関数
	 * @param Node 判定した節点
	 * @param Scope 内部転送を認める切替文の判定範囲
	 * @return 節点と判定範囲の組の鍵
	 */
	uint64_t ControlFlow::TerminationKey(const NodeId Node, const NodeId Scope) {
		// 上位に節点，下位に判定範囲を置いた鍵の返戻
		return uint64_t{ Node } << 32 | Scope;
	}

	/**
	 * 判定済の文の終端の取得関数
	 * @param Known 判定範囲毎の節点の終端の判定結果
	 * @param Node 文（無しは終端しない）
	 * @param Scope 内部転送を認める切替文の判定範囲
	 * @return 終端する場合 true
	 * @throws FormatLimitExceeded 判定する子を作業列へ積まずに結果を読んだ場合（整形器の不具合）
	 */
	bool ControlFlow::KnownTermination(const TerminationResults &Known, const NodeId Node, const NodeId Scope) {
		// 文の無い事の返戻
		if(Node == NoNode) return false;
		const TerminationResults::const_iterator Found = Known.find(TerminationKey(Node, Scope));
		// 判定していない文を読む事は整形器の不具合とする
		if(Found == Known.end()) {
			throw FormatLimitExceeded("termination of an unvisited statement was read", FailureReason::FormatterDefect);
		}
		// 判定済の終端の返戻
		return Found->second;
	}

	/**
	 * 文を直下に並べる節点の文の取得関数
	 * @param Container 文を直下に並べる節点
	 * @return 注記と後送された本文を除く文（実行順）
	 */
	std::vector<NodeId> ControlFlow::StatementsIn(const NodeId Container) const {
		std::vector<NodeId> Statements;
		ForEachStatementOf(
			Syntax,
			Container,
			[&](const NodeId Child) -> void {
				Statements.push_back(Child);
			}
		);
		// 文の並びの返戻
		return Statements;
	}

	/**
	 * 前処理分岐の文の取得関数
	 * @param Node 前処理分岐
	 * @return 条件と代替枝とコメントを除く名前付の子（#else は条件を持たない）
	 */
	std::vector<NodeId> ControlFlow::PreprocessorStatements(const NodeId Node) const {
		const NodeId Alternative = Syntax.FieldChild(Node, ChildRole::Alternative);
		std::vector<NodeId> Statements;
		bool IsGuarded = !Syntax.Is(Node, Role::PreprocElse);
		ForEachNamedChildOf(
			Syntax,
			Node,
			[&](const NodeId Child) -> void {
				// コメントと後の分岐（`#else` 等）の子の場合の終了
				if(Syntax.Is(Child, Role::Comment) || Child == Alternative) return;
				// 最初の子は分岐の条件
				if(IsGuarded) IsGuarded = false;
				else Statements.push_back(Child);
			}
		);
		// 分岐の文の返戻
		return Statements;
	}

	/**
	 * 分岐文の枝の取得関数
	 * @param Node 分岐文
	 * @return 真枝，後続の枝の本体及び条件を問わない最後の枝の有無
	 */
	ControlFlow::BranchParts ControlFlow::BranchesOf(const NodeId Node) const {
		BranchParts Parts;
		Parts.Then = Syntax.IfConsequence(Node);
		if(Parts.Then == NoNode) {
			// 場を持たない Swift の分岐は最初の文の並びが真枝，`else` の後の子が偽枝
			bool IsAfterElse = false;
			ForEachChildOf(
				Syntax,
				Node,
				[&](const NodeId Child) -> void {
					if(Syntax.Is(Child, Role::Else)) IsAfterElse = true;
					else if(IsAfterElse && Syntax.IsNamed(Child) && !Syntax.Is(Child, Role::Comment)) {
						Parts.Alternatives.push_back(Child);
						Parts.HasElse = true;
						IsAfterElse = false;
					} else if(Parts.Then == NoNode && Syntax.Is(Child, Role::ValueStatementSequence)) Parts.Then = Child;
				}
			);
		} else for(uint32_t Idx = 0, Count = Syntax.ChildCount(Node); Idx < Count; ++Idx) {
			Syntax.Charge(1);
			// 後続の枝の場
			NodeId Branch = Syntax.Child(Node, Idx);
			if(!Syntax.HasField(Branch, ChildRole::Alternative)) continue;
			// 条件を持つ枝（Python の elif・PHP の elseif）は続く枝を要し，其の本体を枝とする
			if(Syntax.Is(Branch, Role::ConditionalBranchClause)) {
				Branch = Syntax.IfConsequence(Branch);
				Parts.HasElse = false;
			} else {
				// else 節は包みを外した本体を枝とする
				if(Syntax.Is(Branch, Role::ElseAny)) {
					const NodeId Inner = Syntax.FieldChild(Branch, ChildRole::Body);
					Branch = Inner == NoNode ? Syntax.NamedChild(Branch, 0) : Inner;
				}
				Parts.HasElse = true;
			}
			Parts.Alternatives.push_back(Branch);
		}
		// 分岐文の枝の返戻
		return Parts;
	}

	/**
	 * 例外処理文の節の取得関数
	 * @param Node 例外処理文（Swift の do は捕捉の節を持つ）
	 * @return 本体，例外の無かった時の節，後始末の節及び捕捉の節の本体
	 */
	ControlFlow::TryParts ControlFlow::TryPartsOf(const NodeId Node) const {
		// 節の本体の取得関数（場の無い文法は最後の名前付の子）
		const auto ClauseBody = [this](const NodeId Clause) -> NodeId {
			const NodeId Inner = Syntax.FieldChild(Clause, ChildRole::Body);
			// 本体の返戻
			return Inner == NoNode ? Syntax.LastNamedChild(Clause) : Inner;
		};
		TryParts Parts;
		Parts.Body = Syntax.FieldChild(Node, ChildRole::Body);
		ForEachNamedChildOf(
			Syntax,
			Node,
			[&](const NodeId Child) -> void {
				// Kotlin / Swift の本体は最初の文の並び
				if(Parts.Body == NoNode && Syntax.Is(Child, Role::ValueStatementSequence)) Parts.Body = Child;
				else if(Syntax.Is(Child, Role::CatchClause)) Parts.Handlers.push_back(ClauseBody(Child));
				else if(Syntax.Is(Child, Role::FinallyClause)) Parts.Finally = ClauseBody(Child);
				else if(Syntax.Is(Child, Role::ElseClause)) Parts.Else = ClauseBody(Child);
			}
		);
		// 例外処理文の節の返戻
		return Parts;
	}

	/**
	 * 切替文の節の取得関数
	 * @param Node 切替文
	 * @return 本体と，本体の直下に並ぶ節（節の外の名前付の子，文の内側の節又は前処理分岐を含む場合は節を空とする）
	 */
	std::pair<NodeId, std::vector<NodeId>> ControlFlow::SwitchCases(const NodeId Node) const {
		// 切替本体の抽出
		NodeId Body = Syntax.FieldChild(Node, ChildRole::Body);
		if(Body == NoNode) {
			// 節を直接の子に並べる切替
			if(Syntax.Is(Node, Role::DirectSwitchArms)) Body = Node;
			else {
				ForEachNamedChildOf(
					Syntax,
					Node,
					// 切替本体節点の採用関数
					[&](const NodeId Child) -> void {
						if(Syntax.Is(Child, Role::SwitchBody)) Body = Child;
					}
				);
			}
		}
		std::vector<NodeId> Cases;
		// 本体の無い切替文の返戻
		if(Body == NoNode) return { Body, Cases };
		bool IsInside = Body != Node, IsUnknown = false;
		// 分岐構造の前順の収集（節点と本体からの深さ）
		std::vector<std::pair<NodeId, uint32_t>> Stack { { Body, 0 } };
		while(!Stack.empty() && !IsUnknown) {
			const auto [Child, Depth] = Stack.back();
			Stack.pop_back();
			Syntax.Charge(1);
			const bool IsDirect = Depth == 1, IsRoot = !Depth;
			if(IsDirect && Syntax.IsTokenRole(Child, Role::OpeningBraceToken)) IsInside = true;
			if(!IsRoot && IsInside && Syntax.IsNamed(Child) && !Syntax.Is(Child, Role::Comment)) {
				if(const bool IsCase = Syntax.Is(Child, Role::CaseClause); IsCase && IsDirect) Cases.push_back(Child);
				else if(IsCase || IsDirect) IsUnknown = true;
			}
			// C/C++ の case は文の内側にも置けるが，ネスト switch・関数には非所属
			if(
				!IsUnknown &&
				(IsRoot || Syntax.Rules().CaseMayNestInStatement && !Syntax.Is(Child, Role::SwitchStatement) && !IsFlowBoundary(Child))
			) {
				const std::span<const NodeId> Children = Syntax.ChildrenOf(Child);
				for(std::span<const NodeId>::reverse_iterator Next = Children.rbegin(); Next != Children.rend(); ++Next) {
					Stack.emplace_back(*Next, Depth + 1);
				}
			}
		}
		// ネストした case 又は前処理分岐を含む場合の全節終端認定の回避
		if(IsUnknown) Cases.clear();
		// 本体と節の返戻
		return { Body, std::move(Cases) };
	}

	/**
	 * 次の節へ進む文かの判定関数
	 * @param Node 判定する文
	 * @return 独立した語又は文で次の節へ進むなら true
	 */
	bool ControlFlow::IsFallthroughStatement(const NodeId Node) const {
		// 素通りの文かの返戻
		return Syntax.Is(Node, Role::BareFallthrough) || Syntax.Is(Node, Role::FallthroughStatement);
	}

	/**
	 * 切替の節の本文の取得関数
	 * @param Case 切替の節
	 * @return 本文を開く記号より後の文（素通りの文を含む）と本文を開く記号の有無・種類
	 */
	ControlFlow::CaseBody ControlFlow::CaseBodyOf(const NodeId Case) const {
		CaseBody Body;
		ForEachChildOf(
			Syntax,
			Case,
			[&](const NodeId Child) -> void {
				// ラベルを本文に数えない場合の終了
				if(Syntax.Is(Child, Role::SwitchLabel)) return;
				if(!Body.IsOpened) {
					if(
						Syntax.Is(Child, Role::CaseBodyOpener) || Syntax.Rules().CaseOpensWithStatementEnd && Syntax.IsWord(Child, Word::StatementEnd)
					) {
						Body.IsOpened = true;
						Body.IsArrow = Syntax.IsTokenRole(Child, Role::RightArrowToken);
					}
				} else if(IsFallthroughStatement(Child) || Syntax.IsNamed(Child) && !Syntax.Is(Child, Role::Comment)) {
					Body.Statements.push_back(Child);
				}
			}
		);
		// 節の本文の返戻
		return Body;
	}

	/**
	 * 全ての値に一致する照合かの判定関数
	 * @param Pattern 節の照合（Python の case の照合）
	 * @return 読捨の照合 `_` か，名前１つの捕捉の照合なら true
	 */
	bool ControlFlow::IsIrrefutablePattern(const NodeId Pattern) const {
		// 部品１つでない照合の返戻
		if(Syntax.ChildCount(Pattern) != 1) return false;
		const NodeId Part = Syntax.Child(Pattern, 0);
		// 読捨の字句か，名前１つだけの捕捉かの返戻
		return Syntax.Is(Part, Role::WildcardToken) || Syntax.Is(Part, Role::Identifier) ||
		Syntax.ChildCount(Part) == 1 && Syntax.Is(Syntax.Child(Part, 0), Role::Identifier);
	}

	/**
	 * 不一致の値を受ける切替の節かの判定関数（整形仕様 5.7，既定の節の省略だけを網羅の根拠にしない）
	 * @param Case 切替の節
	 * @return 既定の枝，既定の語・else の語を持つ節，又は条件の無い全てに一致する照合の節なら true
	 */
	bool ControlFlow::IsDefaultCase(const NodeId Case) const {
		// 既定の枝の節点の返戻
		if(Syntax.Is(Case, Role::DefaultCase)) return true;
		if(const NodeId Pattern = Syntax.FirstNamedChildWithRole(Case, Role::CasePattern); Pattern != NoNode) {
			// 照合を持つ節の，条件の無い全てに一致する照合かの返戻
			return Syntax.FieldChild(Case, ChildRole::Guard) == NoNode && IsIrrefutablePattern(Pattern);
		}
		// 既定の語・else の語を直接の子又は節の見出の先頭に持つかの返戻
		return AnyChildOf(
			Syntax,
			Case,
			[this](const NodeId Child) -> bool {
				const NodeId Head = Syntax.Is(Child, Role::SwitchLabel) ? FirstChildNode(Child) : Child;
				// 既定を示す語かの返戻
				return Syntax.IsWord(Head, Word::Default) || Syntax.Is(Head, Role::ElseKeyword);
			}
		);
	}

	/**
	 * 処理系が網羅を検査する照合の切替の節かの判定関数（整形仕様 5.7）
	 * @param Case 切替の節
	 * @return 網羅を要求する照合・値（Java の型の照合と `case null`）を持つ枝のラベルを持てば true
	 */
	bool ControlFlow::RequiresExhaustiveness(const NodeId Case) const {
		// 網羅を要求する照合・値を持つ枝のラベルの有無の返戻
		return AnyChildOf(
			Syntax,
			Case,
			[this](const NodeId Label) -> bool {
				// 網羅を要求する照合・値を持つ枝のラベルかの返戻
				return Syntax.Is(Label, Role::SwitchLabel) && AnyChildOf(
					Syntax,
					Label,
					[this](const NodeId Part) -> bool {
						// 網羅を要求する照合・値かの返戻
						return Syntax.Is(Part, Role::ExhaustiveCaseLabel);
					}
				);
			}
		);
	}

	/**
	 * 次の枝へ進む語の属する切替の節の取得関数
	 * @param Node 判定する節点
	 * @param Scope 内部転送を認める切替文の判定範囲
	 * @return 文の並びに置いた Swift の `fallthrough` を囲む最も内の切替の節（其の他は無し）
	 */
	NodeId ControlFlow::FallthroughEntry(const NodeId Node, const NodeId Scope) const {
		// 次の枝へ進む語でない事の返戻
		if(
			Scope == NoNode || !Syntax.Is(Node, Role::FallthroughName) || !Syntax.IsWord(Node, Word::Fallthrough) ||
			!Syntax.Is(Syntax.ParentOf(Node), Role::ValueStatementSequence)
		) return NoNode;
		for(NodeId Parent = Syntax.ParentOf(Node); Parent != NoNode; Parent = Syntax.ParentOf(Parent)) {
			Syntax.Charge(1);
			// 囲む切替の節の返戻
			if(Syntax.Is(Parent, Role::SwitchEntry)) return Parent;
		}
		// 切替の外の語の返戻
		return NoNode;
	}

	/**
	 * 終端の判定規則の取得関数（子を積む段と結果を合わせる段が同じ規則を選ぶ）
	 * @param Node 判定する節点
	 * @param Scope 内部転送を認める切替文の判定範囲
	 * @return 節点に適用する規則
	 */
	ControlFlow::TerminationRule ControlFlow::TerminationRuleOf(const NodeId Node, const NodeId Scope) const {
		// 節点の無い事の返戻
		if(Node == NoNode) return TerminationRule::None;
		// 式の文は中の式で判定する事の返戻
		if(Syntax.Is(Node, Role::ExpressionStatement)) return TerminationRule::Expression;
		// 返戻・送出・処理の終了と，飛先を問わず次の文へ進まない goto は終端する事の返戻
		if(Syntax.Is(Node, Role::ReturnOrThrow) || Syntax.Is(Node, Role::GotoStatement)) return TerminationRule::Transfer;
		// ラベル付の文の返戻
		if(Syntax.Is(Node, Role::LabeledStatement)) return TerminationRule::Labeled;
		// 次の枝へ進む語の返戻
		if(FallthroughEntry(Node, Scope) != NoNode) return TerminationRule::Fallthrough;
		// Kotlin / Swift は return・throw・break 等を同じ節点に包む為，先頭の制御語を区別する事の返戻
		if(Syntax.Is(Node, Role::KeywordJump)) return TerminationRule::KeywordJump;
		// 戻らない呼出は終端する事の返戻
		if(IsNoReturnCall(Node)) return TerminationRule::Transfer;
		// 条件で終わらない繰返の返戻
		if(NodeId Body = NoNode; IsInfiniteLoop(Node, Body)) return TerminationRule::InfiniteLoop;
		// 其の他の繰返の返戻
		if(Syntax.Is(Node, Role::Loop)) return TerminationRule::Loop;
		// 文列の返戻
		if(Syntax.Is(Node, Role::StatementFlowContainer)) return TerminationRule::Statements;
		// 本体を一度だけ実行する文の返戻
		if(Syntax.Is(Node, Role::SingleBodyStatement)) return TerminationRule::SingleBody;
		// 前処理分岐の返戻
		if(Syntax.Is(Node, Role::PreprocessorBranch)) return TerminationRule::Preprocessor;
		// 分岐文の返戻
		if(Syntax.Is(Node, Role::If)) return TerminationRule::Branch;
		// 例外処理文の返戻
		if(Syntax.Is(Node, Role::TryStatement) || Syntax.Is(Node, Role::CatchableBody)) return TerminationRule::Try;
		// 構文が異なる切替文も同じ全分岐判定へ渡す事の返戻
		if(IsArmSwitch(Node)) return TerminationRule::Switch;
		// 上記何れにも非該当の節点の返戻
		return TerminationRule::None;
	}

	/**
	 * 本体を一度だけ実行する文の本体の取得関数
	 * @param Node 本体を一度だけ実行する文
	 * @return 本体の場の子（無ければ最後の名前付の子）
	 */
	NodeId ControlFlow::SingleBodyOf(const NodeId Node) const {
		const NodeId Inner = Syntax.FieldChild(Node, ChildRole::Body);
		// 本体の返戻
		return Inner == NoNode ? Syntax.LastNamedChild(Node) : Inner;
	}

	/**
	 * 終端の判定が読む子の収集関数
	 * 規則が短絡で読まずに済ませ得る子も含め，規則が読み得る子を全て積む
	 * @param Node 判定する節点
	 * @param Scope 内部転送を認める切替文の判定範囲
	 * @param Out 子と其の判定範囲の格納先
	 */
	void ControlFlow::TerminationOperands(
		const NodeId Node,
		const NodeId Scope,
		std::vector<std::pair<NodeId, NodeId>> &Out
	) const {
		// 判定対象の子の追加関数
		const auto Add = [&Out, Scope](const NodeId Child) -> void {
			Out.emplace_back(Child, Scope);
		};
		// 規則が読み得る子の収集
		if(const TerminationRule Rule = TerminationRuleOf(Node, Scope); Rule == TerminationRule::Expression) {
			Add(Syntax.NamedChild(Node, 0));
		} else if(Rule == TerminationRule::Labeled) {
			if(const uint32_t Count = Syntax.NamedChildCount(Node)) Add(Syntax.NamedChild(Node, Count - 1));
		} else if(Rule == TerminationRule::Loop) {
			if(const NodeId Else = Syntax.FieldChild(Node, ChildRole::Alternative); Syntax.Is(Node, Role::LoopElse) && Else != NoNode) {
				Add(Syntax.LastNamedChild(Else));
			}
		} else if(Rule == TerminationRule::Statements) for(const NodeId Child : StatementsIn(Node)) Add(Child);
		else if(Rule == TerminationRule::SingleBody) Add(SingleBodyOf(Node));
		else if(Rule == TerminationRule::Preprocessor) {
			for(const NodeId Child : PreprocessorStatements(Node)) Add(Child);
			Add(Syntax.FieldChild(Node, ChildRole::Alternative));
		} else if(Rule == TerminationRule::Branch) {
			const BranchParts Parts = BranchesOf(Node);
			Add(Parts.Then);
			for(const NodeId Branch : Parts.Alternatives) Add(Branch);
		} else if(Rule == TerminationRule::Try) {
			const TryParts Parts = TryPartsOf(Node);
			for(const NodeId Part : { Parts.Finally, Parts.Body, Parts.Else }) Add(Part);
			for(const NodeId Handler : Parts.Handlers) Add(Handler);
		} else if(Rule == TerminationRule::Switch) {
			// 節の本文は外側の切替文（単独判定時は此の切替文）を判定範囲とする
			const NodeId InnerScope = Scope == NoNode ? Node : Scope;
			for(const NodeId Case : SwitchCases(Node).second) {
				for(const NodeId Child : CaseBodyOf(Case).Statements) if(!IsFallthroughStatement(Child)) Out.emplace_back(Child, InnerScope);
			}
		}
		// 終了
		return;
	}

	/**
	 * 文の並びの終端判定関数
	 * @param Statements 実行順の文
	 * @param Gotos 同じ関数内の goto 飛先
	 * @param Scope 内部転送を認める切替文の判定範囲
	 * @param Known 判定範囲毎の節点の終端の判定結果
	 * @return 最後の入口より後の文の何れかが終端すれば true
	 */
	bool ControlFlow::DoStatementsTerminate(
		const std::span<const NodeId> Statements,
		const GotoIndex &Gotos,
		const NodeId Scope,
		const TerminationResults &Known
	) const {
		// 当該経路の終端済有無
		bool IsClosed = false;
		for(const NodeId Child : Statements) {
			Syntax.Charge(1);
			// 文毎の終端状態更新（ラベルへの goto は終端後の文へ入口を作る）
			if(HasGotoEntry(Child, Gotos)) IsClosed = false;
			if(!IsClosed) IsClosed = KnownTermination(Known, Child, Scope);
		}
		// 文列を素通り出来ない場合に終端とする事の返戻
		return IsClosed;
	}

	/**
	 * 切替文の全分岐終端判定関数
	 * 記述した全ての節が終端し，不一致の値も既定の節か処理系の網羅・送出で後続へ進まない切替を終端とする（整形仕様 5.7）
	 * 不一致の値が来ないと看做す複製では，記述した全ての節が終端する切替を終端とする
	 * @param Node 切替文
	 * @param Gotos 同じ関数内の goto 飛先
	 * @param TransferScope 外側の切替文の判定範囲（単独判定時は空）
	 * @param Known 判定範囲毎の節点の終端の判定結果
	 * @param Index 同じ終端判定で共有する脱出の索引
	 * @return 素通り・外部脱出の経路が無ければ true
	 */
	bool ControlFlow::DoesSwitchTerminate(
		const NodeId Node,
		const GotoIndex &Gotos,
		const NodeId TransferScope,
		const TerminationResults &Known,
		LoopExitIndex &Index
	) const {
		const auto [Body, Cases] = SwitchCases(Node);
		// 本体の無い切替文は終端しない事の返戻
		if(Body == NoNode) return false;
		// 移動先を探索する範囲
		const NodeId Scope = TransferScope == NoNode ? Node : TransferScope;
		// 本体から抜け出る切替文と，判定出来る節の無い切替文は終端しない事の返戻
		if(Cases.empty() || HasLoopExit(Body, Node, Gotos, Index, Scope)) return false;
		const SyntaxRules &Rules = Syntax.Rules();
		bool DoesNextTerminate = false;
		// 各節の終端と素通り先の判定
		for(std::vector<NodeId>::const_reverse_iterator Iter = Cases.rbegin(); Iter != Cases.rend(); ++Iter) {
			Syntax.Charge(1);
			const CaseBody Case = CaseBodyOf(*Iter);
			// 本文の無い節は終端しない事の返戻
			if(!Case.IsOpened) return false;
			// 分岐本文の閉鎖状態
			bool IsClosed = false;
			for(const NodeId Child : Case.Statements) {
				if(IsFallthroughStatement(Child)) IsClosed = DoesNextTerminate;
				else {
					if(HasGotoEntry(Child, Gotos)) IsClosed = false;
					if(!IsClosed) IsClosed = KnownTermination(Known, Child, Scope);
				}
			}
			// 閉じない節（素通りする先の節も閉じない物を含む）が有れば終端しない事の返戻
			if(
				const bool IsFallthrough = Rules.CaseFallsThrough || Rules.ColonCaseFallsThrough && !Case.IsArrow ||
				Rules.EmptyCaseFallsThrough && Case.Statements.empty();
				!IsClosed && !(IsFallthrough && DoesNextTerminate)
			) return false;
			// 前節から素通りする到達先としての終端扱い
			DoesNextTerminate = true;
		}
		// 全節が閉じ，不一致の値も後続へ進まない事の返戻（網羅を要求する照合の節を持つ切替は処理系が網羅を検査する）
		return AssumesCaseCoverage || Syntax.Is(Node, Role::ExhaustiveSwitch) || std::any_of(
			Cases.begin(),
			Cases.end(),
			[this](const NodeId Case) -> bool {
				// 不一致の値を受ける節か，網羅を要求する照合の節かの返戻
				return IsDefaultCase(Case) || RequiresExhaustiveness(Case);
			}
		);
	}

	/**
	 * 分岐文の全枝終端判定関数
	 * @param Node 分岐文
	 * @param Gotos 同じ関数内の goto 飛先
	 * @param TransferScope 内部転送を認める切替文の判定範囲
	 * @param Known 判定範囲毎の節点の終端の判定結果
	 * @param Index 同じ終端判定で共有する脱出の索引
	 * @return 条件を問わない最後の枝を持ち，全ての枝が終端すれば true
	 */
	bool ControlFlow::DoesIfTerminate(
		const NodeId Node,
		const GotoIndex &Gotos,
		const NodeId TransferScope,
		const TerminationResults &Known,
		LoopExitIndex &Index
	) const {
		const BranchParts Parts = BranchesOf(Node);
		// 条件を問わない枝が無ければ素通り出来る事の返戻
		if(!Parts.HasElse || Parts.Then == NoNode) return false;
		// 一方でも素通り出来る場合は非終端の返戻
		if(!KnownTermination(Known, Parts.Then, TransferScope)) return false;
		for(const NodeId Branch : Parts.Alternatives) {
			Syntax.Charge(1);
			// 後続の枝の一つでも素通り出来る場合の非終端の返戻
			if(!KnownTermination(Known, Branch, TransferScope)) return false;
		}
		// Swift のラベルは if の前の兄弟なので，両枝の return を迂回する脱出も確認
		if(Syntax.Is(Node, Role::SiblingLabeledBranch)) {
			NodeId Previous = Syntax.PrevNamedSibling(Node);
			while(Previous != NoNode && Syntax.Is(Previous, Role::Comment)) {
				Syntax.Charge(1);
				Previous = Syntax.PrevNamedSibling(Previous);
			}
			if(Previous != NoNode && Syntax.Is(Previous, Role::StatementLabel)) {
				// 自身のラベルへ脱出しない場合のみ終端の返戻
				return !HasLoopExit(Node, Node, Gotos, Index, TransferScope);
			}
		}
		// 全ての枝が終端する場合の返戻
		return true;
	}

	/**
	 * 例外処理文の終端判定関数
	 * @param Node 例外処理文
	 * @param TransferScope 内部転送を認める切替文の判定範囲
	 * @param Known 判定範囲毎の節点の終端の判定結果
	 * @return 後始末の節が終端するか，本体と全ての捕捉の節が終端すれば true
	 */
	bool ControlFlow::DoesTryTerminate(const NodeId Node, const NodeId TransferScope, const TerminationResults &Known) const {
		const TryParts Parts = TryPartsOf(Node);
		// 後始末の節が終端すれば全体も終端する事の返戻
		if(KnownTermination(Known, Parts.Finally, TransferScope)) return true;
		// 本体（Python は例外の無かった時の節を含む）が素通り出来れば終端しない事の返戻
		if(!KnownTermination(Known, Parts.Body, TransferScope) && !KnownTermination(Known, Parts.Else, TransferScope)) return false;
		// 全ての捕捉の節の終端の返戻
		return std::all_of(
			Parts.Handlers.begin(),
			Parts.Handlers.end(),
			[&](const NodeId Handler) -> bool {
				Syntax.Charge(1);
				// 捕捉の節の終端の返戻
				return KnownTermination(Known, Handler, TransferScope);
			}
		);
	}

	/**
	 * 判定済の子からの終端判定関数
	 * @param Node 判定対象ノード
	 * @param Gotos 同じ関数内の goto 飛先
	 * @param Scope 内部転送を認める切替文の判定範囲
	 * @param Known 判定範囲毎の節点の終端の判定結果（規則が読む子を全て含む）
	 * @param Index 同じ終端判定で共有する脱出の索引
	 * @return 終端する場合 true
	 */
	bool ControlFlow::TerminatesWith(
		const NodeId Node,
		const GotoIndex &Gotos,
		const NodeId Scope,
		const TerminationResults &Known,
		LoopExitIndex &Index
	) const {
		const TerminationRule Rule = TerminationRuleOf(Node, Scope);
		// 式の文の中の式の終端の返戻
		if(Rule == TerminationRule::Expression) return KnownTermination(Known, Syntax.NamedChild(Node, 0), Scope);
		// 制御を転送する文の返戻
		if(Rule == TerminationRule::Transfer) return true;
		if(Rule == TerminationRule::Labeled) {
			const uint32_t Count = Syntax.NamedChildCount(Node);
			// 文を持たないラベルは終端しない事の返戻
			if(!Count) return false;
			const NodeId Child = Syntax.NamedChild(Node, Count - 1);
			// ラベル末尾が常に転送し且つループを抜けないかの返戻
			return KnownTermination(Known, Child, Scope) && !HasLoopExit(Child, Node, Gotos, Index, Scope);
		}
		if(Rule == TerminationRule::Fallthrough) {
			NodeId Next = Syntax.NextNamedSibling(FallthroughEntry(Node, Scope));
			while(Next != NoNode && Syntax.Is(Next, Role::Comment)) {
				Syntax.Charge(1);
				Next = Syntax.NextNamedSibling(Next);
			}
			// 次節が有る場合だけ内部転送とする事の返戻
			return Next != NoNode && Syntax.Is(Next, Role::SwitchEntry) && Syntax.Contains(Scope, Next);
		}
		// 先頭の制御語が脱出の語かの返戻
		if(Rule == TerminationRule::KeywordJump) return Syntax.Is(FirstChildNode(Node), Role::ExitKeyword);
		if(Rule == TerminationRule::InfiniteLoop) {
			NodeId Body = NoNode;
			IsInfiniteLoop(Node, Body);
			// 脱出の無い無限ループとして終端の返戻
			return !HasLoopExit(Body, Node, Gotos, Index, Scope);
		}
		if(Rule == TerminationRule::Loop) {
			// Python の繰返の else 節は脱出せずに終えた時だけ実行する
			const NodeId Else = Syntax.FieldChild(Node, ChildRole::Alternative);
			// else 節が終端し脱出が無い場合だけ終端とする事の返戻
			return Syntax.Is(Node, Role::LoopElse) && Else != NoNode && KnownTermination(Known, Syntax.LastNamedChild(Else), Scope) &&
			!HasLoopExit(Syntax.FieldChild(Node, ChildRole::Body), Node, Gotos, Index, Scope);
		}
		if(Rule == TerminationRule::Statements) {
			// 文列は順に判定し，終端後の宣言・文で終端性を保持
			// Rust のラベル付の塊は自身への脱出で素通りする事の返戻
			return DoStatementsTerminate(StatementsIn(Node), Gotos, Scope, Known) && (
				!Syntax.Is(Node, Role::LabeledBlockExit) || Syntax.FirstNamedChildWithRole(Node, Role::JumpLabel) == NoNode ||
				!HasLoopExit(Node, Node, Gotos, Index, Scope)
			);
		}
		// 本体を一度だけ実行する文の本体の終端の返戻
		if(Rule == TerminationRule::SingleBody) return KnownTermination(Known, SingleBodyOf(Node), Scope);
		if(Rule == TerminationRule::Preprocessor) {
			const bool IsClosed = DoStatementsTerminate(PreprocessorStatements(Node), Gotos, Scope, Known);
			// #else 以外は後続の枝が無いと全条件不成立で素通り出来る事の返戻
			return IsClosed &&
			(Syntax.Is(Node, Role::PreprocElse) || KnownTermination(Known, Syntax.FieldChild(Node, ChildRole::Alternative), Scope));
		}
		// 全ての枝の終端の返戻
		if(Rule == TerminationRule::Branch) return DoesIfTerminate(Node, Gotos, Scope, Known, Index);
		// 例外処理文の判定結果の返戻
		if(Rule == TerminationRule::Try) return DoesTryTerminate(Node, Scope, Known);
		// 切替文の全分岐判定の返戻
		if(Rule == TerminationRule::Switch) return DoesSwitchTerminate(Node, Gotos, Scope, Known, Index);
		// 上記何れにも非該当のノードの非終端の返戻
		return false;
	}

	/**
	 * 終端の判定結果の構築関数（整形仕様 9.2，深い構文は明示した作業列で辿る）
	 * 規則が読み得る子を作業列で辿り，子から親の順に規則を適用して判定範囲毎に控える
	 * 計算量：規則が読み得る節点の数 V に対し O(V)（控えの参照は期待値で定数）
	 * @param Roots 判定する節点
	 * @param Gotos 同じ関数内の goto 飛先
	 * @param Scope 内部転送を認める切替文の判定範囲
	 * @return 判定範囲毎の節点の終端の判定結果
	 */
	ControlFlow::TerminationResults ControlFlow::EvaluateTermination(
		const std::span<const NodeId> Roots,
		const GotoIndex &Gotos,
		const NodeId Scope
	) const {
		// 判定する節点と判定範囲と，其の子を積み終えたか
		struct Pending {
			NodeId Node;
			NodeId Scope;
			bool IsExpanded;
		};
		TerminationResults Known;
		LoopExitIndex Index;
		std::vector<Pending> Work;
		std::vector<std::pair<NodeId, NodeId>> Operands;
		for(const NodeId Root : Roots) Work.push_back({ Root, Scope, false });
		// 子を先に評価する作業列の走査
		while(!Work.empty()) {
			const Pending Current = Work.back();
			Work.pop_back();
			if(Current.Node == NoNode || Known.contains(TerminationKey(Current.Node, Current.Scope))) continue;
			if(Current.IsExpanded) {
				// 子を判定し終えた節点への規則の適用
				Known.emplace(TerminationKey(Current.Node, Current.Scope), TerminatesWith(Current.Node, Gotos, Current.Scope, Known, Index));
				continue;
			}
			Syntax.Charge(1);
			Work.push_back({ Current.Node, Current.Scope, true });
			Operands.clear();
			TerminationOperands(Current.Node, Current.Scope, Operands);
			for(const auto &[Child, ChildScope] : Operands) Work.push_back({ Child, ChildScope, false });
		}
		// 判定結果の返戻
		return Known;
	}

	/**
	 * 文を直下に並べる節点の終端判定関数
	 * 関数本体の文列は節点の役割を問わず直下の文を判定する
	 * @param Container 文を直下に並べる節点
	 * @param Gotos 同じ関数内の goto 飛先
	 * @return 最後の入口より後の文の何れかが終端すれば true
	 */
	bool ControlFlow::DoStatementsTerminate(const NodeId Container, const GotoIndex &Gotos) const {
		const std::vector<NodeId> Statements = StatementsIn(Container);
		// 文列を素通り出来ない場合に終端とする事の返戻
		return DoStatementsTerminate(Statements, Gotos, NoNode, EvaluateTermination(Statements, Gotos, NoNode));
	}

	/**
	 * 終端した後の返戻文を含む文の取得関数（整形仕様 8.4）
	 * 文の並びの終端を一度だけ判定し，最後の入口より後で終端した文に続く，返戻文を含む文を集める
	 * 計算量：規則が読み得る節点の数 V に対し O(V)
	 * @param Container 文を直下に並べる節点
	 * @param Gotos 同じ関数内の goto 飛先
	 * @return 到達しない返戻文を含む文（原文順）
	 */
	std::vector<NodeId> ControlFlow::ReturningStatementsAfterTermination(const NodeId Container, const GotoIndex &Gotos) const {
		const std::vector<NodeId> Statements = StatementsIn(Container);
		const TerminationResults Known = EvaluateTermination(Statements, Gotos, NoNode);
		std::vector<NodeId> Unreachable;
		bool IsClosed = false;
		for(const NodeId Child : Statements) {
			Syntax.Charge(1);
			// 文毎の終端状態更新（ラベルへの goto は終端後の文へ入口を作る）
			if(HasGotoEntry(Child, Gotos)) IsClosed = false;
			if(IsClosed && HasReturnInFlow(Child)) Unreachable.push_back(Child);
			if(!IsClosed) IsClosed = KnownTermination(Known, Child, NoNode);
		}
		// 到達しない返戻文を含む文の返戻
		return Unreachable;
	}

	/** ========== 通常関数の分類 ========== */
	/**
	 * 通常関数の節点かの判定関数
	 * 名前を持つ関数・メソッドの定義，コンストラクタ，デストラクタ及びアクセサを通常関数とし，ラムダ・無名関数・初期化の塊を含めない
	 * @param Type 判定する節点
	 * @return 通常関数の節点なら true
	 */
	bool ControlFlow::IsOrdinaryFunction(const NodeId Type) const {
		// 通常関数の節点の型かの返戻（Ruby は専用の規定に従う）
		return Syntax.Is(Type, Role::ExplicitResultFunction);
	}

	/**
	 * 通常関数の文の並びの取得関数
	 * @param Node 通常関数の節点
	 * @return 文を直下に並べる節点（本体が無いか式本体なら空）
	 */
	NodeId ControlFlow::StatementsOf(const NodeId Node) const {
		NodeId Body = Syntax.FieldChild(Node, ChildRole::Body);
		// 場を持たない Kotlin / Swift の関数本体の包み
		if(Body == NoNode) Body = Syntax.FirstNamedChildWithRole(Node, Role::FunctionBody);
		// 本体の包みを持たず波括弧を直接の子に持つ Kotlin の副コンストラクタ・Swift のアクセサ
		if(
			Body == NoNode && AnyChildOf(
				Syntax,
				Node,
				[this](const NodeId Child) -> bool {
					return Syntax.IsTokenRole(Child, Role::OpeningBraceToken);
				}
			)
		) Body = Node;
		// 本体が無いか，式本体（Kotlin の `=`・C# の `=>`）である事の返戻
		if(
			Body == NoNode || !Syntax.Is(Body, Role::FunctionBodyContainer) && Body != Node ||
			Syntax.HasWordChild(Body, Word::ExpressionBody)
		) return NoNode;
		// C++ の関数の try の塊は文の並びでない事の返戻
		if(Syntax.Is(Node, Role::CompoundFunctionBody) && !Syntax.Is(Body, Role::CompoundBody)) return NoNode;
		// Kotlin / Swift の `statements` と Go の `statement_list` が包む文の並びの返戻
		if(const NodeId Inner = Syntax.FirstNamedChildWithRole(Body, Role::InnerFunctionStatements); Inner != NoNode) return Inner;
		// 本体の文の並びの返戻
		return Body;
	}

	/**
	 * 生成器・コルーチンかの判定関数
	 * @param Node 通常関数の節点
	 * @param Statements 文を直下に並べる節点
	 * @return 本体が生成器・コルーチンとして実行されるなら true
	 */
	bool ControlFlow::IsGeneratorLike(const NodeId Node, const NodeId Statements) const {
		// JS/TS の `*` を持つメソッドと，本体の yield・C++ の co_ 系の文の有無の返戻
		return Syntax.Is(Node, Role::StarGeneratorFunction) && Syntax.HasTokenRoleChild(Node, Role::AsteriskToken) ||
		HasFlowDescendant(Statements, Role::GeneratorMarker);
	}

	/**
	 * 中身を持たない事を表す本体かの判定関数
	 * @param Statements 文を直下に並べる節点
	 * @return 文が無いか，Python の説明文字列・`pass`・`...` だけなら true
	 */
	bool ControlFlow::IsPlaceholderBody(const NodeId Statements) const {
		bool IsFirst = true;
		const bool AllowsPlaceholders = Syntax.Is(Statements, Role::PlaceholderBody);
		// 中身を表す文が無いかの返戻
		return !AnyChildOf(
			Syntax,
			Statements,
			[&](const NodeId Child) -> bool {
				// 注記と無名の字句は中身でない事の返戻
				if(!Syntax.IsNamed(Child) || Syntax.Is(Child, Role::Comment)) return false;
				const bool IsLeading = IsFirst;
				IsFirst = false;
				// Python 以外は何れの文も中身である事の返戻
				if(!AllowsPlaceholders) return true;
				NodeId Inner = Child;
				if(Syntax.Is(Inner, Role::ExpressionStatement)) Inner = Syntax.NamedChild(Inner, 0);
				// 最初の文字列は説明文字列で，`pass`・`...` は中身の無い事を表す事の返戻
				return !(
					IsLeading && Syntax.Is(Inner, Role::QuotedString) || Syntax.Is(Inner, Role::PlaceholderStatement) ||
					Syntax.Is(Inner, Role::Ellipsis) && Syntax.IsWord(Inner, Word::VariadicMarker)
				);
			}
		);
	}

	/**
	 * 戻らない事を宣言した C / C++ の関数かの判定関数
	 * @param Node 関数の節点
	 * @return `noreturn` 属性・`_Noreturn` 指定を持てば true
	 */
	bool ControlFlow::IsNoReturnDeclared(const NodeId Node) const {
		// 宣言子と本体の手前の指定・属性の中の語の探索結果の返戻
		return AnyChildOf(
			Syntax,
			Node,
			[&](const NodeId Child) -> bool {
				// 指定・属性でない子は対象外である事の返戻
				if(!Syntax.Is(Child, Role::DeclarationSpecifier)) return false;
				// 戻らない事を表す語の有無の返戻
				return AnyNodeOf(
					Syntax,
					Child,
					[&](const NodeId Inner) -> bool {
						// 戻らない事を表す語の字句かの返戻
						return !Syntax.ChildCount(Inner) && Syntax.IsWord(Inner, Word::NoReturnSpecifier);
					}
				);
			}
		);
	}

	/**
	 * C / C++ の起動点かの判定関数
	 * @param Node 関数の節点
	 * @return 名前空間に属さない `int main` の定義なら true
	 */
	bool ControlFlow::IsProgramEntry(const NodeId Node) const {
		const NodeId Declarator = Syntax.FieldChild(Node, ChildRole::Declarator);
		// 戻値の型と其の字句を区切無で繋いだ字面
		const NodeId TypeNode = Syntax.FieldChild(Node, ChildRole::Type);
		const std::string TypeName = Syntax.Text(TypeNode);
		// 関数宣言子の名前が main で戻値の型が int（`signed` と `signed int` を含む）かの判定
		// 起動点でない事の返戻
		if(
			!Syntax.Is(Declarator, Role::FunctionDeclarator) ||
			!Syntax.IsWord(Syntax.FieldChild(Declarator, ChildRole::Declarator), Word::EntryFunction) ||
			!(Syntax.Is(TypeNode, Role::PrimitiveType) || Syntax.Is(TypeNode, Role::SizedTypeSpecifier)) ||
			!Syntax.Rules().IsSpelling(TypeName, Word::EntryResultType)
		) return false;
		// 翻訳単位迄の祖先が外部結合の指定と前処理の分岐だけかの返戻
		for(NodeId Parent = Syntax.ParentOf(Node); Parent != NoNode; Parent = Syntax.ParentOf(Parent)) {
			Syntax.Charge(1);
			// 翻訳単位に達した起動点である事の返戻
			if(Syntax.Is(Parent, Role::TranslationUnit)) return true;
			const bool IsLocalScope = !Syntax.Is(Parent, Role::PreprocessorBranch) && !Syntax.Is(Parent, Role::LinkageSpecification) &&
			!Syntax.Is(Parent, Role::DeclarationList);
			// 名前空間・クラスに属する関数は起動点でない事の返戻
			if(IsLocalScope) return false;
		}
		// 翻訳単位に達しない事の返戻
		return false;
	}

	/**
	 * C / C++ の次の宣言子の取得関数
	 * @param Node 外側の宣言子又は関数の節点
	 * @return 仮引数・属性を除いた内側の宣言子（無ければ無し）
	 */
	NodeId ControlFlow::NextDeclarator(const NodeId Node) const {
		const NodeId Next = Syntax.FieldChild(Node, ChildRole::Declarator);
		// 宣言子の場の子の返戻
		if(Next != NoNode) return Next;
		for(const NodeId Child : Syntax.ChildrenOf(Node)) {
			Syntax.Charge(1);
			if(Syntax.IsNamed(Child) && (Syntax.Is(Child, Role::DeclaratorWrapper) || Syntax.Is(Child, Role::ParenthesizedDeclarator))) {
				// 名前を持たない括弧内の最初の宣言子の返戻
				return Child;
			}
		}
		// 内側の宣言子の無い事の返戻
		return NoNode;
	}

	/**
	 * C / C++ のポインタ・参照を返す関数かの判定関数
	 * @param Function 関数の節点
	 * @return 戻値の宣言子がポインタ又は参照なら true
	 */
	bool ControlFlow::ReturnsPointerOrReference(const NodeId Function) const {
		for(NodeId Node = NextDeclarator(Function); Node != NoNode; Node = NextDeclarator(Node)) {
			Syntax.Charge(1);
			// ポインタ・参照の宣言子が有る事の返戻
			if(Syntax.Is(Node, Role::PointerOrRefDeclarator)) return true;
		}
		// 間接返戻でない事の返戻
		return false;
	}

	/**
	 * 関数の戻値の型の取得関数
	 * 戻値の型の場，結果の場，型の場，型を宣言子に持つ変換関数，場を持たず仮引数の並びの後に置く型の順に探す
	 * @param Node 関数の節点
	 * @return 戻値の型を表す節点（型を省いた関数は無し）
	 */
	NodeId ControlFlow::ResultTypeOf(const NodeId Node) const {
		for(const ChildRole Field : { ChildRole::ReturnType, ChildRole::Result, ChildRole::Type }) {
			// 戻値の型を持つ場の子の返戻
			if(const NodeId Type = Syntax.FieldChild(Node, Field); Type != NoNode) return Type;
		}
		const NodeId Declarator = Syntax.FieldChild(Node, ChildRole::Declarator);
		// 型を宣言子の中に持つ変換関数の返戻
		if(Syntax.Is(Declarator, Role::OperatorCast)) return Declarator;
		NodeId Type = NoNode;
		bool IsAfterParameters = false;
		ForEachNamedChildOf(
			Syntax,
			Node,
			[&](const NodeId Child) -> void {
				if(Syntax.Is(Child, Role::FunctionValueParameters)) IsAfterParameters = true;
				else if(IsAfterParameters && Type == NoNode && Syntax.Is(Child, Role::PostParameterResultType)) Type = Child;
			}
		);
		// 仮引数の並びの後の最初の型の返戻
		return Type;
	}

	/**
	 * 式を本体とする関数かの判定関数
	 * @param Node 関数の節点
	 * @return 関数又は本体の包みが式の本体を導く字句（Kotlin・Ruby の `=` 等）を持てば true
	 */
	bool ControlFlow::HasExpressionBody(const NodeId Node) const {
		NodeId Body = Syntax.FieldChild(Node, ChildRole::Body);
		if(Body == NoNode) Body = Syntax.FirstNamedChildWithRole(Node, Role::FunctionBody);
		// 式の本体を導く字句の有無の返戻
		return Syntax.HasWordChild(Node, Word::ExpressionBody) || Body != NoNode && Syntax.HasWordChild(Body, Word::ExpressionBody);
	}

	/**
	 * 関数の戻値の区分の判定関数
	 * 操作の種類，戻らない事の宣言，戻値の型，型を省いた関数の既定の順に，文法の事実だけで決める
	 * @param Node 関数の節点
	 * @return 戻値の区分と其の根拠
	 */
	ReturnShape ControlFlow::ReturnShapeOf(const NodeId Node) const {
		const SyntaxRules &Rules = Syntax.Rules();
		// 本体の返戻文からの推論
		const auto Inferred = [&]() -> ReturnShape {
			NodeId Body = Syntax.FieldChild(Node, ChildRole::Body);
			if(Body == NoNode) Body = Syntax.FirstNamedChildWithRole(Node, Role::CompoundBody);
			// 値付の返戻文の有無に依る区分の返戻
			return { HasValueReturn(Body) ? ReturnKind::Value : ReturnKind::Void, ReturnEvidence::Inferred };
		};
		// 構築・破棄・設定・監視の操作が値を返さない事の返戻
		if(Syntax.Is(Node, Role::ResultlessRoutine)) return { ReturnKind::Void, ReturnEvidence::Declared };
		// 読出の操作が値を返す事の返戻
		if(Syntax.Is(Node, Role::ReaderRoutine)) return { ReturnKind::Value, ReturnEvidence::Declared };
		if(Syntax.Is(Node, Role::NamedAccessor)) {
			// 読出の名前の操作だけが値を返す事の返戻
			return {
				Syntax.IsWord(Syntax.FieldChild(Node, ChildRole::Name), Word::Getter) ? ReturnKind::Value : ReturnKind::Void,
				ReturnEvidence::Declared
			};
		}
		// 戻らない事の宣言の返戻
		if(IsNoReturnDeclared(Node)) return { ReturnKind::Never, ReturnEvidence::Declared };
		// ポインタ・参照を返す宣言子の返戻
		if(ReturnsPointerOrReference(Node)) return { ReturnKind::Value, ReturnEvidence::Declared };
		NodeId Type = ResultTypeOf(Node);
		if(Type == NoNode) {
			// 型を省いた関数が整数を返す事の返戻
			if(Rules.OmittedResult == OmittedResultNotation::Integer) return { ReturnKind::Value, ReturnEvidence::Declared };
			const bool IsUnit = Rules.OmittedResult == OmittedResultNotation::Unit;
			// 式の本体は其の値を返し，単位型を既定とする文法では値の型を字面から決めれない事の返戻
			if(HasExpressionBody(Node)) return { ReturnKind::Value, IsUnit ? ReturnEvidence::Unknown : ReturnEvidence::Inferred };
			// 単位型を返す事と本体から推論する事の返戻
			return IsUnit ? ReturnShape{ ReturnKind::Void, ReturnEvidence::Declared } : Inferred();
		}
		if(Syntax.Is(Type, Role::PlaceholderTypeSpecifier)) {
			// 推論を要求する型を置換える後置の戻値の型
			NodeId Trailing = NoNode;
			AnyNodeOf(
				Syntax,
				Syntax.FieldChild(Node, ChildRole::Declarator),
				[this, &Trailing](const NodeId Inner) -> bool {
					// 後置の戻値の型でない節点の返戻
					if(!Syntax.Is(Inner, Role::TrailingReturnType)) return false;
					Trailing = Inner;
					// 発見で打切の返戻
					return true;
				}
			);
			// 後置の型が無ければ本体から推論する事の返戻
			if(Trailing == NoNode) return Inferred();
			Type = Syntax.LastNamedChild(Trailing);
		}
		// 表明の注釈が値を返さない事の返戻
		if(Syntax.Is(Type, Role::AssertsAnnotation)) return { ReturnKind::Void, ReturnEvidence::Declared };
		// 型の注釈は中の型で見る
		if(Syntax.Is(Type, Role::TypeAnnotation)) Type = Syntax.LastNamedChild(Type);
		// 限定名 (`typing.NoReturn` / `System.Threading.Tasks.Task`) は最後の名前で見る
		const std::string Name = Syntax.Text(Type);
		const std::string_view Last = std::string_view(Name).substr(Name.rfind('.') + 1);
		// 非同期の修飾を持つ関数か
		const bool IsAsync = AnyChildOf(
			Syntax,
			Node,
			[this](const NodeId Child) -> bool {
				// 非同期の修飾の字句又は修飾語かの返戻
				return (!Syntax.IsNamed(Child) || Syntax.Is(Child, Role::Modifier)) && Syntax.IsWord(Child, Word::AsyncModifier);
			}
		);
		if(Rules.IsSpelling(Last, Word::VoidType) || IsAsync && Rules.IsSpelling(Last, Word::AsyncVoidType)) {
			// 値を返さない型と，非同期の関数の値を持たない包みの型の返戻
			return { ReturnKind::Void, ReturnEvidence::Declared };
		}
		// 戻らない型の返戻
		if(Rules.IsSpelling(Last, Word::NeverType)) return { ReturnKind::Never, ReturnEvidence::Declared };
		// 起動点又は値を返す関数の区分の返戻
		return { IsProgramEntry(Node) ? ReturnKind::Main : ReturnKind::Value, ReturnEvidence::Declared };
	}

	/**
	 * 通常関数の本体の要約関数
	 * @param Node 判定対象ノード
	 * @return 通常関数で文の並びの本体を持てば其の要約（生成器・コルーチン・値を返さない関数の空の本体は除く）
	 */
	std::optional<FunctionEnd> ControlFlow::ClassifyFunction(const NodeId Node) const {
		// 通常関数でない節点を対象外とする事の返戻
		if(!IsOrdinaryFunction(Node)) return std::nullopt;
		const NodeId Statements = StatementsOf(Node);
		if(Statements == NoNode || Syntax.GetNode(Node).Facts.Has(ConstraintKind::ContainsError) || IsGeneratorLike(Node, Statements)) {
			// 本体の無い宣言・式本体・誤りを含む関数・生成器を対象外とする事の返戻
			return std::nullopt;
		}
		const ReturnShape Shape = ReturnShapeOf(Node);
		// 値を返さない関数の空の本体と，Python の中身の無い本体（型の宣言だけの定義を含む）の除外
		// 対象外の関数の返戻
		if(
			(Shape.Kind == ReturnKind::Void || Shape.Kind == ReturnKind::Never || Syntax.Is(Node, Role::MayDeclarePlaceholderFunction)) &&
			IsPlaceholderBody(Statements)
		) return std::nullopt;
		// 最後の文
		NodeId Last = NoNode;
		ForEachStatementOf(
			Syntax,
			Statements,
			[&](const NodeId Child) -> void {
				Last = Child;
			}
		);
		// 本体の要約の返戻
		return FunctionEnd{ Statements, Last, Shape.Kind };
	}

	/**
	 * 暗黙返戻の候補と為る単一式の本体かの判定関数（整形仕様 5.2）
	 * 値を返す関数の本体が注記を除いて式１つだけから成るかを調べる
	 * 計算量：本体の子の数 C に対し O(C)
	 * @param Function 関数の節点
	 * @param End 関数の本体の要約
	 * @return 単一式の本体なら true
	 */
	bool ControlFlow::IsSingleImplicitReturnBody(const NodeId Function, const FunctionEnd &End) const {
		// 単一式の返戻を持ち得ない関数と，最後の文が式でない物の返戻
		if(
			!Syntax.Is(Function, Role::SingleExpressionResultFunction) || End.Kind != ReturnKind::Value ||
			!Syntax.Is(End.Last, Role::ImplicitReturnCandidateExpression)
		) return false;
		// 偽枝の無い分岐は値を与える単一式でない事の返戻
		if(Syntax.Is(End.Last, Role::If) && !BranchesOf(End.Last).HasElse) return false;
		uint32_t Count = 0;
		ForEachStatementOf(
			Syntax,
			End.Statements,
			[&](const NodeId) -> void {
				++Count;
			}
		);
		// 式１つだけの本体かの返戻
		return Count == 1;
	}

	/**
	 * 単一式の本体が値を暗黙に返すかの判定関数（整形仕様 5.2）
	 * 明示返戻に出来る式を持つ単一式の本体だけを対象とする
	 * 計算量：本体の子の数 C に対し O(C)
	 * @param Function 関数の節点
	 * @param End 関数の本体の要約
	 * @return 暗黙の返戻なら true
	 */
	bool ControlFlow::IsImplicitReturn(const NodeId Function, const FunctionEnd &End) const {
		// 明示返戻に出来る暗黙返戻かの返戻
		return Syntax.Is(End.Last, Role::ImplicitReturnExpression) && IsSingleImplicitReturnBody(Function, End);
	}

	/**
	 * 単一式の暗黙返戻を明示しても結果構築子の適用が変わらないかの判定関数（整形仕様 5.2）
	 * 関数と所属型の属性，適合・継承及び拡張を調べ，結果構築子を明示・推論で適用し得ない型の外の関数と型の成員だけを許す
	 * @param Function 関数の節点
	 * @param End 関数の本体の要約
	 * @return 値と束縛を構文から保てる場合 true
	 */
	bool ControlFlow::CanMakeImplicitReturnExplicit(const NodeId Function, const FunctionEnd &End) const {
		// 宣言した戻値の型を持つ関数の暗黙の返戻でない物の返戻
		if(
			!Syntax.Is(Function, Role::ImplicitReturnCandidate) || !IsImplicitReturn(Function, End) ||
			ReturnShapeOf(Function).Evidence != ReturnEvidence::Declared
		) return false;
		// 宣言が属性を持つか
		const auto HasAttribute = [this](const NodeId Declaration) -> bool {
			const NodeId Modifiers = Syntax.FirstNamedChildWithRole(Declaration, Role::ModifierSequence);
			// 修飾の並びの中の属性の有無の返戻
			return Modifiers != NoNode && AnyChildOf(
				Syntax,
				Modifiers,
				[this](const NodeId Child) -> bool {
					// 属性かの返戻
					return Syntax.Is(Child, Role::Attribute);
				}
			);
		};
		// 属性を持つ関数の返戻
		if(HasAttribute(Function)) return false;
		const NodeId Parent = Syntax.ParentOf(Function);
		// 型の外の関数の返戻
		if(Syntax.Is(Parent, Role::SourceFileNode)) return true;
		// 型の成員でない関数の返戻
		if(!Syntax.Is(Parent, Role::ClassMembers)) return false;
		const NodeId Nominal = Syntax.ParentOf(Parent);
		// 適合・継承・拡張・属性の在る型の成員の返戻
		if(
			!Syntax.Is(Nominal, Role::ImplicitReturnNominal) ||
			Syntax.FirstNamedChildWithRole(Nominal, Role::ImplicitReturnConformance) != NoNode ||
			Syntax.HasTokenRoleChild(Nominal, Role::ImplicitReturnExtensionToken) || HasAttribute(Nominal)
		) return false;
		// 最も外の範囲の型の成員かの返戻
		return Syntax.Is(Syntax.ParentOf(Nominal), Role::SourceFileNode);
	}

	/**
	 * Rust の展開前の属性の範囲かの判定関数
	 * 属性は展開前のマクロとして本体を書き換え得る為，其の範囲の束縛・返戻を構文だけで変えない
	 * @param Syntax 構文モデル
	 * @param Node 判定を始める節点（祖先へ遡る）
	 * @param Cache 節点毎の判定の控え（遡った節点へ同じ結果を配る）
	 * @return 節点又は祖先が外部属性の後に在るか内部属性を持てば true
	 */
	bool ControlFlow::HasUnexpandedAttribute(const SyntaxModel &Syntax, NodeId Node, std::unordered_map<NodeId, bool> &Cache) {
		// 今回遡った節点
		std::vector<NodeId> Path;
		bool IsProtected = false;
		for(; Node != NoNode && !IsProtected; Node = Syntax.ParentOf(Node)) {
			if(const std::unordered_map<NodeId, bool>::const_iterator Cached = Cache.find(Node); Cached != Cache.end()) {
				// 既知の判定の採用
				IsProtected = Cached->second;
				break;
			}
			Path.push_back(Node);
			// 直前の外部属性か本体の内部属性に依る保護
			IsProtected = Syntax.Is(Syntax.PrevNamedSibling(Node), Role::OuterExpansionAttribute) || AnyChildOf(
				Syntax,
				Node,
				[&Syntax](const NodeId Child) -> bool {
					// 内部属性かの返戻
					return Syntax.Is(Child, Role::InnerExpansionAttribute);
				}
			);
		}
		// 遡った節点への同じ判定の配布
		for(const NodeId Visited : Path) Cache.emplace(Visited, IsProtected);
		// 属性に依る保護の有無の返戻
		return IsProtected;
	}

	/**
	 * 本体内の未展開マクロ・属性が返戻の束縛又は制御の流れを変え得るかの判定関数
	 * 入れ子の関数・クロージャの中は外の関数の返戻に関わらない為に降りない
	 * 計算量：本体の節点数 V に対し O(V)
	 * @param Statements 関数の文の並び
	 * @return 未展開の呼出又は属性が在れば true
	 */
	bool ControlFlow::ContainsUnexpandedBodySyntax(const NodeId Statements) const {
		bool IsFound = false;
		WalkNodes(
			Syntax,
			Statements,
			[this, &IsFound](const NodeId Node) -> WalkStep {
				IsFound = IsFound || Syntax.Is(Node, Role::MacroCall) || Syntax.Is(Node, Role::OuterExpansionAttribute) ||
				Syntax.Is(Node, Role::InnerExpansionAttribute);
				// 見付けた後と制御フローの境界の中へ降りない事の返戻
				return IsFound || IsFlowBoundary(Node) ? WalkStep::Skip : WalkStep::Descend;
			}
		);
		// 未展開の呼出又は属性の有無の返戻
		return IsFound;
	}

	/**
	 * 塊の値を与える末尾式の取得関数
	 * @param Last 塊の最後の文
	 * @return 末尾式（`;` で終わらない塊の式の文は中の式，文で終わる塊は空）
	 */
	NodeId ControlFlow::TailExpression(const NodeId Last) const {
		if(Syntax.Is(Last, Role::ExpressionStatement)) {
			// 式の文は `;` で終わらない塊の式だけが本体の値を与える事の返戻
			return Syntax.IsWord(Syntax.Child(Last, Syntax.ChildCount(Last) - 1), Word::StatementEnd) ? NoNode : Syntax.NamedChild(Last, 0);
		}
		if(Syntax.Is(Last, Role::MacroCall)) {
			// 波括弧で呼ぶマクロは文である事の返戻
			return Syntax.IsTokenRole(FirstChildNode(Syntax.LastNamedChild(Last)), Role::OpeningBraceToken) ? NoNode : Last;
		}
		// 宣言の文以外の末尾の式の返戻
		return Last == NoNode || Syntax.Is(Last, Role::NonValueDeclaration) ? NoNode : Last;
	}

	/**
	 * 末尾式が文の終わり迄生きる一時値を作るかの判定関数
	 * 末尾式の一時値の破棄の時機は版に依り局所変数の前後が異なる為，`return` の文へ移すと破棄の順が変わり得る
	 * 場所を要する文脈（受け手・欄・添字・参照・比較・照合の対象）へ値を作る式を置く場合と，展開を追えないマクロを一時値の候補とする
	 * 計算量：訪問した節点と子の数 V に対し O(V)
	 * @param Tail 末尾式
	 * @return 一時値を作り得るなら true
	 */
	bool ControlFlow::KeepsTailTemporaries(const NodeId Tail) const {
		// 場所を要する文脈の子かの判定関数
		const auto IsPlaceContext = [this](const NodeId Parent, const NodeId Child) -> bool {
			// 欄・添字の被演算子の返戻
			if(Syntax.Is(Parent, Role::PlaceProjection)) return Syntax.NamedChild(Parent, 0) == Child;
			const bool IsReferenceOperand = Syntax.Is(Parent, Role::ReferenceExpression) ||
			Syntax.Is(Parent, Role::UnaryOperation) && Syntax.IsTokenRole(FirstChildNode(Parent), Role::AsteriskToken);
			// 参照と参照外しの被演算子の返戻
			if(IsReferenceOperand) return true;
			// 照合の対象は照合の終わり迄生きる事の返戻
			if(Syntax.Is(Parent, Role::ScrutineeHost)) return Syntax.FieldChild(Parent, ChildRole::Value) == Child;
			// 比較の演算子は被演算子を参照で受ける事の返戻
			return Syntax.Is(Parent, Role::BinaryOperation) &&
			Syntax.Is(Syntax.FieldChild(Parent, ChildRole::Operator), Role::BorrowingOperator);
		};
		// 子の走査位置を保持した一時値探索の作業列
		struct TailFrame {
			NodeId Node; // 判定中の節点
			std::span<const NodeId> Children; // 判定中の節点の子
			size_t NextChild = 0; // 次の子の位置
			bool Entered = false; // 節点自身の判定済状態
		};
		std::vector<TailFrame> Stack { { Tail, {} } };
		bool IsFound = false;
		// 末尾式の一時値の候補の探索
		while(!Stack.empty() && !IsFound) {
			const NodeId Node = Stack.back().Node;
			if(!Stack.back().Entered) {
				Stack.back().Entered = true;
				if(Node != NoNode) Syntax.Charge(1);
				if(Syntax.Is(Node, Role::MacroCall)) {
					IsFound = true;
					break;
				}
				if(Node == NoNode || IsFlowBoundary(Node) || Syntax.Is(Node, Role::BodyTemporaryScopeLoop)) {
					Stack.pop_back();
					continue;
				}
				// 塊の末尾式への走査の絞込
				if(Syntax.Is(Node, Role::LexicalBlock)) {
					NodeId Last = NoNode;
					ForEachNamedChildOf(
						Syntax,
						Node,
						[this, &Last](const NodeId Child) -> void {
							if(!Syntax.Is(Child, Role::JumpLabel)) Last = Child;
						}
					);
					Stack.pop_back();
					Stack.push_back({ TailExpression(Last), {} });
					continue;
				}
				Stack.back().Children = Syntax.ChildrenOf(Node);
			}
			// 一時値を保つ文脈の子の選別
			if(Stack.back().NextChild == Stack.back().Children.size()) {
				Stack.pop_back();
				continue;
			}
			const NodeId Child = Stack.back().Children[Stack.back().NextChild++];
			Syntax.Charge(1);
			if(!Syntax.IsNamed(Child)) continue;
			// 独立した一時値の範囲に置く条件・枝・本体・遅延評価の右辺を除く
			if(
				Syntax.Is(Node, Role::ConditionalValue) &&
				(Child != Syntax.FieldChild(Node, ChildRole::Condition) || !Syntax.Is(Child, Role::PatternCondition)) ||
				Syntax.Is(Node, Role::IterationValue) && Child != Syntax.FieldChild(Node, ChildRole::Value) ||
				Syntax.Is(Child, Role::MatchBlock) ||
				Syntax.Is(Node, Role::BinaryOperation) && Child == Syntax.FieldChild(Node, ChildRole::Right) &&
				Syntax.Is(Syntax.FieldChild(Node, ChildRole::Operator), Role::LazyLogicalOperator)
			) continue;
			if(IsPlaceContext(Node, Child) && !Syntax.Is(Child, Role::PlaceOrPlainValue)) IsFound = true;
			else Stack.push_back({ Child, {} });
		}
		// 一時値の候補の有無の返戻
		return IsFound;
	}

	/**
	 * 通常関数の末尾の到達可能性の判定関数
	 * @param Function 関数の節点
	 * @param End 関数の本体の要約
	 * @return 末尾の到達可能性
	 */
	EndReachability ControlFlow::EndReach(const NodeId Function, const FunctionEnd &End) const {
		// 到達不能判定で考慮する転送先の収集
		const GotoIndex Gotos = CollectGotoTargets(End.Statements);
		// 定数と為り得る条件を定数でないと看做しても文の並びを素通り出来ない場合の返戻
		if(DoStatementsTerminate(End.Statements, Gotos)) return EndReachability::Unreachable;
		// 記述した全ての節が閉じる切替へ不一致の値が来ないと看做すと末尾への経路が塞がるか
		ControlFlow Covered(Syntax);
		Covered.AssumesCaseCoverage = true;
		// 不一致の値だけが末尾へ達する経路は値の範囲に依り整形器が確かめれない為，補わず要確認とする事の返戻（整形仕様 5.2, 8.4）
		if(Covered.DoStatementsTerminate(End.Statements, Gotos)) return EndReachability::Unknown;
		// 定数式の値で到達可能性を定めない言語の返戻
		if(!Syntax.Rules().ReachabilityUsesConstantCondition) return EndReachability::Reachable;
		// 定数と為り得る条件を常に真と看做すと，脱出先の無い繰返が末尾への経路を塞ぐかの判定
		const VisibleNameTable Names = VisibleNamesOf(Function);
		ControlFlow Pessimistic(Syntax);
		Pessimistic.VisibleNames = &Names;
		// 塞がれば実行時とコンパイル時の到達可能性が確かめれず，塞がれなければ到達する事の返戻
		return Pessimistic.DoStatementsTerminate(End.Statements, Gotos) ? EndReachability::Unknown : EndReachability::Reachable;
	}

	/**
	 * 末尾の返戻の補完を構文から確かめれないかの判定関数（整形仕様 5.2, 8.4）
	 * 展開前の属性の範囲の関数，値を返す関数の本体の未展開のマクロ・属性，破棄の順を変え得る一時値を作る末尾式，明示返戻を確かめれない単一式を未確定とし，補完と静的検査が同じ判定を使う
	 * 計算量：本体の節点数 V に対し O(V)
	 * @param Function 関数の節点
	 * @param End 関数の本体の要約
	 * @param Cache 展開前の属性の範囲の判定の控え
	 * @return 補完の前提を確かめれなければ true
	 */
	bool ControlFlow::IsReturnCompletionUncertain(
		const NodeId Function,
		const FunctionEnd &End,
		std::unordered_map<NodeId, bool> &Cache
	) const {
		// 展開前の属性の範囲と，値を返す関数の本体の未展開のマクロ・属性の返戻
		if(
			Syntax.Is(Function, Role::AttributedTailFunction) && (
				HasUnexpandedAttribute(Syntax, End.Statements, Cache) ||
				End.Kind == ReturnKind::Value && ContainsUnexpandedBodySyntax(End.Statements)
			)
		) return true;
		const NodeId Tail = Syntax.Rules().TailExpressionIsValue ? TailExpression(End.Last) : NoNode;
		// 破棄の順を変え得る一時値を作る末尾式の返戻
		if(Tail != NoNode && KeepsTailTemporaries(Tail)) return true;
		// 式の型・束縛や結果構築子の適用を確かめれない暗黙の返戻かの返戻
		return IsSingleImplicitReturnBody(Function, End) && !CanMakeImplicitReturnExplicit(Function, End);
	}

	/**
	 * コンストラクタ
	 * @param Syntax 判定する構文
	 */
	ControlFlow::ControlFlow(const SyntaxModel &Syntax) : Syntax(Syntax) {}
} // namespace Core
