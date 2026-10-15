#include "LintAnalyzer.hpp"
#include "../Core/CommentRepository.hpp"
#include "../Core/ControlFlow.hpp"
#include "../Core/DocCommentReader.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using Core::ChildRole;
using Core::NodeId;
using Core::NoNode;
using Core::Role;

/**
 * コメントの投影データの取得関数
 * 計算量：期待値 O(1)
 * @param Text 最終テキストに在る投影済のコメントの範囲
 * @return 最終テキストと本文を対応付けるデータ
 * @throws std::logic_error 投影データに無いテキストを渡した場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
const StaticLint::CommentFacts &LintAnalyzer::GetCommentInfo(const std::string_view Text) const {
	// 最終テキストでの開始位置が一致するデータの返戻
	if(
		const std::unordered_map<const char *, const StaticLint::CommentFacts *>::const_iterator Hit = CommentTexts.find(Text.data());
		Hit != CommentTexts.end() && Text.size() == Hit->second->Range.End - Hit->second->Range.Start
	) return *Hit->second;
	throw std::logic_error("final comment is not projected");
}

/**
 * 最終投影を読む静的解析のコンストラクタ
 * 計算量：コメントの記録数 C に対し期待値 O(C)
 * @param Syntax 最終テキストの独自構文
 * @param Src 検証済の最終テキスト
 * @param Comments 分離したコメント
 * @param Facts 最終位置と共通解析のデータ
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 */
LintAnalyzer::LintAnalyzer(
	const Core::SyntaxModel &Syntax,
	const std::string &Src,
	const Core::CommentRepository &Comments,
	const StaticLint::OutputFacts &Facts
) : Syntax(Syntax), Repository(Comments), Facts(Facts), Flow(*Facts.Flow), Rules(Syntax.GetRules()), Src(Src) {
	// 同じテキストの別コメントを混同しない様に，最終テキストの開始位置でインデックス化
	if(Rules.Notation.Doc != Core::DocStyle::Docstring) {
		const char *const Text = Src.data();
		for(const StaticLint::CommentFacts &Comment : Facts.Comments) CommentTexts.emplace(Text + Comment.Range.Start, &Comment);
	}
	// 終了
	return;
}

/**
 * ノードの開始位置の取得関数
 * 計算量：O(1)
 * @param Id 対象のノード
 * @return 最終テキストの開始バイト位置（空のノードは０）
 */
uint32_t LintAnalyzer::GetBeginOf(const NodeId Id) const {
	// ノードの開始バイト位置又は空のノードの原点の返戻
	return Id == NoNode ? 0 : Facts.Positions[Id].Start;
}

/**
 * ノードの終端位置の取得関数
 * 計算量：O(1)
 * @param Id 対象のノード
 * @return 最終テキストの終端バイト位置（空のノードは０）
 */
uint32_t LintAnalyzer::GetEndOf(const NodeId Id) const {
	// ノードの終端バイト位置又は空のノードの原点の返戻
	return Id == NoNode ? 0 : Facts.Positions[Id].End;
}

/**
 * ノードの出力位置の取得関数
 * 計算量：O(1)
 * @param Id 対象のノード
 * @return 最終テキストの０始まりの行・列（空のノードは原点）
 */
StaticLint::OutputPoint LintAnalyzer::GetPointOf(const NodeId Id) const {
	// ノードの出力位置又は空のノードの原点の返戻
	return Id == NoNode ? StaticLint::OutputPoint{} : Facts.Positions[Id].Point;
}

/**
 * 定義の名前のノードの取得関数
 * 計算量：名前を取得する迄に辿るシグネチャのノード数 V に対し O(V)
 * @param Function 対象の関数
 * @return 名前のノード（無名なら空）
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
NodeId LintAnalyzer::GetFunctionNameOf(const NodeId Function) const {
	// 名前の場が有る場合の其のノードの返戻
	if(const NodeId Name = Syntax.GetFieldChild(Function, ChildRole::Name); Name != NoNode) return Name;
	NodeId Found = NoNode;
	WalkChildrenCursor(
		Function,
		[this, &Found](const NodeId Child) -> bool {
			// 名前を取得済又は本体に達した場合の探索打切の返戻
			if(Found != NoNode || Syntax.Is(Child, Role::FunctionBodyContainer)) return false;
			if(
				Syntax.Is(Child, Role::FunctionNameIdentifier) || Syntax.Is(Child, Role::OperatorName) ||
				Syntax.Is(Child, Role::UserDefinedOperator)
			) Found = Child;
			// 名前が見付からない場合の探索継続の返戻
			return Found == NoNode;
		}
	);
	// 探索した関数名のノードの返戻
	return Found;
}

/**
 * 所属する宣言子の後置戻値型の型部分の取得関数
 * 仮引数・初期値・本体へ探索を広げず，所有する宣言子のラッパーだけを辿る
 * 計算量：宣言子の深さ D と直接の子数 C の総和に対し O(D + C)
 * @param Declarator 後置戻値型を読む宣言子
 * @return 矢印と先行するコメントを除いた型部分（無ければ空）
 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
 * @throws std::out_of_range ノードが表に無い場合
 */
