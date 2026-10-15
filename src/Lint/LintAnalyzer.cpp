#include "LintAnalyzer.hpp"
#include "../Core/CommentFormat.hpp"
#include "../Core/ControlFlow.hpp"
#include "../Core/DocCommentReader.hpp"
#include "../Core/SignatureExtractor.hpp"
#include "../Profile/LintRules.hpp"
#include "../Util/CharClass.hpp"
#include "../Util/CommentNormalizer.hpp"
#include <algorithm>
#include <cctype>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using Core::ChildRole;
using Core::DocCommentReader;
using Core::DocItem;
using Core::DocItemKind;
using Core::DocStyle;
using Core::NodeId;
using Core::NoNode;
using Core::Role;
using Core::Word;
using StaticLint::FunctionFacts;
using StaticLint::NumericLiteralKind;
using StaticLint::OutputPoint;

/**
 * 投影済のトークン又はノードのソース範囲の参照取得関数
 * 計算量：O(1)
 * @param Node 対象ノード
 * @return トークンのテキスト，又はノード範囲のテキスト参照（不正範囲なら空）
 */
std::string_view LintAnalyzer::GetNodeView(const NodeId Node) {
	// トークンの判定に，修復前の診断位置から読んだ綴りを使わない事の返戻
	if(const std::string_view Lexeme = Syntax.GetLexemeOf(Node); !Lexeme.empty()) return Lexeme;
	// テキストを参照するバイト範囲の取得
	const uint32_t StartByte = GetBeginOf(Node), EndByte = GetEndOf(Node);
	// 範囲の壊れたノードの空の返戻
	if(StartByte >= EndByte || EndByte > Src.size()) return {};
	// ソース上の対象範囲の参照の返戻
	return std::string_view(Src.data() + StartByte, EndByte - StartByte);
}

/**
 * 共有の束縛事実に対応する参照数の収集関数
 * 計算量：ノード数 N と未取得の束縛事実の収集量 R に対し O(N + R)
 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
 * @throws std::bad_alloc 束縛事実のメモリを確保出来ない場合
 * @throws std::length_error 束縛事実の列の上限を超える場合
 * @throws std::out_of_range 構文の識別子が表に無い場合
 */
void LintAnalyzer::CollectBindingRefs() {
	for(const NodeId Node : Facts.Nodes) {
		if(!Syntax.IsNamed(Node) || !IsBindingName(Node)) continue;
		const Core::BindingFact Ref = Syntax.GetBindingFact(Node);
		if(Ref.Decl != NoNode && !Ref.IsDecl) ++BindingRefs[Ref.Decl];
	}
	// 共有の宣言識別子に対応した参照数の収集の終了
	return;
}

/**
 * ========== 共通判定 ==========
 * 導入名を表せる葉の判定関数
 * 計算量：O(1)
 * @param Name 名前を調べるノード
 * @return 識別子又はフィールドの名前なら true
 */
bool LintAnalyzer::IsBindingName(const NodeId Name) const {
	// 導入名を表せる葉かの返戻
	return Syntax.Is(Name, Role::IdentifierLeafLike) || Syntax.Is(Name, Role::FieldName) ||
	Name != NoNode && Syntax.GetNode(Name).Kind.Def && Syntax.GetNode(Name).Kind.Def->Binding == Core::BindingSyntax::Name;
}

/**
 * 括弧又は単独の式列のラッパーの内側の取得関数
 * @param Node 式
 * @return 最も内の被演算子
 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
 * @throws std::out_of_range ノードが表に無い場合
 */
NodeId LintAnalyzer::UnwrapGroup(NodeId Node) const {
	while(
		(
			Syntax.Is(Node, Role::AssignableWrapper) || Syntax.Is(Node, Role::CondParenWrap) || Syntax.Is(Node, Role::ExprList) || (
				Syntax.Is(Node, Role::GroupingParen) ||
				Syntax.Is(Node, Role::ReturnValList) && Core::ControlFlow::IsReturn(Syntax, Syntax.GetParentOf(Node))
			) && !Syntax.HasTokenChild(Node, ",")
		) && Syntax.GetNamedChildCount(Node) == 1
	) Node = Syntax.GetNamedChild(Node, 0);
	// 内側の式の返戻
	return Node;
}

/**
 * 整数リテラルと其の単項符号の判定関数
 * @param Node 境界の式
 * @return 単一の整数リテラルを括弧又は正負符号で包んだ形か
 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
 * @throws std::out_of_range ノードが表に無い場合
 */
bool LintAnalyzer::IsIntegerLiteral(NodeId Node) {
	Node = UnwrapGroup(Node);
	while(
		Syntax.Is(Node, Role::UnaryPrefixExpr) &&
		(Syntax.HasTokenChild(Node, Rules.Operators.Plus) || Syntax.HasTokenChild(Node, Rules.Operators.Minus))
	) Node = UnwrapGroup(Syntax.GetNamedChild(Node, 0));
	// 整数のトークンの返戻
	return Syntax.IsAny(Node, { Role::IntLiteralCandidate, Role::NumLiteral }) &&
	Facts.NumericLiterals[Node] == NumericLiteralKind::Integer;
}

/**
 * 数値の指数の符号・全桁と桁区切の読取関数
 * 指数は仮数の桁位置と相殺出来る上限で飽和させ，残りの全桁も読む
 * 計算量：指数のテキスト長 L に対し O(L)
 * @param Text 数値のトークン
 * @param Pos 指数記号の次の位置と読取後の位置
 * @param Exponent 符号付の指数の格納先
 * @return 数字を持つ指数であるか
 */
bool LintAnalyzer::IsNumericExponentReadSuccessful(const std::string_view Text, size_t &Pos, int64_t &Exponent) {
	const bool IsNegative = Pos < Text.size() && Text[Pos] == '-';
	if(Pos < Text.size() && (Text[Pos] == '+' || Text[Pos] == '-')) ++Pos;
	const int64_t Limit = (static_cast<int64_t>(Text.size()) << 2) + 4;
	bool HasDigit = false;
	Exponent = 0;
	for(; Pos < Text.size(); ++Pos) {
		const unsigned char Char = Text[Pos];
		if(Char == Rules.DigitSeparator && HasDigit) {
			// マクロのテキストにも適用する引用符の桁区切は数字の間だけに置く
			if(Char == '\'' && (Pos + 1 == Text.size() || !std::isdigit(static_cast<unsigned char>(Text[Pos + 1])))) return false;
			continue;
		}
		if(!std::isdigit(Char)) break;
		HasDigit = true;
		Exponent = std::min(Limit, Exponent * 10 + Char - '0');
	}
	if(IsNegative) Exponent = -Exponent;
	// 全桁を読んだ指数の成立の返戻
	return HasDigit;
}

/**
 * 数値リテラルのテキストの判定関数
 * C / C++ のマクロの置換列ではパーサーが数値のノードを作らない為，トークンの境界を読む
 * @param Text 単一のトークン
 * @return 数値のトークンだけから成るか
 */
bool LintAnalyzer::IsNumericText(const std::string_view Text) {
	// 空のテキストに数値のトークンが無い事の返戻
	if(Text.empty()) return false;
	size_t Pos = 0;
	const bool IsHex = Text.starts_with("0x") || Text.starts_with("0X");
	const bool IsBinary = Text.starts_with("0b") || Text.starts_with("0B");
	if(IsHex || IsBinary) Pos = 2;
	bool HasDigit = false, HasPoint = false, HasExponent = false;
	for(; Pos < Text.size(); ++Pos) {
		const unsigned char Char = Text[Pos];
		if(std::isdigit(Char) && (!IsBinary || Char == '0' || Char == '1') || IsHex && std::isxdigit(Char)) HasDigit = true;
		else if(Char == '\'' && HasDigit) continue;
		else if(Char == '.' && !IsBinary && !HasPoint) HasPoint = true;
		else break;
	}
	// 数字を持たないテキストの対象外の返戻
	if(!HasDigit) return false;
	if(Pos < Text.size() && (IsHex ? Text[Pos] == 'p' || Text[Pos] == 'P' : Text[Pos] == 'e' || Text[Pos] == 'E')) {
		HasExponent = true;
		++Pos;
		int64_t Exponent = 0;
		// 数字を持たない指数又は不正な桁区切の対象外の返戻
		if(!IsNumericExponentReadSuccessful(Text, Pos, Exponent)) return false;
	}
	// 指数の無い１６進小数の対象外の返戻
	if(IsHex && HasPoint && !HasExponent) return false;
	const std::string_view Suffix = Text.substr(Pos);
	// 数値の接尾辞だけを持つトークンの返戻
	return HasPoint || HasExponent ?
	Suffix.empty() || Suffix.size() == 1 && Suffix.find_first_not_of(LintRules::MacroFloatSuffixes) == std::string_view::npos :
	Suffix.empty() || std::ranges::find(LintRules::MacroIntegerSuffixes, Suffix) != std::end(LintRules::MacroIntegerSuffixes);
}

/**
 * 整数リテラルの値の取得関数
 * 桁区切 (`_` / `'`) と基数の接頭辞を解き，値の後の接尾辞を分ける
 * 計算量：テキストの長さ N に対し O(N)
 * @param Text 整数リテラルのテキスト
 * @param Suffix 値の後の接尾辞（`u8` / `L` 等）の格納先
 * @return 値（６４ビットに収まらなければ空）
 */
std::optional<uint64_t> LintAnalyzer::IntLiteralVal(const std::string_view Text, std::string_view &Suffix) {
	const NumericHead Head = ReadNumericPrefix(Text, Rules.HasLeadingZeroOctal);
	size_t Pos = Head.End;
	const uint32_t Base = Head.Base;
	// 桁区切と接尾辞を分けた整数値の読取
	uint64_t Val = 0;
	for(; Pos < Text.size(); ++Pos) {
		const unsigned char Char = Text[Pos];
		// 桁区切の読飛
		if(Char == '_' || Char == '\'') continue;
		const int Lower = std::tolower(Char);
		const uint32_t Digit = std::isdigit(Char) ?
		static_cast<uint32_t>(Char - '0') :
		Lower >= 'a' && Lower <= 'f' ? static_cast<uint32_t>(Lower + 10 - 'a') : Base;
		if(Digit >= Base) break;
		// ６４ビットに収まらない値の返戻
		if(Val > (UINT64_MAX - Digit) / Base) return std::nullopt;
		Val = Val * Base + Digit;
	}
	Suffix = Text.substr(Pos);
	// 値の返戻
	return Val;
}

/**
 * 指定した値の数値リテラルの判定関数
 * 数値型と到達値域を調べず，トークンに書かれた零又は１だけを読む
 * @param Node 数値のノード
 * @param Val 零又は１
 * @return 指定した値の数値リテラルか
 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
 * @throws std::out_of_range ノードが表に無い場合
 */
bool LintAnalyzer::IsNumericVal(const NodeId Node, const uint64_t Val) {
	// 数値リテラルでない式の対象外の返戻
	if(!Syntax.IsAny(Node, { Role::IntLiteralCandidate, Role::NumLiteral })) return false;
	const NumericLiteralKind Kind = Facts.NumericLiterals[Node];
	const bool IsImaginary = Kind == NumericLiteralKind::ImaginaryFloatingPoint || Kind == NumericLiteralKind::ImaginaryInteger;
	// 虚数の１は実数の単位値でない事の返戻（虚数の零だけは係数の零と一致する）
	if(IsImaginary && Val) return false;
	const std::string_view Text = GetNodeView(Node);
	if(Kind == NumericLiteralKind::Integer || Kind == NumericLiteralKind::ImaginaryInteger) {
		std::string_view Suffix;
		// 整数のテキストに書かれた値の返戻
		return IntLiteralVal(Text, Suffix) == Val;
	}
	// 浮動小数の係数を持たない値の対象外の返戻
	if(Kind != NumericLiteralKind::FloatingPoint && Kind != NumericLiteralKind::ImaginaryFloatingPoint) return false;
	const bool IsHex = Text.starts_with("0x") || Text.starts_with("0X");
	size_t Pos = IsHex ? 2 : 0;
	int64_t Fraction = 0, Trailing = 0, Exponent = 0;
	bool HasPoint = false, HasNonzero = false, IsPower = true;
	int Power = 0;
	for(; Pos < Text.size(); ++Pos) {
		const unsigned char Char = Text[Pos];
		if(Char == '.') {
			HasPoint = true;
			continue;
		}
		if(Char == '_' || Char == '\'') continue;
		if(!(IsHex ? std::isxdigit(Char) : std::isdigit(Char))) break;
		if(HasPoint) ++Fraction;
		if(Char != '0') {
			IsPower &= !HasNonzero && (Char == '1' || IsHex && (Char == '2' || Char == '4' || Char == '8'));
			Power = Char == '2' ? 1 : Char == '4' ? 2 : Char == '8' ? 3 : 0;
			HasNonzero = true;
			Trailing = 0;
		} else if(HasNonzero) ++Trailing;
	}
	if(Pos < Text.size() && (IsHex ? Text[Pos] == 'p' || Text[Pos] == 'P' : Text[Pos] == 'e' || Text[Pos] == 'E')) {
		++Pos;
		// 指数の全桁を読めない数値の対象外の返戻
		if(!IsNumericExponentReadSuccessful(Text, Pos, Exponent)) return false;
	}
	// 非零の桁を持たないテキストかの返戻
	if(!Val) return !HasNonzero;
	// １へ縮めれない仮数の対象外の返戻
	if(!HasNonzero || !IsPower) return false;
	// 仮数と指数が相殺して１に為るテキストの返戻
	return !(Exponent + (Trailing - Fraction) * (IsHex ? 4 : 1) + Power);
}

/**
 * ラッパーを除いた式の結果を使用しない文脈の判定関数
 * 計算量：括弧のラッパーの深さ D と文脈の照会費用 Q に対し O(D + Q)
 * @param Node 結果の使用を調べる式
 * @return 式文又は更新部で結果を使用しなければ true
 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
 * @throws std::out_of_range ノードが表に無い場合
 */
bool LintAnalyzer::HasUnusedVal(const NodeId Node) const {
	const NodeId Expr = GetOutermostGroup(Node);
	// 結果を使用しない文脈かの返戻
	return Syntax.IsValUnusedCtx(Expr, Syntax.GetParentOf(Expr));
}

/**
 * 変更不能な関数の文の並びの取得関数
 * 計算量：初回は関数と本体の直接の子の数 C に対し O(C)，再取得は期待値 O(1)
 * @param Function 関数・ラムダ又はアクセサ
 * @return 文を直下へ並べるノード（本体が無いか式本体なら空）
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
NodeId LintAnalyzer::GetFunctionStmtsOf(const NodeId Function) const {
	// 取得済の結果の返戻
	if(
		const std::unordered_map<NodeId, NodeId>::const_iterator Found = FunctionStmts.find(Function);
		Found != FunctionStmts.end()
	) return Found->second;
	const NodeId Stmts = Flow.GetStmtsOf(Function);
	FunctionStmts.emplace(Function, Stmts);
	// 確認した文の並びの返戻
	return Stmts;
}

/**
 * 名前無子トークンからの候補一致トークン検索関数
 * 計算量：直接の子数 C と比較するテキストの総長 L に対し O(C + L)
 * @param Node 走査対象ノード
 * @param Option1 １番目の候補トークン
 * @param Option2 ２番目の候補トークン
 * @return 一致トークン参照（見付からなければ空）
 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::string_view LintAnalyzer::FindUnnamedToken(
	const NodeId Node,
	const std::string_view Option1,
	const std::string_view Option2
) {
	// 検索結果の格納先準備
	std::string_view Result;
	Core::SyntaxModel::ForEachChildOf(
		Syntax,
		Node,
		[&](const NodeId Child) -> bool {
			// 名前無子の候補照合
			if(!Syntax.IsNamed(Child)) if(const std::string_view Token = GetNodeView(Child); Token == Option1 || Token == Option2) {
				Result = Token;
				// 一致発見時の走査打切（対象トークンは１ノードに高々１つ）の返戻
				return false;
			}
			// 未発見の為，走査継続の返戻
			return true;
		}
	);
	// 一致トークンの返戻
	return Result;
}

/**
 * 括弧と論理積だけを通る JSX の左辺の比較の判定関数
 * 計算量：未収集の祖先数 D に対し期待値 O(D)，同じ構文の全照会はノード数 N に対し O(N)
 * @param Node 非０比較
 * @return 描画式の論理積の左辺へ指定された経路で届くなら true
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
bool LintAnalyzer::IsJsxRenderingCond(NodeId Node) {
	struct Step {
		NodeId Node;
		bool IsLeft;
	};
	std::vector<Step> Path;
	bool IsRendering = false, HasLeftAnd = false;
	while(Node != NoNode) {
		if(
			const std::unordered_map<NodeId, std::pair<bool, bool>>::const_iterator Found = JsxPaths.find(Node);
			Found != JsxPaths.end()
		) {
			IsRendering = Found->second.first;
			HasLeftAnd = Found->second.second;
			break;
		}
		const NodeId Parent = Syntax.GetParentOf(Node);
		if(Syntax.Is(Parent, Role::EmbeddedExpr)) {
			const NodeId Host = Syntax.GetParentOf(Parent);
			IsRendering = Syntax.Is(Host, Role::TagElement) && !Syntax.Is(Host, Role::ExprTag);
			JsxPaths.emplace(Node, std::pair(IsRendering, false));
			break;
		}
		const bool IsGroup = Syntax.Is(Parent, Role::CondParenWrap);
		if(!IsGroup && (!Syntax.Is(Parent, Role::BinaryExpr) || !Syntax.HasTokenChild(Parent, Rules.Operators.LogicalAnd))) {
			JsxPaths.emplace(Node, std::pair(false, false));
			break;
		}
		Path.push_back({ Node, !IsGroup && Syntax.GetNamedChild(Parent, 0) == Node });
		Node = Parent;
	}
	for(std::vector<Step>::reverse_iterator It = Path.rbegin(); It != Path.rend(); ++It) {
		HasLeftAnd |= It->IsLeft;
		JsxPaths.emplace(It->Node, std::pair(IsRendering, HasLeftAnd));
	}
	// 描画の論理積の左辺へ指定された経路で届くかの返戻
	return IsRendering && HasLeftAnd;
}

/**
 * 共通の行頭表から位置を含む行の先頭を取得する関数
 * 計算量：最終テキストの行数 L に対し O(log(L + 1))
 * @param Pos 最終テキストのバイト位置
 * @return 位置を含む行の先頭のバイト位置
 */
uint32_t LintAnalyzer::GetLineStartOf(const uint32_t Pos) const {
	const std::span<const uint32_t>::iterator After = std::upper_bound(Facts.LineStarts.begin(), Facts.LineStarts.end(), Pos);
	// 行頭又は原点の返戻
	return After == Facts.LineStarts.begin() ? 0 : *(After - 1);
}

