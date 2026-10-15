#include "SyntaxGeneration.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 文法が生成するトークンの取得関数
	 * 計算量：生成形式の数 F と生成トークンのバイト数 B に対し O(F + B)
	 * @param Syntax 構文モデル
	 * @param Operation 生成する操作
	 * @param Role トークンの部品の役割
	 * @return 対応するトークン
	 * @throws FormatLimitExceeded 生成形式又は部品が無い場合，又はトークンのコピーが作業予算を超えた場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	std::string SyntaxGeneration::GetGeneratedFixed(
		const SyntaxModel &Syntax,
		const FormOperation Operation,
		const FormPartRole Role
	) {
		// 文法が生成するトークンの取得
		const std::vector<CanonicalForm> &Forms = Syntax.GetRules().GeneratedForms;
		const std::vector<CanonicalForm>::const_iterator Found = std::ranges::find_if(
			Forms,
			[Operation](const CanonicalForm &Form) -> bool {
				// 操作の一致の返戻
				return Form.Operation == Operation;
			}
		);
		const CanonicalForm *const Form = Found == Forms.end() ? nullptr : &*Found;
		const FormPart *const Part = Form ? Form->GetPartOf(Role) : nullptr;
		if(!Part) throw FormatLimitExceeded("a generated lexeme has no grammar form part", FailureReason::FormatterDefect);
		Syntax.Charge(Part->FixedLexeme.size());
		// 形式に定めたトークンの返戻
		return std::string(Part->FixedLexeme);
	}

	/**
	 * 本体を波括弧で包む生成形式の書換関数
	 * 正規化と出力検証が同じ文法定義から，選択した本体の構造を作る
	 * 計算量：本体の直下の子の数 N に対し O(N)
	 * @param Syntax 書換前の構文モデル
	 * @param Body 包む本体
	 * @param Form 所属する制御構文の生成形式
	 * @return 波括弧を付ける書換操作
	 * @throws FormatLimitExceeded 本体を包めない生成形式，又は必要な部品・部品の定義が無い場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	RewriteStep SyntaxGeneration::BraceBodyStep(const SyntaxModel &Syntax, const NodeId Body, const CanonicalForm &Form) {
		// ラッパーの可否と必要な生成部品の確認
		if(!Form.CanWrapBody(Syntax.IsBracedBody(Body))) {
			throw FormatLimitExceeded("the brace form cannot wrap this body", FailureReason::FormatterDefect);
		}
		if(!Form.GetPartOf(FormPartRole::Body)) {
			throw FormatLimitExceeded("the brace form lacks its body role", FailureReason::FormatterDefect);
		}
		// 生成形式の指定した部品の組立
		const auto Part = [&Form](const FormPartRole Role, std::vector<GeneratedNode> &&Children) -> GeneratedNode {
			const FormPart *const Found = Form.GetPartOf(Role);
			if(!Found || !Found->Def) throw FormatLimitExceeded("the brace form lacks a required part", FailureReason::FormatterDefect);
			// 部品の種別の生成ノードの返戻
			return { {}, Found->Def->IsNamed, std::move(Children), 0, NoNode, std::string(Found->FixedLexeme), *Found };
		};
		// 既存の本体のラッパーの再構成，又は単一の文を包む新しいブロックの生成
		if(Syntax.Is(Body, Role::BodyWrapper)) {
			std::vector<GeneratedNode> Stmts;
			for(const NodeId Child : Syntax.GetChildrenOf(Body)) Stmts.push_back({ {}, false, {}, Syntax.GetNode(Child).Fields, Child });
			RewriteStep Step { RewriteKind::Reshape, Body };
			Step.Inserted = Part(
				FormPartRole::Block,
				{ Part(FormPartRole::Open, {}), Part(FormPartRole::Stmts, std::move(Stmts)), Part(FormPartRole::Close, {}) }
			);
			// 本体の包みを組み直す操作の返戻
			return Step;
		}
		RewriteStep Step { RewriteKind::Wrap, Body };
		Step.Inserted =
		Part(FormPartRole::Block, { Part(FormPartRole::Open, {}), { {}, false, {}, {}, Body }, Part(FormPartRole::Close, {}) });
		Step.Boundary = BoundaryKind::Spaced;
		// 本体の文を新しいブロックで包む操作の返戻
		return Step;
	}

	/**
	 * 継続括弧の選択に対応する期待構文の構築関数
	 * 計算量：型ラッパーの段数 H，訪れた要素数 N と生成要素数 G に対し O(H + N + G)
	 * @param Syntax 期待構文
	 * @param Id 包む式
	 * @return 継続括弧で包む書換操作（裸のタプル・型の構造は組み直す）
	 * @throws FormatLimitExceeded 継続括弧の生成形式を持たない場合又は作業予算を超えた場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	RewriteStep SyntaxGeneration::ContinuationStep(const SyntaxModel &Syntax, const NodeId Id) {
		// 継続括弧の生成形式の確認
		Syntax.Charge(1);
		const CanonicalForm *const Form = Syntax.GetFormOf(Id, FormOperation::Continuation);
		const FormPart *const ResultPart = Form ? Form->GetPartOf(FormPartRole::Result) : nullptr;
		if(!ResultPart) throw FormatLimitExceeded("a continuation has no grammar form", FailureReason::FormatterDefect);
		const FormPart &Result = *ResultPart;
		if(Syntax.Is(Id, Role::TypeSyntax)) {
			// 式として生成する部分木の組立
			const auto Expr = [&](const NodeId Val, const uint64_t Fields) -> GeneratedNode {
				// 部分木の走査の枠
				struct Frame {
					NodeId Val;
					uint64_t Fields; // 生成後の親の中のフィールド
					GeneratedNode *Out; // 生成したノードの格納先
					size_t NextChild = 0;
					bool HasEntered = false;
				};
				GeneratedNode ExprNode;
				// 型の適用を式の添字として組み立てる走査
				std::vector<Frame> Pending { { Val, Fields, &ExprNode } };
				while(!Pending.empty()) {
					Syntax.Charge(1);
					Frame &Cur = Pending.back();
					if(!Cur.HasEntered) {
						Cur.HasEntered = true;
						while(Syntax.Is(Cur.Val, Role::TypeSyntax)) {
							Syntax.Charge(1);
							Cur.Val = Syntax.GetNamedChild(Cur.Val, 0);
						}
						if(!Syntax.Is(Cur.Val, Role::TypeApplication)) {
							*Cur.Out = { {}, false, {}, Cur.Fields, Cur.Val };
							Pending.pop_back();
							continue;
						}
						*Cur.Out = { {}, true, {}, Cur.Fields };
						Cur.Out->Def = *Form->GetPartOf(FormPartRole::Body);
					}
					NodeId Child;
					uint64_t ChildFields;
					if(!Cur.NextChild) {
						Child = Syntax.GetChild(Cur.Val, 0);
						ChildFields = uint64_t{ 1 } << static_cast<uint8_t>(ChildRole::Val);
					} else {
						const std::span<const NodeId> Args = Syntax.GetChildrenOf(Syntax.GetChild(Cur.Val, 1));
						if(Cur.NextChild > Args.size()) {
							Pending.pop_back();
							continue;
						}
						Child = Args[Cur.NextChild - 1];
						ChildFields = Syntax.IsNamed(Child) ? uint64_t{ 1 } << static_cast<uint8_t>(ChildRole::Other) : 0;
					}
					++Cur.NextChild;
					Syntax.Charge(1);
					Cur.Out->Children.emplace_back();
					Pending.push_back({ Child, ChildFields, &Cur.Out->Children.back() });
				}
				// 式の文法で表した添字の返戻
				return ExprNode;
			};
			// 外側の型・ラッパー・固定括弧の４要素の計上
			Syntax.Charge(4);
			GeneratedNode Paren { {}, true };
			Paren.Def = Result;
			Paren.Children = {
				{ GetGeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallOpen) },
				Expr(Syntax.GetNamedChild(Id, 0), 0),
				{ GetGeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallClose) }
			};
			RewriteStep Type { RewriteKind::Reshape, Id };
			Type.Inserted = { {}, true, { std::move(Paren) } };
			Type.Inserted.Def = Syntax.GetGenerationPartOf(Id);
			// 型の内側を継続括弧で包む操作の返戻
			return Type;
		}
		// 型以外の式と組のラッパー
		const bool IsList = Syntax.Is(Id, Role::BareTuple);
		// ラッパー・固定括弧の３要素の計上
		Syntax.Charge(3);
		RewriteStep Step { IsList ? RewriteKind::Reshape : RewriteKind::Wrap, Id };
		Step.Inserted = { {}, true };
		Step.Inserted.Def = Result;
		Step.Inserted.Children.push_back({ GetGeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallOpen) });
		if(IsList) for(const NodeId Child : Syntax.GetChildrenOf(Id)) {
			Syntax.Charge(1);
			Step.Inserted.Children.push_back({ {}, false, {}, Syntax.GetNode(Child).Fields, Child });
		} else {
			Syntax.Charge(1);
			Step.Inserted.Children.push_back({ {}, false, {}, 0, Id });
			// 包んだ式の外のアンカーの括弧の外への移動
			Step.Transfer.From = Id;
		}
		Step.Inserted.Children.push_back({ GetGeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallClose) });
		// 包む操作の返戻
		return Step;
	}
} // namespace Core
