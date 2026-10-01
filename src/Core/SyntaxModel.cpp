#include "SyntaxModel.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 使用可能な記法かの判定関数（整形仕様 3.2）
	 * @param Notation コメントの記法
	 * @return 文法が記法を読むなら true
	 */
	bool NotationRules::IsAvailable(const CommentNotation Notation) const {
		// 使用可能な記法の有無の返戻
		return std::ranges::find(AvailableNotations, Notation, &AvailableNotation::Notation) != AvailableNotations.end();
	}

	/**
	 * 文書の記法の文書形式の取得関数（整形仕様 3.2）
	 * @param Notation コメントの記法
	 * @return 記法を文書に使う時の入力の文書形式（文書に使わない記法は無し）
	 */
	std::optional<DocumentFormId> NotationRules::DocumentFormOf(const CommentNotation Notation) const {
		const std::vector<std::pair<CommentNotation, DocumentFormId>>::const_iterator Found =
		std::ranges::find(DocumentForms, Notation, &std::pair<CommentNotation, DocumentFormId>::first);
		// 記法の文書形式の返戻（文書に使わない記法は無し）
		return Found == DocumentForms.end() ? std::nullopt : std::optional<DocumentFormId>(Found->second);
	}

	/**
	 * 用途の採用記法の取得関数（整形仕様 3.2, 7.6）
	 * @param Use コメントの用途
	 * @return 用途の採用記法（其の用途の記法を揃えない文法は無し）
	 */
	const CommentOutputForm *NotationRules::AdoptedFormOf(const CommentUse Use) const {
		const std::vector<CommentOutputForm>::const_iterator Found = std::ranges::find(OutputForms.Forms, Use, &CommentOutputForm::Use);
		// 用途の採用記法の返戻（無ければ無し）
		return Found == OutputForms.Forms.end() ? nullptr : &*Found;
	}

	/**
	 * 字面が語の意味の綴りかの判定関数
	 * @param Text 字面
	 * @param Meaning 語の意味
	 * @return 文法が其の意味に定める綴りの何れかと一致すれば true（綴りを持たない意味は false，大小を区別しない語は大小を問わない）
	 */
	bool SyntaxRules::IsSpelling(const std::string_view Text, const Word Meaning) const {
		const std::vector<std::string_view> &Spellings = Words[static_cast<size_t>(Meaning)];
		// 大文字小文字を区別する語の綴りの何れかとの一致の返戻
		if(!CaseInsensitiveWords.test(static_cast<size_t>(Meaning))) return std::ranges::find(Spellings, Text) != Spellings.end();
		// 大文字小文字を区別しない語の綴りの何れかとの一致の返戻
		return std::ranges::any_of(
			Spellings,
			[Text](const std::string_view Candidate) -> bool {
				// 英字の大小を揃えた綴りの一致の返戻
				return std::ranges::equal(
					Text,
					Candidate,
					[](const char Left, const char Right) -> bool {
						// 英字の大小を揃えた字の一致の返戻
						return std::tolower(static_cast<unsigned char>(Left)) == std::tolower(static_cast<unsigned char>(Right));
					}
				);
			}
		);
	}

	/**
	 * 語の意味の生成する綴りの取得関数
	 * @param Meaning 語の意味
	 * @return 文法が其の意味に定める最初の綴り（綴りを持たなければ空）
	 */
	std::string_view SyntaxRules::Spelling(const Word Meaning) const {
		const std::vector<std::string_view> &Spellings = Words[static_cast<size_t>(Meaning)];
		// 最初の綴りの返戻
		return Spellings.empty() ? std::string_view() : Spellings.front();
	}

	/**
	 * 字が演算子の融合に関わるかの判定関数
	 * @param Char 境界の字
	 * @return 演算子の融合に関われば true
	 */
	bool SyntaxRules::FusesOperatorChar(const char Char) const {
		// 融合する字の返戻
		return OperatorFusionChars.find(Char) != std::string_view::npos;
	}

	/**
	 * 代入の字句と隣接する字が融合するかの判定関数
	 * @param Before 代入の前の字
	 * @param After 代入の後の字
	 * @return 何れかの側で融合すれば true
	 */
	bool SyntaxRules::FusesAssignment(const char Before, const char After) const {
		// 左右の何れかで融合するかの返戻
		return Before && AssignmentFusionBefore.find(Before) != std::string_view::npos ||
		After && AssignmentFusionAfter.find(After) != std::string_view::npos;
	}

	/**
	 * 範囲の区切り同士が融合するかの判定関数
	 * @param Before 前の字句の最後の字
	 * @param After 後の字句の最初の字
	 * @return 別の範囲字句へ融合すれば true
	 */
	bool SyntaxRules::FusesRangePair(const char Before, const char After) const {
		const char Pair[] { Before, After };
		// 融合する範囲の組の返戻
		return FusedRangePairs.contains(std::string_view(Pair, 2));
	}

	/**
	 * 語の演算子へ融合する英字かの判定関数
	 * @param Char 境界の字
	 * @return 英字なら true
	 */
	bool SyntaxRules::IsFusionLetter(const char Char) {
		// 英字かの返戻
		return std::isalpha(static_cast<unsigned char>(Char));
	}

	/**
	 * 名前へ融合する英数字かの判定関数
	 * @param Char 境界の字
	 * @return 英数字なら true
	 */
	bool SyntaxRules::IsFusionAlnum(const char Char) {
		// 英数字かの返戻
		return std::isalnum(static_cast<unsigned char>(Char));
	}

	/**
	 * 演算子名へ融合する英字又は下線かの判定関数
	 * @param Char 境界の字
	 * @return 英字又は下線なら true
	 */
	bool SyntaxRules::IsFusionNameStart(const char Char) {
		// 演算子名を始める字かの返戻
		return IsFusionLetter(Char) || Char == '_';
	}

	/**
	 * 隣接する字句が次元値等へ融合するかの判定関数
	 * @param Before 前の字句の最後の字
	 * @param After 後の字句の綴り
	 * @return 融合する組なら true
	 */
	bool SyntaxRules::FusesDimensionTokens(const char Before, const std::string_view After) {
		// 後の字句の無い事の返戻
		if(After.empty()) return false;
		const unsigned char Last = static_cast<unsigned char>(Before), First = static_cast<unsigned char>(After.front());
		const unsigned char Second = After.size() > 1 ? static_cast<unsigned char>(After[1]) : '\0';
		// 名前・数の字かの判定の定義
		const auto IsNameChar = [](const unsigned char Char) -> bool {
			// 英数字・下線・連結符・非 ASCII・逃避の返戻
			return std::isalnum(Char) || Char == '_' || Char == '-' || Char > 0X7F || Char == '\\';
		};
		// 後の字句が数で始まるか
		const bool StartsNumber = std::isdigit(First) || First == '.' && std::isdigit(Second);
		// 融合する組かの返戻
		return IsNameChar(Last) &&
		(IsNameChar(First) || First == '%' || StartsNumber || First == '+' && (std::isdigit(Second) || Second == '.')) ||
		(Last == '+' || Last == '.') && StartsNumber ||
		(Last == '#' || Last == '@' || Last == '$' || Last == '%') && IsNameChar(First) || Last == ':' && First == ':' ||
		Last == '|' && First == '|' ||
		std::string_view("~|^$*").find(static_cast<char>(Last)) != std::string_view::npos && First == '=' ||
		Last == '/' && (First == '*' || First == '/') || Last == '*' && First == '/' || Last == '<' && First == '!';
	}

	/**
	 * 生成形式の役割に対応する部品の取得関数
	 * @param Role 子の役割
	 * @return 部品（無ければ無し）
	 */
	const FormPart *CanonicalForm::PartOf(const FormPartRole Role) const {
		// 役割の一致する部品の返戻
		for(const FormPart &Part : Parts) if(Part.Role == Role) return &Part;
		// 部品の無い事の返戻
		return nullptr;
	}

	/**
	 * 所有者の役割に形式を適用出来るかの判定関数
	 * @param Roles 所有者の構文上の役割
	 * @return 適用出来れば true
	 */
	bool CanonicalForm::AppliesTo(const RoleSet &Roles) const {
		// 定義した役割と一致するかの返戻
		return (ApplicableRoles & Roles).any();
	}

	/**
	 * 未包装の本体を波括弧で包めるかの判定関数
	 * @param IsBraced 本体に波括弧が既に有るか
	 * @return 包めるなら true
	 */
	bool CanonicalForm::CanWrapBody(const bool IsBraced) const {
		// 包装を必要とする形式かの返戻
		return Operation == FormOperation::BraceBody && Condition == FormCondition::UnbracedBody && !IsBraced;
	}

	/**
	 * 展開した並びの閉じの前にカンマを生成出来るかの判定関数
	 * @param IsBroken 並びを複数行へ展開したか
	 * @param HasLastElement 閉じの直前に要素が在るか
	 * @param Closer 閉じの字句
	 * @return カンマを生成出来れば true
	 */
	bool CanonicalForm::CanAddTrailingComma(const bool IsBroken, const bool HasLastElement, const char Closer) const {
		// 展開形の有効な閉じの前に要素が在るかの返戻
		return Operation == FormOperation::TrailingComma && Condition == FormCondition::BrokenListWithLastElement &&
		PartOf(FormPartRole::LastElement) && PartOf(FormPartRole::Close) && IsBroken && HasLastElement &&
		ClosingLexemes.find(Closer) != std::string_view::npos;
	}

	/**
	 * 空の実引数を除いて結果の種別へ組み直せるかの判定関数
	 * @param IsEmpty 実引数の並びが空か
	 * @return 結果の種別を持つ形式で，実引数が空なら true
	 */
	bool CanonicalForm::CanOmitEmptyArguments(const bool IsEmpty) const {
		const FormPart *const Result = PartOf(FormPartRole::Result);
		// 空の実引数を持ち，結果の種別を定義した形式かの返戻
		return Operation == FormOperation::OmitEmptyArguments && Condition == FormCondition::EmptyArguments && IsEmpty && Result &&
		Result->Definition;
	}

	/**
	 * 生成形式の並びの取得関数
	 * @return 記号毎の生成形式の定義（無ければ空）
	 */
	std::span<const CanonicalForm> CanonicalFormList::View() const {
		// 定義の並びの返戻
		return Definitions ? std::span<const CanonicalForm>(*Definitions) : std::span<const CanonicalForm>();
	}

	/**
	 * 生成形式の並びの先頭の取得関数
	 * @return 並びの先頭
	 */
	std::span<const CanonicalForm>::iterator CanonicalFormList::begin() const {
		// 並びの先頭の返戻
		return View().begin();
	}

	/**
	 * 生成形式の並びの終端の取得関数
	 * @return 並びの終端
	 */
	std::span<const CanonicalForm>::iterator CanonicalFormList::end() const {
		// 並びの終端の返戻
		return View().end();
	}

	/**
	 * コンストラクタ（記号毎の定義を参照）
	 * @param Forms 文法の定義が所有する記号毎の生成形式（参照の間は変更しない）
	 */
	CanonicalFormList::CanonicalFormList(const std::vector<CanonicalForm> &Forms) : Definitions(&Forms) {}

	/**
	 * 境界の並びの取得関数
	 * @return 子の番号順の境界（持たなければ空）
	 */
	std::span<const SemanticBoundary> SemanticBoundaryList::View() const {
		// 境界の並びの返戻
		return Entries ? std::span<const SemanticBoundary>(*Entries) : std::span<const SemanticBoundary>();
	}

	/**
	 * 境界の並びの先頭の取得関数
	 * @return 並びの先頭
	 */
	std::span<const SemanticBoundary>::iterator SemanticBoundaryList::begin() const {
		// 並びの先頭の返戻
		return View().begin();
	}

	/**
	 * 境界の並びの終端の取得関数
	 * @return 並びの終端
	 */
	std::span<const SemanticBoundary>::iterator SemanticBoundaryList::end() const {
		// 並びの終端の返戻
		return View().end();
	}

	/**
	 * 変更する境界の並びの取得関数
	 * @return 子の番号順の境界（持たなければ空の並びを確保した物）
	 */
	std::vector<SemanticBoundary> &SemanticBoundaryList::Edit() {
		if(!Entries) Entries = std::make_unique<std::vector<SemanticBoundary>>();
		// 境界の並びの返戻
		return *Entries;
	}

	/**
	 * コピー代入演算子
	 * @param Other 写す境界の並び
	 * @return 自身
	 */
	SemanticBoundaryList &SemanticBoundaryList::operator=(const SemanticBoundaryList &Other) {
		Entries = Other.Entries ? std::make_unique<std::vector<SemanticBoundary>>(*Other.Entries) : nullptr;
		// 自身の返戻
		return *this;
	}

	/**
	 * コンストラクタ（空の並びは領域を確保しない）
	 * @param Boundaries 子の番号順の境界
	 */
	SemanticBoundaryList::SemanticBoundaryList(std::vector<SemanticBoundary> &&Boundaries) : Entries(
		Boundaries.empty() ? nullptr : std::make_unique<std::vector<SemanticBoundary>>(std::move(Boundaries))
	) {}

	/**
	 * コピーコンストラクタ
	 * @param Other 写す境界の並び
	 */
	SemanticBoundaryList::SemanticBoundaryList(const SemanticBoundaryList &Other) : Entries(
		Other.Entries ? std::make_unique<std::vector<SemanticBoundary>>(*Other.Entries) : nullptr
	) {}

	/**
	 * 制約の有無の判定関数
	 * @param Kind 制約の種類
	 * @return 節点が其の制約を持てば true
	 */
	bool SyntaxFacts::Has(const ConstraintKind Kind) const {
		// 制約の位の有無の返戻
		return Constraints >> static_cast<uint8_t>(Kind) & 1;
	}

	/**
	 * 子の前の意味を持つ境界の取得関数
	 * @param Index 境界の後に続く子の番号
	 * @return 境界（記録が無ければ nullptr）
	 */
	const SemanticBoundary *SyntaxFacts::BoundaryBefore(const uint32_t Index) const {
		const std::span<const SemanticBoundary> Recorded = Boundaries.View();
		const std::span<const SemanticBoundary>::iterator Found = std::lower_bound(
			Recorded.begin(),
			Recorded.end(),
			Index,
			[](const SemanticBoundary &Boundary, const uint32_t Wanted) -> bool {
				// 子の番号の大小の返戻
				return Boundary.Before < Wanted;
			}
		);
		// 境界の記録の返戻
		return Found == Recorded.end() || Found->Before != Index ? nullptr : &*Found;
	}

	/**
	 * 制約の追加関数
	 * @param Kind 制約の種類
	 */
	void SyntaxFacts::Add(const ConstraintKind Kind) {
		Constraints |= static_cast<uint16_t>(1 << static_cast<uint8_t>(Kind));
		// 終了
		return;
	}

	/**
	 * 名前付の節点かの判定関数
	 * @return 共通の節点種別が構文なら true
	 */
	bool SyntaxKind::IsNamed() const {
		// 名前付かの返戻
		return Definition->IsNamed;
	}

	/**
	 * 共通の種別の役割の並びの取得関数（共通の種別毎に一度だけ求める）
	 * @param Definition 共通の種別（無ければ役割無）
	 * @return 種別の持つ役割の並び
	 */
	const std::vector<Role> &SyntaxModel::KindRolesOf(const KindDefinition *const Definition) {
		static const std::vector<Role> None;
		// 種別の無い節点の返戻
		if(!Definition) return None;
		// 種別毎の役割の収集と再利用
		std::unordered_map<const KindDefinition *, std::vector<Role>>::iterator Found = KindRoles.find(Definition);
		if(Found == KindRoles.end()) {
			std::vector<Role> Roles;
			for(size_t RoleIndex = 0; RoleIndex < RoleNodes.size(); ++RoleIndex) {
				if(Definition->Roles.test(RoleIndex)) Roles.push_back(static_cast<Role>(RoleIndex));
			}
			Found = KindRoles.emplace(Definition, std::move(Roles)).first;
		}
		// 種別の役割の並びの返戻
		return Found->second;
	}

	/**
	 * 節点の現在の役割の索引への登録関数
	 * @param Id 登録する節点
	 */
	void SyntaxModel::IndexRoles(const NodeId Id) {
		// 登録済の種別と現在の種別の取得
		if(IndexedKinds.size() <= Id) IndexedKinds.resize(Id + 1, nullptr);
		const KindDefinition *const Current = IsRemoved[Id] ? nullptr : Nodes[Id].Kind.Definition;
		// 登録済の種別と同じなら索引は変わらない事の終了
		if(IndexedKinds[Id] == Current) return;
		// 以前の役割からの節点の除去
		for(const Role OldRole : KindRolesOf(IndexedKinds[Id])) if(!Current || !Current->Roles.test(static_cast<size_t>(OldRole))) {
			std::vector<NodeId> &Indexed = RoleNodes[static_cast<size_t>(OldRole)];
			if(
				const std::vector<NodeId>::iterator At = std::lower_bound(Indexed.begin(), Indexed.end(), Id);
				At != Indexed.end() && *At == Id
			) Indexed.erase(At);
		}
		// 現在の役割への節点の登録
		for(const Role KindRole : KindRolesOf(Current)) {
			std::vector<NodeId> &Indexed = RoleNodes[static_cast<size_t>(KindRole)];
			if(
				const std::vector<NodeId>::iterator At = std::lower_bound(Indexed.begin(), Indexed.end(), Id);
				At == Indexed.end() || *At != Id
			) Indexed.insert(At, Id);
		}
		IndexedKinds[Id] = Current;
		// 終了
		return;
	}

	/**
	 * 書換が変更した節点の記録関数
	 * @param Id 変更した節点
	 */
	void SyntaxModel::Touch(const NodeId Id) {
		// 節点の無い記録の終了
		if(Id == NoNode) return;
		LastTouched.push_back(Id);
		// 控えを始めた時点に在った節点の最初の変更前の値の控え（書換は変更の前に記録する）
		if(Journal && Id < JournalEnd) Journal->try_emplace(Id, Nodes[Id]);
		// 終了
		return;
	}

	/**
	 * 節点の追加関数
	 * 識別子は追加の順に振り，除いた節点の識別子も再利用しない
	 * @param Node 追加する節点（識別子は此処で振る）
	 * @return 振った識別子
	 */
	NodeId SyntaxModel::AddNode(SyntaxNode &&Node) {
		// 振る識別子
		const NodeId Id = static_cast<NodeId>(Nodes.size());
		// 節点表と役割索引への追加
		Node.Id = Id;
		Nodes.push_back(std::move(Node));
		IsRemoved.push_back(0);
		if(HasRoleIndex) IndexRoles(Id);
		Touch(Id);
		// 振った識別子の返戻
		return Id;
	}

	/**
	 * 親の子の並びからの切離関数
	 * @param Id 切り離す節点
	 */
	void SyntaxModel::Detach(const NodeId Id) {
		// 親の子の並び
		const NodeId Parent = Nodes[Id].Parent;
		// 親子の世代と子の並びの更新
		Touch(Id);
		Touch(Parent);
		++Nodes[Id].Generation;
		if(Parent != NoNode) {
			++Nodes[Parent].Generation;
			const uint32_t Removed = Nodes[Id].Index;
			std::vector<NodeId> &Siblings = Nodes[Parent].Children;
			Siblings.erase(Siblings.begin() + Removed);
			// 後の兄弟の位置の振直し
			for(uint32_t Index = Removed; Index < Siblings.size(); ++Index) Nodes[Siblings[Index]].Index = Index;
			// 除いた子の前の境界を捨て，後の子の前の境界の番号を詰める
			if(!Nodes[Parent].Facts.Boundaries.View().empty()) {
				std::vector<SemanticBoundary> &Boundaries = Nodes[Parent].Facts.Boundaries.Edit();
				std::erase_if(
					Boundaries,
					[Removed](const SemanticBoundary &Boundary) -> bool {
						// 除いた子の前の境界かの返戻
						return Boundary.Before == Removed;
					}
				);
				for(SemanticBoundary &Boundary : Boundaries) if(Boundary.Before > Removed) --Boundary.Before;
			}
		}
		Nodes[Id].Parent = NoNode;
		// 終了
		return;
	}

	/**
	 * 生成する節点の種別の取得関数
	 * @param Generated 生成する節点
	 * @return 生成形式が与える種別，又は字句の字面で引いた種別
	 * @throws FormatLimitExceeded 文法が其の種別を持たない場合（正規化の不具合）
	 */
	SyntaxKind SyntaxModel::KindFor(const GeneratedNode &Generated) const {
		const KindDefinition *Chosen = Generated.Definition;
		// 名前の無い字句は固定の定義の字面毎の種別で引く
		if(!Chosen && !Generated.IsNamed) {
			const std::unordered_map<std::string_view, const KindDefinition *>::const_iterator Found =
			Definition->TokenDefinitions.find(Generated.Kind);
			if(Found != Definition->TokenDefinitions.end()) Chosen = Found->second;
		}
		if(!Chosen) throw FormatLimitExceeded("the grammar has no kind for a generated node", FailureReason::FormatterDefect);
		// 種別の返戻
		return { Chosen };
	}

	/**
	 * 規則から生成する節点の部分木の追加関数
	 * 節点表へ部分木を加え（既存の節点を移す位置は其の節点を繋ぎ），親の子の並びへの挿入は呼出側が行う
	 * @param Generated 生成する節点
	 * @param Parent 親
	 * @param Index 親の子の並びの中の位置
	 * @return 加えた部分木の根
	 * @throws FormatLimitExceeded 文法が其の種別を持たない場合（正規化の不具合）
	 */
	NodeId SyntaxModel::AddGenerated(const GeneratedNode &Generated, const NodeId Parent, const uint32_t Index) {
		struct Frame {
			const GeneratedNode *Source;
			NodeId Parent;
			uint32_t Index;
			NodeId Id = NoNode;
			uint32_t NextChild = 0;
			bool Entered = false;
		};
		// 生成部分木の後行順の構築
		std::vector<Frame> Pending { { &Generated, Parent, Index } };
		NodeId Result = NoNode;
		while(!Pending.empty()) {
			Frame &Current = Pending.back();
			if(!Current.Entered) {
				Current.Entered = true;
				Current.Id = Current.Source->Existing;
				// 移す既存の節点の元の親からの切離
				if(Current.Id != NoNode && Nodes[Current.Id].Parent != NoNode) Detach(Current.Id);
				if(Current.Id == NoNode) {
					SyntaxNode Added;
					Added.Kind = KindFor(*Current.Source);
					Added.Facts = Current.Source->Facts;
					// 字面の位置に依る事実は置く位置の親から引き継ぐ
					if(Current.Parent != NoNode) Added.Facts.HasBooleanNames = Nodes[Current.Parent].Facts.HasBooleanNames;
					if(Current.Source->Children.empty()) {
						Added.Lexeme = !Current.Source->Lexeme.empty() ?
						Current.Source->Lexeme :
						Current.Source->Source != NoNode ? Nodes[Current.Source->Source].Lexeme : Current.Source->Kind;
					}
					Current.Id = AddNode(std::move(Added));
				}
			}
			if(Current.Source->Existing == NoNode && Current.NextChild < Current.Source->Children.size()) {
				const uint32_t Child = Current.NextChild++;
				Pending.push_back({ &Current.Source->Children[Child], Current.Id, Child });
				continue;
			}
			// 親との接続と生成元の判定
			Nodes[Current.Id].Parent = Current.Parent;
			Nodes[Current.Id].Index = Current.Index;
			Nodes[Current.Id].Fields = Current.Source->Fields;
			// 原文の節点を移さず写さずに作った節点で，子も全て其の様な物なら加えた部分木
			if(Current.Source->Existing == NoNode) {
				Nodes[Current.Id].IsInserted = Current.Source->Source == NoNode && std::ranges::all_of(
					Nodes[Current.Id].Children,
					[this](const NodeId Child) -> bool {
						// 加えた部分木の子かの返戻
						return Nodes[Child].IsInserted;
					}
				);
			}
			const NodeId Id = Current.Id;
			Pending.pop_back();
			if(Pending.empty()) Result = Id;
			else Nodes[Pending.back().Id].Children.push_back(Id);
		}
		// 加えた部分木の根の返戻
		return Result;
	}

	/**
	 * 識別子を保った節点の組直関数（整形仕様 3.2）
	 * 節点の種別と子を生成する節点の物へ替え，移す既存の節点を新しい位置へ繋ぎ，残さない子の錨を移譲の行先へ移す
	 * @param Step 組直の操作
	 * @param Trace 錨の移譲の記録の追加先
	 * @throws FormatLimitExceeded 文法が其の種別を持たない場合（正規化の不具合）
	 */
	void SyntaxModel::ReshapeNode(const RewriteStep &Step, RewriteTrace &Trace) {
		Touch(Step.Target);
		// 移す既存の節点
		std::vector<NodeId> Reused;
		std::vector<const GeneratedNode *> Pending { &Step.Inserted };
		while(!Pending.empty()) {
			const GeneratedNode *const Generated = Pending.back();
			Pending.pop_back();
			if(Generated->Existing != NoNode) Reused.push_back(Generated->Existing);
			for(const GeneratedNode &Child : Generated->Children) Pending.push_back(&Child);
		}
		for(const NodeId Moved : Reused) Detach(Moved);
		// 残さない子の部分木の除去と錨の移譲
		for(std::vector<NodeId> Stack(Nodes.at(Step.Target).Children); !Stack.empty();) {
			const NodeId Removed = Stack.back();
			Stack.pop_back();
			if(std::find(Reused.begin(), Reused.end(), Removed) != Reused.end()) continue;
			IsRemoved[Removed] = 1;
			Touch(Removed);
			Trace.Transfers.push_back(Step.Transfer);
			Trace.Transfers.back().From = Removed;
			Stack.insert(Stack.end(), Nodes[Removed].Children.begin(), Nodes[Removed].Children.end());
		}
		// 対象の種別と子の組直
		SyntaxNode &Node = Nodes[Step.Target];
		++Node.Generation;
		Node.Kind = KindFor(Step.Inserted);
		// 字面の位置に依る事実は同じ位置に留まる組替えの前後で保つ
		const bool HasBooleanNames = Node.Facts.HasBooleanNames;
		Node.Facts = Step.Inserted.Facts;
		Node.Facts.HasBooleanNames = HasBooleanNames;
		Node.Children.clear();
		for(uint32_t Child = 0; Child < Step.Inserted.Children.size(); ++Child) {
			const NodeId ChildId = AddGenerated(Step.Inserted.Children[Child], Step.Target, Child);
			Nodes[Step.Target].Children.push_back(ChildId);
		}
		// 終了
		return;
	}

	/**
	 * 親の子の並びへの接続関数
	 * 親の子の並びの指定位置へ節点を加え，隣接を記録した親では節点の前の境界も記録する
	 * @param Id 接続する節点（親を持たない物）
	 * @param Parent 親
	 * @param Index 親の子の並びの中の位置
	 * @param Boundary 新しい境界の種類
	 * @param RecordedBoundary 元の節点の前から移す境界（有れば字面と由来も保持する）
	 */
	void SyntaxModel::Attach(
		const NodeId Id,
		const NodeId Parent,
		const uint32_t Index,
		const BoundaryKind Boundary,
		const std::optional<SemanticBoundary> &RecordedBoundary
	) {
		// 親子の接続と世代の更新
		Touch(Id);
		Touch(Parent);
		Nodes[Id].Parent = Parent;
		++Nodes[Id].Generation;
		++Nodes[Parent].Generation;
		std::vector<NodeId> &Siblings = Nodes[Parent].Children;
		Siblings.insert(Siblings.begin() + Index, Id);
		// 節点と後の兄弟の位置と境界の番号の振直し
		for(uint32_t Position = Index; Position < Siblings.size(); ++Position) Nodes[Siblings[Position]].Index = Position;
		// 境界を記録した親か移す境界を持つ子だけが境界の並びを持つ
		if(const bool IsRecorded = !Nodes[Parent].Facts.Boundaries.View().empty(); IsRecorded || RecordedBoundary && Index) {
			std::vector<SemanticBoundary> &Boundaries = Nodes[Parent].Facts.Boundaries.Edit();
			for(SemanticBoundary &Recorded : Boundaries) if(Recorded.Before >= Index) ++Recorded.Before;
			if(Index) {
				const std::vector<SemanticBoundary>::iterator At = std::lower_bound(
					Boundaries.begin(),
					Boundaries.end(),
					Index,
					[](const SemanticBoundary &Recorded, const uint32_t Wanted) -> bool {
						// 子の番号の大小の返戻
						return Recorded.Before < Wanted;
					}
				);
				SemanticBoundary Inserted = RecordedBoundary.value_or(SemanticBoundary{ Index, Boundary });
				Inserted.Before = Index;
				Boundaries.insert(At, std::move(Inserted));
			}
		}
		// 終了
		return;
	}

	/**
	 * 役割を持つ節点かの判定関数
	 * @param Id 節点の識別子
	 * @param Wanted 役割
	 * @return 節点の種別が其の役割を持てば true
	 */
	bool SyntaxModel::Is(const NodeId Id, const Role Wanted) const {
		// 種別の役割の返戻
		return Id < Nodes.size() && Nodes[Id].Kind.Definition && Nodes[Id].Kind.Definition->Roles.test(static_cast<size_t>(Wanted));
	}

	/**
	 * 役割を持つ節点の候補の取得関数
	 * @param Wanted 役割
	 * @return 節点番号順の現在の役割を持つ節点
	 * @throws FormatLimitExceeded 正規化の前に役割索引を構築しなかった場合（整形器の不具合）
	 */
	std::span<const NodeId> SyntaxModel::NodesWith(const Role Wanted) const {
		if(!HasRoleIndex) {
			throw FormatLimitExceeded("the role index was not built before normalization", FailureReason::FormatterDefect);
		}
		// 役割の候補の返戻
		return std::span<const NodeId>(RoleNodes[static_cast<size_t>(Wanted)]);
	}

	/**
	 * 正規化の役割索引の構築関数
	 * 正規化の始めに一度だけ作り（検証の構文は索引を使わない為に作らない），以後は追加と組直で更新する
	 * 役割毎の節点の数を先に数えて各索引を一度で確保する
	 * 計算量：節点数 N と節点毎の役割の数 R に対し O(N R)
	 */
	void SyntaxModel::BuildRoleIndex() {
		// 役割毎の必要容量の計数と確保
		IndexedKinds.assign(Nodes.size(), nullptr);
		std::array<size_t, static_cast<size_t>(Role::Count)> Counts {};
		for(NodeId Id = 0; Id < Nodes.size(); ++Id) {
			if(!IsRemoved[Id]) for(const Role KindRole : KindRolesOf(Nodes[Id].Kind.Definition)) ++Counts[static_cast<size_t>(KindRole)];
		}
		for(size_t RoleIndex = 0; RoleIndex < RoleNodes.size(); ++RoleIndex) {
			RoleNodes[RoleIndex].clear();
			RoleNodes[RoleIndex].reserve(Counts[RoleIndex]);
		}
		// 節点番号順の登録
		for(NodeId Id = 0; Id < Nodes.size(); ++Id) {
			if(IsRemoved[Id]) continue;
			IndexedKinds[Id] = Nodes[Id].Kind.Definition;
			for(const Role KindRole : KindRolesOf(IndexedKinds[Id])) RoleNodes[static_cast<size_t>(KindRole)].push_back(Id);
		}
		HasRoleIndex = true;
		// 終了
		return;
	}

	/**
	 * 正規化の役割索引の解放関数
	 * 正規化の後の工程は索引を使わない為，記憶域を空けて以後の書換で更新しない
	 */
	void SyntaxModel::ReleaseRoleIndex() {
		for(std::vector<NodeId> &Indexed : RoleNodes) std::vector<NodeId>().swap(Indexed);
		std::vector<const KindDefinition *>().swap(IndexedKinds);
		HasRoleIndex = false;
		// 終了
		return;
	}

	/**
	 * 最後の計画が変更した節点の取得関数
	 * @return 追加・除去・種別・字句・子の並びを変更した節点と其の親
	 */
	std::span<const NodeId> SyntaxModel::TouchedNodes() const {
		// 変更した節点の返戻
		return std::span<const NodeId>(LastTouched);
	}

	/**
	 * 実際の綴りの字句の役割の判定関数
	 * 正規化で綴りを書き換えた場合も文法の字句定義から役割を引く
	 * @param Id 字句の節点
	 * @param Wanted 役割
	 * @return 綴りに対応する役割を持てば true
	 */
	bool SyntaxModel::IsTokenRole(const NodeId Id, const Role Wanted) const {
		// 節点又は記法が無い場合の返戻
		if(Id >= Nodes.size() || !Definition || !Nodes[Id].Kind.Definition) return false;
		// 部分木は字句として扱わない事の返戻
		if(!Nodes[Id].Children.empty()) return false;
		const std::unordered_map<std::string_view, const KindDefinition *>::const_iterator Found =
		Definition->TokenDefinitions.find(Nodes[Id].Lexeme);
		// 実際の綴りに対応する字句の役割の返戻
		return Found != Definition->TokenDefinitions.end() && Found->second->Roles.test(static_cast<size_t>(Wanted));
	}

	/**
	 * 節点の生成形式の取得関数
	 * @param Id 節点の識別子
	 * @param Operation 共通操作
	 * @return 節点の種別が其の操作に使える生成形式（無ければ無し）
	 */
	const CanonicalForm *SyntaxModel::FormOf(const NodeId Id, const FormOperation Operation) const {
		// 種別の定義を持たない節点に生成形式が無い事の返戻
		if(Id >= Nodes.size() || !Nodes[Id].Kind.Definition) return nullptr;
		const KindDefinition *const Definition = Nodes[Id].Kind.Definition;
		// 投影済の構文事実が持つ生成形式の探索
		for(const CanonicalForm &Form : Nodes[Id].Facts.Forms.View()) {
			// 操作と所有者の役割に適用出来る形式の返戻
			if(Form.Operation == Operation && Form.AppliesTo(Definition->Roles)) return &Form;
		}
		// 使える生成形式の無い事の返戻
		return nullptr;
	}

	/**
	 * 根の取得関数
	 * @return 根の識別子
	 */
	NodeId SyntaxModel::GetRoot() const {
		// 根の返戻
		return Root;
	}

	/**
	 * 節点表の大きさの取得関数
	 * @return 除いた節点を含む節点の数（識別子の上限）
	 */
	size_t SyntaxModel::NodeCount() const {
		// 節点の数の返戻
		return Nodes.size();
	}

	/**
	 * 除いた節点かの判定関数
	 * @param Id 節点
	 * @return 書換で除いた節点（識別子を再利用しない為に節点表に残る）なら true
	 */
	bool SyntaxModel::IsRemovedNode(const NodeId Id) const {
		// 除いたかの返戻
		return IsRemoved.at(Id);
	}

	/**
	 * 文法の構文・意味の固定の定義の取得関数
	 * @return 投影側が文法から定めた構文・意味の固定の定義（コメントの記法の定義を含む）
	 */
	const SyntaxRules &SyntaxModel::Rules() const {
		// 固定の定義の返戻
		return *Definition;
	}

	/**
	 * 構文の正規化と中間表現の構築の作業単位の計上関数（整形仕様 9.2）
	 * @param Amount 計上する作業単位
	 * @throws FormatLimitExceeded 作業単位の合計が上限を超える場合（規定した資源上限の見送）
	 */
	void SyntaxModel::Charge(const size_t Amount) const {
		// 予算を閉じた後の工程（文書の同期・行分割・検証）の読取は此の予算へ数えない事の返戻
		if(Work->IsClosed) return;
		// 上限超過の見送
		if(Amount > Work->Limit - Work->Used) throw FormatLimitExceeded("structured text exceeds formatting limit");
		Work->Used += Amount;
		// 終了
		return;
	}

	/**
	 * 構文とは独立した作業予算の取得関数
	 * @return 正規化と中間表現の構築で共有する予算
	 */
	std::shared_ptr<StructureWorkAccount> SyntaxModel::WorkAccount() const {
		// 共有する作業予算の返戻
		return Work;
	}

	/**
	 * 作業単位の上限の取得関数
	 * @return 投影側が入力の基準量から定めた上限（定めて居なければ最大値）
	 */
	size_t SyntaxModel::GetWorkLimit() const {
		// 上限の返戻
		return Work->Limit;
	}

	/**
	 * 節点の取得関数
	 * @param Id 節点の識別子
	 * @return 節点
	 * @throws std::out_of_range 識別子が節点表に無い場合
	 */
	const SyntaxNode &SyntaxModel::GetNode(const NodeId Id) const {
		// 節点の返戻
		return Nodes.at(Id);
	}

	/**
	 * 変更前の値の控えを始めた時点の節点の取得関数
	 * @param Id 控えを始めた時点に在った節点の識別子
	 * @return 其の後に変えた節点は控えた値，変えて居ない節点は節点表の値
	 * @throws FormatLimitExceeded 控えて居ない場合と控えを始めた後に加えた節点の場合（正規化の不具合）
	 * @throws std::out_of_range 識別子が節点表に無い場合
	 */
	const SyntaxNode &SyntaxModel::JournaledNode(const NodeId Id) const {
		if(!Journal || Id >= JournalEnd) {
			throw FormatLimitExceeded("a journaled node is out of the journal", FailureReason::FormatterDefect);
		}
		const std::unordered_map<NodeId, SyntaxNode>::const_iterator Found = Journal->find(Id);
		// 控えた値か節点表の値の返戻
		return Found != Journal->end() ? Found->second : Nodes.at(Id);
	}

	/**
	 * 所有区画の取得関数（整形仕様 4.4）
	 * @param Id 節点
	 * @return 節点の置く所有区画（所有区画でない節点は無し）
	 */
	const OwnedRegion *SyntaxModel::RegionOf(const NodeId Id) const {
		// 所有区画の返戻
		return Id < Nodes.size() && Nodes[Id].Facts.RegionIndex ? Regions[Nodes[Id].Facts.RegionIndex - 1].get() : nullptr;
	}

	/**
	 * 所有区画の有無の判定関数（整形仕様 4.4）
	 * @return 所有区画を持てば true
	 */
	bool SyntaxModel::HasRegions() const {
		// 所有区画の有無の返戻
		return !Regions.empty();
	}

	/** ========== 節点・字句・構文事実の取得 ========== */
	/**
	 * 字句の字面の取得関数
	 * @param Id 節点の識別子
	 * @return 出力する字句の字面（節点が無ければ空）
	 */
	std::string_view SyntaxModel::LexemeOf(const NodeId Id) const {
		// 字句の字面の返戻
		return Id < Nodes.size() ? std::string_view(Nodes[Id].Lexeme) : std::string_view();
	}

	/**
	 * 名前付の節点かの判定関数
	 * @param Id 節点の識別子
	 * @return 名前付の節点なら true（節点が無ければ false）
	 */
	bool SyntaxModel::IsNamed(const NodeId Id) const {
		// 名前付かの返戻
		return Id < Nodes.size() && Nodes[Id].Kind.IsNamed();
	}

	/**
	 * 子の並びの取得関数
	 * @param Id 節点の識別子
	 * @return 子の並び（節点が無ければ空）
	 */
	std::span<const NodeId> SyntaxModel::ChildrenOf(const NodeId Id) const {
		// 子の並びの返戻
		return Id < Nodes.size() ? std::span<const NodeId>(Nodes[Id].Children) : std::span<const NodeId>();
	}

	/**
	 * 親の取得関数
	 * @param Id 節点の識別子
	 * @return 親（根と無い節点は無し）
	 */
	NodeId SyntaxModel::ParentOf(const NodeId Id) const {
		// 親の返戻
		return Id < Nodes.size() ? Nodes[Id].Parent : NoNode;
	}

	/**
	 * 子の役割の位の取得関数
	 * @param Role 子の役割
	 * @return 子の役割の位（文法に無い役割は０）
	 */
	uint64_t SyntaxModel::FieldBit(const ChildRole Role) const {
		const uint64_t Bit = Role == ChildRole::None ? 0 : uint64_t{ 1 } << static_cast<uint8_t>(Role);
		// 文法に在る役割の位だけの返戻
		return Bit & ChildRoleMask;
	}

	/**
	 * 場の子の取得関数
	 * @param Id 節点の識別子
	 * @param Role 子の役割
	 * @return 最初の其の場の子（無ければ無し）
	 */
	NodeId SyntaxModel::FieldChild(const NodeId Id, const ChildRole Role) const {
		// 場の位
		const uint64_t Bit = FieldBit(Role);
		// 其の場の子の返戻
		if(Bit) for(const NodeId Child : ChildrenOf(Id)) if(Nodes[Child].Fields & Bit) return Child;
		// 其の場の子が無い事の返戻
		return NoNode;
	}

	/**
	 * 親の中の場かの判定関数
	 * @param Id 節点の識別子
	 * @param Role 子の役割
	 * @return 節点が親の中で其の場を持てば true
	 */
	bool SyntaxModel::HasField(const NodeId Id, const ChildRole Role) const {
		// 場の集合に其の場の位が在るかの返戻
		return Id < Nodes.size() && Nodes[Id].Fields & FieldBit(Role);
	}

	/**
	 * 子の数の取得関数
	 * @param Id 節点の識別子
	 * @return 子の数（節点が無ければ０）
	 */
	uint32_t SyntaxModel::ChildCount(const NodeId Id) const {
		// 子の数の返戻
		return static_cast<uint32_t>(ChildrenOf(Id).size());
	}

	/**
	 * 子の取得関数
	 * @param Id 節点の識別子
	 * @param Index 子の位置
	 * @return 子（位置が範囲の外なら無し）
	 */
	NodeId SyntaxModel::Child(const NodeId Id, const uint32_t Index) const {
		const std::span<const NodeId> Children = ChildrenOf(Id);
		// 位置の子の返戻
		return Index < Children.size() ? Children[Index] : NoNode;
	}

	/**
	 * 名前付の子の数の取得関数
	 * 計算量：子の数 C に対し O(C)
	 * @param Id 節点の識別子
	 * @return 名前付の子の数
	 */
	uint32_t SyntaxModel::NamedChildCount(const NodeId Id) const {
		const std::span<const NodeId> Children = ChildrenOf(Id);
		Charge(Children.size());
		// 名前付の子の数の返戻
		return static_cast<uint32_t>(
			std::count_if(
				Children.begin(),
				Children.end(),
				[this](const NodeId Child) -> bool {
					// 名前付の子かの返戻
					return Nodes[Child].Kind.IsNamed();
				}
			)
		);
	}

	/**
	 * 名前付の子の取得関数
	 * 計算量：子の数 C に対し O(C)
	 * @param Id 節点の識別子
	 * @param Index 名前付の子の中の位置
	 * @return 名前付の子（位置が範囲の外なら無し）
	 */
	NodeId SyntaxModel::NamedChild(const NodeId Id, const uint32_t Index) const {
		uint32_t Seen = 0;
		for(const NodeId Child : ChildrenOf(Id)) {
			Charge(1);
			if(!Nodes[Child].Kind.IsNamed()) continue;
			// 位置の名前付の子の返戻
			if(Seen == Index) return Child;
			++Seen;
		}
		// 範囲の外の返戻
		return NoNode;
	}

	/**
	 * 前の兄弟の取得関数
	 * @param Id 節点の識別子
	 * @return 前の兄弟（先頭の子と根は無し）
	 */
	NodeId SyntaxModel::PrevSibling(const NodeId Id) const {
		const NodeId Parent = ParentOf(Id);
		// 前の兄弟の返戻
		return Parent == NoNode || !Nodes[Id].Index ? NoNode : Nodes[Parent].Children[Nodes[Id].Index - 1];
	}

	/**
	 * 後の兄弟の取得関数
	 * @param Id 節点の識別子
	 * @return 後の兄弟（最後の子と根は無し）
	 */
	NodeId SyntaxModel::NextSibling(const NodeId Id) const {
		const NodeId Parent = ParentOf(Id);
		// 後の兄弟の返戻
		return Parent == NoNode ? NoNode : Child(Parent, Nodes[Id].Index + 1);
	}

	/**
	 * 部分木の字句の字面を区切無で繋いだ字面の取得関数
	 * 字句の葉の比較（`await` 等）に使い，字句の間の空白を持たない
	 * 計算量：部分木の節点数 V と字面の長さ N に対し O(V + N)
	 * @param Id 節点の識別子
	 * @return 字面
	 */
	std::string SyntaxModel::Text(const NodeId Id) const {
		// 部分木の字句の連結
		std::string Joined;
		std::vector<NodeId> Stack { Id };
		size_t Visited = 0;
		while(!Stack.empty()) {
			const NodeId Top = Stack.back();
			Stack.pop_back();
			if(Top >= Nodes.size()) continue;
			++Visited;
			Joined += Nodes[Top].Lexeme;
			Stack.insert(Stack.end(), Nodes[Top].Children.rbegin(), Nodes[Top].Children.rend());
		}
		// 辿った節点と複写した字句のバイトの計上（整形仕様 9.2）
		Charge(Visited + Joined.size());
		// 繋いだ字面の返戻
		return Joined;
	}

	/**
	 * 投影済の識別子の比較用の名前の取得関数
	 * @param Id 識別子又は名前の位置に在る節点
	 * @return 投影時に確定した比較用の名前
	 * @throws FormatLimitExceeded 投影が比較用の名前を確定しなかった節点の場合（整形器の不具合）
	 */
	std::string SyntaxModel::ComparisonName(const NodeId Id) const {
		const uint32_t NameIndex = GetNode(Id).Facts.NameIndex;
		if(!NameIndex) throw FormatLimitExceeded("an identifier comparison name was not projected", FailureReason::FormatterDefect);
		const std::string &Name = ComparisonNames[NameIndex - 1];
		// 名前の複写の計上（整形仕様 9.2）
		Charge(Name.size());
		// 比較用の名前の返戻
		return Name;
	}

	/**
	 * 部分木の最初の字句と字の取得関数
	 * @param Id 節点の識別子
	 * @return 最初の字句の葉と字（無ければ節点無と零）
	 */
	LexicalEdge SyntaxModel::FirstEdge(const NodeId Id) const {
		std::vector<NodeId> Stack { Id };
		while(!Stack.empty()) {
			const NodeId Top = Stack.back();
			Stack.pop_back();
			if(Top >= Nodes.size()) continue;
			// 辿った節点の計上（整形仕様 9.2）
			Charge(1);
			// 字面を持つ最初の字句の頭の字の返戻
			if(!Nodes[Top].Lexeme.empty()) return { Top, Nodes[Top].Lexeme.front() };
			Stack.insert(Stack.end(), Nodes[Top].Children.rbegin(), Nodes[Top].Children.rend());
		}
		// 字面が無い事の返戻
		return {};
	}

	/**
	 * 部分木の最初の字の取得関数
	 * @param Id 節点の識別子
	 * @return 最初の字（字面が無ければ `\0`）
	 */
	char SyntaxModel::FirstChar(const NodeId Id) const {
		// 最初の字の返戻
		return FirstEdge(Id).Char;
	}

	/**
	 * 部分木の最後の字句と字の取得関数
	 * @param Id 節点の識別子
	 * @return 最後の字句の葉と字（無ければ節点無と零）
	 */
	LexicalEdge SyntaxModel::LastEdge(const NodeId Id) const {
		std::vector<NodeId> Stack { Id };
		while(!Stack.empty()) {
			const NodeId Top = Stack.back();
			Stack.pop_back();
			if(Top >= Nodes.size()) continue;
			// 辿った節点の計上（整形仕様 9.2）
			Charge(1);
			// 字面を持つ最後の字句の末尾の字の返戻
			if(!Nodes[Top].Lexeme.empty()) return { Top, Nodes[Top].Lexeme.back() };
			Stack.insert(Stack.end(), Nodes[Top].Children.begin(), Nodes[Top].Children.end());
		}
		// 字面が無い事の返戻
		return {};
	}

	/**
	 * 部分木の最後の字の取得関数
	 * @param Id 節点の識別子
	 * @return 最後の字（字面が無ければ `\0`）
	 */
	char SyntaxModel::LastChar(const NodeId Id) const {
		// 最後の字の返戻
		return LastEdge(Id).Char;
	}

	/**
	 * 部分木の端の字句の役割の判定関数
	 * 字句の定義を持つ葉は其の役割，逐語・名前の葉は端の１字が作る文法の字句の役割とする
	 * @param Edge 部分木の端の字句の葉と其の端の字
	 * @param Wanted 判定する役割
	 * @return 端の字句が役割を持てば true
	 */
	bool SyntaxModel::IsEdgeRole(const LexicalEdge &Edge, const Role Wanted) const {
		// 端の字句の無い場合の返戻
		if(Edge.Leaf == NoNode || !Definition) return false;
		const std::unordered_map<std::string_view, const KindDefinition *> &Tokens = Definition->TokenDefinitions;
		std::unordered_map<std::string_view, const KindDefinition *>::const_iterator Found = Tokens.find(Nodes[Edge.Leaf].Lexeme);
		if(Found == Tokens.end()) Found = Tokens.find(std::string_view(&Edge.Char, 1));
		// 端の字句の役割の返戻
		return Found != Tokens.end() && Found->second->Roles.test(static_cast<size_t>(Wanted));
	}

	/**
	 * 部分木の最初の字句の役割の判定関数
	 * @param Id 部分木の節点
	 * @param Wanted 判定する役割
	 * @return 最初の字句が役割を持てば true
	 */
	bool SyntaxModel::FirstTokenRole(const NodeId Id, const Role Wanted) const {
		// 最初の字句の役割の返戻
		return IsEdgeRole(FirstEdge(Id), Wanted);
	}

	/**
	 * 部分木の最後の字句の役割の判定関数
	 * @param Id 部分木の節点
	 * @param Wanted 判定する役割
	 * @return 最後の字句が役割を持てば true
	 */
	bool SyntaxModel::LastTokenRole(const NodeId Id, const Role Wanted) const {
		// 最後の字句の役割の返戻
		return IsEdgeRole(LastEdge(Id), Wanted);
	}

	/**
	 * 括弧を開閉する字句の深さの変化の取得関数
	 * @param Id 字句の節点
	 * @return 開きなら１，閉じなら負の１，其の他なら０
	 */
	int SyntaxModel::BracketLeadDelta(const NodeId Id) const {
		Charge(1);
		// 括弧の深さの変化の返戻
		return IsTokenRole(Id, Role::OpeningBracketLead) ? 1 : IsTokenRole(Id, Role::ClosingBracketLead) ? -1 : 0;
	}

	/**
	 * 括弧を開閉する字句の役割の判定関数
	 * @param Id 字句の節点
	 * @param Wanted 開き又は閉じの役割
	 * @return 指定の役割を持てば true
	 */
	bool SyntaxModel::IsBracketLead(const NodeId Id, const Role Wanted) const {
		Charge(1);
		// 括弧の役割の返戻
		return IsTokenRole(Id, Wanted);
	}

	/**
	 * 部分木の最後の葉の取得関数
	 * @param Id 節点の識別子
	 * @return 最後の子を辿った葉（節点が無ければ無し）
	 */
	NodeId SyntaxModel::LastLeaf(NodeId Id) const {
		while(Id < Nodes.size()) {
			Charge(1);
			if(Nodes[Id].Children.empty()) break;
			Id = Nodes[Id].Children.back();
		}
		// 最後の葉の返戻
		return Id < Nodes.size() ? Id : NoNode;
	}

	/**
	 * 最後の名前付の子の取得関数
	 * @param Id 節点の識別子
	 * @return 最後の名前付の子（無ければ無し）
	 */
	NodeId SyntaxModel::LastNamedChild(const NodeId Id) const {
		const std::span<const NodeId> Children = ChildrenOf(Id);
		for(std::span<const NodeId>::reverse_iterator Child = Children.rbegin(); Child != Children.rend(); ++Child) {
			Charge(1);
			// 最後の名前付の子の返戻
			if(IsNamed(*Child)) return *Child;
		}
		// 名前付の子の無い事の返戻
		return NoNode;
	}

	/**
	 * 前の名前付の兄弟の取得関数
	 * @param Id 節点の識別子
	 * @return 前の名前付の兄弟（無ければ無し）
	 */
	NodeId SyntaxModel::PrevNamedSibling(const NodeId Id) const {
		NodeId Previous = PrevSibling(Id);
		while(Previous != NoNode && !IsNamed(Previous)) Previous = PrevSibling(Previous);
		// 前の名前付の兄弟の返戻
		return Previous;
	}

	/**
	 * 後の名前付の兄弟の取得関数
	 * @param Id 節点の識別子
	 * @return 後の名前付の兄弟（無ければ無し）
	 */
	NodeId SyntaxModel::NextNamedSibling(const NodeId Id) const {
		NodeId Next = NextSibling(Id);
		while(Next != NoNode) {
			Charge(1);
			if(IsNamed(Next)) break;
			Next = NextSibling(Next);
		}
		// 後の名前付の兄弟の返戻
		return Next;
	}

	/**
	 * 構文の順で節点の後に続く物の取得関数（整形仕様 7.6）
	 * 節点の後の兄弟（要素の区切りの字句は飛ばす）を，無ければ親の後に続く物を，正規化の後に作った索引から引く
	 * @param Id 節点の識別子
	 * @return 後に続く節点（無ければ無し）
	 */
	NodeId SyntaxModel::FollowingOf(const NodeId Id) const {
		// 後に続く節点の返戻
		return Id < Following.size() ? Following[Id] : NoNode;
	}

	/**
	 * 構文の順で節点毎の後に続く物の索引の構築関数（整形仕様 7.6）
	 * 正規化の後に根から一度の走査で作り，文書の同期とコメントの配置が記録毎に祖先を辿らずに引く（整形仕様 9.2）
	 * 計算量：節点数 V に対し O(V)
	 */
	void SyntaxModel::BuildFollowingIndex() {
		Following.assign(NodeCount(), NoNode);
		for(std::vector<NodeId> Pending { GetRoot() }; !Pending.empty();) {
			const NodeId Parent = Pending.back();
			Pending.pop_back();
			const std::span<const NodeId> Children = ChildrenOf(Parent);
			Charge(Children.size());
			// 後の兄弟から順に，其の兄弟（区切りでなければ）を前の兄弟の後に続く物とする
			NodeId Next = Following[Parent];
			for(std::span<const NodeId>::reverse_iterator Child = Children.rbegin(); Child != Children.rend(); ++Child) {
				Following[*Child] = Next;
				if(!Is(*Child, Role::SeparatorToken)) Next = *Child;
				Pending.push_back(*Child);
			}
		}
		// 終了
		return;
	}

	/**
	 * 指定の字面の無名の子を持つかの判定関数
	 * @param Id 節点の識別子
	 * @param Token 字面
	 * @return 字面の一致する無名の子が有れば true
	 */
	bool SyntaxModel::HasTokenChild(const NodeId Id, const std::string_view Token) const {
		for(const NodeId Child : ChildrenOf(Id)) {
			Charge(1);
			// 一致する無名の子の返戻
			if(!IsNamed(Child) && Nodes[Child].Lexeme == Token) return true;
		}
		// 一致する子の無い事の返戻
		return false;
	}

	/**
	 * 指定の役割の無名の子を持つかの判定関数
	 * @param Id 親の節点
	 * @param Wanted 字句の役割
	 * @return 役割を持つ無名の子が有れば true
	 */
	bool SyntaxModel::HasTokenRoleChild(const NodeId Id, const Role Wanted) const {
		// 役割を持つ無名の子を見つけた場合の返戻
		for(const NodeId Child : ChildrenOf(Id)) if(!IsNamed(Child) && IsTokenRole(Child, Wanted)) return true;
		// 役割を持つ子が無い事の返戻
		return false;
	}

	/**
	 * 語の意味の綴りの無名の子を持つかの判定関数
	 * @param Id 節点の識別子
	 * @param Meaning 語の意味
	 * @return 文法が其の意味に定める綴りの無名の子が有れば true
	 */
	bool SyntaxModel::HasWordChild(const NodeId Id, const Word Meaning) const {
		for(const NodeId Child : ChildrenOf(Id)) {
			Charge(1);
			// 一致する無名の子の返戻
			if(!IsNamed(Child) && Definition->IsSpelling(Nodes[Child].Lexeme, Meaning)) return true;
		}
		// 一致する子の無い事の返戻
		return false;
	}

	/**
	 * 部分木の字面が語の意味の綴りかの判定関数
	 * 積んだ字句の葉の字面の長さを下限として数え，最長の綴りを超えた時点で打ち切る（深い部分木の背骨を降り切らない）
	 * 計算量：訪れた節点数 V と最長の綴りの長さ L に対し O(V + L)
	 * @param Id 節点の識別子
	 * @param Meaning 語の意味
	 * @return 部分木の字句を繋いだ字面が文法の定める其の意味の綴りなら true（節点が無ければ false）
	 */
	bool SyntaxModel::IsWord(const NodeId Id, const Word Meaning) const {
		// 節点が無ければ語でない事の返戻
		if(Id >= Nodes.size()) return false;
		// 字句の葉は其の字面の一致の返戻
		if(Nodes[Id].Children.empty()) return Definition->IsSpelling(Nodes[Id].Lexeme, Meaning);
		const std::vector<std::string_view> &Spellings = Definition->Words[static_cast<size_t>(Meaning)];
		// 綴りを持たない意味は一致しない事の返戻
		if(Spellings.empty()) return false;
		// 最長の綴りと走査状態の準備
		size_t MaxLength = 0;
		for(const std::string_view Spelling : Spellings) {
			Charge(1);
			MaxLength = std::max(MaxLength, Spelling.size());
		}
		std::string Joined;
		std::vector<NodeId> Work { Id };
		// 積んだ字句の葉の字面の長さの和（何れ繋ぐ字面の下限）
		size_t Pending = 0;
		// 長さの上限を守る字句の連結
		while(!Work.empty()) {
			const NodeId Current = Work.back();
			Work.pop_back();
			if(Current >= Nodes.size()) continue;
			Charge(1);
			const SyntaxNode &Node = Nodes[Current];
			if(Node.Children.empty()) Pending -= Node.Lexeme.size();
			// 最長の綴りを超えた字句の複写を避ける事の返戻
			if(Node.Lexeme.size() > MaxLength - Joined.size()) return false;
			Charge(Node.Lexeme.size());
			Joined += Node.Lexeme;
			for(const NodeId Child : Node.Children | std::views::reverse) {
				Work.push_back(Child);
				if(Child < Nodes.size() && Nodes[Child].Children.empty()) Pending += Nodes[Child].Lexeme.size();
			}
			// 何れ繋ぐ字面が最長の綴りを超える事の返戻
			if(Pending > MaxLength - Joined.size()) return false;
		}
		// 最長の綴り以内で繋いだ字面の一致の返戻
		return Definition->IsSpelling(Joined, Meaning);
	}

	/**
	 * 波括弧で包んだ文の塊かの判定関数
	 * @param Node 節点
	 * @return 文を包む塊の種別で，`{` で始まる物なら true
	 */
	bool SyntaxModel::IsBracedBody(const NodeId Node) const {
		// 塊の種別で波括弧を開く節点かの返戻
		return Is(Node, Role::Block) && Is(Child(Node, 0), Role::OpenBrace);
	}

	/**
	 * 空の本体を閉じの語と同じ行へ畳むと構文誤りに為るかの判定関数
	 * 見出が式・名前で終わる形 (`class A < B end` / `class << obj end` / `def n end` / `def m a end`) は畳めない
	 * @param Id 節点
	 * @return 閉じの前に区切りが要れば true
	 */
	bool SyntaxModel::NeedsSeparatorToClose(const NodeId Id) const {
		// 親クラスと値で開く形は仮引数に関わらず区切りを要する事の返戻
		if(FieldChild(Id, ChildRole::Superclass) != NoNode || FieldChild(Id, ChildRole::Value) != NoNode) return true;
		// メソッド以外は区切りを要しない事の返戻
		if(!Is(Id, Role::EndDelimitedMethod)) return false;
		const NodeId Parameters = FieldChild(Id, ChildRole::Parameters);
		// 括弧で囲んだ仮引数を持つか
		const bool HasParenthesized = Parameters != NoNode && IsTokenRole(Child(Parameters, 0), Role::OpeningParenToken);
		// 仮引数の括弧を持たない `def` の返戻
		return !HasParenthesized;
	}

	/**
	 * カンマで並べる宣言の部品の取得関数（整形仕様 5.6）
	 * 名前を `left` に持つ宣言（Go の短い変数宣言）と名前の場を持つ唯一の仕様の宣言（Go の var・const）は名前の並びと初期化子の並びを，其の他は宣言子を始める節点から `,` で区切った宣言子の並びを読み，並びの外の子を宣言子で共有する
	 * 名前と初期化子の数の異なる多値初期化と初期化子も型も無い仕様と，型推論の `var`（宣言子毎に型が決まる）と，本体を持つ宣言子（Swift の計算型・監視付の宣言）は纏めも分けもしない
	 * @param Node 宣言
	 * @return 宣言の部品（カンマで並べない宣言・誤りを含む宣言は無し）
	 */
	std::optional<CommaDecl> SyntaxModel::CommaDeclOf(const NodeId Node) const {
		const SyntaxModel &Syntax = *this;
		// カンマで併記出来ない宣言の返戻
		if(!Syntax.Is(Node, Role::CommaDeclaration)) return std::nullopt;
		// 誤りを含む宣言の返戻
		if(
			Syntax.GetNode(Node).Facts.Has(ConstraintKind::ContainsError) ||
			Syntax.GetNode(Node).Facts.Has(ConstraintKind::AmbiguousDeclaration)
		) return std::nullopt;
		CommaDecl Found { Node };
		// 並びの節点の最初の要素の子から `,` で区切った要素（要素に含める子が続く限り）の追加
		const auto AddList = [&Syntax, &Found](const NodeId Host, const NodeId First, const auto &IsItemPiece) -> void {
			DeclList List { Host, { {} } };
			const std::span<const NodeId> Children = Syntax.ChildrenOf(Host);
			const uint32_t Count = static_cast<uint32_t>(Children.size());
			for(uint32_t Index = Syntax.GetNode(First).Index; Index < Count; ++Index) {
				if(const NodeId Child = Children[Index]; Syntax.IsTokenRole(Child, Role::CommaToken)) List.Items.emplace_back();
				else if(IsItemPiece(Child)) List.Items.back().push_back(Child);
				else break;
			}
			Found.Lists.push_back(std::move(List));
		};
		const auto IsNamedPiece = [&Syntax](const NodeId Child) -> bool {
			// 名前付の子かの返戻
			return Syntax.IsNamed(Child);
		};
		// 名前の場を持つ唯一の仕様（宣言子其の物は宣言子の並びとして読む）
		const NodeId Spec = Syntax.NamedChildCount(Node) == 1 &&
		!(Syntax.Is(Syntax.NamedChild(Node, 0), Role::DeclaratorHead) || Syntax.Is(Syntax.NamedChild(Node, 0), Role::DeclaratorItem)) ?
		Syntax.NamedChild(Node, 0) :
		NoNode;
		if(const NodeId Left = Syntax.FieldChild(Node, ChildRole::Left); Left != NoNode) {
			AddList(Left, Syntax.NamedChild(Left, 0), IsNamedPiece);
			const NodeId Right = Syntax.FieldChild(Node, ChildRole::Right);
			AddList(Right, Syntax.NamedChild(Right, 0), IsNamedPiece);
		} else if(const NodeId Name = Syntax.FieldChild(Spec, ChildRole::Name); Name != NoNode) {
			AddList(
				Spec,
				Name,
				[&Syntax](const NodeId Child) -> bool {
					// 名前の場の子かの返戻
					return Syntax.HasField(Child, ChildRole::Name);
				}
			);
			const NodeId Values = Syntax.FieldChild(Spec, ChildRole::Value);
			// 初期化子も型も無い仕様の返戻
			if(Values == NoNode && Syntax.FieldChild(Spec, ChildRole::Type) == NoNode) return std::nullopt;
			if(Values != NoNode) AddList(Values, Syntax.NamedChild(Values, 0), IsNamedPiece);
		} else {
			// 宣言の中の変数の宣言 (C#) か宣言自身
			NodeId Host = Node;
			for(const NodeId Child : Syntax.ChildrenOf(Node)) if(Host == Node && Syntax.Is(Child, Role::CommaDeclaration)) Host = Child;
			const std::span<const NodeId> Children = Syntax.ChildrenOf(Host);
			const std::span<const NodeId>::iterator Head = std::ranges::find_if(
				Children,
				[&Syntax](const NodeId Child) -> bool {
					// 宣言子の場の子か宣言子を始める節点かの返戻
					return Syntax.HasField(Child, ChildRole::Declarator) || Syntax.Is(Child, Role::DeclaratorItem) ||
					Syntax.Is(Child, Role::DeclaratorHead);
				}
			);
			const NodeId Type = Syntax.FieldChild(Host, ChildRole::Type);
			// 宣言子の無い宣言と型推論の `var` の返戻
			if(Head == Children.end() || Syntax.IsWord(Type, Word::InferredType)) return std::nullopt;
			AddList(
				Host,
				*Head,
				[&Syntax](const NodeId Child) -> bool {
					// 終端の前の子かの返戻
					return !Syntax.IsTokenRole(Child, Role::StatementTerminatorToken);
				}
			);
		}
		// 本体の塊かの判定
		const auto IsBody = [&Syntax](const NodeId Piece) -> bool {
			// 本体の塊かの返戻
			return Syntax.Is(Piece, Role::IndentContainer);
		};
		for(const DeclList &List : Found.Lists) {
			// 空の要素（末尾のカンマ）と本体を持つ宣言子の返戻
			for(const std::vector<NodeId> &Item : List.Items) if(Item.empty() || std::ranges::any_of(Item, IsBody)) return std::nullopt;
		}
		// 名前と初期化子の数の異なる宣言の返戻
		if(Found.Lists.size() > 1 && Found.Lists[0].Items.size() != Found.Lists[1].Items.size()) return std::nullopt;
		// 要素の子の並びと要素の番号
		std::unordered_map<NodeId, DeclPiece> ItemPieces;
		for(size_t List = 0; List < Found.Lists.size(); ++List) {
			for(size_t Item = 0; Item < Found.Lists[List].Items.size(); ++Item) for(const NodeId Piece : Found.Lists[List].Items[Item]) {
				ItemPieces.emplace(Piece, DeclPiece{ Piece, static_cast<int32_t>(List), static_cast<uint32_t>(Item) });
			}
		}
		// 宣言と並びの節点の子の原文順の収集（要素の間の `,` を除く）
		// 種別は共通の定義の同一性で表す（役割の集合の字面を連結しない，整形仕様 5.6, 9.2）
		const auto KindOf = [&Syntax](const NodeId Part) -> std::string {
			// 共有する種別の定義の番地の返戻
			return std::to_string(reinterpret_cast<uintptr_t>(Syntax.GetNode(Part).Kind.Definition));
		};
		Found.Key.assign(KindOf(Node)).push_back('\x1F');
		// 比較キーへの長さ付の字面の追記
		const auto AppendAtom = [&Found](const std::string_view Value) -> void {
			Found.Key.append(std::to_string(Value.size())).push_back(':');
			Found.Key.append(Value);
		};
		// 比較キーへの節点の形の追記
		const auto AppendShape = [&Syntax, &Found, &AppendAtom, &KindOf](const NodeId Part) -> void {
			std::vector<std::pair<NodeId, bool>> Pending { { Part, false } };
			while(!Pending.empty()) {
				const auto [CurrentId, IsClosing] = Pending.back();
				Pending.pop_back();
				if(IsClosing) {
					Found.Key.push_back(')');
					continue;
				}
				// 開閉を含む節点の形の記録
				const SyntaxNode &Current = Syntax.GetNode(CurrentId);
				Found.Key.push_back('(');
				AppendAtom(KindOf(CurrentId));
				AppendAtom(Current.Lexeme);
				Found.Key.append(std::to_string(Current.Fields)).push_back(':');
				Pending.emplace_back(CurrentId, true);
				const std::span<const NodeId> Children = Syntax.ChildrenOf(CurrentId);
				for(std::span<const NodeId>::reverse_iterator Child = Children.rbegin(); Child != Children.rend(); ++Child) {
					Pending.emplace_back(*Child, false);
				}
			}
		};
		// 並びの節点と共有部の原文順の収集
		const auto Collect = [&](const NodeId Parent) -> void {
			std::vector<std::pair<NodeId, size_t>> Pending { { Parent, 0 } };
			while(!Pending.empty()) {
				std::pair<NodeId, size_t> &Frame = Pending.back();
				const std::span<const NodeId> Children = Syntax.ChildrenOf(Frame.first);
				if(Frame.second == Children.size()) {
					Pending.pop_back();
					continue;
				}
				const NodeId Child = Children[Frame.second++];
				if(
					Child != Frame.first && std::ranges::any_of(
						Found.Lists,
						[Child](const DeclList &List) -> bool {
							return List.Host == Child;
						}
					)
				) {
					Pending.emplace_back(Child, 0);
					continue;
				}
				// 宣言子の部品と共有する部品の分別
				if(const std::unordered_map<NodeId, DeclPiece>::const_iterator Piece = ItemPieces.find(Child); Piece != ItemPieces.end()) {
					Found.Pieces.push_back(Piece->second);
					continue;
				}
				if(
					Syntax.IsTokenRole(Child, Role::CommaToken) && ItemPieces.contains(Syntax.PrevSibling(Child)) &&
					ItemPieces.contains(Syntax.NextSibling(Child))
				) continue;
				Found.Pieces.push_back({ Child });
				// 文末の明示的な区切りは，共有する宣言の構文を変えない為，比較キーに含めない
				if(Syntax.IsTokenRole(Child, Role::StatementTerminatorToken) && Syntax.NextSibling(Child) == NoNode) continue;
				AppendShape(Child);
				Found.Key.push_back('\x1F');
			}
		};
		Collect(Node);
		// 関数の宣言子（宣言子の中の関数ポインタを含む）と，本体を持つ型（型の定義）と宣言子の記号を含むマクロの型の宣言の返戻
		for(const DeclPiece &Piece : Found.Pieces) if(
			Piece.List >= 0 ?
			AnyNodeOf(
				Syntax,
				Piece.Node,
				[&Syntax](const NodeId Sub) -> bool {
					// 関数の宣言子かの返戻
					return Syntax.Is(Sub, Role::FunctionDeclarator);
				}
			) :
			Syntax.FieldChild(Piece.Node, ChildRole::Body) != NoNode ||
			Syntax.GetNode(Piece.Node).Facts.Has(ConstraintKind::DeclaratorMacro)
		) return std::nullopt;
		// 宣言の部品の返戻
		return Found;
	}

	/**
	 * 節点が部分木に含まれるかの判定関数
	 * @param Ancestor 部分木の根
	 * @param Node 節点
	 * @return 節点が根自身か其の子孫なら true
	 */
	bool SyntaxModel::Contains(const NodeId Ancestor, const NodeId Node) const {
		// 空の部分木は何も含まない事の返戻
		if(Ancestor == NoNode) return false;
		for(NodeId Up = Node; Up != NoNode; Up = ParentOf(Up)) {
			Charge(1);
			// 祖先に届いた事の返戻
			if(Up == Ancestor) return true;
		}
		// 部分木の外の返戻
		return false;
	}

	/**
	 * 部分木の写しの生成節点の取得関数
	 * 宣言の分割で共有する型・修飾を各宣言へ写す（写しは錨を持たない）
	 * @param Id 写す部分木の根
	 * @return 種別・場・字面・構文事実・子を写した生成節点
	 */
	GeneratedNode SyntaxModel::CopyOf(const NodeId Id) const {
		// 節点単位の写しの作成
		const auto MakeCopy = [this](const NodeId CurrentId) -> GeneratedNode {
			const SyntaxNode &Node = Nodes.at(CurrentId);
			GeneratedNode Copy { {}, Node.Kind.IsNamed(), {}, Node.Fields, NoNode, Node.Lexeme, Node.Kind.Definition };
			Copy.Facts = Node.Facts;
			Copy.Source = CurrentId;
			// 節点の写しの返戻
			return Copy;
		};
		GeneratedNode Copy = MakeCopy(Id);
		struct Frame {
			NodeId Id;
			GeneratedNode *Out;
			size_t NextChild = 0;
		};
		// 子孫の写しの展開
		std::vector<Frame> Pending { { Id, &Copy } };
		while(!Pending.empty()) {
			Frame &Current = Pending.back();
			const SyntaxNode &Node = Nodes.at(Current.Id);
			if(Current.NextChild == Node.Children.size()) {
				Pending.pop_back();
				continue;
			}
			const NodeId Child = Node.Children[Current.NextChild++];
			Current.Out->Children.push_back(MakeCopy(Child));
			Pending.push_back({ Child, &Current.Out->Children.back() });
		}
		// 写しの返戻
		return Copy;
	}

	/**
	 * 元の節点から生成する節点の子の境界の組直関数
	 * 元と同じ子が隣接する境界だけを写し，除いた宣言子や区切りを越える境界は引継がない
	 * @param Generated 子を組み立てた生成節点
	 */
	void SyntaxModel::RemapGeneratedBoundaries(GeneratedNode &Generated) const {
		// 生成部分木の子の境界の組直
		std::vector<GeneratedNode *> Pending { &Generated };
		while(!Pending.empty()) {
			GeneratedNode &Current = *Pending.back();
			Pending.pop_back();
			if(Current.Source != NoNode) {
				const SyntaxNode &Original = Nodes.at(Current.Source);
				std::vector<SemanticBoundary> Boundaries;
				const auto SourceOf = [](const GeneratedNode &Child) -> NodeId {
					// 移す節点又は写す節点の識別子の返戻
					return Child.Existing != NoNode ? Child.Existing : Child.Source;
				};
				// 元の隣接を保つ境界の複写
				const auto CopyBoundary = [&Original, &Boundaries](const uint32_t Before, const uint32_t After) -> void {
					if(const SemanticBoundary *const Found = Original.Facts.BoundaryBefore(Before)) {
						SemanticBoundary Copy = *Found;
						Copy.Before = After;
						Boundaries.push_back(std::move(Copy));
					}
				};
				if(!Original.Children.empty() && !Current.Children.empty()) {
					if(SourceOf(Current.Children.front()) == Original.Children.front()) CopyBoundary(0, 0);
					for(uint32_t Index = 1; Index < Current.Children.size(); ++Index) {
						const NodeId Previous = SourceOf(Current.Children[Index - 1]), Next = SourceOf(Current.Children[Index]);
						if(Previous == NoNode || Next == NoNode) continue;
						if(
							const SyntaxNode &Left = Nodes.at(Previous), &Right = Nodes.at(Next);
							Left.Parent == Current.Source && Right.Parent == Current.Source && Left.Index + 1 == Right.Index
						) CopyBoundary(Right.Index, Index);
					}
					if(SourceOf(Current.Children.back()) == Original.Children.back()) {
						CopyBoundary(static_cast<uint32_t>(Original.Children.size()), static_cast<uint32_t>(Current.Children.size()));
					}
				}
				Current.Facts.Boundaries = SemanticBoundaryList(std::move(Boundaries));
			}
			for(std::vector<GeneratedNode>::reverse_iterator Child = Current.Children.rbegin(); Child != Current.Children.rend(); ++Child) {
				Pending.push_back(&*Child);
			}
		}
		// 終了
		return;
	}

	/**
	 * 文書の順で前に在るかの判定関数
	 * 共通の祖先の下の子の位置で比べ，祖先は子孫より前とする
	 * @param Lhs 左の節点
	 * @param Rhs 右の節点
	 * @return Lhs が Rhs より前に始まれば true
	 */
	bool SyntaxModel::Precedes(const NodeId Lhs, const NodeId Rhs) const {
		// 同じ節点と空の節点は前に無い事の返戻
		if(Lhs == Rhs || Lhs == NoNode || Rhs == NoNode) return false;
		// 根から節点迄の経路
		const auto PathOf = [this](NodeId Node) -> std::vector<NodeId> {
			std::vector<NodeId> Path;
			for(; Node != NoNode; Node = ParentOf(Node)) {
				Charge(1);
				Path.push_back(Node);
			}
			std::reverse(Path.begin(), Path.end());
			// 経路の返戻
			return Path;
		};
		const std::vector<NodeId> Left = PathOf(Lhs), Right = PathOf(Rhs);
		// 共通の祖先と分岐位置の比較
		size_t Depth = 0;
		while(Depth < Left.size() && Depth < Right.size()) {
			Charge(2);
			if(Left[Depth] != Right[Depth]) break;
			++Depth;
		}
		// 一方が他方の祖先なら祖先が前である事の返戻
		if(Depth == Left.size() || Depth == Right.size()) return Depth == Left.size();
		// 分かれた子の位置の比較の返戻
		return Nodes[Left[Depth]].Index < Nodes[Right[Depth]].Index;
	}

	/**
	 * 分岐の真枝の取得関数
	 * @param Id 分岐の節点
	 * @return `consequence` の場の子（無い文法は `body` の場の子）
	 */
	NodeId SyntaxModel::IfConsequence(const NodeId Id) const {
		const NodeId Consequence = FieldChild(Id, ChildRole::Consequence);
		// 真枝の返戻
		return Consequence == NoNode ? FieldChild(Id, ChildRole::Body) : Consequence;
	}

	/**
	 * 包装の除去関数（整形仕様 3.2）
	 * 中身（唯一の名前付の子か，前後の区切りに挟まれた無名の字句）を包装の親の同じ位置・場へ移し，包装と其の他の字句を除く
	 * 包装の先行・内部の錨と中身の前の字句の錨は中身の前へ，末尾の錨と中身の後の字句の錨は中身の後へ移す
	 * @param Id 包装の節点
	 * @param Trace 錨の移譲の記録の追加先
	 * @throws FormatLimitExceeded 包装が中身を１つに決めれない場合（正規化の不具合）
	 */
	void SyntaxModel::UnwrapNode(const NodeId Id, RewriteTrace &Trace) {
		Touch(Id);
		Touch(Nodes[Id].Parent);
		// 名前付の唯一の子か，前後の区切りに挟まれた無名の字句
		const NodeId Content = NamedChildCount(Id) == 1 ? NamedChild(Id, 0) : ChildCount(Id) == 3 ? Child(Id, 1) : NoNode;
		if(Content == NoNode) throw FormatLimitExceeded("an unwrapped node has no single content", FailureReason::FormatterDefect);
		const AnchorTarget Before { Content, false }, After { Content, true };
		Touch(Content);
		for(const NodeId Child : Nodes[Id].Children) {
			if(Child == Content) continue;
			IsRemoved[Child] = 1;
			Touch(Child);
			const AnchorTarget &Target = Nodes[Child].Index < Nodes[Content].Index ? Before : After;
			Trace.Transfers.push_back({ Child, Target, Target, Target });
		}
		Trace.Transfers.push_back({ Id, Before, After, Before });
		// 中身による包装の置換
		SyntaxNode &Wrapper = Nodes[Id], &Moved = Nodes[Content];
		Moved.Parent = Wrapper.Parent;
		Moved.Fields = Wrapper.Fields;
		Moved.Index = Wrapper.Index;
		++Moved.Generation;
		if(Wrapper.Parent == NoNode) Root = Content;
		else {
			Nodes[Wrapper.Parent].Children[Wrapper.Index] = Content;
			++Nodes[Wrapper.Parent].Generation;
		}
		// 除いた包装の親子を切り，後の操作が中身の位置を包装の位置と取り違えない様にする
		++Wrapper.Generation;
		Wrapper.Parent = NoNode;
		Wrapper.Children.clear();
		IsRemoved[Id] = 1;
		// 終了
		return;
	}

	/**
	 * 投影を終えた構文の基準量からの作業単位の上限の確定関数（整形仕様 9.2）
	 * 基準量 `W = max(1, N + B)` は節点数 N と字句の葉・逐語区画・保持境界の字面のバイト数 B の和で，保持境界以外の非意味的な空白とコメントを含まない
	 * 計算量：節点数 V と字面の長さの和 L に対し O(V)
	 */
	void SyntaxModel::FixWorkLimit() {
		size_t Bytes = 0;
		for(const SyntaxNode &Node : Nodes) {
			Bytes += Node.Lexeme.size();
			for(const SemanticBoundary &Boundary : Node.Facts.Boundaries.View()) {
				if(Boundary.Kind == BoundaryKind::Preserved) Bytes += Boundary.Gap.size();
			}
		}
		// 小さな入力でも足りる固定費と入力に比例する費用の大きい方（投影の作業は此の予算へ数えない）
		Work->Limit = std::max(WorkBudget, std::max<size_t>(1, Nodes.size() + Bytes) << WorkPerBaseShift);
		Work->Used = 0;
		// 終了
		return;
	}

	/**
	 * 合成した行先の鍵の取得関数
	 * @param Anchor 元の錨
	 * @param Side 元の置き方
	 * @param IsInline 文中の記録か（空に畳む本体への移譲で置き方が分かれる）
	 * @return 鍵
	 */
	uint64_t RewriteTrace::MoveKey(const NodeId Anchor, const AnchorSide Side, const bool IsInline) {
		// 錨・置き方・文中かを詰めた鍵の返戻
		return uint64_t{ Anchor } << 3 | uint64_t{ static_cast<uint8_t>(Side) } << 1 | IsInline;
	}

	/**
	 * 移譲の連鎖の合成関数（整形仕様 7.2）
	 * 移譲の元の錨毎に全ての置き方と文中かの組を仮の記録として適用順に移し，最終の行先を元の組へ対応付ける
	 * 同じ錨で同じ行先の状態に為った記録は群へ纏めて移し，群の併合は小さい方の記録を大きい方へ移す
	 * 計算量：移譲の数 T と移譲の元の錨の数 F に対し O(T + F log F)
	 */
	void RewriteTrace::Compose() {
		Moves.clear();
		// 同じ行先の状態を持つ仮の記録の群（元の組の鍵の並びと，文中かと，現在の行先）
		struct ProbeGroup {
			std::vector<uint64_t> Keys;
			bool IsInline;
			AnchorMove Move;
		};
		std::vector<ProbeGroup> Groups;
		std::unordered_map<NodeId, std::vector<size_t>> AtAnchor;
		// 行先の状態の等しい群かの判定
		const auto IsSameState = [](const ProbeGroup &Left, const ProbeGroup &Right) -> bool {
			// 文中かと行先の全ての欄の一致の返戻
			return Left.IsInline == Right.IsInline && Left.Move.Node == Right.Move.Node && Left.Move.Side == Right.Move.Side &&
			Left.Move.IsEmptyBody == Right.Move.IsEmptyBody && Left.Move.IsRemovedCaseBody == Right.Move.IsRemovedCaseBody &&
			Left.Move.Reset == Right.Move.Reset;
		};
		for(const AnchorTransfer &Transfer : Transfers) {
			if(AtAnchor.contains(Transfer.From)) continue;
			std::vector<size_t> &Seeded = AtAnchor[Transfer.From];
			// 錨の前・後・内部の順に仮の記録を作る
			for(const AnchorSide Side : { AnchorSide::Before, AnchorSide::After, AnchorSide::Inside }) {
				for(const bool IsInline : { false, true }) {
					Seeded.push_back(Groups.size());
					Groups.push_back({ { MoveKey(Transfer.From, Side, IsInline) }, IsInline, { Transfer.From, Side } });
				}
			}
		}
		// 元の錨の仮の記録の種を撒いた後の，適用順の移譲
		for(const AnchorTransfer &Transfer : Transfers) {
			const std::unordered_map<NodeId, std::vector<size_t>>::iterator Found = AtAnchor.find(Transfer.From);
			if(Found == AtAnchor.end()) continue;
			std::vector<size_t> Moving = std::move(Found->second);
			AtAnchor.erase(Found);
			for(const size_t Index : Moving) {
				AnchorMove &Move = Groups[Index].Move;
				const AnchorTarget &Target =
				Move.Side == AnchorSide::Before ? Transfer.Leading : Move.Side == AnchorSide::Inside ? Transfer.Inner : Transfer.Trailing;
				// 行先の無い置き方は以後動かさない（此の置き方の記録が在ればコメント側が不具合とする）
				if(Target.Node == NoNode) {
					Move.Node = NoNode;
					continue;
				}
				Move.Node = Target.Node;
				if(Target.IsCaseBody) {
					Move.Side = AnchorSide::After;
					Move.Reset = TokenReset::None;
					Move.IsRemovedCaseBody = true;
				} else if(Target.IsEmptyBody) {
					Move.Side = Groups[Index].IsInline ? AnchorSide::Inside : AnchorSide::Before;
					if(Groups[Index].IsInline) Move.Reset = TokenReset::AllInsideAnchor;
					else Move.IsEmptyBody = true;
				} else if(Move.Side == AnchorSide::Before || Move.Side == AnchorSide::After) {
					// 行先の前後の何方の錨に為るかの反映（錨の中の字句の前の物は行先でも同じ所属を保つ）
					Move.Side = Target.IsAfter ? AnchorSide::After : AnchorSide::Before;
				}
				// 行先の錨で同じ状態の群への併合（小さい方の記録を大きい方へ移す）
				std::vector<size_t> &Arrived = AtAnchor[Target.Node];
				const std::vector<size_t>::iterator Same = std::ranges::find_if(
					Arrived,
					[&](const size_t Other) -> bool {
						// 同じ状態の群かの返戻
						return IsSameState(Groups[Other], Groups[Index]);
					}
				);
				if(Same == Arrived.end()) {
					Arrived.push_back(Index);
					continue;
				}
				if(Groups[*Same].Keys.size() < Groups[Index].Keys.size()) std::swap(Groups[*Same].Keys, Groups[Index].Keys);
				Groups[*Same].Keys.insert(Groups[*Same].Keys.end(), Groups[Index].Keys.begin(), Groups[Index].Keys.end());
				Groups[Index].Keys.clear();
			}
		}
		// 元の組毎の最終行先の記録
		for(const ProbeGroup &Moved : Groups) for(const uint64_t Key : Moved.Keys) Moves.emplace(Key, Moved.Move);
		// 終了
		return;
	}

	/**
	 * ネストの段数の検査関数（整形仕様 9.2，深さを走査の呼出でなく構造の段数で測る）
	 * 計算量：節点数 V に対し O(V)
	 * @throws FormatLimitExceeded 根からの段数が上限を超える場合（規定した資源上限の見送）
	 */
	void SyntaxModel::CheckNesting() const {
		std::vector<std::pair<NodeId, uint32_t>> Stack { { Root, 1 } };
		while(!Stack.empty()) {
			const auto [Node, Depth] = Stack.back();
			Stack.pop_back();
			// 上限を超える段数の見送
			if(Depth > MaxNestingDepth) throw FormatLimitExceeded("nesting exceeds formatting limit");
			for(const NodeId Child : Nodes[Node].Children) Stack.emplace_back(Child, Depth + 1);
		}
		// 終了
		return;
	}

	/**
	 * 除く節点の内部の錨の移譲関数（整形仕様 3.2, 7.2）
	 * 除いた節点に繋がった儘の子孫は出力に現れない為，其の錨を除いた節点の内部の錨の行先へ移す
	 * 同じ計画で個別に除いた子孫（宣言の統合の部品等）は其の移譲に従う為に移さない
	 * @param Transfer 除いた節点の錨の移譲
	 * @param Trace 錨の移譲の記録の追加先
	 */
	void SyntaxModel::TransferInnerAnchors(const AnchorTransfer &Transfer, RewriteTrace &Trace) {
		std::vector<NodeId> Pending(Nodes.at(Transfer.From).Children.begin(), Nodes.at(Transfer.From).Children.end());
		while(!Pending.empty()) {
			const NodeId Inner = Pending.back();
			Pending.pop_back();
			// 子孫の１回の検査を１作業単位とする（整形仕様 9.2）
			Charge(1);
			// 個別に除いた子孫の読飛（其の子孫も其の子孫自身の移譲が扱う）
			if(IsRemoved[Inner]) continue;
			IsRemoved[Inner] = 1;
			Touch(Inner);
			Trace.Transfers.push_back({ Inner, Transfer.Inner, Transfer.Inner, Transfer.Inner });
			Pending.insert(Pending.end(), Nodes[Inner].Children.begin(), Nodes[Inner].Children.end());
		}
		// 終了
		return;
	}

	/**
	 * 書換の１操作の適用関数
	 * 対象の節点と子の並び・境界・世代を一体で更新し，除いた節点の錨の行先を記録する
	 * @param Step 書換の操作
	 * @param Trace 錨の移譲の記録の追加先
	 * @param RemovedRoots 除いた部分木の根の錨の行先の追加先（内部の錨は全ての操作の後に移す）
	 * @throws FormatLimitExceeded 纏めて除く子が対象の子でない場合（正規化の不具合）
	 */
	void SyntaxModel::ApplyStep(const RewriteStep &Step, RewriteTrace &Trace, std::vector<AnchorTransfer> &RemovedRoots) {
		Touch(Step.Target);
		Touch(Nodes.at(Step.Target).Parent);
		Touch(Step.Destination);
		// 操作に対応する節点と錨の更新
		switch(Step.Kind) {
		case RewriteKind::SetLexeme:
			++Nodes.at(Step.Target).Generation;
			Nodes[Step.Target].Lexeme = Step.Text;
			break;
		case RewriteKind::Insert:
			Attach(AddGenerated(Step.Inserted, Step.Target, Step.Index), Step.Target, Step.Index, Step.Boundary, Step.RecordedBoundary);
			break;
		case RewriteKind::Wrap:
			{
				// 対象の位置・場と，原文の間隙を保つ前の境界を包装へ引き継ぐ（切り離す前に控える）
				const NodeId Host = Nodes.at(Step.Target).Parent;
				const uint32_t Position = Nodes[Step.Target].Index;
				const uint64_t Roles = Nodes[Step.Target].Fields;
				const SemanticBoundary *const Before = Host == NoNode ? nullptr : Nodes[Host].Facts.BoundaryBefore(Position);
				const std::optional<SemanticBoundary> Kept =
				Step.RecordedBoundary || !Before || Before->Kind != BoundaryKind::Preserved ? Step.RecordedBoundary : *Before;
				Detach(Step.Target);
				const NodeId Wrapper = AddGenerated(Step.Inserted, Host, Position);
				Nodes[Wrapper].Fields = Roles;
				Attach(Wrapper, Host, Position, Step.Boundary, Kept);
				// 括弧の包装の外に在った先行・末尾の錨の包装の前後への移譲（原文の括弧の内側のコメントとの関係を保つ，整形仕様 7.2）
				if(Step.Transfer.From != NoNode) {
					Trace.Transfers.push_back({ Step.Target, { Wrapper, false }, { Wrapper, true }, { Step.Target, false } });
				}
				break;
			}
		case RewriteKind::Reshape:
			ReshapeNode(Step, Trace);
			break;
		case RewriteKind::Retarget:
			Trace.Transfers.push_back(Step.Transfer);
			Trace.Transfers.back().From = Step.Target;
			break;
		case RewriteKind::Move:
			Detach(Step.Target);
			Attach(Step.Target, Step.Destination, Step.Index, Step.Boundary, Step.RecordedBoundary);
			if(Step.DestinationFields) Nodes[Step.Target].Fields = *Step.DestinationFields;
			break;
		case RewriteKind::Unwrap:
			UnwrapNode(Step.Target, Trace);
			break;
		case RewriteKind::Remove:
			if(Step.RemoveFollowingBoundary) {
				if(const SyntaxNode &Removed = Nodes.at(Step.Target); !Nodes.at(Removed.Parent).Facts.Boundaries.View().empty()) {
					std::erase_if(
						Nodes.at(Removed.Parent).Facts.Boundaries.Edit(),
						[&Removed](const SemanticBoundary &Boundary) -> bool {
							// 消える節点の後の境界かの返戻
							return Boundary.Before == Removed.Index + 1;
						}
					);
				}
			}
			Detach(Step.Target);
			IsRemoved[Step.Target] = 1;
			Trace.Transfers.push_back(Step.Transfer);
			Trace.Transfers.back().From = Step.Target;
			RemovedRoots.push_back(Trace.Transfers.back());
			break;
		case RewriteKind::RemoveSiblings:
			{
				++Nodes.at(Step.Target).Generation;
				std::vector<NodeId> &Children = Nodes[Step.Target].Children;
				Charge(Children.size());
				// 除く子の検査と印付
				std::vector<uint8_t> Removed(Children.size(), 0);
				for(const AnchorTransfer &Transfer : Step.RemovedSiblings) {
					Charge(1);
					const SyntaxNode &Child = Nodes.at(Transfer.From);
					if(Child.Parent != Step.Target || Removed[Child.Index]) {
						throw FormatLimitExceeded("a grouped removal has an invalid child", FailureReason::FormatterDefect);
					}
					Removed[Child.Index] = 1;
				}
				Charge(Children.size() + 1);
				// 除去数の累積
				std::vector<uint32_t> Before(Children.size() + 1, 0);
				for(size_t Index = 0; Index < Children.size(); ++Index) {
					Charge(1);
					Before[Index + 1] = Before[Index] + Removed[Index];
				}
				// 境界の番号の詰直
				if(!Nodes[Step.Target].Facts.Boundaries.View().empty()) {
					std::vector<SemanticBoundary> &Boundaries = Nodes[Step.Target].Facts.Boundaries.Edit();
					Charge(Boundaries.size());
					std::erase_if(
						Boundaries,
						[&Removed](const SemanticBoundary &Boundary) -> bool {
							return Boundary.Before < Removed.size() && Removed[Boundary.Before];
						}
					);
					for(SemanticBoundary &Boundary : Boundaries) {
						Charge(1);
						Boundary.Before -= Before.at(Boundary.Before);
					}
				}
				// 残す子の並びの詰直
				uint32_t Kept = 0;
				for(uint32_t Index = 0; Index < Children.size(); ++Index) {
					Charge(1);
					const NodeId Child = Children[Index];
					if(Removed[Index]) {
						Touch(Child);
						Nodes[Child].Parent = NoNode;
						IsRemoved[Child] = 1;
						continue;
					}
					Children[Kept] = Child;
					Nodes[Child].Index = Kept++;
				}
				Children.resize(Kept);
				Trace.Transfers.insert(Trace.Transfers.end(), Step.RemovedSiblings.begin(), Step.RemovedSiblings.end());
				RemovedRoots.insert(RemovedRoots.end(), Step.RemovedSiblings.begin(), Step.RemovedSiblings.end());
				break;
			}
		}
		// 終了
		return;
	}

	/**
	 * 書換の計画の適用関数
	 * 操作を順に適用し，除いた節点の錨の行先と変更した節点を記録する（再解析しない，整形仕様 3.4, 5.1）
	 * @param Plan 書換の計画
	 * @return 錨の移譲の記録
	 * @throws std::out_of_range 計画が節点表に無い節点を指す場合
	 * @throws FormatLimitExceeded 文法が加える字句を持たない場合（正規化の不具合）
	 */
	RewriteTrace SyntaxModel::Apply(const RewritePlan &Plan) {
		LastTouched.clear();
		// 書換の１操作を１作業単位とする（整形仕様 9.2）
		Charge(Plan.Steps.size());
		RewriteTrace Trace;
		// 計画を集めた時の対象と移す先の世代
		std::vector<std::pair<uint32_t, uint32_t>> Generations;
		Generations.reserve(Plan.Steps.size());
		for(const RewriteStep &Step : Plan.Steps) {
			Generations.emplace_back(
				Nodes.at(Step.Target).Generation,
				Step.Destination == NoNode ? 0 : Nodes.at(Step.Destination).Generation
			);
		}
		// 除いた節点の錨の移譲（子孫の内部の錨は全ての操作の後に明示の移譲の無い物だけを移す）
		std::vector<AnchorTransfer> RemovedRoots;
		for(size_t Rule = 0, Index = 0; Index < Plan.Steps.size(); ++Index) {
			// 規則の始まりで，前の規則が書き換えた節点を対象にする操作を拒む（同じ範囲を読み書きする規則は別の計画で規則順に適用する，整形仕様 5.1）
			if(Rule < Plan.RuleStarts.size() && Plan.RuleStarts[Rule] <= Index) {
				while(Rule < Plan.RuleStarts.size() && Plan.RuleStarts[Rule] <= Index) ++Rule;
				const size_t End = Rule < Plan.RuleStarts.size() ? Plan.RuleStarts[Rule] : Plan.Steps.size();
				for(size_t Later = Index; Later < End; ++Later) if(
					const RewriteStep &Step = Plan.Steps[Later];
					Nodes[Step.Target].Generation != Generations[Later].first ||
					Step.Destination != NoNode && Nodes[Step.Destination].Generation != Generations[Later].second
				) throw FormatLimitExceeded("rewrite steps conflict on the same node", FailureReason::FormatterDefect);
			}
			ApplyStep(Plan.Steps[Index], Trace, RemovedRoots);
		}
		// 除いた部分木の内部の錨の移譲と変更節点の整理
		for(const AnchorTransfer &Transfer : RemovedRoots) TransferInnerAnchors(Transfer, Trace);
		std::sort(LastTouched.begin(), LastTouched.end());
		LastTouched.erase(std::unique(LastTouched.begin(), LastTouched.end()), LastTouched.end());
		if(HasRoleIndex) for(const NodeId Id : LastTouched) IndexRoles(Id);
		// 錨の移譲の記録の返戻
		return Trace;
	}

	/**
	 * 変更前の値の控えの開始関数
	 * 以後の書換が最初に変える節点の変更前の値だけを控え，控えを始めた時点の構文を全節点の複写無に引ける様にする
	 */
	void SyntaxModel::BeginJournal() {
		Journal.emplace();
		JournalEnd = Nodes.size();
		// 終了
		return;
	}

	/**
	 * 変更前の値の控えの終了関数
	 */
	void SyntaxModel::EndJournal() {
		Journal.reset();
		JournalEnd = 0;
		// 終了
		return;
	}

	/**
	 * コンストラクタ
	 * @param Definition 文法の構文・意味の固定の定義
	 * @param Mask 文法に在る子の役割
	 */
	SyntaxModel::SyntaxModel(const SyntaxRules &Definition, const uint64_t Mask) : ChildRoleMask(Mask), Definition(&Definition) {
		return;
	}

	/**
	 * 正規化の規則・制御の流れ・署名・コメント
	 * 役割を持つ最初の名前付の子の取得関数
	 * @param Id 親の節点
	 * @param Desired 求める役割
	 * @return 一致する子（無ければ無し）
	 */
	NodeId SyntaxModel::FirstNamedChildWithRole(const NodeId Id, const Role Desired) const {
		for(const NodeId Child : ChildrenOf(Id)) {
			Charge(1);
			// 指定の役割を持つ最初の子の返戻
			if(IsNamed(Child) && Is(Child, Desired)) return Child;
		}
		// 一致する子の無い事の返戻
		return NoNode;
	}
} // namespace Core