/**
 * 本体の文の始まる行の数の取得関数
 * 返戻コメントを省ける１〜２行の単純な関数を，行長制限に依る折返とコメントを数えず本体の必須改行で区切る文の行で判定する
 * 計算量：最初の照会は本体のノード数 V に対し O(V)，同じ関数の再照会は期待値 O(1)
 * @param Function 関数又はラムダ
 * @return 本体の中で文の始まる行の数（上限で打ち切る，文の並びを確認出来なければ空）
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::optional<size_t> LintAnalyzer::CountStmtLines(const NodeId Function) const {
	// 完成構文を変更しない為，同じ関数の短い本体の判定を共有する
	if(
		const std::unordered_map<NodeId, std::optional<size_t>>::const_iterator Hit = StmtLineCounts.find(Function);
		Hit != StmtLineCounts.end()
	) return Hit->second;
	static constexpr size_t Limit = 3; // 返戻コメントを省ける本体の上限の次の行
	// 関数本体の取得
	const NodeId Body = GetFunctionStmtsOf(Function);
	// 本体を確認出来ない場合は，短い関数の省略条件の根拠にしない事の返戻
	if(Body == NoNode) {
		StmtLineCounts.emplace(Function, std::nullopt);
		// 本体の無い関数の返戻
		return std::nullopt;
	}
	std::vector<uint32_t> Lines;
	// 文の並びを持つ本体と，其の中のブロックの文の始まる行の収集
	Core::SyntaxModel::HasStoppedTraversal(
		Syntax,
		Body,
		[&](const NodeId Node) -> Core::WalkStep {
			// 上限に達した事の打切の返戻
			if(Lines.size() >= Limit) return Core::WalkStep::Stop;
			if(
				const NodeId Parent = Syntax.GetParentOf(Node);
				Node != Body && Syntax.IsNamed(Node) && !Syntax.Is(Node, Role::Comment) && (
					Syntax.Is(Node, Role::ElseClause) || Syntax.Is(Node, Role::ElseIfClause) || Parent == Body || Syntax.Is(Parent, Role::Block) ||
					Syntax.Is(Parent, Role::CompoundBody) || Syntax.Is(Parent, Role::IndentedBody) || Syntax.Is(Parent, Role::InnerFunctionStmts) ||
					Syntax.Is(Parent, Role::LexicalBlock) || Syntax.Is(Parent, Role::StmtSeq)
				)
			) if(const uint32_t Line = GetPointOf(Node).Row; std::ranges::find(Lines, Line) == Lines.end()) Lines.push_back(Line);
			// 子孫の文へ降りる事の返戻
			return Core::WalkStep::Descend;
		}
	);
	// 文の始まる行の数の返戻
	return StmtLineCounts.emplace(Function, std::min(Lines.size(), Limit)).first->second;
}

/**
 * 比較の隣接する２項と演算子の前進走査関数
 * 計算量：直接の子数 C，検査の総費用 Q に対し O(C + Q)
 * @param Syntax 構文モデル
 * @param Node 比較の所有ノード
 * @param Inspect 左項・右項・間の演算子を受ける検査関数
 * @throws std::bad_alloc 検査又は呼出先のメモリの確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会又は検査が作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
template<class Function> void LintAnalyzer::VisitCmpPairs(
	const Core::SyntaxModel &Syntax,
	const NodeId Node,
	const Function &Inspect
) {
	// 隣接する被演算子と間の演算子の前進走査
	NodeId Prev = NoNode, Token = NoNode;
	const NodeId NamedOperator = Syntax.GetFieldChild(Node, ChildRole::OperatorField);
	const bool HasInfixName = Syntax.Is(Node, Role::NamedInfixCall);
	// 次の被演算子と其の直前の演算子の対応関数
	// 計算量：比較の検査費用 Q に対し O(Q)
	// @param Child 直接の子
	// @throws std::bad_alloc 検査又は呼出先のメモリの確保に失敗した場合
	// @throws FormatLimitExceeded 構文の照会又は検査が作業予算を超えた場合
	// @throws std::length_error 本文又は作業列の要素数上限を超える場合
	// @throws std::out_of_range 照会する添字又は識別子が表に無い場合
	const auto InspectChild = [&](const NodeId Child) -> void {
		// 被演算子と演算子の間のコメントの読飛
		if(Syntax.Is(Child, Role::Comment)) return;
		// 名前付の中置関数と利用者定義の演算子も２項の間の演算子として処理
		if(
			Child == NamedOperator || !Syntax.IsNamed(Child) || Syntax.Is(Child, Role::UserDefinedOperator) ||
			HasInfixName && Prev != NoNode && Token == NoNode
		) Token = Child;
		else {
			if(Prev != NoNode && Token != NoNode) Inspect(Prev, Child, Token);
			Prev = Child;
			Token = NoNode;
		}
		// 終了
		return;
	};
	Core::SyntaxModel::ForEachChildOf(Syntax, Node, InspectChild);
	// 終了
	return;
}

/**
 * アンカーの手前に続く独立したコメントの本文の収集関数
 * ドキュメントコメントの有無と完全性の双方が同じ並びを見る為，走査を此処だけで定める
 * 計算量：コメントの数 K と遡るコメントの数 M に対し O(log K + M)
 * @param Anchor アンカーのノード
 * @param DirectiveStarts 標準のディレクティブコメントの始まり（昇順）
 * @return 空白だけを挟んでアンカーの手前に続き，前のコードの行末コメントより後の，ディレクティブコメントを除いたコメントの本文（原文の順）
 * @throws std::bad_alloc 検査の事実・照会又は診断の確保に失敗した場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::vector<std::string_view> LintAnalyzer::LeadingCommentTexts(
	const NodeId Anchor,
	const std::span<const uint32_t> DirectiveStarts
) {
	// 先行するコメント本文の収集準備
	std::vector<std::string_view> Texts;
	const std::vector<StaticLint::CommentFacts> &Comments = Facts.Comments;
	// アンカーより前に始まるコメントを後ろから遡る（間に空白以外の字が在れば並びを終える）
	uint32_t Boundary = GetBeginOf(Anchor);
	for(
		size_t Idx = static_cast<size_t>(
			std::lower_bound(
				Comments.begin(),
				Comments.end(),
				Boundary,
				[](const StaticLint::CommentFacts &Comment, const uint32_t Pos) -> bool {
					// 始まりとの比較の返戻
					return Comment.Range.Start < Pos;
				}
			) - Comments.begin()
		);
		Idx--;
	) {
		const StaticLint::CommentFacts &Comment = Comments[Idx];
		const uint32_t Start = Comment.Range.Start, End = Comment.Range.End;
		if(End > Boundary || Src.find_first_not_of(" \t\r\n", End) < Boundary) break;
		Boundary = Start;
		// 前のコードの行末コメントで並びの終了
		if(Src.find_first_not_of(" \t", Start ? Src.find_last_of('\n', Start - 1) + 1 : 0) < Start) break;
		// ディレクティブコメントの読飛
		if(std::binary_search(DirectiveStarts.begin(), DirectiveStarts.end(), Start)) continue;
		Texts.push_back(std::string_view(Src).substr(Start, End - Start));
	}
	std::reverse(Texts.begin(), Texts.end());
	// 原文の順に並べたコメントの本文の返戻
	return Texts;
}

/**
 * 検証済の出力の検査データの取得関数（整形仕様 8.4）
 * 検証が１度だけ解析・投影した最終構文・コメント・診断位置を読み，関数のシグネチャと到達可能性の共通解析を求める（解析・投影し直さない）
 * 計算量：ノード数 N，コメント数 C，数値のテキストの総長 L，共通解析の総費用 F に対し O(N + C + L + F)
 * @param Output 静的解析専用の事実を確定済の最終構文とコメント・診断位置（返す位置表と本文の参照の寿命を所有する）
 * @return 検査データ
 * @throws std::bad_alloc 検査の事実・照会又は診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文・制御の照会が作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
StaticLint::OutputFacts LintAnalyzer::ReadFacts(const Core::ValidatedOutput &Output) {
	// 最終構文と診断位置のデータの取込
	StaticLint::OutputFacts Facts;
	const Core::SyntaxModel &Syntax = Output.FinalSyntax;
	const Core::SyntaxRules &Rules = Syntax.GetRules();
	const NodeId Root = Syntax.GetRoot();
	const Core::DiagnosticLocationMap &Locations = Output.Locations;
	const std::string_view Text = Locations.Text;
	const bool HasNumericChecks = Rules.HasZeroCmp || Rules.HasBoundaryCmp || Rules.ShouldPreferUnitUpdate;
	Facts.Controls.resize(Syntax.GetNodeCount());
	if(HasNumericChecks) Facts.NumericLiterals.resize(Syntax.GetNodeCount());
	Facts.Nodes.reserve(Syntax.GetNodeCount());
	Facts.Comments.reserve(Locations.Comments.size());
	Facts.FirstTokens.resize(Locations.LineStarts.size(), NoNode);
	Facts.Positions = Locations.Nodes;
	Facts.LineStarts = Locations.LineStarts;
	Facts.DirectiveStarts = Locations.DirectiveStarts;
	// 最終テキストのコメントの位置と記録の対応（逐語領域に残る物も含め原文順）
	for(size_t Idx = 0; Idx < Locations.Comments.size(); ++Idx) {
		const Core::OutputRange &Range = Locations.Comments[Idx];
		Facts.Comments.push_back({ Range, Idx });
	}
	Facts.Flow = std::make_shared<const Core::ControlFlow>(Syntax);
	const Core::ControlFlow &Flow = *Facts.Flow;
	// 展開前の属性の範囲の判定の控え（関数毎の返戻の補完の前提の判定で共有する）
	std::unordered_map<Core::NodeId, bool> AttributeProtection;
	// 先行順の親の経路から，条件への所属を子へ引き継ぐ
	struct FactFrame {
		NodeId Id; // 訪問中のノード
		bool IsPreprocCondition; // 前処理の条件又は名前の内部か
		bool HasLiteralText; // コメント又は逐語本文の内側を読む経路か
		NodeId Condition; // 此のノードが持つ前処理の条件
		NodeId Name; // 此のノードが持つ前処理の名前
		NodeId DocAttribute = NoNode; // 直前の連続する属性の内，最初のドキュメント属性
	};
	std::vector<FactFrame> Path;
	Core::SyntaxModel::HasStoppedTraversal(
		Syntax,
		Root,
		[&](const Core::NodeId Id) -> Core::WalkStep {
			const NodeId Parent = Syntax.GetParentOf(Id);
			while(!Path.empty() && Path.back().Id != Parent) Path.pop_back();
			const Core::KindDef *const Def = Syntax.GetNode(Id).Kind.Def;
			const auto HasRole = [Def](const Role Wanted) -> bool {
				// 同じノードの固定の役割を共有する判定の返戻
				return Def && Def->Roles.test(static_cast<size_t>(Wanted));
			};
			bool IsPreprocCondition = !Path.empty() && Path.back().IsPreprocCondition;
			if(!IsPreprocCondition && !Path.empty()) IsPreprocCondition = Id == Path.back().Condition || Id == Path.back().Name;
			if(IsPreprocCondition && HasRole(Role::IdentifierLeafLike)) {
				const Core::OutputRange &Range = Locations.Nodes[Id];
				std::string_view Name;
				const bool HasText = Range.Start < Range.End && Range.End <= Text.size();
				if(HasText) Name = Text.substr(Range.Start, Range.End - Range.Start);
				Facts.PreprocNames.insert(Name);
			}
			const bool IsDeclaration = HasRole(Role::DeclWithType) || HasRole(Role::CaptureInitializer) || HasRole(Role::ShortVarDecl);
			if(Id != Root && IsDeclaration) Facts.Declarations.push_back(Id);
			// 同じ走査で実トークンの行を記録し，逐語本文の除外を親から継承する
			const bool HasLiteralText =
			!Path.empty() && Path.back().HasLiteralText || HasRole(Role::Comment) || HasRole(Role::LiteralInterior);
			if(!HasLiteralText && !Syntax.GetChildCount(Id)) {
				const Core::OutputRange &Range = Locations.Nodes[Id];
				if(Range.Start < Range.End) {
					NodeId &First = Facts.FirstTokens[Range.Point.Row];
					if(First == NoNode || Range.Start < Locations.Nodes[First].Start) First = Id;
				}
			}
			StaticLint::ControlContext &Context = Facts.Controls[Id];
			if(Parent != NoNode) Context = Facts.Controls[Parent];
			if(Parent == NoNode || HasRole(Role::FlowBoundary)) {
				Context.Flow = Id;
				Context.Case = Context.Switch = Context.Loop = Context.BreakLoop = NoNode;
				Context.BreakDepth = 0;
				Context.LoopDepth = 0;
			}
			if(Context.Flow == Id || HasRole(Role::LambdaLiteral)) Context.Exit = Id;
			if(HasRole(Role::CaseClause)) Context.Case = Id;
			if(HasRole(Role::SwitchStmt)) Context.Switch = Id;
			const bool IsBreakTarget = HasRole(Role::NestedBreakTarget);
			if(IsBreakTarget) ++Context.BreakDepth;
			if(HasRole(Role::RepeatedControl)) {
				Context.Loop = Id;
				if(IsBreakTarget) Context.BreakLoop = Id;
				++Context.LoopDepth;
			}
			const bool IsPreprocBranch = Id != Root && HasRole(Role::PreprocBranch);
			Path.push_back(
				{
					Id,
					IsPreprocCondition,
					HasLiteralText,
					IsPreprocBranch ? Syntax.GetFieldChild(Id, ChildRole::Cond) : NoNode,
					IsPreprocBranch ? Syntax.GetFieldChild(Id, ChildRole::Name) : NoNode
				}
			);
			Facts.Nodes.push_back(Id);
			// 数値の分類は境界と値の各検査で共有し，トークンを読み直さない
			if(HasNumericChecks && (HasRole(Role::NumLiteral) || HasRole(Role::IntLiteralCandidate))) {
				Facts.NumericLiterals[Id] = Syntax.GetNode(Id).Facts.NumericLiteral;
			}
			// ラベル付の脱出を１度だけ集め，所属する部分木の範囲で照会する
			if(
				(HasRole(Role::LoopExitStmt) || HasRole(Role::KeywordJump)) && Syntax.GetNamedChildCount(Id) &&
				Syntax.Is(Syntax.GetNamedChild(Id, 0), Role::NameToken) && Core::SyntaxModel::HasChildMatching(
					Syntax,
					Id,
					[&](const NodeId Child) -> bool {
						return Syntax.Is(Child, Role::BreakKeyword) || Syntax.Is(Child, Role::ContinueKeyword);
					}
				)
			) {
				const Core::OutputRange &Range = Locations.Nodes[Syntax.GetNamedChild(Id, 0)];
				std::string_view Label;
				if(Range.Start < Range.End && Range.End <= Locations.Text.size()) {
					Label = std::string_view(Locations.Text).substr(Range.Start, Range.End - Range.Start);
				}
				if(Label.starts_with("'")) Label.remove_prefix(1);
				if(Label.ends_with(":") || Label.ends_with("@")) Label.remove_suffix(1);
				if(!Label.empty()) Facts.LabelExits[Label].push_back(Id);
			}
			// 名前付兄弟を前進する同じ走査で，先行する属性の要約を親へ残す
			NodeId LeadingDocAttribute = NoNode;
			if(!Rules.Notation.DocAttributeOpen.empty() && Syntax.IsNamed(Id) && Path.size() > 1) {
				FactFrame &Owner = Path[Path.size() - 2];
				if(HasRole(Role::OuterExpansionAttribute)) {
					if(Owner.DocAttribute == NoNode) {
						const std::string Attribute = Syntax.GetText(Id);
						std::string_view Inner = Attribute;
						if(Inner.starts_with(Rules.Notation.DocAttributeOpen)) {
							Inner.remove_prefix(Rules.Notation.DocAttributeOpen.size());
							while(!Inner.empty() && std::isspace(static_cast<unsigned char>(Inner.front()))) Inner.remove_prefix(1);
							const std::string_view DocName = Rules.GetSpelling(Word::DocAttributeName);
							if(Inner.starts_with(DocName)) {
								Inner.remove_prefix(DocName.size());
								while(!Inner.empty() && std::isspace(static_cast<unsigned char>(Inner.front()))) Inner.remove_prefix(1);
								if(Inner.starts_with('=')) Owner.DocAttribute = Id;
							}
						}
					}
				} else {
					LeadingDocAttribute = Owner.DocAttribute;
					Owner.DocAttribute = NoNode;
				}
			}
			// 関数のシグネチャと到達可能性の共通解析
			if(
				const bool IsDocumented = HasRole(Role::DocumentedDef), IsOrdinary = HasRole(Role::OrdinaryFunctionDef);
				IsDocumented || IsOrdinary
			) {
				StaticLint::FunctionFacts Function;
				if(IsDocumented && Rules.Notation.Doc != Core::DocStyle::Docstring) {
					const Core::DocumentationFact Documentation = Core::SignatureExtractor::GetDocumentationFact(Syntax, Id);
					Function.DocAnchor = Documentation.Anchor;
					Function.IsDocumentable = Documentation.IsDocumentable;
				}
				std::optional<Core::ReturnShape> Shape;
				if(IsDocumented) {
					Function.Signature = Core::SignatureExtractor::Extract(Syntax, Flow, Id);
					Shape = Function.Signature.Return;
					Function.HasDocAttribute = LeadingDocAttribute != NoNode;
					Function.DocAttribute = LeadingDocAttribute;
				}
				if(IsOrdinary) {
					const std::optional<Core::FunctionEnd> End = Flow.ClassifyFunction(Id, Shape);
					if(End && End->Kind != Core::ReturnKind::Never) {
						// 直下の返戻・送出は制御の転送として終端し，末尾の不足を作らない（整形仕様 5.2, 8.4）
						const bool HasDirectTerminal = !Syntax.Is(End->Last, Role::ExprStmt) && Syntax.Is(End->Last, Role::ReturnOrThrow);
						const bool HasSingleTerminal =
						HasDirectTerminal && Syntax.GetNamedChildCount(End->Stmts) == 1 && Syntax.GetNamedChild(End->Stmts, 0) == End->Last;
						if(!HasSingleTerminal) {
							const Core::GotoIdx &Gotos = Flow.CollectGotoTargets(End->Stmts);
							Function.UnreachableReturningStmts = Flow.GetReturningStmtsAfterTermination(End->Stmts, Gotos);
						}
						// 前提の判定は不足の候補だけに行い，到達不能・未確定の候補には生成しない
						if(!HasDirectTerminal && Flow.EndReach(Id, *End) == Core::EndReachability::Reachable) {
							const bool HasSingleImplicitResult = Flow.IsSingleImplicitReturnBody(Id, *End);
							const bool CanMakeExplicit = Flow.CanMakeImplicitReturnExplicit(Id, *End, HasSingleImplicitResult);
							Function.IsMissingReturn =
							!Flow.IsReturnCompletionUncertain(Id, *End, HasSingleImplicitResult, CanMakeExplicit, AttributeProtection);
						}
					}
				}
				Facts.Functions.emplace(Id, std::move(Function));
			}
			// 子の投影データも読む指定の返戻
			return Core::WalkStep::Descend;
		}
	);
	// 所属を確定済のドキュメントを，記録毎に１度だけ同じ定義へ対応付ける
	const std::vector<Core::NodeId> Targets = Core::DocCommentReader::GetDocDefinitionTargets(Syntax, Output.FinalComments);
	for(size_t Idx = 0; Idx < Targets.size(); ++Idx) {
		const std::unordered_map<Core::NodeId, StaticLint::FunctionFacts>::iterator Function = Facts.Functions.find(Targets[Idx]);
		if(Function != Facts.Functions.end()) Function->second.Docs.push_back(Idx);
	}
	// 検査データの返戻
	return Facts;
}

/**
 * 表示前の警告順序の比較演算子（整形仕様 8.4）
 * 計算量：比較する置換値数 P，文字列の比較総長 B に対し O(P + B)
 * @param Other 比較する警告
 * @return 位置・識別子・最終構文順・表記前の置換値の順で自身が先なら true
 */
bool LintWarning::operator<(const LintWarning &Other) const {
	// 異なる位置と識別子の前後の返戻
	if(
		const std::strong_ordering PosOrder = std::tie(Row, Column, Rule) <=> std::tie(Other.Row, Other.Column, Other.Rule);
		std::is_neq(PosOrder)
	) return std::is_lt(PosOrder);
	// 一覧外の警告と通知の検出順の保持の返戻
	if(Rule == LintRule::Other) return false;
	// 表示前の最終構文順と文字列・整数の置換値の順の返戻
	return std::tie(Target, Vals) < std::tie(Other.Target, Other.Vals);
}

/**
 * 出力位置の警告の追加関数
 * 計算量：診断のテキスト長 L に対し O(L)
 * @param Point 最終テキストの位置
 * @param Rule 警告一覧の識別子
 * @param Msg 警告の本文
 * @param Output 警告の格納先
 * @param Vals 表記前の置換値
 * @param Target 対象の最終構文順
 * @throws std::bad_alloc 警告又は置換値の確保に失敗した場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void LintAnalyzer::Push(
	const OutputPoint Point,
	const LintRule Rule,
	const std::string_view Msg,
	std::vector<LintWarning> &Output,
	const LintSortVals &Vals,
	const Core::NodeId Target
) {
	// 集成体は `emplace_back` の括弧初期化を受けれない為，波括弧で構築して追加
	Output.push_back(
		{ Point.Row, Point.Column, std::string(Msg), Rule, Target == NoNode ? UINT64_MAX : Syntax.GetOrderRange(Target).first, Vals }
	);
	// 終了
	return;
}

/**
 * 違反１件追加関数
 * 計算量：診断のテキスト長 L に対し O(L)
 * @param Node 違反位置のノード（行・列を取得する）
 * @param Rule 警告一覧の識別子
 * @param Msg 警告文
 * @param Output 警告格納先
 * @param Vals 表記前の置換値
 * @throws std::bad_alloc 警告又は置換値の確保に失敗した場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void LintAnalyzer::Push(
	const NodeId Node,
	const LintRule Rule,
	const std::string_view Msg,
	std::vector<LintWarning> &Output,
	const LintSortVals &Vals
) {
	// 警告を結び付けるソース位置の取得
	Push(GetPointOf(Node), Rule, Msg, Output, Vals, Node);
	// 終了
	return;
}

/**
 * 型を明示出来る仮引数の検査関数
 * 計算量：仮引数のラッパーの深さ D，子数の累計 C と名前の長さ L に対し O(D + C + L)
 * @param Node 対象の仮引数
 * @param Ordinal コメントと区切を除いた１始まりの序数
 * @param Output 警告の格納先
 * @throws std::bad_alloc 診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会が作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void LintAnalyzer::CheckParamType(const NodeId Node, const uint32_t Ordinal, std::vector<LintWarning> &Output) {
	// 型を明記済の仮引数の場合の終了
	if(HasExplicitTypeOf(Node)) return;
	NodeId Pattern = Node;
	while(Syntax.Is(Pattern, Role::ParamNameWrapper)) {
		NodeId Inner = Syntax.GetFieldChild(Pattern, ChildRole::Pattern);
		if(Inner == NoNode) Inner = Syntax.GetFieldChild(Pattern, ChildRole::Name);
		if(Inner == NoNode) for(const NodeId Child : Syntax.GetChildrenOf(Pattern)) {
			if(Syntax.IsAny(Child, { Role::DestructurePattern, Role::ParamName, Role::ParamNameWrapper, Role::TuplePattern })) {
				Inner = Child;
				break;
			}
		}
		if(Inner == NoNode) break;
		Pattern = Inner;
	}
	NodeId Name = NoNode;
	if(!Syntax.IsAny(Pattern, { Role::DestructurePattern, Role::TuplePattern, Role::TypeHintBindingGroup })) {
		Name = Syntax.Is(Pattern, Role::ParamName) ? Pattern : Syntax.GetFieldChild(Pattern, ChildRole::Name);
		if(Name == NoNode) Name = Syntax.GetFirstNamedChildWithRole(Pattern, Role::ParamName);
		if(Name == NoNode && Syntax.IsNamed(Pattern) && !Syntax.GetNamedChildCount(Pattern)) Name = Pattern;
	}
	Push(
		Name == NoNode ? Node : Name,
		LintRule::ParameterType,
		"Review: Add an explicit type to " +
		(Name == NoNode ? "parameter #" + std::to_string(Ordinal) : "parameter '" + std::string(GetNodeView(Name)) + "'"),
		Output,
		{ Name == NoNode ? LintSortVal(int64_t{ Ordinal }) : LintSortVal(std::string(GetNodeView(Name))) }
	);
	// 終了
	return;
}

/**
 * 数値の型・リテラル・加減算の符号の取得関数
 * 名前は同じ完成候補で収集した導入の要約を参照し，深い式は明示した作業列で読む
 * 計算量：未読の式のノード数 V とトークン本文の総長 B に対し期待値 O(V + B)
 * @param Node 調べる式
 * @return 確定した数値の根拠と負の値の可能性
 * @throws FormatLimitExceeded 構文の照会が作業予算を超えた場合
 * @throws std::bad_alloc 式の要約又は作業列を確保出来ない場合
 * @throws std::length_error 作業列の要素数上限を超える場合
 * @throws std::out_of_range ノードが現在の表に無い場合
 */