NodeId LintAnalyzer::GetTrailingReturnTypeOf(NodeId Declarator) const {
	while(Declarator != NoNode) {
		if(const NodeId Type = Syntax.GetFirstNamedChildWithRole(Declarator, Role::TrailingReturnType); Type != NoNode) {
			// 矢印と先行するコメントを除いた型部分の返戻
			for(const NodeId Part : Syntax.GetChildrenOf(Type)) if(Syntax.IsNamed(Part) && !Syntax.Is(Part, Role::Comment)) return Part;
			// 型部分を確認出来ない場合の返戻
			return NoNode;
		}
		Declarator = Flow.GetNextDeclarator(Declarator);
	}
	// 後置戻値型の無い事の返戻
	return NoNode;
}

/**
 * 仮引数又は束縛の型指定の有無の判定関数
 * 計算量：直接の子と分解の束縛の子数の総数 C に対し O(C)
 * @param Node 型指定を読む仮引数又は束縛
 * @return 共有の指定を含め型が明示されて居るか
 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
 * @throws std::out_of_range ノードが表に無い場合
 */
bool LintAnalyzer::HasExplicitTypeOf(const NodeId Node) const {
	// 型のフィールド・型注釈・コロンに続く型指定を持つ事の返戻
	if(
		Syntax.GetFieldChild(Node, ChildRole::Type) != NoNode ||
		Syntax.GetFirstNamedChildWithRole(Node, Role::TypeAnnotation) != NoNode ||
		GetTokenChildOf(Node, ":") != NoNode && Syntax.GetFirstNamedChildWithRole(Node, Role::PostParamResultType) != NoNode
	) return true;
	if(Syntax.Is(Node, Role::TypeHintBindingGroup)) {
		bool HasBinding = false;
		for(const NodeId Child : Syntax.GetChildrenOf(Node)) if(Syntax.Is(Child, Role::VarDecl)) {
			HasBinding = true;
			// 分解した束縛の型指定の不足の返戻
			if(!HasExplicitTypeOf(Child)) return false;
		}
		// 各束縛へ型を指定済かの返戻
		return HasBinding;
	}
	// 型指定の無い事の返戻
	return false;
}

/**
 * 後置戻値型を導く先頭の auto かの判定関数
 * 計算量：所有する宣言子の深さ D と直接の子数 C の総和に対し O(D + C)
 * @param Node 推論指定の型のノード
 * @return 全ての宣言子の後置戻値型を導く必須の指定か
 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
 * @throws std::out_of_range ノードが表に無い場合
 */
bool LintAnalyzer::HasRequiredAuto(const NodeId Node) const {
	const NodeId Owner = Syntax.GetParentOf(Node), OwnDeclarator = Syntax.GetFieldChild(Owner, ChildRole::Declarator);
	// decltype(auto) と，後置戻値型の中の推論指定は先頭のプレースホルダーと区別する
	if(
		Syntax.GetFieldChild(Owner, ChildRole::Type) != Node || Syntax.GetNamedChildCount(Node) != 1 ||
		!Syntax.Is(Syntax.GetNamedChild(Node, 0), Role::DeducedTypeKeyword) || OwnDeclarator == NoNode
	) return false;
	bool HasDeclarator = false;
	for(const NodeId Child : Syntax.GetChildrenOf(Owner)) {
		if(Child != OwnDeclarator && !Syntax.Is(Child, Role::DeclInitDeclarator)) continue;
		HasDeclarator = true;
		// 推論を要する宣言子を含む場合の返戻
		if(GetTrailingReturnTypeOf(Child) == NoNode) return false;
	}
	// 全ての宣言子に後置戻値型が在るかの返戻
	return HasDeclarator;
}

