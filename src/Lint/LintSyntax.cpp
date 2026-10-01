#include "LintAnalyzer.hpp"
#include "../Core/CommentRepository.hpp"
#include "../Core/DocCommentSynchronizer.hpp"
#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using Core::NodeId;
using Core::NoNode;
using Core::Role;
using Core::ChildRole;

/**
 * 最終投影を読む静的検査の構築子
 * @param Syntax 最終字面の独自構文
 * @param Source 検証済の最終字面
 * @param Comments 分離したコメント
 * @param Facts 最終位置と共通解析の資料
 */
LintAnalyzer::LintAnalyzer(
	const Core::SyntaxModel &Syntax,
	const std::string &Source,
	const Core::CommentRepository &Comments,
	const StaticLint::OutputFacts &Facts
) : Syntax(Syntax), Repository(Comments), Facts(Facts), Rules(Syntax.Rules()), Source(Source) {
	// 同じ字面の別コメントを混同しない様に，最終字面の開始位置で索引化
	for(const StaticLint::CommentFacts &Comment : Facts.Comments) {
		CommentTexts.emplace(Source.data() + Comment.Range.Start, &Comment);
	}
	// コメントの位置別索引を構築した後の終了
	return;
}

/**
 * 節点の開始位置の取得関数
 * @param Id 対象の節点
 * @return 最終字面の開始バイト位置（空の節点は０）
 */
uint32_t LintAnalyzer::BeginOf(const NodeId Id) const {
	return Id == NoNode ? 0 : Facts.Positions.at(Id).Start;
}

/**
 * 節点の終端位置の取得関数
 * @param Id 対象の節点
 * @return 最終字面の終端バイト位置（空の節点は０）
 */
uint32_t LintAnalyzer::EndOf(const NodeId Id) const {
	return Id == NoNode ? 0 : Facts.Positions.at(Id).End;
}

/**
 * 節点の出力位置の取得関数
 * @param Id 対象の節点
 * @return 最終字面の０始まりの行・列（空の節点は原点）
 */
StaticLint::OutputPoint LintAnalyzer::PointOf(const NodeId Id) const {
	return Id == NoNode ? StaticLint::OutputPoint{} : Facts.Positions.at(Id).Point;
}

/**
 * 定義の名前の節点の取得関数
 * @param Function 対象の関数
 * @return 名前の節点（無名なら空）
 */
NodeId LintAnalyzer::FunctionNameOf(const NodeId Function) const {
	// 名前の場が在る場合の其の節点の返戻
	if(const NodeId Name = Syntax.FieldChild(Function, ChildRole::Name); Name != NoNode) return Name;
	NodeId Found = NoNode;
	WalkChildrenCursor(
		Function,
		[this, &Found](const NodeId Child) -> bool {
			// 名前を取得済又は本体に達した場合の探索打切の返戻
			if(Found != NoNode || Syntax.Is(Child, Role::FunctionBodyContainer)) return false;
			if(Syntax.Is(Child, Role::FunctionNameIdentifier)) Found = Child;
			// 名前が見付からない場合の探索継続の返戻
			return Found == NoNode;
		}
	);
	// 探索した関数名の節点の返戻
	return Found;
}

/**
 * 関数の明示された戻値型の節点の取得関数
 * @param Function 対象の関数
 * @return 型の先頭の節点（明示無しなら空）
 */
NodeId LintAnalyzer::ExplicitReturnTypeOf(const NodeId Function) const {
	// 戻値型の場・型注釈・仮引数の後の結果型・後置の戻値型の順の探索
	NodeId Type = Syntax.FieldChild(Function, ChildRole::ReturnType);
	if(Type == NoNode) Type = Syntax.FirstNamedChildWithRole(Function, Role::TypeAnnotation);
	if(Type == NoNode) Type = Syntax.FirstNamedChildWithRole(Function, Role::PostParameterResultType);
	if(Type == NoNode) Type = Syntax.FirstNamedChildWithRole(Function, Role::TrailingReturnType);
	// 名前の前に戻値型を書く文法の型の場（推論の指定は明示された型でない）
	if(
		const NodeId Leading = Syntax.FieldChild(Function, ChildRole::Type);
		Type == NoNode && !Syntax.Is(Leading, Role::PlaceholderTypeSpecifier)
	) Type = Leading;
	if(Syntax.Is(Type, Role::TypeAnnotation) && Syntax.NamedChildCount(Type)) Type = Syntax.NamedChild(Type, 0);
	// 型の先頭の節点の返戻
	return Type;
}