LintAnalyzer::NumericFact LintAnalyzer::GetNumericFact(const NodeId Node) {
	std::vector<std::pair<NodeId, bool>> Pending { { Node, false } };
	while(!Pending.empty()) {
		const auto [Part, IsExit] = Pending.back();
		Pending.pop_back();
		if(Part == NoNode || NumericExpressions.contains(Part)) continue;
		Syntax.Charge(1);
		if(!IsExit) {
			Pending.emplace_back(Part, true);
			if(Syntax.IsAny(Part, { Role::BinaryExpr, Role::CmpInfix, Role::GroupingParen, Role::PrefixUnary, Role::UnaryPrefixExpr })) {
				for(const NodeId Child : Syntax.GetChildrenOf(Part)) if(Syntax.IsNamed(Child)) Pending.emplace_back(Child, false);
			}
			continue;
		}
		NumericFact Fact;
		if(Syntax.IsAny(Part, { Role::IntLiteralCandidate, Role::NumLiteral })) {
			Fact.IsNumeric =
			Facts.NumericLiterals[Part] == NumericLiteralKind::Integer || Facts.NumericLiterals[Part] == NumericLiteralKind::FloatingPoint;
			const std::string_view Text = GetNodeView(Part);
			Fact.CanBeNegative = Text.starts_with('-') && !IsNumericVal(Part, 0);
			if(Facts.NumericLiterals[Part] == NumericLiteralKind::Integer) {
				std::string_view Suffix;
				Fact.LiteralMagnitude = IntLiteralVal(Text, Suffix);
				if(Fact.LiteralMagnitude) Fact.IsUnsigned = Suffix.find_first_of("uU") != std::string_view::npos;
			}
		} else if(Syntax.Is(Part, Role::BoolLiteral)) {
			Fact.IsNumeric = true;
			Fact.LiteralMagnitude = Syntax.IsWord(Part, Word::True) ? 1 : 0;
		} else if(IsBindingName(Part)) {
			const Core::BindingFact Binding = Syntax.GetBindingFact(Part);
			if(!Binding.IsUncertain) {
				if(
					const std::unordered_map<NodeId, NumericFact>::const_iterator Found = NumericBindings.find(Binding.Decl);
					Found != NumericBindings.end()
				) Fact = Found->second;
			}
		} else if(Syntax.Is(Part, Role::GroupingParen) && Syntax.GetNamedChildCount(Part) == 1) {
			Fact = NumericExpressions.at(Syntax.GetNamedChild(Part, 0));
		} else if(Syntax.IsAny(Part, { Role::PrefixUnary, Role::UnaryPrefixExpr }) && Syntax.GetNamedChildCount(Part) == 1) {
			const NodeId Operand = Syntax.GetNamedChild(Part, 0);
			Fact = NumericExpressions.at(Operand);
			if(Syntax.HasTokenChild(Part, Rules.Operators.Minus) && Fact.IsNumeric && !Fact.IsUnsigned) {
				Fact.CanBeNegative = Fact.LiteralMagnitude ?
				static_cast<bool>(*Fact.LiteralMagnitude) && !Fact.CanBeNegative :
				!IsNumericVal(UnwrapGroup(Operand), 0);
			} else if(!Syntax.HasTokenChild(Part, Rules.Operators.Plus)) Fact = {};
		} else if(Syntax.Is(Part, Role::BinaryExpr) || Syntax.Is(Part, Role::CmpInfix)) {
			bool IsComparison = false, IsMembership = false;
			for(const NodeId Child : Syntax.GetChildrenOf(Part)) {
				IsComparison |= Rules.CmpOperators.contains(Syntax.GetLexemeOf(Child));
				IsMembership |= Syntax.IsWord(Child, Word::In);
			}
			if(IsMembership) Fact.IsNumeric = true;
			else if(IsComparison && Syntax.GetNamedChildCount(Part) == 2) {
				Fact.IsNumeric = NumericExpressions.at(Syntax.GetNamedChild(Part, 0)).IsNumeric &&
				NumericExpressions.at(Syntax.GetNamedChild(Part, 1)).IsNumeric;
			} else if(Syntax.GetNamedChildCount(Part) == 2) {
				const NodeId Left = Syntax.GetNamedChild(Part, 0), Right = Syntax.GetNamedChild(Part, 1);
				const NumericFact Lhs = NumericExpressions.at(Left), Rhs = NumericExpressions.at(Right);
				const bool IsMinus = Syntax.HasTokenChild(Part, Rules.Operators.Minus);
				const bool IsPlus = Syntax.HasTokenChild(Part, Rules.Operators.Plus);
				if(Lhs.IsNumeric && Rhs.IsNumeric && (IsMinus || IsPlus)) {
					Fact.IsNumeric = true;
					Fact.IsUnsigned = Lhs.IsUnsigned || Rhs.IsUnsigned;
					Fact.CanBeNegative =
					!Fact.IsUnsigned && (Lhs.CanBeNegative || Rhs.CanBeNegative || IsMinus && !IsNumericVal(UnwrapGroup(Right), 0));
					if(IsMinus && !Fact.IsUnsigned && Lhs.LiteralMagnitude && Rhs.LiteralMagnitude && !Lhs.CanBeNegative && !Rhs.CanBeNegative) {
						Fact.CanBeNegative = *Lhs.LiteralMagnitude < *Rhs.LiteralMagnitude;
						Fact.LiteralMagnitude =
						Fact.CanBeNegative ? *Rhs.LiteralMagnitude - *Lhs.LiteralMagnitude : *Lhs.LiteralMagnitude - *Rhs.LiteralMagnitude;
					}
				}
			}
		}
		NumericExpressions.emplace(Part, Fact);
	}
	// 確定した式又は未知の式の要約の返戻
	return Node == NoNode ? NumericFact{} : NumericExpressions.at(Node);
}

/**
 * 数値の導入と書込の共通要約の収集関数
 * 型の綴り・初期値・代入対象の同一性を使い，コメントの本文を根拠にしない
 * 計算量：導入・書込・必要な式のノード数 V と型本文の総長 B に対し期待値 O(V + B)
 * @throws FormatLimitExceeded 構文の照会が作業予算を超えた場合
 * @throws std::bad_alloc 数値の要約又は作業列を確保出来ない場合
 * @throws std::length_error 作業列の要素数上限を超える場合
 * @throws std::out_of_range ノードが現在の表に無い場合
 */
void LintAnalyzer::CollectNumericBindings() {
	std::vector<std::pair<NodeId, NodeId>> Initializers;
	std::vector<NodeId> Writes;
	for(const NodeId Name : Facts.Nodes) {
		if(Syntax.IsAny(Name, { Role::AssignmentOperation, Role::PrefixableIncrement, Role::UnaryPreOrUpdate })) Writes.push_back(Name);
		if(!IsBindingName(Name)) continue;
		const Core::BindingFact Binding = Syntax.GetBindingFact(Name);
		if(Binding.IsUncertain || Binding.Decl != Name) continue;
		NodeId Type = NoNode, Val = NoNode;
		bool HasPointer = false;
		for(NodeId Owner = Syntax.GetParentOf(Name); Owner != NoNode; Owner = Syntax.GetParentOf(Owner)) {
			Syntax.Charge(1);
			HasPointer |= Syntax.Is(Owner, Role::PtrTypeDeclarator) || Syntax.Is(Owner, Role::FunctionScope);
			if(Val == NoNode) {
				Val = Syntax.GetFieldChild(Owner, Syntax.Is(Owner, Role::AssignmentOperation) ? ChildRole::Right : ChildRole::Val);
			}
			Type = Syntax.GetFieldChild(Owner, ChildRole::Type);
			if(
				Type != NoNode || Syntax.Is(Owner, Role::TypedDecl) || Syntax.Is(Owner, Role::Param) || Syntax.Is(Owner, Role::FunctionScope) ||
				Syntax.Is(Owner, Role::MemberOwner) || Syntax.Is(Owner, Role::NamespaceDef)
			) break;
			if(Syntax.Is(Owner, Role::AssignmentOperation)) {
				Val = Syntax.GetFieldChild(Owner, ChildRole::Right);
				break;
			}
		}
		NumericFact Fact;
		if(Type != NoNode) {
			Fact.HasExplicitType = Syntax.IsBuiltinArithmeticType(Type);
			Core::SyntaxModel::HasNodeMatching(
				Syntax,
				Type,
				[&](const NodeId Part) -> bool {
					Fact.IsUnsigned |= Syntax.Is(Part, Role::NonnegativeNumericType);
					Fact.IsNumeric |= Fact.IsUnsigned || Syntax.Is(Part, Role::SignedNumericType);
					// 型の全ての指定を読む事の返戻
					return false;
				}
			);
			Fact.IsBlocked =
			HasPointer || !Fact.IsNumeric || !(Syntax.IsBuiltinArithmeticType(Type) || Syntax.Is(Type, Role::SignedNumericType));
			NodeId TypeName = Type;
			while(!IsBindingName(TypeName) && Syntax.GetNamedChildCount(TypeName) == 1) TypeName = Syntax.GetNamedChild(TypeName, 0);
			if(!Syntax.IsBuiltinArithmeticType(Type) && IsBindingName(TypeName)) {
				const Core::BindingFact TypeBinding = Syntax.GetBindingFact(TypeName);
				Fact.IsBlocked |= TypeBinding.IsUncertain || TypeBinding.Decl != NoNode;
			}
			Fact.IsBlocked |= Syntax.GetNode(Type).Facts.Has(Core::ConstraintKind::DeclaratorMacro) ||
			Syntax.GetNode(Type).Facts.Has(Core::ConstraintKind::MacroBinding);
			Fact.IsNumeric &= !Fact.IsBlocked;
			Fact.CanBeNegative = Fact.IsNumeric && !Fact.IsUnsigned;
		}
		NumericBindings.emplace(Name, Fact);
		if(Val != NoNode) Initializers.emplace_back(Name, Val);
	}
	for(const auto [Name, Val] : Initializers) {
		NumericFact &Fact = NumericBindings.at(Name);
		if(Fact.IsBlocked) continue;
		const NumericFact Initial = GetNumericFact(Val);
		if(Fact.HasExplicitType) Fact.CanBeNegative = !Fact.IsUnsigned && (!Initial.IsNumeric || Initial.CanBeNegative);
		else Fact = Initial;
	}
	NumericExpressions.clear();
	for(const NodeId Node : Writes) {
		const bool IsAssignment = Syntax.Is(Node, Role::AssignmentOperation);
		const NodeId Name = UnwrapGroup(IsAssignment ? Syntax.GetFieldChild(Node, ChildRole::Left) : Syntax.GetNamedChild(Node, 0));
		if(!IsBindingName(Name)) continue;
		const Core::BindingFact Binding = Syntax.GetBindingFact(Name);
		const std::unordered_map<NodeId, NumericFact>::iterator Found = NumericBindings.find(Binding.Decl);
		if(Binding.IsUncertain || Found == NumericBindings.end() || Found->second.IsBlocked || Found->second.IsUnsigned) continue;
		NumericFact &Fact = Found->second;
		Fact.LiteralMagnitude.reset();
		if(!IsAssignment) {
			if(Syntax.HasTokenChild(Node, Rules.Operators.Decrement)) Fact.CanBeNegative |= Fact.IsNumeric;
			continue;
		}
		const NumericFact Rhs = GetNumericFact(Syntax.GetFieldChild(Node, ChildRole::Right));
		const bool IsSubtract = Syntax.HasTokenChild(Node, Rules.Operators.SubtractAssign);
		const bool IsAdd = Syntax.HasTokenChild(Node, Rules.Operators.AddAssign);
		if(IsSubtract || IsAdd) {
			Fact.CanBeNegative |= Fact.IsNumeric && (IsSubtract || Rhs.CanBeNegative || Fact.HasExplicitType && !Rhs.IsNumeric);
		} else if(Syntax.HasTokenChild(Node, Rules.Operators.Assignment)) {
			Fact.IsNumeric &= Rhs.IsNumeric || Fact.HasExplicitType;
			Fact.CanBeNegative |= Rhs.CanBeNegative || Fact.HasExplicitType && !Rhs.IsNumeric;
		} else Fact.IsNumeric = false;
	}
	NumericExpressions.clear();
	// 収集の終了
	return;
}

/**
 * 隣接する２項の０比較候補の検出関数
 * 計算量：数値のテキスト長 L，括弧の深さ D，構文の照会・診断生成の費用 Q に対し O(L + D + Q)
 * @param Node 比較の所有ノード
 * @param Left 左の被演算子
 * @param Right 右の被演算子
 * @param Token 此の２項の間の演算子
 * @param Output 警告格納先
 * @throws std::bad_alloc 検査の事実・照会又は診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void LintAnalyzer::CheckZeroCmpPair(
	const NodeId Node,
	const NodeId Left,
	const NodeId Right,
	const NodeId Token,
	std::vector<LintWarning> &Output
) {
	const NodeId Lhs = UnwrapGroup(Left), Rhs = UnwrapGroup(Right);
	const bool IsLeftZero = IsNumericVal(Lhs, 0), IsRightZero = IsNumericVal(Rhs, 0);
	// 数値の零との比較でない場合の終了
	if(!IsLeftZero && !IsRightZero) return;
	const std::string_view Operator = GetNodeView(Token);
	const bool IsEqual = Operator == Rules.Operators.Equal || Rules.HasStrictEquality && Operator == Rules.Operators.StrictEqual;
	const bool IsNotEqual =
	Operator == Rules.Operators.NotEqual || Rules.HasStrictEquality && Operator == Rules.Operators.StrictNotEqual;
	const bool IsPositive = IsRightZero && Operator == Rules.Operators.Greater || IsLeftZero && Operator == Rules.Operators.Less;
	if(IsPositive && Rules.HasPositiveZeroCmp && !HasNumericBindings) {
		CollectNumericBindings();
		HasNumericBindings = true;
	}
	const NumericFact PositiveValue =
	IsPositive && Rules.HasPositiveZeroCmp ? GetNumericFact(IsRightZero ? Lhs : Rhs) : NumericFact{};
	if((IsNotEqual || IsPositive) && IsJsxRenderingCond(Node)) {
		Push(Token, LintRule::JsxNonzeroCondition, "Review: Use `!!x` for nonzero in JSX condition", Output);
	} else if(
		IsEqual || Rules.HasZeroInequality && IsNotEqual ||
		Rules.HasPositiveZeroCmp && IsPositive && !(PositiveValue.IsNumeric && PositiveValue.CanBeNegative && !PositiveValue.IsUnsigned)
	) Push(Token, LintRule::ZeroComparison, "Review: Use the zero-test form required for this numeric type", Output);
	// 終了
	return;
}

/**
 * 隣接する２項の整数リテラル境界比較検出関数（全言語）
 * 整数のトークン又は指定された単位の調整の形だけを読む
 * 計算量：境界の直接の子数 C，整数リテラルの照会と診断生成の総費用 Q に対し O(C + Q)
 * @param Left 左の被演算子
 * @param Right 右の被演算子
 * @param Token 此の２項の間の演算子
 * @param Output 警告格納先
 * @throws std::bad_alloc 構文の照会又は診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void LintAnalyzer::CheckBoundaryCmpPair(
	const NodeId Left,
	const NodeId Right,
	const NodeId Token,
	std::vector<LintWarning> &Output
) {
	const std::string_view Operator = GetNodeView(Token);
	// 調整する閉じた境界でない場合の終了
	if(Operator != Rules.Operators.GreaterEqual && Operator != Rules.Operators.LessEqual) return;
	// 整数又は単位の調整を持つ境界の候補形の判定
	// 計算量：照会する子の総数 C と数値のテキスト長 L に対し O(C + L)
	// @param Expr 境界を成す式
	// @param IsLower 下限側の調整か
	// @return 整数リテラル又は指定の単位の調整を持つなら true
	// @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
	// @throws std::out_of_range ノードが表に無い場合
	const auto IsBoundary = [this](const NodeId Expr, const bool IsLower) -> bool {
		const NodeId Bound = UnwrapGroup(Expr);
		// 整数リテラルの境界の返戻
		if(IsIntegerLiteral(Bound)) return true;
		// 二項の演算でない境界の対象外の返戻
		if(!Syntax.Is(Bound, Role::BinaryExpr)) return false;
		bool HasAdjustment = false;
		// 単位を調整する演算子と右項の対応の検査（左項の値は不使用）
		// 計算量：単位のトークンの照会費用 Q に対し O(Q)
		// @param Right 演算子の直後の項
		// @param Token ２項の間の演算子
		// @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
		// @throws std::out_of_range ノードが表に無い場合
		const auto InspectAdjustment = [&](const NodeId, const NodeId Right, const NodeId Token) -> void {
			HasAdjustment |=
			GetNodeView(Token) == (IsLower ? Rules.Operators.Plus : Rules.Operators.Minus) && IsNumericVal(UnwrapGroup(Right), 1);
			// 終了
			return;
		};
		VisitCmpPairs(Syntax, Bound, InspectAdjustment);
		// 指定された単位の調整の形かの返戻
		return HasAdjustment;
	};
	if(IsBoundary(Right, Operator == Rules.Operators.GreaterEqual) || IsBoundary(Left, Operator == Rules.Operators.LessEqual)) {
		Push(Token, LintRule::ComparisonBoundary, "Review: Use a strict inequality with an equivalent adjusted boundary", Output);
	}
	// 終了
	return;
}

/**
 * ネストの深さの検査関数
 * 最終構文の実トークンを持つ行だけを連続範囲とし，継続行を含む生成済のインデントの段数を数える（整形仕様 8.4）
 * 計算量：最終テキストの長さ B，行数 L に対し O(B + L)，実トークンの行は共通の事実を読む
 * @param Output 警告格納先
 * @throws std::bad_alloc コード行のインデックス又は診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void LintAnalyzer::CheckNestingDepth(std::vector<LintWarning> &Output) {
	// 共通走査で求めた実トークンの行の参照
	const std::vector<NodeId> &FirstTokens = Facts.FirstTokens;
	const size_t Limit = Rules.NestingLimit;
	bool IsPrevDeep = false;
	for(size_t Row = 0; Row < FirstTokens.size(); ++Row) {
		const NodeId Token = FirstTokens[Row];
		// 空行・コメントだけの行は範囲の連続性を変えない
		if(Token == NoNode) continue;
		size_t Depth = 0;
		for(size_t Pos = Facts.LineStarts[Row]; Pos < Src.size() && Src[Pos] == '\t'; ++Pos) ++Depth;
		if(Depth > Limit && !IsPrevDeep) {
			Push(
				Token,
				LintRule::NestingDepth,
				"Violation: Nesting depth " + std::to_string(Depth) + " exceeds the limit of " + std::to_string(Limit),
				Output,
				{ static_cast<int64_t>(Depth), static_cast<int64_t>(Limit) }
			);
		}
		IsPrevDeep = Depth > Limit;
	}
	// 終了
	return;
}

/**
 * 修飾のラッパーとトークンの投影済の役割の取得関数
 * 計算量：単一の子を持つラッパーの深さ D に対し O(D)
 * @param Node 修飾又は其のトークン
 * @return 構造とトークンが持つ役割の集合
 * @throws FormatLimitExceeded 修飾の照会が作業予算を超える場合
 */
Core::RoleSet LintAnalyzer::GetModifierRoles(NodeId Node) const {
	Core::RoleSet Result;
	while(Node != NoNode) {
		Syntax.Charge(1);
		Result |= Syntax.GetNode(Node).Kind.Def->Roles;
		const std::span<const NodeId> Children = Syntax.GetChildrenOf(Node);
		// 名前の解決を要するアノテーションの対象へ役割を継がない
		if(Children.size() != 1 || Syntax.IsNamed(Children.front())) break;
		Node = Children.front();
	}
	// 投影済の役割の集合の返戻
	return Result;
}

/**
 * アクセス修飾子の集合に対応する順位の取得関数
 * @param Access 投影済の可視性の意味
 * @return 同じ意味の登録の順位，其の他は -1
 * @throws FormatLimitExceeded 登録の照会が作業予算を超える場合
 */
int LintAnalyzer::GetAccessRank(const Core::AccessMeaning &Access) {
	for(size_t Rank = 0; Rank < Rules.AccessMeanings.size(); ++Rank) {
		Syntax.Charge(1);
		if(Rules.AccessMeanings[Rank].Mask == Access.Mask && Rules.AccessMeanings[Rank].Length == Access.Length) {
			// 同じ可視性の順位の返戻
			return static_cast<int>(Rank);
		}
	}
	// 未登録の可視性の返戻
	return -1;
}

/**
 * メンバの見出の修飾子と可視性の取得関数
 * 計算量：未収集の見出のノード数 V とテキスト長 B に対し期待値 O(V + B)，再照会は期待値 O(1)
 * @param Node メンバのノード
 * @return 同じ完成構文で共有する見出の要約
 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
 * @throws std::bad_alloc 見出又は走査のメモリを確保出来ない場合
 * @throws std::length_error 見出又は走査列の要素数上限を超える場合
 * @throws std::out_of_range ノードが構文表に無い場合
 */
const LintAnalyzer::MemberHeader &LintAnalyzer::GetMemberHeader(const NodeId Node) {
	Syntax.Charge(1);
	if(const std::unordered_map<NodeId, MemberHeader>::const_iterator Hit = MemberHeaders.find(Node); Hit != MemberHeaders.end()) {
		// 共有済の見出の返戻
		return Hit->second;
	}
	size_t Limit = 0;
	for(const Core::AccessMeaning &Access : Rules.AccessMeanings) Limit = std::max(Limit, Access.Length);
	struct Frame {
		NodeId Node;
		bool IsEntered = false;
		size_t Next = 0;
		std::span<const NodeId> Children {};
		MemberHeader Header {};
	};
	std::vector<Frame> Path { { Node } };
	while(!Path.empty()) {
		Frame &Current = Path.back();
		if(!Current.IsEntered) {
			Current.IsEntered = true;
			const NodeId Part = Current.Node;
			// 見出の外の本体・初期値・仮引数の境界で打ち切る
			if(
				Syntax.IsAny(Part, { Role::FunctionBodyContainer, Role::MemberOrderBody, Role::TypeMemberBody }) ||
				Syntax.HasField(Part, ChildRole::Val) || Syntax.HasField(Part, ChildRole::Params)
			) {
				MemberHeaders.emplace(Part, MemberHeader());
				Path.pop_back();
				continue;
			}
			if(Syntax.Is(Part, Role::MemberPrivateName)) {
				Current.Header.HasAccess = !Rules.GetSpelling(Word::PrivateAccess).empty();
				Current.Header.AccessMeaning = Rules.PrivateAccessMeaning;
				Syntax.Charge(1);
				Current.Header.AccessRank = GetAccessRank(Current.Header.AccessMeaning);
				MemberHeaders.emplace(Part, std::move(Current.Header));
				Path.pop_back();
				continue;
			}
			if(Syntax.Is(Part, Role::MemberModifier) || Syntax.Is(Part, Role::AccessModifier) || !Syntax.IsNamed(Part)) {
				Current.Header.IsTypeMember = GetModifierRoles(Part).test(static_cast<size_t>(Role::MemberTypeOperation));
				Current.Header.HasProperty = Syntax.Is(Part, Role::MemberProperty);
				if(const int Rank = GetAccessRank(Syntax.GetNode(Part).Facts.Access); Rank != -1) {
					Current.Header.AccessMeaning = Syntax.GetNode(Part).Facts.Access;
					Current.Header.HasAccess = static_cast<bool>(Current.Header.AccessMeaning.Length);
					Current.Header.AccessRank = Rank;
					Current.Header.Specifier = Part;
					MemberHeaders.emplace(Part, std::move(Current.Header));
					Path.pop_back();
					continue;
				}
				// セッターだけの可視性等をメンバ全体の指定として読まない
				if(Syntax.Is(Part, Role::AccessModifier)) {
					MemberHeaders.emplace(Part, std::move(Current.Header));
					Path.pop_back();
					continue;
				}
			}
			Current.Children = Syntax.GetChildrenOf(Part);
		}
		if(Current.Next == Current.Children.size()) {
			MemberHeaders.emplace(Current.Node, std::move(Current.Header));
			Path.pop_back();
			continue;
		}
		const NodeId Child = Current.Children[Current.Next];
		const std::unordered_map<NodeId, MemberHeader>::const_iterator Hit = MemberHeaders.find(Child);
		if(Hit == MemberHeaders.end()) {
			Path.push_back({ Child });
			continue;
		}
		Syntax.Charge(1);
		Current.Header.IsTypeMember |= Hit->second.IsTypeMember;
		Current.Header.HasProperty |= Hit->second.HasProperty;
		// 修飾子を合成した時だけ順位を求め，単独の要約は其の順位を引き継ぐ
		if(Hit->second.HasAccess && Current.Header.AccessMeaning.Length < Limit + 1) {
			if(!Current.Header.HasAccess) {
				Current.Header.HasAccess = true;
				Current.Header.AccessMeaning = Hit->second.AccessMeaning;
				Current.Header.AccessRank = Hit->second.AccessRank;
			} else {
				Current.Header.AccessMeaning.Mask |= Hit->second.AccessMeaning.Mask;
				Current.Header.AccessMeaning.ExactRank = -1;
				Current.Header.AccessMeaning.Length =
				std::min(Limit + 1, Current.Header.AccessMeaning.Length + 1 + Hit->second.AccessMeaning.Length);
				Current.Header.AccessRank = GetAccessRank(Current.Header.AccessMeaning);
			}
		}
		if(Current.Header.Specifier == NoNode) Current.Header.Specifier = Hit->second.Specifier;
		++Current.Next;
	}
	// 共有した見出の返戻
	return MemberHeaders.at(Node);
}