/**
 * 関数の明示された戻値型のノードの取得関数
 * 計算量：シグネチャ・所有する宣言子・結果のコンテナの子数の総数 C に対し O(C)
 * @param Function 戻値型を読む関数のノード
 * @return 戻値型のノード（明示無なら空）
 * @throws FormatLimitExceeded 型の探索でモデルの作業予算を超えた場合
 * @throws std::bad_alloc 収集中の読取記録の追加に失敗した場合
 * @throws std::out_of_range 子の識別子が構文表に無い場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
NodeId LintAnalyzer::GetExplicitReturnTypeOf(const NodeId Function) const {
	// 戻値型の場・型注釈・仮引数の後の結果型・後置の戻値型の順の探索
	NodeId Type = Syntax.GetFieldChild(Function, ChildRole::ReturnType);
	if(Type == NoNode) Type = Syntax.GetFirstNamedChildWithRole(Function, Role::TypeAnnotation);
	if(Type == NoNode) {
		bool HasParams = false;
		for(const NodeId Child : Syntax.GetChildrenOf(Function)) {
			HasParams |= Syntax.Is(Child, Role::ParamContainer);
			if(HasParams && Syntax.Is(Child, Role::PostParamResultType)) {
				Type = Child;
				break;
			}
		}
	}
	if(Type == NoNode) {
		const NodeId Declarator = Syntax.GetFieldChild(Function, ChildRole::Declarator);
		Type = GetTrailingReturnTypeOf(Declarator == NoNode ? Function : Declarator);
	}
	// 名前の前に戻値型を書く文法の型の場（推論の指定は明示された型でない）
	if(
		const NodeId Leading = Syntax.GetFieldChild(Function, ChildRole::Type);
		Type == NoNode && !Syntax.Is(Leading, Role::PlaceholderTypeSpecifier) && !Syntax.Is(Leading, Role::LambdaFunctionType)
	) Type = Leading;
	// クロージャのヘッダの矢印に続く明示型だけを取得する
	if(
		const NodeId Header = Syntax.GetFieldChild(Function, ChildRole::Type);
		Type == NoNode && Syntax.Is(Header, Role::LambdaFunctionType) && GetTokenChildOf(Header, "->") != NoNode
	) Type = Syntax.GetFirstNamedChildWithRole(Header, Role::PostParamResultType);
	// 関数の結果型の場だけを取得し，別の所有者の式を結果型と混同しない
	if(Type == NoNode && Syntax.Is(Function, Role::FunctionScope)) Type = Syntax.GetFieldChild(Function, ChildRole::Result);
	if(Syntax.Is(Type, Role::TypeAnnotation) && Syntax.GetNamedChildCount(Type)) Type = Syntax.GetNamedChild(Type, 0);
	// １つの結果宣言の型だけを取得し，複数名又は複数結果を真偽値１つと扱わない
	if(Syntax.Is(Type, Role::ParamContainer) && Syntax.GetNamedChildCount(Type) == 1) {
		if(const NodeId Entry = Syntax.GetNamedChild(Type, 0); Syntax.Is(Entry, Role::ParamDecl)) {
			if(
				const NodeId ValType = Syntax.GetFieldChild(Entry, ChildRole::Type), Name = Syntax.GetFieldChild(Entry, ChildRole::Name);
				ValType != NoNode && (Name == NoNode || Syntax.Is(Name, Role::Identifier)) &&
				Syntax.GetNamedChildCount(Entry) == (Name != NoNode) + 1U
			) Type = ValType;
		}
	}
	// 明示された戻値型のノードの返戻
	return Type;
}

/**
 * 親の直下に在る名前無トークンの取得関数
 * 計算量：調べる子の数 V と比較するテキストの総長 B に対し O(V + B)
 * @param Parent 対象の構文
 * @param Spelling トークンの綴り
 * @return 一致するトークンのノード（無ければ空）
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
NodeId LintAnalyzer::GetTokenChildOf(const NodeId Parent, const std::string_view Spelling) const {
	// 綴りが一致する名前無の子の返戻
	for(const NodeId Child : Syntax.GetChildrenOf(Parent)) if(!Syntax.IsNamed(Child) && Syntax.GetText(Child) == Spelling) {
		// 綴りが一致する子の返戻
		return Child;
	}
	// 一致する子が無い場合の返戻
	return NoNode;
}

/**
 * コメント本文の位置を最終テキストへコピーする関数
 * 計算量：初回は本文長 B に対し O(B)，本文行数 L と初めて照合する行の長さ S に対し平均 O(log(L + 1) + S²)
 * @param Idx コメントの記録の添字
 * @param Body 記法を除いた本文
 * @param Offset 本文内のバイト位置
 * @return 最終テキストの行と UTF-8 バイト列
 * @throws std::bad_alloc 本文の行インデックスのメモリを確保出来ない場合
 * @throws std::length_error 本文の行インデックスの要素数上限を超える場合
 */