/**
 * 署名内の仮引数名の節点の取得関数
 * @param Function 対象の関数
 * @param Name 対象の仮引数名
 * @return 名前の節点（無名又は見付からなければ空）
 */
NodeId LintAnalyzer::ParameterNameOf(const NodeId Function, const std::string_view Name) const {
	NodeId Found = NoNode;
	WalkChildrenCursor(
		Function,
		[this, &Found, Name](const NodeId Child) -> bool {
			// 名前を取得済又は本体に達した場合の探索打切の返戻
			if(Found != NoNode || Syntax.Is(Child, Role::FunctionBodyContainer)) return false;
			if(Syntax.Is(Child, Role::ParameterName) && Syntax.Text(Child) == Name) Found = Child;
			// 名前が見付からない場合の探索継続の返戻
			return Found == NoNode;
		}
	);
	// 探索した仮引数名の節点の返戻
	return Found;
}

/**
 * 親の直下に在る名前無字句の取得関数
 * @param Parent 対象の構文
 * @param Spelling 字句の綴り
 * @return 一致する字句の節点（無ければ空）
 */
NodeId LintAnalyzer::TokenChildOf(const NodeId Parent, const std::string_view Spelling) const {
	for(const NodeId Child : Syntax.ChildrenOf(Parent)) if(!Syntax.IsNamed(Child) && Syntax.Text(Child) == Spelling) return Child;
	return NoNode;
}

/**
 * コメント本文の位置を最終字面へ写す関数
 * @param Index コメントの記録の添字
 * @param Body 記法を除いた本文
 * @param Offset 本文内のバイト位置
 * @return 最終字面の行と UTF-8 バイト列
 */
StaticLint::OutputPoint LintAnalyzer::PointInComment(
	const size_t Index,
	const std::string_view Body,
	const size_t Offset
) const {
	const StaticLint::CommentFacts &Comment = Facts.Comments[Index];
	// 本文の外を指す位置の場合の元の位置の返戻
	if(Offset >= Body.size()) return Comment.Range.Point;
	const size_t BodyLine = Body.rfind('\n', Offset), BodyStart = BodyLine == std::string_view::npos ? 0 : BodyLine + 1;
	const size_t BodyEnd = std::min(Body.find('\n', BodyStart), Body.size());
	const std::string_view Segment = Body.substr(BodyStart, BodyEnd - BodyStart);
	const uint32_t LineIndex = static_cast<uint32_t>(std::count(Body.begin(), Body.begin() + BodyStart, '\n'));
	size_t SourceStart = Comment.Range.Start;
	for(uint32_t Line = 0; Line < LineIndex; ++Line) {
		const size_t End = Source.find('\n', SourceStart);
		// コメント内に改行が無い場合の元の位置の返戻
		if(End == std::string::npos || End >= Comment.Range.End) return Comment.Range.Point;
		SourceStart = End + 1;
	}
	const size_t SourceEnd = std::min(Source.find('\n', SourceStart), static_cast<size_t>(Comment.Range.End));
	const size_t Match = std::string_view(Source).substr(SourceStart, SourceEnd - SourceStart).find(Segment);
	// 字面が一致しない場合の元の位置の返戻
	if(Match == std::string_view::npos) return Comment.Range.Point;
	// 本文内の位置に対応する原文位置の返戻
	return {
		Comment.Range.Point.Row + LineIndex,
		static_cast<uint32_t>((LineIndex ? 0 : Comment.Range.Point.Column) + Match + Offset - BodyStart)
	};
}

/**
 * 投影した構文の破損の判定関数
 * @param Id 対象の節点
 * @return 未修復の構文破損を含むか
 */
bool LintAnalyzer::HasError(const NodeId Id) const {
	return Id != NoNode && Syntax.GetNode(Id).Facts.Has(Core::ConstraintKind::ContainsError);
}

/**
 * 単一式の最外の括弧の取得関数
 * 計算量：探索する節点又は字面の長さ N に対し O(N)
 * @param Id 内側の式
 * @return 同じ値を表す最外の括弧又は元の式
 */
NodeId LintAnalyzer::OutermostGroup(NodeId Id) const {
	for(
		NodeId Parent = Syntax.ParentOf(Id);
		Parent != NoNode && Syntax.Is(Parent, Role::GroupingParen) && Syntax.NamedChildCount(Parent) == 1 &&
		Syntax.ChildCount(Parent) == 3;
		Parent = Syntax.ParentOf(Id)
	) Id = Parent;
	// 値を変えずに包む最外の括弧の返戻
	return Id;
}