/**
 * クラスメンバ宣言順違反検出関数
 * 計算量：メンバの見出と宣言子のノード数 V，テキストの総長 B，診断生成の総費用 Q に対し平均 O(V + B + Q)
 * @param Node 対象ノード
 * @param Output 警告格納先
 * @throws std::bad_alloc 検査の事実・照会又は診断の確保に失敗した場合
 * @throws FormatLimitExceeded メンバの見出又は受手の走査が作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void LintAnalyzer::CheckMemberDeclOrder(const NodeId Node, std::vector<LintWarning> &Output) {
	// メンバの順序を検査しない言語の場合の終了
	if(!Rules.ShouldOrderMembers) return;
	// 名前に最も近い派生宣言子が関数であるメンバの取得関数（関数ポインタのフィールドはメンバ関数でない）
	const auto MemberFunctionDeclaratorOf = [this](const NodeId FieldNode) -> NodeId {
		NodeId Found = NoNode;
		// 名前側の最後の派生宣言子で関数とポインタ・参照・配列のフィールドを区別する
		for(NodeId Part = Syntax.GetFieldChild(FieldNode, ChildRole::Declarator); Part != NoNode;) {
			if(Syntax.Is(Part, Role::FunctionDeclarator)) Found = Part;
			else if(
				Syntax.Is(Part, Role::PtrOrRefDeclarator) || Syntax.Is(Part, Role::PtrTypeDeclarator) || Syntax.Is(Part, Role::ArrayDeclarator)
			) Found = NoNode;
			Part = Syntax.Is(Part, Role::QualifiedIdentifier) ? Syntax.GetFieldChild(Part, ChildRole::Name) : Flow.GetNextDeclarator(Part);
		}
		// メンバ関数の宣言子又はフィールドを表す空のノードの返戻
		return Found;
	};
	NodeId Owner = Syntax.Is(Node, Role::MemberOwner) ? Node : Syntax.GetParentOf(Node);
	if(Syntax.Is(Owner, Role::EnumerationBody)) Owner = Syntax.GetParentOf(Owner);
	// 型の本体でない並びへメンバの詳細順位を適用しない場合の終了
	if(!Syntax.Is(Owner, Role::MemberOwner)) return;
	const NodeId OwnerName = Syntax.GetFirstNamedChildWithRole(Owner, Role::TypeNameNode);
	const std::string_view TypeName = OwnerName == NoNode ? std::string_view() : GetNodeView(OwnerName);
	int MemberAccessRank = -1;
	// 文法が詳細順位を定めるメンバの分類
	const auto MemberRank = [this, TypeName, Owner, &MemberAccessRank](NodeId Child, const NodeId Declarator) -> int {
		if(Rules.HasDetailedMemberOrder) {
			// フレンド宣言の順位の返戻
			if(Syntax.Is(Child, Role::MemberRelation)) return 1;
			if(
				Syntax.Is(Child, Role::MemberTypeRank) ||
				Syntax.Is(Child, Role::FieldDef) && Syntax.GetFieldChild(Child, ChildRole::Declarator) == NoNode &&
				Syntax.Is(Syntax.GetFieldChild(Child, ChildRole::Type), Role::MemberTypeRank)
			// 型宣言と，型宣言を兼ねるフィールドの順位の返戻
			) return 2;
			bool IsStatic = false;
			for(const NodeId Part : Syntax.GetChildrenOf(Child)) {
				if(Syntax.Is(Part, Role::MemberStorageSpecifier) && GetModifierRoles(Part).test(static_cast<size_t>(Role::ModifierStatic))) {
					IsStatic = true;
				}
			}
			if(Declarator != NoNode || Syntax.Is(Child, Role::MemberMethodRank)) {
				if(Declarator != NoNode) {
					// デストラクタの順位の返戻
					if(Syntax.GetFirstNamedChildWithRole(Declarator, Role::MemberDestructorName) != NoNode) return 10;
					// コンストラクタの順位の返戻
					if(!TypeName.empty() && GetNodeView(Syntax.GetNamedChild(Declarator, 0)) == TypeName) return 9;
					// 演算子の順位の返戻
					if(Syntax.GetFirstNamedChildWithRole(Declarator, Role::OperatorName) != NoNode) return 8;
				}
				// 変換演算子の順位の返戻
				if(Syntax.GetFirstNamedChildWithRole(Child, Role::OperatorCast) != NoNode) return 8;
				// 静的メソッドの順位の返戻
				if(IsStatic) return 5;
				// 仮想メソッドの順位の返戻
				if(
					Core::SyntaxModel::HasChildMatching(
						Syntax,
						Child,
						[this](const NodeId Part) -> bool {
							// 見出の無名の語に投影した仮想指定かの返戻
							return !Syntax.IsNamed(Part) && GetModifierRoles(Part).test(static_cast<size_t>(Role::ModifierVirtual));
						}
					) || Core::SyntaxModel::HasChildMatching(
						Syntax,
						Declarator,
						[this](const NodeId Specifier) -> bool {
							// 明示した仮想関数の指定かの返戻
							return GetModifierRoles(Specifier).test(static_cast<size_t>(Role::ExplicitVirtualModifier));
						}
					)
				) return 6;
				// 通常のメソッドの順位の返戻
				return 7;
			}
			// フィールドの静的指定に応じた順位の返戻
			if(Syntax.Is(Child, Role::FieldDef)) return IsStatic ? 3 : 4;
			// 分類出来ないメンバの順位の返戻
			return -1;
		}
		// 修飾とアノテーションは本体へ降りず，宣言の見出だけから型への所属を読む
		bool IsTypeMember = Syntax.Is(Child, Role::MemberTypeState) || Syntax.Is(Child, Role::MemberTypeOperation) ||
		Syntax.Is(Owner, Role::MemberTypeState) || Syntax.Is(Owner, Role::MemberTypeFields) && Syntax.Is(Child, Role::FieldDef);
		const MemberHeader &Header = GetMemberHeader(Child);
		IsTypeMember |= Header.IsTypeMember;
		const bool HasPropertyModifier = Header.HasProperty;
		MemberAccessRank = Header.AccessRank;
		if(Header.HasAccess && MemberAccessRank < 0) MemberAccessRank = static_cast<int>(Rules.AccessMeanings.size());
		while(Syntax.Is(Child, Role::MemberWrapper)) {
			NodeId Def = NoNode;
			for(const NodeId Part : Syntax.GetChildrenOf(Child)) if(Syntax.IsNamed(Part) && !Syntax.Is(Part, Role::MemberModifier)) {
				Def = Part;
			}
			if(Def == NoNode) break;
			Child = Def;
		}
		// 型の関係の順位の返戻
		if(
			Syntax.Is(Child, Role::MemberRelation) ||
			Syntax.Is(Child, Role::MemberEmbeddedField) && Syntax.GetFieldChild(Child, ChildRole::Name) == NoNode
		) return 1;
		// 型エイリアス・ネスト型の順位の返戻
		if(Syntax.Is(Child, Role::MemberTypeRank)) return 2;
		// 名前で宣言する実体の状態の順位の返戻
		if(Syntax.Is(Child, Role::MemberInstanceState)) return 4;
		// アノテーションで宣言するプロパティの順位の返戻
		if(HasPropertyModifier) return IsTypeMember ? 3 : 4;
		const bool IsMethod = Syntax.Is(Child, Role::MemberMethodRank) || Declarator != NoNode;
		const NodeId Name = IsMethod ? GetFunctionNameOf(Child) : NoNode;
		const std::string_view MethodName = Name == NoNode ? std::string_view() : GetNodeView(Name);
		// 生成・破棄を担う構文と，言語が固定するメンバ名の順位の返戻
		if(
			Syntax.Is(Child, Role::MemberConstructor) ||
			std::ranges::find(Rules.MemberConstructorNames, MethodName) != Rules.MemberConstructorNames.end()
		) return 7;
		// 破棄を担うメンバの順位の返戻
		if(
			Syntax.Is(Child, Role::MemberDestructor) ||
			std::ranges::find(Rules.MemberDestructorNames, MethodName) != Rules.MemberDestructorNames.end()
		) return 8;
		if(IsMethod) {
			if(Rules.HasAssociatedFunctions) {
				IsTypeMember = !Core::SyntaxModel::HasNodeMatching(
					Syntax,
					Syntax.GetFieldChild(Child, ChildRole::Params),
					[this](const NodeId Part) -> bool {
						// 実体を受ける仮引数かの返戻
						return Syntax.Is(Part, Role::MemberReceiver);
					}
				);
			}
			// 型に属する操作又は実体毎の操作の順位の返戻
			return IsTypeMember || Syntax.Is(Child, Role::MemberTypeOperation) ? 5 : 6;
		}
		if(Syntax.Is(Child, Role::AssignmentOperation)) {
			const NodeId Target = Syntax.GetFieldChild(Child, ChildRole::Left);
			// 代入で導入する型の定数・状態と実体の状態の順位の返戻
			return Syntax.Is(Child, Role::MemberTypeState) || Syntax.Is(Target, Role::MemberTypeState) ? 3 : 4;
		}
		// 型の定数・静的状態の順位の返戻
		if(Syntax.Is(Child, Role::MemberTypeState)) return 3;
		// フィールド・プロパティの型の状態（型に属する物）と実体毎の状態の順位の返戻
		if(Syntax.Is(Child, Role::FieldDef) || Syntax.Is(Child, Role::MemberFieldRank)) return IsTypeMember ? 3 : 4;
		// 分類出来ないメンバの順位の返戻
		return -1;
	};
	// 詳細順位の見出の明示指定を１度集める分類内順位
	const auto MemberPriority =
	[this, TypeName](const NodeId Child, const NodeId Declarator, const int Rank) -> std::pair<int, int> {
		// 詳細順位を定めない言語の返戻
		if(!Rules.HasDetailedMemberOrder) return { 0, 0 };
		Core::RoleSet Modifiers;
		for(const NodeId Header : { Child, Declarator }) {
			for(const NodeId Part : Syntax.GetChildrenOf(Header)) {
				if(
					!Syntax.IsNamed(Part) || Syntax.Is(Part, Role::TypeQualifier) || Syntax.Is(Part, Role::MemberStorageSpecifier) ||
					GetModifierRoles(Part).test(static_cast<size_t>(Role::ExplicitVirtualModifier))
				) Modifiers |= GetModifierRoles(Part);
			}
		}
		if(Rank == 3 || Rank == 4) {
			NodeId ClosestPtr = NoNode;
			bool IsRef = false;
			for(NodeId Part = Syntax.GetFieldChild(Child, ChildRole::Declarator); Part != NoNode;) {
				if(Syntax.Is(Part, Role::PtrDeclarator) || Syntax.Is(Part, Role::PtrTypeDeclarator)) ClosestPtr = Part;
				IsRef |= Syntax.Is(Part, Role::RefDeclarator);
				Part = Flow.GetNextDeclarator(Part);
			}
			if(ClosestPtr != NoNode || IsRef) {
				Modifiers.reset(static_cast<size_t>(Role::ImmutableTypeQualifier));
				Modifiers.reset(static_cast<size_t>(Role::VolatileTypeQualifier));
				if(!IsRef) for(const NodeId Part : Syntax.GetChildrenOf(ClosestPtr)) if(Syntax.Is(Part, Role::TypeQualifier)) {
					Modifiers |= GetModifierRoles(Part);
				}
			}
		}
		const bool IsConst = Core::SyntaxModel::HasChildMatching(
			Syntax,
			Declarator,
			[this](const NodeId Part) -> bool {
				// 受手の定数指定かの返戻
				return Syntax.Is(Part, Role::TypeQualifier) && GetModifierRoles(Part).test(static_cast<size_t>(Role::ImmutableTypeQualifier));
			}
		);
		const int ReceiverRank = Declarator != NoNode && !IsConst ? 1 : 0;
		// 言語が許さない指定のメンバを順位候補にしない事の返戻
		if(
			Rank == 4 && Modifiers.test(static_cast<size_t>(Role::InstanceStateInvalidModifier)) || Rank == 5 && (
				IsConst || Core::SyntaxModel::HasChildMatching(
					Syntax,
					Declarator,
					[this](const NodeId Part) -> bool {
						// 受手の揮発指定かの返戻
						return Syntax.Is(Part, Role::TypeQualifier) && GetModifierRoles(Part).test(static_cast<size_t>(Role::VolatileTypeQualifier));
					}
				)
			)
		) return { -1, -1 };
		// 型の分類を其の見出から決める
		switch(Rank) {
		case 2:
			{
				const NodeId Type = Syntax.Is(Child, Role::FieldDef) ? Syntax.GetFieldChild(Child, ChildRole::Type) : Child;
				// 本体の無い型の前方宣言の除外
				if(
					(Syntax.Is(Type, Role::ClassLike) || Syntax.Is(Type, Role::EnumSpecifier)) &&
					Syntax.GetFieldChild(Type, ChildRole::Body) == NoNode
				) return { -1, -1 };
				// 型エイリアスの後の列挙・構造体・クラスの優先順
				static constexpr Role TypeOrder[] { Role::MemberEnumKeyword, Role::MemberStructKeyword, Role::MemberClassKeyword };
				// 定義した型の詳細順位の返戻
				for(size_t Idx = 0; Idx < std::size(TypeOrder); ++Idx) if(
					Core::SyntaxModel::HasChildMatching(
						Syntax,
						Type,
						[this, Wanted = TypeOrder[Idx]](const NodeId Part) -> bool {
							// 無名の見出の語に投影した役割かの返戻
							return !Syntax.IsNamed(Part) && Syntax.Is(Part, Wanted);
						}
					)
				) return { static_cast<int>(Idx) + 1, 0 };
				// 型エイリアスの順位の返戻
				return { 0, 0 };
			}
		// 仮想関数の純粋指定・明示指定・上書指定を区別する
		case 6:
			{
				if(
					Syntax.HasTokenChild(Child, "=") && Core::SyntaxModel::HasChildMatching(
						Syntax,
						Child,
						[this](const NodeId Part) -> bool {
							// 純粋仮想関数を指定する０かの返戻
							return Syntax.Is(Part, Role::ZeroLiteral) && GetNodeView(Part) == "0";
						}
					)
				// 純粋仮想関数の順位の返戻
				) return { 0, ReceiverRank };
				// 明示した仮想関数又は上書する関数の順位の返戻
				return { Modifiers.test(static_cast<size_t>(Role::ModifierVirtual)) ? 1 : 2, ReceiverRank };
			}
		case 8:
		case 9:
			{
				const NodeId Params = Syntax.GetFieldChild(Declarator, ChildRole::Params);
				std::vector<NodeId> Args;
				for(const NodeId Param : Syntax.GetChildrenOf(Params)) if(Syntax.IsNamed(Param)) Args.push_back(Param);
				int TransferRank = 0;
				bool HasRequiredTail = false, HasRequiredArg = false;
				for(size_t Idx = 0; Idx < Args.size(); ++Idx) if(!Syntax.Is(Args[Idx], Role::DefaultParam)) {
					HasRequiredTail |= static_cast<bool>(Idx);
					HasRequiredArg = true;
				}
				if(!Args.empty() && !HasRequiredTail && GetNodeView(Syntax.GetFieldChild(Args.front(), ChildRole::Type)) == TypeName) {
					for(NodeId Part = Syntax.GetFieldChild(Args.front(), ChildRole::Declarator); Part != NoNode;) {
						if(Syntax.Is(Part, Role::RefDeclarator)) {
							TransferRank = Syntax.HasTokenChild(Part, "&&") ? 2 : 1;
							break;
						}
						const bool IsPtr = Syntax.Is(Part, Role::PtrDeclarator) || Syntax.Is(Part, Role::PtrTypeDeclarator);
						if(IsPtr || Syntax.Is(Part, Role::ArrayDeclarator)) break;
						Part = Flow.GetNextDeclarator(Part);
					}
				}
				// コンストラクタの値・コピー・ムーブ・既定の順位の返戻
				if(Rank == 9) return { TransferRank ? TransferRank : HasRequiredArg ? 0 : 3, 0 };
				const NodeId Name = Syntax.GetFieldChild(Declarator, ChildRole::Declarator);
				// コピー・ムーブ代入の順位の返戻
				if(TransferRank && Syntax.HasTokenChild(Name, "=")) return { TransferRank + 1, 0 };
				// 演算子の読取専用・変更系の順位の返戻
				return { IsConst ? 0 : 1, 0 };
			}
		default:
			break;
		}
		// 各分類で先に一致する明示指定の優先順（MSVC は case の中の静的な配列の初期化も飛越と扱う為，switch の前に置く）
		static constexpr Role StaticStateOrder[] {
			Role::ModifierConstantExpression,
			Role::ModifierConstantInitialization,
			Role::ImmutableTypeQualifier,
			Role::ModifierInline,
			Role::ModifierThreadLocal,
			Role::VolatileTypeQualifier
		};
		static constexpr Role InstanceStateOrder[] { Role::ImmutableTypeQualifier, Role::VolatileTypeQualifier, Role::ModifierMutable };
		static constexpr Role StaticOperationOrder[] {
			Role::ModifierConstantEvaluation,
			Role::ModifierConstantExpression,
			Role::ModifierInline
		};
		static constexpr Role InstanceOperationOrder[] {
			Role::ModifierConstantEvaluation,
			Role::ModifierConstantExpression,
			Role::ModifierInline,
			Role::VolatileTypeQualifier
		};
		std::span<const Role> Order;
		switch(Rank) {
		case 3:
			Order = StaticStateOrder;
			break;
		case 4:
			Order = InstanceStateOrder;
			break;
		case 5:
			Order = StaticOperationOrder;
			break;
		case 7:
			Order = InstanceOperationOrder;
			break;
		default:
			break;
		}
		for(size_t Pos = 0; Pos < Order.size(); ++Pos) if(Modifiers.test(static_cast<size_t>(Order[Pos]))) {
			// 明示指定の順位の返戻
			return { static_cast<int>(Pos), ReceiverRank };
		}
		// 指定の無い分類内順位の返戻
		return { static_cast<int>(Order.size()), ReceiverRank };
	};
	// 先行するネスト型の完全な定義だけを名前と分類順位で記録する
	std::unordered_map<std::string_view, int> CompleteTypes;
	const auto HasCompleteTypeDependency =
	[this, &CompleteTypes, &MemberFunctionDeclaratorOf](const NodeId Type, const int Priority) -> bool {
		const NodeId Body = Syntax.GetFieldChild(Type, ChildRole::Body);
		std::unordered_set<std::string_view> LocalTypes;
		for(const NodeId Member : Syntax.GetChildrenOf(Body)) {
			const NodeId MemberType = Syntax.Is(Member, Role::FieldDef) ? Syntax.GetFieldChild(Member, ChildRole::Type) : Member;
			if(Syntax.Is(MemberType, Role::MemberTypeRank)) {
				const NodeId Name = Syntax.GetFieldChild(MemberType, ChildRole::Name);
				if(Name != NoNode) LocalTypes.emplace(Syntax.GetCmpNameView(Name));
			}
		}
		for(const NodeId Member : Syntax.GetChildrenOf(Body)) {
			const NodeId MemberType = Syntax.GetFieldChild(Member, ChildRole::Type);
			if(
				!Syntax.Is(Member, Role::FieldDef) || MemberFunctionDeclaratorOf(Member) != NoNode || !Syntax.Is(MemberType, Role::TypeName) ||
				!Syntax.GetChildrenOf(MemberType).empty() || Core::SyntaxModel::HasChildMatching(
					Syntax,
					Member,
					[this](const NodeId Part) -> bool {
						// 完全型を要しない静的データメンバかの返戻
						return Syntax.Is(Part, Role::MemberStorageSpecifier) && GetModifierRoles(Part).test(static_cast<size_t>(Role::ModifierStatic));
					}
				)
			) continue;
			// 単純な型名と値又は配列の宣言子から確定する依存だけを認める
			const std::string_view Name = Syntax.GetCmpNameView(MemberType);
			const std::unordered_map<std::string_view, int>::const_iterator Found = CompleteTypes.find(Name);
			if(LocalTypes.contains(Name) || Found == CompleteTypes.end() || Found->second <= Priority) continue;
			NodeId Part = Syntax.GetFieldChild(Member, ChildRole::Declarator);
			while(Syntax.Is(Part, Role::ArrayDeclarator) || Syntax.Is(Part, Role::ParenthesizedDeclarator)) {
				Part = Flow.GetNextDeclarator(Part);
			}
			// ポインタ・参照・関数の宣言子や未確定の型へ例外を広げない事の返戻
			if(Syntax.Is(Part, Role::FieldName) && Syntax.GetChildrenOf(Part).empty()) return true;
		}
		// 完全型への依存を確定出来ない事の返戻
		return false;
	};
	// 同じ可視性内の役割・分類内の順位の検査
	const int ProjectedDefaultRank =
	GetAccessRank(Syntax.Is(Owner, Role::ImplicitPublicMembers) ? Rules.PublicAccessMeaning : Rules.ImplicitAccessMeaning);
	const size_t DefaultAccessRank =
	ProjectedDefaultRank < 0 ? Rules.AccessMeanings.size() : static_cast<size_t>(ProjectedDefaultRank);
	using OrderRank = std::tuple<int, int, int>;
	std::vector<OrderRank> LastRanks(Rules.AccessMeanings.size() + 1, OrderRank{ -1, -1, -1 });
	for(const NodeId Child : Syntax.GetChildrenOf(Node)) {
		if(!Syntax.IsNamed(Child)) continue;
		if(
			Syntax.Is(Child, Role::AccessSection) ||
			Syntax.Is(Child, Role::MemberVisibilitySection) && Syntax.GetNode(Child).Facts.Access.ExactRank > -1
		) {
			std::fill(LastRanks.begin(), LastRanks.end(), OrderRank{ -1, -1, -1 });
			CompleteTypes.clear();
			continue;
		}
		MemberAccessRank = -1;
		const NodeId Declarator = MemberFunctionDeclaratorOf(Child);
		const int Rank = MemberRank(Child, Declarator);
		if(Rank < 0) continue;
		const std::pair<int, int> Priority = MemberPriority(Child, Declarator, Rank);
		if(Priority.first < 0) continue;
		const OrderRank Current { Rank, Priority.first, Priority.second };
		const size_t Access = MemberAccessRank < 0 ? DefaultAccessRank : static_cast<size_t>(MemberAccessRank);
		OrderRank &Last = LastRanks[Access];
		const NodeId Type = Syntax.Is(Child, Role::FieldDef) ? Syntax.GetFieldChild(Child, ChildRole::Type) : Child;
		if(
			Current < Last &&
			!(Rank == 2 && std::get<0>(Last) == 2 && Rules.HasDetailedMemberOrder && HasCompleteTypeDependency(Type, Priority.first))
		) Push(Child, LintRule::MemberOrder, "Review: Reorder this member according to its role", Output);
		Last = std::max(Last, Current);
		if(Rank == 2 && Rules.HasDetailedMemberOrder && Syntax.GetFieldChild(Type, ChildRole::Body) != NoNode) {
			const NodeId Name = Syntax.GetFieldChild(Type, ChildRole::Name);
			if(Name != NoNode) CompleteTypes.emplace(Syntax.GetCmpNameView(Name), Priority.first);
		}
	}
	// 終了
	return;
}

/**
 * 関数本体終端と到達不能 return 検証関数
 * 末尾の返戻の必要性又は到達不能を確定した物だけを報告する
 * 計算量：到達不能な返戻の数 R，到達可能性の解析・診断生成の費用 Q に対し O(R + Q)
 * @param Node 対象ノード
 * @param Output 警告格納先
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void LintAnalyzer::CheckMissingExplicitReturn(const NodeId Node, std::vector<LintWarning> &Output) {
	// 到達不能な返戻と関数末尾の不足の報告
	const FunctionFacts &Function = Facts.Functions.at(Node);
	for(const NodeId Stmt : Function.UnreachableReturningStmts) {
		const NodeId Return = Syntax.GetFirstNamedChildWithRole(Stmt, Role::ReturnStmt);
		Push(
			Return == NoNode ? Stmt : Return,
			LintRule::UnreachableReturn,
			"Violation: Remove the unreachable trailing `return`",
			Output
		);
	}
	const NodeId Name = GetFunctionNameOf(Node);
	if(Function.IsMissingReturn) {
		Push(
			Name == NoNode ? Node : Name,
			LintRule::MissingReturn,
			"Violation: Function is missing an explicit trailing return",
			Output
		);
	}
	// 終了
	return;
}

/**
 * 型を明示出来る宣言子の検査関数
 * 計算量：宣言と型の直接の子数 C，診断のテキスト長 L に対し O(C + L)
 * @param Node 対象の宣言子
 * @param Output 警告の格納先
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void LintAnalyzer::CheckDeclType(const NodeId Node, std::vector<LintWarning> &Output) {
	// 後続の型指定が先行する束縛にも適用される宣言の検査
	if(Rules.HasBackwardBindingTypeAnnotation) {
		std::vector<NodeId> Pending;
		bool IsInitializer = false;
		for(const NodeId Child : Syntax.GetChildrenOf(Node)) {
			if(Syntax.Is(Child, Role::Pattern)) Pending.push_back(Child);
			else if(Syntax.Is(Child, Role::TypeAnnotation)) Pending.clear();
			else if(Syntax.Is(Child, Role::AssignmentToken)) IsInitializer = true;
			else if(IsInitializer && Syntax.IsNamed(Child) && !Syntax.Is(Child, Role::Comment)) {
				for(const NodeId Pattern : Pending) {
					const NodeId Name = Syntax.GetNamedChildCount(Pattern) == 1 ? Syntax.GetNamedChild(Pattern, 0) : Pattern;
					Push(Name, LintRule::DeclarationType, "Review: Add an explicit type to this declaration", Output);
				}
				Pending.clear();
				IsInitializer = false;
			}
		}
		for(const NodeId Pattern : Pending) {
			const NodeId Name = Syntax.GetNamedChildCount(Pattern) == 1 ? Syntax.GetNamedChild(Pattern, 0) : Pattern;
			Push(Name, LintRule::DeclarationType, "Review: Add an explicit type to this declaration", Output);
		}
		// 共有の型指定を持つ宣言の終了
		return;
	}
	// 型と式を省いた定数の項目は直前の指定を共有し，独立した型の位置として報告しない
	if(Syntax.Is(Node, Role::ConstantDeclNode)) {
		for(const NodeId Entry : Syntax.GetChildrenOf(Node)) {
			if(Syntax.Is(Entry, Role::ConstantSpec) && Syntax.GetFieldChild(Entry, ChildRole::Val) != NoNode && !HasExplicitTypeOf(Entry)) {
				Push(
					Syntax.GetFieldChild(Entry, ChildRole::Name),
					LintRule::DeclarationType,
					"Review: Add an explicit type to this declaration",
					Output
				);
			}
		}
		// 共有の型指定を持つ定数の終了
		return;
	}
	// 型を明記済又は同じ具体型を名前で記述出来ないクロージャ自身の宣言の終了
	if(HasExplicitTypeOf(Node) || Syntax.Is(UnwrapGroup(Syntax.GetFieldChild(Node, ChildRole::Val)), Role::ClosureExpr)) return;
	bool HasBindings = false;
	for(const NodeId Child : Syntax.GetChildrenOf(Node)) {
		if(Syntax.Is(Child, Role::VarDecl)) {
			HasBindings = true;
			if(!HasExplicitTypeOf(Child)) {
				Push(
					Syntax.GetFirstNamedChildWithRole(Child, Role::ParamName),
					LintRule::DeclarationType,
					"Review: Add an explicit type to this declaration",
					Output
				);
			}
		} else if(Syntax.Is(Child, Role::TypeHintBindingGroup)) {
			HasBindings = true;
			CheckDeclType(Child, Output);
		}
	}
	// 個別の束縛を検査した宣言の終了
	if(HasBindings) return;
	NodeId Name = Syntax.GetFieldChild(Node, ChildRole::Name);
	if(Name == NoNode) Name = Syntax.GetFieldChild(Node, ChildRole::Pattern);
	if(Name == NoNode) Name = Syntax.GetFieldChild(Node, ChildRole::Params);
	if(Syntax.Is(Node, Role::AssignmentExpr)) {
		Name = Syntax.GetFieldChild(Node, ChildRole::Left);
		// 型注釈を書けない分解・連鎖代入の終了
		if(
			!IsBindingName(Name) && !Syntax.Is(Name, Role::CallAttributeOrSubscript) && !Syntax.Is(Name, Role::SubscriptExprNode) ||
			Syntax.Is(Syntax.GetParentOf(Node), Role::AssignmentExpr) ||
			Syntax.Is(Syntax.GetFieldChild(Node, ChildRole::Right), Role::AssignmentExpr)
		) return;
	}
	if(Name != NoNode && !Syntax.IsNamed(Name)) Name = NoNode;
	if(Name == NoNode && Syntax.Is(Node, Role::ShortVarDecl)) Name = Syntax.GetFieldChild(Node, ChildRole::Left);
	if(Name == NoNode) for(const NodeId Child : Syntax.GetChildrenOf(Node)) {
		if(const NodeId Binding = Syntax.GetFieldChild(Child, ChildRole::Name); Binding != NoNode) {
			Name = Binding;
			break;
		}
	}
	Push(Name == NoNode ? Node : Name, LintRule::DeclarationType, "Review: Add an explicit type to this declaration", Output);
	// 終了
	return;
}

/**
 * 仮引数列の序数と型の明示の検査関数
 * 計算量：ラッパーの深さ D，仮引数列とラッパーの子数の累計 C，診断の長さ L に対し O(D + C + L)
 * @param Params 仮引数列
 * @param Output 警告の格納先
 * @throws std::bad_alloc 診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会が作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void LintAnalyzer::CheckParamTypes(const NodeId Params, std::vector<LintWarning> &Output) {
	uint32_t Ordinal = 0, PendingOrdinal = 0;
	NodeId PendingGroup = NoNode;
	bool HasTypeSeparator = false;
	for(const NodeId Param : Syntax.GetChildrenOf(Params)) {
		if(PendingGroup != NoNode && GetNodeView(Param) == ":") {
			HasTypeSeparator = true;
			continue;
		}
		if(PendingGroup != NoNode && HasTypeSeparator && Syntax.Is(Param, Role::PostParamResultType)) {
			PendingGroup = NoNode;
			HasTypeSeparator = false;
			continue;
		}
		if(GetNodeView(Param) == "," || Syntax.Is(Param, Role::OptionalParamType)) {
			if(PendingGroup != NoNode) CheckParamType(std::exchange(PendingGroup, NoNode), PendingOrdinal, Output);
			HasTypeSeparator = false;
		}
		bool IsParam = Syntax.Is(Param, Role::OptionalParamType) || Syntax.Is(Param, Role::ParamDecl);
		switch(Syntax.GetBindingSyntax(Param)) {
		case Core::BindingSyntax::ConstructorParam:
		case Core::BindingSyntax::DefaultPattern:
		case Core::BindingSyntax::Name:
		case Core::BindingSyntax::NamePattern:
		case Core::BindingSyntax::Param:
		case Core::BindingSyntax::Pattern:
		case Core::BindingSyntax::RestPattern:
			IsParam = true;
			break;
		default:
			break;
		}
		if(IsParam) ++Ordinal;
		if(Syntax.Is(Param, Role::TypeHintBindingGroup)) {
			PendingGroup = Param;
			PendingOrdinal = Ordinal;
		} else if(Syntax.Is(Param, Role::OptionalParamType)) CheckParamType(Param, Ordinal, Output);
	}
	if(PendingGroup != NoNode) CheckParamType(PendingGroup, PendingOrdinal, Output);
	// 終了
	return;
}

/**
 * ========== 規約検査 ==========
 * アクセス修飾子順序違反検出関数
 * 計算量：調べるメンバ・修飾子数 N と比較するテキストの総長 B に対し O(N + B)
 * @param Node 対象ノード
 * @param Output 警告格納先
 * @throws std::bad_alloc 順位のテキスト又は診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void LintAnalyzer::CheckAccessSpecifierOrder(const NodeId Node, std::vector<LintWarning> &Output) {
	// アクセス順序を定めない言語の場合の終了
	if(Rules.AccessOrder.empty()) return;
	const int DefaultRank =
	GetAccessRank(Syntax.Is(Node, Role::ImplicitPublicMembers) ? Rules.PublicAccessMeaning : Rules.ImplicitAccessMeaning);
	std::string Order;
	for(const std::string_view Access : Rules.AccessOrder) {
		if(!Order.empty()) Order += " → ";
		Order += Access;
	}
	int LastRank = -1;
	bool IsReported = false;
	// 直前の修飾子との順位の照合
	const auto TrackRank = [&](const NodeId Target, const int Rank) -> void {
		// 順位を持たない修飾子の場合の終了
		if(Rank < 0) return;
		if(Rank < LastRank && !IsReported) {
			Push(Target, LintRule::AccessOrder, "Review: Reorder access specifiers as " + Order, Output, { Order });
			IsReported = true;
		}
		LastRank = Rank;
		// 終了
		return;
	};
	Core::SyntaxModel::ForEachNamedChildOf(
		Syntax,
		Node,
		[&](const NodeId Body) -> bool {
			// 宣言の並びの取得
			const NodeId Members = Rules.HasModifierGroups && Syntax.Is(Body, Role::EnumerationBody) ?
			Syntax.GetFirstNamedChildWithRole(Body, Role::EnumBodyDecls) :
			Syntax.Is(Body, Role::AccessOrderedBody) || Rules.HasAccessSections && Syntax.Is(Body, Role::ClassBody) ? Body : NoNode;
			// メンバを持たない本体を通す事の返戻
			if(Members == NoNode) return true;
			LastRank = -1;
			IsReported = false;
			bool HasSection = false, IsReportedPrivate = false;
			Core::SyntaxModel::ForEachNamedChildOf(
				Syntax,
				Members,
				[&](const NodeId Member) -> bool {
					if(Rules.HasAccessSections) {
						if(Syntax.Is(Member, Role::AccessSection)) {
							HasSection = true;
							TrackRank(Member, Syntax.GetNode(Member).Facts.Access.ExactRank);
							// 後続のメンバの検査継続の返戻
							return true;
						}
						// 対象外のメンバ又は既存の区分を通す事の返戻
						if(!Syntax.Is(Member, Role::AccessOrderMember) || HasSection) return true;
						if(Rules.ShouldRequireExplicitPrivate && Syntax.Is(Node, Role::ImplicitPrivateClass) && !IsReportedPrivate) {
							Push(Member, LintRule::ExplicitAccess, "Violation: Make private: explicit", Output);
							IsReportedPrivate = true;
						}
						const Core::AccessMeaning &DefaultAccess =
						Syntax.Is(Node, Role::ImplicitPrivateClass) ? Rules.PrivateAccessMeaning : Rules.PublicAccessMeaning;
						TrackRank(Member, GetAccessRank(DefaultAccess));
						// 後続のメンバの検査継続の返戻
						return true;
					}
					// 対象外のメンバを通す事の返戻
					if(!Syntax.Is(Member, Role::AccessOrderMember)) return true;
					const MemberHeader &Header = GetMemberHeader(Member);
					if(Header.Specifier == NoNode && Rules.ShouldRequireExplicitInternal) {
						Push(Member, LintRule::ExplicitAccess, "Violation: Make internal explicit", Output);
					}
					TrackRank(Header.Specifier == NoNode ? Member : Header.Specifier, !Header.HasAccess ? DefaultRank : Header.AccessRank);
					// 後続のメンバの検査継続の返戻
					return true;
				}
			);
			// 後続の本体の検査継続の返戻
			return true;
		}
	);
	// 終了
	return;
}

/**
 * 重なるラベルを除いた文の取得関数
 * 計算量：未収集のラベル数 L に対し期待値 O(L)，同じ完成構文の全照会は O(N)
 * @param Node ラベル又は文
 * @return 最も内の文（本体の無いラベルなら其のラベル）
 * @throws FormatLimitExceeded 構文の照会が作業予算を超えた場合
 * @throws std::bad_alloc ラベルのインデックス又は経路の確保に失敗した場合
 * @throws std::length_error 経路の要素数の上限を超える場合
 */
