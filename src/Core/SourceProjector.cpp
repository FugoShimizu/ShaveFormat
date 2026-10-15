#include "SourceProjector.hpp"
#include "ControlFlow.hpp"
#include "DocCommentReader.hpp"
#include "SignatureExtractor.hpp"
#include "SyntaxGeneration.hpp"
#include "../Profile/GrammarProfile.hpp"
#include "../Profile/ReadingRules.hpp"
#include "../Profile/ReferenceProjection.hpp"
#include "../Util/CharClass.hpp"
#include "../Util/CommentNormalizer.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include "../Util/ParseAdapter.hpp"
#include "../Util/SyntaxParser.hpp"
#include "../Util/TextEdit.hpp"
#include <tree_sitter/api.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
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
	 * 入力の参照先を解決するコンストラクタ
	 * @param Profile 標準対象と語の入力定義
	 * @param Syntax 投影結果の格納先
	 */
	ReferenceProjection::ReferenceProjection(const GrammarProfile &Profile, SyntaxModel &Syntax) : ControlFlow(Syntax),
	Profile(Profile),
	Projected(Syntax) {}

	/**
	 * インポートの供給元と戻値型を確定する関数
	 * 計算量：ノード数 N とインポートの読取費用 B に対し O(N + B)
	 * @throws FormatLimitExceeded 構文又はインポートの照会が作業予算を超える場合
	 * @throws std::bad_alloc インポートの事実を確保出来ない場合
	 */
	void ReferenceProjection::RecordImports() {
		for(NodeId Node = 0; Node < Syntax.GetNodeCount(); ++Node) {
			Syntax.Charge(1);
			if(Syntax.Is(Node, Role::ImportedPathBinding)) {
				CollectImportedPaths(
					Node,
					[](const NodeId) -> bool {
						return false;
					},
					Projected.ProjectedImports[Node]
				);
			} else if(Syntax.Is(Node, Role::ImportedMemberBinding) || Syntax.Is(Node, Role::ImportedModuleBinding)) {
				CallableNames &Imports = Projected.ProjectedImports[Node];
				const bool IsMember = Syntax.Is(Node, Role::ImportedMemberBinding);
				const std::string Prefix = IsMember ? Syntax.GetText(Syntax.GetNamedChild(Node, 0)) : std::string();
				for(const NodeId Item : Syntax.GetChildrenOf(Node)) {
					Syntax.Charge(1);
					if(!Syntax.HasField(Item, ChildRole::Name)) continue;
					const NodeId Alias = Syntax.GetFieldChild(Item, ChildRole::Alias);
					const NodeId Original = Syntax.GetFirstEdge(Alias == NoNode ? Item : Syntax.GetFieldChild(Item, ChildRole::Name)).Leaf;
					const NodeId Name = Alias == NoNode ? Original : Alias;
					const std::string Target = IsMember ?
					Prefix + "." + Syntax.GetText(Original) :
					Syntax.GetText(Alias == NoNode ? Item : Syntax.GetFieldChild(Item, ChildRole::Name));
					Syntax.Charge(Target.size());
					const bool IsSame = Syntax.GetCmpNameIdx(Name) == Syntax.GetCmpNameIdx(Original);
					const std::optional<Word> Type = IsMember && IsSame ? StandardResultType(Profile.ReadResultMeanings(Target)) : std::nullopt;
					Imports[Syntax.GetCmpNameIdx(Name)].push_back(
						{
							.Name = Name,
							.Target = Target,
							.IsModuleImport = !IsMember && IsSame,
							.IsTerminatingImport = IsMember && IsSame && Profile.IsSpelling(Target, Word::TerminatingImport),
							.SrcRoot = Type ? Syntax.GetFirstEdge(Syntax.GetNamedChild(Node, 0)).Leaf : NoNode,
							.ImportedResultType = Type,
							.IsValueImport = IsMember
						}
					);
				}
			}
		}
		// 終了
		return;
	}

	/**
	 * マクロの未束縛時の役割と，解決に使った束縛の記録関数
	 * 計算量：名前長 B と束縛の照会費用 Q に対し O(B + Q)
	 * @param Head 束縛を照会した先頭の名前
	 * @param OriginalName 入力の修飾名
	 * @param ResolvedName 束縛を確認した供給元
	 * @param IsUnknown 供給元を確認出来ないか
	 * @return 束縛の変更後も役割だけで照会出来る事実
	 * @throws FormatLimitExceeded 束縛の照会が作業予算を超える場合
	 * @throws std::bad_alloc 事実を確保出来ない場合
	 */
	std::shared_ptr<const ReferenceFacts> ReferenceProjection::MakeMacroReference(
		const NodeId Head,
		const std::string_view OriginalName,
		const std::string_view ResolvedName,
		const bool IsUnknown
	) const {
		std::shared_ptr<ReferenceFacts> Facts = std::make_shared<ReferenceFacts>(
			ReferenceFacts{ std::string(ResolvedName), Profile.ReadWordMeanings(ResolvedName), IsUnknown }
		);
		Facts->UnboundMeanings = Profile.ReadWordMeanings(OriginalName);
		Facts->BindingHead = Head;
		if(Head != NoNode) {
			const std::string HeadName = Syntax.GetText(Head);
			const std::string_view AbsolutePrefix = Syntax.GetRules().GetSpelling(Word::AbsoluteNamePrefix);
			Facts->IsGlobal = !AbsolutePrefix.empty() && OriginalName.starts_with(AbsolutePrefix);
			const std::string_view LocalName = OriginalName.substr(Facts->IsGlobal ? AbsolutePrefix.size() : 0);
			if(LocalName.starts_with(HeadName)) Facts->BindingMember = LocalName.substr(HeadName.size());
			const BindingFact Fact = Syntax.GetBindingFact(Head, Facts->BindingMember, Facts->IsGlobal);
			Facts->BindingDecl = Fact.Source ? Fact.Source->Name : Fact.Decl;
		}
		// 供給元と未束縛時の役割を持つ事実の返戻
		return Facts;
	}

	/**
	 * 呼出効果と捕捉条件を確定する関数
	 * 計算量：対象の参照先の解決費用 Q に対し O(Q)
	 * @param Node 既存の走査が訪問したノード
	 * @param InHandler 捕捉条件の内側か
	 * @throws FormatLimitExceeded 構文又は束縛の照会が作業予算を超える場合
	 * @throws std::bad_alloc 参照先の事実を確保出来ない場合
	 */
	void ReferenceProjection::RecordFacts(const NodeId Node, const bool InHandler) const {
		if(Syntax.Is(Node, Role::CallNode) || Syntax.Is(Node, Role::MacroCall)) {
			bool Registered = false;
			Projected.Nodes[Node].Facts.CallEffects = GetCallEffects(Node, Registered);
			Projected.Nodes[Node].Facts.IsRegisteredMacro = Registered;
		}
		if(InHandler && Syntax.IsNamed(Node) && !Syntax.GetChildCount(Node)) {
			bool HasContract = false;
			const std::string Name = ResolveCallName(Node, Syntax.GetText(Node), HasContract);
			Projected.Nodes[Node].Facts.Reference =
			std::make_shared<ReferenceFacts>(ReferenceFacts{ Name, Profile.ReadWordMeanings(Name) });
		}
		if(
			Syntax.Is(Node, Role::ExclamationToken) && Syntax.Is(Syntax.GetParentOf(Node), Role::PreservedTokenSeq) &&
			Syntax.Is(Syntax.GetNextSibling(Node), Role::PreservedTokenSeq) && Syntax.Is(Syntax.GetPrevSibling(Node), Role::Identifier)
		) {
			const NodeId Head = Syntax.GetPrevSibling(Node);
			Projected.Nodes[Head].Facts.Reference = ResolveTokenMacro(Head);
		}
		if(Syntax.Is(Node, Role::MacroCall)) {
			const NodeId Macro = Syntax.GetFieldChild(Node, ChildRole::Macro);
			if(Macro != NoNode && Projected.Nodes[Node].Facts.Reference) {
				Projected.Nodes[Macro].Facts.Reference = Projected.Nodes[Node].Facts.Reference;
			} else if(Macro != NoNode) {
				NodeId Head = Macro;
				while(Syntax.GetChildCount(Head)) Head = Syntax.GetNamedChild(Head, 0);
				bool HasContract = false;
				const std::string Name = ResolveCallName(Head, Syntax.GetText(Macro), HasContract);
				Projected.Nodes[Macro].Facts.Reference = MakeMacroReference(Head, Syntax.GetText(Macro), Name);
			}
		}
		// 終了
		return;
	}

	/**
	 * 明示的なインポートの末尾名又は別名の収集関数
	 * 同じ名前を供給する既知のインポートと，別の供給元の導入を区別する
	 * 計算量：インポート項目数 N，修飾名の総トークン長 B と接頭辞のコピー量 P，未訪問の祖先の延探索量 D に対し O(N + B + P + D)
	 * @param Import インポートの宣言
	 * @param IsCustom 走査中の祖先の事実から独自定義を判定する関数
	 * @param Names 同じ範囲の呼出先を隠す導入名の表
	 * @throws FormatLimitExceeded インポートの走査又は名前の照会の作業予算を超える場合
	 * @throws std::bad_alloc 作業列又は名前の表のメモリを確保出来ない場合
	 * @throws std::length_error 修飾名又は作業列の要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void ReferenceProjection::CollectImportedPaths(
		const NodeId Import,
		const std::function<bool(NodeId)> &IsCustom,
		CallableNames &Names
	) const {
		// インポート項目と，外側のグループが共有する修飾名の収集
		if(const NodeId Src = Syntax.GetFieldChild(Import, ChildRole::Path); Src != NoNode) {
			const std::unordered_map<std::string_view, std::string_view> &Prefixes = Syntax.GetRules().StandardImportPrefixes;
			const std::string Path = Syntax.GetText(Src);
			Syntax.Charge(1 + Path.size());
			const std::unordered_map<std::string_view, std::string_view>::const_iterator Prefix = Prefixes.find(Path);
			// 確認した標準の供給元だけから，既存の登録名を導入する
			if(Prefix != Prefixes.end()) {
				for(const std::string_view Target : Syntax.GetRules().Words[static_cast<size_t>(Word::TerminatingCall)]) {
					Syntax.Charge(1 + Prefix->second.size());
					if(!Target.starts_with(Prefix->second)) continue;
					const std::string_view Separator = Syntax.GetRules().ImportPathSeparator;
					Syntax.Charge(Target.size());
					const size_t End = Target.find(Separator);
					if(Prefix->second.empty() && End != std::string_view::npos) continue;
					const std::string Head(Target.substr(0, End));
					Syntax.Charge(Head.size() + Head.size()); // 名前のコピーとハッシュを計上
					if(
						const std::unordered_map<std::string, uint32_t>::const_iterator Idx = Syntax.CmpNameIndexes.find(Head);
						Idx != Syntax.CmpNameIndexes.end()
					) {
						Syntax.Charge(Target.size() + (End == std::string_view::npos ? 0 : Target.size() - End));
						Names[Idx->second].push_back(
							{
								.Name = Src,
								.Member = End == std::string_view::npos ? std::string() : std::string(Target.substr(End)),
								.Target = std::string(Target),
								.IsTerminatingImport = true,
								.IsSequential = true
							}
						);
					}
				}
			}
			// 供給元のパスをローカルの導入名にしない事の返戻
			return;
		}
		const bool HasNamespaceImports = Syntax.GetRules().HasNamespaceFunctionImports;
		const std::string_view ScopeDelimiter = Syntax.GetRules().ImportPathSeparator; // グループの供給元と項目を繋ぐ区切
		const std::string_view NameDelimiters = HasNamespaceImports ? ".:\\" : ".:";
		NodeId Path = Syntax.GetFieldChild(Import, ChildRole::ArgField), Alias = Syntax.GetFieldChild(Import, ChildRole::Name);
		const NodeId Wildcard = Syntax.GetFirstNamedChildWithRole(Import, Role::ImportWildcard);
		if(Alias == NoNode && (Syntax.Is(Import, Role::ImportAliasOnly) || Wildcard != NoNode)) {
			// 静的インポートはメソッド名を，通常のインポートは供給元の型名を導入する
			const std::vector<std::string_view> &Modifiers = Syntax.GetRules().MemberTypeModifiers;
			const bool IsStatic = Core::SyntaxModel::HasChildMatching(
				Syntax,
				Import,
				[&](const NodeId Child) -> bool {
					// 静的な導入の指定かの返戻
					return !Syntax.IsNamed(Child) && std::ranges::find(Modifiers, std::string_view(Syntax.GetText(Child))) != Modifiers.end();
				}
			);
			// 対象を解決出来ない範囲全体のインポートの終了
			if(Wildcard != NoNode && !IsStatic) return;
			const NodeId Src =
			Path != NoNode ? Path : Wildcard != NoNode ? Syntax.GetPrevNamedSibling(Wildcard) : Syntax.GetLastNamedChild(Import);
			const std::string Prefix = Syntax.GetText(Src) + ".";
			const Word Meaning = IsStatic ? Word::TerminatingCall : Word::TerminatingImport;
			for(const std::string_view Target : Syntax.GetRules().Words[static_cast<size_t>(Meaning)]) {
				if(!Target.starts_with(Prefix)) continue;
				const std::string Local(Target.substr(Prefix.size()));
				const std::unordered_map<std::string, uint32_t>::const_iterator Idx = Syntax.CmpNameIndexes.find(Local);
				if(Idx != Syntax.CmpNameIndexes.end()) {
					Names[Idx->second].push_back(
						{
							.Name = Src,
							.Target = std::string(Target),
							.IsTerminatingImport = true,
							.IsScopeWide = true,
							.SrcRoot = Syntax.GetFirstEdge(Src).Leaf,
							.IsCustomDefinition = IsCustom(Src)
						}
					);
				}
			}
			// 通常の単独名の導入へ重ねない事の返戻
			return;
		}
		// 単独の名前を導入しない範囲全体のインポートの終了
		if(
			Syntax.Is(Import, Role::ImportRequiresKind) && !Syntax.Is(Import, Role::ImportPathGroup) &&
			!Core::SyntaxModel::HasChildMatching(
				Syntax,
				Import,
				[&](const NodeId Child) -> bool {
					// 単独の名前の種別を指定する語かの返戻
					return Syntax.Is(Child, Role::ImportKindMarker);
				}
			) || Syntax.Is(Import, Role::ScopeImport) && Core::SyntaxModel::HasChildMatching(
				Syntax,
				Import,
				[&](const NodeId Child) -> bool {
					// 範囲全体の名前を取り込む語かの返戻
					return Syntax.Is(Child, Role::ImportAllNamesMarker);
				}
			)
		) return;
		if(const NodeId Wrapper = Syntax.GetFirstNamedChildWithRole(Import, Role::ImportAlias); Wrapper != NoNode) {
			Alias = Syntax.GetNamedChild(Wrapper, 0);
			Path = Syntax.GetPrevNamedSibling(Wrapper);
		}
		if(Syntax.Is(Import, Role::ImportPathGroup)) Path = Import;
		else if(Path == NoNode) Path = Syntax.GetLastNamedChild(Import);
		// 全項目の指定は明示的な単独の導入名を持たない
		// 明示名の無いインポートの終了
		if(Wildcard != NoNode) return;
		// インポートの修飾名を辿る経路
		struct ImportPath {
			NodeId Node; // インポートの項目
			std::string Prefix; // グループが共有する修飾の接頭辞
			NodeId Root; // 最外の供給元の修飾名
			bool IsFunction; // 関数を導入する項目か
		};
		std::vector<ImportPath> Pending { { Path, {}, NoNode, !HasNamespaceImports } };
		while(!Pending.empty()) {
			const ImportPath Cur = std::move(Pending.back());
			Pending.pop_back();
			Syntax.Charge(1);
			if(Syntax.Is(Cur.Node, Role::ImportWildcard)) continue;
			NodeId Kind = HasNamespaceImports ? Syntax.GetFieldChild(Cur.Node, ChildRole::Type) : NoNode;
			if(HasNamespaceImports && Kind == NoNode && Syntax.Is(Cur.Node, Role::ImportedPathBinding)) {
				// カンマ形式の共有種別は最初の項目に属し，グループ内の個別指定は共有しない
				const NodeId First = Syntax.GetFirstNamedChildWithRole(Cur.Node, Role::ImportPathItem);
				if(First != NoNode) Kind = Syntax.GetFieldChild(First, ChildRole::Type);
			}
			const bool IsFunction = Kind == NoNode ? Cur.IsFunction : Syntax.Is(Kind, Role::ImportKindMarker);
			if(Syntax.Is(Cur.Node, Role::ImportPathGroup)) {
				Syntax.Charge(Cur.Prefix.size());
				std::string Prefix = Cur.Prefix;
				NodeId Root = Cur.Root;
				if(const NodeId Shared = Syntax.GetFieldChild(Cur.Node, ChildRole::Path); Shared != NoNode) {
					Prefix += Syntax.GetText(Shared);
					Prefix += ScopeDelimiter;
					if(Root == NoNode) Root = Shared;
				}
				for(const NodeId Child : Syntax.GetChildrenOf(Cur.Node)) {
					Syntax.Charge(1);
					if(!Syntax.IsNamed(Child) || Syntax.HasField(Child, ChildRole::Path)) continue;
					Syntax.Charge(Prefix.size());
					Pending.push_back({ Child, Prefix, Root, IsFunction });
				}
				continue;
			}
			if(!IsFunction) continue;
			const NodeId LocalAlias = Syntax.GetFieldChild(Cur.Node, ChildRole::Alias);
			const NodeId ItemPath = Syntax.GetFieldChild(Cur.Node, ChildRole::Path);
			const NodeId Target = Syntax.Is(Cur.Node, Role::ImportPathItem) ? ItemPath : LocalAlias == NoNode ? Cur.Node : ItemPath;
			const NodeId Name = LocalAlias != NoNode ? LocalAlias : Alias != NoNode ? Alias : Syntax.GetLastEdge(Target).Leaf;
			const bool IsSelfImport = Syntax.Is(Target, Role::ImportSelfName) && !Cur.Prefix.empty();
			std::string Qualified =
			IsSelfImport ? Cur.Prefix.substr(0, Cur.Prefix.size() - ScopeDelimiter.size()) : Cur.Prefix + Syntax.GetText(Target);
			if(HasNamespaceImports && !Qualified.starts_with(ScopeDelimiter)) Qualified.insert(0, ScopeDelimiter);
			Syntax.Charge(Qualified.size());
			// 逐語に保持した修飾名の葉の，投影済の比較名からの末尾の導入名の読取
			std::string Spelling;
			if(IsSelfImport && LocalAlias == NoNode) {
				Spelling = Qualified;
				Syntax.Charge(Spelling.size());
			} else {
				if(!Syntax.IsAny(Name, { Role::NameToken, Role::NamespaceName, Role::TypeName })) continue;
				Spelling = Syntax.GetCmpName(Name);
			}
			if(Syntax.GetRules().AreFunctionNamesCaseInsensitive) {
				Syntax.Charge(Spelling.size());
				ToLowerAscii(Spelling);
			}
			std::string_view Introduced = Spelling;
			if(const size_t Separator = Introduced.find_last_of(NameDelimiters); Separator != std::string_view::npos) {
				Introduced.remove_prefix(Separator + 1);
			}
			if(
				const std::string_view Quote = Syntax.GetRules().BindingNameQuote;
				!Quote.empty() && Introduced.starts_with(Quote) && Introduced.ends_with(Quote)
			) Introduced = Introduced.substr(Quote.size(), Introduced.size() - (Quote.size() << 1));
			Syntax.Charge(Introduced.size() + 1);
			const std::unordered_map<std::string, uint32_t>::const_iterator Idx = Syntax.CmpNameIndexes.find(std::string(Introduced));
			if(Idx == Syntax.CmpNameIndexes.end()) continue;
			const size_t Separator = Qualified.find_last_of(NameDelimiters);
			const bool IsSameName = Introduced == std::string_view(Qualified).substr(Separator == std::string::npos ? 0 : Separator + 1);
			const bool IsTerminatingImport = (IsSameName || HasNamespaceImports) && Profile.IsSpelling(Qualified, Word::TerminatingImport);
			const std::optional<Word> ImportedType = IsSameName ? StandardResultType(Profile.ReadResultMeanings(Qualified)) : std::nullopt;
			const CalleeSummary Source = GetCalleeSummary(Cur.Root == NoNode ? Target : Cur.Root);
			Names[Idx->second].push_back(
				{
					.Name = Name,
					.Target = Qualified,
					.IsTerminatingImport = IsTerminatingImport,
					.IsScopeWide = Syntax.GetNode(Import).Kind.Def->Bindings.IsStaticItem,
					.SrcRoot = !HasNamespaceImports && (IsTerminatingImport || ImportedType || !Syntax.GetRules().StandardImportPrefixes.empty()) ?
					Source.Head :
					NoNode,
					.ImportedResultType = ImportedType,
					.IsCustomDefinition = IsCustom(Name),
					.IsUncertain = Source.HasMacroBinding,
					.IsSequential = !Syntax.GetRules().StandardImportPrefixes.empty()
				}
			);
		}
		// 終了
		return;
	}

	/**
	 * 別名の受手を同じ標準対象へ解決してメンバの再代入を確認する関数
	 * 条件付又は対応を確定出来ない受手の変更も，標準登録への復帰を許さない
	 * 計算量：初回のインデックス構築費用 B，可視範囲数 S，対象名長 L，一致する導入数 D と受手の解決費用 Q に対し期待値 O(B + S L² + D + Q)，外側の同じ対象の再照会は期待値 O(S L)
	 * @param Reference メンバを取得する位置
	 * @param Target 解決した標準対象の完全名
	 * @return 同一又は未確定の受手への再代入が在れば真
	 * @throws FormatLimitExceeded 名前又は位置の照会が作業予算を超える場合
	 * @throws std::bad_alloc 導入表又は名前を確保出来ない場合
	 * @throws std::length_error 導入表又は名前の長さが上限を超える場合
	 * @throws std::out_of_range 導入又はノードが表に無い場合
	 */
	bool ReferenceProjection::HasMemberReassignment(const NodeId Reference, const std::string &Target) const {
		const NodeId LocalScope = GetScopeOf(Reference);
		bool HasOrder = false;
		for(NodeId Scope = LocalScope; Scope != NoNode; Scope = GetScopeOf(Syntax.GetParentOf(Scope))) {
			const CallableNameOwner Names = GetCallableNamesOf(Scope);
			const bool IsEntire = Syntax.Is(Scope, Role::ClassBody) || Scope == Syntax.GetRoot() && LocalScope != Scope;
			// 別の関数から読む根の範囲は，可視位置に依らない同じ要約を使う
			const bool CanShare = Scope == Syntax.GetRoot() && LocalScope != Scope;
			if(CanShare) if(
				const std::unordered_map<std::string, bool>::const_iterator Known = Names->EntireMemberReassignments.find(Target);
				Known != Names->EntireMemberReassignments.end()
			) {
				Syntax.Charge(1 + Target.size());
				// 再代入を持つ共有要約の返戻
				if(Known->second) return true;
				continue;
			}
			bool HasReassignment = false;
			// 対象の修飾に一致する導入だけを引き，無関係なメンバ代入を再走査しない
			for(size_t Start = 0; Start < Target.size() && !HasReassignment; ++Start) {
				Syntax.Charge(1 + Target.size() - Start);
				const std::unordered_map<std::string, CallableIndex::MemberPositions>::const_iterator Found =
				Names->MemberAssignments.find(Target.substr(Start));
				if(Found == Names->MemberAssignments.end()) continue;
				for(const std::pair<uint32_t, size_t> &Position : Found->second) {
					Syntax.Charge(1);
					const CallableIntroduction &Binding = Names->Names.at(Position.first).at(Position.second);
					if(Binding.Name == Reference || !IsEntire && !IsCallableBefore(Binding.Name, Reference, HasOrder)) continue;
					bool HasContract = false, IsUnknown = false;
					const std::string Receiver = ResolveCallName(Binding.Name, Syntax.GetText(Binding.Name), HasContract, &IsUnknown, false);
					// 受手の同一性を確定出来ない変更も，無関係と看做さない
					HasReassignment = IsUnknown || Receiver.empty() || Receiver + Binding.Member == Target;
					if(HasReassignment) break;
				}
			}
			if(CanShare) {
				Syntax.Charge(1 + Target.size());
				Names->EntireMemberReassignments.emplace(Target, HasReassignment);
			}
			// 同じ標準対象への変更が在る事の返戻
			if(HasReassignment) return true;
		}
		// 同じ対象への変更が無い事の返戻
		return false;
	}

	/**
	 * 可視の導入と，追跡出来るインポート・代入から呼出先を解決する関数
	 * 計算量：名前表・供給元・宣言契約の照会費用 Q，別名の数 A，範囲の深さ H，同名宣言数 D と処理するテキスト長 L に対し O(Q + A H² log D + L)
	 * @param Head 呼出先の先頭名
	 * @param Name 修飾を含む呼出先
	 * @param HasContract 非復帰の宣言契約の格納先
	 * @param IsUnknown 入力の束縛を確定出来ない場合の格納先（省略時は記録しない）
	 * @param ShouldCheckMembers 別名経由のメンバ再代入を調べるか（受手だけの解決時は偽）
	 * @return 標準名又は未遮蔽の名前（別対象・未確定なら空）
	 * @throws FormatLimitExceeded 名前・宣言・祖先の照会が作業予算を超える場合
	 * @throws std::bad_alloc 名前表・探索列・テキストを確保出来ない場合
	 * @throws std::length_error 名前表又はテキストの要素数上限を超える場合
	 * @throws std::out_of_range ノード又は比較用の名前の番号が表に無い場合
	 */
	std::string ReferenceProjection::ResolveCallName(
		NodeId Head,
		std::string_view Name,
		bool &HasContract,
		bool *IsUnknown,
		const bool ShouldCheckMembers
	) const {
		NodeId Reference = Head; // モジュールの別名は使用位置，関数の別名は取得位置でメンバを読む
		struct PendingResolution {
			CallableNameOwner Owner;
			NodeId Src;
			std::string Suffix;
			size_t ReferenceVersion;
		}; // 未解決の供給値と其の取得位置の変更回数
		std::vector<PendingResolution> Pending;
		size_t ReferenceVersion = 0;
		bool HasResolvedContract = false, IsResolvedUnknown = false, ShouldCheckResolvedMembers = false;
		bool ShouldCheckImportedValue = false;
		const auto Finish = [&](const std::string_view Resolved) -> std::string {
			Syntax.Charge(Resolved.size());
			std::string Result(Resolved);
			for(const PendingResolution &Entry : Pending) {
				Syntax.Charge(1 + Entry.Suffix.size() + Result.size());
				Entry.Owner->Resolutions[Entry.Src].insert_or_assign(
					Entry.Suffix,
					CallableResolution{
						Result,
						ReferenceVersion == Entry.ReferenceVersion ? NoNode : Reference,
						HasResolvedContract,
						IsResolvedUnknown,
						ShouldCheckResolvedMembers
					}
				);
			}
			if(ShouldCheckResolvedMembers && ShouldCheckMembers && HasMemberReassignment(Reference, Result)) {
				IsResolvedUnknown = true;
				Result.clear();
			}
			HasContract |= HasResolvedContract;
			if(IsUnknown && IsResolvedUnknown) *IsUnknown = true;
			// 使用位置に依るメンバの変更を含む解決結果の返戻
			return Result;
		};
		const auto Unresolved = [&]() -> std::string {
			IsResolvedUnknown = true;
			// 未確定の名前の返戻
			return Finish(std::string());
		};
		// 完全修飾の関数名にはローカルのインポートを適用しない事の返戻
		if(Syntax.GetRules().HasNamespaceFunctionImports && Name.starts_with(Syntax.GetRules().ImportPathSeparator)) {
			// 完全修飾の名前の返戻
			return Finish(Name);
		}
		std::string AliasName; // 別名を辿る場合だけ所有し，参照の寿命を此の呼出内に保つ
		std::unordered_set<NodeId> Seen;
		while(Head != NoNode) {
			Syntax.Charge(1);
			const SyntaxNode &HeadNode = Syntax.GetNode(Head);
			// 解決出来ない対象の返戻
			if(HeadNode.Facts.Has(ConstraintKind::MacroBinding)) return Unresolved();
			// 葉の所有テキストと投影済の比較名を使い，名前の複写と同じ綴りの再検索を避ける
			const std::string JoinedHead = HeadNode.Children.empty() ? std::string() : Syntax.GetText(Head);
			const std::string &HeadName = HeadNode.Children.empty() ? HeadNode.Lexeme : JoinedHead;
			if(HeadNode.Children.empty()) Syntax.Charge(1 + HeadName.size());
			const std::string_view AbsolutePrefix = Syntax.GetRules().GetSpelling(Word::AbsoluteNamePrefix);
			const size_t Prefix = !AbsolutePrefix.empty() && Name.starts_with(AbsolutePrefix) ? AbsolutePrefix.size() : 0;
			const std::string_view LocalName = Name.substr(Prefix);
			const std::string_view Member = LocalName.starts_with(HeadName) ? LocalName.substr(HeadName.size()) : std::string_view();
			const BindingFact Fact = Syntax.GetBindingFact(Head, Member, static_cast<bool>(Prefix));
			const CallableIntroduction *const Binding = Fact.Source;
			const CallableNameOwner BindingOwner = Fact.Sources;
			const bool HasCustomDefinition = Fact.HasCustomDefinition;
			// 未初期化のローカル束縛を外側の標準名にしない事の返戻
			if(!Binding && Fact.Decl != NoNode && Fact.IsUncertain) return Unresolved();
			// 導入を要する標準対象なら未解決，其の他は未遮蔽の名前の返戻
			if(!Binding) {
				if(!Syntax.GetRules().ImplicitBindingTargets.empty()) {
					Syntax.Charge(1 + Name.size());
					const std::unordered_map<std::string_view, std::string_view>::const_iterator Implicit =
					Syntax.GetRules().ImplicitBindingTargets.find(Name);
					if(Implicit != Syntax.GetRules().ImplicitBindingTargets.end()) {
						ShouldCheckResolvedMembers = true;
						// 未遮蔽の暗黙の標準対象の返戻
						return Finish(Implicit->second);
					}
				}
				// 導入条件に従う対象の返戻
				return Finish(Profile.IsSpelling(Name, Word::ImportedTarget) ? std::string_view() : Name);
			}
			// 分解又は複合代入の対応が未確定な名前の返戻
			if(Binding->IsUncertain) return Unresolved();
			// 条件付で導入された名前の未確定の返戻
			if(const NodeId Guard = GetCallableGuard(*BindingOwner, *Binding); Guard != NoNode && !Syntax.HasNodeWithin(Guard, Head)) {
				// 別の実行範囲に属する供給値の未確定の返戻
				return Unresolved();
			}
			// 名前の参照以外の式を別名として追わない事の返戻
			if(Name.substr(Prefix, HeadName.size()) != HeadName) return Finish(std::string());
			const std::string Suffix(Binding->Member.empty() ? Name.substr(Prefix + HeadName.size()) : std::string_view());
			// インポートが参照する可変の値を，取得位置の固定した標準対象にしない事の返戻
			if(ShouldCheckImportedValue && Binding->AliasSrc != NoNode && !Fact.IsDeclaredConstant) return Finish(std::string());
			#if !defined(ShaveFormatRefFacts)
			const NodeId ResolutionSrc = Binding->AliasSrc == NoNode ? Binding->Name : Binding->AliasSrc;
			if(
				const std::unordered_map<NodeId, std::unordered_map<std::string, CallableResolution>>::const_iterator Known =
				BindingOwner->Resolutions.find(ResolutionSrc);
				Known != BindingOwner->Resolutions.end()
			) if(
				const std::unordered_map<std::string, CallableResolution>::const_iterator Resolved = Known->second.find(Suffix);
				Resolved != Known->second.end()
			) {
				const CallableResolution &Result = Resolved->second;
				Syntax.Charge(1 + Suffix.size() + Result.Name.size());
				HasResolvedContract = Result.HasContract;
				IsResolvedUnknown = Result.IsUnknown;
				ShouldCheckResolvedMembers = Result.ShouldCheckMembers;
				if(Result.Reference != NoNode) {
					Reference = Result.Reference;
					++ReferenceVersion;
				}
				// 導入時に確定した供給値を再び辿らない事の返戻
				return Finish(Result.Name);
			}
			#endif
			// 判定結果の返戻
			if(!Binding->Target.empty()) {
				// 宣言の導入を確認するインポートは，修飾の別名も供給元へ解決する
				if(Binding->SrcRoot != NoNode && !Syntax.GetRules().StandardImportPrefixes.empty()) {
					// 循環した供給元の未確定な解決の返戻
					if(!Seen.insert(Binding->Name).second) return Unresolved();
					Pending.push_back({ BindingOwner, Binding->Name, Suffix, ReferenceVersion });
					ShouldCheckImportedValue = true;
					AliasName = Binding->Target + Suffix;
					Name = AliasName;
					Head = Binding->SrcRoot;
					continue;
				}
				// 入力の独自定義又は供給元の遮蔽を標準の導入で覆わない事の返戻
				if(
					Binding->IsTerminatingImport && HasCustomDefinition ||
					Binding->SrcRoot != NoNode && HasShadowingDecl(Binding->SrcRoot, false, true)
				) return Finish(std::string());
				// インポート元の修飾名の返戻
				const std::string Target = Binding->Target + Suffix;
				if(Binding->IsValueImport && Suffix.empty()) {
					Reference = Binding->Name;
					++ReferenceVersion;
				}
				ShouldCheckResolvedMembers = true;
				// 使用位置の検査へ渡す標準対象の返戻
				return Finish(Target);
			}
			if(
				Binding->Function != NoNode && Suffix.empty() &&
				GetBodyReturnShapeOf(Binding->Function, false, true, Binding->Name).Kind == ReturnKind::Never
			) {
				HasResolvedContract = true;
				// 解決出来ない対象の返戻
				return Finish(std::string());
			}
			if(Binding->AliasSrc != NoNode) {
				Pending.push_back({ BindingOwner, Binding->AliasSrc, Suffix, ReferenceVersion });
				NodeId Src = Binding->AliasSrc;
				std::vector<NodeId> SourcePath;
				while(Src != NoNode) {
					Syntax.Charge(1);
					if(
						const std::unordered_map<NodeId, NodeId>::const_iterator Found = BindingOwner->AliasSources.find(Src);
						Found != BindingOwner->AliasSources.end()
					) {
						Src = Found->second;
						break;
					}
					SourcePath.push_back(Src);
					if(Syntax.Is(Src, Role::GroupingParen) && Syntax.GetNamedChildCount(Src) == 1) {
						Src = Syntax.GetNamedChild(Src, 0);
						continue;
					}
					if(!Syntax.Is(Src, Role::AssignmentOperation)) break;
					if(Syntax.Is(Src, Role::CompoundAssignment)) {
						Src = NoNode;
						break;
					}
					Src = Syntax.GetFieldChild(Src, ChildRole::Right);
				}
				for(const NodeId Source : SourcePath) {
					Syntax.Charge(1);
					BindingOwner->AliasSources.emplace(Source, Src);
				}
				// 複合代入又は供給値の欠落を確定した別名にしない事の返戻
				if(Src == NoNode) return Unresolved();
				const CalleeSummary Summary = GetCalleeSummary(Src);
				// 別名の取得時に置換される修飾名を標準対象にしない事の返戻
				if(Summary.HasMacroBinding) return Unresolved();
				// 呼出の戻値のメンバは，明示的な代入が在る場合だけ全文の照合へ進む
				if(Summary.HasCall && !HasCallableMember(Summary)) return Finish(std::string());
				// 別名を辿る呼出だけに循環検出の集合を用意する
				if(Seen.empty()) Seen.insert(Head);
				AliasName = Syntax.GetText(Src) + Suffix;
				Name = AliasName;
				Head = Summary.Head;
				if(Suffix.empty()) {
					Reference = Head;
					++ReferenceVersion;
				}
				// 既に解決を始めた先頭名へ戻った場合の未解決の返戻
				if(Head != NoNode && !Seen.insert(Head).second) return Finish(std::string());
				continue;
			}
			// 標準関数と同じ関数宣言は，標準登録を隠さない
			NodeId Owner = Syntax.GetParentOf(Binding->Name), Function = NoNode;
			bool HasPtr = false;
			while(Owner != NoNode && !Syntax.Is(Owner, Role::Decl) && !IsFlowBoundary(Owner)) {
				Syntax.Charge(1);
				if(Syntax.Is(Owner, Role::FunctionDeclarator)) Function = Owner;
				HasPtr = HasPtr || Syntax.Is(Owner, Role::PtrOrRefDeclarator);
				Owner = Syntax.GetParentOf(Owner);
			}
			if(
				Function != NoNode && !HasPtr && !HasCustomDefinition && Syntax.Is(Owner, Role::Decl) &&
				Profile.IsSpelling(Name, Word::StandardTerminalDecl) && Syntax.IsWord(GetResultTypeOf(Owner), Word::VoidType)
			) {
				const NodeId Params = Syntax.GetFieldChild(Function, ChildRole::Params);
				const size_t Count = Syntax.GetNamedChildCount(Params);
				const NodeId Param = Syntax.GetNamedChild(Params, 0), Type = Syntax.GetFieldChild(Param, ChildRole::Type);
				const NodeId Decl = Syntax.GetFieldChild(Param, ChildRole::Declarator);
				const bool HasNoArgs = Profile.IsSpelling(Name, Word::StandardTerminalNoArgs);
				// 標準と同じ返戻型・仮引数の再宣言の標準名の返戻
				if(
					HasNoArgs ?
					!Count || Count == 1 && Syntax.IsWord(Type, Word::VoidType) && Decl == NoNode :
					Count == 1 && IsPlainIntType(Type) && (Decl == NoNode || Syntax.Is(Decl, Role::Identifier))
				) {
					// 同じ契約の再宣言も，先行する標準の導入迄辿る
					if(!Seen.insert(Binding->Name).second) return Finish(std::string());
					Pending.push_back({ BindingOwner, Binding->Name, Suffix, ReferenceVersion });
					Head = Binding->Name;
					continue;
				}
			}
			// 解決出来ない対象の返戻
			return Finish(std::string());
		}
		// 解決出来ない対象の返戻
		return Finish(std::string());
	}

	/**
	 * 言語・標準ライブラリの終端名に束縛される呼出かの判定関数
	 * 計算量：未収集の呼出先のノード数 N，限定名の長さ B と導入の照会費用 Q に対し期待値 O(N + B + Q)
	 * @param Node 判定対象ノード
	 * @param IsRegisteredMacro 登録済のマクロかの格納先
	 * @return 非復帰の呼出の効果（通常復帰し得る場合は０）
	 * @throws FormatLimitExceeded 呼出先又は束縛の探索が作業予算を超える場合
	 * @throws std::bad_alloc 名前表又はテキストを確保出来ない場合
	 * @throws std::length_error 名前表又はテキストの長さが上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	uint8_t ReferenceProjection::GetCallEffects(const NodeId Node, bool &IsRegisteredMacro) const {
		// 呼出の構造と束縛の照合
		IsRegisteredMacro = false;
		const bool IsMacro = Syntax.Is(Node, Role::MacroCall);
		// 判定結果の返戻
		if(!IsMacro && !Syntax.Is(Node, Role::CallNode)) return 0;
		NodeId Function = GetCallTarget(Node);
		NodeId Method = NoNode;
		if(Syntax.Is(Node, Role::MethodInvocation)) {
			const NodeId Object = Syntax.GetFieldChild(Node, ChildRole::Object);
			Method = Syntax.GetFieldChild(Node, ChildRole::Name);
			// 呼出先の名前を持たない対象の返戻
			if(Method == NoNode) return 0;
			Function = Object == NoNode ? Method : Object;
			if(Object == NoNode) Method = NoNode;
		}
		// 末尾名が終端先へ解決され得ない属性呼出の返戻
		if(!IsMacro && Method == NoNode && CanSkipMemberResolution(Function)) return 0;
		const CalleeSummary Summary = GetCalleeSummary(Function);
		if(Summary.HasMacroBinding || Method != NoNode && Syntax.GetNode(Method).Facts.Has(ConstraintKind::MacroBinding)) {
			// 修飾名の末尾を含むマクロ置換の未確定な効果の返戻
			return UnknownCallEffect;
		}
		const std::string Suffix = Method == NoNode ? std::string() : "." + Syntax.GetText(Method);
		// 呼出結果のメンバの明示的な代入も，通常の名前解決へ渡す
		if(Summary.HasCall && !HasCallableMember(Summary)) return 0;
		const NodeId Head = Summary.Head;
		const bool IsLeaf = Function != NoNode && Syntax.GetNode(Function).Children.empty();
		const std::string JoinedName = IsLeaf && Suffix.empty() ? std::string() : Syntax.GetText(Function) + Suffix;
		const std::string_view CallName =
		IsLeaf && Suffix.empty() ? std::string_view(Syntax.GetNode(Function).Lexeme) : std::string_view(JoinedName);
		if(IsLeaf && Suffix.empty()) Syntax.Charge(1 + CallName.size());
		bool HasContract = false, IsUnknown = false;
		const std::string Name = ResolveCallName(Head, CallName, HasContract, &IsUnknown);
		Projected.Nodes[Node].Facts.Reference = IsMacro ?
		MakeMacroReference(Head, CallName, Name, IsUnknown) :
		std::make_shared<ReferenceFacts>(ReferenceFacts{ Name, Profile.ReadWordMeanings(Name), IsUnknown });
		// 未確定の呼出を通常復帰の既定値へ戻さない事の返戻
		if(IsUnknown) return UnknownCallEffect;
		IsRegisteredMacro =
		IsMacro && (Profile.IsSpelling(Name, Word::OrdinaryMacro) || Profile.IsSpelling(Name, Word::TerminatingMacro));
		// 判定結果の返戻
		if(!HasContract && !Profile.IsSpelling(Name, IsMacro ? Word::TerminatingMacro : Word::TerminatingCall)) return 0;
		// プロセス終了の効果の返戻
		if(Profile.IsSpelling(Name, Word::ProcessExitCall)) return ProcessExitEffect;
		// 標準の終了例外の効果の返戻
		if(Profile.IsSpelling(Name, Word::SystemExitCall)) return SystemExitEffect;
		// 非復帰だけの契約は，送出かプロセス終了かを確定しない
		return HasContract ? UnknownExitEffect : ThrowEffect;
	}

	/**
	 * トークン列の修飾されたマクロ名の解決関数
	 * 計算量：修飾の深さ D，名前長 L と束縛の照会量 Q に対し O(D + L + Q)
	 * @param Head マクロ名の最後の識別子
	 * @return 解決した役割と，変更を照会する束縛の事実
	 * @throws FormatLimitExceeded 名前又は束縛の照会が作業予算を超える場合
	 * @throws std::bad_alloc 名前又は作業列を確保出来ない場合
	 * @throws std::length_error 名前又は作業列が長さの上限を超える場合
	 */
	std::shared_ptr<const ReferenceFacts> ReferenceProjection::ResolveTokenMacro(NodeId Head) const {
		// 修飾を遡り，記述順に名前を組み立てる
		std::vector<NodeId> Parts { Head };
		while(Syntax.Is(Syntax.GetPrevSibling(Head), Role::ScopeQualifierToken)) {
			const NodeId Separator = Syntax.GetPrevSibling(Head), Prev = Syntax.GetPrevSibling(Separator);
			if(!Syntax.Is(Prev, Role::Identifier)) break;
			Parts.push_back(Separator);
			Parts.push_back(Prev);
			Head = Prev;
		}
		std::string Name;
		for(std::vector<NodeId>::const_reverse_iterator It = Parts.crbegin(); It != Parts.crend(); ++It) Name += Syntax.GetText(*It);
		bool HasContract = false;
		// 束縛を照合した名前の返戻
		return MakeMacroReference(Head, Name, ResolveCallName(Head, Name, HasContract));
	}

	/**
	 * メンバ代入の末尾名を保守的に収集する関数
	 * 非復帰への代入以外も残し，削除済の名前も残して候補の除外だけに使う
	 * 計算量：部分木のノード数 N，代入先の総長 L，候補名数 M に対し O(N + L log(M + 1))
	 * @param Root 収集対象の根
	 * @throws FormatLimitExceeded 走査又はテキストの収集が作業予算を超える場合
	 * @throws std::bad_alloc 候補名又は走査列を確保出来ない場合
	 * @throws std::length_error テキスト又は走査列の長さが上限を超える場合
	 */
	void ReferenceProjection::CollectTerminalMemberNames(const NodeId Root) const {
		// 代入で導入されるメンバ名の収集
		SyntaxModel::HasStoppedTraversal(
			Syntax,
			Root,
			[&](const NodeId Node) -> WalkStep {
				if(Syntax.Is(Node, Role::AssignmentOperation) || Syntax.Is(Node, Role::DeleteStmt)) {
					const NodeId Left = Syntax.Is(Node, Role::DeleteStmt) ? Node : Syntax.GetFieldChild(Node, ChildRole::Left);
					for(const NodeId Root : Syntax.Is(Node, Role::DeleteStmt) ? Syntax.GetChildrenOf(Node) : std::span<const NodeId>(&Left, 1)) {
						for(const NodeId Target : GetAssignedTargets(Root)) {
							const std::string Name = Syntax.GetText(Target);
							const size_t Dot = Name.rfind('.');
							if(Dot != std::string::npos) TerminalMemberNames.insert(Name.substr(Dot + 1));
						}
					}
				}
				// 内側の関数に在る代入も候補から落とさない
				return WalkStep::Descend;
			}
		);
		// 終了
		return;
	}

	/**
	 * 終端し得ない修飾呼出の名前解決の省略判定関数
	 * ローカル名の別名は修飾の末尾を保ち，メンバ代入だけが末尾を置き換え得る
	 * 単純名の呼出はインポートの別名・代入・非復帰の宣言の従来の照会へ渡す
	 * 計算量：初回の構文数 N，変更部分木の総量 D，末尾名長 L，候補数 M に対し O(N + D + L log(M + 1))
	 * @param Function 呼出先の式
	 * @return 標準終端名にもメンバ代入名にも一致しなければ真
	 * @throws FormatLimitExceeded 構文又は名前の走査が作業予算を超える場合
	 * @throws std::bad_alloc 名前又は作業列を確保出来ない場合
	 * @throws std::length_error 名前又は作業列の長さが上限を超える場合
	 */
	bool ReferenceProjection::CanSkipMemberResolution(const NodeId Function) const {
		// 関数全体でローカルを束縛する，大小を区別した属性参照だけを対象にする
		if(!Syntax.GetRules().HasFunctionLocalBindings || !Syntax.Is(Function, Role::Attribute)) return false;
		const uint32_t Count = Syntax.GetNamedChildCount(Function);
		const NodeId Member = Count ? Syntax.GetNamedChild(Function, Count - 1) : NoNode;
		// 計算したメンバ名と不完全な参照は従来の解決へ渡す
		if(Member == NoNode || Syntax.GetChildCount(Member) || !Syntax.Is(Member, Role::Identifier)) return false;
		if(!HasTerminalMemberNames) {
			for(const std::string_view Name : Syntax.GetRules().Words[static_cast<size_t>(Word::TerminatingCall)]) {
				Syntax.Charge(Name.size());
				const size_t Dot = Name.rfind('.');
				TerminalMemberNames.emplace(Dot == std::string_view::npos ? Name : Name.substr(Dot + 1));
			}
			CollectTerminalMemberNames(Syntax.GetRoot());
			HasTerminalMemberNames = true;
		} else for(const NodeId Root : ChangedCallNames) CollectTerminalMemberNames(Root);
		ChangedCallNames.clear();
		const std::string_view Name = Syntax.GetLexemeOf(Member);
		Syntax.Charge(Name.size() + 1);
		// 候補集合に無い末尾名の返戻
		const bool Result = !TerminalMemberNames.contains(Name);
		Projected.Nodes[Function].Facts.CannotResolveTerminalMember = Result;
		// 投影した候補の返戻
		return Result;
	}

	/**
	 * 参照先の名前を既存の束縛解決で確認する関数
	 * 計算量：参照の構造 S と束縛・別名の解決費用 Q に対し O(S + Q)
	 * @param Node デコレータ又はアノテーションの対象，宣言呼出の操作名
	 * @param CanInvokeConstructor アノテーションのコンストラクタ参照を許すか
	 * @return 確認した対象名（値を求める呼出又は未確定の対象は空）
	 * @throws FormatLimitExceeded 構文又は束縛の照会が作業予算を超える場合
	 * @throws std::bad_alloc 構造・束縛・テキストのメモリを確保出来ない場合
	 * @throws std::length_error 名前又は作業列の上限を超える場合
	 * @throws std::out_of_range ノード又は名前の番号が表に無い場合
	 */
	std::string ReferenceProjection::ResolveReference(const NodeId Node, const bool CanInvokeConstructor) const {
		NodeId Target = UnwrapCallTarget(Node);
		if(CanInvokeConstructor && Syntax.Is(Target, Role::CallNode)) Target = GetCallTarget(Target);
		const CalleeSummary Summary = GetCalleeSummary(Target);
		// 参照先の値を確定出来ない事の返戻
		if(Summary.Head == NoNode || Summary.HasCall) return {};
		bool HasContract = false;
		// 同じ束縛解決の結果の返戻
		return ResolveCallName(Summary.Head, Syntax.GetText(Target), HasContract);
	}

	/**
	 * 読違と祖先の所属を未取得にする生成関数
	 * 計算量：O(1)
	 */
	SourceProjector::ReadingFact::ReadingFact() noexcept : IsCall(false),
	IsComparison(false),
	IsProduct(false),
	IsLeadArgParen(false),
	HasCallContext(false),
	IsInCallArguments(false) {} // 判定と所属の初期化

	/**
	 * ノード数の取得関数
	 * 計算量：O(1)
	 * @return 参照する列のノード数
	 */
	size_t SourceProjector::ParsedNodes::GetSize() const {
		// 参照する列の長さの返戻
		return References.empty() ? Nodes.size() : References.size();
	}

	/**
	 * 元の解析文脈を保つノードの取得関数
	 * 計算量：O(1)
	 * @param Idx 列内の位置
	 * @return 所有者が保持するノードの値
	 */
	TSNode SourceProjector::ParsedNodes::operator[](const size_t Idx) const {
		// 記録の所有者からの値の返戻
		return References.empty() ? Nodes[Idx] : *References[Idx];
	}

	/**
	 * 外部の値の列を参照する生成関数
	 * 計算量：O(1)
	 * @param Nodes 呼出の間は生存する値の列
	 */
	SourceProjector::ParsedNodes::ParsedNodes(const std::span<const TSNode> Nodes) : Nodes(Nodes) {} // 値の列の参照
	/**
	 * 解析木の記録の参照列からの生成関数
	 * 計算量：O(1)
	 * @param References 呼出の間は生存する参照列とノードの所有者
	 */
	SourceProjector::ParsedNodes::ParsedNodes(const std::vector<const TSNode *> &References) : References(References) {} // 記録の参照

	/**
	 * コメント以外の３つの子を取得する関数
	 * 計算量：４番目のコードの子迄の子数 C と親子表の準備費用 R に対し平均 O(C + R)
	 * @param SrcTree 解析結果
	 * @param Node ノード
	 * @param Children 子の格納先（true の場合だけ全要素が有効）
	 * @return コメント以外の子が丁度３つなら true
	 * @throws std::bad_alloc 未構築の親子表のメモリを確保出来ない場合
	 * @throws std::length_error 未構築の親子表の要素数が上限を超える場合
	 */
	bool SourceProjector::IsCodeTripleReadSuccessful(
		const ::ParsedTree &SrcTree,
		const TSNode Node,
		std::array<TSNode, 3> &Children
	) {
		// 子の記録を読み，三つ組でない事が確定した時点で打ち切る
		size_t Count = 0;
		for(const ParsedChild &Child : SrcTree.GetChildrenOf(Node)) {
			if(SrcTree.GetProfile().GetKindOf(Child.Symbol).Roles.test(static_cast<size_t>(Role::Comment))) continue;
			// ４番目のコードの子を持つ事の返戻
			if(Count == Children.size()) return false;
			Children[Count++] = Child.Node;
		}
		// ３つの子を持つ事の返戻
		return Count == Children.size();
	}

	/**
	 * 空白・コメント・解析用の括弧を除くトークンの範囲の収集関数（整形仕様 4.4）
	 * 文法が子に出さず子の間の原文にだけ残るトークン（C# の修飾子付の暗黙型のラムダの仮引数 `(ref x) => x` の `(` 等）を，空白とコメントを除いて切り出す
	 * コメントの前置区切（全角スペースを含む）はコメントの配置が扱う為，トークンに含めない（整形仕様 7.6）
	 * 計算量：間隙のバイト数 B，コメント数 C，解析用の位置数 P，前置区切の走査と入力取得の総費用 Q に対し O(B log(2 + P) + C + log(2 + C) + Q)
	 * @param SrcTree コメントを分離した解析結果
	 * @param From 間隙の開始位置
	 * @param To 間隙の終了位置
	 * @param Gaps 間隙から除くコメントの範囲と解析用の括弧の位置
	 * @param Tokens トークンの範囲の格納先（領域を再使用する）
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	void SourceProjector::CollectHiddenTokens(
		const ::ParsedTree &SrcTree,
		const uint32_t From,
		const uint32_t To,
		const GapExclusions &Gaps,
		std::vector<std::pair<uint32_t, uint32_t>> &Tokens
	) {
		// 間隙から除くコメントと読取規則の準備
		Tokens.clear();
		// 空の間隙はトークン・コメント・挿入位置を持たない事の返戻
		if(From >= To) return;
		if(Gaps.Work) {
			Gaps.Work->Bytes += To - From;
			++Gaps.Work->Splits;
		}
		const std::vector<std::pair<uint32_t, uint32_t>> &CommentRanges = Gaps.CommentRanges;
		const ReadingRules &Reading = SrcTree.GetReading();
		std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Comment =
		std::lower_bound(CommentRanges.begin(), CommentRanges.end(), std::pair<uint32_t, uint32_t>(From, 0));
		// 補った範囲の内側かの判定
		const auto IsSynthetic = [&Gaps](const uint32_t Pos) -> bool {
			// 解析用の括弧の位置かの返戻
			return std::binary_search(Gaps.SyntheticPositions.begin(), Gaps.SyntheticPositions.end(), Pos);
		};
		// 次のコメントの前置区切の始まり
		uint32_t Lead = Comment == CommentRanges.end() ? To : TextEdit::SeparatorBefore(SrcTree, Reading, Comment->first).first;
		for(uint32_t Cursor = From; Cursor < To;) {
			if(Comment != CommentRanges.end() && Lead <= Cursor) {
				// 前置区切とコメントの読飛
				Cursor = std::max(Cursor, Comment->second);
				Lead = ++Comment == CommentRanges.end() ? To : TextEdit::SeparatorBefore(SrcTree, Reading, Comment->first).first;
				continue;
			}
			if(IsSynthetic(Cursor)) {
				++Cursor;
				continue;
			}
			if(const size_t Width = TextEdit::IntertokenSpaceAt(SrcTree, Reading, Cursor)) {
				Cursor += static_cast<uint32_t>(Width);
				continue;
			}
			// 空白かコメントの前置区切の前迄のトークン
			const uint32_t Start = Cursor, Stop = std::min(To, Lead);
			while(Cursor < Stop && !IsSynthetic(Cursor) && !TextEdit::IntertokenSpaceAt(SrcTree, Reading, Cursor)) ++Cursor;
			Tokens.emplace_back(Start, Cursor);
		}
		// 終了
		return;
	}

	/**
	 * １度分割した本文から間隙のトークンを取得する関数
	 * コメント内から始まる範囲は，開始位置以後のコメントだけを除く収集の契約を保つ
	 * 計算量：本文のトークン数 T，コメント数 C，照会のトークン数 K に対し O(log(T + 1) + log(C + 1) + K)，コメント内から始まる場合は其の範囲の分割費用 Q を加える
	 * @param SrcTree コメントを分離した解析結果
	 * @param From 間隙の開始位置
	 * @param To 間隙の終了位置
	 * @param Gaps １度分割した本文と端の切出の作業領域
	 * @return 次の照会迄有効なトークンの範囲
	 * @throws std::bad_alloc 端を切る作業領域を確保出来ない場合
	 * @throws std::length_error 作業領域の要素数が上限を超える場合
	 */
	std::span<const std::pair<uint32_t, uint32_t>> SourceProjector::HiddenTokens(
		const ::ParsedTree &SrcTree,
		const uint32_t From,
		const uint32_t To,
		GapExclusions &Gaps
	) {
		// 空の間隙の返戻
		if(From >= To) return {};
		if(Gaps.Work) ++Gaps.Work->Queries;
		// 短い空白だけの間隙は，共有トークン表を検索せずに確定出来る
		// 読む長さを制限し，重なる長い間隙を各祖先で再走査しない
		if(To - From < 17) {
			uint32_t Cursor = From;
			while(Cursor < To) {
				if(Gaps.Work) ++Gaps.Work->SpaceReads;
				const size_t Width = TextEdit::IntertokenSpaceAt(SrcTree, SrcTree.GetReading(), Cursor);
				if(!Width || Width > To - Cursor) break;
				Cursor += static_cast<uint32_t>(Width);
			}
			// 分割で全て除かれる空白の範囲の返戻
			if(Cursor == To) return {};
		}
		if(Gaps.Work) ++Gaps.Work->TokenSearches;
		const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Comment =
		std::lower_bound(Gaps.CommentRanges.begin(), Gaps.CommentRanges.end(), std::pair<uint32_t, uint32_t>(From, 0));
		if(Comment != Gaps.CommentRanges.begin() && std::prev(Comment)->second > From) {
			CollectHiddenTokens(SrcTree, From, To, Gaps, Gaps.Buffer);
			// コメントの途中からの従来の分割結果の返戻
			return Gaps.Buffer;
		}
		const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator First = std::partition_point(
			Gaps.Tokens.begin(),
			Gaps.Tokens.end(),
			[From](const std::pair<uint32_t, uint32_t> &Range) -> bool {
				// 開始位置より前に終わる範囲かの返戻
				return Range.second <= From;
			}
		);
		// トークンの無い間隙の返戻
		if(First == Gaps.Tokens.end() || First->first >= To) return {};
		const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Last = First->second >= To ?
		std::next(First) :
		std::lower_bound(std::next(First), Gaps.Tokens.cend(), std::pair<uint32_t, uint32_t>(To, 0));
		// 端を切らずに共有する範囲の返戻
		if(First->first >= From && std::prev(Last)->second <= To) return { First, Last };
		Gaps.Buffer.assign(First, Last);
		Gaps.Buffer.front().first = std::max(Gaps.Buffer.front().first, From);
		Gaps.Buffer.back().second = std::min(Gaps.Buffer.back().second, To);
		// 作業領域で端だけを切った範囲の返戻
		return Gaps.Buffer;
	}

	/**
	 * 文法が子に出さない語の種別の取得関数（整形仕様 4.2）
	 * 計算量：種別名のバイト数 B に対し平均 O(B)
	 * @param SrcTree コメントを分離した解析結果
	 * @param Host 語を子の間に持つノード
	 * @return ノードが子に出さない語の種別（其の様な語を持たないノードは無）
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	const KindDef *SourceProjector::GetHiddenLexemeKind(const ::ParsedTree &SrcTree, const TSNode Host) {
		// 文法が子に出さない語の種別の取得
		const std::unordered_map<std::string_view, const KindDef *> &Kinds = SrcTree.GetReading().HiddenLexemeKinds;
		const std::unordered_map<std::string_view, const KindDef *>::const_iterator Found =
		Kinds.find(SrcTree.GetProfile().GetSymbolName(SrcTree.GetSymbol(Host)));
		// 語の種別の返戻
		return Found == Kinds.end() ? nullptr : Found->second;
	}

	/**
	 * 比較と誤読したジェネリック呼出の判定関数
	 * `<`・`>` を演算子とする二項式で，`>` の後の部分が `(` から始まる形を判定する
	 * 二項式のネストと，一覧の隣接する要素へ分かれた形を調べる（整形仕様 6.6）
	 * 子はコメントを除いて数え，文中のコメントの有無で判定を変えない（整形仕様 1.2）
	 * 計算量：取得する子の総数 C と解析木の取得・判定の総費用 Q に対し O(C + Q)
	 * @param SrcTree 解析結果
	 * @param Node 判定するノード
	 * @return 誤読したジェネリック呼出の外側の二項式なら true
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	bool SourceProjector::IsMisparsedGenericCall(const ::ParsedTree &SrcTree, const TSNode Node) {
		// 型引数を持たない言語と二項式でないノードの返戻
		if(ts_node_is_null(Node) || !SrcTree.GetProfile().GetProjectionChecks(SrcTree.GetSymbol(Node)).CanReadGenericCall) return false;
		std::array<TSNode, 3> Parts;
		// 閉じの山括弧の三つ組でないノードの返戻
		if(!IsCodeTripleReadSuccessful(SrcTree, Node, Parts) || !SrcTree.Is(Parts[1], Role::GreaterThanToken)) return false;
		// 閉じの後に続く部分の最左のトークン（呼出・メンバ参照へ続く形もジェネリック呼出の確定条件を満たす）
		TSNode Leftmost = Parts[2];
		while(SrcTree.GetChildCount(Leftmost)) {
			TSNode First {};
			ForEachChild(
				Leftmost,
				[&SrcTree, &First](const TSNode Child) -> bool {
					// コメントを読み飛ばし，最初のコードの子で走査を終える
					if(SrcTree.Is(Child, Role::Comment)) return true;
					First = Child;
					// 発見した事の返戻
					return false;
				}
			);
			if(ts_node_is_null(First)) break;
			Leftmost = First;
		}
		// 閉じの後が実引数の括弧で始まらない場合の返戻
		if(!SrcTree.Is(Leftmost, Role::OpeningParenToken)) return false;
		// 左の子の三つ組を調べる有効範囲
		{
			std::array<TSNode, 3> Left;
			const bool IsBinary = SrcTree.Is(Parts[0], Role::BinaryOperation);
			// 左が `<` を演算子とする二項式である事の返戻
			if(IsBinary && IsCodeTripleReadSuccessful(SrcTree, Parts[0], Left) && SrcTree.Is(Left[1], Role::LessThanToken)) return true;
		}
		// 型引数を一覧の要素へ分割した形でない場合の返戻
		if(!SrcTree.Is(ts_node_parent(Node), Role::TypeArgLookalikeSlot)) return false;
		TSNode Earlier = ts_node_prev_named_sibling(Node);
		while(!ts_node_is_null(Earlier) && SrcTree.Is(Earlier, Role::Comment)) Earlier = ts_node_prev_named_sibling(Earlier);
		// 先行する二項式が無い場合の返戻
		if(!SrcTree.Is(Earlier, Role::BinaryOperation)) return false;
		std::array<TSNode, 3> Prev;
		// 直前の要素が型引数の開きを持つ二項式である事の返戻
		return IsCodeTripleReadSuccessful(SrcTree, Earlier, Prev) && SrcTree.Is(Prev[1], Role::LessThanToken);
	}

	/**
	 * 空白を除きコメントをマーカーとしたテキストの追記関数
	 * 計算量：対象のバイト数 B，コメント数 C，解析用の区切数 P，入力取得の総費用 Q に対し O(B + C + P + log(2 + C) + log(2 + P) + Q)
	 * @param SrcTree 解析済のソース
	 * @param Start 追記する範囲の始まり
	 * @param End 追記する範囲の終わり
	 * @param Comments コメントの範囲の並び（開始位置の昇順）
	 * @param Output 追記先
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	void SourceProjector::AppendCode(
		const ::ParsedTree &SrcTree,
		uint32_t Start,
		const uint32_t End,
		const std::vector<std::pair<uint32_t, uint32_t>> &Comments,
		std::string &Output
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
		// コメントを跨がない連続した断片だけをデコードし，保持する字を数える
		const bool NeedsDecoding = SrcTree.HasCodeTextDecoding();
		uint32_t FragmentStart = Start;
		const auto AppendFragment = [&](const uint32_t Limit) -> void {
			const std::string Text = ParseAdapter::DecodeLexeme(
				SrcTree,
				std::string(std::string_view(SrcTree).substr(FragmentStart, Limit - FragmentStart)),
				FragmentStart,
				true
			);
			for(const unsigned char Char : Text) if(!std::isspace(Char)) Output.push_back(Char);
			// 終了
			return;
		};
		// 解析用の区切を除くテキストの追記
		const std::vector<uint32_t> &Inserted = SrcTree.GetInsertedBreaks();
		std::vector<uint32_t>::const_iterator Semicolon = std::lower_bound(Inserted.begin(), Inserted.end(), Start);
		while(Start < End) {
			while(Semicolon != Inserted.end() && *Semicolon < Start) ++Semicolon;
			if(Comment != Comments.end() && Comment->first <= Start) {
				if(NeedsDecoding) AppendFragment(Start);
				// 連続するコメントを１つの NUL マーカーで表す追記（部分整形の入力は NUL を含まない）
				if(Output.empty() || Output.back() != '\0') Output.push_back('\0');
				Start = Comment++->second;
				FragmentStart = Start;
				continue;
			}
			if(Semicolon != Inserted.end() && *Semicolon == Start) {
				if(NeedsDecoding) AppendFragment(Start);
				++Start;
				FragmentStart = Start;
				++Semicolon;
				continue;
			}
			// 空白以外の字の追記
			if(NeedsDecoding) {
				++Start;
				continue;
			}
			if(const unsigned char Char = SrcTree[Start++]; !std::isspace(Char)) Output.push_back(Char);
		}
		if(NeedsDecoding && FragmentStart < End) AppendFragment(End);
		// 終了
		return;
	}

	/**
	 * テキストの出現数の一括計数関数
	 * マーカー（コメントの位置）は整形が差し込む物（ドキュメントコメントの雛形等）も有る為，マーカーを除いた字で数える
	 * 計算量：テキストの全体の長さ N と数えるテキストの長さの和 L に対し O(N + L)（遷移の表の引きを O(1) と看做す）
	 * @param Code 空白を除きコメントをマーカーとしたテキストの全体
	 * @param Texts 数えるテキストの並び
	 * @return テキスト毎の出現数（重なる出現も数え，マーカーだけのテキストは０）
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	std::vector<uint32_t> SourceProjector::CountZones(const std::string_view Code, const std::vector<std::string_view> &Texts) {
		// Aho–Corasick 法の照合状態・遷移表・各テキストの終端状態の準備
		using LiteralEdges = std::vector<std::pair<unsigned char, uint32_t>>; // １状態から進む字と遷移先
		std::unordered_map<uint64_t, uint32_t> Goto;
		std::vector<LiteralEdges> Children(1);
		std::vector<uint32_t> Fail(1, 0), Terminal;
		Terminal.reserve(Texts.size());
		// 文字を読んだ後の照合状態の取得
		const auto Edge = [&Goto](const uint32_t State, const unsigned char Char) -> uint32_t {
			const std::unordered_map<uint64_t, uint32_t>::const_iterator Iter = Goto.find(static_cast<uint64_t>(State) << 8 | Char);
			// 遷移先の返戻（無ければ根の０）
			return Iter == Goto.end() ? 0 : Iter->second;
		};
		// テキストの木の構築
		for(const std::string_view Text : Texts) {
			uint32_t State = 0;
			for(const unsigned char Char : Text) {
				// マーカーの読飛
				if(!Char) continue;
				const std::pair<std::unordered_map<uint64_t, uint32_t>::iterator, bool> Added =
				Goto.try_emplace(static_cast<uint64_t>(State) << 8 | Char, static_cast<uint32_t>(Fail.size()));
				if(Added.second) {
					Children[State].push_back({ Char, Added.first->second });
					Children.emplace_back();
					Fail.push_back(0);
				}
				State = Added.first->second;
			}
			Terminal.push_back(State);
		}
		// 幅優先の順と失敗の遷移の算出
		std::vector<uint32_t> Order;
		Order.reserve(Fail.size());
		for(const std::pair<unsigned char, uint32_t> &Transition : Children[0]) Order.push_back(Transition.second);
		for(size_t Head = 0; Head < Order.size(); ++Head) {
			const uint32_t State = Order[Head];
			for(const std::pair<unsigned char, uint32_t> &Transition : Children[State]) {
				const unsigned char Byte = Transition.first;
				const uint32_t Child = Transition.second;
				uint32_t Back = Fail[State];
				// 同じ字で進める最長の接尾辞の探索
				while(Back && !Edge(Back, Byte)) Back = Fail[Back];
				Fail[Child] = Edge(Back, Byte);
				Order.push_back(Child);
			}
		}
		// テキスト全体の走査と状態毎の到達数
		std::vector<uint32_t> Hits(Fail.size(), 0);
		uint32_t State = 0;
		for(const unsigned char Char : Code) {
			if(!Char) continue;
			while(State && !Edge(State, Char)) State = Fail[State];
			State = Edge(State, Char);
			++Hits[State];
		}
		// 失敗の遷移の先（接尾辞のテキスト）への到達数の累積
		for(size_t Idx = Order.size(); Idx--;) Hits[Fail[Order[Idx]]] += Hits[Order[Idx]];
		// 状態毎の到達数からテキスト毎の出現数への変換
		std::vector<uint32_t> Counts;
		Counts.reserve(Terminal.size());
		for(const uint32_t End : Terminal) Counts.push_back(End ? Hits[End] : 0);
		// テキスト毎の出現数の返戻
		return Counts;
	}

	/**
	 * 片側だけ空白の有る Swift の利用者定義演算子を持つかの判定関数
	 * Swift は演算子の前後の空白の有無で前置・後置・中置を見分け（`a^^` の後に改行が続けば後置），両側に空白を置くと中置に変わる
	 * パーサーは宣言を知らず中置の式と読む為（`a^^↵print(b)` を `a ^^ print(b)`），片側だけ空白の有る利用者定義演算子の式は原文の儘コピーする
	 * 計算量：検査する子の数 C と解析結果の取得・判定の総費用 Q に対し O(C + Q)
	 * @param SrcTree 解析結果
	 * @param Children 中置の式の子と記号の共有列
	 * @return 片側だけ空白の有る利用者定義演算子を直接の子に持つ Swift の中置の式なら true
	 */
	bool SourceProjector::HasLopsidedOperator(const ::ParsedTree &SrcTree, const std::span<const ParsedChild> Children) {
		// 兄弟を欠く両端を除き，共有した子列から左右の隣接を読む
		for(size_t Idx = 1; Idx + 1 < Children.size(); ++Idx) {
			const ParsedChild &Child = Children[Idx];
			if(!SrcTree.GetProfile().GetKindOf(Child.Symbol).Roles.test(static_cast<size_t>(Role::UserDefinedOperator))) continue;
			const bool IsLeftAttached = Children[Idx - 1].End == Child.GetStart();
			const bool IsRightAttached = Child.End == Children[Idx + 1].GetStart();
			// 片側だけ空白の有る演算子かの返戻
			if(IsLeftAttached != IsRightAttached) return true;
		}
		// 片側だけ空白の有る演算子を持たない事の返戻
		return false;
	}

	/**
	 * アンカーへ紐付けたコメントを除いた子の数の取得関数（整形仕様 7.1）
	 * 分離したコメントは原文の木の子に残る為，除いた跡へトークンの融合を防ぐ CSS の区切記法を置く物だけを子に数える
	 * 計算量：子の数 C と分離記録・解析結果の取得と判定の総費用 Q に対し O(C + Q)
	 * @param SrcTree 解析結果
	 * @param Separated 投影の間だけ持つコメントの分離の結果
	 * @param Node パーサーのノード
	 * @param IsNamedOnly 名前付の子だけを数えるか
	 * @param First 最初に数えた子の格納先（不要なら nullptr）
	 * @return 数えた子の数
	 * @throws std::bad_alloc 未構築の親子表を確保出来ない場合
	 * @throws std::length_error 親子表の要素数上限を超える場合
	 */
	uint32_t SourceProjector::CountCodeChildren(
		const ::ParsedTree &SrcTree,
		const CommentSeparation &Separated,
		const TSNode Node,
		const bool IsNamedOnly,
		TSNode *const First
	) {
		// 分離したコメントを除く子の計数
		uint32_t Count = 0;
		for(const ParsedChild &Part : SrcTree.GetChildrenOf(Node)) {
			const TSNode Child = Part.Node;
			// 名前無の子と，跡へ区切記法を置かない分離したコメントの読飛
			if(IsNamedOnly && !Part.IsNamed || Separated.IsSeparatedComment(SrcTree, Child) && !Separated.IsNotationSeparated(Child)) {
				continue;
			}
			if(!Count && First) *First = Child;
			++Count;
		}
		// 数えた子の数の返戻
		return Count;
	}

	/**
	 * CSS の `url(...)` の引数の並びのテキストの取得関数
	 * CSS の URL は丸括弧の内側だけ詰め，参照先のテキストを保つ
	 * 計算量：引数のバイト数 B に対し O(B)
	 * @param Args デコードした引数の並びのテキスト
	 * @return 丸括弧の内側の両端の余白を詰めたテキスト
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	std::string SourceProjector::GetCssUrlArgsText(const std::string_view Args) {
		// 参照先を変えずに詰める丸括弧内の範囲
		std::string_view Text = Args;
		const bool IsOpen = Text.starts_with('('), IsClose = Text.size() > 1 && Text.ends_with(')');
		// 括弧の外形と内側の余白の分離
		Text.remove_prefix(IsOpen);
		// 対象のテキストからの末尾区切除去
		Text.remove_suffix(IsClose);
		TextEdit::TrimView(Text);
		// 組立中の結果
		std::string Result;
		// 組立中の結果の格納領域予約
		Result.reserve(Text.size() + 2);
		// 原文に存在した括弧だけの復元
		if(IsOpen) Result += '(';
		// 組立中の結果へのテキスト追加
		Result.append(Text);
		if(IsClose) Result += ')';
		// 詰めた原文の返戻
		return Result;
	}

	/**
	 * 出所のパーサーノードの取得関数
	 * 計算量：O(1)
	 * @param Src 投影したノードの出所
	 * @param ValidationNodes 検証データに含むパーサーノード
	 * @param ExtraNodes 検証データと異なる投影ノード
	 * @return 出所のパーサーノード（隠れたトークンでは空）
	 */
	TSNode SourceProjector::SrcNode(
		const Origin &Src,
		const ParsedNodes ValidationNodes,
		const std::span<const TSNode> ExtraNodes
	) {
		// 隠れたトークンの空のノードの返戻
		if(Src.NodeIdx == NoNode) return {};
		// 検証データと異なる投影ノードの返戻
		if(Src.NodeIdx & ExtraNodeBit) return ExtraNodes[Src.NodeIdx & ~ExtraNodeBit];
		// 検証データに含むノードの返戻
		return ValidationNodes[Src.NodeIdx];
	}

	/**
	 * 構文エラーの周りのテキストの収集関数
	 * 部分整形はパーサーが読めなかった範囲 (`ERROR`) の周りを誤読した木で編集し得る（区切の補完・隣の括弧の除去・コメントの差込）
	 * 其の範囲と前後に接する字を集める（空白を除き，コメントは位置のマーカーとする）
	 * 整形後の並びの保持は `CountLostErrorZones` が照合する
	 * 計算量：入力の長さ N，ノードの数 V に対し O(N log N + V)
	 * @param SrcTree 解析済の入力（コメントを含む）
	 * @param Nodes 投影のノードの走査で記録した原文順のノード
	 * @param Errors 共通の祖先経路で分類済の最外のエラーノード
	 * @param ProtectedRanges 領域毎の原文範囲の格納先（不要なら null，空で渡す）
	 * @return テキストと入力全体での出現数の並び（原文順，重なる範囲は統合，空白除去・コメントのマーカー化済）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	std::vector<ValidationContext::ErrorZone> SourceProjector::CollectErrorZones(
		const ::ParsedTree &SrcTree,
		const ParsedNodes Nodes,
		const std::vector<TSNode> &Errors,
		std::vector<std::pair<uint32_t, uint32_t>> *const ProtectedRanges
	) {
		// 構文エラーの周りのテキストの収集
		std::vector<ValidationContext::ErrorZone> Zones;
		// 解析木の無い入力の返戻
		if(!SrcTree.IsParsed()) return Zones;
		const TSNode Root = SrcTree.GetRoot();
		// 構文エラーの無い入力の返戻
		if(!SrcTree.HasError(Root)) return Zones;
		// 構文エラーの照合に使うコメント範囲
		std::vector<std::pair<uint32_t, uint32_t>> Comments;
		for(size_t NodeIdx = 0; NodeIdx < Nodes.GetSize(); ++NodeIdx) {
			if(const TSNode Node = Nodes[NodeIdx]; !ts_node_eq(Node, Root) && SrcTree.Is(Node, Role::Comment)) {
				Comments.push_back({ SrcTree.GetStart(Node), SrcTree.GetEnd(Node) });
			}
		}
		std::sort(Comments.begin(), Comments.end());
		Comments.erase(std::unique(Comments.begin(), Comments.end()), Comments.end());
		const uint32_t Size = static_cast<uint32_t>(SrcTree.GetCodeText().size());
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
		std::vector<std::pair<uint32_t, uint32_t>> Ranges, Bodies;
		// 分類済の最外のエラーからの保護範囲と所有ノードの収集
		for(const TSNode Node : Errors) {
			// 前に接する字の位置（空白とコメントを越える）
			uint32_t Before = SrcTree.GetStart(Node);
			while(Before) {
				if(std::isspace(static_cast<unsigned char>(SrcTree[Before - 1]))) --Before;
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
			uint32_t After = SrcTree.GetEnd(Node);
			while(After < Size) {
				if(std::isspace(static_cast<unsigned char>(SrcTree[After]))) ++After;
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
			if(!Ranges.empty() && Before <= Ranges.back().second) {
				Ranges.back().second = std::max(Ranges.back().second, After);
				Bodies.back().second = std::max(Bodies.back().second, SrcTree.GetEnd(Node));
			} else {
				Ranges.push_back({ Before, After });
				Bodies.emplace_back(SrcTree.GetStart(Node), SrcTree.GetEnd(Node));
			}
		}
		// 出現数を数える入力の全体のテキスト
		std::string Code;
		AppendCode(SrcTree, 0, Size, Comments, Code);
		// 範囲毎のテキストの抽出（照合する字の無い範囲は除く）
		for(size_t Idx = 0; Idx < Ranges.size(); ++Idx) {
			const std::pair<uint32_t, uint32_t> &Range = Ranges[Idx];
			std::string Text;
			AppendCode(SrcTree, Range.first, Range.second, Comments, Text);
			if(!Text.empty()) {
				Zones.push_back({ std::move(Text), 0 });
				if(ProtectedRanges) ProtectedRanges->push_back(Bodies[Idx]);
			}
		}
		// 入力の全体での出現数の一括計数
		std::vector<std::string_view> Texts;
		Texts.reserve(Zones.size());
		for(const ValidationContext::ErrorZone &Zone : Zones) Texts.push_back(Zone.Text);
		const std::vector<uint32_t> Counts = CountZones(Code, Texts);
		for(size_t Idx = 0; Idx < Zones.size(); ++Idx) Zones[Idx].Count = Counts[Idx];
		// テキストの並びの返戻
		return Zones;
	}

	/**
	 * 意味上の区別の収集関数（整形仕様 3.2，8.1）
	 * 関数・ラムダの種類（束縛・構築呼出の可否が異なる関数式とアロー関数等）と，投影が組み替える前のパーサーの結付から言語の規則で定まる結付を集める
	 * 前置・二項の演算子の右の被演算子について，後置式を結び直した後の範囲内の名前のトークン数を記録する
	 * 制御構文の本体について，空の文・空のブロック・本体より前の閉タグに依る空の本体の区別を記録する
	 * 此れらは整形で変わる括弧・トークンの数に依らない量とし，最終検証で完成候補の解析木と照合する
	 * 空のディレクティブは解析結果から数・順序を採り，ディレクティブの投影の欠落・複製も独立して照合する
	 * 共通の木の走査で選んだ候補の番号を読み，未修復のエラーを含む構文はエラー領域の照合に委ねて除く
	 * 計算量：ノード数 V，デコードデータの数 R，被演算子の連鎖の長さ L に対し O(V (L + log V + log(R + 1)))
	 * @param SrcTree 解析済のソース
	 * @param UnrepairedSubtrees 宣言の修復後もエラーを含む部分木（修復が無ければ null）
	 * @return 関数の種類（始まりの順），演算子の右の被演算子（演算子の順），制御構文の本体（始まりの順）と，空のディレクティブ（始まりの順）を続けた並び
	 * @throws std::bad_alloc 意味の照合データ又は結付の表を確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	std::vector<ValidationContext::SemanticCheck> SourceProjector::CollectSemanticChecks(
		const ::ParsedTree &SrcTree,
		const std::unordered_set<const void *> *const UnrepairedSubtrees
	) {
		// 意味上の区別の収集
		std::vector<ValidationContext::SemanticCheck> Checks;
		// 解析して居ないソースの空の並びの返戻
		if(!SrcTree.IsParsed()) return Checks;
		const ReadingRules &Reading = SrcTree.GetReading();
		SrcTree.RecordNodeRelations();
		const ::ParsedTree::NodeRelations &Relations = *SrcTree.Syntax.Relations;
		const bool HasTreeError = Relations.Root.HasError;
		// 修復の証明が無いエラーを含むか（宣言の修復が有れば修復後もエラーを含む部分木だけを数える）
		const auto HasError = [&](const ParsedChild &Record) -> bool {
			// 修復後の事実又は同じ木の記録に依る判定の返戻
			return HasTreeError && (UnrepairedSubtrees ? UnrepairedSubtrees->contains(Record.Node.id) : Record.HasError);
		};
		// 名前のトークンの始まり（原文順）
		std::vector<uint32_t> Names;
		if(Reading.DoesMisbindPostfixOperand) {
			for(const ::ParsedTree::SemanticNode &Candidate : Relations.SemanticNodes) if(Candidate.IsName) {
				Names.push_back(SrcTree.GetNodeAt(Candidate.Relation).GetStart());
			}
			std::sort(Names.begin(), Names.end());
		}
		// コメントを除く最後の子
		const auto LastCodeChild = [&SrcTree](const TSNode Node) -> TSNode {
			for(uint32_t Idx = SrcTree.GetChildCount(Node); Idx--;) {
				// コメントでない子の返戻
				if(const TSNode Child = SrcTree.GetChild(Node, Idx); !SrcTree.Is(Child, Role::Comment)) return Child;
			}
			// 子の無い事の返戻
			return TSNode{};
		};
		// 範囲の名前のトークンの数
		const auto CountNames = [&Names](const uint32_t Start, const uint32_t End) -> uint32_t {
			const std::vector<uint32_t>::const_iterator From = std::lower_bound(Names.begin(), Names.end(), Start);
			// 範囲の始まりから終わりの前迄の数の返戻
			return static_cast<uint32_t>(std::lower_bound(From, Names.cend(), End) - From);
		};
		// 後置の式を結び直す前置・二項の式か
		const auto IsOperation = [&SrcTree](const TSNode Node) -> bool {
			// 被演算子を持つ前置・二項の式かの返戻
			return SrcTree.Is(Node, Role::PostfixBindingOperand) && SrcTree.GetChildCount(Node) > 1;
		};
		// 前置・二項の式毎の右の被演算子の意味上の終わり（後置の式が最も右の被演算子へ結び付くと其の後置の式の終わり迄）
		std::unordered_map<const void *, uint32_t> OperandEnds;
		// 後置の式を結び直した後に其の位置を占める前置・二項の式（内側の後置の式から順に定まる）
		std::unordered_map<const void *, TSNode> Replacing;
		if(Reading.DoesMisbindPostfixOperand) {
			for(const ::ParsedTree::SemanticNode &Candidate : std::views::reverse(Relations.SemanticNodes)) {
				if(!Candidate.IsPostfix) continue;
				const ParsedChild &Record = SrcTree.GetNodeAt(Candidate.Relation);
				if(HasError(Record)) continue;
				const TSNode Node = Record.Node;
				TSNode Operand = SrcTree.GetChild(Node, 0);
				if(
					const std::unordered_map<const void *, TSNode>::const_iterator Found = Replacing.find(Operand.id);
					Found != Replacing.end()
				) Operand = Found->second;
				if(!IsOperation(Operand)) continue;
				Replacing.emplace(Node.id, Operand);
				for(TSNode Operation = Operand; IsOperation(Operation); Operation = LastCodeChild(Operation)) {
					uint32_t &End = OperandEnds.try_emplace(Operation.id, SrcTree.GetEnd(Operation)).first->second;
					End = std::max(End, SrcTree.GetEnd(Node));
				}
			}
		}
		// 種類毎の区別と其の並べ順のキー
		using OrderedChecks = std::vector<std::pair<uint32_t, ValidationContext::SemanticCheck>>;
		OrderedChecks Functions, Operations, Bodies;
		std::vector<ValidationContext::SemanticCheck> NullDirectives;
		for(const ::ParsedTree::SemanticNode &Candidate : Relations.SemanticNodes) {
			// 照合項目を持つ種別だけで部分木のエラーを照会し，４種類の記録の順は保つ
			if(!Candidate.IsFunction && !Candidate.IsDirective && !Candidate.IsOperand && !Candidate.IsBody) continue;
			const ParsedChild &Record = SrcTree.GetNodeAt(Candidate.Relation);
			if(HasError(Record)) continue;
			const TSNode Node = Record.Node;
			const TSSymbol Symbol = Record.Symbol;
			if(Candidate.IsDirective && ParseAdapter::IsNullDirective(SrcTree, Node)) NullDirectives.push_back({ Symbol, 0 });
			if(Candidate.IsFunction) Functions.push_back({ Record.GetStart(), { Symbol, 0 } });
			if(Reading.DoesMisbindPostfixOperand && IsOperation(Node)) {
				// 演算子のトークン（最初の名前の無い子）の位置と，右の被演算子の意味上の範囲の名前のトークンの数
				uint32_t Operator = SrcTree.GetStart(Node);
				for(uint32_t Idx = 0; Idx < SrcTree.GetChildCount(Node); ++Idx) {
					const TSNode Child = SrcTree.GetChild(Node, Idx);
					if(SrcTree.IsNamed(Child)) continue;
					Operator = SrcTree.GetStart(Child);
					break;
				}
				const std::unordered_map<const void *, uint32_t>::const_iterator Found = OperandEnds.find(Node.id);
				Operations.push_back(
					{
						Operator,
						{ 0, CountNames(SrcTree.GetStart(LastCodeChild(Node)), Found == OperandEnds.end() ? SrcTree.GetEnd(Node) : Found->second) }
					}
				);
			}
			if(Reading.DoesMisbindPostfixOperand && SrcTree.Is(Node, Role::OperatorCall) && SrcTree.GetChildCount(Node) == 2) {
				// 前置の演算子の後の括弧を演算子の呼出と読んだ式（中置の式の末尾のトークンの演算子を含む）は，括弧の中を右の被演算子とする前置の演算
				TSNode Prefix = SrcTree.GetChild(Node, 0);
				if(IsOperation(Prefix)) Prefix = LastCodeChild(Prefix);
				if(
					const TSNode Suffix = SrcTree.GetChild(Node, 1);
					(!SrcTree.IsNamed(Prefix) || SrcTree.Is(Prefix, Role::ExclamationMark)) && SrcTree.Is(Suffix, Role::CallSuffix) &&
					ts_node_named_child_count(Suffix) == 1
				) Operations.push_back({ SrcTree.GetStart(Prefix), { 0, CountNames(SrcTree.GetStart(Suffix), SrcTree.GetEnd(Suffix)) } });
			}
			if(Candidate.IsBody) {
				const TSNode Body = SrcTree.GetFieldChild(Node, ChildRole::Body);
				if(ts_node_is_null(Body)) continue;
				// 空の文と文を持たないブロックの本体と，本体より前の閉タグが文を終える（コロンで本体を開く代替の構文を除く）本体は空
				bool IsEmpty = SrcTree.Is(Body, Role::EmptyStmt) || SrcTree.Is(Body, Role::CompoundBody) && !HasChildOf(
					Body,
					[&SrcTree](const TSNode Child) -> bool {
						// コメントでない名前付の子かの返戻
						return SrcTree.IsNamed(Child) && !SrcTree.Is(Child, Role::Comment);
					}
				);
				for(uint32_t Idx = 0; Idx < SrcTree.GetChildCount(Node) && !ts_node_eq(SrcTree.GetChild(Node, Idx), Body); ++Idx) {
					if(SrcTree.Is(SrcTree.GetChild(Node, Idx), Role::ColonToken)) break;
					IsEmpty = IsEmpty || SrcTree.Is(SrcTree.GetChild(Node, Idx), Role::CodeBoundary);
				}
				Bodies.push_back({ SrcTree.GetStart(Node), { 0, IsEmpty } });
			}
		}
		// 種類毎の原文順に依る照合データの連結
		for(OrderedChecks *const Kind : { &Functions, &Operations, &Bodies }) {
			std::stable_sort(
				Kind->begin(),
				Kind->end(),
				[](
					const std::pair<uint32_t, ValidationContext::SemanticCheck> &Lhs,
					const std::pair<uint32_t, ValidationContext::SemanticCheck> &Rhs
				) -> bool {
					// 並べ順のキーの比較の返戻
					return Lhs.first < Rhs.first;
				}
			);
			for(const std::pair<uint32_t, ValidationContext::SemanticCheck> &Entry : *Kind) Checks.push_back(Entry.second);
		}
		// 記録済の空のディレクティブのパーサーノードの原文順での連結
		Checks.insert(Checks.end(), NullDirectives.begin(), NullDirectives.end());
		// 意味上の区別の並びの返戻
		return Checks;
	}

	/**
	 * 山括弧の誤解析の一括判定関数
	 * C++ の `b < c && d > e`，C#・TypeScript の `a < b, c >> d` 等を型引数リストと誤読した形を構造で見分ける
	 * 計算量：入力のバイト数 B，訪問ノード数 N，解析木の取得・判定の総費用 Q に対し O(B + N + Q)
	 * @param SrcTree ソースコード
	 * @return 実体化のノードと型引数リスト毎の誤解析の判定（投影の間だけ持つ）
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	std::unordered_map<const void *, uint8_t> SourceProjector::PrepareTemplateContexts(const ::ParsedTree &SrcTree) {
		// ノード別の判定控え
		std::unordered_map<const void *, uint8_t> Memo;
		// 型引数を誤読し得ない言語と，型引数リストを持ち得ない（`<` の無い）ソースの空の判定の返戻
		if(!SrcTree.GetReading().CanMisreadTemplateArgs || SrcTree.GetCodeText().find('<') == std::string_view::npos) return Memo;
		SrcTree.RecordNodeRelations();
		// 同じ世代の記号と子列を，記録番号から直接読む
		const auto HasRole = [&SrcTree](const ParsedChild *const Record, const Role Wanted) -> bool {
			// 空のノードは役割を持たない事の返戻
			return Record && SrcTree.GetProfile().GetKindOf(Record->Symbol).Roles.test(static_cast<size_t>(Wanted));
		};
		const auto FirstNamedChild = [&SrcTree](const ParsedChild &Record) -> const ParsedChild * {
			const ::ParsedTree::NodeRelations &Relations = *SrcTree.Syntax.Relations;
			const uint32_t Slot = Relations.Parents.Records[Record.Relation].FirstNamedSlot;
			// 共通カーソルが記録した最初の名前付の子，又は名前付の子の無い事の返戻
			return Slot == UINT32_MAX ? nullptr : &Relations.Children[Slot];
		};
		// 型引数の誤読条件の判定
		const auto IsMisparsed = [&](
			const ParsedChild &List,
			const ParsedChild &Template,
			const ParsedChild *const After,
			const ParsedChild *const Outer,
			const bool IsExpr
		) -> bool {
			// 閉じの後に無名のトークンが続く型引数の判定
			if(After && !After->IsNamed) {
				// 型引数の閉じへ割った `>>`・`>=` 等は元のトークンを保つ（正当なネストの `>>` は二項式の外の為に除く）
				// トークンを割った誤解析である事の返戻
				if(
					Template.End == After->GetStart() && (HasRole(After, Role::AngleCloseToken) || HasRole(After, Role::AssignmentToken)) &&
					(HasRole(Outer, Role::BinaryOperation) || HasRole(Outer, Role::AssignmentOperation))
				) return true;
				// 閉じの後に `::` が続く型引数は，比較の被演算子に為れないトークンが続く為に正当なテンプレートである事の返戻
				if(HasRole(After, Role::DoubleColonToken)) return false;
			}
			// 空の実引数が閉じの後に続く型引数を正当とする事の返戻
			if(HasRole(After, Role::ArgSeq) && !FirstNamedChild(*After)) return false;
			// 初期化子側の二項式を誤解析とする事の返戻（宣言子側の実体化は正当なテンプレートとして除く）
			return HasRole(FirstNamedChild(List), Role::BinaryOperation) && IsExpr;
		};
		// 式の側の正当な型引数リストで，外すと最初の型引数が二項式に為り比較と読まれる括弧の記録（整形仕様 5.4）
		const auto KeepLeadParen = [&](const ParsedChild &List, const bool IsExpr) -> void {
			const ParsedChild *const Lead = FirstNamedChild(List);
			// 式以外の型引数では括弧の保持を判定しない
			if(!IsExpr) return;
			const std::unordered_map<const void *, uint8_t>::const_iterator Known = Memo.find(List.Node.id);
			if(
				(Known == Memo.end() || !Known->second) && HasRole(Lead, Role::GroupingParen) &&
				HasRole(FirstNamedChild(*Lead), Role::BinaryOperation)
			) Memo[Lead->Node.id] = KeptLeadParen;
			// 終了
			return;
		};
		// 実体化のノードの最初の型引数リスト（無ければ空）
		const auto ListOf = [&](const ParsedChild &Template) -> const ParsedChild * {
			for(const ParsedChild &Child : SrcTree.GetChildrenAt(Template.Relation)) {
				// 最初の名前付の型引数リストの返戻
				if(Child.IsNamed && HasRole(&Child, Role::AngleBracketList)) return &Child;
			}
			// 型引数リストの無い事の返戻
			return nullptr;
		};
		// 巡るノード・其の後に続くトークン・親の型・其の中が式の側か
		struct Frame {
			const ParsedChild *Node; // 対象ノードの記録
			const ParsedChild *After; // 直後のノードの記録
			const ParsedChild *Outer; // 対象を包むノードの記録
			bool IsExpr; // 式側の文脈か
		};
		// 深さ３乗を避ける構文木の単一先行順走査
		// 構文木の根ノード
		std::vector<Frame> Frames { { &SrcTree.GetNodeAt(0), nullptr, nullptr, true } };
		std::vector<const ParsedChild *> Kids;
		while(!Frames.empty()) {
			// 現在処理する走査枠
			const Frame Top = Frames.back();
			// 構文木走査の作業列からの末尾要素取出
			Frames.pop_back();
			// 走査用のコメントでない子ノード列（直後のトークンの判定を文中のコメントの有無で変えない，整形仕様 1.2）
			Kids.clear();
			for(const ParsedChild &Child : SrcTree.GetChildrenAt(Top.Node->Relation)) if(!HasRole(&Child, Role::Comment)) {
				Kids.push_back(&Child);
			}
			// 子の文脈：初期化子を持つ宣言子の初期化子は式の側
			const bool IsInitializedDeclarator = HasRole(Top.Node, Role::InitDeclarator);
			const TSNode Val = IsInitializedDeclarator ? SrcTree.GetFieldChild(Top.Node->Node, ChildRole::Val) : TSNode{};
			const bool IsHost = HasRole(Top.Node, Role::MisparsedTemplateHost);
			for(size_t Idx = 0; Idx < Kids.size(); ++Idx) {
				// 走査中の子ノード
				const ParsedChild *const Child = Kids[Idx], *const After = Idx + 1 < Kids.size() ? Kids[Idx + 1] : nullptr;
				const bool IsExpr = IsInitializedDeclarator ?
				ts_node_eq(Child->Node, Val) :
				!HasRole(Top.Node, Role::TypedDecl) && (Top.IsExpr || HasRole(Top.Node, Role::TemplateScanBoundary));
				if(HasRole(Child, Role::MisparsedTemplateHost)) {
					// 実体化のノードと其の型引数リストは同一判定
					if(const ParsedChild *const List = ListOf(*Child)) {
						// ノード別の判定控えの確定
						if(IsMisparsed(*List, *Child, After, Top.Node, IsExpr)) Memo[Child->Node.id] = Memo[List->Node.id] = MisreadTemplate;
						else {
							Memo.erase(Child->Node.id);
							Memo.erase(List->Node.id);
						}
						KeepLeadParen(*List, IsExpr);
					}
				} else if(!IsHost && HasRole(Child, Role::AngleBracketList)) {
					if(IsMisparsed(*Child, *Top.Node, Top.After, Top.Outer, IsExpr)) Memo[Child->Node.id] = MisreadTemplate;
					else Memo.erase(Child->Node.id);
					KeepLeadParen(*Child, IsExpr);
				}
				if(!SrcTree.GetChildrenAt(Child->Relation).empty()) Frames.push_back({ Child, After, Top.Node, IsExpr });
			}
		}
		// 誤解析の判定の返戻
		return Memo;
	}

	/**
	 * プリプロセッサディレクティブ判定関数
	 * 計算量：判定位置以後のバイト数 L に対し O(L)
	 * @param Raw 判定対象テキスト
	 * @param Pos 判定開始位置
	 * @return `#` の直後（空白を挟んでも良い）にディレクティブ名が有れば true
	 */
	bool SourceProjector::IsPreprocDirectiveAt(const std::string_view Raw, const size_t Pos) {
		// `#` で始まらない事の返戻
		if(Pos >= Raw.size() || Raw[Pos] != '#') return false;
		// 対象範囲の開始位置
		size_t Start = Pos + 1;
		while(Start < Raw.size() && (Raw[Start] == ' ' || Raw[Start] == '\t')) ++Start;
		// ディレクティブ名は処理系の拡張（`#import` / `#include_next` / `#ident` 等）も有る為，名前の種類を限らない事の返戻
		return Start < Raw.size() && std::isalpha(static_cast<unsigned char>(Raw[Start]));
	}

	/**
	 * 子の所有経路の受渡関数
	 * 計算量：償却 O(1)
	 * @param Profile 子の入力で確定した文法（共有定義の寿命は全工程より長い）
	 * @param Code 子のコードだけの配置
	 * @param Syntax 子の正規化後の期待構文
	 * @param Comments 子の同期・正規化済の所有記録
	 * @param Ctx 入力ビューの所有者を持つ子の検証データ
	 * @param Ordinal 親ドキュメントの埋込の順
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	void OwnedInputRoutes::Add(
		const GrammarProfile &Profile,
		std::shared_ptr<const LaidOutCode> Code,
		std::shared_ptr<OutputExpectation> Syntax,
		std::shared_ptr<const CommentRepository> Comments,
		std::shared_ptr<ValidationContext> Ctx,
		const size_t Ordinal
	) {
		// ３所有経路の受取
		Children.push_back({ Ordinal, &Profile, std::move(Code), std::move(Syntax), std::move(Comments), std::move(Ctx) });
		// 終了
		return;
	}

	/**
	 * トークンにしない隠れたテキストを持つ間隙かの判定関数（整形仕様 4.4）
	 * 隠れたトークンの内，文を終える区切記号（文境界）・行継続の記号と文法のトークン（トークンのノードにする物）を除いたテキスト（文字列の引用符等）を持つかを見る
	 * 計算量：間隙の分割費用 T，トークンの総バイト数 B，文法の記法・語の判定の総費用 Q に対し O(T + B + Q)
	 * @param SrcTree コメントを分離した解析結果
	 * @param Host 間隙を子の間に持つノード
	 * @param From 間隙の開始位置
	 * @param To 間隙の終了位置
	 * @param Gaps 間隙から除くコメントの範囲と解析用の括弧の位置
	 * @return 原文の間隙として保つテキストを持てば true
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	bool SourceProjector::HasOpaqueHidden(
		const ::ParsedTree &SrcTree,
		const TSNode Host,
		const uint32_t From,
		const uint32_t To,
		GapExclusions &Gaps
	) {
		// トークンにしない隠れたテキストを持つ間隙かの判定
		const std::span<const std::pair<uint32_t, uint32_t>> Pieces = HiddenTokens(SrcTree, From, To, Gaps);
		// 文境界でも文法のトークンでもないテキストの有無の返戻
		return std::any_of(
			Pieces.begin(),
			Pieces.end(),
			[&](const std::pair<uint32_t, uint32_t> &Range) -> bool {
				const std::string_view Text = std::string_view(SrcTree).substr(Range.first, Range.second - Range.first);
				// 文境界でもトークンでもないテキストかの返戻
				return !SrcTree.GetProfile().IsSpelling(Text, Word::StmtEnd) &&
				!SrcTree.GetProfile().IsSpelling(Text, Word::LineContinuation) && !SrcTree.GetProfile().GetTokenSymbol(Text) &&
				!GetHiddenLexemeKind(SrcTree, Host);
			}
		);
	}

	/**
	 * 山括弧の読みの控えの取得関数
	 * 計算量：平均 O(1)
	 * @param Misreads `PrepareTemplateContexts` が控えた判定
	 * @param Node 実体化のノード，型引数リスト又は最初の型引数の括弧
	 * @return 比較を型引数と誤読した実体化・型引数リストは `MisreadTemplate`，除くと比較に為る括弧は `KeptLeadParen`，其の他は０
	 */
	uint8_t SourceProjector::GetTemplateReading(const std::unordered_map<const void *, uint8_t> &Misreads, const TSNode Node) {
		const std::unordered_map<const void *, uint8_t>::const_iterator Hit = Misreads.find(Node.id);
		// 控えた読み（控えの無いノードは０）の返戻
		return Hit != Misreads.end() ? Hit->second : 0;
	}

	/**
	 * 一覧の要素へ分けて誤読したジェネリック呼出の型引数の開きの判定関数
	 * パーサーは型引数に値と読める型（`typeof import("x")` 等）を含む呼出を，`f < 型` と `型 > (実引数)` の二項式の要素へ分ける
	 * 子はコメントを除いて数え，文中のコメントの有無で判定を変えない（整形仕様 1.2）
	 * 計算量：取得する子の総数 C と解析木の取得・判定の総費用 Q に対し O(C + Q)
	 * @param SrcTree 解析結果
	 * @param Node 判定するノード
	 * @return 後続の要素が誤読したジェネリック呼出の閉じである `<` の二項式なら true
	 * @throws std::bad_alloc 子の並びの確保に失敗した場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	bool SourceProjector::IsMisparsedGenericOpener(const ::ParsedTree &SrcTree, const TSNode Node) {
		// 型引数を持たない言語と，一覧の要素の二項式でないノードの返戻
		if(
			ts_node_is_null(Node) || !SrcTree.GetProfile().GetProjectionChecks(SrcTree.GetSymbol(Node)).CanReadGenericCall ||
			!SrcTree.Is(ts_node_parent(Node), Role::TypeArgLookalikeSlot)
		) return false;
		std::array<TSNode, 3> Parts;
		TSNode Later = ts_node_next_named_sibling(Node);
		while(!ts_node_is_null(Later) && SrcTree.Is(Later, Role::Comment)) Later = ts_node_next_named_sibling(Later);
		// 開きの山括弧の三つ組で，後続の要素が一覧へ分けた形（左に開きを持たない閉じ）の誤読のジェネリック呼出である事の返戻
		return IsCodeTripleReadSuccessful(SrcTree, Node, Parts) && SrcTree.Is(Parts[1], Role::LessThanToken) &&
		IsMisparsedGenericCall(SrcTree, Later) && !SrcTree.Is(ts_node_named_child(Later, 0), Role::BinaryOperation);
	}

	/**
	 * C# の宣言式と誤読した乗算かの判定関数
	 * C# パーサーが実引数の乗算を宣言式と誤読したかを判定する
	 * 計算量：組の祖先の段数 H と解析木の取得・判定の総費用 Q に対し O(H + Q)
	 * @param SrcTree ソースコード
	 * @param PtrType 宣言式の型に読まれたポインタ型
	 * @return 乗算と読む位置なら true
	 */
	bool SourceProjector::IsCSharpProduct(const ::ParsedTree &SrcTree, const TSNode PtrType) {
		// 宣言式のノード
		const TSNode Decl = ts_node_parent(PtrType), Arg = ts_node_parent(Decl);
		// 実引数でない宣言式と `out` の宣言式を，乗算へ読み替えない事の返戻
		if(
			!SrcTree.Is(Decl, Role::DeclExpr) || !SrcTree.Is(Arg, Role::Arg) ||
			HasUnnamedTokenChild(SrcTree, Arg, SrcTree.GetRules().Words[static_cast<size_t>(Word::OutputArg)].front())
		) return false;
		// タプルのネストを外側へ遡り，分解代入の左辺と `foreach` の変数は宣言として保つ
		TSNode Tuple = ts_node_parent(Arg);
		while(SrcTree.Is(Tuple, Role::TupleExpr)) {
			// 対象を保持するノード
			const TSNode Host = ts_node_parent(Tuple);
			if(SrcTree.Is(Host, Role::Arg)) {
				// タプル候補のノードの確定
				Tuple = ts_node_parent(Host);
				continue;
			}
			// 分解代入の左辺又は `foreach` の変数でなければ，乗算とする事の返戻
			return !(
				SrcTree.Is(Host, Role::Assignment) && ts_node_eq(SrcTree.GetFieldChild(Host, ChildRole::Left), Tuple) ||
				SrcTree.Is(Host, Role::ForeachStmt)
			);
		}
		// 呼出の実引数は乗算の返戻
		return true;
	}

	/**
	 * コメントを除いた間隙の取得関数（整形仕様 4.4）
	 * 間隙からコメントを除き，空白の区間毎に改行を数える（コメント前後に改行が在れば１つを重複分として除く）
	 * 改行が無い場合は，最後のコメントの後にテキストが在れば空白１つ，無ければ空文字列を返戻する
	 * 計算量：間隙のバイト数 B，コメント数 C，分離記録の判定とテキストの取得の総費用 Q に対し O(B + C + log(2 + C) + Q)
	 * @param SrcTree コメントを分離した解析結果
	 * @param Separated 投影の間だけ持つコメントの分離の結果
	 * @param From 間隙の開始位置
	 * @param To 間隙の終了位置
	 * @param CommentRanges 原文のコメントの範囲（原文順）
	 * @return コメントを除いて畳んだ間隙（未分離のコメントを含まなければ `std::nullopt`）
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	std::optional<std::string> SourceProjector::CommentFreeGap(
		const ::ParsedTree &SrcTree,
		const CommentSeparation &Separated,
		const uint32_t From,
		const uint32_t To,
		const std::vector<std::pair<uint32_t, uint32_t>> &CommentRanges
	) {
		// 未分離のコメントの最初の位置
		std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Comment =
		std::lower_bound(CommentRanges.begin(), CommentRanges.end(), std::pair<uint32_t, uint32_t>(From, 0));
		while(Comment != CommentRanges.end() && Comment->first < To && Separated.IsSeparatedAt(Comment->first)) ++Comment;
		// コメントを除く必要の無い間隙の返戻
		if(!(From < To) || Comment == CommentRanges.end() || !(Comment->first < To)) return std::nullopt;
		// コメントで分けた空白の区間毎の改行の数と，最後の区間が空でないか
		uint32_t Newlines = 0, Prev = 0;
		bool HasSpace = false;
		uint32_t Cursor = From;
		while(Cursor < To) {
			const uint32_t End = Comment != CommentRanges.end() && Comment->first < To ? Comment->first : To;
			const std::string Space = Separated.BuildSeparatedText(SrcTree, Cursor, End);
			const uint32_t Count = static_cast<uint32_t>(std::count(Space.begin(), Space.end(), '\n'));
			// 独立コメントの前後の改行は１つと数える
			Newlines += Count - (Prev && Count ? 1 : 0);
			Prev = Count;
			HasSpace = !Space.empty();
			if(End == To) break;
			Cursor = std::min(To, Comment->second);
			++Comment;
			// アンカーへ紐付けたコメントは分離の記録で除き，テキストを保つ領域に残るコメントだけで区切る
			while(Comment != CommentRanges.end() && Comment->first < To && Separated.IsSeparatedAt(Comment->first)) ++Comment;
		}
		// 改行か最後のコメントの後の空白へ畳んだ間隙の返戻
		return Newlines ? std::string(Newlines, '\n') : HasSpace ? std::string(" ") : std::string();
	}

	/**
	 * 原文の隣接が読みを決める境界かの判定関数（整形仕様 4.2，トークン間空白仕様 4.2）
	 * 空白の有無で演算子の役割・実引数の読み・アノテーションの適用先・値のトークンが変わる境界だけを記録し，其の他の原文の空白は捨てる
	 * 空白の追加が意味を変える記法はトークンの間の空白の有無が値の区切・子孫結合子を成す為，全ての子の境界を記録する（区切トークンの周りの空白はトークン間空白の規則が決める）
	 * @param Syntax 構文モデル
	 * @param Parent 親のノード
	 * @param Next 境界の後の子
	 * @return 原文の隣接を構文事実として記録するなら true
	 */
	bool SourceProjector::ShouldPreserveAdjacency(const SyntaxModel &Syntax, const NodeId Parent, const NodeId Next) {
		// 隣接が意味を持つ境界かの返戻
		return Syntax.GetRules().IsWhitespaceSemantic || Syntax.Is(Parent, Role::AdjacencySensitiveInfix) ||
		Syntax.Is(Parent, Role::AdjacencyRecordingHost) || Syntax.Is(Next, Role::AdjacencyRecordingOperand);
	}

	/**
	 * Go の短い宣言が同じ有効範囲の既存変数へ代入するかの記録関数
	 * 文の並びに導入済の名前と関数本体の仮引数を追い，確認出来ない位置は再代入の可能性を残す
	 * 計算量：ノード表の大きさ N，仮引数・宣言の総訪問数 V，名前のインデックス・コピーの総バイト数 B に対し平均 O(N + V + B)
	 * @param Syntax 投影済の構文モデル
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SourceProjector::RecordGoBindingFacts(SyntaxModel &Syntax) {
		// 文の並び毎の束縛追跡
		for(NodeId List = 0; List < Syntax.Nodes.size(); ++List) {
			if(!Syntax.Is(List, Role::PlainStmtSeq)) continue;
			std::unordered_set<std::string> BoundNames;
			// パターンが導入する名前の登録
			const auto AddNames = [&Syntax, &BoundNames](const NodeId Parent) -> void {
				for(const NodeId Name : Syntax.GetChildrenOf(Parent)) {
					if(!Syntax.Is(Name, Role::Identifier)) continue;
					if(const SyntaxNode &Named = Syntax.GetNode(Name); !Named.Facts.DoesDiscardBinding) BoundNames.insert(Named.Lexeme);
				}
				// 終了
				return;
			};
			// 関数の仮引数の束縛
			if(
				const NodeId Block = Syntax.GetParentOf(List), Function = Syntax.GetParentOf(Block);
				Syntax.Is(Block, Role::LexicalBlock) && (
					Syntax.Is(Function, Role::FunctionDeclNode) || Syntax.Is(Function, Role::MethodDecl) ||
					Syntax.Is(Function, Role::FunctionLiteral)
				)
			) for(const NodeId Child : Syntax.GetChildrenOf(Function)) {
				if(!Syntax.Is(Child, Role::DeclaratorParams)) continue;
				Core::SyntaxModel::HasStoppedTraversal(
					Syntax,
					Child,
					[&Syntax, &AddNames](const NodeId Param) -> WalkStep {
						// 通常と可変長の仮引数の収集
						// 仮引数でないノードは更に辿る事の返戻
						if(!Syntax.Is(Param, Role::ParamDecl) && !Syntax.Is(Param, Role::VariadicParamDecl)) return WalkStep::Descend;
						AddNames(Param);
						// 仮引数の型の中へ降りない事の返戻
						return WalkStep::Skip;
					}
				);
			}
			// case 節の受信名と別名の束縛
			if(const NodeId Case = Syntax.GetParentOf(List); Syntax.Is(Case, Role::CommunicationCase)) {
				if(
					const NodeId Communication = Syntax.GetFieldChild(Case, ChildRole::Communication); Syntax.Is(
						Communication,
						Role::ReceiveStmt
					) && std::ranges::any_of(
						Syntax.GetChildrenOf(Communication),
						[&Syntax](const NodeId Child) -> bool {
							// 短い宣言の記号かの返戻
							return Syntax.Is(Child, Role::ShortAssignmentToken);
						}
					)
				) AddNames(Syntax.GetFieldChild(Communication, ChildRole::Left));
			} else if(
				(Syntax.Is(Case, Role::TypeCase) || Syntax.Is(Case, Role::DefaultCase)) && Syntax.Is(Syntax.GetParentOf(Case), Role::TypeSwitch)
			) AddNames(Syntax.GetFieldChild(Syntax.GetParentOf(Case), ChildRole::Alias));
			// 文を順に読んだ再代入の判定
			for(const NodeId Child : Syntax.GetChildrenOf(List)) {
				NodeId Stmt = Child;
				while(Syntax.Is(Stmt, Role::LabeledStmt)) {
					NodeId Following = NoNode;
					for(const NodeId Part : Syntax.GetChildrenOf(Stmt)) if(Syntax.IsNamed(Part) && !Syntax.Is(Part, Role::LabelName)) {
						Following = Part;
					}
					Stmt = Following;
				}
				if(Syntax.Is(Stmt, Role::ShortVarDecl)) {
					const NodeId Left = Syntax.GetFieldChild(Stmt, ChildRole::Left);
					bool HasName = false, IsKnown = Left != NoNode, HasReassignment = false;
					for(const NodeId Name : Syntax.GetChildrenOf(Left)) {
						if(!Syntax.IsNamed(Name)) continue;
						if(!Syntax.Is(Name, Role::Identifier)) {
							IsKnown = false;
							continue;
						}
						const SyntaxNode &Named = Syntax.GetNode(Name);
						if(Named.Facts.DoesDiscardBinding) continue;
						HasName = true;
						HasReassignment = HasReassignment || BoundNames.contains(Named.Lexeme);
					}
					Syntax.Nodes[Stmt].Facts.CanReassignExistingBinding = !IsKnown || !HasName || HasReassignment;
					AddNames(Left);
				} else if(Syntax.Is(Stmt, Role::VarDeclNode) || Syntax.Is(Stmt, Role::ConstantDeclNode)) {
					Core::SyntaxModel::HasStoppedTraversal(
						Syntax,
						Stmt,
						[&Syntax, &AddNames](const NodeId Spec) -> WalkStep {
							// 宣言子以外のノードは更に辿る事の返戻
							if(!Syntax.Is(Spec, Role::VarSpec) && !Syntax.Is(Spec, Role::ConstantSpec)) return WalkStep::Descend;
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
	 * 識別子の中の Unicode のエスケープのデコード関数
	 * `\u` と４桁（`u` の繰返を含む），`\u{…}`，`\U` と８桁を読む
	 * 計算量：エスケープの位置以後の名前のバイト数 L に対し O(L)
	 * @param Reading 入力の読取規則
	 * @param Name 名前のテキスト
	 * @param Pos エスケープの始まり（成功時はエスケープの後へ進む）
	 * @param Point デコードしたコードポイントの格納先
	 * @return 識別子の字として解釈出来るエスケープなら true
	 */
	bool SourceProjector::HasDecodedIdentifierEscape(
		const ReadingRules &Reading,
		const std::string_view Name,
		size_t &Pos,
		char32_t &Point
	) {
		// エスケープの記法と桁数の判定
		size_t At = Pos + 1;
		const bool IsShort = At < Name.size() && Name[At] == 'u';
		// エスケープのマーカーでない事の返戻
		if(!IsShort && (At >= Name.size() || Name[At] != 'U')) return false;
		while(At < Name.size() && Name[At] == Name[Pos + 1]) ++At;
		const bool IsBraced = IsShort && At < Name.size() && Name[At] == '{';
		const size_t First = At + IsBraced, Last = IsBraced ? Name.find('}', First) : First + (IsShort ? 4 : 8);
		// 桁数が確定したエスケープのデコードとコードポイントの検査
		// デコード出来ないエスケープ又は文法が指せない基本文字（`$` `@` `` ` `` を除く `U+00A0` 未満）の返戻
		if(
			const auto HasInvalidDigits = [&]() -> bool {
				// 整数の読取先を要求する数字列のデコード
				uint32_t Digits = static_cast<uint32_t>(Point);
				const std::from_chars_result Parsed = std::from_chars(Name.data() + First, Name.data() + Last, Digits, 16);
				Point = static_cast<char32_t>(Digits);
				// デコードの失敗又は Unicode のスカラー値でない事の返戻
				return Parsed.ec != std::errc{} || Parsed.ptr != Name.data() + Last || Point > 0X10FFFF || Point > 0XD7FF && Point < 0XE000;
			}; Last == std::string_view::npos || Last > Name.size() || Last == First || HasInvalidDigits() ||
			Reading.ShouldRestrictBasicIdentifierEscapes && Point < 0XA0 && Point != '$' && Point != '@' && Point != '`'
		) return false;
		Pos = Last + IsBraced;
		// デコード出来た事の返戻
		return true;
	}

	/**
	 * 識別子の書式文字の判定関数
	 * 計算量：固定の Unicode の範囲表の探索は O(1)
	 * @param Point コードポイント
	 * @return Unicode 17.0.0 の一般カテゴリ `Cf` なら true
	 */
	bool SourceProjector::IsFormatChar(const char32_t Point) {
		// 一般カテゴリ `Cf` の範囲（両端を含まない境界）
		static constexpr char32_t Ranges[][2] = {
			{ 0XAC, 0XAE },
			{ 0X5FF, 0X606 },
			{ 0X61B, 0X61D },
			{ 0X6DC, 0X6DE },
			{ 0X70E, 0X710 },
			{ 0X88F, 0X892 },
			{ 0X8E1, 0X8E3 },
			{ 0X180D, 0X180F },
			{ 0X200A, 0X2010 },
			{ 0X2029, 0X202F },
			{ 0X205F, 0X2065 },
			{ 0X2065, 0X2070 },
			{ 0XFEFE, 0XFF00 },
			{ 0XFFF8, 0XFFFC },
			{ 0X110BC, 0X110BE },
			{ 0X110CC, 0X110CE },
			{ 0X1342F, 0X13440 },
			{ 0X1BC9F, 0X1BCA4 },
			{ 0X1D172, 0X1D17B },
			{ 0XE0000, 0XE0002 },
			{ 0XE001F, 0XE0080 }
		};
		// 範囲に入る事の返戻
		for(const char32_t (&Range)[2] : Ranges) if(Point > Range[0] && Point < Range[1]) return true;
		// `Cf` でない事の返戻
		return false;
	}

	/**
	 * 空の左辺として誤読された数値の符号と被演算子の取得関数（整形仕様 4.3，トークン間空白仕様 5.3）
	 * 計算量：直接の子の数 C とコメントの分離記録の照会費用 Q に対し O(C + Q)
	 * @param SrcTree 解析結果
	 * @param Separated 投影の間だけ持つコメントの分離の結果
	 * @param Node パーサーのノード
	 * @return 符号と符号無の数値，該当しない場合は空のノードの組
	 */
	std::pair<TSNode, TSNode> SourceProjector::SignedDimensionParts(
		const ::ParsedTree &SrcTree,
		const CommentSeparation &Separated,
		const TSNode Node
	) {
		// 符号を離せる数値の読取規則と，空の左辺を持つ読違の形の選択
		// 符号付数値でない事の返戻
		if(!SrcTree.GetRules().HasSpacedDimensionSigns || !SrcTree.Is(Node, Role::BinaryOperation)) return {};
		// コメントを除いた直接の部品の取得
		std::array<TSNode, 3> Parts {};
		size_t Count = 0;
		for(uint32_t Idx = 0; Idx < SrcTree.GetChildCount(Node); ++Idx) {
			const TSNode Child = SrcTree.GetChild(Node, Idx);
			if(Separated.IsSeparatedComment(SrcTree, Child)) continue;
			// 符号式より部品が多い事の返戻
			if(Count == Parts.size()) return {};
			Parts[Count++] = Child;
		}
		// 幅零の左辺と符号無の数値の照合
		if(
			Count != Parts.size() || !SrcTree.Is(Parts[0], Role::DimensionNum) || SrcTree.GetLen(Parts[0]) ||
			!SrcTree.Is(Parts[2], Role::DimensionNum)
		// 数値だけの符号式でない事の返戻
		) return {};
		const std::string_view Sign = SrcTree.GetView(Parts[1]), Num = SrcTree.GetView(Parts[2]);
		// 連鎖した符号を１つの数のテキストへ融合しない，単独の符号と数値の返戻
		return (Sign == "+" || Sign == "-") && !Num.empty() && Num.front() != '+' && Num.front() != '-' ?
		std::pair<TSNode, TSNode>(Parts[1], Parts[2]) :
		std::pair<TSNode, TSNode>();
	}

	/**
	 * 子へ降りずテキストを保つノードかの判定関数（整形仕様 4.4，4.6）
	 * 値の内容・読めなかった領域・処理系で読みの分かれる本文等は，トークンへ分けずに１つの葉として保つ
	 * 計算量：候補の子の数 C と分離・マクロ・トークンの判定の総費用 Q に対し O(C + Q)
	 * @param SrcTree コメントを分離した解析結果
	 * @param Separated 投影の間だけ持つコメントの分離の結果
	 * @param Node パーサーのノード（子を持つ物）
	 * @param Roles 記号の表から取得した役割
	 * @param Children カーソルで１度取得した子とフィールド
	 * @param HasFilePreamble ファイル先頭のバイト列として読むか
	 * @param Observation 直接子の読取の観測用計数（無ければ計上しない）
	 * @return 子へ降りないノードなら true
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	bool SourceProjector::IsVerbatimNode(
		const ::ParsedTree &SrcTree,
		const CommentSeparation &Separated,
		const TSNode Node,
		const RoleSet &Roles,
		const std::span<const ParsedChild> Children,
		const bool HasFilePreamble,
		HiddenWork *const Observation
	) {
		// ノードの役割の有無
		const auto Has = [&Roles](const Role Wanted) -> bool {
			// 役割の有無の返戻
			return Roles.test(static_cast<size_t>(Wanted));
		};
		// 厳密な先頭記法に当たらない宣言は，引用符・区切の変更で有効化しない
		const ReadingRules &Reading = SrcTree.GetReading();
		if(HasFilePreamble && !Reading.EncodingBytePrefix.empty() && !Children.empty()) {
			const std::string_view Head = Reading.EncodingBytePrefix.substr(0, Reading.EncodingBytePrefix.find_first_of(" \t"));
			if(SrcTree.GetView(Children.front().Node) == Head) {
				const std::string_view Text = SrcTree.GetView(Node);
				if(
					SrcTree.GetStart(Node) || Text.size() > Reading.EncodingPrescanBytes || !Text.starts_with(Reading.EncodingBytePrefix) ||
					!Text.ends_with(Reading.EncodingByteSuffix)
				// 宣言の不成立を表すテキストを保つ事の返戻
				) return true;
			}
		}
		// 子へ降りずテキストを保つノードかの判定
		if(Has(Role::TypeAliasStmt)) {
			if(
				const TSNode Left = SrcTree.GetFieldChild(Node, ChildRole::Left);
				SrcTree.Is(ts_node_named_child(Left, 0), Role::Identifier) || SrcTree.Is(ts_node_named_child(Left, 0), Role::AppliedGenericType)
			// 型の別名の部品へ降りる事の返戻
			) return false;
		}
		// 補間内の引用符と表示対象のテキスト，及び子を持つ無名のノード（別名で包んだトークン）をトークンとして保つ事の返戻
		if(!SrcTree.IsNamed(Node)) return true;
		// ドキュメントの要素は本文とタグの境界を個別に投影し，マクロのトークン列は子へ投影して意味を持つ境界を保つ（整形仕様 4.3，4.4）
		// ドキュメントの要素又はマクロの構文を子へ投影する事の返戻
		if(Has(Role::MarkupContainer) || Has(Role::MacroRuleDef) || Has(Role::PreservedTokenSeq)) return false;
		// 補間の無いリテラルは，引用の変更前後で同じトークンの葉として投影する
		if(Has(Role::InterpolatedLiteral)) {
			std::vector<ParsedChild> Parts(Children.begin(), Children.end());
			bool HasCode = false;
			while(!Parts.empty()) {
				const ParsedChild Part = Parts.back();
				Parts.pop_back();
				if(Observation) ++Observation->ArgTextReads;
				const ProjectionChecks &Checks = SrcTree.GetProfile().GetProjectionChecks(Part.Symbol);
				if(Checks.IsLiteralBody) {
					const std::span<const ParsedChild> Nested = SrcTree.GetChildrenOf(Part.Node);
					Parts.insert(Parts.end(), Nested.begin(), Nested.end());
				} else if(Part.IsNamed && !Checks.IsLiteralText) {
					HasCode = true;
					break;
				}
			}
			// 本文だけの引用を単独のトークンとして保つ事の返戻
			if(!HasCode) return true;
		}
		// 埋込式と埋込式を持つ文字列は，葉の集合に在るが埋込式を正規化する為に降りる（整形仕様 4.4）
		const bool IsInterpolated = Has(Role::StrInterpolation) || Has(Role::InterpolatedLiteral) ||
		(Has(Role::SpaceSkip) || Has(Role::SrcLeaf) || Has(Role::VerbatimSubtree) || Has(Role::VerbatimQuotedStr)) && std::any_of(
			Children.begin(),
			Children.end(),
			[&SrcTree](const ParsedChild &Child) -> bool {
				// 埋込式の子かの返戻
				return SrcTree.GetProfile().GetKindOf(Child.Symbol).Roles.test(static_cast<size_t>(Role::StrInterpolation));
			}
		);
		// エラー領域は子のトークンと其の間隙を保つ境界として投影する為に降りる（整形仕様 4.6）
		// 補間を持たない引用文字列とテキストをコピーする種別の返戻
		if(
			Has(Role::VerbatimQuotedStr) && !IsInterpolated || Has(Role::SrcCopiedNode) && !Has(Role::Error) || Has(Role::DocTypeDecl) ||
			Has(Role::ImportLeaf) || Has(Role::ElementText) ||
			Has(Role::SpaceSkip) && !Has(Role::EqualsTight) && !Has(Role::EqualsSpaced) && !Has(Role::NormColon) && !IsInterpolated ||
			Has(Role::SrcLeaf) && (SrcTree.GetReading().HasOuterText || !Has(Role::QualifiedNameNode)) && !Has(Role::AngleBracketList) &&
			!IsInterpolated
		) return true;
		// プリプロセッサディレクティブはディレクティブの語・名前・仮引数・本体のトークンを構文として投影し，解析の為にマーカーへ置き換えた連結指定は１つの葉としてデコードする事（整形仕様 4.2，トークン間空白仕様 5.4）の返戻
		if(Has(Role::Preproc)) return ParseAdapter::IsLinkageGuard(SrcTree, Node);
		// 整形出来ない構文とテキストが値に為る領域の返戻
		if(
			Has(Role::VerbatimSubtree) && !Has(Role::TemplateInstantiationType) && !IsInterpolated ||
			Has(Role::SpacingSensitiveInfix) && HasLopsidedOperator(SrcTree, Children) ||
			SrcTree.GetRules().IsWhitespaceSemantic && SrcTree.IsCssUrlArgs(Node)
		) return true;
		TSNode Only {};
		uint32_t ChildCount = 0;
		for(const ParsedChild &Child : Children) {
			if(
				SrcTree.GetProfile().GetKindOf(Child.Symbol).Roles.test(static_cast<size_t>(Role::Comment)) &&
				Separated.IsSeparatedComment(SrcTree, Child.Node) && !Separated.IsNotationSeparated(Child.Node)
			) continue;
			if(!ChildCount) Only = Child.Node;
			++ChildCount;
		}
		// 空白が意味を持つ文法の単位付の数値で，子が複数の物の返戻
		if(SrcTree.GetRules().IsWhitespaceSemantic && ChildCount > 1 && Has(Role::DimensionNum)) return true;
		// 子の外に空白以外のテキストを持つ唯一の子（パーサーがトークンをノードへ出さなかった形）の返戻（根は子の並びを組む為に除く）
		if(ChildCount == 1 && !IsTreeRoot(Node)) {
			const std::string Before = Separated.BuildSeparatedText(SrcTree, SrcTree.GetStart(Node), SrcTree.GetStart(Only));
			const std::string After = Separated.BuildSeparatedText(SrcTree, SrcTree.GetEnd(Only), SrcTree.GetEnd(Node));
			// 空白・文を終える隠れた区切記号（文境界でトークンにしない）・行継続の記号以外のテキストを持つか
			const auto HasToken = [&SrcTree](std::string_view Text) -> bool {
				const std::string_view Terminator = SrcTree.GetRules().GetSpelling(Word::StmtEnd);
				const std::string_view Continuation = SrcTree.GetRules().GetSpelling(Word::LineContinuation);
				while(!Text.empty()) {
					if(std::string_view(" \t\r\n").find(Text.front()) != std::string_view::npos) Text.remove_prefix(1);
					else if(!Terminator.empty() && Text.starts_with(Terminator)) Text.remove_prefix(Terminator.size());
					else if(!Continuation.empty() && Text.starts_with(Continuation)) Text.remove_prefix(Continuation.size());
					// トークンのテキストを持つ事の返戻
					else return true;
				}
				// トークンのテキストを持たない事の返戻
				return false;
			};
			// 子の外に文境界以外のトークンを持つ唯一の子かの返戻
			return !Has(Role::MarkupElementNode) && (HasToken(Before) || HasToken(After));
		}
		// 子へ降りるノードの返戻
		return false;
	}

	/**
	 * テキストを保つノードのテキストの取得関数（整形仕様 4.4）
	 * 解析の為にマーカーへ置き換えた連結指定は其のトークンだけ，CSS の `url()` は引数の参照先のテキストを保つ
	 * 計算量：テキストのバイト数 B と分離・デコード・解析結果の取得の総費用 Q に対し O(B + Q)
	 * @param SrcTree コメントを分離した解析結果
	 * @param Separated 投影の間だけ持つコメントの分離の結果
	 * @param Node パーサーのノード
	 * @return 葉のテキスト
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	std::string SourceProjector::GetVerbatimText(
		const ::ParsedTree &SrcTree,
		const CommentSeparation &Separated,
		const TSNode Node
	) {
		// テキストを保つノードのテキストの取得
		if(ParseAdapter::IsLinkageGuard(SrcTree, Node)) {
			// 解析の為にマーカーへ置き換えた連結指定のトークン（ディレクティブを終える改行を含めない）の返戻
			return Separated.GetDecodedText(
				SrcTree,
				SrcTree.GetStart(Node),
				SrcTree.GetEnd(SrcTree.GetChild(Node, SrcTree.GetChildCount(Node) - 1))
			);
		}
		if(SrcTree.GetRules().IsWhitespaceSemantic && SrcTree.GetChildCount(Node) && SrcTree.IsCssUrlArgs(Node)) {
			// URL の引数のテキストの返戻
			return GetCssUrlArgsText(ParseAdapter::DecodeLexeme(SrcTree, std::string(SrcTree.GetView(Node)), SrcTree.GetStart(Node)));
		}
		// アンカーへ紐付けたコメントを除き，断片毎にデコードした原文のテキストの返戻
		return Separated.GetDecodedText(SrcTree, SrcTree.GetStart(Node), SrcTree.GetEnd(Node));
	}

	/**
	 * トークンのノードにする隠れたテキストかの判定関数（整形仕様 4.4）
	 * 計算量：テキストのバイト数 B と文法の記法・語の種別の取得費用 Q に対し O(B + Q)
	 * @param SrcTree コメントを分離した解析結果
	 * @param Host テキストを子の間に持つノード
	 * @param Text 隠れたテキスト
	 * @return 文を終える区切記号（文境界）でも行継続の記号でもない，文法のトークン又は文法が子に出さない語なら true
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	bool SourceProjector::IsTokenizedHidden(const ::ParsedTree &SrcTree, const TSNode Host, const std::string_view Text) {
		// トークンのノードにする隠れたテキストかの判定
		// 文境界でない文法のトークンか文法が子に出さない語かの返戻
		return !SrcTree.GetProfile().IsSpelling(Text, Word::StmtEnd) &&
		!SrcTree.GetProfile().IsSpelling(Text, Word::LineContinuation) &&
		(SrcTree.GetProfile().GetTokenSymbol(Text) || GetHiddenLexemeKind(SrcTree, Host));
	}

	/**
	 * 呼出又は其れへの代入と読まれた括弧付宣言子の投影の修復関数
	 * 基本型又は宣言済の型を用い，文全体が宣言子と任意の初期化子だけに為る形を確定する
	 * 計算量：文法の記号数 G，ノード数 N，修復する宣言子の総ノード数 D，コメント数 C，名前の照合・有効範囲の照会の総費用 Q に対し期待値 O(G + N + D + C + Q)
	 * @param SrcTree 型名と値の宣言をインデックス化した解析結果
	 * @param Profile 入力で確定した文法
	 * @param ValidationNodes 入力の解析ノード
	 * @param ExtraNodes 補った解析ノード
	 * @param IsVerification 完成候補の投影で，整形用の生成形式と空行比較キーを記録しないか
	 * @param Sources 各ノードの原文の出所
	 * @param Result 投影した構文とコメント
	 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
	 * @throws std::bad_alloc 宣言子列又はアンカーの移譲先の確保に失敗した場合
	 * @throws std::out_of_range 確定した宣言のノードが投影に無い場合（内部の不変条件の破壊）
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	void SourceProjector::RebindParenthesizedDecls(
		const ::ParsedTree &SrcTree,
		const GrammarProfile &Profile,
		const ParsedNodes ValidationNodes,
		const std::span<const TSNode> ExtraNodes,
		const bool IsVerification,
		std::vector<Origin> &Sources,
		ProjectionResult &Result
	) {
		// 修復を要する宣言を持たない入力の終了
		if(SrcTree.ReboundDecls.empty()) return;
		SyntaxModel &Syntax = Result.Syntax;
		std::unordered_map<const void *, NodeId> Projected;
		for(NodeId Id = 0; Id < Sources.size(); ++Id) {
			Syntax.Charge(1);
			if(const TSNode Src = SrcNode(Sources[Id], ValidationNodes, ExtraNodes); !ts_node_is_null(Src)) Projected.emplace(Src.id, Id);
		}
		std::unordered_map<NodeId, NodeId> Anchors;
		// 原文のノードに対応する識別子の取得
		const auto NodeIdentifierOf = [&](const TSNode Node) -> NodeId {
			// 投影したノードの識別子の返戻
			return Projected.at(Node.id);
		};
		// 投影したノードの構造種別の変更
		const auto Retype = [&](const NodeId Id, const Role Shape) -> void {
			Syntax.Charge(1);
			const TSSymbol Symbol = Profile.FindSymbolWithRole(Shape);
			SyntaxNode &Node = Syntax.Nodes[Id];
			Node.Kind = { &Profile.GetKindOf(Symbol) };
			if(!IsVerification) {
				Node.Facts.Forms = Profile.GetFormsOf(Symbol);
				Node.Facts.BlankKey = Profile.GetBlankKeyOf(Symbol);
			}
			// 終了
			return;
		};
		// ノードの切離と移譲先の記録
		const auto Detach = [&](const NodeId Id, const NodeId Dst) -> void {
			Syntax.Nodes[Id].Children.clear();
			Syntax.Nodes[Id].Parent = NoNode;
			Syntax.IsRemoved[Id] = 1;
			Anchors.emplace(Id, Dst);
			// 終了
			return;
		};
		const uint64_t DimensionField = Profile.GetFieldBits(ChildRole::SizeField);
		for(const ::ParsedTree::ParenthesizedDecl &Original : SrcTree.ReboundDecls) {
			const std::unordered_map<const void *, NodeId>::const_iterator Found = Projected.find(Original.Stmt.id);
			if(Found == Projected.end() || !Syntax.Is(Found->second, Role::ExprStmt)) continue;
			const NodeId Stmt = Found->second, Type = NodeIdentifierOf(Original.Type);
			NodeId Call = NoNode, Args = NoNode;
			uint32_t DeclaratorStart;
			Retype(Stmt, Role::LeadingConstDecl);
			if(Syntax.Is(Type, Role::IdentifierLeafLike)) Retype(Type, Role::TypeName);
			Syntax.Nodes[Type].Fields = Syntax.GetFieldBit(ChildRole::Type);
			if(ts_node_is_null(Original.Call)) {
				// 型を二項式から外し，残る演算子と右辺を宣言子へ投影
				const NodeId Root = NodeIdentifierOf(Original.Items.front().Root);
				Syntax.Nodes[Root].Children.erase(Syntax.Nodes[Root].Children.begin());
				DeclaratorStart = Sources[Syntax.Nodes[Root].Children.front()].Start;
			} else {
				// 型の呼出だけを外し，外側の関数・配列の宣言子へ括弧付宣言子を結ぶ
				Call = NodeIdentifierOf(Original.Call);
				Args = NodeIdentifierOf(Original.Args);
				SyntaxNode &Parent = Syntax.Nodes[Syntax.Nodes[Call].Parent];
				Parent.Children[Syntax.Nodes[Call].Idx] = Args;
				Syntax.Nodes[Args].Parent = Syntax.Nodes[Call].Parent;
				Syntax.Nodes[Args].Idx = Syntax.Nodes[Call].Idx;
				DeclaratorStart = Sources[Args].Start;
			}
			std::pmr::vector<NodeId> Children({ Type }, Syntax.Nodes[Stmt].Children.get_allocator());
			for(size_t Idx = 0; Idx < Original.Items.size(); ++Idx) {
				const ::ParsedTree::DeclaratorItem &Item = Original.Items[Idx];
				const NodeId Root = NodeIdentifierOf(Item.Root);
				for(const ::ParsedTree::DeclaratorPart &Part : Item.Parts) {
					const NodeId Id = NodeIdentifierOf(Part.Node);
					Retype(Id, Part.Shape);
					if(!Idx && Sources[Id].Start < DeclaratorStart) Sources[Id].Start = DeclaratorStart;
					SyntaxNode &Node = Syntax.Nodes[Id];
					const NodeId Indices = Part.Shape == Role::ArrayDeclarator ? Syntax.GetNamedChild(Id, 1) : NoNode;
					if(Syntax.Is(Indices, Role::ArgSeq)) {
						Node.Children.pop_back();
						const std::span<const NodeId> Brackets = Syntax.Nodes[Indices].Children;
						Node.Children.insert(Node.Children.end(), Brackets.begin(), Brackets.end());
						Detach(Indices, Id);
					}
					NodeId First = NoNode;
					for(uint32_t ChildIdx = 0; ChildIdx < Node.Children.size(); ++ChildIdx) {
						Syntax.Charge(1);
						const NodeId Child = Node.Children[ChildIdx];
						SyntaxNode &Val = Syntax.Nodes[Child];
						Val.Parent = Id;
						Val.Idx = ChildIdx;
						Val.Fields = 0;
						if(Syntax.IsNamed(Child)) {
							if(First == NoNode) First = Child;
							else if(Part.Shape == Role::FunctionDeclarator) Val.Fields = Syntax.GetFieldBit(ChildRole::Params);
							else if(Part.Shape == Role::ArrayDeclarator) Val.Fields = DimensionField;
						}
					}
					// 括弧付・参照の宣言子には宣言子のフィールドが無い（ポインタ・関数・配列の宣言子には在る）
					if(Part.Shape == Role::PtrDeclarator || Part.Shape == Role::FunctionDeclarator || Part.Shape == Role::ArrayDeclarator) {
						Syntax.Nodes[First].Fields = Syntax.GetFieldBit(ChildRole::Declarator);
					}
				}
				Syntax.Nodes[Root].Fields = Syntax.GetFieldBit(ChildRole::Declarator);
				if(!ts_node_is_null(Item.Val)) {
					const NodeId Initializer = NodeIdentifierOf(Item.Expr), Val = NodeIdentifierOf(Item.Val);
					Retype(Initializer, Role::InitDeclarator);
					Syntax.Nodes[Initializer].Fields = Syntax.GetFieldBit(ChildRole::Declarator);
					for(const NodeId Child : Syntax.GetChildrenOf(Initializer)) {
						Syntax.Charge(1);
						Syntax.Nodes[Child].Fields =
						Child == Root ? Syntax.GetFieldBit(ChildRole::Declarator) : Child == Val ? Syntax.GetFieldBit(ChildRole::Val) : 0;
					}
					Sources[Initializer].Start = Sources[Root].Start;
					Children.push_back(Initializer);
				} else Children.push_back(Root);
				if(Idx < Original.Commas.size()) {
					const NodeId Comma = NodeIdentifierOf(Original.Commas[Idx]);
					Syntax.Nodes[Comma].Fields = 0;
					Children.push_back(Comma);
				}
			}
			for(const NodeId Child : Syntax.GetChildrenOf(Stmt)) if(!Syntax.IsNamed(Child)) Children.push_back(Child);
			Syntax.Nodes[Stmt].Children = std::move(Children);
			for(uint32_t Idx = 0; Idx < Syntax.Nodes[Stmt].Children.size(); ++Idx) {
				Syntax.Charge(1);
				SyntaxNode &Child = Syntax.Nodes[Syntax.Nodes[Stmt].Children[Idx]];
				Child.Parent = Stmt;
				Child.Idx = Idx;
			}
			if(Call != NoNode) Detach(Call, Args);
			for(const TSNode Wrapper : Original.Wrappers) Detach(NodeIdentifierOf(Wrapper), Stmt);
		}
		for(CommentRecord &Comment : Result.Comments.EditRecords()) {
			if(const std::unordered_map<NodeId, NodeId>::const_iterator Dst = Anchors.find(Comment.Anchor); Dst != Anchors.end()) {
				Comment.Anchor = Dst->second;
			}
		}
		// 終了
		return;
	}

	/**
	 * 外部のノード列からの検証データの組立関数
	 * 計算量：データの組立費用 Q に対し O(Q)
	 * @param SrcTree 受理した解析結果
	 * @param Profile 入力時に確定した文法
	 * @param Input 適応済の入力と修復前のテキスト
	 * @param Nodes 原文順のノード列（呼出の間は生存する）
	 * @param BrokenMissingParents 許容対象外の補完トークンの親の格納先
	 * @param BrokenSubtrees 許容対象外の破損を含むノードの格納先
	 * @param ErrorRanges エラー領域の範囲の格納先
	 * @param CommentRanges 収集済のコメントの範囲
	 * @param Observation 追加の分類の訪問数の格納先
	 * @param TemplateDecls 修復した宣言の範囲
	 * @return 最終検証が照合するデータ
	 * @throws FormatLimitExceeded 構文の探索又は事実の収集が作業予算を超える場合
	 * @throws std::bad_alloc ノードの参照又は検証データのメモリを確保出来ない場合
	 * @throws std::length_error ノードの参照又は検証データの要素数上限を超える場合
	 */
	ValidationContext SourceProjector::CollectCtx(
		const ::ParsedTree &SrcTree,
		const GrammarProfile &Profile,
		const ParseInput &Input,
		const std::span<const TSNode> Nodes,
		std::unordered_set<const void *> *const BrokenMissingParents,
		std::unordered_set<const void *> *const BrokenSubtrees,
		std::vector<std::pair<uint32_t, uint32_t>> *const ErrorRanges,
		std::vector<std::pair<uint32_t, uint32_t>> *const CommentRanges,
		HiddenWork *const Observation,
		const std::span<const std::pair<uint32_t, uint32_t>> TemplateDecls
	) {
		// 同じ判定経路で組み立てた検証データの返戻
		return CollectCtxFromNodes(
			SrcTree,
			Profile,
			Input,
			ParsedNodes(Nodes),
			TemplateDecls,
			BrokenMissingParents,
			BrokenSubtrees,
			ErrorRanges,
			CommentRanges,
			Observation
		);
	}

	/**
	 * 投影のノードからの検証データの組立関数（整形仕様 3.4，4.3）
	 * 計算量：親の表と祖先経路の収集は木のノード数 V に対し O(V)（他のデータの組立を除く）
	 * @param SrcTree 受理した解析結果
	 * @param Profile 入力時に確定した文法
	 * @param Input 適応済の入力と修復前のテキスト
	 * @param Nodes 投影中に記録したノードの原文順の並び
	 * @param TemplateDecls 束縛と文全体の構文から修復した宣言の範囲（位置順）
	 * @param BrokenMissingParents 許容対象外の補完トークンを直接持つパーサーノードの格納先（不要なら null，空で渡す）
	 * @param BrokenSubtrees 許容対象外の破損を含むパーサーノードの格納先（不要なら null，空で渡す）
	 * @param ErrorRanges 領域毎の原文範囲の格納先（不要なら null，空で渡す）
	 * @param CommentRanges 同じ投影の訪問で収集したコメントの範囲（無ければ此処で収集する）
	 * @param Observation 追加の分類の訪問数の格納先（無ければ観測しない）
	 * @return 最終検証が照合するデータ
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	ValidationContext SourceProjector::CollectCtxFromNodes(
		const ::ParsedTree &SrcTree,
		const GrammarProfile &Profile,
		const ParseInput &Input,
		const ParsedNodes Nodes,
		const std::span<const std::pair<uint32_t, uint32_t>> TemplateDecls,
		std::unordered_set<const void *> *const BrokenMissingParents,
		std::unordered_set<const void *> *const BrokenSubtrees,
		std::vector<std::pair<uint32_t, uint32_t>> *const ErrorRanges,
		std::vector<std::pair<uint32_t, uint32_t>> *const CommentRanges,
		HiddenWork *const Observation
	) {
		// 診断に使う親表とコメント範囲の収集
		ValidationContext Ctx;
		const TSNode Root = SrcTree.GetRoot();
		const bool HasTreeError = SrcTree.HasError(Root);
		const ReadingRules &Reading = Profile.GetReading();
		std::vector<TSNode> Path, Errors;
		// 修復した宣言のエラーだけを除き，其の他のエラーは診断で許容しても意味の照合から除く
		std::unordered_set<const void *> UnrepairedSubtrees;
		const bool HasTemplateRepairs = !TemplateDecls.empty();
		TSNode OuterError {};
		if(CommentRanges) Ctx.InputDiagnostics.CommentRanges = std::move(*CommentRanges);
		if(HasTreeError || !CommentRanges) {
			for(size_t NodeIdx = 0; NodeIdx < Nodes.GetSize(); ++NodeIdx) {
				const TSNode Node = Nodes[NodeIdx];
				if(Observation) ++Observation->CtxVisits;
				if(!CommentRanges && !ts_node_eq(Node, Root) && SrcTree.Is(Node, Role::Comment)) {
					Ctx.InputDiagnostics.CommentRanges.push_back({ SrcTree.GetStart(Node), SrcTree.GetEnd(Node) });
				}
				if(!HasTreeError) continue;
				const TSNode Parent = SrcTree.GetParentOf(Node);
				while(!Path.empty() && !ts_node_eq(Path.back(), Parent)) {
					if(HasTemplateRepairs && Path.size() > 1 && UnrepairedSubtrees.contains(Path.back().id)) {
						UnrepairedSubtrees.insert(Path[Path.size() - 2].id);
					}
					// 既存の祖先経路を閉じる時の分類済の破損の親への集約
					if(BrokenSubtrees && Path.size() > 1 && BrokenSubtrees->contains(Path.back().id)) {
						BrokenSubtrees->insert(Path[Path.size() - 2].id);
					}
					if(ts_node_eq(Path.back(), OuterError)) OuterError = {};
					Path.pop_back();
				}
				if(!ts_node_is_null(Parent) && Path.empty()) {
					std::vector<TSNode> Missing;
					for(TSNode Up = Parent; !ts_node_is_null(Up); Up = SrcTree.GetParentOf(Up)) Missing.push_back(Up);
					Path.assign(Missing.rbegin(), Missing.rend());
				}
				const uint32_t Start = SrcTree.GetStart(Node), End = SrcTree.GetEnd(Node);
				const std::span<const std::pair<uint32_t, uint32_t>>::iterator After = std::upper_bound(
					TemplateDecls.begin(),
					TemplateDecls.end(),
					Start,
					[](const uint32_t Pos, const std::pair<uint32_t, uint32_t> &Range) -> bool {
						return Pos < Range.first;
					}
				);
				const bool IsRepaired = After != TemplateDecls.begin() && Start < std::prev(After)->second && End <= std::prev(After)->second;
				if(HasTemplateRepairs && !IsRepaired && (ts_node_is_error(Node) || ts_node_is_missing(Node))) {
					UnrepairedSubtrees.insert(Node.id);
				}
				const bool IsBroken = !IsRepaired && SyntaxParser::IsBrokenNode(SrcTree, Node, std::span<const TSNode>(Path));
				if(IsBroken) {
					++Ctx.InputDiagnostics.BrokenCount;
					if(BrokenSubtrees) BrokenSubtrees->insert(Node.id);
				}
				// 最外のエラーの記録（許容した外側のエラーの内側も従来通り除く）
				if(SrcTree.Is(Node, Role::Error) && ts_node_is_null(OuterError)) {
					OuterError = Node;
					if(IsBroken) Errors.push_back(Node);
				}
				// 既存の末尾カンマの除外を保つ，許容対象外の欠落の構文事実への投影
				if(
					BrokenMissingParents && IsBroken && SrcTree.IsMissing(Node) && !ts_node_is_null(Parent) &&
					!BrokenMissingParents->contains(Parent.id) &&
					(!Reading.CanTolerateTrailingCommaError || ts_node_is_null(SyntaxParser::JsonTrailingCommaToken(SrcTree, Node)))
				) BrokenMissingParents->insert(Parent.id);
				Path.push_back(Node);
			}
		}
		// 最後に開いた儘の祖先経路への分類の集約
		while(Path.size() > 1) {
			if(HasTemplateRepairs && UnrepairedSubtrees.contains(Path.back().id)) UnrepairedSubtrees.insert(Path[Path.size() - 2].id);
			if(BrokenSubtrees && BrokenSubtrees->contains(Path.back().id)) BrokenSubtrees->insert(Path[Path.size() - 2].id);
			Path.pop_back();
		}
		std::sort(Ctx.InputDiagnostics.CommentRanges.begin(), Ctx.InputDiagnostics.CommentRanges.end());
		Ctx.InputDiagnostics.CommentRanges.erase(
			std::unique(Ctx.InputDiagnostics.CommentRanges.begin(), Ctx.InputDiagnostics.CommentRanges.end()),
			Ctx.InputDiagnostics.CommentRanges.end()
		);
		if(
			const bool IsVerification = std::holds_alternative<ParseInput::VerificationSrc>(Input.Origin);
			SrcTree.HasOriginalInputError() || IsVerification && HasTreeError
		) {
			// 入力の破損又は完成候補のエラーが在る時に領域を記録
			Ctx.ErrorZones = CollectErrorZones(SrcTree, Nodes, Errors, ErrorRanges);
			Ctx.InputDiagnostics.InputBrokenCount = std::min(Input.PreparedBroken, Ctx.InputDiagnostics.BrokenCount);
		}
		// 意味上の区別と保護する位置の収集
		Ctx.SemanticChecks.Checks = CollectSemanticChecks(SrcTree, HasTemplateRepairs ? &UnrepairedSubtrees : nullptr);
		if(Reading.CanAbsorbRulesIntoValComments) Ctx.ProtectedRegions.InputText = Input.GetSrcText();
		// 解析の為に置いたマーカー
		if(const unsigned char Marker = SrcTree.GetAnalysisMark()) Ctx.ProtectedRegions.AnalysisMarks.emplace_back(1, Marker);
		// 検証データの返戻
		return Ctx;
	}

	/**
	 * パーサーの読違を，実引数・値の並びと意味を保つ境界へ変換する関数
	 * 計算量：ノード数 N，子の総数 C，ノード毎の境界の数 B に対し O(N + C B)
	 * @param Syntax 投影中の構文
	 * @param Reading 入力側の読取規則
	 * @param FirstChars ノード毎に継承済の最初のトークンの字
	 * @param HasReadingCalls ジェネリック呼出の読違が在るか
	 * @param Readings 入力側で確定した読取結果と此の投影だけで使う所属の格納先
	 * @throws FormatLimitExceeded 走査が作業予算を超える場合
	 * @throws std::bad_alloc 境界の列のメモリを確保出来ない場合
	 * @throws std::length_error 境界の列の要素数が上限を超える場合
	 */
	void SourceProjector::ProjectReadings(
		SyntaxModel &Syntax,
		const ReadingRules &Reading,
		const std::span<const uint16_t> FirstChars,
		const bool HasReadingCalls,
		std::vector<ReadingFact> &Readings
	) {
		// 同じ先頭経路の祖先を候補毎に辿らず，未取得の所属だけを確定した祖先から継ぐ
		std::vector<NodeId> Path;
		const auto IsInCall = [&](const NodeId Node) -> bool {
			Syntax.Charge(1);
			Path.clear();
			for(NodeId Id = Node; Id != NoNode && !Readings[Id].HasCallContext; Id = Syntax.GetParentOf(Id)) {
				Syntax.Charge(1);
				Path.push_back(Id);
				const NodeId Parent = Syntax.GetParentOf(Id);
				if(Parent == NoNode || Readings[Parent].IsCall || Syntax.GetChild(Parent, 0) != Id) break;
			}
			for(size_t Idx = Path.size(); Idx--;) {
				Syntax.Charge(1);
				const NodeId Id = Path[Idx], Parent = Syntax.GetParentOf(Id);
				ReadingFact &Fact = Readings[Id];
				Fact.IsInCallArguments =
				Parent != NoNode && (Readings[Parent].IsCall || Syntax.GetChild(Parent, 0) == Id && Readings[Parent].IsInCallArguments);
				Fact.HasCallContext = true;
			}
			// 確定した所属の返戻
			return Readings[Node].IsInCallArguments;
		};
		const auto SpaceBefore = [&](const NodeId Parent, const NodeId CurNode, const NodeId Next) -> std::optional<bool> {
			const bool IsCurNamed = Syntax.IsNamed(CurNode), IsNextNamed = Syntax.IsNamed(Next);
			const unsigned char NextChar = static_cast<unsigned char>(FirstChars[Next]);
			const auto IsCurTokenRole = [&](const Role Wanted) -> bool {
				return !Syntax.GetChildCount(CurNode) && Syntax.IsTokenRole(CurNode, Wanted);
			};
			const auto IsNextTokenRole = [&](const Role Wanted) -> bool {
				return !Syntax.GetChildCount(Next) && Syntax.IsTokenRole(Next, Wanted);
			};
			// 型引数の読違（投影が構文事実として記録した物）に対する原文空白へ依存しない出力
			const auto IsMisparsedGenericCall = [&Readings](const NodeId Node) -> bool {
				// ジェネリック呼出を比較と誤読した二項式かの返戻
				return Node != NoNode && Readings[Node].IsCall;
			};
			// 型引数と誤読した比較式の判定
			const auto IsMisparsedTemplate = [&Readings](const NodeId Node) -> bool {
				// 比較を型引数と誤読した山括弧かの返戻
				return Node != NoNode && Readings[Node].IsComparison;
			};
			// ジェネリック呼出の型引数のトークンを密着させる事の返戻
			if(!IsCurNamed && IsCurTokenRole(Role::LessThanToken) || !IsNextNamed && IsNextTokenRole(Role::LessThanToken)) {
				if(const NodeId Owner = Syntax.GetParentOf(Parent); IsMisparsedGenericCall(Owner) || IsMisparsedGenericCall(Parent)) {
					// 誤読したジェネリック呼出（一覧の要素へ分けた開きを含む）の型引数を密着させる事の返戻
					return false;
				}
			} else if(
				(!IsCurNamed && IsCurTokenRole(Role::GreaterThanToken) || !IsNextNamed && IsNextTokenRole(Role::GreaterThanToken)) &&
				IsMisparsedGenericCall(Parent)
			// 誤読したジェネリック呼出の型引数を密着させる事の返戻
			) return false;
			// 誤解析で割れた比較演算子は元は１トークンの為に密着
			// 正当な型引数リスト自身の閉じは，型引数の後の比較演算子ではない
			if(
				!IsNextNamed && Reading.MisparsedCmpLeadChars.find(NextChar) != std::string_view::npos &&
				(!Syntax.Is(Parent, Role::AngleBracketList) || IsMisparsedTemplate(Parent) || !IsNextTokenRole(Role::GreaterThanToken))
			) {
				// 正当な型引数の後だけ空白を許す事の返戻
				if(Syntax.Is(CurNode, Role::RecoverableTemplate)) return !IsMisparsedTemplate(CurNode);
				NodeId Right = CurNode;
				while(Right != NoNode && !Syntax.Is(Right, Role::MisparsedTemplateHost)) {
					Syntax.Charge(1);
					const uint32_t Count = Syntax.GetChildCount(Right);
					Right = Count ? Syntax.GetChild(Right, Count - 1) : NoNode;
				}
				// 右端に包んだ誤読の型引数の後の割れた演算子を密着させる事の返戻
				if(IsMisparsedTemplate(Right)) return false;
			}
			// 同じ誤解析の左側に在る本来の比較演算子の空白区切と，カンマの密着の返戻
			if(
				(
					Syntax.Is(Parent, Role::MisparsedTemplateHost) && Syntax.Is(Next, Role::AngleBracketList) ||
					Syntax.Is(Parent, Role::AngleBracketList)
				) && IsMisparsedTemplate(Parent) ||
				Syntax.Is(Parent, Role::CallExpr) && Syntax.Is(CurNode, Role::GenericFunction) && Syntax.Is(Next, Role::ArgSeq) &&
				IsMisparsedTemplate(CurNode)
			) return !IsNextTokenRole(Role::CommaToken);
			if(
				Readings[Parent].IsProduct && IsCurNamed && !IsNextNamed &&
				(IsNextTokenRole(Role::AmpersandToken) || IsNextTokenRole(Role::AsteriskToken))
			// 乗算の前の空白の返戻
			) return true;
			// 境界を追加しない事の返戻
			return std::nullopt;
		};
		for(NodeId Parent = 0; Parent < Syntax.GetNodeCount(); ++Parent) {
			SyntaxNode &Entry = Syntax.Nodes[Parent];
			if(HasReadingCalls && Syntax.Is(Parent, Role::GroupingParen) && IsInCall(Parent)) {
				Entry.Facts.Add(ConstraintKind::CallArguments);
			}
			if(
				Readings[Parent].IsComparison && Syntax.Is(Parent, Role::AngleBracketList) && SyntaxModel::HasChildMatching(
					Syntax,
					Parent,
					[&](const NodeId Child) -> bool {
						return Syntax.IsTokenRole(Child, Role::CommaToken);
					}
				)
			) Entry.Facts.Add(ConstraintKind::ValueSequence);
			const std::span<const NodeId> Children = Syntax.GetChildrenOf(Parent);
			for(uint32_t Idx = 1; Idx < Children.size(); ++Idx) {
				const SemanticBoundary *const Existing = Entry.Facts.GetBoundaryBefore(Idx);
				if(Existing && Existing->Kind != BoundaryKind::Glued && Existing->Kind != BoundaryKind::Spaced) continue;
				const std::optional<bool> Space = SpaceBefore(Parent, Children[Idx - 1], Children[Idx]);
				if(!Space) continue;
				std::vector<SemanticBoundary> &Boundaries = Entry.Facts.Boundaries.Edit();
				const std::vector<SemanticBoundary>::iterator At = std::lower_bound(
					Boundaries.begin(),
					Boundaries.end(),
					Idx,
					[](const SemanticBoundary &Boundary, const uint32_t Before) -> bool {
						return Boundary.Before < Before;
					}
				);
				const BoundaryKind Kind = *Space ? BoundaryKind::RequiredSpace : BoundaryKind::NoInlineSpace;
				if(At != Boundaries.end() && At->Before == Idx) At->Kind = Kind;
				else Boundaries.insert(At, { Idx, Kind });
			}
		}
		// 終了
		return;
	}

	/**
	 * 型引数の中の比較・シフトの括弧の保持条件の記録関数
	 * 計算量：ノード数 N と子の総数 C に対し O(N + C)
	 * @param Syntax 投影した構文モデル
	 * @param Readings 入力側で確定した読取結果
	 * @throws FormatLimitExceeded 走査が作業予算を超える場合
	 * @throws std::bad_alloc 走査の順と事実の表のメモリを確保出来ない場合
	 * @throws std::length_error 走査の順又は事実の表の要素数が上限を超える場合
	 */
	void SourceProjector::RecordAngleParens(SyntaxModel &Syntax, const std::vector<ReadingFact> &Readings) {
		// 山括弧の並びを持たない文法の終了
		if(!Syntax.GetRules().HasAngleListSyntax) return;
		std::vector<uint8_t> HasConflict(Syntax.GetNodeCount(), 0), IsInAngles(Syntax.GetNodeCount(), 0);
		// 投影中に加えた親も含め，番号でなく木の接続から親が先の順を作る
		std::vector<NodeId> Order { Syntax.GetRoot() };
		for(size_t Idx = 0; Idx < Order.size(); ++Idx) {
			Syntax.Charge(1);
			const std::span<const NodeId> Children = Syntax.GetChildrenOf(Order[Idx]);
			Syntax.Charge(Children.size());
			Order.insert(Order.end(), Children.begin(), Children.end());
		}
		// 同じ部分木のトークンを繰返し読まず，子から親へ１度だけ集計する
		for(size_t Idx = Order.size(); Idx--;) {
			const NodeId Id = Order[Idx];
			Syntax.Charge(1);
			HasConflict[Id] |= static_cast<uint8_t>(Syntax.IsTokenRole(Id, Role::AngleConflictToken));
			const NodeId Parent = Syntax.GetParentOf(Id);
			if(Parent != NoNode) HasConflict[Parent] |= HasConflict[Id];
		}
		for(const NodeId Id : Order) {
			Syntax.Charge(1);
			const NodeId Parent = Syntax.GetParentOf(Id);
			if(Parent == NoNode) continue;
			const NodeId Head = Syntax.GetChild(Parent, 0);
			// 重なるラッパーは最外の文脈を共有し，実引数・添字・本体等の区切では探索範囲を閉じる
			IsInAngles[Id] =
			Syntax.Is(Parent, Role::AngleBracketList) && (Readings.empty() || !Readings[Parent].IsComparison) || IsInAngles[Parent] &&
			(Syntax.Is(Parent, Role::GroupingParen) || Syntax.IsNamed(Head) || !Syntax.IsBracketLead(Head, Role::OpeningBracketLead));
			Syntax.Nodes[Id].Facts.NeedsAngleParens =
			Syntax.Is(Id, Role::GroupingParen) && (IsInAngles[Id] && HasConflict[Id] || !Readings.empty() && Readings[Id].IsLeadArgParen);
		}
		// 終了
		return;
	}

	/**
	 * 構文事実の記録関数（整形仕様 3.2，4.2）
	 * 比較と型引数・ジェネリック呼出の誤読，又は乗算を型と読んだ記号の判定をノードへ記録する
	 * 原文の隣接が読みを決める境界も記録する
	 * 計算量：ノード数 V，保持するテキスト長 B と意味判定・間隙取得の費用 Q に対し O(V + B + Q)
	 * @param SrcTree コメントを分離した解析結果
	 * @param Syntax 構文事実を記録するノード表
	 * @param Profile 入力で確定した文法
	 * @param Separated 投影の間だけ持つコメントの分離の結果
	 * @param Sources 投影したノード毎の原文の出所
	 * @param Symbols 投影時に取得したノード毎の記号（後の検証データと共有）
	 * @param ValidationNodes 検証データに含むパーサーノード
	 * @param ExtraNodes 検証データと異なる投影ノード
	 * @param BrokenMissingParents 許容対象外の補完トークンを直接持つパーサーノード
	 * @param BrokenSubtrees 許容対象外の破損を含むパーサーノード
	 * @param SeparatedBefore 投影したノード毎の，区切記法の後の最初のトークンか
	 * @param IsVerification 出力の最終検証用の投影か
	 * @param HasFilePreamble ファイル先頭のバイト列として読むか
	 * @param Gaps 分割済の本文と間隙の切出の作業領域
	 * @param Work 事実の準備・記録で訪ねたノードの観測用計数（無ければ計上しない）
	 * @throws FormatLimitExceeded 意味判定・事実取得が処理予算又は前提を満たさない場合
	 * @throws std::bad_alloc 事実・所有範囲又は保持するテキストのメモリを確保出来ない場合
	 * @throws std::length_error 事実・所有範囲又は保持するテキストの上限を超える場合
	 * @throws std::out_of_range 事実を照合するノードが構文表に無い場合
	 */
	void SourceProjector::RecordFacts(
		const ::ParsedTree &SrcTree,
		SyntaxModel &Syntax,
		const GrammarProfile &Profile,
		const CommentSeparation &Separated,
		const std::vector<Origin> &Sources,
		const std::span<const TSSymbol> Symbols,
		const ParsedNodes ValidationNodes,
		const std::span<const TSNode> ExtraNodes,
		const std::unordered_set<const void *> &BrokenMissingParents,
		const std::unordered_set<const void *> &BrokenSubtrees,
		const std::vector<uint8_t> &SeparatedBefore,
		const bool IsVerification,
		const bool HasFilePreamble,
		GapExclusions &Gaps,
		StructureWorkAccount *const Work
	) {
		// 読違判定と読取規則の取得
		const std::unordered_map<const void *, uint8_t> TemplateMisreads =
		IsVerification ? std::unordered_map<const void *, uint8_t>() : PrepareTemplateContexts(SrcTree);
		const SyntaxRules &Rules = SrcTree.GetRules();
		const ReadingRules &Reading = SrcTree.GetReading();
		const bool HasInterpolation =
		Profile.FindKindWithRole(Role::StrInterpolation) != nullptr || Profile.FindKindWithRole(Role::InterpolatedLiteral) != nullptr;
		// 括弧の意味は生成形式の固定トークンの定義から１度だけ取得する
		const auto GetDelimiterDef = [&](const FormPartRole Part) -> const KindDef * {
			const std::string Token = SyntaxGeneration::GetGeneratedFixed(Syntax, FormOperation::GeneratedLexemes, Part);
			const std::unordered_map<std::string_view, FormPart>::const_iterator Found = Rules.TokenDefs.find(Token);
			if(Found == Rules.TokenDefs.end()) {
				throw FormatLimitExceeded("an interpolation delimiter has no grammar token", FailureReason::FormatterDefect);
			}
			// 文法が所有する固定トークンの定義の返戻
			return Found->second.Def;
		};
		const KindDef *const BraceOpen = HasInterpolation ? GetDelimiterDef(FormPartRole::BlockOpen) : nullptr;
		const KindDef *const BraceClose = HasInterpolation ? GetDelimiterDef(FormPartRole::BlockClose) : nullptr;
		const KindDef *const ParenOpen = HasInterpolation ? GetDelimiterDef(FormPartRole::CallOpen) : nullptr;
		const KindDef *const ParenClose = HasInterpolation ? GetDelimiterDef(FormPartRole::CallClose) : nullptr;
		const bool NeedsReadings = !IsVerification && (
			Reading.CanMisreadTemplateArgs || Reading.CanMisreadGenericCalls || !Reading.MisparsedCmpLeadChars.empty() ||
			Profile.FindKindWithRole(Role::ProductAmbiguousPtrType)
		);
		std::vector<ReadingFact> Readings(NeedsReadings ? Syntax.GetNodeCount() : 0);
		bool HasReadingSpaces = !Reading.MisparsedCmpLeadChars.empty(), HasReadingCalls = false; // 正当な型引数の後の比較演算子にも空白を要する
		const std::vector<std::pair<uint32_t, uint32_t>> &CommentRanges = Gaps.CommentRanges;
		// パーサーだけに見せた区切とアンカーへ紐付けたコメントは，原文からコピーする保持境界のトークンではない
		std::vector<uint32_t> Inserted;
		Inserted.reserve(SrcTree.GetInsertedBreaks().size() + Gaps.SyntheticPositions.size());
		std::set_union(
			SrcTree.GetInsertedBreaks().begin(),
			SrcTree.GetInsertedBreaks().end(),
			Gaps.SyntheticPositions.begin(),
			Gaps.SyntheticPositions.end(),
			std::back_inserter(Inserted)
		);
		// 補った範囲と分離したコメントを除く原文の間隙の取得
		const auto OriginalGap =
		[&SrcTree, &Separated, &Inserted](const uint32_t From, const uint32_t To, const bool ShouldExcludeInserted = true)
		-> std::string {
			// 挿入トークンを残す境界では，コメントの分離だけを適用する
			if(!ShouldExcludeInserted) return Separated.BuildSeparatedText(SrcTree, From, To);
			// 重なるノードの間にはコピーするテキストが無い事の返戻
			if(To <= From) return {};
			std::string Gap;
			Gap.reserve(To - From);
			uint32_t Cursor = From;
			for(
				std::vector<uint32_t>::const_iterator Pos = std::lower_bound(Inserted.begin(), Inserted.end(), From);
				Pos != Inserted.end() && *Pos < To;
				++Pos
			) {
				Gap.append(Separated.BuildSeparatedText(SrcTree, Cursor, *Pos));
				Cursor = *Pos + 1;
			}
			Gap.append(Separated.BuildSeparatedText(SrcTree, Cursor, To));
			// 原文に在ったトークンだけの返戻
			return Gap;
		};
		const bool HasSynthetic = !SrcTree.GetSyntheticGroups().empty();
		// C 系の読めなかったプリプロセッサディレクティブの物理行（行継続を含む，先行順の為に開始位置の昇順）
		std::vector<std::pair<uint32_t, uint32_t>> DirectiveLines;
		if(Rules.HasLineDirectives && SrcTree.HasError(SrcTree.GetRoot())) for(NodeId Id = 0; Id < Sources.size(); ++Id) {
			if(Work) Work->Charge(1);
			if(!Syntax.Is(Id, Role::Error) || Sources[Id].Start >= SrcTree.size() || SrcTree[Sources[Id].Start] != '#') continue;
			// プリプロセッサディレクティブで始まるエラー領域の記録
			if(!IsVerification && IsPreprocDirectiveAt(SrcTree, Sources[Id].Start)) {
				Syntax.Nodes[Id].Facts.Add(ConstraintKind::DirectiveZone);
			}
			size_t LineEnd = Sources[Id].End;
			while((LineEnd = SrcTree.find('\n', LineEnd)) != std::string::npos && LineEnd && SrcTree[LineEnd - 1] == '\\') ++LineEnd;
			DirectiveLines.emplace_back(Sources[Id].Start, static_cast<uint32_t>(std::min(LineEnd, SrcTree.size())));
		}
		// 読めなかったディレクティブの後に同じ物理行で始まるノードか（ディレクティブの残りのトークンは領域と共に原文の間隙を保つ）
		const auto IsDirectiveTail = [&DirectiveLines](const uint32_t Start) -> bool {
			const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator After = std::lower_bound(
				DirectiveLines.begin(),
				DirectiveLines.end(),
				Start,
				[](const std::pair<uint32_t, uint32_t> &Line, const uint32_t Pos) -> bool {
					// ディレクティブが位置より前に始まるかの返戻
					return Line.first < Pos;
				}
			);
			// 直前に始まるディレクティブの行の中かの返戻
			return After != DirectiveLines.begin() && Start < std::prev(After)->second;
		};
		// 投影後に残るエラーを祖先へ伝える
		const KindDef &NameDef = Profile.GetKindOf(Profile.FindSymbolWithRole(Role::Identifier));
		const bool HasBroken = !BrokenSubtrees.empty() || !BrokenMissingParents.empty();
		std::vector<uint8_t> HasError(HasBroken ? Sources.size() : 0, 0);
		if(HasBroken) for(NodeId Id = static_cast<NodeId>(Sources.size()); Id--;) {
			if(Work) Work->Charge(1);
			const TSNode Src = SrcNode(Sources[Id], ValidationNodes, ExtraNodes);
			// 隠れたトークンはエラーを持たない
			if(Sources[Id].NodeIdx == NoNode) continue;
			// 逐語保持と破損を区別し，検証データと同じ祖先経路で求めた分類だけを使う
			const bool IsOwn =
			BrokenSubtrees.contains(Src.id) && (SrcTree.IsError(Src) || Syntax.Nodes[Id].Facts.Has(ConstraintKind::Verbatim)) ||
			BrokenMissingParents.contains(Src.id);
			HasError[Id] = HasError[Id] || IsOwn;
			if(HasError[Id] && Syntax.Nodes[Id].Parent != NoNode) HasError[Syntax.Nodes[Id].Parent] = 1;
		}
		// 破損の無い投影では，零だけのノード数分の表を作らない
		const auto ContainsError = [&HasError](const NodeId Id) -> bool {
			// 同じ投影の破損の有無の返戻
			return !HasError.empty() && HasError[Id];
		};
		// エラー領域・マクロ・引用文字列・名前・破損宣言の所属は，確定した親の状態から同じ前進走査で継承
		struct Context {
			bool IsInErrorZone : 1; // エラー又は保持するディレクティブの内側か
			bool IsInMacroTokens : 1; // マクロのトークン列の内側か
			bool IsInQuotedText : 1; // 逐語の引用文字列の内側か
			bool HasObservedText : 1; // 観測される式のテキストの内側か
			bool DoesRequireInlineCode : 1; // 囲む引用の記法が補間の改行を許さないか
			bool IsInInterpolation : 1; // 補間のコード又は其の内部の引用か
			bool IsImportNamePart : 1; // 同行に保つインポート名の部品か
			bool IsInBrokenDecl : 1; // 境界を保持する破損宣言の内側か
			bool IsInLoopCond : 1; // 制御フローが値を照会する反復条件のラッパーの内側か
			bool IsAccessTooLong = false; // 可視性の定義の最大長を超えるか
			bool IsWordTooLong = false; // 文法の語の最大長を超えるか
			std::string WordText {}; // 親へ渡す最大語長以内のトークン列
			std::string AccessText {}; // 可視性の読取に使う上限内のトークン列
		};
		std::vector<Context> Contexts(Sources.size());
		size_t AccessLimit = 0;
		for(const std::string_view Access : Rules.AccessOrder) AccessLimit = std::max(AccessLimit, Access.size());
		// 同じ束縛の区間で真偽値名を共有し，遮蔽出来ない言語では固定値を使う
		const bool HasBoolBindings = SrcTree.GetBoolNameSource() != ::ParsedTree::BoolNameSource::Literal;
		std::optional<uint32_t> BoolUse;
		uint32_t BoolLimit = 0;
		bool HasFalseName = true, HasTrueName = true, HasTokenSpacing = false;
		// 所属の走査で最外のマクロ使用位置も原文順に集める
		std::vector<uint32_t> ExprMacroUses, DeclaratorMacroUses;
		uint32_t MacroReadEnd = 0;
		for(NodeId Id = 0; Id < Sources.size(); ++Id) {
			if(Work) Work->Charge(1);
			// 真偽値名の照会も原文順で行い，各ノードの種別を所属判定の前に確定
			SyntaxNode &Node = Syntax.Nodes[Id];
			if(Node.Children.empty()) {
				Node.Facts.Meanings = Profile.ReadWordMeanings(Node.Lexeme);
				Node.Facts.TokenDef = Profile.ReadTokenDef(Node.Lexeme);
			}
			if(HasBoolBindings && (!BoolUse || Sources[Id].Start < *BoolUse || Sources[Id].Start >= BoolLimit)) {
				BoolUse = Sources[Id].Start;
				HasFalseName = SrcTree.HasBoolNameAt(Syntax.GetRules().GetSpelling(Word::False), *BoolUse);
				HasTrueName = SrcTree.HasBoolNameAt(Syntax.GetRules().GetSpelling(Word::True), *BoolUse);
				BoolLimit = SrcTree.GetBoolNameLimit(*BoolUse);
				if(Gaps.Work) Gaps.Work->BoolQueries += 2;
			}
			Node.Facts.HasFalseName = HasFalseName;
			Node.Facts.HasTrueName = HasTrueName;
			if(
				Node.Children.empty() && Syntax.Is(Id, Role::ExprStmtLiteral) &&
				(Syntax.IsWord(Id, Word::True) && !Node.Facts.HasTrueName || Syntax.IsWord(Id, Word::False) && !Node.Facts.HasFalseName)
			) Node.Kind = { &NameDef };
			if(
				Sources[Id].NodeIdx != NoNode &&
				Profile.GetProjectionChecks(SrcTree.GetSymbol(SrcNode(Sources[Id], ValidationNodes, ExtraNodes))).CanReadContextualType
			) {
				const TSNode Raw = SrcNode(Sources[Id], ValidationNodes, ExtraNodes);
				const TSNode Owner = SrcTree.GetParentOf(Raw);
				const ::ParsedTree::NameBinding *const Binding = SrcTree.GetNameBinding(SrcTree.GetView(Raw), Sources[Id].Start);
				bool IsDeconstruction = false;
				for(const ParsedChild &Entry : SrcTree.GetChildrenOf(Owner)) {
					Syntax.Charge(1);
					if(!Entry.IsNamed) continue;
					const TSNode Name = SrcTree.GetFieldChild(Entry.Node, ChildRole::Name);
					IsDeconstruction |= SrcTree.Is(Entry.Node, Role::TuplePattern) || SrcTree.Is(Name, Role::TuplePattern);
				}
				// 型の名前を推論の指定や分解の指定へ読み替えない
				if(
					!IsDeconstruction &&
					(!SrcTree.Is(Owner, Role::VarDecl) && !SrcTree.Is(Owner, Role::ForeachStmt) || Binding && Binding->IsKnown && Binding->IsType)
				) Node.Kind = { &NameDef };
			}
			const NodeId Parent = Node.Parent;
			const RoleSet &Roles = Node.Kind.Def->Roles;
			const auto Has = [&Roles](const Role Wanted) -> bool {
				// 真偽値名の読替後の役割の返戻
				return Roles.test(static_cast<size_t>(Wanted));
			};
			if(Has(Role::Interpolation) || Has(Role::TemplateInterpolation)) if(Node.Children.size() > 1) {
				const NodeId Open = Node.Children.front(), Close = Node.Children.back();
				const bool IsParen = Syntax.Is(Open, Role::OpeningParenToken);
				Syntax.Nodes[Open].Facts.DelimiterDef = IsParen ? ParenOpen : BraceOpen;
				Syntax.Nodes[Close].Facts.DelimiterDef = IsParen ? ParenClose : BraceClose;
			}
			if(Has(Role::InterpolationOpenToken)) Node.Facts.DelimiterDef = Has(Role::OpeningParenToken) ? ParenOpen : BraceOpen;
			else if(Has(Role::InterpolationCloseToken)) Node.Facts.DelimiterDef = BraceClose;
			Contexts[Id].IsInInterpolation = Has(Role::StrInterpolation) || Parent != NoNode && Contexts[Parent].IsInInterpolation;
			Node.Facts.DoesPreserveQuoteForm =
			Reading.ShouldKeepInterpolationQuotes && Has(Role::QuotedStr) && Parent != NoNode && Contexts[Parent].IsInInterpolation;
			Contexts[Id].IsInErrorZone = Has(Role::Error) || Parent != NoNode && Contexts[Parent].IsInErrorZone ||
			!DirectiveLines.empty() && IsDirectiveTail(Sources[Id].Start);
			if(Node.Facts.TokenSpacing == TokenSpacingMode::None && Parent != NoNode) {
				Node.Facts.TokenSpacing = Syntax.Nodes[Parent].Facts.TokenSpacing;
			}
			Contexts[Id].IsInMacroTokens = Node.Facts.TokenSpacing != TokenSpacingMode::None;
			HasTokenSpacing = HasTokenSpacing || Contexts[Id].IsInMacroTokens;
			// 条件以外の数値と，評価しない複合式の内部の値は生成しない
			Contexts[Id].IsInLoopCond = Parent != NoNode && Syntax.HasField(Id, ChildRole::Cond) &&
			Syntax.IsAny(Parent, { Role::CountedLoop, Role::ForClause, Role::PostCondLoop, Role::WhileLoop });
			if(Parent != NoNode && Contexts[Parent].IsInLoopCond) {
				if(Syntax.Is(Parent, Role::CondParenWrap)) Contexts[Id].IsInLoopCond = true;
				else if(Syntax.Is(Parent, Role::UnaryPrefixExpr)) {
					const std::string_view Operator = Syntax.GetLexemeOf(Syntax.GetChild(Parent, 0));
					Contexts[Id].IsInLoopCond = Operator == "+" || Operator == "-";
				}
			}
			if(Has(Role::DebugInterpolation)) {
				if(Work) Work->Charge(Node.Children.size());
				// 子のトークンの定義は前順の走査で後から記録する為，子の綴りから読む
				Node.Facts.DoesObserveText = std::any_of(
					Node.Children.begin(),
					Node.Children.end(),
					[&Syntax, &Profile](const NodeId Child) -> bool {
						const SyntaxNode &Part = Syntax.Nodes[Child];
						const KindDef *const Token = Part.Children.empty() ? Profile.ReadTokenDef(Part.Lexeme) : nullptr;
						// 等号で表示を要求する補間かの返戻
						return Token && Token->Roles.test(static_cast<size_t>(Role::AssignmentToken));
					}
				);
			}
			Contexts[Id].HasObservedText = Node.Facts.DoesObserveText || Parent != NoNode && Contexts[Parent].HasObservedText;
			// 本文の保護は補間のコードへ継承せず，表示される式のテキストは内部も保つ
			Contexts[Id].IsInQuotedText = Contexts[Id].HasObservedText || Has(Role::VerbatimQuotedStr) ||
			!Has(Role::StrInterpolation) && Parent != NoNode && Contexts[Parent].IsInQuotedText;
			Contexts[Id].DoesRequireInlineCode = Parent != NoNode && Contexts[Parent].DoesRequireInlineCode;
			if(!Reading.LineBoundInterpolationQuotes.empty() && Has(Role::StrLikeAll)) {
				const std::string_view Text = SrcTree.GetView(SrcNode(Sources[Id], ValidationNodes, ExtraNodes));
				const size_t Quote = Text.find_first_of(Reading.LineBoundInterpolationQuotes);
				if(Work) Work->Charge(Quote == std::string_view::npos ? Text.size() : Quote + 1);
				if(Quote != std::string_view::npos) for(const LiteralReading &Literal : Reading.Literals) {
					if(Work) Work->Charge(Literal.Open.size());
					// 長い開始記号から照合する登録順で，単一行と複数行の引用を区別する
					if(!Text.substr(Quote).starts_with(Literal.Open)) continue;
					const bool IsVerbatim =
					std::ranges::find(Reading.VerbatimStrPrefixes, Text.substr(0, Quote)) != Reading.VerbatimStrPrefixes.end();
					Contexts[Id].DoesRequireInlineCode = !Literal.CanCrossLine && !IsVerbatim;
					break;
				}
			}
			Node.Facts.DoesRequireInlineCode = Has(Role::StrInterpolation) && Contexts[Id].DoesRequireInlineCode;
			if(!IsVerification && Has(Role::StrLikeAll) && Contexts[Id].DoesRequireInlineCode) {
				Node.Facts.Add(ConstraintKind::NoOptionalBreak);
			}
			if(!IsVerification) {
				Contexts[Id].IsImportNamePart = (Has(Role::Identifier) || Has(Role::ScopedIdentifier)) && Parent != NoNode &&
				(Syntax.Is(Parent, Role::UnbrokenImport) || Contexts[Parent].IsImportNamePart);
			}
			if(
				Rules.HasLineDirectives && Parent != NoNode && ContainsError(Id) && Syntax.Is(Parent, Role::Preproc) &&
				Syntax.GetFieldChild(Parent, ChildRole::Cond) == Id
			) Contexts[Id].IsInErrorZone = true;
			// 読めなかったセレクタの並びは子孫の境界も保ち，回復した名前を通常の密着規則へ渡さない
			Contexts[Id].IsInBrokenDecl = ContainsError(Id) && Has(Role::SelectorList) || Rules.ShouldProtectBrokenDeclBoundaries && (
				ContainsError(Id) && (Has(Role::Decl) || Has(Role::TypeMemberBody) && !Has(Role::MemberBody) || Has(Role::FunctionDeclNode)) &&
				std::any_of(
					Syntax.GetChildrenOf(Id).begin(),
					Syntax.GetChildrenOf(Id).end(),
					[&](const NodeId Child) -> bool {
						// エラーのノードかの返戻
						return Syntax.Is(Child, Role::Error);
					}
				)
			) || Parent != NoNode && Contexts[Parent].IsInBrokenDecl;
			if(Reading.HasPreprocMacros) {
				const TSNode Src = SrcNode(Sources[Id], ValidationNodes, ExtraNodes);
				if(
					Node.Children.empty() || Sources[Id].NodeIdx == NoNode || Contexts[Id].IsInErrorZone || ContainsError(Id) ||
					!(Parent != NoNode && Syntax.Is(Parent, Role::Decl)) && (IsVerification || !Has(Role::MacroGuardedParens))
				) continue;
				const uint32_t Start = SrcTree.GetStart(Src), End = SrcTree.GetEnd(Src);
				if(Start < MacroReadEnd) continue;
				const uint8_t Mask = static_cast<uint8_t>(::ParsedTree::MacroProp::Declarator) |
				(IsVerification ? 0 : static_cast<uint8_t>(::ParsedTree::MacroProp::Expr));
				SrcTree.CollectMacroUses(Src, Mask, ExprMacroUses, DeclaratorMacroUses);
				MacroReadEnd = End;
			}
		}
		std::vector<NodeId> ParenInner(!IsVerification && Reading.HasPreprocMacros ? Sources.size() : 0, NoNode);
		std::vector<uint8_t> HasParenExprMacro(ParenInner.size(), 0);
		std::vector<NodeId> LastLeaf(!ParenInner.empty() || HasTokenSpacing ? Sources.size() : 0, NoNode);
		// 葉と保持する頭尾の境界の所有範囲の子から親への１度の継承
		// Go の文の並び等に含まれる配置用の改行は所有範囲を広げない
		std::vector<NodeId> FirstLeaf(Sources.size());
		std::vector<uint16_t> FirstChars(NeedsReadings ? Sources.size() : 0, 0);
		std::vector<std::pair<uint32_t, uint32_t>> OwnedEdges(Sources.size());
		std::vector<uint8_t> IsInterpolated(HasInterpolation ? Sources.size() : 0, 0);
		std::vector<PreservedSrc> GapSources(Sources.size(), PreservedSrc::HiddenContent);
		for(NodeId Id = static_cast<NodeId>(Sources.size()); Id--;) {
			if(Work) Work->Charge(1);
			SyntaxNode &Node = Syntax.Nodes[Id];
			// 語の要約と可視性は，境界と同じ後行順の訪問で子から親へ渡す
			Context &Current = Contexts[Id];
			const size_t WordLimit = Profile.GetWordTextLimit();
			Current.IsWordTooLong = Node.Lexeme.size() > WordLimit;
			if(!Current.IsWordTooLong) Current.WordText = Node.Lexeme;
			Current.IsAccessTooLong = Node.Lexeme.size() > AccessLimit;
			if(!Current.IsAccessTooLong) Current.AccessText = Node.Lexeme;
			for(const NodeId Child : Node.Children) {
				Context &Part = Contexts[Child];
				Current.IsWordTooLong |= Part.IsWordTooLong || Part.WordText.size() > WordLimit - Current.WordText.size();
				if(!Current.IsWordTooLong) {
					if(Current.WordText.empty()) Current.WordText = std::move(Part.WordText);
					else Current.WordText += Part.WordText;
				}
				if(!Current.IsAccessTooLong && !(Syntax.Is(Id, Role::AccessSection) && Syntax.Is(Child, Role::ColonToken))) {
					Current.IsAccessTooLong = Part.IsAccessTooLong;
					const std::string &Text = Part.AccessText;
					if(!Current.IsAccessTooLong && !Text.empty()) {
						const bool NeedsSpace = !Current.AccessText.empty() && IsWordChar(static_cast<unsigned char>(Current.AccessText.back())) &&
						IsWordChar(static_cast<unsigned char>(Text.front()));
						const size_t Remaining = AccessLimit - Current.AccessText.size();
						Current.IsAccessTooLong = Text.size() > Remaining || NeedsSpace && Text.size() == Remaining;
						if(!Current.IsAccessTooLong) {
							if(NeedsSpace) Current.AccessText += ' ';
							Current.AccessText += Text;
						}
					}
				}
			}
			if(!Current.IsWordTooLong) {
				if(!Node.Children.empty()) Node.Facts.Meanings = Profile.ReadWordMeanings(Current.WordText);
				const WordSet TypeMeanings = Node.Facts.Meanings.test(static_cast<size_t>(Word::QualifiedResultType)) ?
				Profile.ReadWordMeanings(std::string_view(Current.WordText).substr(Current.WordText.rfind('.') + 1)) :
				Node.Facts.Meanings;
				for(const Word Meaning : { Word::AsyncVoidType, Word::NeverType, Word::VoidType }) {
					if(TypeMeanings.test(static_cast<size_t>(Meaning))) Node.Facts.ResultMeanings.set(static_cast<size_t>(Meaning));
				}
			}
			if(
				!Node.Kind.IsNamed() ||
				Syntax.IsAny(Id, { Role::AccessModifier, Role::AccessSection, Role::MemberModifier, Role::MemberVisibilitySection })
			) Node.Facts.Access = Current.IsAccessTooLong ? AccessMeaning{ UINT64_MAX, 0 } : Profile.ReadAccessMeaning(Current.AccessText);
			const RoleSet &Roles = Node.Kind.Def->Roles;
			const auto Has = [&Roles](const Role Wanted) -> bool {
				// 境界を所有するノードの役割の返戻
				return Roles.test(static_cast<size_t>(Wanted));
			};
			const TSNode Src = SrcNode(Sources[Id], ValidationNodes, ExtraNodes);
			if(!IsVerification && Has(Role::MacroGuardedParens)) {
				const NodeId Child = Syntax.GetNamedChild(Id, 0);
				const bool IsChain = Syntax.Is(Child, Role::MacroGuardedParens) && Syntax.GetNamedChildCount(Child) == 1;
				ParenInner[Id] = IsChain ? ParenInner[Child] : Child;
				// 子の一致を継承し，一致が無ければ共有した使用位置を此の括弧の範囲で照会する
				HasParenExprMacro[Id] = IsChain && HasParenExprMacro[Child];
				if(!HasParenExprMacro[Id] && Sources[Id].NodeIdx != NoNode) {
					const std::vector<uint32_t>::const_iterator Use =
					std::lower_bound(ExprMacroUses.begin(), ExprMacroUses.end(), SrcTree.GetStart(Src));
					HasParenExprMacro[Id] = Use != ExprMacroUses.end() && *Use < SrcTree.GetEnd(Src);
				}
			}
			// 自身又は最初の非空の子のトークンの字を同じ後進走査で継承
			if(NeedsReadings) {
				uint16_t FirstChar = Node.Lexeme.empty() ? 0 : static_cast<uint16_t>(0X100U | static_cast<unsigned char>(Node.Lexeme.front())); // 上位ビットは値０の字とテキスト無しを区別する
				if(!FirstChar) for(const NodeId Child : Node.Children) {
					if(Gaps.Work) ++Gaps.Work->EdgeReads;
					if(FirstChars[Child]) {
						FirstChar = FirstChars[Child];
						break;
					}
				}
				FirstChars[Id] = FirstChar;
			}
			FirstLeaf[Id] = Node.Children.empty() ? Id : FirstLeaf[Node.Children.front()];
			if(!LastLeaf.empty()) {
				LastLeaf[Id] = Node.Lexeme.empty() ? NoNode : Id;
				if(LastLeaf[Id] == NoNode) {
					for(std::pmr::vector<NodeId>::const_reverse_iterator Child = Node.Children.crbegin(); Child != Node.Children.crend(); ++Child) {
						if(Work) Work->Charge(1);
						if(LastLeaf[*Child] == NoNode) continue;
						LastLeaf[Id] = LastLeaf[*Child];
						break;
					}
				}
			}
			OwnedEdges[Id] = Node.Children.empty() ?
			std::pair<uint32_t, uint32_t>(Sources[Id].Start, Sources[Id].End) :
			std::pair<uint32_t, uint32_t>(OwnedEdges[Node.Children.front()].first, OwnedEdges[Node.Children.back()].second);
			if(HasInterpolation) {
				IsInterpolated[Id] = !Node.Children.empty() && Has(Role::InterpolatedLiteral) || std::any_of(
					Node.Children.begin(),
					Node.Children.end(),
					[&Syntax](const NodeId Child) -> bool {
						// 補間の子かの返戻
						return Syntax.Is(Child, Role::StrInterpolation);
					}
				);
			}
			// 葉の値は入力の同じ走査で１度だけ確定し，全ての制御フローの照会へ渡す
			if(
				Contexts[Id].IsInLoopCond && Node.Children.empty() && !Contexts[Id].IsInErrorZone && !Contexts[Id].IsInMacroTokens &&
				!ContainsError(Id) && !(HasInterpolation && IsInterpolated[Id]) && (Has(Role::NumLiteral) || Has(Role::StrLikeAll))
			) Node.Facts.Truth = Profile.GetLiteralTruth(Node.Lexeme, Roles, Work);
			const bool IsLiteralContent = Contexts[Id].IsInQuotedText || HasInterpolation && IsInterpolated[Id] || Has(Role::StrLikeAll) ||
			Has(Role::InterpolationFormat);
			GapSources[Id] = IsLiteralContent ? PreservedSrc::LiteralContent : PreservedSrc::HiddenContent;
			// 葉とエラー領域は新しい頭尾の保持境界を持たない
			if(Node.Children.empty() || Sources[Id].NodeIdx == NoNode || Contexts[Id].IsInErrorZone || ContainsError(Id)) continue;
			for(
				const std::tuple<uint32_t, uint32_t, uint32_t> &Gap :
				{
					std::tuple<uint32_t, uint32_t, uint32_t>(
						0,
						HasFilePreamble && Node.Parent == NoNode && !Reading.EncodingBytePrefix.empty() ? 0 : Sources[Id].Start,
						OwnedEdges[Id].first
					),
					std::tuple<uint32_t, uint32_t, uint32_t>(static_cast<uint32_t>(Node.Children.size()), OwnedEdges[Id].second, Sources[Id].End)
				}
			) {
				const uint32_t Edge = std::get<0>(Gap), From = std::get<1>(Gap), To = std::get<2>(Gap);
				const bool IsByteHeadGap = HasFilePreamble && !Edge && Node.Parent == NoNode && !Reading.EncodingBytePrefix.empty() &&
				Syntax.GetNode(Node.Children.front()).Lexeme.starts_with(Reading.EncodingBytePrefix);
				if(From < To && IsByteHeadGap) {
					Node.Facts.Boundaries.Edit().push_back({ Edge, BoundaryKind::Preserved, " ", PreservedSrc::LiteralContent });
					OwnedEdges[Id].first = From;
					continue;
				}
				if(From >= To || !HasOpaqueHidden(SrcTree, Src, From, To, Gaps)) continue;
				Node.Facts.Boundaries.Edit().push_back({ Edge, BoundaryKind::Preserved, OriginalGap(From, To), GapSources[Id] });
				if(!Edge) OwnedEdges[Id].first = From;
				else OwnedEdges[Id].second = To;
			}
		}
		// ノード毎の構文事実の記録
		for(NodeId Id = 0; Id < Sources.size(); ++Id) {
			if(Work) Work->Charge(1);
			SyntaxNode &Node = Syntax.Nodes[Id];
			const TSNode Src = SrcNode(Sources[Id], ValidationNodes, ExtraNodes);
			// 隠れたトークンは読違も子の境界も持たない
			if(Sources[Id].NodeIdx == NoNode) continue;
			const RoleSet &SrcRoles = SrcTree.GetProfile().GetKindOf(Symbols[Id]).Roles;
			const RoleSet &Roles = Node.Kind.Def->Roles;
			const auto Has = [&Roles](const Role Wanted) -> bool {
				// 此のノードの確定済の役割の返戻
				return Roles.test(static_cast<size_t>(Wanted));
			};
			Node.Facts.DoesDiscardBinding =
			Syntax.IsWord(Id, Word::DiscardName) && (Has(Role::Identifier) || Syntax.GetBindingSyntax(Id) == BindingSyntax::Name);
			Node.Facts.CanReassignExistingBinding = !IsVerification && Has(Role::ReassigningShortDecl);
			Node.Facts.DoesAbsorbLeadingComment = !IsVerification && Reading.HasCommentAbsorbingDecls && SrcTree.CanAbsorbGoComment(Src);
			// 区切の無い実引数で除算と紛れる正規表現の開始トークンの記録
			if(!IsVerification && Has(Role::Regex)) {
				const std::string_view Text = SrcTree.GetView(Src);
				if(
					Text.size() > 1 && Profile.IsSpelling(Text.substr(0, 1), Word::RegexDelimiter) &&
					std::isspace(static_cast<unsigned char>(Text[1]))
				) Syntax.Nodes[FirstLeaf[Id]].Facts.Add(ConstraintKind::NeedsArgParens);
			}
			if(ContainsError(Id)) Node.Facts.Add(ConstraintKind::ContainsError);
			// フィールドを持たない中置演算の中央の子を，共通の演算子の役割へ投影する（行分割の候補は此の役割から作る）
			if((Has(Role::NamedInfix) || Has(Role::UnfieldedOperator)) && !ContainsError(Id) && Node.Children.size() == 3) {
				Syntax.Nodes[Node.Children[1]].Fields |= Syntax.GetFieldBit(ChildRole::Operator);
			}
			if(SrcRoles.test(static_cast<size_t>(Role::PotentialExprDecl)) && SrcTree.IsAmbiguousDecl(Src)) {
				Node.Facts.Add(ConstraintKind::AmbiguousDecl);
			}
			if(!IsVerification && (Contexts[Id].IsImportNamePart || Has(Role::SpacelessTypeName))) {
				Node.Facts.Add(ConstraintKind::NoOptionalBreak);
			}
			// 文へ展開するマクロの呼出の文
			if(
				!IsVerification && Reading.HasPreprocMacros && SrcRoles.test(static_cast<size_t>(Role::ExprStmt)) &&
				SrcTree.IsStmtMacroCall(Src)
			) Node.Facts.Add(ConstraintKind::StmtMacro);
			// 宣言の型の宣言子の記号へ展開するマクロ
			if(Reading.HasPreprocMacros && Node.Parent != NoNode && Syntax.Is(Node.Parent, Role::Decl)) {
				const std::vector<uint32_t>::const_iterator Use =
				std::lower_bound(DeclaratorMacroUses.begin(), DeclaratorMacroUses.end(), SrcTree.GetStart(Src));
				if(Use != DeclaratorMacroUses.end() && *Use < SrcTree.GetEnd(Src)) Node.Facts.Add(ConstraintKind::DeclaratorMacro);
			}
			// 後の原文をデータとして読む PHP の `__halt_compiler` の文（関数名の大小を区別しない，入力側の判定と同じ関数で決める）
			if(!IsVerification && Reading.HasHaltedData && ParseAdapter::IsHaltCall(SrcTree, Src)) Node.Facts.Add(ConstraintKind::EndsCode);
			// 前処理の条件のブロックを挟んで型名と読まれた `else`（処理系は前の `if` へ繋げる）
			if(
				!IsVerification && Rules.HasLineDirectives && Has(Role::Decl) && !Node.Children.empty() &&
				Syntax.IsWord(Node.Children.front(), Word::AlternateBranch)
			) {
				// `else` の前の文（条件のブロックは越え，ブロックの最初の文からはブロックの前の文へ遡る）
				NodeId Anchor = Id, Before = NoNode;
				while(Before == NoNode && Anchor != NoNode) {
					const NodeId Up = Syntax.Nodes[Anchor].Parent;
					Before = Syntax.GetPrevNamedSibling(Anchor);
					while(
						Before != NoNode && (
							Syntax.Is(Before, Role::PreprocBranch) || Syntax.Is(Up, Role::PreprocBranch) &&
							(Syntax.GetFieldChild(Up, ChildRole::Cond) == Before || Syntax.GetFieldChild(Up, ChildRole::Name) == Before)
						)
					) Before = Syntax.GetPrevNamedSibling(Before);
					Anchor = Syntax.Is(Up, Role::PreprocBranch) ? Up : NoNode;
				}
				// `else` の結付が波括弧の増減で変わる右端の文の並び
				if(Syntax.Is(Before, Role::If)) for(NodeId Edge = Before; Edge != NoNode; Edge = Syntax.GetLastNamedChild(Edge)) {
					Syntax.Nodes[Edge].Facts.Add(ConstraintKind::DetachedElse);
				}
			}
			if(NeedsReadings) {
				const ProjectionChecks &Checks = Profile.GetProjectionChecks(Symbols[Id]);
				if(Checks.HasTemplateContext) {
					if(Gaps.Work) ++Gaps.Work->ReadingQueries;
					const uint8_t TemplateReading = GetTemplateReading(TemplateMisreads, Src);
					Readings[Id].IsComparison = TemplateReading == MisreadTemplate;
					Readings[Id].IsLeadArgParen = TemplateReading == KeptLeadParen;
				}
				if(Checks.CanReadGenericCall) {
					if(Gaps.Work) ++Gaps.Work->ReadingQueries;
					if(IsMisparsedGenericCall(SrcTree, Src)) Readings[Id].IsCall = true;
					else {
						if(Gaps.Work) ++Gaps.Work->ReadingQueries;
						if(IsMisparsedGenericOpener(SrcTree, Src)) Readings[Id].IsCall = true;
					}
				}
				if(Has(Role::ProductAmbiguousPtrType) && IsCSharpProduct(SrcTree, Src)) Readings[Id].IsProduct = true;
				HasReadingSpaces = HasReadingSpaces || Readings[Id].IsCall || Readings[Id].IsComparison || Readings[Id].IsProduct;
				HasReadingCalls = HasReadingCalls || Readings[Id].IsCall;
			}
			if(
				Reading.HasPreprocMacros && (
					Has(Role::Identifier) || Has(Role::FieldName) || Has(Role::TypeName) || Has(Role::PrimitiveType) || Has(Role::BoolLiteral) ||
					!Node.Kind.IsNamed() && Syntax.IsWord(Id, Word::EntryResultType)
				) && SrcTree.HasMacroBinding(SrcTree.GetView(Src), Sources[Id].Start)
			) Node.Facts.Add(ConstraintKind::MacroBinding);
			// 呼出先の契約と，未読の定義が実引数の評価を変え得る事実を分離
			if(
				Reading.HasPreprocMacros && Has(Role::Identifier) && Syntax.Is(Node.Parent, Role::CallNode) &&
				Syntax.HasField(Id, ChildRole::Function) && SrcTree.IsMacroUncertain(SrcTree.GetView(Src), Sources[Id].Start)
			) Node.Facts.Add(ConstraintKind::UncertainMacroArguments);
			if(!IsVerification && Has(Role::MacroGuardedParens)) {
				const NodeId Parent = Node.Parent;
				const NodeId Inner = ParenInner[Id];
				const std::string_view Name =
				Syntax.Is(Inner, Role::Identifier) ? SrcTree.GetView(SrcNode(Sources[Inner], ValidationNodes, ExtraNodes)) : std::string_view();
				// 修飾付の呼出先も，末尾の名前の使用位置で関数形式マクロの展開を抑える括弧だけを保つ
				const bool IsGroupedReceiver =
				Inner != NoNode && Name.empty() && Syntax.Is(Parent, Role::CallExpr) && Syntax.HasField(Id, ChildRole::Function);
				if(Work && IsGroupedReceiver) Work->Charge(1);
				const NodeId ReceiverName = IsGroupedReceiver ? LastLeaf[Inner] : NoNode;
				const std::string_view SuppressedName = Syntax.Is(ReceiverName, Role::Identifier) || Syntax.Is(ReceiverName, Role::FieldName) ?
				Syntax.Nodes[ReceiverName].Lexeme :
				std::string_view();
				// 実引数の端に在る括弧の最も内の実引数の並び（端を離れれば無）
				NodeId Args = NoNode;
				for(NodeId Host = Parent; Host != NoNode && Args == NoNode; Host = Syntax.Nodes[Host].Parent) {
					if(Syntax.Is(Host, Role::ArgSeq)) Args = Host;
					else if(Sources[Host].Start != Sources[Id].Start && Sources[Host].End != Sources[Id].End) break;
				}
				const NodeId Call = Args == NoNode ? NoNode : Syntax.Nodes[Args].Parent;
				const NodeId Callee = Syntax.Is(Call, Role::CallExpr) ? Syntax.GetNamedChild(Call, 0) : NoNode;
				if(Work && Callee != NoNode) Work->Charge(1);
				const NodeId CalleeNameNode = Callee == NoNode ? NoNode : LastLeaf[Callee];
				const std::string_view CalleeName =
				Syntax.Is(CalleeNameNode, Role::Identifier) ? Syntax.Nodes[CalleeNameNode].Lexeme : std::string_view();
				// 関数形式又は未確定のマクロの呼出先では，実引数の端だけを保つ
				if(
					const bool IsMacroArgEdge = !CalleeName.empty() && (
						SrcTree.IsFunctionMacro(CalleeName, Sources[CalleeNameNode].Start) ||
						SrcTree.IsMacroUncertain(CalleeName, Sources[CalleeNameNode].Start)
					);
					IsMacroArgEdge || !Name.empty() && !SrcTree.IsNonMacroName(Name, Sources[Inner].Start) || !SuppressedName.empty() && (
						SrcTree.IsFunctionMacro(SuppressedName, Sources[ReceiverName].Start) ||
						SrcTree.IsMacroUncertain(SuppressedName, Sources[ReceiverName].Start)
					) || !Syntax.Is(Parent, Role::ReturningStmt) && HasParenExprMacro[Id]
				) Node.Facts.Add(ConstraintKind::MacroSensitiveParen);
			}
			// 制御構文の中でエラー領域の後に続くか，本文の境界を保つリテラルか
			bool IsAfterControlError = false;
			// マクロの本体と行のプリプロセッサディレクティブか
			const bool IsMacroBody = Has(Role::MacroBody), IsDirectiveLine = Has(Role::PreprocLine);
			const bool HasDisputedText = Has(Role::RenderedChildren) && SrcTree.HasDisputedJsxText(Src);
			for(uint32_t Idx = 1; Idx < Node.Children.size(); ++Idx) {
				const NodeId Prev = Node.Children[Idx - 1], Next = Node.Children[Idx];
				const RoleSet &PrevRoles = Syntax.Nodes[Prev].Kind.Def->Roles, &NextRoles = Syntax.Nodes[Next].Kind.Def->Roles;
				const auto HasPrevRole = [&PrevRoles](const Role Wanted) -> bool {
					// 前の子の確定済の役割の返戻
					return PrevRoles.test(static_cast<size_t>(Wanted));
				};
				const auto HasNextRole = [&NextRoles](const Role Wanted) -> bool {
					// 次の子の確定済の役割の返戻
					return NextRoles.test(static_cast<size_t>(Wanted));
				};
				// ディレクティブの語の後・マクロの本体の前・本体のトークンの間は空白１個，関数形式のマクロの名前と仮引数の並びは密着（トークン間空白仕様 5.4）
				if(IsMacroBody || IsDirectiveLine) {
					if(IsDirectiveLine && Syntax.GetFieldChild(Id, ChildRole::Params) == Next) {
						if(!IsVerification) Node.Facts.Boundaries.Edit().push_back({ Idx, BoundaryKind::NoInlineSpace });
						continue;
					}
					// ディレクティブの行にトークンとして残す行末コメント（解析用のマーカーで覆った物を除く）の前置区切は，原文のタブの数だけのタブか半角スペース１つ（整形仕様 7.1，7.6）
					if(
						const std::string_view Raw = std::string_view(SrcTree).substr(Sources[Next].Start, Sources[Next].End - Sources[Next].Start);
						(
							Raw.starts_with(CommentRepository::GetOpenerOf(CommentNotation::SlashLine)) ||
							Raw.starts_with(CommentRepository::GetOpenerOf(CommentNotation::Block))
						) && SrcTree.ClassifyComment(SrcTree.GetCommentSpans(), Sources[Next].Start, Sources[Next].End) == CommentKind::Trailing
					) {
						const size_t Tabs = TextEdit::SeparatorBefore(SrcTree, Reading, Sources[Next].Start).second;
						Node.Facts.Boundaries.Edit().push_back(
							{ Idx, BoundaryKind::Preserved, Tabs ? std::string(Tabs, '\t') : " ", PreservedSrc::MacroTokens }
						);
						continue;
					}
					if(IsMacroBody || Idx == 1 || Sources[Next].NodeIdx != NoNode && HasNextRole(Role::MacroBody)) {
						if(!IsVerification) Node.Facts.Boundaries.Edit().push_back({ Idx, BoundaryKind::RequiredSpace });
						continue;
					}
				}
				const uint32_t GapStart = OwnedEdges[Prev].second, GapEnd = OwnedEdges[Next].first;
				IsAfterControlError = IsAfterControlError || HasPrevRole(Role::Error) && Has(Role::Control);
				if(Reading.HasKeywordClosedBlocks && (HasPrevRole(Role::CommentNode) || HasNextRole(Role::CommentNode))) {
					Node.Facts.Boundaries.Edit().push_back(
						{ Idx, BoundaryKind::Preserved, OriginalGap(GapStart, GapEnd, HasSynthetic), PreservedSrc::LiteralContent }
					);
					continue;
				}
				// エラー領域・読めなかったディレクティブの行の中と其れに接する境界，エラーを含むノードの読み飛ばしたトークンを挟む境界，Go のエラーを含む子の後のトークンの前は原文の間隙を保つ
				if(
					Contexts[Id].IsInErrorZone || Contexts[Prev].IsInErrorZone || Contexts[Id].IsInBrokenDecl && !Has(Role::StmtHost) ||
					HasPrevRole(Role::Error) || HasNextRole(Role::Error) || IsAfterControlError ||
					ContainsError(Id) && OriginalGap(GapStart, GapEnd).find_first_not_of(" \t\r\n") != std::string::npos ||
					Rules.HasAutomaticLineTerminator && ContainsError(Prev) && !Syntax.Nodes[Next].Kind.IsNamed()
				) {
					Node.Facts.Boundaries.Edit().push_back(
						{
							Idx,
							BoundaryKind::Preserved,
							OriginalGap(GapStart, GapEnd),
							Contexts[Id].IsInErrorZone ? PreservedSrc::ErrorContent : PreservedSrc::ErrorBoundary
						}
					);
					continue;
				}
				// トークン列は空白の有無又は記号同士の密着だけを記録し，コメントもソース側では空白の区切として扱う
				if(
					!Contexts[Id].IsInQuotedText && (
						Node.Facts.TokenSpacing == TokenSpacingMode::Whitespace || Node.Facts.TokenSpacing == TokenSpacingMode::Tokens &&
						Syntax.Nodes[LastLeaf[Prev]].Facts.TokenSpacing == TokenSpacingMode::Whitespace &&
						Syntax.Nodes[FirstLeaf[Next]].Facts.TokenSpacing == TokenSpacingMode::Whitespace ||
						Node.Facts.TokenSpacing == TokenSpacingMode::Punctuation && Syntax.Is(LastLeaf[Prev], Role::JointPunctuationToken) &&
						Syntax.Is(FirstLeaf[Next], Role::JointPunctuationToken)
					)
				) {
					Node.Facts.Boundaries.Edit().push_back({ Idx, GapStart == GapEnd ? BoundaryKind::TokenGlued : BoundaryKind::TokenSpaced });
					continue;
				}
				// 文字列の直接の子に為る補間の区切の内側は，値の本文でなくコードの空白として扱う
				if(
					HasInterpolation && IsInterpolated[Id] && !Contexts[Id].IsInQuotedText && (
						HasPrevRole(Role::InterpolationOpenToken) || HasNextRole(Role::InterpolationCloseToken) ||
						HasPrevRole(Role::OpeningBraceToken) || HasNextRole(Role::ClosingBraceToken) || HasNextRole(Role::ClosingParenToken)
					)
				) {
					if(!IsVerification) {
						Node.Facts.Boundaries.Edit().push_back(
							{
								Idx,
								Reading.ShouldKeepInterpolationHead && HasPrevRole(Role::OpeningBraceToken) ?
								BoundaryKind::NoBreak :
								BoundaryKind::NoInlineSpace
							}
						);
					}
					continue;
				}
				// 埋込式を持つ文字列の本文の境界と C# の書式の後の境界（間隙が文字列の値）と，文法のトークンに無いテキストを挟む境界は，コメントを除いた原文の間隙を保つ
				// 宣言の終端との空の間隙は値の本文でない為，補間の本文の境界に含めない
				if(
					Contexts[Id].IsInQuotedText ||
					HasInterpolation && IsInterpolated[Id] && !(GapStart == GapEnd && Has(Role::TypedDecl) && Syntax.IsWord(Next, Word::StmtEnd)) ||
					HasPrevRole(Role::InterpolationFormat) || HasOpaqueHidden(SrcTree, Src, GapStart, GapEnd, Gaps)
				) {
					std::string Gap = OriginalGap(GapStart, GapEnd, HasSynthetic);
					std::optional<std::string> Stripped = CommentFreeGap(SrcTree, Separated, GapStart, GapEnd, CommentRanges);
					std::string Preserved;
					if(Stripped && *Stripped != Gap) Preserved = std::move(*Stripped);
					else if(HasSynthetic && !Stripped) {
						// 合成グループの境界では，挿入したトークンを除く前の間隙と比較する既存の契約
						Preserved = Separated.BuildSeparatedText(SrcTree, GapStart, GapEnd);
						if(Preserved == Gap) Preserved = std::move(Gap);
					// 取得済の間隙を再使用し，解析用に挿入したトークンが未除去の時だけ原文へ戻す
					} else Preserved = HasSynthetic || Inserted.empty() ? std::move(Gap) : OriginalGap(GapStart, GapEnd);
					Node.Facts.Boundaries.Edit().push_back(
						{
							Idx,
							BoundaryKind::Preserved,
							std::move(Preserved),
							GapSources[Id] == PreservedSrc::HiddenContent && HasPrevRole(Role::InterpolationFormat) ?
							PreservedSrc::LiteralContent :
							GapSources[Id]
						}
					);
					continue;
				}
				// 処理系で行端の読みが分かれる本文は，原文の子の境界だけを改行する
				if(HasDisputedText) {
					const std::string Gap = OriginalGap(GapStart, GapEnd, HasSynthetic);
					Node.Facts.Boundaries.Edit().push_back(
						{
							Idx,
							BoundaryKind::Preserved,
							std::string(static_cast<size_t>(std::count(Gap.begin(), Gap.end(), '\n')), '\n'),
							PreservedSrc::RenderedLineEdges
						}
					);
					continue;
				}
				// 値の無い宣言は，コロンと終端の間の原文の空白を値として残す
				if(Has(Role::TypedDecl) && Has(Role::StyleLexeme) && HasPrevRole(Role::ColonToken) && HasNextRole(Role::StmtTerminatorToken)) {
					if(
						!IsVerification &&
						std::string_view(SrcTree).substr(GapStart, GapEnd - GapStart).find_first_of(" \t\n\r\f") != std::string_view::npos
					) Node.Facts.Boundaries.Edit().push_back({ Idx, BoundaryKind::RequiredSpace });
					continue;
				}
				if(Has(Role::NameParts) || Has(Role::AnnotationParts) && !(HasPrevRole(Role::UserType) && HasNextRole(Role::UserType))) {
					// 名前とアノテーションの部品を同行で密着させる境界の記録
					if(!IsVerification) Node.Facts.Boundaries.Edit().push_back({ Idx, BoundaryKind::NoInlineSpace });
					continue;
				}
				if(
					Has(Role::MacroRuleDef) && (
						HasPrevRole(Role::MacroRulesToken) || Syntax.Nodes[Prev].Kind.IsNamed() && !Syntax.Nodes[Next].Kind.IsNamed() &&
						(HasNextRole(Role::OpenBrace) || HasNextRole(Role::OpeningParenToken) || HasNextRole(Role::OpeningBracketToken))
					)
				) {
					// 定義の語・名前・本体の開きの間に要る空白の投影
					if(!IsVerification) Node.Facts.Boundaries.Edit().push_back({ Idx, BoundaryKind::RequiredSpace });
					continue;
				}
				// マクロ規則の外側は通常の空白規則に委ね，内部のトークン列だけを保つ
				// 完成候補では区切記法の有無だけを照合し，配置用の密着・空白の分類は作らない
				if(Has(Role::MacroRuleDef) || IsVerification && !SeparatedBefore[FirstLeaf[Next]]) continue;
				if(Gaps.Work) ++Gaps.Work->AdjacencyQueries;
				if(!ShouldPreserveAdjacency(Syntax, Id, Next)) continue;
				// 区切記法を挟む境界と，空白無で接する境界と，其の他の境界の記録
				const BoundaryKind Kind = SeparatedBefore[FirstLeaf[Next]] ?
				BoundaryKind::Separated :
				OriginalGap(GapStart, GapEnd, HasSynthetic).empty() ? BoundaryKind::Glued : BoundaryKind::Spaced;
				Node.Facts.Boundaries.Edit().push_back({ Idx, Kind });
			}
			// 終端を後で補う値の無い宣言も，コロンの後の原文の空白を値として末尾の境界へ残す
			if(
				!IsVerification && Has(Role::TypedDecl) && Has(Role::StyleLexeme) && !Node.Children.empty() &&
				Syntax.IsTokenRole(Node.Children.back(), Role::ColonToken)
			) {
				const std::string_view Text = SrcTree;
				const uint32_t After = OwnedEdges[Node.Children.back()].second;
				if(After < Text.size() && std::string_view(" \t\n\r\f").find(Text[After]) != std::string_view::npos) {
					Node.Facts.Boundaries.Edit().push_back({ static_cast<uint32_t>(Node.Children.size()), BoundaryKind::RequiredSpace });
				}
			}
			// 子の番号順への両端と内部の境界の整列（組立時は二分探索）
			if(!Node.Facts.Boundaries.GetView().empty()) {
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
		// 完成候補は再正規化・再配置しない為，生成する空白・括弧と宣言統合の条件は入力側だけで求める
		if(!IsVerification) {
			// 空白又は実引数の読替を持たない入力では，空の判定結果で子の境界を走査し直さない
			if(HasReadingSpaces) ProjectReadings(Syntax, Reading, FirstChars, HasReadingCalls, Readings);
			RecordAngleParens(Syntax, Readings);
			if(Reading.HasShortVarDecls) RecordGoBindingFacts(Syntax);
		}
		// 終了
		return;
	}

	/**
	 * ドキュメントの表示境界の投影関数
	 * 計算量：ノード数 V，子の総数 E，読むトークンの長さ B に対し O(V + E + B)
	 * @param SrcTree 解析済のドキュメント
	 * @param Syntax 表示の空白と逐語内容を記録する構文
	 * @param Sources ノードの原文の出所
	 * @param HasFilePreamble ファイル先頭のバイト列として読むか
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::invalid_argument 呼出先へ渡す投影又は検証の引数が契約を満たさない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SourceProjector::RecordMarkupFacts(
		const ::ParsedTree &SrcTree,
		SyntaxModel &Syntax,
		const std::vector<Origin> &Sources,
		const bool HasFilePreamble
	) {
		// 親から継承する空白保持・名前空間・XML の契約の文脈
		std::vector<uint8_t> Traits(Syntax.GetNodeCount()), Preserved(Syntax.GetNodeCount()), Xml(Syntax.GetNodeCount());
		std::vector<GrammarProfile::MarkupContext> Contexts(Syntax.GetNodeCount(), GrammarProfile::MarkupContext::Html);
		std::vector<NodeId> Parents(Syntax.GetNodeCount(), NoNode);
		std::vector<uint32_t> Rebuilds(Syntax.GetNodeCount(), UINT32_MAX);
		RewritePlan Plan;
		const GrammarProfile &Profile = SrcTree.GetProfile();
		const ReadingRules &Reading = Profile.GetReading();
		const KindDef *const OpenTag = Profile.FindKindWithRole(Role::StartTag);
		const KindDef *const CloseToken = Profile.FindKindWithRole(Role::GreaterThanToken);
		const std::string_view CloseSpelling = Profile.GetSymbolName(Profile.FindSymbolWithRole(Role::GreaterThanToken)); // トークンのノード名は其のテキスト
		bool HasUnresolvedDisplay = false; // 内容境界へ作用する表示規則を確定出来ないか
		size_t EncodingPrefixEnd = 0; // 後続の宣言を先頭の読取範囲へ入れない為に保持する境界
		// 要素の特性と空白保持の継承
		for(NodeId Id = 0; Id < Syntax.GetNodeCount(); ++Id) {
			Syntax.Charge(1);
			if(const NodeId OriginalParent = Syntax.GetParentOf(Id); OriginalParent != NoNode) {
				// 空要素が取り込んだ本文は，開始タグの後の兄弟として本来の親の文脈を継承する
				const NodeId Parent =
				Traits[OriginalParent] & GrammarProfile::Void && !Xml[OriginalParent] && Syntax.GetChild(OriginalParent, 0) != Id ?
				Parents[OriginalParent] :
				OriginalParent;
				Parents[Id] = Parent;
				Preserved[Id] = Preserved[Parent];
				Xml[Id] = Xml[Parent];
				Contexts[Id] = Contexts[Parent];
				if(Parent != OriginalParent) for(const NodeId Host : { OriginalParent, Parent }) if(Rebuilds[Host] == UINT32_MAX) {
					Rebuilds[Host] = static_cast<uint32_t>(Plan.Steps.size());
					Plan.Steps.push_back({ RewriteKind::RebuildChildren, Host });
				}
			}
			if(!Syntax.Is(Id, Role::MarkupContainer) || !Syntax.GetChildCount(Id)) continue;
			const NodeId Tag = Syntax.GetChild(Id, 0), Name = Syntax.GetNamedChild(Tag, 0);
			std::string Lower = Name == NoNode ? std::string() : Syntax.GetText(Name);
			ToLowerAscii(Lower);
			const GrammarProfile::MarkupContext Context = GrammarProfile::GetMarkupContext(Contexts[Id], Lower);
			Traits[Id] = GrammarProfile::GetMarkupTraits(Context, Lower);
			std::string Encoding;
			bool HasEncoding = false;
			// 開始タグの後から終了タグの前迄の本文（終了タグを持たない要素は要素の終わり迄）
			const NodeId Last = Syntax.GetChild(Id, Syntax.GetChildCount(Id) - 1);
			const uint32_t ContentEnd = Last != Tag && Syntax.Is(Last, Role::MarkupClose) ? Sources[Last].Start : Sources[Id].End;
			HasUnresolvedDisplay = HasUnresolvedDisplay || GrammarProfile::HasMarkupDisplayRules(
				Context,
				Lower,
				std::string_view(SrcTree).substr(Sources[Tag].End, ContentEnd - Sources[Tag].End)
			);
			for(const NodeId Attribute : Syntax.GetChildrenOf(Tag)) {
				const NodeId AttributeName = Syntax.GetNamedChild(Attribute, 0);
				std::string Val = Syntax.GetText(Syntax.GetNamedChild(Attribute, 1));
				if(Val.size() > 1 && (Val.front() == '\'' || Val.front() == '"')) Val = Val.substr(1, Val.size() - 2);
				std::string NameText = Syntax.GetText(AttributeName);
				ToLowerAscii(NameText);
				if(
					HasFilePreamble && Reading.HasMarkupEncodingPrescan && Lower == Reading.EncodingElementName &&
					Sources[Tag].End > Reading.EncodingPrescanBytes &&
					(NameText == Reading.EncodingCharsetAttribute || NameText == Reading.EncodingContentAttribute)
				) EncodingPrefixEnd = Reading.EncodingPrescanBytes;
				if(!HasEncoding && NameText == Reading.MarkupEncodingAttribute) {
					HasEncoding = true;
					if(const std::optional<std::string> Decoded = GrammarProfile::DecodeMarkupAttribute(Val)) Encoding = *Decoded;
				}
				if(Profile.ShouldPreserveMarkupWhitespace(NameText, Val)) Traits[Id] |= GrammarProfile::Preserve;
				HasUnresolvedDisplay = HasUnresolvedDisplay || Profile.HasMarkupDisplayOverride(Context, Lower, NameText, Val);
				HasUnresolvedDisplay = HasUnresolvedDisplay || GrammarProfile::HasMarkupDisplayRef(Context, Lower, NameText, Val);
				if(Syntax.IsWord(AttributeName, Word::NamespaceAttribute) && Val == Reading.NativeNamespaceVal) Xml[Id] = true; // HTML 表示の儘
			}
			Contexts[Id] = GrammarProfile::GetMarkupChildContext(Context, Lower, Encoding);
			Preserved[Id] = Preserved[Id] || Traits[Id] & GrammarProfile::Preserve;
			// 空要素のタグの正規化
			if(
				Context == GrammarProfile::MarkupContext::Html && !Xml[Id] && Traits[Id] & GrammarProfile::Void &&
				Syntax.Is(Tag, Role::SelfClosingTag)
			) {
				Syntax.Nodes[Tag].Kind = { OpenTag };
				SyntaxNode &Close = Syntax.Nodes[Syntax.GetChild(Tag, Syntax.GetChildCount(Tag) - 1)];
				Close.Kind = { CloseToken };
				Close.Lexeme = CloseSpelling;
			}
		}
		// 原文順の１走査で移動元と移動先の子列を組み，全ての結付を同じ書換で復元する
		if(!Plan.Steps.empty()) {
			std::vector<NodeId> Pending { Syntax.GetRoot() };
			while(!Pending.empty()) {
				const NodeId Id = Pending.back();
				Pending.pop_back();
				Syntax.Charge(1);
				if(const NodeId Parent = Parents[Id]; Parent != NoNode && Rebuilds[Parent] != UINT32_MAX) {
					Plan.Steps[Rebuilds[Parent]].Inserted.Children.push_back(
						{ {}, false, {}, Parent == Syntax.GetParentOf(Id) ? Syntax.GetNode(Id).Fields : 0, Id }
					);
				}
				const std::span<const NodeId> Children = Syntax.GetChildrenOf(Id);
				Pending.insert(Pending.end(), Children.rbegin(), Children.rend());
			}
			Syntax.Apply(Plan);
		}
		// 未確定の表示規則の下では内容の間隙を値として保持し，タグ・属性・独立した所有領域は通常の工程で整形
		if(HasUnresolvedDisplay) for(NodeId Id = 0; Id < Syntax.GetNodeCount(); ++Id) if(!Syntax.Is(Id, Role::EmbeddedCodeElement)) {
			Preserved[Id] = 1;
		}
		// 暗黙に閉じる要素と空要素が末尾に取り込んだ空白を親の表示境界へ帰属し，空要素は元の閉じトークンの終わりを使う
		std::vector<uint32_t> Ends(Syntax.GetNodeCount());
		for(NodeId Id = Syntax.GetNodeCount(); Id--;) {
			const SyntaxNode &Node = Syntax.GetNode(Id);
			Ends[Id] = Sources[Id].End;
			if((!Preserved[Id] || Traits[Id] & GrammarProfile::Void) && Syntax.Is(Id, Role::MarkupContainer) && !Node.Children.empty()) {
				Ends[Id] = Ends[Node.Children.back()];
			}
		}
		// 子の境界を表示上の空白と構造上の区切へ変換
		for(NodeId Id = 0; Id < Syntax.GetNodeCount(); ++Id) {
			SyntaxNode &Node = Syntax.Nodes[Id];
			if(EncodingPrefixEnd && Sources[Id].Start < EncodingPrefixEnd) {
				// 先頭のバイト数を構成するトークンと内部の間隙を入力の事実として保つ
				Node.Facts.DoesPreserveQuoteForm = true;
				if(!Syntax.Is(Id, Role::MarkupContainer)) Node.Facts.Add(ConstraintKind::NoOptionalBreak);
				if(!Syntax.Is(Id, Role::MarkupContainer) && !Node.Children.empty()) {
					Node.Facts.Boundaries.Edit().clear();
					for(uint32_t Idx = 0; Idx <= Node.Children.size(); ++Idx) {
						const uint32_t From = Idx ? Sources[Node.Children[Idx - 1]].End : Sources[Id].Start;
						if(From >= EncodingPrefixEnd) break;
						const uint32_t To = Idx < Node.Children.size() ? Sources[Node.Children[Idx]].Start : Sources[Id].End;
						const std::string_view Gap =
						From < To ? std::string_view(SrcTree).substr(From, std::min<size_t>(To - From, EncodingPrefixEnd - From)) : std::string_view();
						Syntax.Charge(Gap.size() + 1);
						Node.Facts.Boundaries.Edit().push_back({ Idx, BoundaryKind::Preserved, std::string(Gap), PreservedSrc::LiteralContent });
					}
				}
			}
			if(!Syntax.Is(Id, Role::MarkupContainer)) continue;
			if(!Node.Facts.Boundaries.GetView().empty()) Node.Facts.Boundaries.Edit().clear();
			const bool IsRoot = Node.Parent == NoNode;
			std::optional<bool> IsHead;
			for(uint32_t Idx = (HasUnresolvedDisplay || EncodingPrefixEnd) && IsRoot ? 0 : 1; Idx <= Node.Children.size(); ++Idx) {
				const NodeId Left = Idx ? Node.Children[Idx - 1] : NoNode;
				const NodeId Right = Idx < Node.Children.size() ? Node.Children[Idx] : NoNode;
				const uint32_t From = Left == NoNode ? IsRoot && EncodingPrefixEnd ? 0 : Sources[Id].Start : Ends[Left];
				const uint32_t To = Right == NoNode ? Sources[Id].End : Sources[Right].Start;
				const std::string_view Gap = From < To ? std::string_view(SrcTree).substr(From, To - From) : std::string_view();
				if(
					Right == NoNode && From >= EncodingPrefixEnd &&
					(!Preserved[Id] || Traits[Id] & GrammarProfile::Void || !HasUnresolvedDisplay && IsRoot || Syntax.Is(Left, Role::MarkupClose))
				) continue;
				const bool IsVerbatimGap = Preserved[Id] || Gap.find_first_of("\v\f") != std::string_view::npos;
				if(From < EncodingPrefixEnd || IsVerbatimGap) {
					const std::string_view KeptGap = From < EncodingPrefixEnd && !IsVerbatimGap ? Gap.substr(0, EncodingPrefixEnd - From) : Gap;
					if(From < EncodingPrefixEnd) Syntax.Charge(KeptGap.size() + 1);
					Node.Facts.Boundaries.Edit().push_back({ Idx, BoundaryKind::Preserved, std::string(KeptGap), PreservedSrc::LiteralContent });
					continue;
				}
				const bool IsEdge = Syntax.Is(Left, Role::MarkupOpen) || Syntax.Is(Right, Role::MarkupClose);
				const bool HasText = Syntax.Is(Left, Role::MarkupText) || Syntax.Is(Right, Role::MarkupText);
				const bool IsBlock = Traits[Left] & GrammarProfile::Block || Right != NoNode && Traits[Right] & GrammarProfile::Block;
				const bool IsHidden = Syntax.Is(Left, Role::CommentNode) || Syntax.Is(Right, Role::CommentNode);
				// 同じ要素の見出は境界毎に本文を作り直さず，最初の必要時だけ照会する
				if(!IsHead) IsHead = Syntax.IsWord(Syntax.GetNamedChild(Syntax.GetChild(Id, 0), 0), Word::MetadataElement);
				BoundaryKind Boundary = BoundaryKind::NoBreak;
				if(
					IsRoot && (IsBlock || IsHidden || Syntax.Is(Left, Role::DocTypeDecl)) || !HasText && (IsBlock || *IsHead) && !IsEdge ||
					IsEdge && !(Traits[Id] & GrammarProfile::Void) && Traits[Id] & GrammarProfile::Block
				) Boundary = BoundaryKind::Separated;
				else if(IsEdge && !HasText && !Gap.empty() && Syntax.Is(Left, Role::MarkupOpen) && Syntax.Is(Right, Role::MarkupClose)) {
					Boundary = BoundaryKind::RequiredSpace;
				} else if(HasText || IsEdge && Traits[Id] & GrammarProfile::Void || Right == NoNode) {
					Boundary = Gap.empty() ? BoundaryKind::NoBreak : BoundaryKind::RequiredSpace;
				} else if(IsEdge) Boundary = BoundaryKind::Separated;
				else if(!IsHidden && !Gap.empty()) Boundary = BoundaryKind::Spaced;
				else if(IsHidden) {
					// コメントを越えた最寄の子（コメントだけが続く端はコンテナの端）
					uint32_t Prev = Idx - 1, Next = Idx;
					while(Prev && Syntax.Is(Node.Children[Prev], Role::CommentNode)) --Prev;
					while(Next + 1 < Node.Children.size() && Syntax.Is(Node.Children[Next], Role::CommentNode)) ++Next;
					// 行内の内容に接するか（ブロックでないコンテナの端は外の行内の内容へ続き得る）
					// 両側が行内の内容なら表示される空白の有無を保ち，ブロックの端・行を区切る要素に接する境界は空白に依らず表示されない区切とする（整形仕様 6.6）
					if(
						const auto IsInlineSide = [&](const NodeId Side) -> bool {
							// コンテナの端はコンテナがブロックでない事，其の他は行を区切る要素とコメントでない事の返戻
							return Syntax.Is(Side, Role::CommentNode) || Syntax.Is(Side, Role::MarkupOpen) || Syntax.Is(Side, Role::MarkupClose) ?
							!(Traits[Id] & GrammarProfile::Block) :
							!(Traits[Side] & GrammarProfile::Block);
						}; !IsInlineSide(Node.Children[Prev]) || !IsInlineSide(Node.Children[Next])
					) Boundary = BoundaryKind::Separated;
					else if(!Gap.empty()) Boundary = BoundaryKind::Spaced;
				}
				Node.Facts.Boundaries.Edit().push_back({ Idx, Boundary });
			}
		}
		// 終了
		return;
	}

	/**
	 * 子の所有領域と３経路の投影関数
	 * 所有領域を持つ原文ノードと投影済のアンカーとの対応は此の段だけで決める
	 * 計算量：投影元のノード数 V，所有領域数 C に対し O(V + C)，所有領域と経路が共に空なら O(1)
	 * @param SrcTree 解析済のドキュメント（子のコードだけの領域を持つ）
	 * @param Sources ノードの原文の出所
	 * @param ValidationNodes 検証データに含むパーサーノード
	 * @param ExtraNodes 検証データと異なる投影ノード
	 * @param Owned 子が渡したコード・配置・検証の独立した所有経路
	 * @param Result 構文・コメント・検証データを別々に所有する投影先
	 * @throws FormatLimitExceeded 所有経路が不完全，又は原文領域との対応が重複・欠落する場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	void SourceProjector::RecordOwnedRegions(
		const ::ParsedTree &SrcTree,
		const std::vector<Origin> &Sources,
		const ParsedNodes ValidationNodes,
		const std::span<const TSNode> ExtraNodes,
		const OwnedInputRoutes &Owned,
		ProjectionResult &Result
	) {
		// 埋込の所有領域と入力経路が共に無ければ，照合する対応は無い
		if(SrcTree.OwnedRegions.empty() && Owned.Children.empty()) return;
		// 所有コードを引く入力経路のインデックスと，ドキュメント順の一意性の確認
		std::unordered_map<const LaidOutCode *, const OwnedInputRoutes::Child *> Children;
		std::unordered_set<size_t> Ordinals;
		for(const OwnedInputRoutes::Child &Child : Owned.Children) {
			if(
				!Child.Profile || !Child.Code || !Child.Syntax || !Child.Comments || !Child.Ctx ||
				!Children.emplace(Child.Code.get(), &Child).second || !Ordinals.insert(Child.Ordinal).second
			) throw FormatLimitExceeded("embedded input ownership is incomplete", FailureReason::FormatterDefect);
		}
		// 独立したコードとして整形した埋込の所有領域と，配置・検証だけの経路の対応
		SyntaxModel &Syntax = Result.Syntax;
		const NodeId Count = static_cast<NodeId>(Sources.size());
		for(NodeId Id = 0; Id < Count; ++Id) {
			std::shared_ptr<const OwnedRegion> Region =
			Syntax.Is(Id, Role::RawTextNode) ? SrcTree.GetOwnedRegionOf(SrcNode(Sources[Id], ValidationNodes, ExtraNodes)) : nullptr;
			if(!Region) continue;
			const std::unordered_map<const LaidOutCode *, const OwnedInputRoutes::Child *>::iterator Found =
			Children.find(Region->Code.get());
			if(Found == Children.end()) throw FormatLimitExceeded("embedded code has no ownership route", FailureReason::FormatterDefect);
			const OwnedInputRoutes::Child &Child = *Found->second;
			// 子のコードの共有と零幅の所有境界の保持（コードの無い領域も境界を保持）
			Syntax.Nodes[Id].Lexeme.clear();
			Syntax.Regions.push_back(std::move(Region));
			Syntax.Nodes[Id].Facts.RegionIdx = static_cast<uint32_t>(Syntax.Regions.size());
			Result.Comments.OwnedChildren.emplace(Id, Child.Comments);
			Result.Validation.OwnedChildren.push_back(
				{ Child.Ordinal, Child.Profile, Child.Code, Child.Syntax, Child.Comments, Child.Ctx }
			);
			Children.erase(Found);
		}
		if(!Children.empty()) throw FormatLimitExceeded("embedded input has no projected owner", FailureReason::FormatterDefect);
		// 終了
		return;
	}

	/**
	 * 閉括弧の内側へ読まれた逐語本文の結付の復元関数
	 * 本文から最初の閉トークン迄に別のトークンが無ければ，閉じの後にトークンが続いても開始を含む文（語で閉じるブロックの見出の中の物は其の見出の部品）の後へ移す
	 * 同じ文の中に移せない本文が在れば，其の本文が括弧の中を複数行にする為に，其の文の本文は移さない
	 * 計算量：ノード数 V，本文数 B，調べる部分木と経路の大きさの和 A，移動先の子の最大数 C に対し O(V + B log B + A + BC)
	 * @param Syntax 投影した構文
	 * @param Sources ノードの原文の出所
	 * @throws FormatLimitExceeded 本文の移動が作業・構造の上限を超えるか，書換の生成形式・前提が不正な場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SourceProjector::RebindDeferredLiteralBodies(SyntaxModel &Syntax, const std::vector<Origin> &Sources) {
		// 括弧の中の本文と，其の開始を含む文と文の並び，文の後へ移せるか
		struct Candidate {
			NodeId Body;
			NodeId Stmt;
			NodeId Sequence;
			bool IsMovable; // 此の逐語本文が，閉じ迄のトークンの条件を満たすか
		};
		// 逐語本文の原文順の収集
		std::vector<NodeId> Bodies;
		for(NodeId Id = 0; Id < Syntax.GetNodeCount(); ++Id) if(Syntax.Is(Id, Role::DeferredLiteralBody)) Bodies.push_back(Id);
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
			const NodeId Brackets = Syntax.GetParentOf(Body);
			if(
				Brackets == NoNode || Syntax.Is(Brackets, Role::StmtSeq) || Syntax.Is(Brackets, Role::StmtHost) || !(
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
				for(const NodeId Child : Syntax.GetChildrenOf(Part)) Pending.push_back(Child);
			}
			NodeId Within = Body, Stmt = NoNode, Sequence = NoNode;
			bool HasClearCloser = HasStart, HasCloser = false;
			for(NodeId Parent = Brackets; Parent != NoNode; Parent = Syntax.GetParentOf(Parent)) {
				// 文の並びの中の文の後，又は語で閉じるブロックの見出の部品の後（閉括弧を同じ行に書いた時にパーサーが置く所）
				if(Syntax.Is(Parent, Role::StmtSeq) || Syntax.Is(Parent, Role::StmtHost) || Syntax.Is(Parent, Role::KeywordBlock)) {
					Stmt = Within;
					Sequence = Parent;
					break;
				}
				const std::span<const NodeId> Children = Syntax.GetChildrenOf(Parent);
				// 最初の閉じ迄に別のトークンが無いかを確かめ，閉じより後の式のトークンは許す
				for(uint32_t Idx = Syntax.GetNode(Within).Idx + 1; HasClearCloser && !HasCloser && Idx < Children.size(); ++Idx) {
					const NodeId After = Children[Idx];
					const bool IsCloser =
					!Syntax.IsNamed(After) && Syntax.IsTokenRole(After, Role::ClosingBracketLead) && Syntax.IsTokenRole(After, Role::CloserToken);
					HasClearCloser = Syntax.Is(After, Role::DeferredLiteralBody) || IsCloser;
					HasCloser = HasCloser || IsCloser;
				}
				Within = Parent;
			}
			if(Sequence == NoNode) continue;
			Candidates.push_back({ Body, Stmt, Sequence, HasClearCloser && HasCloser });
			if(!Candidates.back().IsMovable) Blocked.insert(Stmt);
		}
		// 移動可能な本文の結付変更
		for(const Candidate &Item : Candidates) {
			const NodeId Stmt = Item.Stmt, Sequence = Item.Sequence;
			if(!Item.IsMovable || Blocked.contains(Stmt)) continue;
			uint32_t Idx = Syntax.GetNode(Stmt).Idx + 1;
			while(Idx < Syntax.GetChildCount(Sequence) && Syntax.Is(Syntax.GetChild(Sequence, Idx), Role::DeferredLiteralBody)) ++Idx;
			RewriteStep Move { RewriteKind::Move, Item.Body };
			Move.Dst = Sequence;
			Move.Idx = Idx;
			Move.DstFields = 0;
			RewritePlan Plan;
			Plan.Steps.push_back(std::move(Move));
			Syntax.Apply(Plan);
		}
		// 終了
		return;
	}

	/**
	 * 閉タグと枝の所属に関するパーサーの修復関数
	 * 閉タグが終える空の本体を明示し，後続の地の文と文を外へ戻す
	 * 開いた条件分岐に続く枝は最も近い未完の分岐へ結ぶ
	 * 計算量：ノードの訪問数 N，子と祖先の総走査量 A，書換の総費用 R に対し O(N + A + R)
	 * @param Syntax 投影した構文
	 * @param Reading 入力の読取規則
	 * @param IsVerification 修復で補うノードに整形専用の事実を渡さないか
	 * @throws FormatLimitExceeded 枝・本体の書換が作業・構造の上限を超えるか，生成形式・前提が不正な場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SourceProjector::RebindEmbeddedBranches(SyntaxModel &Syntax, const ReadingRules &Reading, const bool IsVerification) {
		// 閉タグで終わる本体の明示
		const uint64_t BodyField = uint64_t{ 1 } << static_cast<uint8_t>(ChildRole::Body);
		for(NodeId Id = 0; Id < Syntax.GetNodeCount(); ++Id) {
			if(
				!Syntax.Is(Id, Role::BodyOwner) && !Syntax.Is(Id, Role::If) && !Syntax.Is(Id, Role::ElseIfClause) ||
				Syntax.HasTokenRoleChild(Id, Role::ColonToken)
			) continue;
			// フィールドを持たない単一文本体は，後に終端のトークンが無い最後の文から補完
			NodeId Body = Syntax.GetFieldChild(Id, ChildRole::Body);
			if(Body == NoNode) {
				Body = Syntax.GetLastNamedChild(Id);
				if(Body == NoNode || Syntax.GetChild(Id, Syntax.GetChildCount(Id) - 1) != Body) continue;
				Syntax.Nodes[Body].Fields |= BodyField;
			}
			NodeId Boundary = NoNode;
			for(const NodeId Child : Syntax.GetChildrenOf(Id)) {
				if(Child == Body) break;
				if(Syntax.Is(Child, Role::CodeBoundary)) {
					Boundary = Child;
					break;
				}
			}
			if(Boundary == NoNode || Body == NoNode) continue;
			NodeId Owner = Id;
			while(Syntax.Is(Owner, Role::ElseClause) || Syntax.Is(Owner, Role::ElseIfClause) || Syntax.HasField(Owner, ChildRole::Body)) {
				Owner = Syntax.GetParentOf(Owner);
			}
			const NodeId Parent = Syntax.GetParentOf(Owner);
			if(Parent == NoNode) continue;
			// 本体の移動と空の文の挿入
			RewritePlan Plan;
			for(const NodeId Child : { Boundary, Body }) {
				RewriteStep Move { RewriteKind::Move, Child };
				Move.Dst = Parent;
				Move.Idx = Syntax.GetNode(Owner).Idx + (Child == Boundary ? 1 : 2);
				Move.DstFields = 0;
				Plan.Steps.push_back(std::move(Move));
			}
			RewriteStep Empty { RewriteKind::Insert, Id };
			Empty.Dst = Id;
			Empty.Idx = Syntax.GetNode(Boundary).Idx;
			Empty.Inserted =
			{ {}, true, { { std::string(Syntax.GetRules().GetSpelling(Word::StmtEnd)) } }, BodyField, NoNode, {}, Reading.EmptyStmtDef };
			// 修復用の生成ノードにも，整形の為の形式と空行比較キーを持ち込まない
			if(IsVerification) for(GeneratedNode *const Part : { &Empty.Inserted, &Empty.Inserted.Children.front() }) {
				Part->Def = Syntax.GetDefFor(*Part);
				Part->Def.Forms = CanonicalFormList();
				Part->Def.BlankKey = 0;
			}
			Plan.Steps.push_back(std::move(Empty));
			Syntax.Apply(Plan);
		}
		// 最も内側の開いた枝（明示したブロックと終端を持つ構文を境界とし，単一文本体の連鎖を内へ辿る，整形仕様 5.5）
		const auto FindOpen = [&Syntax](NodeId Id) -> NodeId {
			NodeId Open = NoNode;
			while(Syntax.Is(Id, Role::BodyOwner) || Syntax.Is(Id, Role::ElseIfClause)) {
				// 後判定の終端と代替構文の終端を越えない為の中断
				if(
					Syntax.Is(Id, Role::DoLoop) || Syntax.HasTokenRoleChild(Id, Role::ColonToken) ||
					Syntax.HasTokenRoleChild(Id, Role::AlternativeBodyCloser) ||
					Syntax.Is(Syntax.GetFieldChild(Id, ChildRole::Body), Role::DelimitedBody)
				) break;
				NodeId LastBranch = NoNode;
				if(Syntax.Is(Id, Role::If) || Syntax.Is(Id, Role::ElseIfClause)) {
					for(const NodeId Child : Syntax.GetChildrenOf(Id)) {
						if(Syntax.Is(Child, Role::ElseClause) || Syntax.Is(Child, Role::ElseIfClause)) LastBranch = Child;
					}
					// 最後の else を持たない分岐
					if(!Syntax.Is(LastBranch, Role::ElseClause)) Open = Id;
				}
				Id = Syntax.GetFieldChild(LastBranch == NoNode ? Id : LastBranch, ChildRole::Body);
			}
			// 最も内側の開いた分岐の返戻
			return Open;
		};
		// 分岐を後行順に並べ，内側の枝を先に確定する
		// 外側の枝は確定済の内側の構造から結付先を求める為，各分岐を１度だけ処理する
		std::vector<NodeId> Ifs;
		for(std::vector<std::pair<NodeId, bool>> Pending { { Syntax.GetRoot(), false } }; !Pending.empty();) {
			const std::pair<NodeId, bool> Cur = Pending.back();
			Pending.pop_back();
			if(Cur.second) {
				if(Syntax.Is(Cur.first, Role::If)) Ifs.push_back(Cur.first);
				continue;
			}
			Pending.emplace_back(Cur.first, true);
			for(const NodeId Child : Syntax.GetChildrenOf(Cur.first)) Pending.emplace_back(Child, false);
		}
		// 後続枝の未完の分岐への結付
		for(const NodeId Id : Ifs) {
			std::vector<NodeId> Branches;
			for(const NodeId Child : Syntax.GetChildrenOf(Id)) {
				if(Syntax.Is(Child, Role::ElseClause) || Syntax.Is(Child, Role::ElseIfClause)) Branches.push_back(Child);
			}
			for(const NodeId Branch : Branches) {
				const NodeId Target = FindOpen(Syntax.GetFieldChild(Id, ChildRole::Body));
				if(Target == NoNode) break;
				RewriteStep Move { RewriteKind::Move, Branch };
				Move.Dst = Target;
				Move.Idx = Syntax.GetChildCount(Target);
				RewritePlan Plan;
				Plan.Steps.push_back(std::move(Move));
				Syntax.Apply(Plan);
			}
		}
		// 終了
		return;
	}

	/**
	 * 二項演算子の読違を文法の優先順位で修復する関数
	 * 明示した括弧や登録外の式を越えず，トークンの順を保って親子関係と位置を結び直す
	 * 計算量：ノード数 N と登録した連鎖の長さの和 L に対し O(N + L)
	 * @param Syntax 投影した構文モデル
	 * @param Reading 読違を修復する演算子の登録
	 * @param Sources ノードの出所と修復後の範囲の格納先
	 * @throws FormatLimitExceeded 構文の走査が作業予算を超える場合
	 * @throws std::bad_alloc 連鎖の作業列を確保出来ない場合
	 * @throws std::length_error 作業列の要素数上限を超える場合
	 */
	void SourceProjector::RebindBinaryOperators(SyntaxModel &Syntax, const ReadingRules &Reading, std::vector<Origin> &Sources) {
		// 登録の無い文法の終了
		if(Reading.MisboundBinaryOperators.empty()) return;
		const SyntaxRules &Rules = Syntax.GetRules();
		// エラーの無い登録済の二項式の優先順位の取得
		// @param Id 調べるノード
		// @return 登録した演算子の優先順位，対象外なら０
		// @throws FormatLimitExceeded 照会の作業予算を超える場合
		const auto GetPrec = [&](const NodeId Id) -> int {
			// 根の外側は対象外として終了
			if(Id == NoNode) return 0;
			Syntax.Charge(1);
			const SyntaxNode &Node = Syntax.Nodes[Id];
			// 正常な二項式だけを対象にする
			if(!Syntax.Is(Id, Role::BinaryOperator) || Node.Children.size() != 3 || Node.Facts.Has(ConstraintKind::ContainsError)) return 0;
			const std::string_view Operator = Syntax.GetLexemeOf(Node.Children[1]);
			// 読違の登録が無い演算子では連鎖を区切る
			if(!Reading.MisboundBinaryOperators.contains(Operator)) return 0;
			const std::unordered_map<std::string_view, int>::const_iterator Found = Rules.OperatorPrec.find(Operator);
			// 既存の優先順位表を参照し，未登録の結合を推測しない
			return Found == Rules.OperatorPrec.end() ? 0 : Found->second;
		};
		// 組替前に連鎖の根を確定し，組替後の親を辿って同じ連鎖を集め直さない
		std::vector<NodeId> Roots;
		for(NodeId Id = 0; Id < Syntax.GetNodeCount(); ++Id) if(GetPrec(Id) && !GetPrec(Syntax.Nodes[Id].Parent)) Roots.push_back(Id);
		struct Part {
			NodeId Id; // 元のノード
			int Prec; // 演算子の優先順位（被演算子は０）
			uint64_t LeftFields; // 左の被演算子へ渡すフィールド
			uint64_t RightFields; // 右の被演算子へ渡すフィールド
		}; // 連鎖の演算子又は分割しない被演算子
		std::vector<Part> Parts, Operators;
		std::vector<NodeId> Values;
		std::vector<std::pair<NodeId, bool>> Stack;
		for(const NodeId Root : Roots) {
			const NodeId Parent = Syntax.Nodes[Root].Parent;
			const uint32_t Idx = Syntax.Nodes[Root].Idx;
			const uint64_t Fields = Syntax.Nodes[Root].Fields;
			Parts.clear();
			Operators.clear();
			Values.clear();
			Stack.emplace_back(Root, false);
			// 元の子列を全て読み終えてから書き換え，作業列への参照を追加の間に保持しない
			while(!Stack.empty()) {
				const auto [Id, IsOperator] = Stack.back();
				Stack.pop_back();
				const int Prec = GetPrec(Id);
				const SyntaxNode &Node = Syntax.Nodes[Id];
				if(!Prec) Parts.push_back({ Id, 0, 0, 0 });
				else if(IsOperator) Parts.push_back({ Id, Prec, Syntax.Nodes[Node.Children[0]].Fields, Syntax.Nodes[Node.Children[2]].Fields });
				else {
					Stack.emplace_back(Node.Children[2], false);
					Stack.emplace_back(Id, true);
					Stack.emplace_back(Node.Children[0], false);
				}
			}
			// 優先順位の確定した演算子へ左右を接続し，部分式を次の被演算子とする処理
			// @throws FormatLimitExceeded 組替の作業予算を超える場合
			const auto Reduce = [&]() -> void {
				Syntax.Charge(1);
				const Part Operation = Operators.back();
				Operators.pop_back();
				const NodeId Right = Values.back();
				Values.pop_back();
				const NodeId Left = Values.back();
				SyntaxNode &Node = Syntax.Nodes[Operation.Id];
				Node.Children[0] = Left;
				Node.Children[2] = Right;
				Syntax.Nodes[Left].Parent = Syntax.Nodes[Right].Parent = Operation.Id;
				Syntax.Nodes[Left].Idx = 0;
				Syntax.Nodes[Right].Idx = 2;
				Syntax.Nodes[Left].Fields = Operation.LeftFields;
				Syntax.Nodes[Right].Fields = Operation.RightFields;
				Sources[Operation.Id].Start = Sources[Left].Start;
				Sources[Operation.Id].End = Sources[Right].End;
				Values.back() = Operation.Id;
				// 終了
				return;
			};
			for(const Part &Current : Parts) {
				Syntax.Charge(1);
				if(!Current.Prec) {
					Values.push_back(Current.Id);
					continue;
				}
				while(!Operators.empty()) {
					const int Prec = Operators.back().Prec;
					if(Prec < Current.Prec || Prec == Current.Prec && Rules.RightAssocPrecs.contains(Prec)) break;
					Reduce();
				}
				Operators.push_back(Current);
			}
			while(!Operators.empty()) Reduce();
			// 連鎖の外側の所属を新しい根へ移し，元のノード識別子と演算子トークンを再使用する
			const NodeId Rebound = Values.back();
			Syntax.Nodes[Rebound].Parent = Parent;
			Syntax.Nodes[Rebound].Idx = Idx;
			Syntax.Nodes[Rebound].Fields = Fields;
			if(Parent == NoNode) Syntax.Root = Rebound;
			else Syntax.Nodes[Parent].Children[Idx] = Rebound;
		}
		// 終了
		return;
	}

	/**
	 * Swift の後置の式の結付の修復関数（整形仕様 4.2，4.3）
	 * tree-sitter-swift は，式の後の呼出・添字・後置の演算子を，加減・乗除・前置の式全体へ結び付ける
	 * `a + g()` を `(a + g)()` と読む誤りを，後置の式を最も右の被演算子へ結び直して正す
	 * トークンの順は保ち，ノードの親子関係とフィールドだけを組み替える
	 * 計算量：ノード数 N と組替の回数 K に対し O(N + K)
	 * @param Syntax 構文事実を記録した構文モデル
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	void SourceProjector::RebindSwiftPostfix(SyntaxModel &Syntax) {
		// 子を親より先に並べたノードの順（内側の組替の後に外側を組み替える）
		std::vector<NodeId> Order, Stack { Syntax.Root };
		while(!Stack.empty()) {
			const NodeId Id = Stack.back();
			Stack.pop_back();
			Order.push_back(Id);
			Stack.insert(Stack.end(), Syntax.Nodes[Id].Children.begin(), Syntax.Nodes[Id].Children.end());
		}
		// 後置の式を最も右の被演算子へ結び直す処理
		for(std::vector<NodeId>::reverse_iterator Post = Order.rbegin(); Post != Order.rend(); ++Post) {
			while(Syntax.Is(*Post, Role::PostfixBindingExpr) && !Syntax.Nodes[*Post].Children.empty()) {
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
				const uint32_t Idx = Postfix.Idx, LastIdx = Last.Idx;
				// 被演算子の式の後置の式の位置への移動
				Operand.Parent = Parent;
				Operand.Idx = Idx;
				Operand.Fields = PostfixFields;
				if(Parent == NoNode) Syntax.Root = Left;
				else Syntax.Nodes[Parent].Children[Idx] = Left;
				// 後置の式の最も右の被演算子の位置への移動
				Operand.Children[LastIdx] = *Post;
				Postfix.Parent = Left;
				Postfix.Idx = LastIdx;
				Postfix.Fields = LastFields;
				// 最も右の被演算子の後置の式の先頭への接続
				Postfix.Children.front() = Right;
				Last.Parent = *Post;
				Last.Idx = 0;
				Last.Fields = OperandFields;
			}
		}
		// 終了
		return;
	}

	/**
	 * 関数インポートの対象と名前空間の領域の投影関数
	 * 計算量：ノード数 N に対し O(N)
	 * @param Syntax 投影と構造修復を終えた構文
	 * @param NeedsBindings 呼出の解決に使う名前空間の所属も記録するか（パスのフィールドは常に記録する）
	 * @throws FormatLimitExceeded 構文の走査が作業予算を超える場合
	 * @throws std::bad_alloc 作業列のメモリを確保出来ない場合
	 * @throws std::length_error 作業列の要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード又は事実のキーが範囲外の場合
	 */
	void SourceProjector::RecordImportFields(SyntaxModel &Syntax, const bool NeedsBindings) {
		// 名前空間を分けない文法の終了
		if(!Syntax.GetRules().HasNamespaceFunctionImports) return;
		// セミコロン形式の名前空間は後続の兄弟を，ブロック形式は自身の子だけを所有する
		NodeId Scope = NeedsBindings ? Syntax.GetRoot() : NoNode;
		std::vector<std::pair<NodeId, NodeId>> Pending;
		for(const NodeId Child : Syntax.GetChildrenOf(Syntax.GetRoot())) {
			Syntax.Charge(1);
			const bool IsNamespace = NeedsBindings && Syntax.Is(Child, Role::NamespaceDef);
			const NodeId Body = IsNamespace ? Syntax.GetFieldChild(Child, ChildRole::Body) : NoNode;
			if(IsNamespace && Body == NoNode) Scope = Child;
			Pending.emplace_back(Child, IsNamespace ? Child : Scope);
		}
		while(!Pending.empty()) {
			const std::pair<NodeId, NodeId> Cur = Pending.back();
			Pending.pop_back();
			Syntax.Charge(1);
			if(NeedsBindings) Syntax.Nodes[Cur.first].Facts.ImportScope = Cur.second;
			const bool IsItem = Syntax.Is(Cur.first, Role::ImportPathItem), IsGroup = Syntax.Is(Cur.first, Role::ImportPathGroup);
			bool HasPath = false;
			for(const NodeId Child : Syntax.GetChildrenOf(Cur.first)) {
				Syntax.Charge(1);
				if(
					!HasPath && (IsItem || IsGroup) && Syntax.IsNamed(Child) && !Syntax.HasField(Child, ChildRole::Alias) &&
					!Syntax.Is(Child, Role::ImportPathItem) && !Syntax.Is(Child, Role::ImportPathGroup)
				) {
					Syntax.Nodes[Child].Fields |= Syntax.GetFieldBit(ChildRole::Path);
					HasPath = true;
				}
				Pending.emplace_back(Child, Cur.second);
			}
		}
		// 終了
		return;
	}

	/**
	 * 比較名と必要な綴りの事実の記録関数
	 * 計算量：ノード数 N，名前の総バイト数 B，NFKC の展開後のコードポイント数 M に対し O(N + B + M log M)
	 * @param Syntax 投影済の構文
	 * @param Reading 名前の読取規則
	 * @param NeedsSpelling 入力の宣言正規化用の綴りも記録するか
	 * @param NeedsBindings 子の束縛フィールドを同じ訪問で補うか
	 * @throws std::bad_alloc 名前のメモリを確保出来ない場合
	 * @throws std::length_error 名前の表の上限を超える場合
	 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
	 */
	void SourceProjector::RecordNames(
		SyntaxModel &Syntax,
		const ReadingRules &Reading,
		const bool NeedsSpelling,
		const bool NeedsBindings
	) {
		// 束縛のフィールドを全て補い，追加された親が既存の名前へ与える役割も確定する
		if(NeedsBindings && Syntax.GetRules().HasBindingIdentity) for(NodeId Id = 0; Id < Syntax.GetNodeCount(); ++Id) {
			RecordBindingFields(Syntax, Id);
		}
		// 比較する名前の役割だけを収集し，名前の場に在る複合式・型の接尾部分木は対象にしない
		// 宣言・インポートの完全名は，導入名と並替の照会が直接読む位置だけで１度記録する
		for(NodeId Id = 0; Id < Syntax.GetNodeCount(); ++Id) {
			Syntax.Charge(1);
			const bool IsLeaf = Syntax.Nodes[Id].Children.empty();
			if(!Syntax.IsComparisonName(Id)) continue;
			// 葉のテキストは既に所有して居る為，名前を変換する前の全体コピーを作らない
			const std::string Joined = IsLeaf ? std::string() : Syntax.GetText(Id);
			const std::string_view Spelling = IsLeaf ? std::string_view(Syntax.Nodes[Id].Lexeme) : std::string_view(Joined);
			if(IsLeaf) Syntax.Charge(1 + Spelling.size());
			if(NeedsSpelling && IsLeaf) Syntax.Nodes[Id].Facts.Spelling = Syntax.ReadNameSpelling(Spelling);
			std::string Name = GetCmpNameOf(Reading, Spelling, Syntax.Is(Id, Role::Label));
			const NodeId Parent = Syntax.GetParentOf(Id);
			if(
				Syntax.GetRules().AreFunctionNamesCaseInsensitive && Syntax.Is(Id, Role::FunctionNameIdentifier) && (
					Syntax.Is(Parent, Role::FunctionScope) && Syntax.HasField(Id, ChildRole::Name) ||
					Syntax.Is(Parent, Role::CallNode) && (Syntax.HasField(Id, ChildRole::Function) || Syntax.GetNamedChild(Parent, 0) == Id)
				)
			) {
				Syntax.Charge(Name.size());
				ToLowerAscii(Name);
			}
			if(Name.empty()) continue;
			Syntax.Nodes[Id].Facts.NameIdx = Syntax.InternCmpName(std::move(Name), IsLeaf && Syntax.Is(Id, Role::IdentifierLeafLike));
		}
		// 終了
		return;
	}

	/**
	 * 束縛を確認した標準対象のメンバの効果を投影する関数
	 * 未確認の呼出・デコレータから宣言の役割を作らず，解決の事実を後段で共有する
	 * 計算量：構文数 N，対象の参照構造 S と束縛の解決費用 Q に対し O(N + S + Q)
	 * @param Profile 標準対象と効果の文法の定義
	 * @param Syntax 比較名と束縛フィールドを持つ投影済の構文
	 * @param ForModifiers 静的解析用の修飾か，ドキュメント用の宣言呼出か
	 * @throws FormatLimitExceeded 構文又は束縛の照会が作業予算を超える場合
	 * @throws std::bad_alloc 走査列又は束縛のメモリを確保出来ない場合
	 * @throws std::length_error 走査列又は名前の上限を超える場合
	 * @throws std::out_of_range ノード又は名前の番号が表に無い場合
	 */
	void SourceProjector::RecordMemberRoles(const GrammarProfile &Profile, SyntaxModel &Syntax, const bool ForModifiers) {
		// 対象の無い投影の終了
		if(ForModifiers && !Syntax.GetRules().HasBoundMemberModifiers) return;
		const ReferenceProjection Flow(Profile, Syntax);
		struct MemberContext {
			NodeId Node = NoNode; // 訪問する構文
			NodeId Owner = NoNode; // メンバを所有する型
			bool InHandler = false; // 捕捉条件の内側か
			bool InTypeBody = false; // 型の受手を確認出来る本体か
		};
		std::vector<MemberContext> Pending { { .Node = Syntax.GetRoot() } };
		while(!Pending.empty()) {
			const MemberContext Entry = Pending.back();
			const NodeId Node = Entry.Node;
			Pending.pop_back();
			Syntax.Charge(1);
			const bool InHandler = Entry.InHandler || Syntax.Is(Node, Role::CatchClause);
			if(!ForModifiers) Flow.RecordFacts(Node, InHandler);
			bool HasReceiver = Entry.InTypeBody;
			NodeId Owner = Syntax.Is(Node, Role::MemberOwner) ? Node : Entry.Owner;
			if(Syntax.HasField(Node, ChildRole::Body) && Syntax.Is(Syntax.GetParentOf(Node), Role::FunctionScope)) Owner = NoNode;
			if(Syntax.IsAny(Node, { Role::FunctionScope, Role::MemberOwner })) HasReceiver = false;
			if(Syntax.Is(Node, Role::MemberOrderBody) && Syntax.Is(Syntax.GetParentOf(Node), Role::MemberOwner)) HasReceiver = true;
			if(ForModifiers && Syntax.Is(Node, Role::MemberBindingModifier)) {
				const bool IsDecorator = Syntax.Is(Node, Role::DeclDecorator);
				const KindDef &Base = *Syntax.Nodes[Node].Kind.Def;
				for(const NodeId Target : Syntax.GetChildrenOf(Node)) if(Syntax.IsNamed(Target)) {
					const std::string Resolved = Flow.ResolveReference(Target, !IsDecorator);
					Syntax.Charge(1 + (Resolved.size() << 1));
					const KindDef &Kind = Profile.GetBindingKindOf(Base, Resolved, Owner == NoNode ? nullptr : Syntax.Nodes[Owner].Kind.Def);
					if(&Kind != &Base) Syntax.Nodes[Node].Kind.Def = &Kind;
				}
			} else if(!ForModifiers && HasReceiver && Syntax.Is(Node, Role::MemberDeclCall)) {
				const NodeId Object = Flow.UnwrapCallTarget(Syntax.GetFieldChild(Node, ChildRole::Object));
				if(Object == NoNode || Syntax.Is(Object, Role::ThisRef)) {
					const NodeId Target = Syntax.GetFieldChild(Node, ChildRole::Function);
					const std::string Resolved = Flow.ResolveReference(Target);
					Syntax.Charge(1 + (Resolved.size() << 1));
					Syntax.Nodes[Node].Kind.Def = &Profile.GetBindingKindOf(*Syntax.Nodes[Node].Kind.Def, Resolved);
				}
			}
			for(const NodeId Child : Syntax.GetChildrenOf(Node)) {
				Pending.push_back(
					{ .Node = Child, .Owner = Owner, .InHandler = InHandler && !Syntax.HasField(Child, ChildRole::Body), .InTypeBody = HasReceiver }
				);
			}
		}
		// 終了
		return;
	}

	/**
	 * 関数インポートの名前空間の所属だけを記録する関数
	 * 計算量：ノード数 N に対し O(N)
	 * @param Syntax 投影済の構文
	 * @throws std::bad_alloc 作業列のメモリを確保出来ない場合
	 * @throws std::length_error 作業列の上限を超える場合
	 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
	 */
	void SourceProjector::RecordImportScopes(SyntaxModel &Syntax) {
		// 名前空間を分けない文法の終了
		if(!Syntax.GetRules().HasNamespaceFunctionImports) return;
		NodeId Scope = Syntax.GetRoot();
		std::vector<std::pair<NodeId, NodeId>> Pending;
		for(const NodeId Child : Syntax.GetChildrenOf(Syntax.GetRoot())) {
			Syntax.Charge(1);
			const bool IsNamespace = Syntax.Is(Child, Role::NamespaceDef);
			const NodeId Body = IsNamespace ? Syntax.GetFieldChild(Child, ChildRole::Body) : NoNode;
			if(IsNamespace && Body == NoNode) Scope = Child;
			Pending.emplace_back(Child, IsNamespace ? Child : Scope);
		}
		while(!Pending.empty()) {
			const std::pair<NodeId, NodeId> Cur = Pending.back();
			Pending.pop_back();
			Syntax.Charge(1);
			Syntax.Nodes[Cur.first].Facts.ImportScope = Cur.second;
			for(const NodeId Child : Syntax.GetChildrenOf(Cur.first)) {
				Syntax.Charge(1);
				Pending.emplace_back(Child, Cur.second);
			}
		}
		// 終了
		return;
	}

	/**
	 * 投影済の出所を所有して静的解析用の事実を遅延生成する関数
	 * 構文木を保持せず，投影の出所と位置の逆マッピングを引き継ぐ
	 * 計算量：準備の編集数 E とコメント数 C に対し O(E + C)，生成時はノード数 N と名前の総バイト数 B に対し O(N + B + C)，位置変換は編集の組毎の二分探索
	 * @param SrcTree 完成候補の独立した解析結果
	 * @param Sources 投影で既に得たノードの出所（所有を移す）
	 * @param CommentRanges 検証と同じコメントの原文範囲
	 * @param DirectiveStarts 保持条件の収集で得たディレクティブの出所（所有を移す）
	 * @return 同じ完成候補の静的解析用の事実の生成処理
	 * @throws std::bad_alloc 対応表又は遅延処理のメモリを確保出来ない場合
	 * @throws std::length_error 対応表の上限を超える場合
	 */
	LintPreparation SourceProjector::DeferLintFacts(
		const ::ParsedTree &SrcTree,
		std::vector<Origin> Sources,
		const std::vector<std::pair<uint32_t, uint32_t>> &CommentRanges,
		std::vector<uint32_t> DirectiveStarts
	) {
		// 位置変換は既存の一括対応を共有し，解析木を手放した後も編集の数値だけを所有する
		SrcTree.OffsetBeforePreparation(0);
		// 静的解析の入口で１度だけ実行する生成処理の返戻
		return [
			Profile = &SrcTree.GetProfile(),
			Sources = std::move(Sources),
			Edits = SrcTree.EditBatches,
			Preparation = SrcTree.PreparationBatches,
			CommentRanges,
			DirectiveStarts = std::move(DirectiveStarts)
		](SyntaxModel &Syntax, CommentRepository &Comments, DiagnosticLocationMap &Locations) -> void {
			RecordMemberRoles(*Profile, Syntax, true);
			// 部分整形の照合で既に作ったノード範囲は作り直さない
			if(Locations.Nodes.empty()) {
				Locations.Nodes.resize(Syntax.GetNodeCount());
				for(NodeId Id = 0; Id < Sources.size(); ++Id) {
					Locations.Nodes[Id] =
					{ ::ParsedTree::OffsetBeforeBatches(Sources[Id].Start, Edits), ::ParsedTree::OffsetBeforeBatches(Sources[Id].End, Edits), {} };
				}
				for(NodeId Id = static_cast<NodeId>(Syntax.GetNodeCount()); Id-- > Sources.size();) {
					if(const std::span<const NodeId> Children = Syntax.GetChildrenOf(Id); !Children.empty()) {
						Locations.Nodes[Id] = Locations.Nodes[Children.front()];
						Locations.Nodes[Id].End = Locations.Nodes[Children.back()].End;
					}
				}
				for(OutputRange &Range : Locations.Nodes) {
					Range.Start = ::ParsedTree::OffsetBeforeBatches(Range.Start, Preparation);
					Range.End = ::ParsedTree::OffsetBeforeBatches(Range.End, Preparation);
				}
			}
			// 完成候補の位置を使う数値・キャストと束縛の事実を同じ走査で記録する
			for(NodeId Id = 0; Id < Syntax.GetNodeCount(); ++Id) {
				Syntax.Charge(1);
				SyntaxNode &Node = Syntax.Nodes[Id];
				if(Id < Sources.size() && Sources[Id].NodeIdx != NoNode) {
					Node.Facts.DoesDiscardBinding =
					(Syntax.Is(Id, Role::Identifier) || Syntax.GetBindingSyntax(Id) == BindingSyntax::Name) && Syntax.IsWord(Id, Word::DiscardName);
				}
				// 識別子のエスケープを解いた比較名で読む欠如値も，入力側で同じ意味へ対応付ける
				if(Node.Facts.NameIdx && Syntax.Is(Id, Role::IdentifierLeafLike)) {
					const std::string_view Name = Syntax.GetCmpNameView(Id);
					if(Profile->IsSpelling(Name, Word::NullishVal)) Node.Kind.Def = &Profile->GetLexemeKindOf(*Node.Kind.Def, Name);
				}
				const bool IsCast = Syntax.Is(Id, Role::CastInspection);
				if(!IsCast && !Syntax.IsAny(Id, { Role::NumLiteral, Role::IntLiteralCandidate })) continue;
				const NodeId Target = IsCast ? Syntax.GetFieldChild(Id, ChildRole::Type) : Id;
				if(Target == NoNode) continue;
				const OutputRange &Range = Locations.Nodes[Target];
				if(Range.Start >= Range.End || Range.End > Locations.Text.size()) continue;
				std::string_view Text = Syntax.GetLexemeOf(Target);
				if(Text.empty()) Text = std::string_view(Locations.Text).substr(Range.Start, Range.End - Range.Start);
				Syntax.Charge(Text.size());
				if(IsCast) Node.Facts.HasBuiltinCastType = Profile->IsBuiltinCastType(Text);
				else Node.Facts.NumericLiteral = Profile->ReadNumericLiteral(Text);
			}
			// 部分整形の照合から受け取った範囲は既に準備前の座標であり，行と桁だけを追加する
			for(OutputRange &Range : Locations.Nodes) Range.Point = Locations.GetPointAt(Range.Start);
			// 登録の判定は再実行せず，照合の走査で得た出所から診断位置だけを作る
			for(size_t Idx = 0; Idx < CommentRanges.size(); ++Idx) {
				const uint32_t Start = ::ParsedTree::OffsetBeforeBatches(CommentRanges[Idx].first, Preparation);
				const uint32_t End = ::ParsedTree::OffsetBeforeBatches(CommentRanges[Idx].second, Preparation);
				Locations.Comments.push_back({ Start, End, Locations.GetPointAt(Start) });
			}
			for(const uint32_t Start : DirectiveStarts) {
				Locations.DirectiveStarts.push_back(
					::ParsedTree::OffsetBeforeBatches(::ParsedTree::OffsetBeforeBatches(Start, Edits), Preparation)
				);
			}
			Comments.SetStructure(CommentNormalizer::Inspect(ReadOnlyCommentView(Comments.GetRecords())));
			// 終了
			return;
		};
	}

	/**
	 * 固定の束縛構造から意味上の子のフィールドを補う関数
	 * 文法のフィールドが無い仮引数・本体にも，ラッパーの除去・移動後の束縛構造からフィールドを補う
	 * 計算量：直接子数 C に対し O(C)
	 * @param Syntax 投影と構造修復を終えた構文
	 * @param Id フィールドを補う親ノード
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SourceProjector::RecordBindingFields(SyntaxModel &Syntax, const NodeId Id) {
		// 固定の束縛構造から意味上の子のフィールドを補う
		if(!Syntax.GetRules().HasBindingIdentity) return;
		const BindingSyntax Shape = Syntax.GetBindingSyntax(Id);
		// 宣言の初期化式のフィールドの補完（ゲッター・セッターの本体とは区別する）
		if(Syntax.GetRules().HasOrderedBindings && Shape == BindingSyntax::Decl) {
			bool IsValNext = false;
			for(const NodeId Child : Syntax.GetChildrenOf(Id)) {
				Syntax.Charge(1);
				if(IsValNext && Syntax.IsNamed(Child) || Syntax.GetBindingSyntax(Child) == BindingSyntax::Initializer) {
					Syntax.Nodes[Child].Fields |= Syntax.GetFieldBit(ChildRole::Val);
					IsValNext = false;
				}
				IsValNext |= Syntax.IsTokenRole(Child, Role::AssignmentToken);
			}
		}
		// 型名のトークンへ，導入名のフィールドを補完
		if(
			(Shape == BindingSyntax::ClassDecl || Shape == BindingSyntax::ClassExpr || Shape == BindingSyntax::ModuleDecl) &&
			Syntax.GetFieldChild(Id, ChildRole::Name) == NoNode
		) for(const NodeId Child : Syntax.GetChildrenOf(Id)) if(Syntax.GetNode(Child).Kind.Def->Binding == BindingSyntax::TypeName) {
			Syntax.Nodes[Child].Fields |= Syntax.GetFieldBit(ChildRole::Name);
			break;
		}
		if(Syntax.IsBindingFunction(Id) || Shape == BindingSyntax::Loop || Shape == BindingSyntax::Catch) {
			for(const NodeId Child : Syntax.GetChildrenOf(Id)) {
				const BindingSyntax Part = Syntax.GetBindingSyntax(Child);
				if(Part == BindingSyntax::Block) Syntax.Nodes[Child].Fields |= Syntax.GetFieldBit(ChildRole::Body);
				if(Syntax.IsBindingFunction(Id) && (Part == BindingSyntax::Params || Part == BindingSyntax::Param)) {
					Syntax.Nodes[Child].Fields |= Syntax.GetFieldBit(ChildRole::Params);
				}
			}
		}
		// 終了
		return;
	}

	/**
	 * 識別子の比較用の名前の取得関数
	 * 生の接頭辞・引用記号・ラベルの定義の記号を除き，エスケープをデコードし，比較で無視する字を除く（解釈出来ない名前はテキストの儘）
	 * 計算量：名前の長さ L に対し O(L)
	 * @param Reading 入力の読取規則
	 * @param Name 名前のテキスト
	 * @param IsLabel ラベルのトークンか
	 * @return 比較用の名前
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	std::string SourceProjector::GetCmpNameOf(const ReadingRules &Reading, std::string_view Name, const bool IsLabel) {
		// 比較用の名前から外形の記号を除去
		const std::string_view Raw = Name;
		if(IsLabel && !Reading.LabelDefSuffix.empty() && Name.ends_with(Reading.LabelDefSuffix)) {
			Name.remove_suffix(Reading.LabelDefSuffix.size());
		}
		if(!Reading.IdentifierRawPrefix.empty() && Name.starts_with(Reading.IdentifierRawPrefix)) {
			Name.remove_prefix(Reading.IdentifierRawPrefix.size());
		}
		if(
			const std::string_view Quote = Reading.IdentifierQuote;
			!Quote.empty() && Name.size() > Quote.size() && Name.starts_with(Quote) && Name.ends_with(Quote)
		) Name = Name.substr(Quote.size(), Name.size() - (Quote.size() << 1));
		// エスケープのデコードと書式文字の除去
		std::string Result;
		Result.reserve(Name.size());
		for(size_t Pos = 0; Pos < Name.size();) {
			const size_t Start = Pos;
			char32_t Point = 0;
			const bool IsEscape = Reading.ShouldDecodeIdentifierEscapes && Name[Pos] == '\\';
			if(IsEscape ? !HasDecodedIdentifierEscape(Reading, Name, Pos, Point) : !TextEdit::IsUtf8Decoded(Name, Pos, Point)) {
				// 解釈出来ないエスケープと不正な UTF-8 を持つ名前のテキストの返戻
				return std::string(Raw);
			}
			if(Reading.ShouldIgnoreIdentifierFormatChars && IsFormatChar(Point)) {
				// 書式文字で始まる名前（識別子に為れない）のテキストの返戻
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
	 * コメントを分離した解析結果のノード表への投影関数（整形仕様 3.4，4.3）
	 * コメントをアンカーへ紐付けた構文木を，独自のノード表とアンカーのノード毎のコメントへコピーする（アンカーへ紐付けたコメントのノードはコピーしない）
	 * パーサーが補ったトークン (`MISSING`) は原文に無い為にノードを作らず，文法が子に出さない隠れたトークンは無名のトークンのノードにする
	 * 計算量：入力のバイト数 N，ノード数 V，コメント数 K，エラー領域数 Z，所有対応数 E と呼出先の費用 Q に対し O(N + V log(2 + Z) + K + E + Q)
	 * @param SrcTree 解析結果（書き換えない）
	 * @param Profile 入力で確定した文法
	 * @param Input 適応済の入力と修復前のテキスト
	 * @param Owned 子のコード・配置・検証の独立した所有経路
	 * @param Separated 投影の間だけ持つコメントの分離の結果（検証の投影では行末コメントの前置区切を結果へ移す）
	 * @param NeedsPositions 完成候補で静的解析又は部分整形の照合用のノードの位置を記録するか
	 * @param Work 投影の既存の訪問と構文事実の準備・記録のノードの観測用計数（無ければ計上しない）
	 * @param Hidden 隠れたトークンの分割・照会の観測用計数（無ければ計上しない）
	 * @return ノード表・コメント・検証データ
	 * @throws FormatLimitExceeded 分離の対象のコメントが残る場合，テキストを保つ領域へコメントが紐付く場合，自己終了のタグの要素の定義が無い場合，又は子の所有経路が不完全・重複・欠落する場合（投影の不具合）
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::invalid_argument 呼出先へ渡す投影又は検証の引数が契約を満たさない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	ProjectionResult SourceProjector::ProjectNodes(
		const ::ParsedTree &SrcTree,
		const GrammarProfile &Profile,
		const ParseInput &Input,
		const OwnedInputRoutes &Owned,
		CommentSeparation &Separated,
		const bool NeedsPositions,
		StructureWorkAccount *const Work,
		HiddenWork *const Hidden
	) {
		// 投影先のノード表と検証データの準備
		const bool IsVerification = std::holds_alternative<ParseInput::VerificationSrc>(Input.Origin);
		const ReadingRules &TreeReading = SrcTree.GetReading(), &Rules = Profile.GetReading();
		ProjectionResult Result { SyntaxModel(Profile.GetRules(), Profile.GetChildRoleMask()), CommentRepository(), {} };
		if(Rules.SrcGoal != Lang::SrcGoal::Undetermined) Result.Syntax.IsModuleSrc = Rules.SrcGoal == Lang::SrcGoal::Module;
		SyntaxModel &Syntax = Result.Syntax;
		// 投影するノードと其の親・子の役割
		struct Frame {
			TSNode Node; // パーサーのノード（隠れたトークンは空）
			NodeId Parent; // 投影済の親
			uint64_t Fields; // 親の中の子の役割の集合
			std::pair<uint32_t, uint32_t> Hidden {}; // 隠れたトークンの原文の範囲
			TSNode ParentNode {}; // パーサーの親のノード（深さに比例する親の探索を避ける）
			std::vector<TSNode> Groups {}; // 投影せずに内側の子へ紐付を渡す解析用の括弧のラッパー
			bool IsInVerbatim = false; // 分離検査で逐語領域の内部を除くか
			bool IsValidationOnly = false; // 投影しない子の検証データだけを採るか
			TSSymbol Symbol = 0; // 親子表から引き継ぐ記号（根は其の記録から読む）
			size_t Relation = std::string::npos; // 同じ木の親子表の記録番号（隠れたトークンは番号を持たない）
		};
		struct ArgScope {
			const ::ParsedTree::StringizingArgs *Args; // 此の呼出で文字列化される仮引数（閉じの処理で参照を終える）
			uint32_t Argument = 0; // プリプロセッサが読む実引数の番号
			size_t Param = 0; // 昇順の仮引数の位置
			uint32_t Depth = 0; // 丸括弧の深さ（角括弧・波括弧・山括弧は区切を隠さない）
			bool InheritsWhitespace = false; // 外側の実引数の空白も観測されるか
		};
		std::vector<ArgScope> Arguments;
		// 昇順の対象を現在の実引数迄進め，外側から引き継いだ観測も含める
		const auto IsSelectedArgument = [&](ArgScope &Scope, const bool IsComma) -> bool {
			while(Scope.Param < Scope.Args->Params.size() && Scope.Args->Params[Scope.Param] < Scope.Argument) {
				if(Work) Work->Charge(1);
				++Scope.Param;
			}
			// 固定の実引数の区切は其の実引数の内容に含めない事の返戻
			return Scope.InheritsWhitespace || Scope.Argument >= Scope.Args->VariadicFrom ||
			!IsComma && Scope.Param < Scope.Args->Params.size() && Scope.Args->Params[Scope.Param] == Scope.Argument;
		};
		// 実引数のトークンを投影する同じ前順の走査で区切を読み，構文木の要素数で実引数を数えない
		const auto ReadArgument = [&](const KindDef &Kind) -> TokenSpacingMode {
			if(Work) Work->Charge(1);
			ArgScope &Scope = Arguments.back();
			const bool IsOpen = !Kind.IsNamed && Kind.Roles.test(static_cast<size_t>(Role::OpeningParenToken));
			const bool IsClose = !Kind.IsNamed && Kind.Roles.test(static_cast<size_t>(Role::ClosingParenToken));
			const bool IsComma = !Kind.IsNamed && Kind.Roles.test(static_cast<size_t>(Role::CommaToken)) && Scope.Depth == 1;
			const bool IsEdge = IsOpen && !Scope.Depth || IsClose && Scope.Depth == 1;
			const bool KeepsWhitespace = Scope.InheritsWhitespace || !IsEdge && IsSelectedArgument(Scope, IsComma);
			if(IsOpen) {
				if(Scope.Depth == UINT32_MAX) throw std::length_error("macro argument nesting exceeds its range");
				++Scope.Depth;
			} else if(IsClose && Scope.Depth) --Scope.Depth;
			else if(IsComma) {
				if(Scope.Argument == UINT32_MAX) throw std::length_error("macro argument count exceeds its range");
				++Scope.Argument;
			}
			if(IsClose && !Scope.Depth) Arguments.pop_back();
			// 外側の括弧と固定の実引数の区切を保持範囲に含めないモードの返戻
			return KeepsWhitespace ? TokenSpacingMode::Whitespace : TokenSpacingMode::Tokens;
		};
		// ノードの子孫に紐付いたコメントの有無（ノードを作らない部分木へ紐付いた物は置場を失う）
		const auto HasAttached = [&SrcTree, &Separated](const TSNode Node) -> bool {
			// 部分木の範囲に紐付の有るかの返戻
			return Separated.HasAttachedCommentInRange(SrcTree.GetStart(Node), SrcTree.GetEnd(Node) + 1) && (
				!Separated.GetLeading(Node).empty() || !Separated.GetTrailing(Node).empty() || HasDescendantOf(
					Node,
					[&Separated](const TSNode Sub) -> bool {
						// 子孫の紐付の有無の返戻
						return !Separated.GetLeading(Sub).empty() || !Separated.GetTrailing(Sub).empty();
					}
				)
			);
		};
		// 親の中で既に投影した最後のノード（親に無ければ祖先の中で親より前のノード，除くノードは数えない）
		const auto LastProjected = [&Result](NodeId Parent, const NodeId Excluded) -> NodeId {
			for(NodeId Up = Parent; Up != NoNode; Up = Result.Syntax.Nodes[Up].Parent) {
				const std::span<const NodeId> Siblings = Result.Syntax.Nodes[Up].Children;
				for(std::span<const NodeId>::reverse_iterator Found = Siblings.rbegin(); Found != Siblings.rend(); ++Found) {
					// 除くノードでも其の祖先でもない最後の子の返戻
					if(*Found != Excluded && *Found != Parent) return *Found;
				}
				Parent = Up;
			}
			// 前に投影したノードの無い事の返戻
			return NoNode;
		};
		// ノードを作らない又は出力の位置を持たない部分木へ紐付いたコメントの移譲
		// 空の本体の中の独立した物は本体の所有者の空の本体の物とし，其の他は既に投影した直前のノードの後（無ければ親）の物とする
		const auto MoveAttached =
		[&Separated, &Result](const TSNode Node, const NodeId Parent, const NodeId Owner, const NodeId Prev, const bool IsSubtree)
		-> void {
			// 部分木の投影位置の移譲
			const auto Transfer = [&](const TSNode Sub) -> void {
				for(const std::vector<CommentRecord> *const Attached : { &Separated.GetLeading(Sub), &Separated.GetTrailing(Sub) }) {
					for(CommentRecord Record : *Attached) {
						if(Owner != NoNode && Record.Kind == CommentKind::OwnLine) {
							Record.Anchor = Owner;
							Record.Placement.Side = CommentPlacement::Before;
							Record.Placement.IsEmptyBody = true;
						} else if(Prev != NoNode) {
							Record.Anchor = Prev;
							Record.Placement.Side = CommentPlacement::After;
							Record.Placement.TokensBefore = 0;
						} else Record.Anchor = Parent;
						Result.Comments.Add(std::move(Record));
					}
				}
				// 終了
				return;
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
		// PHP の本体の範囲（其の外の地の文は走査の後に逐語の子として加え，閉タグの後の子とコメントは投影しない，整形仕様 4.4）
		const std::pair<uint32_t, uint32_t> BodyBounds = ParseAdapter::GetOuterTextBounds(SrcTree);
		const uint32_t BodyStart = BodyBounds.first, BodyEnd = BodyBounds.second;
		// テキストを保つ領域に残るコメントの範囲（原文順）と解析用の括弧の位置（昇順）
		GapExclusions Gaps { SrcTree.CollectCommentRanges(BodyEnd), {} };
		Gaps.SyntheticPositions.reserve(SrcTree.GetSyntheticGroups().size() << 1);
		for(const std::pair<uint32_t, uint32_t> &Group : SrcTree.GetSyntheticGroups()) {
			Gaps.SyntheticPositions.push_back(Group.first);
			Gaps.SyntheticPositions.push_back(Group.second);
		}
		std::sort(Gaps.SyntheticPositions.begin(), Gaps.SyntheticPositions.end());
		Gaps.Work = Hidden;
		// 本文の分割結果は投影と構文事実の記録で共有する
		CollectHiddenTokens(SrcTree, 0, static_cast<uint32_t>(SrcTree.size()), Gaps, Gaps.Tokens);
		const std::vector<std::pair<uint32_t, uint32_t>> &CommentRanges = Gaps.CommentRanges;
		// 保持する区切記法を持たない領域では，照合だけの為にテキストをコピーしない
		std::vector<std::pair<uint32_t, uint32_t>> NotationRanges;
		for(const CommentSeparation::Range &Range : Separated.Separations) if(Range.IsNotation) {
			NotationRanges.emplace_back(Range.Begin, Range.End);
		}
		// テキストを保つ領域のテキストの中のコメントと分離の跡へ置いた区切記法の範囲の記録（テキストの変換はコメントのテキストと順を保つ）
		const auto RecordKept =
		[&SrcTree, &Profile, &Rules, &Input, &Separated, &CommentRanges, &NotationRanges, &Result, Hidden, Work](
			const uint32_t Start,
			const uint32_t End,
			const NodeId Id
		) -> void {
			const std::string &Lexeme = Result.Syntax.Nodes[Id].Lexeme;
			std::vector<ValidationContext::KeptComment> &KeptComments = Result.Validation.ProtectedRegions.KeptComments;
			const size_t FirstKept = KeptComments.size();
			std::vector<std::pair<uint32_t, uint32_t>> Kept;
			// 分離の跡へ置いた区切記法（原文のテキストの儘の領域だけが持つ）
			if(
				const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Notation =
				std::lower_bound(NotationRanges.begin(), NotationRanges.end(), std::pair<uint32_t, uint32_t>(Start, 0));
				Notation != NotationRanges.end() && Notation->first < End && Notation->second <= End
			) {
				if(Hidden) ++Hidden->KeptCopies;
				if(Lexeme == Separated.BuildSeparatedText(SrcTree, Start, End, &Kept)) for(const std::pair<uint32_t, uint32_t> &Range : Kept) {
					KeptComments.push_back({ Id, Range.first, Range.second });
				}
			}
			const size_t FirstComment = KeptComments.size();
			size_t Cursor = 0;
			for(
				std::vector<std::pair<uint32_t, uint32_t>>::const_iterator It =
				std::lower_bound(CommentRanges.begin(), CommentRanges.end(), std::pair<uint32_t, uint32_t>(Start, 0));
				It != CommentRanges.end() && It->second <= End;
				++It
			) {
				// アンカーへ紐付けてテキストから除いたコメントの読飛
				if(Separated.IsSeparatedAt(It->first)) continue;
				std::string Decoded;
				std::string_view Body = std::string_view(SrcTree).substr(It->first, It->second - It->first);
				if(SrcTree.HasCodeTextDecoding()) {
					Decoded = ParseAdapter::DecodeLexeme(SrcTree, std::string(Body), It->first);
					Body = Decoded;
				}
				const size_t At = Lexeme.find(Body, Cursor);
				if(At == std::string::npos) {
					throw FormatLimitExceeded("a comment kept in a verbatim region is lost", FailureReason::FormatterDefect);
				}
				KeptComments.push_back({ Id, static_cast<uint32_t>(At), static_cast<uint32_t>(At + Body.size()) });
				Cursor = At + Body.size();
				// ソース側に残るコメントも，完成候補の診断には最初の投影の記録を渡す
				if(std::holds_alternative<ParseInput::VerificationSrc>(Input.Origin)) {
					CommentRecord Record;
					Record.Text = Body;
					Record.Anchor = Id;
					Record.Kind = SrcTree.ClassifyComment(SrcTree.GetCommentSpans(), It->first, It->second);
					Record.Placement.Side = CommentPlacement::Inside;
					Record.Notation = CommentRepository::NotationOf(Profile.GetRules().Notation, Body, !It->first);
					Record.Directive = DirectiveOf(SrcTree, Rules, Separated.Head, Body, It->first, Record.Notation, TSNode{});
					Record.Role = Record.Directive ? CommentRole::Directive : CommentRole::Presentation;
					CommentRepository::AttachRules(Profile.GetRules().Notation, Record);
					Record.OriginalOrder = Separated.RecordRanges.size();
					CommentRepository::Decompose(Record);
					Result.Comments.Records.push_back(std::move(Record));
					Separated.RecordRanges.emplace_back(SrcTree.OffsetInInput(It->first), SrcTree.OffsetInInput(It->second));
				}
			}
			// 確定したコメント範囲の間だけを調べ，本文の物理改行とコードの改行を分ける
			if(FirstKept < KeptComments.size()) {
				if(Work) Work->Charge(Lexeme.size() + KeptComments.size() - FirstKept);
				const std::string_view Text = Lexeme;
				size_t From = 0, NotationIdx = FirstKept, CommentIdx = FirstComment;
				bool HasCodeNewline = false;
				// 其々位置順の区切記法と原文保持の範囲を，同じ作業列から順に読む
				for(size_t Idx = FirstKept; Idx < KeptComments.size(); ++Idx) {
					const ValidationContext::KeptComment &Range = KeptComments[
						CommentIdx == KeptComments.size() ||
						NotationIdx < FirstComment && KeptComments[NotationIdx].Start <= KeptComments[CommentIdx].Start ? NotationIdx++ : CommentIdx++
					];
					if(From < Range.Start) {
						HasCodeNewline = HasCodeNewline || Text.substr(From, Range.Start - From).find('\n') != std::string_view::npos;
					}
					From = std::max(From, static_cast<size_t>(Range.End));
				}
				Result.Syntax.Nodes[Id].Facts.HasCodeNewline = HasCodeNewline || Text.substr(From).find('\n') != std::string_view::npos;
			}
			// 終了
			return;
		};
		// 投影したノード毎の原文の出所（構文事実の記録に使い，中核へは渡さない）と，区切記法の後の最初のトークンか
		std::vector<Origin> Sources;
		std::vector<TSSymbol> Symbols;
		std::vector<uint8_t> SeparatedBefore;
		// 投影するノードの数はパーサーのノードの数に概ね等しい為，伸長の度の再確保を避ける（ノード表は deque の為に予約を持たない）
		const TSNode RootNode = SrcTree.GetRoot();
		const bool HasTreeError = SrcTree.HasError(RootNode);
		const uint32_t DescendantCount = ts_node_descendant_count(RootNode);
		Syntax.IsRemoved.reserve(DescendantCount);
		Sources.reserve(DescendantCount);
		Symbols.reserve(DescendantCount);
		// 記号は出所へ重ねて持たず，此の列を後の検証データへ移す
		const auto RecordOrigin = [&](const Origin Src, const TSSymbol Symbol = 0) -> void {
			Sources.push_back(Src);
			Symbols.push_back(Symbol);
			// 終了
			return;
		};
		SeparatedBefore.reserve(Sources.capacity());
		bool IsSeparatorPending = false;
		std::vector<Frame> Stack { { RootNode, NoNode, 0 } }, Children;
		SrcTree.RecordNodeRelations();
		Stack.front().Relation = 0;
		Stack.front().IsInVerbatim = TreeReading.HasCommentDocContent || SrcTree.Syntax.Comments->Nodes.empty();
		std::span<const ParsedChild> ParsedChildren;
		std::vector<uint8_t> ConsumedGroups(SrcTree.GetSyntheticGroups().size());
		const size_t HaltedStart = SrcTree.GetHaltedDataStart();
		bool HasProjectedData = false;
		const uint32_t CodeEnd = static_cast<uint32_t>(HaltedStart == std::string::npos ? SrcTree.size() : HaltedStart);
		const bool HasOuterTail = BodyEnd < CodeEnd;
		// CSS の宣言の値の語の記号
		const TSSymbol PlainVal = Profile.FindSymbolWithRole(Role::UnitText);
		// 名前付実引数と共有するキー値の要素の定義
		const TSSymbol ColonPair = Profile.FindSymbolWithRole(Role::Arg);
		// 解析用の配列の宣言子から戻す，幅を指定する宣言子の定義
		const TSSymbol SizedDeclarator = Profile.FindSymbolWithRole(Role::SizedDeclarator);
		// 値の補間の解析用の囲みを本来の構文種別へ戻す定義
		const TSSymbol Interpolation =
		TreeReading.ValueInterpolationOpen.empty() ? 0 : Profile.FindSymbolWithRole(Role::StrInterpolation);
		// 型引数付宣言の読違を持たない文法では，ノード毎の束縛照会を行わない
		const bool CanReadTemplateDecl = Rules.CanMisreadTemplateDecls, HasUrlArgs = !Rules.UrlFunctionHead.empty();
		// 数値トークンへ吸収された符号を分ける，文法が持つ単項式の生成定義
		const TSSymbol NumericUnary = Profile.FindSymbolWithRole(Role::UnaryOperation);
		// 自己終了のタグを包む要素は，開始・終了タグを持つ要素と同じ定義を使う
		const TSSymbol MarkupElement = Profile.FindSymbolWithRole(Role::MarkupElement);
		// 検証データを集めるノード（原文順，各ノードを１度だけ記録する）
		std::vector<const TSNode *> ValidationNodes;
		std::vector<TSNode> ExtraNodes;
		std::deque<TSNode> ContextNodes;
		std::vector<std::pair<uint32_t, uint32_t>> TemplateDecls;
		std::vector<std::pair<uint32_t, uint32_t>> ValidationComments;
		ValidationNodes.reserve(DescendantCount);
		// 分離検査で子孫を数えない逐語領域（従来の計数と同じ条件）
		const auto IsVerbatimCommentHost = [&](const TSNode Node, const TSSymbol Symbol) -> bool {
			const RoleSet &Roles = SrcTree.GetProfile().GetKindOf(Symbol).Roles;
			// 逐語の所属の返戻
			return Roles.test(static_cast<size_t>(Role::SrcLeaf)) || Roles.test(static_cast<size_t>(Role::SpaceSkip)) ||
			Roles.test(static_cast<size_t>(Role::PreprocNode)) || HasUrlArgs && SrcTree.IsCssUrlArgs(Node);
		};
		// 検証で参照するノードの記録と，同じ訪問での分離検査
		const auto RecordNode =
		[&](const TSNode Node, const ParsedChild &Record, const TSSymbol Symbol, const bool IsInVerbatim) -> void {
			if(Work) Work->Charge(1);
			const bool IsComment =
			!ts_node_is_null(Node) && SrcTree.GetProfile().GetKindOf(Symbol).Roles.test(static_cast<size_t>(Role::Comment));
			if(
				!IsInVerbatim && IsComment &&
				(SrcTree.GetRules().LexicalSeparator.empty() || SrcTree.GetView(Node) != SrcTree.GetRules().LexicalSeparator) &&
				!Separated.IsSeparatedComment(SrcTree, Node) && SrcTree.GetStart(Node) < BodyEnd
			) throw FormatLimitExceeded("comment nodes reach the core passes", FailureReason::FormatterDefect);
			if(!ts_node_is_null(Node)) {
				if(Node.id == Record.Node.id && Node.tree == Record.Node.tree && std::ranges::equal(Node.context, Record.Node.context)) {
					ValidationNodes.push_back(&Record.Node);
				} else {
					ContextNodes.push_back(Node);
					ValidationNodes.push_back(&ContextNodes.back());
				}
				if(IsComment && !ts_node_eq(Node, RootNode)) ValidationComments.emplace_back(SrcTree.GetStart(Node), SrcTree.GetEnd(Node));
			}
			// 終了
			return;
		};
		bool IsInVerbatim = false;
		// 子孫の訪問は同じ世代の子列を読み，作業列の容量を逐語領域間で再使用する
		struct DescendantFrame {
			std::span<const ParsedChild> Children; // 既に記録した直接子
			size_t Next; // 次に訪ねる子の位置
			bool IsExcluded; // 此の親迄の逐語の所属
		};
		std::vector<DescendantFrame> Descendants;
		// 投影しない子孫にも逐語の所属を継ぎ，分離検査と検証データの記録を同じ順に行う
		const auto RecordDescendants = [&](const ParsedChild &Record) -> void {
			const std::span<const ParsedChild> Direct = SrcTree.GetChildrenAt(Record.Relation);
			// 子を持たないノードの終了
			if(Direct.empty()) return;
			Descendants.clear();
			Descendants.push_back({ Direct, 0, IsInVerbatim || IsVerbatimCommentHost(Record.Node, Record.Symbol) });
			while(!Descendants.empty()) {
				DescendantFrame &Parent = Descendants.back();
				if(Parent.Next == Parent.Children.size()) {
					Descendants.pop_back();
					continue;
				}
				const ParsedChild &Part = Parent.Children[Parent.Next++];
				RecordNode(Part.Node, Part, Part.Symbol, Parent.IsExcluded);
				const bool IsExcluded = Parent.IsExcluded || IsVerbatimCommentHost(Part.Node, Part.Symbol);
				const std::span<const ParsedChild> Nested = SrcTree.GetChildrenAt(Part.Relation);
				if(!Nested.empty()) Descendants.push_back({ Nested, 0, IsExcluded });
			}
			// 終了
			return;
		};
		// 構文木からノード表への投影
		while(!Stack.empty()) {
			Frame Top = std::move(Stack.back());
			Stack.pop_back();
			IsInVerbatim = Top.IsInVerbatim;
			if(ts_node_is_null(Top.Node)) {
				// 隠れたトークンの無名のトークンのノード
				const uint32_t Start = Top.Hidden.first, End = Top.Hidden.second;
				// 本体の外の地の文の中のトークンの読飛
				if(HasOuterTail && Start >= BodyEnd && ts_node_eq(Top.ParentNode, RootNode)) continue;
				const std::string_view Text = std::string_view(SrcTree).substr(Start, End - Start);
				const TSSymbol Symbol = Profile.GetTokenSymbol(Text);
				SyntaxNode Hidden;
				Hidden.Kind = { Symbol ? &Profile.GetKindOf(Symbol) : GetHiddenLexemeKind(SrcTree, Top.ParentNode) };
				if(!IsVerification) Hidden.Facts.BlankKey = Symbol ? Profile.GetBlankKeyOf(Symbol) : 0;
				Hidden.Parent = Top.Parent;
				Hidden.Lexeme = ParseAdapter::DecodeLexeme(SrcTree, std::string(Text), Start);
				const NodeId Id = Syntax.AddNode(std::move(Hidden));
				RecordOrigin({ NoNode, Start, End });
				SeparatedBefore.push_back(IsSeparatorPending);
				IsSeparatorPending = false;
				Syntax.Nodes[Id].Idx = static_cast<uint32_t>(Syntax.Nodes[Top.Parent].Children.size());
				Syntax.Nodes[Top.Parent].Children.push_back(Id);
				continue;
			}
			const size_t RecordedIdx = ValidationNodes.size();
			const ParsedChild &TopRecord = SrcTree.GetNodeAt(Top.Relation);
			if(!Top.Symbol) Top.Symbol = TopRecord.Symbol;
			RecordNode(Top.Node, TopRecord, Top.Symbol, IsInVerbatim);
			if(Top.IsValidationOnly) {
				RecordDescendants(TopRecord);
				continue;
			}
			// 読違の修復は，空白の補正より前に入力と完成候補の型の役割へ揃える
			if(
				const std::span<const ::ParsedTree::TemplatePart> Parts = CanReadTemplateDecl ?
				std::span<const ::ParsedTree::TemplatePart>(SrcTree.GetTemplateDecl(Top.Node)) :
				std::span<const ::ParsedTree::TemplatePart>();
				!Parts.empty()
			) {
				TemplateDecls.emplace_back(SrcTree.GetStart(Top.Node), SrcTree.GetEnd(Top.Node));
				RecordDescendants(TopRecord);
				const NodeId First = static_cast<NodeId>(Syntax.GetNodeCount());
				const bool HasComments = Separated.HasAttachedCommentInRange(Parts.front().Start, Parts.front().End + 1);
				std::unordered_map<uint64_t, NodeId> Anchors;
				std::vector<std::pair<uint32_t, NodeId>> CommentTokens;
				for(NodeId Idx = 0; Idx < Parts.size(); ++Idx) {
					if(Work) Work->Charge(1);
					const ::ParsedTree::TemplatePart &Part = Parts[Idx];
					SyntaxNode Projected;
					Projected.Kind = { &Profile.GetKindOf(Part.Symbol) };
					Projected.Parent = Idx ? First + Part.Parent : Top.Parent;
					Projected.Fields = Idx ? Part.Fields : Top.Fields;
					if(Part.IsLeaf) {
						Projected.Lexeme = ParseAdapter::DecodeLexeme(SrcTree, std::string(SrcTree, Part.Start, Part.End - Part.Start), Part.Start);
					}
					if(!IsVerification) {
						Projected.Facts.Forms = Profile.GetFormsOf(Part.Symbol);
						Projected.Facts.BlankKey = Profile.GetBlankKeyOf(Part.Symbol);
						if(Projected.Kind.Def->Roles.test(static_cast<size_t>(Role::SpacelessTypeName))) {
							Projected.Facts.Add(ConstraintKind::NoOptionalBreak);
						}
					}
					const NodeId Id = Syntax.AddNode(std::move(Projected));
					const NodeId Parent = Syntax.Nodes[Id].Parent;
					Syntax.Nodes[Id].Idx = static_cast<uint32_t>(Syntax.Nodes[Parent].Children.size());
					Syntax.Nodes[Parent].Children.push_back(Id);
					RecordOrigin({ Idx ? NoNode : static_cast<NodeId>(RecordedIdx), Part.Start, Part.End }, Part.Symbol);
					SeparatedBefore.push_back(!Idx && IsSeparatorPending);
					if(HasComments) {
						Anchors.try_emplace(uint64_t{ Part.Start } << 32 | Part.End, Id);
						if(Part.IsLeaf) CommentTokens.emplace_back(SrcTree.OffsetInInput(Part.Start), Id);
					}
				}
				IsSeparatorPending = false;
				// 外側のアンカーは同じ範囲へ移し，文中のコメントは分割後の実在するトークンとの順を保つ
				if(HasComments) for(size_t Idx = RecordedIdx; Idx < ValidationNodes.size(); ++Idx) {
					const TSNode Original = *ValidationNodes[Idx];
					const uint64_t Key = uint64_t{ SrcTree.GetStart(Original) } << 32 | SrcTree.GetEnd(Original);
					const std::unordered_map<uint64_t, NodeId>::const_iterator Found = Anchors.find(Key);
					for(const std::vector<CommentRecord> *const Attached : { &Separated.GetLeading(Original), &Separated.GetTrailing(Original) }) {
						for(CommentRecord Record : *Attached) {
							Record.Anchor = Found == Anchors.end() ? First : Found->second;
							const std::pair<uint32_t, uint32_t> &Range = Separated.RecordRanges[Record.OriginalOrder];
							if(Range.first >= SrcTree.OffsetInInput(Parts.front().Start) && Range.second <= SrcTree.OffsetInInput(Parts.front().End)) {
								const std::vector<std::pair<uint32_t, NodeId>>::const_iterator Next = std::lower_bound(
									CommentTokens.begin(),
									CommentTokens.end(),
									Range.second,
									[](const std::pair<uint32_t, NodeId> &Token, const uint32_t Pos) -> bool {
										// コメントより前のトークンかの返戻
										return Token.first < Pos;
									}
								);
								if(Record.Placement.Side == CommentPlacement::Before && Next != CommentTokens.end()) Record.Anchor = Next->second;
								else if(Next != CommentTokens.begin()) {
									Record.Anchor = std::prev(Next)->second;
									Record.Placement.Side = CommentPlacement::After;
								}
								Record.Placement.TokensBefore = 0;
							}
							Result.Comments.Add(std::move(Record));
						}
					}
				}
				continue;
			}
			// 両側が解析用の囲みであるラッパーは，名前付の子を元の親とフィールドへ直接投影する
			const std::pair<uint32_t, uint32_t> Span { TopRecord.GetStart(), TopRecord.End - 1 };
			if(
				const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Group =
				std::ranges::lower_bound(SrcTree.GetSyntheticGroups(), Span);
				Group != SrcTree.GetSyntheticGroups().end() && *Group == Span && SrcTree.GetChildCount(Top.Node) > 2 &&
				SrcTree.Is(SrcTree.GetChild(Top.Node, 0), Role::OpeningParenToken) &&
				SrcTree.Is(SrcTree.GetChild(Top.Node, SrcTree.GetChildCount(Top.Node) - 1), Role::ClosingParenToken)
			) {
				ConsumedGroups[static_cast<size_t>(std::distance(SrcTree.GetSyntheticGroups().begin(), Group))] = 1;
				Children.clear();
				size_t InnerIdx = std::string::npos;
				for(const ParsedChild &Part : SrcTree.GetChildrenAt(Top.Relation)) {
					const TSNode Child = Part.Node;
					Frame Next { Child, Top.Parent, Top.Fields };
					Next.Relation = Part.Relation;
					Next.Symbol = Part.Symbol;
					Next.IsInVerbatim = IsInVerbatim || IsVerbatimCommentHost(Top.Node, Top.Symbol);
					Next.IsValidationOnly = !Part.IsNamed || SrcTree.Is(Child, Role::Comment);
					if(!Next.IsValidationOnly) {
						if(InnerIdx != std::string::npos) {
							throw FormatLimitExceeded("a synthetic group has multiple expressions", FailureReason::FormatterDefect);
						}
						InnerIdx = Children.size();
						Next.ParentNode = Top.ParentNode;
						Next.Groups = std::move(Top.Groups);
					}
					Children.push_back(std::move(Next));
				}
				if(InnerIdx == std::string::npos) {
					throw FormatLimitExceeded("a synthetic group has no expression", FailureReason::FormatterDefect);
				}
				Frame &Inner = Children[InnerIdx];
				Inner.Groups.push_back(Top.Node);
				Inner.Groups.push_back(Children.front().Node);
				Inner.Groups.push_back(Children.back().Node);
				// 同じ前進走査の順でのラッパー・開き・コメント・内側の式・閉じの記録
				Stack.insert(Stack.end(), std::make_move_iterator(Children.rbegin()), std::make_move_iterator(Children.rend()));
				continue;
			}
			// 末尾カンマに対応しないパーサーが破損として包んだ JSON のカンマは，通常の区切のトークンとしてコピー，包みに紐付いたコメントも其のカンマへ移す（整形仕様 4.2）
			const TSNode Wrapper = Top.Node;
			const TSNode Misread = Rules.CanTolerateTrailingCommaError && SrcTree.Is(Wrapper, Role::Error) ?
			SyntaxParser::JsonTrailingCommaToken(SrcTree, Wrapper) :
			TSNode{};
			const TSNode Comma = !ts_node_is_null(Misread) && ts_node_eq(ts_node_parent(Misread), Wrapper) ? Misread : TSNode{};
			const TSNode Node = ts_node_is_null(Comma) ? Wrapper : Comma;
			const ParsedChild &NodeRecord = ts_node_is_null(Comma) ? TopRecord : *SrcTree.FindRecordedNode(Node);
			const uint32_t NodeStart = NodeRecord.GetStart();
			const TSSymbol NodeSymbol = NodeRecord.Symbol;
			const size_t Relation = NodeRecord.Relation;
			ParsedChildren = SrcTree.GetChildrenAt(Relation);
			const uint32_t NodeChildCount = static_cast<uint32_t>(ParsedChildren.size());
			const KindDef &NodeKind = Profile.GetKindOf(NodeSymbol);
			const RoleSet &NodeRoles = NodeKind.Roles;
			if(!ts_node_is_null(Comma)) RecordDescendants(TopRecord);
			// 本体の外の地の文の中の子（閉タグの後をパーサーがコメント等と読んだ物）の読飛
			if(HasOuterTail && NodeStart >= BodyEnd && ts_node_eq(Top.ParentNode, RootNode)) {
				RecordDescendants(NodeRecord);
				continue;
			}
			// アンカーへ紐付けたコメントはノードにせず，除いた跡でトークンが融合する CSS の境界だけ区切記法を置く（整形仕様 6.1，7.1）
			if(NodeRoles.test(static_cast<size_t>(Role::Comment)) && Separated.IsSeparatedComment(SrcTree, Node)) {
				RecordDescendants(NodeRecord);
				IsSeparatorPending = IsSeparatorPending || Separated.IsNotationSeparated(Node);
				continue;
			}
			// 根の末尾の自動終端は，改行の場合と同じく直前の構文の終端で表し，独立したトークンを作らない
			if(
				ts_node_eq(Top.ParentNode, RootNode) && Profile.IsEofTerminator(Node, static_cast<uint32_t>(std::string_view(SrcTree).size()))
			) {
				RecordDescendants(NodeRecord);
				MoveAttached(Node, Top.Parent, NoNode, LastProjected(Top.Parent, NoNode), false);
				continue;
			}
			// 空のカスタムプロパティ値を整数と読む偽のノード
			// パーサーが補ったトークンと，CSS の空値の偽ノード，トークンの融合を防ぐ空の区切記法は投影しない
			if(
				const bool IsEmptyCustomVal = ParseAdapter::IsEmptyCssCustomVal(SrcTree, Node);
				NodeRecord.IsMissing || IsEmptyCustomVal ||
				!SrcTree.GetRules().LexicalSeparator.empty() && NodeRoles.test(static_cast<size_t>(Role::Comment)) &&
				SrcTree.GetView(Node) == SrcTree.GetRules().LexicalSeparator
			) {
				RecordDescendants(NodeRecord);
				// 許容した欠落は既存の末尾カンマを指す為，カンマを足さず後続境界へ紐付だけを渡す
				if(
					NodeRecord.IsMissing && Rules.CanTolerateTrailingCommaError &&
					!ts_node_is_null(SyntaxParser::JsonTrailingCommaToken(SrcTree, Node))
				) {
					TSNode Following = SrcTree.GetNextSibling(Node);
					while(!ts_node_is_null(Following) && (SrcTree.IsMissing(Following) || SrcTree.Is(Following, Role::Comment))) {
						Following = SrcTree.GetNextSibling(Following);
					}
					for(
						std::unordered_map<const void *, std::vector<CommentRecord>> *const Map :
						{ &Separated.LeadingByNode, &Separated.TrailingByNode }
					) {
						std::unordered_map<const void *, std::vector<CommentRecord>>::node_type Attached = Map->extract(Node.id);
						if(Attached.empty()) continue;
						for(CommentRecord &Record : Attached.mapped()) {
							Record.Placement.TokensBefore = 0;
							if(!ts_node_is_null(Following) && SrcTree.Is(Following, Role::CloserToken)) {
								Record.Placement.Side = CommentPlacement::Before;
								Separated.Attach(SrcTree, Following, true, std::move(Record));
							} else {
								// 実在する閉じが無い場合は，既に投影した所有領域の終端
								Record.Anchor = Top.Parent;
								Record.Placement.Side = CommentPlacement::After;
								Result.Comments.Add(std::move(Record));
							}
						}
					}
				}
				if(HasAttached(Node)) {
					throw FormatLimitExceeded("a comment is attached to an unprojected token", FailureReason::FormatterDefect);
				}
				IsSeparatorPending = IsSeparatorPending || !NodeRecord.IsMissing && !IsEmptyCustomVal;
				continue;
			}
			if(
				TreeReading.HasKeywordClosedBlocks && (
					NodeRoles.test(static_cast<size_t>(Role::EmptyStmt)) ||
					NodeRoles.test(static_cast<size_t>(Role::ThenSection)) && !CountCodeChildren(SrcTree, Separated, Node, true, nullptr)
				)
			) {
				RecordDescendants(NodeRecord);
				// 空の `then` の本体の所有者は其の親
				MoveAttached(
					Node,
					Top.Parent,
					NodeRoles.test(static_cast<size_t>(Role::ThenSection)) ? Top.Parent : NoNode,
					LastProjected(Top.Parent, NoNode),
					true
				);
				continue;
			}
			if(
				TreeReading.HasKeywordClosedBlocks && !NodeKind.IsNamed && (
					(
						NodeRoles.test(static_cast<size_t>(Role::CondBodyDelimiterToken)) ||
						NodeRoles.test(static_cast<size_t>(Role::IterationBodyDelimiterToken))
					) && (SrcTree.Is(Top.ParentNode, Role::ThenSection) || SrcTree.Is(Top.ParentNode, Role::IterationSection)) ||
					NodeRoles.test(static_cast<size_t>(Role::StmtTerminatorToken)) && !SrcTree.Is(Top.ParentNode, Role::BlockParamList)
				)
			) {
				RecordDescendants(NodeRecord);
				// 名前付の子を持たない `then`・`do` の本体の語へ紐付いたコメントは，所有者の空の本体の物
				const bool IsEmptyBody = !NodeRoles.test(static_cast<size_t>(Role::StmtTerminatorToken)) &&
				!CountCodeChildren(SrcTree, Separated, Top.ParentNode, true, nullptr);
				MoveAttached(Node, Top.Parent, IsEmptyBody ? Syntax.Nodes[Top.Parent].Parent : NoNode, LastProjected(Top.Parent, NoNode), true);
				continue;
			}
			// 符号が数のトークンへ吸収された形と，空の左辺へ分かれた形の投影の統一
			const std::string_view NumText = NumericUnary && (
				!NodeChildCount && NodeRoles.test(static_cast<size_t>(Role::NumLiteral)) ||
				NodeRoles.test(static_cast<size_t>(Role::SrcLeaf)) && SrcTree.Is(ts_node_named_child(Node, 0), Role::NumLiteral)
			) ? SrcTree.GetView(Node) : std::string_view();
			const bool IsSignedNum = !NumText.empty() && (NumText.front() == '+' || NumText.front() == '-');
			const std::pair<TSNode, TSNode> Dimension = SignedDimensionParts(SrcTree, Separated, Node);
			const bool IsSignedDimension = !ts_node_is_null(Dimension.first);
			const TSNode Literal = IsSignedDimension ? Dimension.second : Node;
			const bool HasChildren = IsSignedNum || !IsSignedDimension && NodeChildCount;
			bool HasMacroBody = false;
			if(IsSignedDimension) ParsedChildren = SrcTree.GetChildrenOf(Literal);
			if(!IsVerification && NodeRoles.test(static_cast<size_t>(Role::MacroDef))) for(const ParsedChild &Child : ParsedChildren) {
				HasMacroBody |= Profile.GetKindOf(Child.Symbol).Roles.test(static_cast<size_t>(Role::MacroBody));
			}
			const TSSymbol LiteralSymbol = IsSignedDimension ? SrcTree.GetSymbol(Literal) : NodeSymbol;
			const KindDef &LiteralKind = IsSignedDimension ? Profile.GetKindOf(LiteralSymbol) : NodeKind;
			TokenSpacingMode TokenSpacing = Top.Parent == NoNode ? TokenSpacingMode::None : Syntax.Nodes[Top.Parent].Facts.TokenSpacing;
			if(!Arguments.empty()) TokenSpacing = TokenSpacingMode::Tokens;
			if(NodeRoles.test(static_cast<size_t>(Role::PreservedTokenSeq))) TokenSpacing = TokenSpacingMode::Punctuation;
			else if(
				TreeReading.HasPreprocMacros &&
				(NodeRoles.test(static_cast<size_t>(Role::DelimitedList)) || NodeRoles.test(static_cast<size_t>(Role::ParamContainer)))
			) {
				if(const ::ParsedTree::StringizingArgs *const Stringizing = SrcTree.GetStringizingArgs(Node); Stringizing) {
					TokenSpacing = TokenSpacingMode::Tokens;
					const bool InheritsWhitespace = !Arguments.empty() && IsSelectedArgument(Arguments.back(), false);
					if(Work) Work->Charge(1);
					Arguments.push_back({ Stringizing, 0, 0, 0, InheritsWhitespace });
				} else for(const ParsedChild &Child : ParsedChildren) {
					if(Hidden) ++Hidden->ArgTextReads;
					if(Profile.GetKindOf(Child.Symbol).Roles.test(static_cast<size_t>(Role::CompoundBody))) {
						TokenSpacing = TokenSpacingMode::Tokens;
						break;
					}
				}
			}
			const bool IsVerbatimLiteral = !ParsedChildren.empty() && (
				TokenSpacing == TokenSpacingMode::None || LiteralKind.Roles.test(static_cast<size_t>(Role::StrLikeAll)) || !LiteralKind.IsNamed
			) && IsVerbatimNode(SrcTree, Separated, Literal, LiteralKind.Roles, ParsedChildren, Input.HasFilePreamble, Hidden);
			const bool IsVerbatim = !IsSignedNum && IsVerbatimLiteral;
			if(!Arguments.empty() && (!HasChildren || IsVerbatim)) TokenSpacing = ReadArgument(LiteralKind);
			SyntaxNode Projected;
			Projected.Facts.TokenSpacing = TokenSpacing;
			// 投影前の子の役割から，空・欠損を含むマクロ本体の有無を１度記録する
			if(HasMacroBody) Projected.Facts.Add(ConstraintKind::ValuedMacro);
			// CSS の宣言の値の語は，密着したコメントの有無でパーサーが名前と値の語に読み分ける為，値の語へ揃える（整形仕様 4.2）
			const bool IsGluedValName =
			NodeRoles.test(static_cast<size_t>(Role::PlainValWord)) && Top.Parent != NoNode && Syntax.Is(Top.Parent, Role::TypedDecl);
			// 解析用の二項式は，復元する区切がコロンなら算術でなくキー値の要素へ戻す
			bool IsColonPair = false;
			if(NodeRoles.test(static_cast<size_t>(Role::BinaryOperation))) {
				for(const ParsedChild &Child : ParsedChildren) {
					if(
						const LexemeRestoration *const Original = SrcTree.GetRestorationAt(Child.GetStart(), Child.End);
						!Child.IsNamed && Original && Original->Original == ":"
					) IsColonPair = true;
				}
			}
			if(IsColonPair && !ColonPair) {
				throw FormatLimitExceeded("a restored colon pair has no definition", FailureReason::FormatterDefect);
			}
			const LexemeRestoration *const OpeningRestoration =
			!Interpolation || !NodeRoles.test(static_cast<size_t>(Role::BreakableDelimiterInterior)) || ParsedChildren.empty() ?
			nullptr :
			SrcTree.GetRestorationAt(ParsedChildren.front().GetStart(), ParsedChildren.front().End);
			// コロンから解析用に変えた開区切を持つ配列だけを，名前と幅の宣言子へ投影する
			bool IsSizedDeclarator = false;
			if(SizedDeclarator && NodeRoles.test(static_cast<size_t>(Role::ArrayDeclarator))) {
				for(const ParsedChild &Child : ParsedChildren) {
					if(Work) Work->Charge(1);
					if(
						const LexemeRestoration *const Original = SrcTree.GetRestorationAt(Child.GetStart(), Child.End);
						!Child.IsNamed && Original && Original->Original == ":"
					) IsSizedDeclarator = true;
				}
			}
			const bool IsRestoredInterpolation =
			Interpolation && OpeningRestoration && OpeningRestoration->Original == TreeReading.ValueInterpolationOpen;
			const TSSymbol ProjectedSymbol = IsSizedDeclarator ? SizedDeclarator : IsRestoredInterpolation ?
			Interpolation :
			IsSignedNum ? NumericUnary : IsGluedValName ? PlainVal : IsColonPair ? ColonPair : LiteralSymbol;
			Projected.Kind = { ProjectedSymbol == LiteralSymbol ? &LiteralKind : &Profile.GetKindOf(ProjectedSymbol) };
			if(!IsVerification) {
				Projected.Facts.Forms = Profile.GetFormsOf(ProjectedSymbol);
				Projected.Facts.BlankKey = Profile.GetBlankKeyOf(ProjectedSymbol);
			}
			if(NodeRoles.test(static_cast<size_t>(Role::ReturnHintContext))) {
				const TSNode Name = SrcTree.GetFieldChild(Node, ChildRole::Name);
				const std::string_view Text = ts_node_is_null(Name) ? std::string_view() : SrcTree.GetView(Name);
				if(Work) Work->Charge(NodeChildCount + Text.size() + 1);
				Projected.Kind = { &Profile.GetDeclarationKindOf(Node, Text) };
			}
			Projected.Parent = Top.Parent;
			Projected.Fields = Top.Fields;
			// 解析用のマーカーを本来のトークンへデコードしたテキスト（整形仕様 4.2）
			if(!HasChildren || IsVerbatim) Projected.Lexeme = GetVerbatimText(SrcTree, Separated, Literal);
			if(IsSignedDimension) Projected.Lexeme.insert(0, SrcTree.GetView(Dimension.first));
			const LexemeRestoration *const Restoration = HasChildren ? nullptr : SrcTree.GetRestorationAt(NodeStart, NodeRecord.End);
			// 解析用に補った閉区切は検証だけへ記録し，コメントを直前の幅へ渡す
			if(Restoration && Restoration->Original.empty()) {
				MoveAttached(Top.Node, Top.Parent, NoNode, LastProjected(Top.Parent, NoNode), false);
				continue;
			}
			// 識別子として読み直した語は，復元した綴りに依って演算子へ戻さない
			Projected.Facts.IsKeywordName = Restoration && Restoration->IsIdentifier;
			if(Restoration && !Projected.Facts.IsKeywordName) {
				const std::unordered_map<std::string_view, FormPart>::const_iterator Found =
				SrcTree.GetRules().TokenDefs.find(Projected.Lexeme);
				if(Found == SrcTree.GetRules().TokenDefs.end()) {
					throw FormatLimitExceeded("a restored token has no definition", FailureReason::FormatterDefect);
				}
				Projected.Kind = { Found->second.Def };
				const TSSymbol Restored = Profile.GetTokenSymbol(Projected.Lexeme);
				if(!Restored) throw FormatLimitExceeded("a restored token has no grammar symbol", FailureReason::FormatterDefect);
				if(!IsVerification) {
					Projected.Facts.Forms = Profile.GetFormsOf(Restored);
					Projected.Facts.BlankKey = Profile.GetBlankKeyOf(Restored);
				}
			}
			// 本体の場又は文・宣言列の独立したセミコロンを空文へ投影し，見出と宣言の区切を混同しない
			static constexpr uint64_t BodyFields =
			uint64_t{ 1 } << static_cast<uint8_t>(ChildRole::Alternative) | uint64_t{ 1 } << static_cast<uint8_t>(ChildRole::Body) |
			uint64_t{ 1 } << static_cast<uint8_t>(ChildRole::Consequence);
			const std::span<const NodeId> ProjectedSiblings = Syntax.GetChildrenOf(Top.Parent);
			const NodeId Previous = ProjectedSiblings.empty() ? NoNode : ProjectedSiblings.back();
			if(
				Projected.Kind.Def->Roles.test(static_cast<size_t>(Role::StmtTerminatorToken)) && Top.Parent != NoNode &&
				!(Syntax.Is(Previous, Role::TerminatedMember) && !Syntax.HasLastTokenRole(Previous, Role::StmtTerminatorToken)) && (
					Top.Fields & BodyFields || Syntax.Is(Top.Parent, Role::EmptyStmtContainer) &&
					!(Syntax.Is(Top.Parent, Role::EnumBodyDecls) && Syntax.GetChildrenOf(Top.Parent).empty())
				)
			) Projected.Kind = { &Profile.GetEmptyStmtKind() };
			// 本文の ASCII インデントの正規化
			if(Projected.Kind.Def->Roles.test(static_cast<size_t>(Role::CollapsibleText))) {
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
			}
			// プリプロセッサディレクティブの語は `#` と名前の間の空白を詰めた綴り（トークン間空白仕様 5.4）
			if(
				Top.Parent != NoNode && Syntax.Is(Top.Parent, Role::Preproc) && Projected.Lexeme.size() > 1 &&
				Projected.Lexeme.front() == '#' && (Projected.Lexeme[1] == ' ' || Projected.Lexeme[1] == '\t')
			) Projected.Lexeme.erase(1, Projected.Lexeme.find_first_not_of(" \t", 1) - 1);
			// 改行だけから成るトークン（プリプロセッサディレクティブを終える改行）は後続の空行を含み得る為，テキストを１つの改行にして空行を配置へ委ねる
			if(!HasChildren && !Projected.Lexeme.empty() && Projected.Lexeme.find_first_not_of("\r\n") == std::string::npos) {
				Projected.Lexeme = "\n";
			}
			if(IsVerbatim) Projected.Facts.Add(ConstraintKind::Verbatim);
			// 自己終了の JSX も要素の子に開始タグを置き，タグの中の既存の深さの範囲を保つ
			if(
				NodeRoles.test(static_cast<size_t>(Role::ExprTag)) && NodeRoles.test(static_cast<size_t>(Role::MarkupOpen)) &&
				NodeRoles.test(static_cast<size_t>(Role::SelfClosingTag))
			) {
				if(!MarkupElement) throw FormatLimitExceeded("a self-closing tag has no element definition", FailureReason::FormatterDefect);
				SyntaxNode Element;
				Element.Kind = { &Profile.GetKindOf(MarkupElement) };
				Element.Parent = Projected.Parent;
				Element.Fields = Projected.Fields;
				if(!IsVerification) {
					Element.Facts.Forms = Profile.GetFormsOf(MarkupElement);
					Element.Facts.BlankKey = Profile.GetBlankKeyOf(MarkupElement);
				}
				const NodeId ElementId = Syntax.AddNode(std::move(Element));
				RecordOrigin({ NoNode, NodeStart, NodeRecord.End }, MarkupElement);
				SeparatedBefore.push_back(false);
				if(Top.Parent == NoNode) Syntax.Root = ElementId;
				else {
					Syntax.Nodes[ElementId].Idx = static_cast<uint32_t>(Syntax.Nodes[Top.Parent].Children.size());
					Syntax.Nodes[Top.Parent].Children.push_back(ElementId);
				}
				Top.Parent = ElementId;
				Projected.Parent = ElementId;
				Projected.Fields = 0;
			}
			// 名前付の葉の語も，デコードした綴りに対応する固定の役割を投影時に確定する
			if(!Projected.Lexeme.empty()) Projected.Kind = { &Profile.GetLexemeKindOf(*Projected.Kind.Def, Projected.Lexeme) };
			const NodeId Id = Syntax.AddNode(std::move(Projected));
			uint32_t SrcIdx = static_cast<uint32_t>(RecordedIdx);
			if(!ts_node_eq(Node, Top.Node)) {
				SrcIdx = ExtraNodeBit | static_cast<uint32_t>(ExtraNodes.size());
				ExtraNodes.push_back(Node);
			}
			RecordOrigin({ SrcIdx, NodeStart, NodeRecord.End }, NodeSymbol);
			if(IsSignedDimension) Sources.back().Start = SrcTree.GetStart(Dimension.first);
			// 末尾のデータ領域は領域を開くマーカーの行から始まる（パーサーはマーカーをノードに含めない）
			if(NodeRoles.test(static_cast<size_t>(Role::TrailingDataSection))) {
				const std::string_view Code = SrcTree.GetCodeText();
				const uint32_t Start = Sources.back().Start;
				const size_t MarkEnd = !Start ? std::string_view::npos : Code.rfind('\n', Start - 1);
				const uint32_t LineStart = MarkEnd == std::string_view::npos ? 0 : static_cast<uint32_t>(MarkEnd + 1);
				Syntax.Nodes[Id].Lexeme.insert(0, Code.substr(LineStart, Start - LineStart));
				Sources.back().Start = LineStart;
				if(HaltedStart != std::string::npos) {
					// 文法の境界ノードの種別・空行キー・コメントのアンカーを保ち，解析から外したデータだけを共有境界からコピーする
					Syntax.Nodes[Id].Lexeme.append(SrcTree, HaltedStart);
					Sources.back().End = static_cast<uint32_t>(SrcTree.size());
					HasProjectedData = true;
				}
			}
			// マクロの本体は空白で区切られたトークンの並び（トークンの間は投影する境界が表す，トークン間空白仕様 5.4）
			if(NodeRoles.test(static_cast<size_t>(Role::MacroBody))) {
				const std::string Body = std::move(Syntax.Nodes[Id].Lexeme);
				Syntax.Nodes[Id].Lexeme.clear();
				// ヘッダ名を引数に取るディレクティブの引数か
				std::string_view Directive = SrcTree.GetView(SrcTree.GetChild(Top.ParentNode, 0));
				Directive.remove_prefix(std::min(Directive.find_first_not_of("# \t"), Directive.size()));
				const std::vector<std::string_view> &HeaderNameDirectives = TreeReading.HeaderNameDirectives;
				const bool IsHeaderName = std::ranges::find(HeaderNameDirectives, Directive) != HeaderNameDirectives.end();
				for(const std::pair<uint32_t, uint32_t> &Range : ParseAdapter::MacroBodyTokens(Body, IsHeaderName)) {
					const uint32_t From = Range.first, To = Range.second;
					SyntaxNode Token;
					Token.Kind = Syntax.Nodes[Id].Kind;
					Token.Parent = Id;
					Token.Lexeme = Body.substr(From, To - From);
					const NodeId TokenIdentifier = Syntax.AddNode(std::move(Token));
					RecordOrigin({ NoNode, Sources[Id].Start + From, Sources[Id].Start + To });
					SeparatedBefore.push_back(false);
					Syntax.Nodes[TokenIdentifier].Idx = static_cast<uint32_t>(Syntax.Nodes[Id].Children.size());
					Syntax.Nodes[Id].Children.push_back(TokenIdentifier);
					// テキストを持つ子のトークンで，保持コメントの範囲と完成候補の診断記録を確定する
					RecordKept(Sources[TokenIdentifier].Start, Sources[TokenIdentifier].End, TokenIdentifier);
				}
			}
			SeparatedBefore.push_back(IsSeparatorPending && (!HasChildren || IsVerbatim));
			if(!HasChildren || IsVerbatim) IsSeparatorPending = false;
			if(Top.Parent == NoNode) Syntax.Root = Id;
			else {
				Syntax.Nodes[Id].Idx = static_cast<uint32_t>(Syntax.Nodes[Top.Parent].Children.size());
				Syntax.Nodes[Top.Parent].Children.push_back(Id);
			}
			// Ruby の `do` の本体へ紐付いたコメントは，名前付の子が無ければ所有者の物とする
			// 此の本体は独立した出力位置を持たず，開始・終端の語は所有者が出力する
			if(
				TreeReading.HasKeywordClosedBlocks && NodeKind.IsNamed && NodeRoles.test(static_cast<size_t>(Role::IterationSection)) &&
				!CountCodeChildren(SrcTree, Separated, Node, true, nullptr) && Top.Parent != NoNode
			) MoveAttached(Node, Top.Parent, Top.Parent, LastProjected(Top.Parent, Id), false);
			// アンカーのノードを此のノードへコピーしたコメントの記録
			else for(const ParsedChild *const Src : { &TopRecord, ts_node_is_null(Comma) ? nullptr : &NodeRecord }) {
				if(!Src) continue;
				for(const std::vector<CommentRecord> *const Attached : { &Separated.GetLeading(*Src), &Separated.GetTrailing(*Src) }) {
					for(CommentRecord Record : *Attached) {
						Record.Anchor = Id;
						Result.Comments.Add(std::move(Record));
					}
				}
			}
			for(const TSNode GroupNode : Top.Groups) {
				for(
					const std::vector<CommentRecord> *const Attached :
					{ &Separated.GetLeading(GroupNode), &Separated.GetTrailing(GroupNode) }
				) for(CommentRecord Record : *Attached) {
					Record.Anchor = Id;
					Result.Comments.Add(std::move(Record));
				}
			}
			// C 系の数値に含まれる符号と被演算子の直接の投影（元のノードの検証データは１度だけ共有）
			if(IsSignedNum) {
				for(const bool IsSign : { true, false }) {
					const uint32_t Start = NodeStart + !IsSign;
					const std::string_view Text = NumText.substr(IsSign ? 0 : 1, IsSign ? 1 : std::string_view::npos);
					const TSSymbol Symbol = IsSign ? Profile.GetTokenSymbol(Text) : NodeSymbol;
					SyntaxNode Part;
					Part.Kind = { &Profile.GetKindOf(Symbol) };
					if(!IsVerification) {
						Part.Facts.Forms = Profile.GetFormsOf(Symbol);
						Part.Facts.BlankKey = Profile.GetBlankKeyOf(Symbol);
					}
					if(!IsSign && IsVerbatimLiteral) Part.Facts.Add(ConstraintKind::Verbatim);
					Part.Parent = Id;
					Part.Fields = Syntax.GetFieldBit(IsSign ? ChildRole::Operator : ChildRole::Other);
					Part.Idx = !IsSign;
					Part.Lexeme = Text;
					const NodeId PartIdentifier = Syntax.AddNode(std::move(Part));
					RecordOrigin({ IsSign ? NoNode : SrcIdx, Start, IsSign ? Start + 1 : NodeRecord.End }, NodeSymbol);
					SeparatedBefore.push_back(IsSign && IsSeparatorPending);
					Syntax.Nodes[Id].Children.push_back(PartIdentifier);
				}
				IsSeparatorPending = false;
			}
			// 数値のテキストへ集約した部品の検証データとコメントのアンカーの移譲
			if(IsSignedNum || IsSignedDimension) {
				RecordDescendants(NodeRecord);
				const NodeId Anchor = IsSignedNum ? Syntax.Nodes[Id].Children.back() : Id;
				for(const ParsedChild &Child : SrcTree.GetChildrenAt(Relation)) MoveAttached(Child.Node, Anchor, NoNode, Anchor, true);
				continue;
			}
			// テキストを持つ葉と逐語領域で，保持コメントの範囲を確定する
			if(IsVerbatim || NodeRoles.test(static_cast<size_t>(Role::Comment))) RecordKept(NodeStart, NodeRecord.End, Id);
			// 子の無いトークンの終了
			if(!HasChildren) continue;
			if(IsVerbatim) {
				RecordDescendants(NodeRecord);
				// テキストを保つ領域の中へ紐付いたコメントは置場を失う
				for(const ParsedChild &Child : ParsedChildren) if(HasAttached(Child.Node)) {
					throw FormatLimitExceeded("a comment is attached inside a verbatim region", FailureReason::FormatterDefect);
				}
				continue;
			}
			// 子を原文順に訪ねる為の逆順の積上（エラーを含まないノードは子の間の隠れたトークンも子にし，エラーを含むノードの読み飛ばしたトークンは保つ境界がコピーする）
			Children.clear();
			const bool HasHiddenText = !HasTreeError || !NodeRecord.HasError;
			uint32_t From = NodeStart;
			// パーサーが読み飛ばしたトークンの追加
			const auto AddHidden = [&](const uint32_t To) -> void {
				// 隠れたトークンを探さないエラーを含むノードの場合の終了
				if(!HasHiddenText) return;
				for(const std::pair<uint32_t, uint32_t> &Range : HiddenTokens(SrcTree, From, To, Gaps)) {
					// 文を終える隠れた `;` は文境界でトークンにせず（文の並びは文を改行で区切，改行で繋がる文にだけ `;` を置く），文法のトークンに無いテキスト（文字列の引用符等）は構文事実の保つ間隙としてコピーする
					if(!IsTokenizedHidden(SrcTree, Node, std::string_view(SrcTree).substr(Range.first, Range.second - Range.first))) continue;
					Children.push_back({ TSNode{}, Id, 0, Range, Node });
				}
				// 終了
				return;
			};
			SrcTree.PrepareChildFields(Node, Relation);
			for(const ParsedChild &Part : ParsedChildren) {
				const TSNode Child = Part.Node;
				AddHidden(Part.GetStart());
				if(!Profile.GetKindOf(Part.Symbol).Roles.test(static_cast<size_t>(Role::LineContinuation))) {
					Children.push_back({ Child, Id, Part.Fields, {}, Node });
					// 無名の幅指定も，名前付の宣言子と同じ寸法の場へ揃える
					if(NodeRoles.test(static_cast<size_t>(Role::SizedDeclarator)) && Part.IsNamed) {
						Children.back().Fields = Syntax.GetFieldBit(ChildRole::Length);
					}
					Children.back().Symbol = Part.Symbol;
					Children.back().Relation = Part.Relation;
				} else {
					Frame PendingValidation { Child, Id, 0, {}, Node };
					PendingValidation.IsValidationOnly = true;
					PendingValidation.Relation = Part.Relation;
					PendingValidation.Symbol = Part.Symbol;
					Children.push_back(std::move(PendingValidation));
				}
				From = std::max(From, Part.End);
			}
			AddHidden(NodeRecord.End);
			Syntax.Nodes[Id].Children.reserve(Children.size());
			// 分離検査の逐語所属だけを継ぎ，投影の子・フィールドの選択を保つ
			IsInVerbatim = IsInVerbatim || IsVerbatimCommentHost(Node, NodeSymbol);
			for(Frame &Child : Children) Child.IsInVerbatim = IsInVerbatim;
			Stack.insert(Stack.end(), std::make_move_iterator(Children.rbegin()), std::make_move_iterator(Children.rend()));
		}
		// 解析用括弧の照合と走査領域の解放
		if(std::ranges::find(ConsumedGroups, uint8_t{ 0 }) != ConsumedGroups.end()) {
			throw FormatLimitExceeded("a synthetic group has no parser wrapper", FailureReason::FormatterDefect);
		}
		std::vector<Frame>().swap(Stack);
		std::vector<Frame>().swap(Children);
		std::vector<uint8_t>().swap(ConsumedGroups);
		// PHP の本体の外の地の文（先頭の空白と閉タグの後の地の文）の根の逐語の子としての保持（整形仕様 4.4）
		if(BodyStart || HasOuterTail) {
			// 外側の本文の領域の記録
			const auto AddOuterText = [&](const uint32_t Start, const uint32_t End, const bool IsFirst) -> void {
				// 地の文の逐語ノードの追加
				SyntaxNode Outer;
				Outer.Kind = { &Profile.GetOuterTextKind() };
				Outer.Parent = Syntax.Root;
				Outer.Lexeme = ParseAdapter::DecodeLexeme(SrcTree, std::string(SrcTree, Start, End - Start), Start);
				Outer.Facts.Add(ConstraintKind::Verbatim);
				const NodeId Id = Syntax.AddNode(std::move(Outer));
				RecordOrigin({ NoNode, Start, End });
				SeparatedBefore.push_back(false);
				std::pmr::vector<NodeId> &Children = Syntax.Nodes[Syntax.Root].Children;
				Children.insert(IsFirst ? Children.begin() : Children.end(), Id);
				for(uint32_t Idx = 0; Idx < Children.size(); ++Idx) Syntax.Nodes[Children[Idx]].Idx = Idx;
				// 終了
				return;
			};
			if(BodyStart) AddOuterText(0, BodyStart, true);
			if(HasOuterTail) AddOuterText(BodyEnd, CodeEnd, false);
		}
		// 文法が境界ノードを持たない実行停止後のデータだけを，根の末尾の逐語領域へコピーする
		if(HaltedStart != std::string::npos && !HasProjectedData) {
			SyntaxNode Data;
			Data.Kind = { &Profile.GetTrailingDataKind() };
			Data.Parent = Syntax.Root;
			Data.Lexeme.assign(SrcTree, HaltedStart);
			Data.Facts.Add(ConstraintKind::Verbatim);
			const NodeId Id = Syntax.AddNode(std::move(Data));
			RecordOrigin({ NoNode, CodeEnd, static_cast<uint32_t>(SrcTree.size()) });
			SeparatedBefore.push_back(false);
			std::pmr::vector<NodeId> &RootChildren = Syntax.Nodes[Syntax.Root].Children;
			Syntax.Nodes[Id].Idx = static_cast<uint32_t>(RootChildren.size());
			RootChildren.push_back(Id);
		}
		// 省略可能な節の区切語に依り本体へ取り込まれたヒアドキュメントの見出直後への移動
		if(TreeReading.HasKeywordClosedBlocks) for(NodeId Id = 0; Id < Syntax.GetNodeCount(); ++Id) {
			if(!Syntax.Is(Id, Role::KeywordBlock)) continue;
			std::pmr::vector<NodeId> &Children = Syntax.Nodes[Id].Children;
			for(size_t Idx = 0; Idx < Children.size(); ++Idx) {
				const NodeId Body = Children[Idx];
				if(!Syntax.Is(Body, Role::StmtSeq)) continue;
				std::pmr::vector<NodeId> &Parts = Syntax.Nodes[Body].Children;
				size_t Count = 0;
				while(Count < Parts.size() && Syntax.Is(Parts[Count], Role::HeredocBody)) ++Count;
				if(!Count) continue;
				for(size_t Part = 0; Part < Count; ++Part) Syntax.Nodes[Parts[Part]].Parent = Id;
				Children.insert(Children.begin() + Idx, Parts.begin(), Parts.begin() + Count);
				Parts.erase(Parts.begin(), Parts.begin() + Count);
				Sources[Body].Start = Parts.empty() ? Sources[Body].End : Sources[Parts.front()].Start;
				for(uint32_t Part = 0; Part < Parts.size(); ++Part) Syntax.Nodes[Parts[Part]].Idx = Part;
				for(uint32_t Part = 0; Part < Children.size(); ++Part) Syntax.Nodes[Children[Part]].Idx = Part;
				Idx += Count;
			}
		}
		RebindParenthesizedDecls(SrcTree, Profile, ValidationNodes, ExtraNodes, IsVerification, Sources, Result);
		// 検証データと構文事実の組立
		std::unordered_set<const void *> BrokenMissingParents, BrokenSubtrees;
		std::vector<std::pair<uint32_t, uint32_t>> ErrorRanges;
		ValidationContext Validation = CollectCtxFromNodes(
			SrcTree,
			Profile,
			Input,
			ValidationNodes,
			TemplateDecls,
			&BrokenMissingParents,
			&BrokenSubtrees,
			&ErrorRanges,
			&ValidationComments,
			Hidden
		);
		// エラー本体と投影したトークン・逐語領域の対応（保持条件の隣接トークンは正常領域の照合から除かない）
		if(!ErrorRanges.empty()) {
			Syntax.ErrorZoneOwners.resize(ErrorRanges.size());
			std::vector<std::pair<uint32_t, uint32_t>> OwnerRanges(ErrorRanges.size(), { UINT32_MAX, 0 });
			bool HasOverhang = false;
			for(NodeId Id = 0; Id < Sources.size(); ++Id) {
				if(!Syntax.Nodes[Id].Children.empty()) continue;
				const Origin &Src = Sources[Id];
				std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Range = std::upper_bound(
					ErrorRanges.begin(),
					ErrorRanges.end(),
					Src.Start,
					[](const uint32_t Pos, const std::pair<uint32_t, uint32_t> &Bounds) -> bool {
						// トークンの前に終わらない範囲かの返戻
						return Pos < Bounds.second;
					}
				);
				for(; Range != ErrorRanges.end() && Range->first < Src.End; ++Range) {
					const size_t Idx = static_cast<size_t>(Range - ErrorRanges.begin());
					Syntax.ErrorZoneOwners[Idx].push_back(Id);
					OwnerRanges[Idx].first = std::min(OwnerRanges[Idx].first, Src.Start);
					OwnerRanges[Idx].second = std::max(OwnerRanges[Idx].second, Src.End);
					HasOverhang = HasOverhang || Src.Start < Range->first || Src.End > Range->second;
				}
			}
			// １つの逐語ノードがエラー本文の外も含む場合だけ，本文との範囲差を控える
			if(HasOverhang) {
				// 領域が同じ所有ノードに複数在っても，原文を１度だけ走査して位置を得る
				std::vector<uint32_t> Positions(SrcTree.size() + 1, 0);
				const std::vector<uint32_t> &Inserted = SrcTree.GetInsertedBreaks();
				const std::vector<std::pair<uint32_t, uint32_t>> &Comments = Validation.InputDiagnostics.CommentRanges;
				size_t Comment = 0, Break = 0;
				for(size_t Pos = 0; Pos < SrcTree.size(); ++Pos) {
					while(Comment < Comments.size() && Comments[Comment].second <= Pos) ++Comment;
					while(Break < Inserted.size() && Inserted[Break] < Pos) ++Break;
					const bool IsExcluded =
					Comment < Comments.size() && Comments[Comment].first <= Pos || Break < Inserted.size() && Inserted[Break] == Pos;
					Positions[Pos + 1] = Positions[Pos] + (!IsExcluded && !std::isspace(static_cast<unsigned char>(SrcTree[Pos])));
				}
				for(size_t Idx = 0; Idx < ErrorRanges.size(); ++Idx) {
					const auto [Start, End] = OwnerRanges[Idx];
					if(Start == UINT32_MAX) continue;
					ValidationContext::ErrorZone &Zone = Validation.ErrorZones[Idx];
					if(Start < ErrorRanges[Idx].first) Zone.OwnerPrefix = Positions[ErrorRanges[Idx].first] - Positions[Start];
					if(End > ErrorRanges[Idx].second) Zone.OwnerSuffix = Positions[End] - Positions[ErrorRanges[Idx].second];
				}
			}
		}
		// ディレクティブの保持値は両側で収集し，完成候補の出所は静的解析の遅延処理へ渡す
		Validation.ProtectedRegions.Directives =
		CollectDirectives(SrcTree, Separated.Head, IsVerification ? &Validation.ProtectedRegions.DirectiveStarts : nullptr);
		Validation.ProtectedRegions.KeptComments = std::move(Result.Validation.ProtectedRegions.KeptComments);
		Result.Validation = std::move(Validation);
		RecordFacts(
			SrcTree,
			Syntax,
			Profile,
			Separated,
			Sources,
			Symbols,
			ValidationNodes,
			ExtraNodes,
			BrokenMissingParents,
			BrokenSubtrees,
			SeparatedBefore,
			IsVerification,
			Input.HasFilePreamble,
			Gaps,
			Work
		);
		if(TreeReading.HasEmbeddedScripts) RecordMarkupFacts(SrcTree, Syntax, Sources, Input.HasFilePreamble);
		RecordOwnedRegions(SrcTree, Sources, ValidationNodes, ExtraNodes, Owned, Result);
		if(Profile.GetRules().HasDeferredLiteralBodies) RebindDeferredLiteralBodies(Syntax, Sources);
		std::vector<uint8_t>().swap(SeparatedBefore);
		// 文法の定義するパーサーの読違の結付の復元（整形仕様 4.3，意味の異なる構文を同じ表現へ投影しない）
		Syntax.ImplicitFunctionNameIdx = Syntax.InternCmpName(std::string(Syntax.GetRules().ImplicitFunctionBinding), true);
		if(Rules.DoesMisbindEmbeddedBranches) RebindEmbeddedBranches(Syntax, Rules, IsVerification);
		RebindBinaryOperators(Syntax, Rules, Sources);
		if(Rules.DoesMisbindPostfixOperand) RebindSwiftPostfix(Syntax);
		RecordImportFields(Syntax, !IsVerification);
		RecordImportScopes(Syntax);
		RecordNames(Syntax, Rules, !IsVerification, true);
		ReferenceProjection References(Profile, Syntax);
		References.RecordImports();
		RecordMemberRoles(Profile, Syntax, false);
		std::vector<const TSNode *>().swap(ValidationNodes);
		std::deque<TSNode>().swap(ContextNodes);
		std::vector<TSNode>().swap(ExtraNodes);
		// 入力側の同期が使うドキュメント化対象の登録（完成候補の説明対象は GetDocTargets が独立して求める）
		if(!std::holds_alternative<ParseInput::VerificationSrc>(Input.Origin)) {
			for(NodeId Id = 0; Id < Syntax.GetNodeCount(); ++Id) if(SignatureExtractor::GetDocumentationFact(Syntax, Id).IsDocumentable) {
				Result.Comments.AddDocSite({ Id, &Profile.GetRules().Notation });
			}
		}
		// 記録済の記号は同じ列の中で確定し，隠れたトークンだけ綴りと投影先の種別から補う
		Symbols.resize(Syntax.GetNodeCount(), std::numeric_limits<TSSymbol>::max());
		for(NodeId Id = 0; Id < Sources.size(); ++Id) {
			const TSSymbol Original = Symbols[Id];
			if(
				Sources[Id].NodeIdx != NoNode || Original && &Profile.GetKindOf(Original) == Profile.GetBaseKindOf(Syntax.Nodes[Id].Kind.Def)
			) continue;
			Symbols[Id] = std::numeric_limits<TSSymbol>::max();
			if(Sources[Id].Start > Sources[Id].End || Sources[Id].End > SrcTree.size()) continue;
			const std::string_view Raw = std::string_view(SrcTree).substr(Sources[Id].Start, Sources[Id].End - Sources[Id].Start);
			if(
				const TSSymbol Symbol = Profile.GetTokenSymbol(Raw);
				Symbol && &Profile.GetKindOf(Symbol) == Profile.GetBaseKindOf(Syntax.Nodes[Id].Kind.Def)
			) Symbols[Id] = Symbol;
		}
		Result.Validation.SemanticChecks.Symbols = std::move(Symbols);
		if(std::holds_alternative<ParseInput::VerificationSrc>(Input.Origin)) {
			// 最初の投影の記録を原文順に並べ，記録毎の範囲も同じ順で渡す
			std::stable_sort(
				Result.Comments.Records.begin(),
				Result.Comments.Records.end(),
				[&Separated](const CommentRecord &Left, const CommentRecord &Right) -> bool {
					// 分離した記録とソース側に残る記録を合わせた原文の出現順の返戻
					return Separated.RecordRanges[Left.OriginalOrder].first < Separated.RecordRanges[Right.OriginalOrder].first;
				}
			);
			for(CommentRecord &Record : Result.Comments.Records) {
				Result.Validation.InputDiagnostics.CommentRecords.push_back(Separated.RecordRanges.at(Record.OriginalOrder));
				Record.OriginalOrder = Result.Validation.InputDiagnostics.CommentRecords.size() - 1;
			}
			Result.Comments.Reindex();
			// 完成候補のドキュメントの説明対象と構造（整形仕様 7.4，8.1，期待コメントから同じ規則で集めた物と検証が比べる）
			Result.Comments.DocTargets = DocCommentReader::GetDocTargets(Syntax, Result.Comments, &Result.Comments.TargetTables);
			Result.Validation.ProtectedRegions.Docs =
			DescribeDocs(Syntax, Result.Comments.GetRecords(), Result.Comments.GetDocTargets(), Hidden ? &Hidden->Docs : nullptr);
			// 完成候補の行末コメントの前置区切の形（期待コメントの記録と検証が照合する，整形仕様 7.6，8.1）
			Result.Validation.InputDiagnostics.LeadingSeparators = std::move(Separated.LeadingSeparators);
		} else Result.Comments.Reindex();
		// 投影と検証データの収集を終えた構文の基準量からの構文の正規化と中間表現の構築の作業単位の上限と，構造の段数の上限（整形仕様 9.2）
		Syntax.FixWorkLimit();
		// 入力側の名前の探索と修復候補の実際の訪問を，確定した中核の予算へ引き継ぐ
		Syntax.Charge(SrcTree.GetNameWork());
		Syntax.Charge(SrcTree.GetBoundaryRepairWork());
		Syntax.Charge(SrcTree.GetCastRepairWork());
		Syntax.Charge(SrcTree.GetPrefixTypeRepairWork());
		Syntax.CheckNesting();
		if(IsVerification && NeedsPositions) {
			// 静的解析又は部分整形の照合が読む検証の構文の診断位置の投影（準備・修復前の本文の位置，行と桁は本文を持つ呼出側が求める）
			Result.Validation.InputDiagnostics.Positions.resize(Syntax.GetNodeCount());
			for(NodeId Id = 0; Id < Sources.size(); ++Id) {
				Result.Validation.InputDiagnostics.Positions[Id] =
				{ SrcTree.OffsetInInput(Sources[Id].Start), SrcTree.OffsetInInput(Sources[Id].End), {} };
			}
			for(NodeId Id = static_cast<NodeId>(Syntax.GetNodeCount()); Id-- > Sources.size();) {
				if(const std::span<const NodeId> Children = Syntax.GetChildrenOf(Id); !Children.empty()) {
					Result.Validation.InputDiagnostics.Positions[Id] = Result.Validation.InputDiagnostics.Positions[Children.front()];
					Result.Validation.InputDiagnostics.Positions[Id].End = Result.Validation.InputDiagnostics.Positions[Children.back()].End;
				}
			}
		}
		if(IsVerification) {
			Result.PrepareLint = DeferLintFacts(
				SrcTree,
				std::move(Sources),
				Result.Validation.InputDiagnostics.CommentRecords,
				std::move(Result.Validation.ProtectedRegions.DirectiveStarts)
			);
		}
		// 投影中の構造の計数を終了し，完成した子列だけを後段で共有する
		Syntax.NamedChildCounts.clear();
		// 投影の結果の返戻
		return Result;
	}

	/**
	 * 解析用の入力を手放した後の検証ビューの寿命の確保関数
	 * パーサーが宣言の値の中の行コメントの後の規則を取り込む文法では，保護領域の入力テキストを結果の所有へコピーする
	 * 計算量：入力の UTF-8 バイト数 B に対し O(B)
	 * @param Profile 入力で確定した文法
	 * @param Result 投影の結果（保護領域の入力のビューを結果の所有へ付け替える）
	 * @throws std::bad_alloc 入力テキストのコピーのメモリを確保出来ない場合
	 * @throws std::length_error テキストの要素数上限を超える場合
	 */
	void SourceProjector::RetainValidationInput(const GrammarProfile &Profile, ProjectionResult &Result) {
		if(Profile.GetReading().CanAbsorbRulesIntoValComments) {
			Result.Validation.InputOwner = std::make_shared<const std::string>(Result.Validation.ProtectedRegions.InputText);
			Result.Validation.ProtectedRegions.InputText = *Result.Validation.InputOwner;
		}
		// 終了
		return;
	}

	/**
	 * 構文・コメント・検証データへの投影関数（整形仕様 3.4，4.3，7.1）
	 * 原文のコメントをアンカーへ紐付けて分離し，構文を独自のノード表へコピーする
	 * 計算量：入力の UTF-8 バイト数 B と各投影工程の総費用 Q に対し O(B + Q)
	 * @param SrcTree 解析済のソース（書き換えない）
	 * @param Profile 入力で確定した文法
	 * @param Input 適応済の入力と修復前のテキスト
	 * @param NeedsPositions 完成候補で静的解析又は部分整形の照合用のノードの位置を記録するか
	 * @return ノード表・コメント・検証データと診断位置
	 * @throws FormatLimitExceeded 分離の対象のコメントのノードが残る場合，子の所有経路が不完全・重複・欠落する場合，投影した構造・束縛の収集がリソース上限を超える場合，又は書換の生成形式・前提が不正な場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::exception 呼出先の領域の整形が失敗し，其の例外が伝わる場合
	 * @throws std::invalid_argument 呼出先へ渡す投影又は検証の引数が契約を満たさない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	ProjectionResult SourceProjector::Project(
		const ::ParsedTree &SrcTree,
		const GrammarProfile &Profile,
		const ParseInput &Input,
		const bool NeedsPositions
	) {
		// 原文のコメントの分離
		CommentSeparation Separated = SeparateComments(SrcTree, Input);
		// 投影結果の所有と，解析用の入力を手放した後の検証ビューの寿命の確保
		ProjectionResult Result = ProjectNodes(SrcTree, Profile, Input, Input.Owned, Separated, NeedsPositions);
		RetainValidationInput(Profile, Result);
		// 構文と同じ解析結果から組み立てたデータの返戻
		return Result;
	}
} // namespace Core