/**
 * 式の値を消費しない文脈の判定関数
 * @param Id 対象の式
 * @param Parent 直上の親
 * @return 式文又は反復の更新部か
 */
bool LintAnalyzer::IsValueUnusedContext(const NodeId Id, const NodeId Parent) const {
	// 文脈が無い場合の不一致の返戻
	if(Parent == NoNode) return false;
	// 値を捨てる文の一致の返戻
	if(Syntax.Is(Parent, Role::UnusedValueHost)) return true;
	// 更新欄を持たない構文の不一致の返戻
	if(!Syntax.Is(Parent, Role::CountedLoop)) return false;
	NodeId Update = Syntax.FieldChild(Parent, ChildRole::Update);
	if(Update == NoNode) Update = Syntax.FieldChild(Parent, ChildRole::Increment);
	// 更新欄が対象の式であるかの返戻
	return Update != NoNode && Update == Id;
}

/**
 * 最終字面の範囲を包む最小の子孫の取得関数
 * 計算量：探索する節点又は字面の長さ N に対し O(N)
 * @param Root 探索を始める根
 * @param Begin 範囲の始まり
 * @param End 範囲の終わり
 * @return 範囲を包む最も内側の節点
 */
NodeId LintAnalyzer::DescendantAt(NodeId Root, const uint32_t Begin, const uint32_t End) const {
	while(true) {
		NodeId Found = NoNode;
		for(const NodeId Child : Syntax.ChildrenOf(Root)) {
			if(BeginOf(Child) <= Begin) if(const uint32_t ChildEnd = EndOf(Child); ChildEnd >= End && ChildEnd > Begin) {
				Found = Child;
				break;
			}
		}
		// 範囲を包む更に内側の子が無い場合の返戻
		if(Found == NoNode) return Root;
		Root = Found;
	}
}

/**
 * コメントの投影資料の取得関数
 * @param Text 最終字面に在る投影済のコメントの範囲
 * @return 最終字面と本文を対応付ける資料
 * @throws std::logic_error 投影資料に無い字面を渡した場合
 */
const StaticLint::CommentFacts &LintAnalyzer::CommentInfo(const std::string_view Text) const {
	// 最終字面での開始位置が一致する資料の返戻
	if(
		const std::unordered_map<const char *, const StaticLint::CommentFacts *>::const_iterator Hit = CommentTexts.find(Text.data());
		Hit != CommentTexts.end() && Text.size() == Hit->second->Range.End - Hit->second->Range.Start
	) return *Hit->second;
	throw std::logic_error("final comment is not projected");
}

/**
 * 文書コメントの装飾を除いた論理行の取得関数
 * 最終字面を投影した記録（記法・役割・本文の分解済）の論理行を読む
 * 本文を其の儘保つ文書属性（Rust の文書）は，囲みの飾りの `*` と区切空白１つだけを除き，其の他の字下げ（入れ子の箇条等）を値として保つ
 * 計算量：本文の長さ N に対し O(N)
 * @param Text 最終字面のコメント
 * @return 本文の論理行の並び
 */
std::vector<std::string> LintAnalyzer::SourceLogicalLines(const std::string_view Text) const {
	const CommentRecord &Record = Repository.GetRecords()[CommentInfo(Text).Record];
	// 分解済の本文の論理行の返戻
	if(Record.Role != CommentRole::Directive) return Core::DocCommentSynchronizer::LogicalLines(Record);
	const bool IsBlock = Core::CommentRepository::IsBlockNotation(Record.Notation);
	std::vector<std::string> Lines;
	for(size_t Cursor = 0; Cursor <= Record.Text.size();) {
		const size_t End = std::min(Record.Text.find('\n', Cursor), Record.Text.size());
		std::string_view Line = std::string_view(Record.Text).substr(Cursor, End - Cursor);
		Cursor = End + 1;
		// 囲みの継続行の飾りの `*` と，記号・飾りの後の区切空白１つ
		if(IsBlock) {
			Line.remove_prefix(std::min(Line.find_first_not_of(" \t"), Line.size()));
			if(Line.starts_with('*')) Line.remove_prefix(1);
		}
		if(Line.starts_with(' ')) Line.remove_prefix(1);
		Lines.emplace_back(Line);
	}
	// 囲みの開始記号・終端記号だけの行の除去
	if(IsBlock && !Lines.empty() && Lines.back().find_first_not_of(" \t") == std::string::npos) Lines.pop_back();
	if(IsBlock && !Lines.empty() && Lines.front().empty()) Lines.erase(Lines.begin());
	// 飾りを除いた論理行の返戻
	return Lines;
}