LintAnalyzer::NodeId LintAnalyzer::GetLabelBody(NodeId Node) {
	std::vector<NodeId> Path;
	while(Syntax.Is(Node, Role::LabeledConstruct)) {
		if(const std::unordered_map<NodeId, NodeId>::const_iterator Hit = LabelBodies.find(Node); Hit != LabelBodies.end()) {
			Node = Hit->second;
			break;
		}
		const uint32_t Count = Syntax.GetNamedChildCount(Node);
		if(Count < 2) break;
		Path.push_back(Node);
		Node = Syntax.GetNamedChild(Node, Count - 1);
	}
	for(const NodeId Label : Path) LabelBodies.emplace(Label, Node);
	// 共通の最も内の文の返戻
	return Node;
}

/**
 * goto／ラベル付 break, continue 検出関数
 * 計算量：関数の初回はノード数 V に対し O(V)，ラベル付脱出は同名の参照数 R と報告数 Q に対し O(log(R + 1) + Q)
 * @param Node 対象ノード
 * @param Labels ラベルが名付けるループのインデックス（親を１度だけ走って纏めて記録する）
 * @param Output 警告格納先
 * @throws std::bad_alloc ラベルのインデックス・走査又は診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void LintAnalyzer::CheckGotoOrLabel(const NodeId Node, LabelTargetIdx &Labels, std::vector<LintWarning> &Output) {
	// ノードの型は `Run` の振分で確定済
	if(Syntax.Is(Node, Role::GotoStmt)) {
		const NodeId Name = Syntax.GetNamedChild(Node, 0);
		const NodeId Scope = Facts.Controls[Node].Flow;
		NodeId Target = NoNode;
		if(
			Syntax.Is(Node, Role::CaseLabelJump) && (Syntax.HasWordChild(Node, Word::Case) || Syntax.HasWordChild(Node, Word::Default))
		) {
			const NodeId Parent = Syntax.GetParentOf(Node);
			if(Parent != NoNode) Target = Facts.Controls[Parent].Switch;
		} else if(Name != NoNode) {
			// 同じ関数のラベルを１度だけ読み，重複名でも元の前進走査の選択を保つ
			const auto [Owner, IsNew] = GotoLabels.try_emplace(Scope);
			if(IsNew) {
				WalkChildrenCursor(
					Scope,
					[this, & Index = Owner->second](const NodeId Cur) -> bool {
						// 別の関数へ降りない事の返戻
						if(Syntax.Is(Cur, Role::FlowBoundary)) return false;
						if(Syntax.Is(Cur, Role::LabeledConstruct) && Syntax.GetNamedChildCount(Cur)) {
							const std::string_view Label = GetNodeView(Syntax.GetNamedChild(Cur, 0));
							const auto [Hit, IsFirst] = Index.emplace(Label, Cur);
							const NodeId Parent = Syntax.GetParentOf(Cur);
							if(
								!IsFirst && Syntax.GetOrderRange(Parent).first < Syntax.GetOrderRange(Hit->second).first &&
								Syntax.GetOrderRange(Hit->second).first < Syntax.GetOrderRange(Parent).second
							) Hit->second = Cur;
						}
						// 関数内の全てのラベルへ降りる事の返戻
						return true;
					}
				);
			}
			if(const NamedLabels::const_iterator Hit = Owner->second.find(GetNodeView(Name)); Hit != Owner->second.end()) {
				Target = Hit->second;
			}
		}
		// ２番目に内側の繰返の後へ進む場合だけ，複数の繰返を脱出する
		if(Target != NoNode) {
			const NodeId Inner = Facts.Controls[Node].Loop;
			const NodeId Parent = Inner == NoNode ? NoNode : Syntax.GetParentOf(Inner);
			const NodeId Outer = Parent == NoNode ? NoNode : Facts.Controls[Parent].Loop;
			// 複数の繰返を脱出するジャンプを警告しない事の終了
			if(Outer != NoNode && Syntax.GetOrderRange(Target).first >= Syntax.GetOrderRange(Outer).second) return;
			if(Outer != NoNode && Syntax.Is(Target, Role::LabeledConstruct)) {
				const NodeId Body = Syntax.GetFieldChild(Outer, ChildRole::Body), Tail = GetLabelBody(Target);
				// 本体の末尾の空文を名付けるラベルは，外側のループの継続先と一致する
				if(
					Syntax.GetLastStmt(Body) == Target && (
						Syntax.Is(Tail, Role::EmptyStmt) || Syntax.Is(Tail, Role::ExprStmt) && !Syntax.GetNamedChildCount(Tail) &&
						Syntax.IsTokenRole(Syntax.GetFirstEdge(Tail).Leaf, Role::StmtTerminatorToken)
					)
				// 実際の継続先へ移るジャンプの非発報の返戻
				) return;
			}
		}
		Push(Node, LintRule::JumpUsage, "Review: Replace this jump with structured control flow", Output);
	} else {
		// 定義でない脱出の参照を除く場合の終了
		if(Syntax.Is(Syntax.GetParentOf(Node), Role::JumpWithLabel)) return;
		std::string_view Label = Syntax.Is(Node, Role::LabeledStmt) && Syntax.GetNamedChildCount(Node) ?
		GetNodeView(Syntax.GetNamedChild(Node, 0)) :
		GetNodeView(Node);
		if(Label.starts_with("'")) Label.remove_prefix(1);
		if(Label.ends_with(":") || Label.ends_with("@")) Label.remove_suffix(1);
		// ラベルを持たない場合の終了
		if(Label.empty()) return;
		// ラベル対象がループかの判定関数
		const auto IsLoopNode = [this](const NodeId Cur) -> bool {
			// 繰返かの返戻
			return Syntax.Is(Cur, Role::LabeledLoop);
		};
		// 言語別配置に応じたラベル対象ループの位置解決
		NodeId Target = NoNode;
		Core::SyntaxModel::ForEachNamedChildOf(
			Syntax,
			Node,
			[this, &Target, &IsLoopNode](const NodeId Child) -> bool {
				// 重ねたラベル (`a: b: for`) は内側のラベルの後の文を名付ける為，ラベルを辿って其の文で見る
				const NodeId Inner = GetLabelBody(Child);
				if(IsLoopNode(Inner)) Target = Inner;
				// ループを包むラベル (Java / JavaScript / TypeScript / Go / C#) を見付ける迄の走査継続の返戻
				return Target == NoNode;
			}
		);
		// 文を包むラベルの子に限定した対象解決
		const bool IsWrapping = Syntax.Is(Node, Role::LabeledStmt);
		if(IsWrapping && Target == NoNode) Target = Node;
		// ラベルが脱出先の子に在る形（Rust の `'outer: for`）
		if(!IsWrapping && Target == NoNode) {
			if(const NodeId Parent = Syntax.GetParentOf(Node); IsLoopNode(Parent) || Syntax.Is(Parent, Role::LabeledBlockExit)) {
				Target = Parent;
			}
		}
		// ラベルが対象の文の前の兄弟に在る形
		if(!IsWrapping && Target == NoNode) {
			if(Labels.find(Node) == Labels.end()) {
				NodeId Pending = NoNode;
				Core::SyntaxModel::ForEachNamedChildOf(
					Syntax,
					Syntax.GetParentOf(Node),
					// ラベルと直後の文の対応付関数
					[this, &Labels, &Pending](const NodeId Child) -> void {
						if(Pending != NoNode) {
							// ラベル名別のノードインデックスへの登録
							Labels.emplace(Pending, Child);
							Pending = NoNode;
						}
						if(Syntax.Is(Child, Role::LabelInspection)) Pending = Child;
					}
				);
				if(Pending != NoNode) Labels.emplace(Pending, NoNode);
			}
			if(const LabelTargetIdx::const_iterator Hit = Labels.find(Node); Hit != Labels.end()) Target = Hit->second;
		}
		// 同じラベルへ向かう脱出の内，対象の部分木と同じ関数に在る物だけ読む
		const NodeId Root = Target == NoNode ? Node : Target;
		if(
			const std::unordered_map<std::string_view, std::vector<NodeId>>::const_iterator Hit = Facts.LabelExits.find(Label);
			Hit != Facts.LabelExits.end()
		) {
			const std::vector<NodeId> &Exits = Hit->second;
			const StaticLint::ControlContext &Context = Facts.Controls[Root];
			const uint64_t End = Syntax.GetOrderRange(Root).second;
			uint64_t SkipEnd = Syntax.GetOrderRange(Root).first + 1;
			const std::vector<NodeId>::const_iterator First = std::lower_bound(
				Exits.begin(),
				Exits.end(),
				SkipEnd,
				[this](const NodeId Id, const uint64_t Order) -> bool {
					// 対象の部分木の先頭より前かの返戻
					return Syntax.GetOrderRange(Id).first < Order;
				}
			);
			for(std::vector<NodeId>::const_iterator Exit = First; Exit != Exits.end() && Syntax.GetOrderRange(*Exit).first < End; ++Exit) {
				const NodeId Cur = *Exit;
				if(Syntax.GetOrderRange(Cur).first < SkipEnd || Facts.Controls[Cur].Flow != Context.Flow) continue;
				SkipEnd = Syntax.GetOrderRange(Cur).second;
				const NodeId Parent = Syntax.GetParentOf(Root);
				const size_t OuterDepth = Parent == NoNode ? 0 : Facts.Controls[Parent].LoopDepth;
				// ラムダを跨がず複数の繰返を脱出する場合は，従来通り除外する
				if(Target != NoNode && Facts.Controls[Cur].Exit == Context.Exit && Facts.Controls[Cur].LoopDepth > OuterDepth + 1) continue;
				Push(Cur, LintRule::JumpUsage, "Review: Replace this jump with structured control flow", Output);
			}
		}
	}
	// 終了
	return;
}

/**
 * 段数を指定した脱出の検査関数
 * 計算量：引数のテキスト長 L と診断生成の費用 Q に対し O(L + Q)
 * @param Node 段数を指定出来る脱出文
 * @param Output 警告の格納先
 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
 * @throws std::bad_alloc 診断のメモリを確保出来ない場合
 * @throws std::length_error 診断のテキストの上限を超える場合
 * @throws std::out_of_range 照会するノードが表に無い場合
 */
