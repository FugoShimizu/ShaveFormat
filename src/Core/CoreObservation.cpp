#include "CoreObservation.hpp"
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * テキストの１６進表現の取得関数
	 * 計算量：バイト数 B に対し O(B)
	 * @param Text 観測するテキスト
	 * @return 制御文字を含まない１６進のテキスト
	 * @throws std::bad_alloc テキストのメモリを確保出来ない場合
	 * @throws std::length_error テキストの長さが上限を超える場合
	 */
	std::string CoreObservation::Hex(const std::string_view Text) {
		// テキストの１６進表現の取得
		std::string Result;
		for(const unsigned char Byte : Text) {
			Result.push_back("0123456789ABCDEF"[Byte >> 4]);
			Result.push_back("0123456789ABCDEF"[Byte & 0XF]);
		}
		// テキストの１６進表現の返戻
		return Result;
	}

	/**
	 * 数値フィールドの追記関数
	 * 計算量：数値の桁数 D に対し O(D)
	 * @param Val フィールドの値
	 * @throws std::bad_alloc 観測のメモリを確保出来ない場合
	 * @throws std::length_error 観測の長さが上限を超える場合
	 */
	void CoreObservation::Append(const size_t Val) {
		// 数値フィールドの追記
		Records.push_back(' ');
		Records.append(std::to_string(Val));
		// 終了
		return;
	}

	/**
	 * テキストフィールドの追記関数
	 * 計算量：テキストのバイト数 B に対し O(B)
	 * @param Text フィールドのテキスト
	 * @throws std::bad_alloc 観測のメモリを確保出来ない場合
	 * @throws std::length_error 観測の長さが上限を超える場合
	 */
	void CoreObservation::AppendText(const std::string_view Text) {
		// テキストフィールドの追記
		Records.push_back(' ');
		Records.append(Hex(Text));
		// 終了
		return;
	}

	/**
	 * 固定の種別定義の記録と番号の取得関数
	 * 計算量：固定の役割数 R，評価位置数 E とトークン長 B に対し期待値 O(R + E + B)
	 * @param Def 記録する種別（`nullptr` なら番号０を返戻）
	 * @return 初出順の種別の番号
	 * @throws std::bad_alloc 観測又は対応表のメモリを確保出来ない場合
	 * @throws std::length_error 観測の長さが上限を超える場合
	 */
	size_t CoreObservation::ObserveKind(const KindDef *const Def) {
		// 種別の無い事の返戻
		if(!Def) return 0;
		const std::pair<std::unordered_map<const KindDef *, size_t>::iterator, bool> Entry = Kinds.try_emplace(Def, Kinds.size() + 1);
		if(Entry.second) {
			Records.append("[kind]");
			Append(Entry.first->second);
			Append(Def->IsNamed);
			Append(static_cast<size_t>(Def->Binding));
			AppendText(Def->Roles.to_string());
			Append(static_cast<size_t>(Def->Bindings.Namespace));
			Append(static_cast<size_t>(Def->Bindings.Params));
			Append(static_cast<size_t>(Def->Bindings.PatternRef));
			Append(static_cast<size_t>(Def->Bindings.Constructor));
			Append(Def->Bindings.IsStaticItem);
			Append(Def->Bindings.ShouldRequireImmutableConstantSrc);
			const EvaluationDef &Evaluation = Def->Evaluation;
			Append(static_cast<size_t>(Evaluation.Rule));
			Append(static_cast<size_t>(Evaluation.Children));
			Append(static_cast<size_t>(Evaluation.Invocation));
			Append(static_cast<size_t>(Evaluation.FirstInput));
			Append(static_cast<size_t>(Evaluation.CatchInput));
			Append(static_cast<size_t>(Evaluation.ConditionField));
			Append(Evaluation.ConditionIndex.has_value());
			if(Evaluation.ConditionIndex) Append(*Evaluation.ConditionIndex);
			AppendText(Evaluation.UnevaluatedHead);
			AppendText(Evaluation.UnknownHead);
			Append(Evaluation.HasDeferredArguments);
			Append(Evaluation.DoesEvaluateChildInputs);
			Append(Evaluation.HasEntry);
			Append(Evaluation.IsContainer);
			Append(Evaluation.IsOrdered);
			Append(Evaluation.MaySkipInputs);
			Append(Evaluation.IsRequiredFirst);
			Append(static_cast<size_t>(Evaluation.ThrowBeforeField));
			Append(Evaluation.ConditionalOperators.size());
			for(const std::string_view Operator : Evaluation.ConditionalOperators) AppendText(Operator);
			Append(Evaluation.Inputs.size());
			Records.push_back('\n');
			for(const EvaluationInput &Input : Evaluation.Inputs) {
				Records.append("[evaluation-input]");
				Append(static_cast<size_t>(Input.Field));
				Append(Input.NamedIndex);
				Append(static_cast<size_t>(Input.Class));
				Append(static_cast<size_t>(Input.WhenPresent));
				Append(Input.ChildKind.has_value());
				if(Input.ChildKind) Append(static_cast<size_t>(*Input.ChildKind));
				AppendText(Input.WhenToken);
				AppendText(Input.AfterToken);
				Append(Input.UnlessKind.has_value());
				if(Input.UnlessKind) Append(static_cast<size_t>(*Input.UnlessKind));
				Append(Input.WhenSiblingKind.has_value());
				if(Input.WhenSiblingKind) Append(static_cast<size_t>(*Input.WhenSiblingKind));
				Records.push_back('\n');
			}
		}
		// 初出順の種別の番号の返戻
		return Entry.first->second;
	}

	/**
	 * 生成形式列の記録と番号の取得関数
	 * 計算量：新たな形式数 F，部品数 P と固定トークン長 B に対し期待値 O(F + P + B)
	 * @param List 事実が参照する生成形式列
	 * @return 初出順の形式列の番号（空は０）
	 * @throws std::bad_alloc 観測又は対応表のメモリを確保出来ない場合
	 * @throws std::length_error 観測の長さが上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	size_t CoreObservation::ObserveForms(const CanonicalFormList &List) {
		// 空の形式列の返戻
		if(List.GetView().empty()) return 0;
		std::vector<CanonicalFormList> Pending;
		// 共有する生成形式の照合用の番号の取得
		const auto Num = [&](const CanonicalFormList &Input) -> size_t {
			// 空の形式列の返戻
			if(Input.GetView().empty()) return 0;
			const std::pair<std::unordered_map<const CanonicalForm *, size_t>::iterator, bool> Entry =
			Forms.try_emplace(Input.GetView().data(), Forms.size() + 1);
			if(Entry.second) Pending.push_back(Input);
			// 形式列の番号の返戻
			return Entry.first->second;
		};
		const size_t Result = Num(List);
		for(size_t Idx = 0; Idx < Pending.size(); ++Idx) {
			const CanonicalFormList Cur = Pending[Idx];
			Records.append("[forms]");
			Append(Forms.at(Cur.GetView().data()));
			Append(Cur.GetView().size());
			Records.push_back('\n');
			for(const CanonicalForm &Form : Cur.GetView()) {
				Records.append("[form]");
				Append(static_cast<size_t>(Form.Operation));
				Append(static_cast<size_t>(Form.Cond));
				AppendText(Form.FixedLexeme);
				AppendText(Form.ClosingLexemes);
				AppendText(Form.ApplicableRoles.to_string());
				Append(Form.Parts.size());
				Records.push_back('\n');
				for(const FormPart &Part : Form.Parts) {
					const size_t Kind = ObserveKind(Part.Def), TokenKind = ObserveKind(Part.TokenDef);
					Records.append("[part]");
					Append(static_cast<size_t>(Part.Role));
					Append(Kind);
					Append(TokenKind);
					Append(static_cast<size_t>(Part.Parent));
					Append(static_cast<size_t>(Part.Field));
					Append(Part.IsNamed);
					Append(Part.BlankKey);
					Append(Num(Part.Forms));
					AppendText(Part.FixedLexeme);
					AppendText(Part.Meanings.to_string());
					Records.push_back('\n');
				}
			}
		}
		// 形式列の番号の返戻
		return Result;
	}

	/**
	 * 正規化後の前順番号の取得関数
	 * 計算量：期待値 O(1)
	 * @param Node アンカー又はノード（無も可）
	 * @return 前順番号，対応が無ければ０
	 */
	size_t CoreObservation::GetPos(const NodeId Node) const {
		// 正規化後の前順番号の取得
		const std::unordered_map<NodeId, size_t>::const_iterator Found = Positions.find(Node);
		// 前順番号の返戻
		return Found == Positions.end() ? 0 : Found->second;
	}

	/**
	 * ノードの構文事実の記録関数
	 * 計算量：新たな生成形式の記録量 F，境界数 C とテキスト長 B に対し O(F + C + B)
	 * @param Syntax ノードの比較用の名前を持つ構文
	 * @param Id 現存するノードの識別子
	 * @throws std::bad_alloc 観測のメモリを確保出来ない場合
	 * @throws std::length_error 観測の長さが上限を超える場合
	 * @throws FormatLimitExceeded 比較用の名前が未投影の場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void CoreObservation::ObserveFacts(const SyntaxModel &Syntax, const NodeId Id) {
		// ノードの構文事実の記録
		const SyntaxFacts &Facts = Syntax.GetNode(Id).Facts;
		const size_t FormNum = ObserveForms(Facts.Forms), DelimiterNum = ObserveKind(Facts.DelimiterDef);
		const size_t TokenNum = ObserveKind(Facts.TokenDef);
		Records.append("[facts]");
		AppendText(Facts.Meanings.to_string());
		AppendText(Facts.ResultMeanings.to_string());
		Append(Facts.CallEffects);
		Append(Facts.IsRegisteredMacro);
		Append(Facts.CannotResolveTerminalMember);
		Append(Facts.Reference != nullptr);
		if(Facts.Reference) {
			AppendText(Facts.Reference->Name);
			AppendText(Facts.Reference->Meanings.to_string());
			Append(Facts.Reference->IsUnknown);
			AppendText(Facts.Reference->UnboundMeanings.to_string());
			Append(GetPos(Facts.Reference->BindingHead));
			Append(GetPos(Facts.Reference->BindingDecl));
			AppendText(Facts.Reference->BindingMember);
			Append(Facts.Reference->IsGlobal);
		}
		Append(Facts.Access.Mask);
		Append(Facts.Access.Length);
		Append(static_cast<size_t>(Facts.Access.ExactRank + 1));
		Append(Facts.HasBuiltinCastType);
		Append(static_cast<size_t>(Facts.NumericLiteral));
		Append(Facts.BlankKey);
		Append(Facts.Constraints);
		Append(Facts.DoesDiscardBinding);
		Append(Facts.CanReassignExistingBinding);
		Append(Facts.HasFalseName);
		Append(Facts.HasTrueName);
		Append(Facts.DoesAbsorbLeadingComment);
		Append(Facts.NeedsAngleParens);
		Append(Facts.DoesObserveText);
		Append(Facts.DoesRequireInlineCode);
		Append(Facts.DoesPreserveQuoteForm);
		Append(TokenNum);
		Append(DelimiterNum);
		Append(Facts.ImportScope != NoNode);
		Append(GetPos(Facts.ImportScope));
		Append(Facts.Spelling.has_value());
		if(Facts.Spelling) {
			Append(Facts.Spelling->HasNonAscii);
			Append(Facts.Spelling->HasEscape);
		}
		AppendText(Facts.NameIdx ? Syntax.ReadCmpNameView(Id) : std::string_view());
		Append(static_cast<bool>(Facts.RegionIdx));
		Append(FormNum);
		Append(Facts.Boundaries.GetView().size());
		Records.push_back('\n');
		for(const SemanticBoundary &Boundary : Facts.Boundaries.GetView()) {
			Records.append("[boundary]");
			Append(Boundary.Before);
			Append(static_cast<size_t>(Boundary.Kind));
			Append(static_cast<size_t>(Boundary.Src));
			AppendText(Boundary.Gap);
			Records.push_back('\n');
		}
		// 終了
		return;
	}

	/**
	 * 投影時の前順を記録するコンストラクタ
	 * 計算量：投影時のノード数 N に対し O(N)
	 * @param Syntax 書換前の投影した構文
	 * @throws std::bad_alloc 前順列又は走査列のメモリを確保出来ない場合
	 * @throws std::length_error 前順列又は走査列の長さが上限を超える場合
	 * @throws std::out_of_range 読取るノードがノード表に無い場合
	 */
	CoreObservation::CoreObservation(const SyntaxModel &Syntax) {
		// 投影時の前順を記録するコンストラクタ
		for(std::vector<NodeId> Pending { Syntax.GetRoot() }; !Pending.empty();) {
			const NodeId Node = Pending.back();
			Pending.pop_back();
			Projected.push_back(Node);
			const std::span<const NodeId> Children = Syntax.GetChildrenOf(Node);
			Pending.insert(Pending.end(), Children.rbegin(), Children.rend());
		}
		// 終了
		return;
	}

	/**
	 * 正規化した構文とアンカーの対応の記録関数
	 * 計算量：ノード数 N，テキストの総バイト数 B に対し期待値 O(N + B)
	 * @param Syntax 正規化後の構文
	 * @param Trace 合成済のアンカーの移譲
	 * @throws std::bad_alloc 観測又は走査列のメモリを確保出来ない場合
	 * @throws std::length_error 観測又は走査列の長さが上限を超える場合
	 * @throws std::out_of_range 読取るノードがノード表に無い場合
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 */
	void CoreObservation::ObserveSyntax(const SyntaxModel &Syntax, const RewriteTrace &Trace) {
		// 正規化した構文とアンカーの対応の記録
		std::vector<std::pair<NodeId, size_t>> Pending { { Syntax.GetRoot(), 0 } }, Order;
		// 後方を参照する導入先も生の番号に依存せず記録する為に，構造上の対応を先に確定する
		while(!Pending.empty()) {
			const std::pair<NodeId, size_t> Cur = Pending.back();
			Pending.pop_back();
			const SyntaxNode &Node = Syntax.GetNode(Cur.first);
			Positions.emplace(Cur.first, Positions.size() + 1);
			Normalized.push_back(Cur.first);
			Order.push_back(Cur);
			for(size_t Idx = Node.Children.size(); Idx--;) Pending.emplace_back(Node.Children[Idx], Cur.second + 1);
		}
		for(const std::pair<NodeId, size_t> &Cur : Order) {
			const SyntaxNode &Node = Syntax.GetNode(Cur.first);
			// 初めて現れた種別の定義（以後のノードは番号で参照する）
			const size_t Kind = ObserveKind(Node.Kind.Def);
			// コメント配置が使う生成由来のマーカーは，中核の構造と構文事実の比較に含めない
			Records.append("[syntax]");
			Append(Cur.second);
			Append(Kind);
			Append(Node.Fields);
			AppendText(Node.Children.empty() ? std::string_view(Node.Lexeme) : std::string_view());
			Records.push_back('\n');
			ObserveFacts(Syntax, Cur.first);
		}
		// コメントの有無と独立して，投影したアンカーの内，移した物とノードを失った物の各置方を記録する（其の他は同じノードの同じ置方）
		for(size_t Idx = 0; Idx < Projected.size(); ++Idx) {
			for(const AnchorSide Side : { AnchorSide::Before, AnchorSide::After, AnchorSide::Inside }) {
				for(const bool IsInline : { false, true }) {
					const std::unordered_map<uint64_t, AnchorMove>::const_iterator Found =
					Trace.Moves.find(RewriteTrace::GetMoveKey(Projected[Idx], Side, IsInline));
					if(Found == Trace.Moves.end() && GetPos(Projected[Idx])) continue;
					const AnchorMove Move = Found == Trace.Moves.end() ? AnchorMove{ Projected[Idx], Side } : Found->second;
					Records.append("[anchors]");
					Append(Idx + 1);
					Append(static_cast<size_t>(Side));
					Append(IsInline);
					Append(GetPos(Move.Node));
					Append(static_cast<size_t>(Move.Side));
					Append(Move.IsEmptyBody);
					Append(Move.IsRemovedCaseBody);
					Append(static_cast<size_t>(Move.Reset));
					Records.push_back('\n');
				}
			}
		}
		// 終了
		return;
	}

	/**
	 * 観測の所有権の受渡関数
	 * 計算量：O(1)
	 * @return 記録した観測の行
	 */
	std::string CoreObservation::Take() {
		// 観測の所有権の返戻
		return std::move(Records);
	}

	/**
	 * 幅非依存の中間表現の記録関数
	 * 計算量：要素数 I，子リンク数 L，テキストのバイト数 B に対し O(I + L + B)
	 * @param IR 行分割前の中間表現
	 * @throws std::bad_alloc 観測のメモリを確保出来ない場合
	 * @throws std::length_error 観測の長さが上限を超える場合
	 * @throws std::out_of_range 属性又はテキストの範囲が不正な場合
	 */
	void CoreObservation::ObserveFormat(const FormatIR &IR) {
		// 幅非依存の中間表現の記録
		Records.append("[format-root]");
		Append(IR.Root);
		Records.push_back('\n');
		for(const FormatItem &Item : IR.Items) {
			Records.append("[format]");
			Append(static_cast<size_t>(Item.Kind));
			Append(static_cast<size_t>(Item.Generated));
			Append(Item.Operation ? static_cast<size_t>(*Item.Operation) + 1 : 0);
			Append(Item.Group);
			Append(GetPos(Item.Node));
			AppendText(IR.GetTextOf(Item));
			Append(Item.Children.Size);
			for(const FormatItemId Child : IR.GetChildrenOf(Item)) Append(Child);
			if(Item.Detail != NoDetail) {
				const FormatItemDetail &Detail = IR.GetDetailOf(Item.Detail);
				Append(Detail.ExpansionDependency);
				Append(GetPos(Detail.PunctuationOwner));
				Append(GetPos(Detail.DeclOwner));
				Append(Detail.DeclRow);
				Append(GetPos(Detail.Before));
				Append(GetPos(Detail.After));
				Append(Detail.IsForced);
			}
			Records.push_back('\n');
		}
		// 終了
		return;
	}

	/**
	 * 基準順位の選択状態と解決済要素の記録関数
	 * 計算量：要素数 I，テキストの総バイト数 B，選択に属するノード・操作数 S に対し期待値 O(I + B + S)
	 * @param IR 行分割後の中間表現
	 * @throws std::bad_alloc 観測のメモリを確保出来ない場合
	 * @throws std::length_error 観測の長さが上限を超える場合
	 * @throws std::out_of_range テキストの範囲が不正な場合
	 */
	void CoreObservation::ObserveBroken(const BrokenIR &IR) {
		// 基準順位の選択状態と解決済要素の記録
		for(size_t Rank = 0; Rank < IR.Expectation.SelectedForms.size(); ++Rank) {
			const SelectedForm &Form = IR.Expectation.SelectedForms[Rank];
			Records.append("[selection]");
			Append(Rank);
			Append(Form.IsBroken);
			Append(Form.Nodes.size());
			for(const NodeId Node : Form.Nodes) Append(GetPos(Node));
			Append(Form.Effects.size());
			for(const SelectedFormEffect &Effect : Form.Effects) {
				Append(GetPos(Effect.Node));
				Append(static_cast<size_t>(Effect.Operation));
				Append(GetPos(Effect.DeclOwner));
				Append(Effect.DeclRow);
			}
			Records.push_back('\n');
		}
		for(const DeclPartition &Partition : IR.Expectation.DeclarationPartitions) {
			Records.append("[partition]");
			Append(GetPos(Partition.Node));
			for(const DeclaratorRange &Row : Partition.Rows) {
				Append(Row.Begin);
				Append(Row.End);
			}
			Records.push_back('\n');
		}
		for(const ResolvedItem &Item : IR.Items) {
			Records.append("[broken]");
			Append(static_cast<size_t>(Item.Kind));
			Append(Item.IsForced);
			Append(GetPos(Item.Node));
			Append(GetPos(Item.Before));
			Append(GetPos(Item.After));
			Append(static_cast<size_t>(Item.Generated));
			AppendText(IR.GetTextOf(Item));
			Records.push_back('\n');
		}
		// 終了
		return;
	}

	/**
	 * 確定したコードの配置計画の記録関数
	 * 計算量：所有領域を含む断片数 F，テキストのバイト数 B，アンカー・トークン片・区切数 N に対し期待値 O(F + B + N)
	 * @param Code コメントを配置する前の変更不能なコード
	 * @throws std::bad_alloc 観測のメモリを確保出来ない場合
	 * @throws std::length_error 観測の長さが上限を超える場合
	 */
	void CoreObservation::ObserveLayout(const LaidOutCode &Code) {
		// 確定したコードの配置計画の記録
		for(const CodeFragment &Fragment : Code.GetFragments()) {
			Records.append("[laidout]");
			Append(Fragment.Start);
			Append(Fragment.End);
			Append(Fragment.IsBlank);
			Records.push_back(' ');
			for(uint32_t Idx = 0; Idx < Fragment.PrefixTabs; ++Idx) Records.append("09");
			Records.append(Hex(Fragment.Text));
			Records.push_back('\n');
		}
		const AnchorPositionMap &Anchors = Code.GetAnchors();
		for(const NodeId Node : Normalized) {
			if(Node >= Anchors.Nodes.size()) continue;
			const NodeSpan &Span = Anchors.Nodes[Node];
			Records.append("[placement]");
			Append(GetPos(Node));
			Append(Span.StartFragment);
			Append(Span.Start);
			Append(Span.EndFragment);
			Append(Span.End);
			Append(Span.IsPlaced);
			Records.push_back('\n');
		}
		for(size_t Idx = 0; Idx < Anchors.FragmentTabs.size(); ++Idx) {
			Records.append("[fragment]");
			Append(Idx);
			Append(Anchors.FragmentTabs[Idx]);
			Append(Idx < Anchors.VerbatimBreaks.size() ? Anchors.VerbatimBreaks[Idx] : 0);
			Records.push_back('\n');
		}
		for(const CodePiece &Piece : Anchors.Pieces) {
			Records.append("[piece]");
			Append(Piece.Start);
			Append(Piece.End);
			Append(Piece.Shapes);
			Records.push_back('\n');
		}
		for(const std::pair<uint32_t, uint32_t> &Separator : Code.GetSeparators()) {
			Records.append("[separator]");
			Append(Separator.first);
			Append(Separator.second);
			Records.push_back('\n');
		}
		for(const OwnedRegionSpan &Owned : Anchors.OwnedRegions) {
			Records.append("[owned]");
			Append(GetPos(Owned.Node));
			Append(Owned.FirstFragment);
			Append(Owned.EndFragment);
			Append(Owned.Terminal.has_value());
			if(Owned.Terminal) {
				Append(static_cast<size_t>(Owned.Terminal->Kind));
				Append(Owned.Terminal->Fragment);
				Append(Owned.Terminal->Offset);
			}
			Records.push_back('\n');
			CoreObservation Child(*Owned.Placement->GetAnchors().Syntax);
			Child.ObserveSyntax(*Owned.Placement->GetAnchors().Syntax, RewriteTrace());
			Child.ObserveLayout(*Owned.Placement);
			Records.append(Child.Take());
			Records.append("[owned-end]\n");
		}
		const std::optional<uint32_t> Following = Code.GetFollowingTabs();
		Records.append("[following]");
		Append(Following.has_value());
		Append(Following.value_or(0));
		Records.push_back('\n');
		Records.append("[ending]");
		Append(Code.GetEnding().DoesEndWithNewline);
		Records.push_back('\n');
		// 終了
		return;
	}

	/**
	 * グループの閉じで観測するノードの記録関数
	 * 製品の作業予算を参照せず，同じ組の初出だけを閉じの順で記録する
	 * @param Node グループの対象
	 * @param Rank 決定順位
	 * @throws std::bad_alloc 記録済の組の集合への挿入又は決定順位毎のノード列の拡張に必要なメモリを確保出来ない場合
	 * @throws std::length_error 決定順位毎のノード列の拡張が対象のベクターの最大要素数を超える場合
	 */
	void CoreObservation::RecordSelection(const NodeId Node, const uint32_t Rank) {
		// 記録済の組の終了
		if(!SeenSelections.insert(static_cast<uint64_t>(Rank) << 32 | Node).second) return;
		if(SelectionNodes.size() <= Rank) SelectionNodes.resize(static_cast<size_t>(Rank) + 1);
		SelectionNodes[Rank].push_back(Node);
		// 終了
		return;
	}

	/**
	 * 選択の観測を出力の決定順位へ対応付ける関数
	 * @param Ranks 元の決定順位から出力の決定順位への対応
	 * @throws std::bad_alloc 出力の決定順位毎のノード列の拡張に必要なメモリを確保出来ない場合
	 * @throws std::length_error 出力の決定順位毎のノード列の拡張がベクターの最大要素数を超える場合
	 */
	void CoreObservation::ReorderSelections(const std::span<const uint32_t> Ranks) {
		std::vector<std::vector<NodeId>> Ordered;
		for(size_t Rank = 0; Rank < SelectionNodes.size(); ++Rank) if(Ranks[Rank] != NoGroup) {
			const uint32_t Next = Ranks[Rank];
			if(Ordered.size() <= Next) Ordered.resize(static_cast<size_t>(Next) + 1);
			Ordered[Next] = std::move(SelectionNodes[Rank]);
		}
		SelectionNodes = std::move(Ordered);
		// 終了
		return;
	}

	/**
	 * 観測したノードを診断だけが読む列へ移譲する関数
	 * @param Selections 製品が確定した選択の診断用の格納先
	 */
	void CoreObservation::CompleteSelections(const std::span<SelectedForm> Selections) {
		for(size_t Rank = 0; Rank < SelectionNodes.size(); ++Rank) Selections[Rank].Nodes = std::move(SelectionNodes[Rank]);
		SelectionNodes.clear();
		SeenSelections.clear();
		// 終了
		return;
	}

	// ノード収集だけを使う観測のコンストラクタ
	CoreObservation::CoreObservation() = default;
} // namespace Core