StaticLint::OutputPoint LintAnalyzer::GetPointInComment(
	const size_t Idx,
	const std::string_view Body,
	const size_t Offset
) const {
	// 対象コメントの構文事実の取得
	const StaticLint::CommentFacts &Comment = Facts.Comments[Idx];
	// 本文の外を指す位置の場合の元の位置の返戻
	if(Offset >= Body.size()) return Comment.Range.Point;
	// 同じ本文の行頭を１度だけインデックス化する
	const std::pair<std::unordered_map<const char *, std::vector<CommentLine>>::iterator, bool> Entry =
	CommentLines.try_emplace(Body.data());
	std::vector<CommentLine> &Lines = Entry.first->second;
	if(Entry.second) {
		Lines.push_back({ 0, std::nullopt });
		for(size_t Pos = 0; Pos < Body.size(); ++Pos) if(Body[Pos] == '\n') Lines.push_back({ Pos + 1, std::nullopt });
	}
	const std::vector<CommentLine>::iterator Line = std::upper_bound(
		Lines.begin(),
		Lines.end(),
		Offset + 1,
		// 本文の位置に対する行頭の大小比較
		[](const size_t Pos, const CommentLine &Candidate) -> bool {
			// 対象位置より後の行頭かの返戻
			return Pos < Candidate.Begin;
		}
	) - 1;
	const size_t LineIdx = static_cast<size_t>(Line - Lines.begin()), SrcRow = Comment.Range.Point.Row + LineIdx;
	if(!Line->Match) {
		Line->Match = std::string_view::npos;
		if(SrcRow < Facts.LineStarts.size()) {
			const size_t SrcStart = LineIdx ? Facts.LineStarts[SrcRow] : Comment.Range.Start;
			if(SrcStart < Comment.Range.End) {
				const size_t SrcEnd = std::min(
					SrcRow + 1 < Facts.LineStarts.size() ? static_cast<size_t>(Facts.LineStarts[SrcRow + 1] - 1) : Src.size(),
					static_cast<size_t>(Comment.Range.End)
				);
				const size_t BodyEnd = Line + 1 == Lines.end() ? Body.size() : (Line + 1)->Begin - 1;
				Line->Match = std::string_view(Src).substr(SrcStart, SrcEnd - SrcStart).find(Body.substr(Line->Begin, BodyEnd - Line->Begin));
			}
		}
	}
	// テキストが一致しない場合の元の位置の返戻
	if(*Line->Match == std::string_view::npos) return Comment.Range.Point;
	// 本文内の位置に対応する最終テキストの位置の返戻
	return {
		static_cast<uint32_t>(SrcRow),
		static_cast<uint32_t>((LineIdx ? 0 : Comment.Range.Point.Column) + *Line->Match + Offset - Line->Begin)
	};
}

/**
 * 投影した構文の破損の判定関数
 * 計算量：部分木のノード数 N に対し O(N)
 * @param Id 対象のノード
 * @return 未修復の構文破損を含むか
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
bool LintAnalyzer::HasError(const NodeId Id) const {
	// 未修復の構文破損の有無の返戻
	return Id != NoNode && Syntax.GetNode(Id).Facts.Has(Core::ConstraintKind::ContainsError);
}

/**
 * 単一式の最外の括弧の取得関数
 * 計算量：探索するノード又はテキストの長さ N に対し O(N)
 * @param Id 内側の式
 * @return 同じ値を表す最外の括弧又は元の式
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
NodeId LintAnalyzer::GetOutermostGroup(NodeId Id) const {
	// 対象範囲の走査
	for(
		NodeId Parent = Syntax.GetParentOf(Id);
		Parent != NoNode && Syntax.Is(Parent, Role::GroupingParen) && Syntax.GetNamedChildCount(Parent) == 1 &&
		Syntax.GetChildCount(Parent) == 3;
		Parent = Syntax.GetParentOf(Id)
	) Id = Parent;
	// 値を変えずに包む最外の括弧の返戻
	return Id;
}

/**
 * ドキュメントコメントの装飾を除いた論理行の追加関数
 * 最終テキストを投影した記録（記法・役割・本文の分解済）の論理行を読む
 * 計算量：本文の長さ N に対し O(N)
 * @param Record 最終テキストを投影したコメントの記録
 * @param Lines 本文の論理行の追加先
 * @return 最初に追加した論理行が始まる記録内の位置（行内の装飾を含む）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::logic_error 呼出先で構文・所有関係の内部契約が成立しない場合
 */
size_t LintAnalyzer::AppendSrcLogicalLines(const CommentRecord &Record, std::vector<std::string> &Lines) const {
	// 分解済の本文の論理行の直接追加
	Core::DocCommentReader::AppendLogicalLines(Record, Lines);
	const std::string_view Text = Core::CommentRepository::GetOutputTextOf(Record);
	// 先頭の空行だけを除いた論理行の開始位置の返戻
	return Core::CommentRepository::IsBlockNotation(Core::CommentRepository::GetOutputNotationOf(Record)) ?
	std::min(Text.find_first_not_of('\n'), Text.size()) :
	0;
}