void LintAnalyzer::CheckNumericJump(const NodeId Node, std::vector<LintWarning> &Output) {
	const NodeId Arg = Syntax.GetNamedChild(Node, 0);
	// 段数を指定しない普通の脱出の終了
	if(Arg == NoNode) return;
	std::string_view Suffix;
	const std::optional<uint64_t> Depth = IntLiteralVal(GetNodeView(Arg), Suffix);
	const bool IsBreak = Syntax.HasTokenRoleChild(Node, Role::BreakKeyword);
	if(Depth && *Depth && Suffix.empty()) {
		const StaticLint::ControlContext &Context = Facts.Controls[Node];
		const bool DoesCountSwitch = IsBreak || Rules.DoesContinueCountSwitch;
		const size_t Available = DoesCountSwitch ? Context.BreakDepth : Context.LoopDepth;
		const NodeId Inner = DoesCountSwitch ? Context.BreakLoop : Context.Loop;
		const NodeId Parent = Inner == NoNode ? NoNode : Syntax.GetParentOf(Inner);
		NodeId Outer = NoNode;
		if(Parent != NoNode) {
			const StaticLint::ControlContext &Owner = Facts.Controls[Parent];
			Outer = DoesCountSwitch ? Owner.BreakLoop : Owner.Loop;
		}
		// ２番目の繰返迄に含む脱出対象の数で，多重ループの例外を判定する
		if(
			Outer != NoNode && *Depth <= Available &&
			*Depth >= (DoesCountSwitch ? Context.BreakDepth - Facts.Controls[Outer].BreakDepth + 1 : 2)
		) return;
	}
	Push(Node, LintRule::JumpUsage, "Review: Replace this jump with structured control flow", Output);
	// 終了
	return;
}

/**
 * 直前の文に属する導入名と初期化子の共通判定関数
 * 初期値のフィールド，又は代入トークンの後の式を読み，子孫全体を再走査しない
 * 計算量：導入名から文迄の祖先数 H と直接の子数 C に対し O(H + C)
 * @param Name 同じ宣言への束縛を確定した導入名
 * @param Stmt 直前の文
 * @return 初期化した束縛の宣言が直前の文に含まれるか
 * @throws FormatLimitExceeded 構文の照会が作業予算を超えた場合
 * @throws std::out_of_range ノードが構文表に無い場合
 */
bool LintAnalyzer::HasInitializedBinding(const NodeId Name, const NodeId Stmt) const {
	bool HasInitializer = false;
	for(NodeId Part = Name, Id = Syntax.GetParentOf(Part); Id != NoNode; Part = Id, Id = Syntax.GetParentOf(Id)) {
		// 初回の代入は返戻の直前の文其の物である場合だけを報告する
		if(
			Syntax.Is(Id, Role::FirstAssignmentBinding) && Id != Stmt &&
			!(Syntax.Is(Stmt, Role::ExprStmt) && Syntax.GetNamedChild(Stmt, 0) == Id)
		) return false;
		if(
			Syntax.Is(Id, Role::DeclWithType) || Syntax.Is(Id, Role::DeclInitDeclarator) || Syntax.Is(Id, Role::AssignmentOperation) ||
			Syntax.Is(Id, Role::ShortVarDecl)
		) {
			NodeId Val = Syntax.GetFieldChild(Id, ChildRole::Val);
			if(Val == NoNode) Val = Syntax.GetFieldChild(Id, ChildRole::Right);
			HasInitializer |= Val != NoNode && Val != Part;
			bool HasAssignment = false;
			for(const NodeId Item : Syntax.GetChildrenOf(Id)) {
				if(!Syntax.IsNamed(Item)) HasAssignment |= Syntax.IsTokenRole(Item, Role::AssignmentToken);
				else if(HasAssignment) HasInitializer = true;
			}
		}
		// 束縛を含む文に達した場合の返戻
		if(Id == Stmt) return HasInitializer;
	}
	// 直前の文に属さない束縛の対象外の返戻
	return false;
}

/**
 * 無意味なローカル変数検出関数
 * 計算量：宣言・返戻・所有者の照会ノード数 V，束縛の照会費用 Q，名前の総長 L，診断生成の費用 P に対し O(V + Q + L + P)
 * @param Node 対象ノード
 * @param Output 警告格納先
 * @throws std::bad_alloc 診断の確保に失敗した場合
 * @throws FormatLimitExceeded 宣言のメモリ又は束縛の照会が作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void LintAnalyzer::CheckUselessLocalVar(const NodeId Node, std::vector<LintWarning> &Output) {
	NodeId Prev = NoNode;
	for(const NodeId Child : Syntax.GetChildrenOf(Node)) {
		if(!Syntax.IsNamed(Child) || Syntax.Is(Child, Role::Comment)) continue;
		const NodeId Decl = std::exchange(Prev, Child);
		const NodeId Return = Syntax.Is(Child, Role::ExprStmt) ? Syntax.GetNamedChild(Child, 0) : Child;
		if(!Core::ControlFlow::IsReturn(Syntax, Return) || Decl == NoNode) continue;
		const NodeId Returned = UnwrapGroup(Syntax.GetNamedChild(Return, 0));
		if(!IsBindingName(Returned)) continue;
		// 静的な束縛は初期化回数と返した参照の寿命を保つ為，返戻式へ移さない
		bool HasStaticStorage = false;
		for(const NodeId Part : Syntax.GetChildrenOf(Decl)) if(Syntax.Is(Part, Role::MemberStorageSpecifier)) {
			HasStaticStorage |= GetModifierRoles(Part).test(static_cast<size_t>(Role::StaticLifetimeModifier));
		}
		if(HasStaticStorage) continue;
		const Core::BindingFact Ref = Syntax.GetBindingFact(Returned);
		// 同じ宣言への唯一の参照又は初期化した束縛を確定出来ない場合の継続
		if(Ref.Decl == NoNode || Ref.IsUncertain || BindingRefs[Ref.Decl] != 1 || !HasInitializedBinding(Ref.Decl, Decl)) continue;
		const std::string Name(GetNodeView(Ref.Decl));
		Push(Ref.Decl, LintRule::ImmediateReturnVariable, "Review: Inline immediately returned variable " + Name, Output, { Name });
	}
	// 終了
	return;
}

/**
 * 初期化子の匿名型と構造化束縛の判定関数
 * ラムダの呼出結果やラムダを実引数に取る呼出は匿名型と決め付けず，推論の候補に残す
 * 計算量：宣言の直接の子数 C と宣言子・括弧の深さ D の総和に対し O(C + D)
 * @param Node 推論指定の型のノード
 * @return 全宣言子がラムダ自身の初期化又は構造化束縛であるか
 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
 * @throws std::out_of_range ノードが表に無い場合
 */
bool LintAnalyzer::HasUnnameableInitializer(const NodeId Node) const {
	const NodeId Owner = Syntax.GetParentOf(Node);
	const NodeId OwnDeclarator = Syntax.GetFieldChild(Owner, ChildRole::Declarator);
	bool HasDeclarator = false;
	for(const NodeId Child : Syntax.GetChildrenOf(Owner)) {
		NodeId Declarator = Child;
		if(Syntax.Is(Child, Role::DeclInitDeclarator)) Declarator = Syntax.GetFieldChild(Child, ChildRole::Declarator);
		else if(Child != OwnDeclarator) continue;
		HasDeclarator = true;
		// 宣言子のラッパーだけを辿り，初期化子の導入名とは混同しない
		while(Declarator != NoNode && !IsBindingName(Declarator) && !Syntax.Is(Declarator, Role::StructuredBindingDeclarator)) {
			Declarator = Flow.GetNextDeclarator(Declarator);
		}
		if(Syntax.Is(Declarator, Role::StructuredBindingDeclarator)) continue;
		const NodeId Val = UnwrapGroup(Syntax.GetFieldChild(Child, ChildRole::Val));
		// 初期化子がラムダ自身でない宣言を含む場合の返戻
		if(!Syntax.Is(Val, Role::LambdaExpr)) return false;
	}
	// 対応する宣言子を全て確認出来たかの返戻
	return HasDeclarator;
}

/**
 * auto / var / any 使用検出関数
 * 計算量：対象文脈の判定・テキストの生成・診断生成の総費用 Q に対し O(Q)
 * @param Node 対象ノード
 * @param Output 警告格納先
 * @throws std::bad_alloc 型推論のテキスト又は診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void LintAnalyzer::CheckTypeInferenceKeyword(const NodeId Node, std::vector<LintWarning> &Output) {
	NodeId Keyword = Node;
	switch(Rules.InferredType) {
	case Core::InferredTypeNotation::Placeholder:
		// 後置戻値型を導く必須の指定と，名前で型を明示出来ない初期化又は分解の終了
		if(HasRequiredAuto(Node) || HasUnnameableInitializer(Node)) return;
		Keyword = NoNode;
		WalkChildrenCursor(
			Node,
			[this, &Keyword](const NodeId Part) -> bool {
				if(Syntax.Is(Part, Role::DeducedTypeKeyword)) Keyword = Part;
				// 指定トークンを見付ける迄の探索の返戻
				return Keyword == NoNode;
			}
		);
		break;
	case Core::InferredTypeNotation::Implicit:
		break;
	case Core::InferredTypeNotation::Named:
		// 型の位置の推論指定でない場合の終了
		if(!Syntax.IsWord(Node, Word::InferredType)) return;
		break;
	case Core::InferredTypeNotation::DynamicTop:
		if(Syntax.IsWord(Node, Word::DynamicTopType)) {
			Push(Node, LintRule::AnyType, "Violation: Replace `any` with `unknown` or a specific type", Output);
		}
		// 終了
		return;
	default:
		// 推論の指定を持たない言語の終了
		return;
	}
	// 推論を指定したトークンが無い場合の終了
	if(Keyword == NoNode) return;
	const std::string KeywordText(GetNodeView(Keyword));
	Push(Keyword, LintRule::InferredType, "Review: Use an explicit type instead of " + KeywordText, Output, { KeywordText });
	// 終了
	return;
}

/**
 * 関数戻値型の不在検出関数
 * 計算量：戻値型の直接の子数 C と診断のテキスト長 L に対し O(C + L)
 * @param Node 対象ノード
 * @param Output 警告格納先
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void LintAnalyzer::CheckReturnTypeHint(const NodeId Node, std::vector<LintWarning> &Output) {
	// 解析のエラーを含む定義の終了（型を書けない定義は投影で検査対象から除く）
	if(HasError(Node)) return;
	// 戻値型ノードの確認
	if(GetExplicitReturnTypeOf(Node) == NoNode) {
		const NodeId Name =
		Syntax.Is(Node, Role::NamedFunctionDef) ? GetFunctionNameOf(Node) : Syntax.GetFieldChild(Node, ChildRole::Name);
		Push(Name == NoNode ? Node : Name, LintRule::ReturnType, "Review: Add an explicit return type to this function", Output);
	}
	// 終了
	return;
}

/**
 * #define 定数検出関数
 * 計算量：マクロの直接の子数 C とテキスト長 L に対し O(C + L)
 * @param Node 対象ノード
 * @param Output 警告格納先
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void LintAnalyzer::CheckMacroConstant(const NodeId Node, std::vector<LintWarning> &Output) {
	const NodeId Name = Syntax.GetFirstNamedChildWithRole(Node, Role::Identifier);
	// 名前が無いか前処理条件で使うマクロの終了
	if(Name == NoNode || Facts.PreprocNames.contains(GetNodeView(Name))) return;
	const std::string_view Text = GetNodeView(Syntax.GetFieldChild(Node, ChildRole::Val));
	size_t Pos = 0, Depth = 0;
	// 置換列の空白とコメントの読飛の関数
	// @return 次のトークンが在るなら true
	const auto HasTokenAfterSkipping = [&]() -> bool {
		while(Pos < Text.size()) {
			if(std::isspace(static_cast<unsigned char>(Text[Pos]))) ++Pos;
			else if(Text.substr(Pos).starts_with("/*")) {
				const size_t End = Text.find("*/", Pos + 2);
				Pos = End == std::string_view::npos ? Text.size() : End + 2;
			} else if(Text.substr(Pos).starts_with("//")) Pos = Text.size();
			else break;
		}
		// 次のトークンの存在の返戻
		return Pos < Text.size();
	};
	while(HasTokenAfterSkipping()) {
		if(Text[Pos] == '(') ++Depth;
		else if(Text[Pos] == '+' || Text[Pos] == '-') {
			// 増減トークンは単項の符号に含めない
			if(Pos + 1 < Text.size() && Text[Pos + 1] == Text[Pos]) return;
		} else break;
		++Pos;
	}
	const size_t Begin = Pos;
	while(Pos < Text.size()) {
		const unsigned char Char = Text[Pos];
		if(
			!(
				std::isalnum(Char) || Char == '.' || Char == '\'' || (Char == '+' || Char == '-') && Pos > Begin &&
				(Text[Pos - 1] == 'e' || Text[Pos - 1] == 'E' || Text[Pos - 1] == 'p' || Text[Pos - 1] == 'P')
			)
		) break;
		++Pos;
	}
	// 単一の数値のトークンでない置換列の終了
	if(!IsNumericText(Text.substr(Begin, Pos - Begin))) return;
	while(HasTokenAfterSkipping()) {
		// 対応する閉括弧でない置換列の終了
		if(Text[Pos] != ')' || !Depth) return;
		--Depth;
		++Pos;
	}
	// 閉括弧の足りない置換列の終了
	if(Depth) return;
	Push(Name, LintRule::ConstantMacro, "Review: Replace this constant macro with a language constant", Output);
	// 終了
	return;
}

/**
 * using namespace 検出関数
 * 計算量：宣言の子の数 C，診断生成の費用 Q に対し O(C + Q)
 * @param Node 対象ノード
 * @param Output 警告格納先
 * @throws std::bad_alloc 診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void LintAnalyzer::CheckUsingNamespace(const NodeId Node, std::vector<LintWarning> &Output) {
	// using ディレクティブと using 宣言は同じノード型の為，名前空間の予約語の子で判別
	if(Syntax.HasWordChild(Node, Word::UsingNamespace)) {
		Push(Node, LintRule::UsingNamespace, "Review: Avoid `using namespace`; qualify names explicitly", Output);
	}
	// 終了
	return;
}

/**
 * return コメント不在検出関数
 * 計算量：先行コメントのテキスト長 L，文数の共有照会と診断生成の費用 Q に対し O(L + Q)
 * @param Node 対象ノード
 * @param Comments コメントの範囲（開始の昇順）
 * @param Output 警告格納先
 * @throws std::bad_alloc 検査の事実・照会又は診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void LintAnalyzer::CheckReturnCommentPresence(
	const NodeId Node,
	const std::vector<std::pair<uint32_t, uint32_t>> &Comments,
	std::vector<LintWarning> &Output
) {
	// 返戻の説明を規約の対象としない文法（CSS / SCSS の @return）の場合の終了
	if(!Rules.HasReturnComments) return;
	// 切替の節と短い関数の判定を，同じ完成構文の所属から読む
	const NodeId ParentContext = Syntax.GetParentOf(Node);
	if(ParentContext != NoNode) {
		const StaticLint::ControlContext &Context = Facts.Controls[ParentContext];
		if(Context.Case != NoNode || Syntax.Is(Context.Flow, Role::FlowBoundary) && CountStmtLines(Context.Flow).value_or(3) < 3) {
			// 切替の節の中と短い関数の return には返戻コメントを求めない事の終了
			return;
		}
	}
	// 波括弧の無い制御構文の本体の return は，見出の折返で行の途中に在っても其の制御構文の行の前を返戻コメントの位置とする（整形仕様 7.6 の畳んだ本体）
	const NodeId Parent = Syntax.GetParentOf(Node);
	const NodeId Owner = Syntax.Is(Parent, Role::BodyOwner) || Syntax.Is(Parent, Role::If) ? Parent : NoNode;
	// return の行又は其の制御構文の行の直前の行で終わるコメント（行の途中の return は其の行で始まる文の直前のコメント）が有れば警告不要
	for(const NodeId Target : { Node, Owner }) {
		const uint32_t TargetLine = Target == NoNode ? 0 : GetLineStartOf(GetBeginOf(Target));
		if(!TargetLine) continue;
		// コメントは重ならず開始の昇順が終端の昇順でも有る為，直前の行より後で終わる最初のコメントを確認
		// 独立した直前のコメントだけを返戻の説明として読む
		if(
			const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Above = std::lower_bound(
				Comments.begin(),
				Comments.end(),
				GetLineStartOf(TargetLine - 1) + 1,
				[](const std::pair<uint32_t, uint32_t> &Comment, const uint32_t Pos) -> bool {
					// コメントの終端が位置より前かの返戻
					return Comment.second < Pos;
				}
			); Above != Comments.end() && Above->second <= TargetLine
		) {
			if(
				const size_t LineStart = GetLineStartOf(Above->first);
				std::string_view(Src).substr(LineStart, Above->first - LineStart).find_first_not_of(" \t") != std::string_view::npos
			) continue;
			const size_t Idx = static_cast<size_t>(Above - Comments.begin());
			const std::string_view Text = Repository.GetRecords()[Facts.Comments[Idx].Record].Text;
			const size_t First = Text.find_first_not_of(" \t\r\n");
			if(First == std::string_view::npos) break;
			// 直前に説明を持つコメントが在る場合の終了
			return;
		}
	}
	// return 前コメント欠落の警告登録
	Push(Node, LintRule::MissingReturnComment, "Violation: Add the required comment before this `return`", Output);
	// 終了
	return;
}

/**
 * 数値の０比較候補の検出関数
 * 連鎖比較の各比較は隣接する２項と其の間の演算子に対応する
 * 計算量：直接の子数 C，各比較の判定・診断生成の総費用 Q に対し O(C + Q)
 * @param Node 対象ノード
 * @param Output 警告格納先
 * @throws std::bad_alloc 検査の事実・照会又は診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void LintAnalyzer::CheckZeroCmp(const NodeId Node, std::vector<LintWarning> &Output) {
	// 隣接する２項の比較の検査関数
	// 計算量：比較の判定・診断生成の費用 Q に対し O(Q)
	// @param Left 左の被演算子
	// @param Right 右の被演算子
	// @param Token 此の２項の間の演算子
	// @throws std::bad_alloc 検査の事実・照会又は診断の確保に失敗した場合
	// @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
	// @throws std::length_error 本文又は作業列の要素数上限を超える場合
	// @throws std::out_of_range 照会する添字又は識別子が表に無い場合
	const auto InspectPair = [&](const NodeId Left, const NodeId Right, const NodeId Token) -> void {
		// 隣接する比較の検査
		CheckZeroCmpPair(Node, Left, Right, Token, Output);
		// 終了
		return;
	};
	VisitCmpPairs(Syntax, Node, InspectPair);
	// 終了
	return;
}

/**
 * ヌリッシュ比較縮約候補検出関数
 * 計算量：対象ノードの子数 C とテキストの総長 L，診断生成の費用 Q に対し O(C + L + Q)
 * @param Node 対象二項式
 * @param Output 警告格納先
 * @throws std::bad_alloc 診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void LintAnalyzer::CheckNullishCmpCandidate(const NodeId Node, std::vector<LintWarning> &Output) {
	const std::string_view Operator = FindUnnamedToken(Node, Rules.Operators.LogicalOr, Rules.Operators.LogicalAnd);
	// 対象の演算子を持たない式の終了
	if(Operator.empty()) return;
	const NodeId Outer = GetOutermostGroup(Node), Parent = Syntax.GetParentOf(Outer);
	// 同じ演算子の最も外側だけで連続範囲を列挙する
	if(Syntax.Is(Parent, Role::BinaryExpr) && Syntax.HasTokenChild(Parent, Operator)) return;
	struct Part {
		NodeId Expr;
		NodeId Token;
	};
	std::vector<Part> Pending { { Node, NoNode } };
	std::string_view Name;
	NodeId FirstToken = NoNode;
	bool HasNull = false, HasUndefined = false;
	const auto Finish = [&]() -> void {
		if(HasNull && HasUndefined) {
			Push(FirstToken, LintRule::NullishComparison, "Review: Combine the `null` and `undefined` checks", Output);
		}
		// グループの状態の初期化
		Name = {};
		FirstToken = NoNode;
		HasNull = HasUndefined = false;
		// 終了
		return;
	};
	while(!Pending.empty()) {
		const Part Current = Pending.back();
		Pending.pop_back();
		const NodeId Expr = UnwrapGroup(Current.Expr);
		if(Syntax.Is(Expr, Role::BinaryExpr) && Syntax.HasTokenChild(Expr, Operator)) {
			Pending.push_back({ Syntax.GetNamedChild(Expr, 1), GetTokenChildOf(Expr, Operator) });
			Pending.push_back({ Syntax.GetNamedChild(Expr, 0), Current.Token });
			continue;
		}
		const std::string_view Compare = Operator == Rules.Operators.LogicalOr ?
		FindUnnamedToken(Expr, Rules.Operators.Equal, Rules.Operators.StrictEqual) :
		FindUnnamedToken(Expr, Rules.Operators.NotEqual, Rules.Operators.StrictNotEqual);
		NodeId Subject = UnwrapGroup(Syntax.GetNamedChild(Expr, 0));
		NodeId Nullish = UnwrapGroup(Syntax.GetNamedChild(Expr, 1));
		if(Syntax.Is(Subject, Role::NullishNull) || Syntax.Is(Subject, Role::NullishUndefined)) std::swap(Subject, Nullish);
		if(
			Compare.empty() || !Syntax.Is(Subject, Role::IdentifierLeafLike) ||
			!(Syntax.Is(Nullish, Role::NullishNull) || Syntax.Is(Nullish, Role::NullishUndefined))
		) {
			Finish();
			continue;
		}
		const std::string_view NextName = Syntax.GetCmpNameView(Subject);
		if(Name != NextName) {
			Finish();
			Name = NextName;
		} else if(FirstToken == NoNode) FirstToken = Current.Token;
		HasNull |= Syntax.Is(Nullish, Role::NullishNull);
		HasUndefined |= Syntax.Is(Nullish, Role::NullishUndefined);
	}
	Finish();
	// 終了
	return;
}

/**
 * C 形式キャスト検出関数
 * 計算量：宣言型の照会と診断生成の総費用 Q に対し O(Q)
 * @param Node 対象ノード
 * @param Output 警告格納先
 * @throws std::bad_alloc 構文の照会又は診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void LintAnalyzer::CheckCStyleCast(const NodeId Node, std::vector<LintWarning> &Output) {
	// 投影で確認した変換先の型でない場合の終了
	if(!Syntax.GetNode(Node).Facts.HasBuiltinCastType) return;
	// 指定された変換先の C 形式のキャストの報告
	Push(Node, LintRule::CStyleCast, "Violation: Replace the C-style cast with the appropriate C++ cast", Output);
	// 終了
	return;
}

/**
 * 条件の初期化文と導入名の判定関数
 * 条件の入口だけを読み，本体や実引数の宣言を条件の束縛と混同しない
 * 計算量：条件のラッパーの深さ D に対し O(D)
 * @param Node 対象の分岐
 * @return 条件へ移せない束縛を持つか
 * @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
 * @throws std::out_of_range ノードが表に無い場合
 */
bool LintAnalyzer::HasConditionBinding(const NodeId Node) const {
	// 分岐自身へ投影された初期化文を持つ場合の返戻
	if(Syntax.GetFieldChild(Node, ChildRole::Initializer) != NoNode) return true;
	NodeId Id = Syntax.GetFieldChild(Node, ChildRole::Cond);
	while(Id != NoNode) {
		// 初期化文又は条件内の宣言を持つ場合の返戻
		if(
			Syntax.GetFieldChild(Id, ChildRole::Initializer) != NoNode || Syntax.Is(Id, Role::BindingCond) ||
			Syntax.Is(Id, Role::DeclWithType)
		) return true;
		// 条件のラッパー以外へ降りない場合の返戻
		if(!Syntax.Is(Id, Role::CondParenWrap) && !Syntax.Is(Id, Role::ValParens)) return false;
		const NodeId Val = Syntax.GetFieldChild(Id, ChildRole::Val);
		Id = Val == NoNode ? Syntax.GetNamedChild(Id, 0) : Val;
	}
	// 条件内の束縛が無い場合の返戻
	return false;
}

/**
 * 連続又は連鎖する同一本体の if 統合候補検出関数
 * 計算量：隣接するコメント数 C，条件・本体の走査，テキストの照合と診断生成の費用 Q に対し O(C + Q)
 * @param Node 対象 if 文
 * @param Output 警告格納先
 * @throws std::bad_alloc 検査の事実・照会又は診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文・制御の照会が作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void LintAnalyzer::CheckUnsafeIfMergeCandidate(const NodeId Node, std::vector<LintWarning> &Output) {
	// 最長のグループに含めた分岐の終了
	if(MergedIfs.contains(Node)) return;
	// ラッパーを除いた分岐本体の取得関数
	// 計算量：ラッパーの深さ D に対し O(D)
	// @param Body 分岐本体
	// @return 単独の文を囲むラッパーの内側又は元の本体
	// @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
	const auto SingleBody = [this](NodeId Body) -> NodeId {
		while(
			(
				Syntax.Is(Body, Role::FunctionBodyContainer) || Syntax.Is(Body, Role::BodyWrapper) || Syntax.Is(Body, Role::StmtSeq) ||
				Syntax.Is(Body, Role::PlainStmtSeq)
			) && Syntax.GetNamedChildCount(Body) == 1
		) Body = Syntax.GetNamedChild(Body, 0);
		// ラッパーを除いた本体の返戻
		return Body;
	};
	// 同じ候補の照合で共有する分岐の構造
	struct Branch {
		NodeId Body = NoNode; // 真枝の本体
		NodeId Next = NoNode; // 次の条件節
		bool HasAlternative = false; // 条件付又は最後の後続節を持つか
		bool HasBody = false; // 空の波括弧を含め本体を持つか
	};
	// 本体と後続節をフィールド又は投影済の役割から読む
	// 計算量：直接の子数 C とラッパーの深さ D に対し O(C + D)
	// @param If 条件分岐
	// @return 分岐の本体と後続節の構造
	// @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
	const auto GetBranch = [this, &SingleBody](const NodeId If) -> Branch {
		Branch Parts;
		// 条件節でない後続ノードは照会しない
		if(!Syntax.Is(If, Role::CondMergeInspection)) return Parts;
		Parts.Body = Syntax.GetIfConsequence(If);
		NodeId Alternative = NoNode;
		bool IsAfterElse = false;
		for(const NodeId Child : Syntax.GetChildrenOf(If)) {
			if(Syntax.Is(Child, Role::OpenBrace)) Parts.HasBody = true;
			if(Syntax.HasField(Child, ChildRole::Alternative) || IsAfterElse && Syntax.IsNamed(Child)) {
				Parts.HasAlternative = true;
				if(Alternative == NoNode) Alternative = Child;
				IsAfterElse = false;
			} else if(Syntax.Is(Child, Role::Else)) {
				Parts.HasAlternative = true;
				IsAfterElse = true;
			} else if(
				Parts.Body == NoNode &&
				(Syntax.Is(Child, Role::BranchBody) || Syntax.Is(Child, Role::ValStmtSeq) || Syntax.Is(Child, Role::ThenSection))
			) Parts.Body = Child;
		}
		// else 節と制御本体のラッパーだけを除いて後続の条件節を読む
		if(Syntax.Is(Alternative, Role::AlternativeClause)) Alternative = Syntax.GetNamedChild(Alternative, 0);
		Alternative = SingleBody(Alternative);
		if(Syntax.Is(Alternative, Role::CondMergeInspection)) Parts.Next = Alternative;
		else if(Syntax.Is(If, Role::ConditionalBranchClause)) {
			const NodeId Next = Syntax.GetNextNamedSibling(If);
			if(Syntax.Is(Next, Role::ConditionalBranchClause)) {
				Parts.Next = Next;
				Parts.HasAlternative = true;
			}
		}
		Parts.Body = SingleBody(Parts.Body);
		Parts.HasBody |= Parts.Body != NoNode;
		// １つの候補から読んだ構造の返戻
		return Parts;
	};
	const Branch FirstBranch = GetBranch(Node);
	const NodeId Body = FirstBranch.Body;
	// 本体の無い分岐の終了
	if(!FirstBranch.HasBody) return;
	const bool HasElse = FirstBranch.HasAlternative;
	// 同じ位置のノードとトークンを，境界を保って比較する
	// 計算量：照合するノード数 V とトークンの総長 L に対し O(V + L)
	// @param Left 左の本体
	// @param Right 右の本体
	// @return 種別・子の並び・個々のトークンが一致するか
	// @throws FormatLimitExceeded 比較が作業予算を超える場合
	// @throws std::bad_alloc 比較の作業列を確保出来ない場合
	// @throws std::length_error 作業列の要素数上限を超える場合
	// @throws std::out_of_range ノードが表に無い場合
	const auto IsSameBody = [this](const NodeId Left, const NodeId Right) -> bool {
		// 本体のノードを持たない空の波括弧同士の一致の返戻
		if(Left == NoNode || Right == NoNode) return Left == Right;
		std::vector<std::pair<NodeId, NodeId>> Pending { { Left, Right } };
		while(!Pending.empty()) {
			const std::pair<NodeId, NodeId> Pair = Pending.back();
			Pending.pop_back();
			const Core::SyntaxNode &First = Syntax.GetNode(Pair.first), &Second = Syntax.GetNode(Pair.second);
			Syntax.Charge(1);
			// 種別又は子数が異なる本体の返戻
			if(First.Kind.Def != Second.Kind.Def || First.Children.size() != Second.Children.size()) return false;
			Syntax.Charge(std::min(First.Lexeme.size(), Second.Lexeme.size()));
			// トークンの境界内の不一致の返戻
			if(First.Lexeme != Second.Lexeme) return false;
			// 子の並びの順に照合する対の記録
			for(size_t Idx = First.Children.size(); Idx; --Idx) Pending.emplace_back(First.Children[Idx - 1], Second.Children[Idx - 1]);
		}
		// 全ての対応が一致した事の返戻
		return true;
	};
	bool HasMerge = false;
	NodeId Current = Node;
	while(true) {
		NodeId Stmt = Current;
		const NodeId Parent = Syntax.GetParentOf(Stmt);
		if(!HasElse && Syntax.Is(Parent, Role::ExprStmt) && Syntax.GetNamedChildCount(Parent) == 1) Stmt = Parent;
		NodeId Next = HasElse ? GetBranch(Current).Next : Syntax.GetNextNamedSibling(Stmt);
		while(!HasElse && Syntax.Is(Next, Role::Comment)) Next = Syntax.GetNextNamedSibling(Next);
		if(!HasElse && Syntax.Is(Next, Role::ExprStmt) && Syntax.GetNamedChildCount(Next) == 1) Next = Syntax.GetNamedChild(Next, 0);
		const Branch NextBranch = GetBranch(Next);
		if(
			// 後続の初期化と独立した束縛の寿命を保つ
			!Syntax.Is(Next, Role::CondMergeInspection) || !HasElse && NextBranch.HasAlternative || HasConditionBinding(Next) ||
			!HasElse && HasConditionBinding(Current)
		) break;
		const NodeId NextBody = NextBranch.Body;
		if(!NextBranch.HasBody || !IsSameBody(Body, NextBody)) break;
		MergedIfs.insert(Next);
		HasMerge = true;
		Current = Next;
	}
	if(HasMerge) Push(Node, LintRule::MergeIf, "Review: Merge the conditions of these identical branch bodies", Output);
	// 終了
	return;
}

/**
 * 真偽値の排他的論理和の代入を勧める反転代入の検出関数
 * 計算量：条件と代入の照会・診断生成の総費用 Q に対し O(Q)
 * @param Node 対象ノード
 * @param Output 警告格納先
 * @throws std::bad_alloc 診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::length_error 語又は診断の作業列の要素数上限を超える場合
 * @throws std::out_of_range ノードが表に無い場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void LintAnalyzer::CheckBoolFlipPattern(const NodeId Node, std::vector<LintWarning> &Output) {
	const NodeId Lhs = UnwrapGroup(Syntax.GetNamedChild(Node, 0)), Rhs = UnwrapGroup(Syntax.GetNamedChild(Node, 1));
	// 単純名の自己代入でない式の終了
	if(!Syntax.Is(Lhs, Role::BoolFlipName)) return;
	NodeId Token = GetTokenChildOf(Node, "^=");
	bool IsCandidate = Token != NoNode && Syntax.IsWord(Rhs, Word::True) && !Rules.ShouldPreferBoolXorAssignment;
	if(Token == NoNode) {
		Token = GetTokenChildOf(Node, Rules.Operators.Assignment);
		// 代入の演算子を持たない式の終了
		if(Token == NoNode) return;
		if(Syntax.Is(Rhs, Role::UnaryPrefixExpr) && Rules.ShouldPreferBoolXorAssignment) {
			const NodeId Operand = UnwrapGroup(Syntax.GetNamedChild(Rhs, 0));
			IsCandidate = Syntax.Is(Operand, Role::BoolFlipName) && Syntax.GetCmpNameView(Operand) == Syntax.GetCmpNameView(Lhs) &&
			(Syntax.HasTokenChild(Rhs, Rules.Operators.LogicalNot) || Syntax.HasWordChild(Rhs, Word::Negation));
		} else if(Syntax.Is(Rhs, Role::BinaryExpr)) {
			// 真と同じ名前の２項だけで成る排他的論理和の検査
			// 計算量：ラッパーの深さ D，語の照会で訪れるノード数 V と名前のテキスト長 L に対し O(D + V + L)
			// @param Left 左項
			// @param Right 右項
			// @param Operator ２項の間の演算子
			// @throws FormatLimitExceeded 構文の照会が作業予算を超える場合
			// @throws std::bad_alloc 語の照会の作業列を確保出来ない場合
			// @throws std::length_error 語の照会の作業列の要素数上限を超える場合
			// @throws std::out_of_range ノードが表に無い場合
			const auto InspectFlip = [&](const NodeId Left, const NodeId Right, const NodeId Operator) -> void {
				// 排他的論理和でない演算の終了
				if(!Syntax.Is(Operator, Role::XorOperator)) return;
				const NodeId First = UnwrapGroup(Left), Second = UnwrapGroup(Right);
				IsCandidate = Syntax.Is(First, Role::BoolFlipName) && Syntax.GetCmpNameView(First) == Syntax.GetCmpNameView(Lhs) &&
				Syntax.IsWord(Second, Word::True) ||
				Syntax.Is(Second, Role::BoolFlipName) && Syntax.GetCmpNameView(Second) == Syntax.GetCmpNameView(Lhs) &&
				Syntax.IsWord(First, Word::True);
				// 終了
				return;
			};
			VisitCmpPairs(Syntax, Rhs, InspectFlip);
		}
	}
	if(IsCandidate) {
		Push(Token, LintRule::BooleanNegation, "Review: Use the boolean-negation assignment required for this target", Output);
	}
	// 終了
	return;
}

/**
 * 関数ドキュメントコメント不在検出関数
 * 計算量：関数の事実の取得に平均 O(1)，名前の照会・診断生成の費用 Q に対し O(Q + 1)
 * @param Node ドキュメントを付ける定義
 * @param Output 警告格納先
 * @throws std::bad_alloc 検査の事実・照会又は診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::logic_error 呼出先で構文・所有関係の内部契約が成立しない場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void LintAnalyzer::CheckDocCommentPresence(const NodeId Node, std::vector<LintWarning> &Output) {
	// 共通の認識で同じ定義へ所属した記録と，処理系の読むドキュメント属性の有無
	const StaticLint::FunctionFacts &Function = Facts.Functions.at(Node);
	if(!Function.HasDocAttribute && Function.Docs.empty()) {
		const NodeId Name = GetFunctionNameOf(Node);
		Push(
			Name == NoNode ? Node : Name,
			LintRule::MissingDocumentation,
			"Violation: Function is missing a documentation comment",
			Output
		);
	}
	// 終了
	return;
}

/**
 * ドキュメントコメントの項目と説明の不足の検出関数
 * 同期しないドキュメント（順を保てない行形式のドキュメント等）の項目の不足・余分も，シグネチャと照合して報告する
 * 計算量：訪れるノードの累計数 N，ドキュメント行数 L，項目数 I，仮引数数 P，照合する本文の累計長 B に対し平均 O(N + L² + I + P + B)
 * @param Node 対象ノード
 * @param Texts アンカーの手前に続く独立したコメントの本文（原文の順，１本でも挟まると直前の兄弟がドキュメントで無く為る為，並び全体を見る）
 * @param Output 警告格納先
 * @throws std::bad_alloc 検査の事実・照会又は診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文・制御の照会が作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::logic_error 呼出先で構文・所有関係の内部契約が成立しない場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void LintAnalyzer::CheckDocCommentCompleteness(
	const NodeId Node,
	const std::span<const std::string_view> Texts,
	std::vector<LintWarning> &Output
) {
	// 自動付与の雛形は説明文が空の為，概要・仮引数・戻値の説明記入を要求
	const Core::DocStyle Dialect = Rules.Notation.Doc;
	std::vector<std::string> Lines;
	std::optional<Core::DocReading> CodeStarts { Core::DocReading{} };
	bool HasDecodedDoc = false;
	const CommentStructure &Structure = Repository.GetOutputStructure();
	// ドキュメントの論理行の追加
	const auto AppendLines =
	[this, &Structure, &Lines, &CodeStarts, &HasDecodedDoc](const StaticLint::CommentFacts &Comment) -> void {
		const size_t Idx = Comment.Record;
		const CommentRecord &Record = Repository.GetRecords()[Idx];
		HasDecodedDoc = HasDecodedDoc || Structure.GetReadBody(Idx).has_value();
		const size_t Begin = Lines.size();
		AppendSrcLogicalLines(Record, Lines);
		if(DocWork) for(size_t Pos = Begin; Pos < Lines.size(); ++Pos) {
			DocWork->Bytes += Lines[Pos].size();
			++DocWork->Lines;
		}
		std::optional<Core::DocReading> Starts =
		DocCommentReader::DocCodeStarts(Record, Structure.GetCodeMask(Idx), Structure.HasUnclosedCode(Idx));
		if(CodeStarts && Starts) {
			CodeStarts->Code.insert(CodeStarts->Code.end(), Starts->Code.begin(), Starts->Code.end());
			for(std::vector<unsigned char> &Mask : Starts->Protected) CodeStarts->Protected.push_back(std::move(Mask));
		} else CodeStarts.reset();
		// 終了
		return;
	};
	OutputPoint DocPoint = GetPointOf(Node);
	size_t RecordIdx = Facts.Comments.size();
	// ドキュメントの有無（空の骨格の囲みのドキュメントは論理行を持たない）
	bool HasDoc = false;
	const auto AppendDoc = [&AppendLines, &DocPoint, &RecordIdx, &HasDoc](const StaticLint::CommentFacts &Comment) -> void {
		if(!HasDoc) {
			DocPoint = Comment.Range.Point;
			RecordIdx = Comment.Record;
		}
		AppendLines(Comment);
		HasDoc = true;
		// 終了
		return;
	};
	// 所属確定済の記録と，原文に保持した属性の先行記録を原文順で併合し，同じ記録を重ねない
	const std::vector<size_t> &Docs = Facts.Functions.at(Node).Docs;
	size_t DocIdx = 0;
	for(const std::string_view Text : Texts) {
		const StaticLint::CommentFacts &Comment = GetCommentInfo(Text);
		const CommentRecord &Record = Repository.GetRecords()[Comment.Record];
		if(!DocCommentReader::IsDocBlock(Record) && DocCommentReader::GetDocLineMarker(Record).empty()) continue;
		while(DocIdx < Docs.size() && Docs[DocIdx] < Comment.Record) AppendDoc(Facts.Comments[Docs[DocIdx++]]);
		if(DocIdx < Docs.size() && Docs[DocIdx] == Comment.Record) ++DocIdx;
		AppendDoc(Comment);
	}
	while(DocIdx < Docs.size()) AppendDoc(Facts.Comments[Docs[DocIdx++]]);
	if(!HasDoc && Facts.Functions.at(Node).HasDocAttribute) {
		const NodeId Attribute = Facts.Functions.at(Node).DocAttribute;
		if(!Rules.DocForm.empty()) {
			Push(
				Attribute,
				LintRule::DocumentationNotation,
				"Violation: Use " + std::string(Rules.DocForm) + " for this documentation",
				Output,
				{ std::string(Rules.DocForm) }
			);
		}
		// 本文を値として持つドキュメントの検査の終了
		return;
	}
	// ドキュメントの無い定義の終了
	if(!HasDoc) return;
	const Core::Signature &Signature = Facts.Functions.at(Node).Signature;
	const Core::NotationRules &ReadingRules = Rules.Notation;
	// 展開後の読取情報を持つドキュメントは，共通判定を使う
	if(HasDecodedDoc) CodeStarts = DocCommentReader::CodeLineStarts(Rules.Notation, Lines);
	Core::DocParamIdx ParamIdx;
	const std::optional<std::vector<DocItem>> Items =
	DocCommentReader::ParseItems(ReadingRules, Lines, Signature, CodeStarts, ParamIdx);
	// 項目の構造を読めないドキュメントの報告の終了
	if(!Items) return;
	// 名前だけ又は空の本文なら，文章の項目構造を推測せず説明の不在を確定出来る
	bool CanCheckAbsentItems =
	Core::VocabularyOf(ReadingRules.Vocabulary, Core::DocDescription::Summary).Policy != Core::DocStylePolicy::NameSummary;
	if(!CanCheckAbsentItems) {
		bool HasNameLine = false;
		CanCheckAbsentItems = true;
		for(const std::string &Text : Lines) {
			std::string_view Line = Text;
			Line.remove_prefix(std::min(Line.find_first_not_of(" \t\r"), Line.size()));
			while(!Line.empty() && (Line.back() == ' ' || Line.back() == '\t' || Line.back() == '\r')) Line.remove_suffix(1);
			if(Line.empty()) continue;
			if(!HasNameLine && Line == Signature.Name) HasNameLine = true;
			else {
				CanCheckAbsentItems = false;
				break;
			}
		}
	}
	// シグネチャ・項目・名前のノードを１度インデックス化する
	if(!ParamIdx) ParamIdx.emplace(Signature.Params.begin(), Signature.Params.end());
	const std::unordered_set<std::string_view> &ParamNames = *ParamIdx;
	std::unordered_set<std::string_view> CoveredNames, DescribedNames;
	std::unordered_set<uint32_t> DescribedParams;
	bool HasReturnItem = false, HasReturnDescription = false;
	for(const DocItem &Item : *Items) {
		// 項目の数でなく，実際の仮引数と戻値毎に説明の充足を集約
		if(Item.Kind != DocItemKind::Param && Item.Kind != DocItemKind::Return) continue;
		const bool HasDescription = DocCommentReader::HasItemDescription(ReadingRules, Item, Lines);
		if(Item.Kind == DocItemKind::Return) {
			HasReturnItem = true;
			HasReturnDescription = HasReturnDescription || HasDescription;
			continue;
		}
		if(Item.ParamOrdinal && HasDescription) DescribedParams.insert(Item.ParamOrdinal);
		for(std::string_view Part : Item.Names) {
			// 複数名の各名前でも，文法の順で参照渡・可変長・変数のマーカーを除く
			for(const std::string_view Mark : ReadingRules.DocParamMarks) if(Part.starts_with(Mark)) Part.remove_prefix(Mark.size());
			if(!ParamNames.contains(Part)) continue;
			CoveredNames.insert(Part);
			if(HasDescription) DescribedNames.insert(Part);
		}
	}
	std::unordered_map<std::string, NodeId> ParamNodes;
	std::unordered_map<NodeId, const Core::ParamShape *> ParamOwners;
	bool HasParamNodes = false;
	// 説明不足の仮引数が在る時だけ，位置と分解構造の対応を作る
	const auto ParamNode = [this, &Signature, &ParamNodes, &ParamOwners, &HasParamNodes](const std::string_view Name) -> NodeId {
		if(!HasParamNodes) {
			for(const Core::ParamShape &Param : Signature.ParamShapes) for(const NodeId Binding : Param.Bindings) {
				ParamNodes.emplace(Syntax.GetCmpName(Binding), Binding);
				ParamOwners.emplace(Binding, &Param);
				if(DocWork) ++DocWork->Bindings;
			}
			HasParamNodes = true;
		}
		const std::unordered_map<std::string, NodeId>::const_iterator Found = ParamNodes.find(std::string(Name));
		// 最初の名前のノード又は無名の返戻
		return Found == ParamNodes.end() ? NoNode : Found->second;
	};
	if(Dialect == Core::DocStyle::Prose && !Signature.Name.empty() && RecordIdx < Facts.Comments.size()) {
		const std::string_view Body = Repository.GetRecords()[RecordIdx].Text;
		if(
			const size_t First = Body.find_first_not_of(" \t\r\n");
			First != std::string_view::npos && (
				Body.compare(First, Signature.Name.size(), Signature.Name) ||
				First + Signature.Name.size() < Body.size() && IsIdentifierChar(Body[First + Signature.Name.size()])
			)
		) {
			Push(
				GetPointInComment(RecordIdx, Body, First),
				LintRule::DocumentationName,
				"Violation: Start the documentation with " + Signature.Name,
				Output,
				{ Signature.Name },
				Node
			);
		}
	}
	if(RecordIdx < Facts.Comments.size()) {
		const std::string_view Body = Repository.GetRecords()[RecordIdx].Text;
		size_t Search = 0;
		for(const DocItem &Item : *Items) {
			if(Item.Begin >= Lines.size()) continue;
			std::string_view Line = Lines[Item.Begin];
			Line.remove_prefix(std::min(Line.find_first_not_of(" \t"), Line.size()));
			for(const Core::DocTagSynonym &Synonym : ReadingRules.DocTagSynonyms) {
				if(!Core::DocTagWordLength(ReadingRules, Line, Synonym.From)) continue;
				if(const size_t Found = Body.find(Synonym.From, Search); Found != std::string_view::npos) {
					Push(
						GetPointInComment(RecordIdx, Body, Found),
						LintRule::DocumentationItemForm,
						"Violation: Use " + std::string(Synonym.To) + " for this documentation item",
						Output,
						{ std::string(Synonym.To) },
						Node
					);
					Search = Found + Synonym.From.size();
				}
				break;
			}
		}
	}
	// 項目を持つ書式のシグネチャに必要な項目の照合
	for(const std::string &LocalParam : Signature.Params) {
		if(DescribedNames.contains(LocalParam) || !CanCheckAbsentItems && !CoveredNames.contains(LocalParam)) continue;
		const NodeId Param = ParamNode(LocalParam);
		const std::unordered_map<NodeId, const Core::ParamShape *>::const_iterator Owner = ParamOwners.find(Param);
		if(Owner != ParamOwners.end() && Owner->second->IsDestructure) continue;
		Push(
			Param == NoNode ? GetPointOf(Node) : GetPointOf(Param),
			LintRule::DocumentationParameter,
			"Violation: Documentation is missing a description for parameter '" + LocalParam + "'",
			Output,
			{ LocalParam },
			Param == NoNode ? Node : Param
		);
	}
	if(CanCheckAbsentItems) {
		for(const Core::UnnamedParam &Param : Signature.UnnamedParams) {
			Push(
				Param.Node,
				LintRule::DocumentationParameter,
				"Violation: Documentation is missing a description for parameter #" + std::to_string(Param.Ordinal),
				Output,
				{ int64_t{ Param.Ordinal } }
			);
		}
		for(const Core::ParamShape &Param : Signature.ParamShapes) if(Param.IsDestructure) {
			bool HasAllDescriptions = !Param.Bindings.empty();
			for(const NodeId Binding : Param.Bindings) {
				HasAllDescriptions = DescribedNames.contains(Syntax.GetCmpName(Binding));
				if(!HasAllDescriptions) break;
			}
			if(HasAllDescriptions || DescribedParams.contains(Param.Ordinal) || Param.IsUncertain) continue;
			Push(
				Param.Node,
				LintRule::DocumentationParameter,
				"Violation: Documentation is missing a description for parameter #" + std::to_string(Param.Ordinal),
				Output,
				{ int64_t{ Param.Ordinal } }
			);
		}
	}
	// ドキュメント全体の戻値の説明不足を１度だけ報告
	if(Signature.Return.HasCallResult == true && !HasReturnDescription && (CanCheckAbsentItems || HasReturnItem)) {
		const NodeId ReturnType = GetExplicitReturnTypeOf(Node), Name = GetFunctionNameOf(Node);
		Push(
			ReturnType != NoNode ? ReturnType : Name == NoNode ? Node : Name,
			LintRule::DocumentationReturn,
			"Violation: Documentation is missing a return-value description",
			Output
		);
	}
	// 概要の無いドキュメントコメントの報告（godoc の書出の関数名は概要に数えない）
	if(!DocCommentReader::HasSummary(ReadingRules, Lines, *Items, Signature.Name)) {
		Push(DocPoint, LintRule::DocumentationSummary, "Violation: Documentation is missing a summary", Output, LintSortVals(), Node);
	}
	// 終了
	return;
}

/**
 * 整数リテラル境界比較検出関数（全言語）
 * 計算量：直接の子数 C，各比較の判定・診断生成の総費用 Q に対し O(C + Q)
 * @param Node 対象ノード
 * @param Output 警告格納先
 * @throws std::bad_alloc 構文の照会又は診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void LintAnalyzer::CheckCmpBoundary(const NodeId Node, std::vector<LintWarning> &Output) {
	// 隣接する２項の境界の検査関数
	// 計算量：比較の判定・診断生成の費用 Q に対し O(Q)
	// @param Left 左の被演算子
	// @param Right 右の被演算子
	// @param Token 此の２項の間の演算子
	// @throws std::bad_alloc 構文の照会又は診断の確保に失敗した場合
	// @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
	// @throws std::length_error 本文又は作業列の要素数上限を超える場合
	// @throws std::out_of_range 照会する添字又は識別子が表に無い場合
	const auto InspectPair = [&](const NodeId Left, const NodeId Right, const NodeId Token) -> void {
		CheckBoundaryCmpPair(Left, Right, Token, Output);
		// 終了
		return;
	};
	VisitCmpPairs(Syntax, Node, InspectPair);
	// 終了
	return;
}

/**
 * １の増減を増減演算子で書く言語の増分代入の検出関数
 * 計算量：数値リテラルの照会と診断生成の総費用 Q に対し O(Q)
 * @param Node 対象ノード
 * @param Output 警告格納先
 * @throws std::bad_alloc 構文の照会又は診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void LintAnalyzer::CheckCompoundAssignIncrement(const NodeId Node, std::vector<LintWarning> &Output) {
	const NodeId Rhs = UnwrapGroup(Syntax.GetNamedChild(Node, 1));
	// 値が１の数値リテラルを持たない式の終了
	if(!IsNumericVal(Rhs, 1)) return;
	const std::string_view Operator = FindUnnamedToken(Node, Rules.Operators.AddAssign, Rules.Operators.SubtractAssign);
	// 対象の演算子を持たない式の終了
	if(Operator.empty()) return;
	Push(
		GetTokenChildOf(Node, Operator),
		LintRule::UnitAssignment,
		"Review: Use the increment or decrement form required for this target",
		Output
	);
	// 終了
	return;
}

/**
 * 結果未使用の後置増減候補検出関数
 * 計算量：前置化の文脈の照会と診断生成の総費用 Q に対し O(Q)
 * @param Node 対象更新式
 * @param Output 警告格納先
 * @throws std::bad_alloc 診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void LintAnalyzer::CheckUnusedPostfixIncrement(const NodeId Node, std::vector<LintWarning> &Output) {
	// 被演算子の取得
	const NodeId Operand = Syntax.GetNamedChild(Node, 0);
	// 被演算子が無い場合の終了
	if(Operand == NoNode) return;
	// 後置演算子の探索
	std::string_view Operator;
	NodeId OperatorNode = NoNode;
	uint32_t OperatorStart = 0;
	Core::SyntaxModel::ForEachChildOf(
		Syntax,
		Node,
		[&](const NodeId Child) -> bool {
			// 名前付の子を読み飛ばす事の返戻
			if(Syntax.IsNamed(Child)) return true;
			// 名前無トークンの抽出
			const std::string_view Token = GetNodeView(Child);
			// 増減以外のトークンを読み飛ばす事の返戻
			if(Token != Rules.Operators.Increment && Token != Rules.Operators.Decrement) return true;
			// 演算子と開始位置の記録
			Operator = Token;
			OperatorNode = Child;
			OperatorStart = GetBeginOf(Child);
			// 更新演算子発見で走査打切の返戻
			return false;
		}
	);
	// 後置の演算子が無いか，式の値を使用する場合の終了
	if(Operator.empty() || OperatorStart < GetEndOf(Operand) || !HasUnusedVal(Node)) return;
	Push(OperatorNode, LintRule::UnusedPostfix, "Review: Use prefix increment or decrement when the old value is unused", Output);
	// 終了
	return;
}

/**
 * 規約の採用形式でないドキュメントの検出関数（整形仕様 8.4 のドキュメントの形式）
 * 本文の解釈が異なる為に変換しない行形式のドキュメントを，連続する行の最初で１度報告する
 * 計算量：ドキュメントと項目の照会・診断生成の総費用 Q に対し O(Q)
 * @param Idx 最終テキストのコメントの原文順の番号
 * @param Output 警告の格納先
 * @throws std::bad_alloc ドキュメントの照会又は診断の確保に失敗した場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
void LintAnalyzer::CheckDocCommentForm(const size_t Idx, std::vector<LintWarning> &Output) {
	const CommentRecord &Record = Repository.GetRecords()[Facts.Comments[Idx].Record];
	// 説明ドキュメントでない場合の終了
	if(!DocCommentReader::IsDoc(Record)) return;
	const bool IsInner = Rules.Notation.IsInnerDoc(Core::CommentRepository::GetOutputNotationOf(Record));
	const Core::CommentOutputForm *const Form = Rules.Notation.GetAdoptedFormOf(Core::CommentUse::Doc, IsInner);
	// 固定した採用形を持たない記法の場合の終了
	if(!Form || Rules.DocForm.empty()) return;
	const std::string_view Text = Src;
	const auto NotationAt = [this](const size_t At) -> CommentNotation {
		// 記録の出力記法の返戻
		return Core::CommentRepository::GetOutputNotationOf(Repository.GetRecords()[Facts.Comments[At].Record]);
	};
	const CommentNotation Notation = NotationAt(Idx);
	// 採用する記法に既に一致するドキュメントの終了
	if(Notation == Form->Notation) return;
	const StaticLint::OutputRange &Range = Facts.Comments[Idx].Range;
	if(Idx && NotationAt(Idx - 1) == Notation) {
		// 前の行の始まりから此の行の始まり迄と，其の間の空白
		const StaticLint::OutputRange &Prev = Facts.Comments[Idx - 1].Range;
		// 前の行と改行１つだけを挟んで続く場合の終了
		if(
			const std::string_view Span = Text.substr(Prev.Start, Range.Start - Prev.Start),
			Gap = Text.substr(Prev.End, Range.Start - std::min(Prev.End, Range.Start));
			Gap.find_first_not_of(" \t\r\n") == std::string_view::npos && std::count(Span.begin(), Span.end(), '\n') == 1
		) return;
	}
	Push(
		Range.Point,
		LintRule::DocumentationNotation,
		IsInner ?
		"Violation: Use inner Rust block documentation for this documentation" :
		"Violation: Use " + std::string(Rules.DocForm) + " for this documentation",
		Output,
		{ IsInner ? "inner Rust block documentation" : std::string(Rules.DocForm) }
	);
	// 終了
	return;
}

/**
 * ========== 検査実行 ==========
 * 違反警告リスト返戻関数
 * 計算量：ノード数 V，最大祖先数 D，ソース長 N，コメント数 K，個別検査の総費用 F に対し O(V D log(V + 1) + N + K log K + F)
 * @return 検出された違反警告リスト（検査の順）
 * @throws std::bad_alloc 検査の事実・照会又は診断の確保に失敗した場合
 * @throws FormatLimitExceeded 構文・制御の照会が作業予算を超えた場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::logic_error 呼出先で構文・所有関係の内部契約が成立しない場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
std::vector<LintWarning> LintAnalyzer::Run() {
	// 出力の格納先
	std::vector<LintWarning> Output;
	const NodeId Root = Syntax.GetRoot();
	// 空の構文に対する空の警告列の返戻
	if(Root == NoNode) return Output;
	// 整形済のインデントの深さの検査
	CheckNestingDepth(Output);
	const std::span<const uint32_t> DirectiveStarts = Facts.DirectiveStarts;
	// 最終テキストのコメントの範囲（原文順）
	std::vector<std::pair<uint32_t, uint32_t>> Comments;
	Comments.reserve(Facts.Comments.size());
	for(const StaticLint::CommentFacts &Comment : Facts.Comments) Comments.emplace_back(Comment.Range.Start, Comment.Range.End);
	// 宣言子の読取で同じ制御の文脈を共有する
	BindingRefs.resize(Syntax.GetNodeCount(), 0);
	// 未評価の参照を含む共有の束縛事実の計数
	CollectBindingRefs();
	// 関数全体で束縛される名前は，型指定・仮引数・外側の束縛を１度の前進走査で共有する
	std::vector<uint8_t> TypeHintTargets;
	if(Rules.HasFunctionLocalBindings) {
		using TypeHintBinding = std::pair<NodeId, bool>; // 最初の型検査対象と明示型又は仮引数の有無
		std::unordered_map<NodeId, TypeHintBinding> Hints;
		TypeHintTargets.resize(Syntax.GetNodeCount());
		for(const std::pair<const NodeId, FunctionFacts> &Function : Facts.Functions) {
			for(const Core::ParamShape &Param : Function.second.Signature.ParamShapes) for(const NodeId Name : Param.Bindings) {
				const Core::BindingFact Binding = Syntax.GetBindingFact(Name);
				if(Binding.Decl != NoNode) Hints[Binding.Decl] = { NoNode, true };
			}
		}
		for(const NodeId Node : Facts.Nodes) {
			if(Syntax.Is(Node, Role::OptionalDeclType)) {
				const NodeId Name = Syntax.GetFieldChild(Node, ChildRole::Left);
				const Core::BindingFact Binding = Syntax.GetBindingFact(Name);
				if(Binding.Decl == NoNode) TypeHintTargets[Node] = 1;
				else {
					const bool HasType = HasExplicitTypeOf(Node);
					const std::pair<std::unordered_map<NodeId, TypeHintBinding>::iterator, bool> Entry =
					Hints.try_emplace(Binding.Decl, Node, HasType);
					Entry.first->second.second |= HasType;
				}
			} else if(Syntax.Is(Node, Role::TypeHintExternalBinding)) for(const NodeId Name : Syntax.GetChildrenOf(Node)) {
				const Core::BindingFact Binding = Syntax.GetBindingFact(Name);
				if(Binding.Decl != NoNode) Hints[Binding.Decl] = { NoNode, true };
			}
		}
		for(const std::pair<const NodeId, TypeHintBinding> &Hint : Hints) if(!Hint.second.second && Hint.second.first != NoNode) {
			TypeHintTargets[Hint.second.first] = 1;
		}
	}
	LabelTargetIdx Labels;
	// ノードの役割別の規約検査
	for(const NodeId Node : Facts.Nodes) {
		// 名前の無いトークンは次へ進む
		if(!Syntax.IsNamed(Node)) continue;
		const Core::KindDef *const Def = Syntax.GetNode(Node).Kind.Def;
		// 同じノードの固定の役割を読み，ノード表への引直をしない
		const auto HasRole = [Def](const Role Wanted) -> bool {
			// 固定の種別が対象の役割を持つかの返戻
			return Def && Def->Roles.test(static_cast<size_t>(Wanted));
		};
		if(HasRole(Role::MemberOrderBody)) CheckMemberDeclOrder(Node, Output);
		if(HasRole(Role::OrdinaryFunctionDef)) CheckMissingExplicitReturn(Node, Output);
		if(
			HasRole(Role::JumpWithLabel) && Core::ControlFlow::IsReturn(Syntax, Node) &&
			Syntax.GetFirstNamedChildWithRole(Node, Role::LabelInspection) != NoNode
		) Push(Node, LintRule::JumpUsage, "Review: Replace this jump with structured control flow", Output);
		if(HasRole(Role::OptionalDeclType) && (TypeHintTargets.empty() || TypeHintTargets[Node])) CheckDeclType(Node, Output);
		if(Rules.HasNumericExitDepth && HasRole(Role::LoopExitStmt)) CheckNumericJump(Node, Output);
		if(HasRole(Role::ParamContainer) || HasRole(Role::TypeHintParamContainer)) CheckParamTypes(Node, Output);
		if(HasRole(Role::LambdaExpr) || HasRole(Role::ArrowFunctionNode)) {
			const NodeId Param = Syntax.GetFieldChild(Node, ChildRole::Params);
			if(Syntax.Is(Param, Role::OptionalParamType)) CheckParamType(Param, 1, Output);
		}
		if(HasRole(Role::AccessOrderedClass)) CheckAccessSpecifierOrder(Node, Output);
		else if(HasRole(Role::GotoInspection) || HasRole(Role::LabelInspection)) CheckGotoOrLabel(Node, Labels, Output);
		else if(HasRole(Role::StmtFlowContainer) || HasRole(Role::StmtSeq)) CheckUselessLocalVar(Node, Output);
		else if(HasRole(Role::TypeInferenceInspection)) CheckTypeInferenceKeyword(Node, Output);
		else if(HasRole(Role::NamedFunctionDef) || HasRole(Role::ReturnHintRequired)) {
			if(HasRole(Role::ReturnHintRequired)) CheckReturnTypeHint(Node, Output);
		} else if(HasRole(Role::MacroConstantInspection)) CheckMacroConstant(Node, Output);
		else if(HasRole(Role::ScopeImport)) CheckUsingNamespace(Node, Output);
		else if(Core::ControlFlow::IsReturn(Syntax, Node)) CheckReturnCommentPresence(Node, Comments, Output);
		else if(HasRole(Role::BinaryExpr)) {
			if(Rules.HasZeroCmp) CheckZeroCmp(Node, Output);
			if(HasRole(Role::StrictEqualityInspection)) CheckNullishCmpCandidate(Node, Output);
		} else if(HasRole(Role::CastInspection)) CheckCStyleCast(Node, Output);
		else if(HasRole(Role::CondMergeInspection)) CheckUnsafeIfMergeCandidate(Node, Output);
		else if(HasRole(Role::BoolFlipInspection)) CheckBoolFlipPattern(Node, Output);
		if(Rules.Notation.Doc != Core::DocStyle::Docstring && HasRole(Role::DocumentedDef)) {
			const StaticLint::FunctionFacts &Function = Facts.Functions.at(Node);
			const bool IsTarget = Function.IsDocumentable;
			// 項目の検査に用いるアンカーの手前の本文（ドキュメンテーション文字列の言語と項目を検査しない解析失敗の定義では集めない）
			const std::vector<std::string_view> Leading =
			IsTarget || !HasError(Node) ? LeadingCommentTexts(Function.DocAnchor, DirectiveStarts) : std::vector<std::string_view>();
			if(IsTarget) CheckDocCommentPresence(Node, Output);
			CheckDocCommentCompleteness(Node, Leading, Output);
		}
		if(Rules.HasReturnLabels && Syntax.HasWordChild(Node, Word::LabeledReturn)) {
			Push(Node, LintRule::JumpUsage, "Review: Replace this jump with structured control flow", Output);
		}
		// 数値演算の候補検査
		if(Rules.HasBoundaryCmp && HasRole(Role::BinaryExpr)) CheckCmpBoundary(Node, Output);
		if(Rules.ShouldPreferUnitUpdate && HasRole(Role::IncrementInspection)) CheckCompoundAssignIncrement(Node, Output);
		if(Rules.ShouldPreferPrefixUpdate && HasRole(Role::PostfixInspection)) CheckUnusedPostfixIncrement(Node, Output);
	}
	// 最終テキストのドキュメントの記法の検査（原文順）
	if(Rules.Notation.Doc != Core::DocStyle::Docstring) for(size_t Idx = 0; Idx < Facts.Comments.size(); ++Idx) {
		CheckDocCommentForm(Idx, Output);
	}
	// 検査の順の警告（位置順の整列は部分整形の通知と合わせてフォーマッターが行う）の返戻
	return Output;
}

/**
 * 検証済の出力の規約検査関数（整形仕様 3.4 の Analyze，8.4）
 * 同じ完成候補の静的解析専用の事実を確定してから読み，解析・投影し直さずに規約違反を求める
 * 計算量：ノード数 N，文面のバイト数 B，コメント数 C，シグネチャ・到達可能性・個別検査の総費用 F に対し O(N + B + C log C + F)
 * @param Output 検証を通った最終構文・コメント・診断位置（静的解析専用の事実だけを書き足し，生成処理は空にする）
 * @return 検査の順の違反警告リスト（位置順の整列は部分整形の通知と合わせてフォーマッターが行う，整形仕様 8.4，10.4）
 * @throws FormatLimitExceeded 構文の照会・修復又は整形の作業予算を超えた場合
 * @throws std::bad_alloc 処理又は呼出先のメモリを確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
std::vector<LintWarning> LintAnalyzer::Analyze(Core::ValidatedOutput &Output) {
	// 生成処理の所有を取り出し，呼出元の保留を空にする
	if(const Core::LintPreparation Prepare = std::exchange(Output.PrepareFacts, Core::LintPreparation()); Prepare) {
		Prepare(Output.FinalSyntax, Output.FinalComments, Output.Locations);
	}
	// 確定した検査データからの検査結果の返戻
	return LintAnalyzer(Output.FinalSyntax, Output.Locations.Text, Output.FinalComments, ReadFacts(Output)).Run();
}
