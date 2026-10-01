#include "SyntaxNormalizer.hpp"
#include "../Util/CharClass.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include "ControlFlow.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
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

namespace Core {
	/**
	 * 文法に共通の生成形式の取得関数
	 * @param Syntax 構文モデル
	 * @param Operation 生成する操作
	 * @return 対応する形式（無ければ無し）
	 */
	static const CanonicalForm *GeneratedFormOf(const SyntaxModel &Syntax, const FormOperation Operation) {
		const std::vector<CanonicalForm> &Forms = Syntax.Rules().GeneratedForms;
		const std::vector<CanonicalForm>::const_iterator Found = std::ranges::find_if(
			Forms,
			[Operation](const CanonicalForm &Form) -> bool {
				// 操作の一致の返戻
				return Form.Operation == Operation;
			}
		);
		// 対応する形式か，無い事の返戻
		return Found == Forms.end() ? nullptr : &*Found;
	}

	/**
	 * 文法が生成する字句の取得関数
	 * @param Syntax 構文モデル
	 * @param Operation 生成する操作
	 * @param Role 字句の部品の役割
	 * @return 対応する字句
	 * @throws FormatLimitExceeded 生成形式又は部品が無い場合
	 */
	std::string SyntaxNormalizer::GeneratedFixed(
		const SyntaxModel &Syntax,
		const FormOperation Operation,
		const FormPartRole Role
	) {
		const CanonicalForm *const Form = GeneratedFormOf(Syntax, Operation);
		const FormPart *const Part = Form ? Form->PartOf(Role) : nullptr;
		if(!Part) throw FormatLimitExceeded("a generated lexeme has no grammar form part", FailureReason::FormatterDefect);
		// 形式に定めた字句の返戻
		return std::string(Part->FixedLexeme);
	}

	/**
	 * 控えの節点の値の取得関数
	 * @param State 構文の控え
	 * @param Syntax 最初の状態の値を変更前の控えから引く構文モデル
	 * @param Id 節点
	 * @return 其の状態で最後に変更された節点の値（最初の状態の節点は変更前の控えの値）
	 * @throws FormatLimitExceeded 控えに節点が無い場合
	 */
	SyntaxNormalizer::StateView SyntaxNormalizer::StateViewOf(
		const StateSnapshot &State,
		const SyntaxModel &Syntax,
		const NodeId Id
	) {
		for(const StateSnapshot *At = &State; At; At = At->Previous.get()) {
			if(!At->Previous) {
				// 最初の状態に無い節点の走査打切
				if(Id >= At->Hashes.size()) break;
				const SyntaxNode &Node = Syntax.JournaledNode(Id);
				// 最初の状態の値の返戻
				return { Node.Kind.Definition, Node.Fields, Node.Lexeme, Node.Children, &Node.Facts, At->Hashes[Id] };
			}
			if(const std::unordered_map<NodeId, StateNode>::const_iterator Found = At->Changes.find(Id); Found != At->Changes.end()) {
				const StateNode &Node = Found->second;
				// 変更を持つ最も新しい状態の値の返戻
				return { Node.Definition, Node.Fields, Node.Lexeme, Node.Children, &Node.Facts, Node.Hash };
			}
		}
		throw FormatLimitExceeded("syntax state lost a node", FailureReason::FormatterDefect);
	}

	/**
	 * 編集差分を共有する構文の状態の取得関数
	 * 最初は全節点の要約だけを控え（値は節点表の変更前の控えから引く），以後は変更節点と其の祖先の値と要約だけを前の状態へ重ねる
	 * 計算量：再評価する節点の数 V と字句の長さの和 L に対し O(V + L)
	 * @param Syntax 構文モデル（最初の状態を作る時は変更前の値の控えを始めて居る物）
	 * @param Previous 変更前の状態（初回は無し）
	 * @return 前の状態と変更箇所を共有する控え
	 * @throws FormatLimitExceeded 状態の検査又は複写が作業予算を超えた場合
	 */
	std::shared_ptr<const SyntaxNormalizer::StateSnapshot> SyntaxNormalizer::StateOf(
		const SyntaxModel &Syntax,
		std::shared_ptr<const StateSnapshot> Previous
	) {
		// 状態の基点と変更された祖先の準備
		const std::shared_ptr<StateSnapshot> State = std::make_shared<StateSnapshot>();
		State->Previous = std::move(Previous);
		State->Root = Syntax.GetRoot();
		if(!State->Previous) State->Hashes.resize(Syntax.NodeCount());
		std::unordered_set<NodeId> Affected;
		if(State->Previous) for(const NodeId Id : Syntax.TouchedNodes()) {
			for(NodeId Up = Id; Up != NoNode && Affected.insert(Up).second;) Up = Syntax.ParentOf(Up);
		}
		// 影響する部分木の後行順の走査
		std::vector<std::pair<NodeId, bool>> Pending { { State->Root, false } };
		while(!Pending.empty()) {
			const auto [Id, IsExit] = Pending.back();
			Pending.pop_back();
			if(State->Previous && !Affected.contains(Id)) continue;
			const SyntaxNode &Node = Syntax.GetNode(Id);
			if(!IsExit) {
				Pending.emplace_back(Id, true);
				for(size_t Index = Node.Children.size(); Index--;) Pending.emplace_back(Node.Children[Index], false);
				continue;
			}
			// 節点の値と構文事実からの要約の計算
			size_t Hash = static_cast<size_t>(1469598103934665603ULL);
			const auto Mix = [&Hash](const size_t Part) -> void {
				Hash = (Hash ^ Part) * static_cast<size_t>(1099511628211ULL);
			};
			Mix(reinterpret_cast<uintptr_t>(Node.Kind.Definition));
			Mix(Node.Fields);
			Mix(Node.Lexeme.size());
			Mix(Node.Children.size());
			Mix(Node.Facts.Constraints);
			Mix(Node.Facts.DiscardsBinding);
			Mix(Node.Facts.MayReassignExistingBinding);
			Mix(Node.Facts.HasBooleanNames);
			Mix(Node.Facts.AbsorbsLeadingComment);
			Mix(reinterpret_cast<uintptr_t>(Node.Facts.Forms.View().data()));
			Mix(Node.Facts.Forms.View().size());
			for(const unsigned char Byte : Node.Lexeme) Mix(Byte);
			for(const SemanticBoundary &Boundary : Node.Facts.Boundaries.View()) {
				Mix(Boundary.Before);
				Mix(static_cast<size_t>(Boundary.Kind));
				Mix(static_cast<size_t>(Boundary.Source));
				Mix(Boundary.Gap.size());
				for(const unsigned char Byte : Boundary.Gap) Mix(Byte);
				Syntax.Charge(Boundary.Gap.size());
				Syntax.Charge(Boundary.Gap.size());
			}
			for(const NodeId Child : Node.Children) Mix(StateViewOf(*State, Syntax, Child).Hash);
			Syntax.Charge(1);
			Syntax.Charge(Node.Children.size());
			Syntax.Charge(Node.Lexeme.size());
			Syntax.Charge(Node.Lexeme.size());
			// 最初の状態は要約だけ，以後は値と要約を控える
			if(!State->Previous) State->Hashes[Id] = Hash;
			else State->Changes.emplace(Id, StateNode{ Node.Kind.Definition, Node.Fields, Node.Lexeme, Node.Children, Node.Facts, Hash });
		}
		State->Hash = StateViewOf(*State, Syntax, State->Root).Hash;
		// 編集差分を共有する状態の返戻
		return State;
	}

	/**
	 * 構文状態の完全比較関数
	 * 要約が一致した時だけ，根から対応する全節点の値と子の並びを照合する
	 * @param Syntax 作業単位を計上する構文モデル
	 * @param Lhs 先の状態
	 * @param Rhs 後の状態
	 * @param RequireIdentity 節点番号も一致させるか
	 * @return 完全に同じ構文なら true
	 * @throws FormatLimitExceeded 完全比較が作業予算を超えた場合
	 */
	bool SyntaxNormalizer::SameState(
		const SyntaxModel &Syntax,
		const StateSnapshot &Lhs,
		const StateSnapshot &Rhs,
		const bool RequireIdentity
	) {
		// 対応する節点の構造と字句の比較
		std::vector<std::pair<NodeId, NodeId>> Pending { { Lhs.Root, Rhs.Root } };
		while(!Pending.empty()) {
			const auto [Left, Right] = Pending.back();
			Pending.pop_back();
			const StateView A = StateViewOf(Lhs, Syntax, Left), B = StateViewOf(Rhs, Syntax, Right);
			Syntax.Charge(2);
			Syntax.Charge(A.Lexeme.size());
			Syntax.Charge(B.Lexeme.size());
			// 節点番号も比べる場合の異なる節点の不一致の返戻
			if(RequireIdentity && Left != Right) return false;
			if(A.Definition != B.Definition || A.Fields != B.Fields || A.Lexeme != B.Lexeme || A.Children.size() != B.Children.size()) {
				// 異なる種別・欄・字句・子の数は一致しない事の返戻
				return false;
			}
			const SyntaxFacts &LeftFacts = *A.Facts, &RightFacts = *B.Facts;
			// 構文事実の異なる節点の不一致の返戻
			if(
				LeftFacts.Constraints != RightFacts.Constraints || LeftFacts.DiscardsBinding != RightFacts.DiscardsBinding ||
				LeftFacts.MayReassignExistingBinding != RightFacts.MayReassignExistingBinding ||
				LeftFacts.HasBooleanNames != RightFacts.HasBooleanNames ||
				LeftFacts.AbsorbsLeadingComment != RightFacts.AbsorbsLeadingComment ||
				LeftFacts.Forms.View().data() != RightFacts.Forms.View().data() ||
				LeftFacts.Forms.View().size() != RightFacts.Forms.View().size()
			) return false;
			// 意味境界と子の並びの比較
			const std::span<const SemanticBoundary> LeftBoundaries = LeftFacts.Boundaries.View();
			const std::span<const SemanticBoundary> RightBoundaries = RightFacts.Boundaries.View();
			// 境界の数の異なる節点の不一致の返戻
			if(LeftBoundaries.size() != RightBoundaries.size()) return false;
			for(size_t Index = 0; Index < LeftBoundaries.size(); ++Index) {
				const SemanticBoundary &X = LeftBoundaries[Index], &Y = RightBoundaries[Index];
				Syntax.Charge(X.Gap.size());
				Syntax.Charge(Y.Gap.size());
				// 異なる境界の不一致の返戻
				if(X.Before != Y.Before || X.Kind != Y.Kind || X.Source != Y.Source || X.Gap != Y.Gap) return false;
			}
			for(size_t Index = 0; Index < A.Children.size(); ++Index) Pending.emplace_back(A.Children[Index], B.Children[Index]);
		}
		// 全節点の一致の返戻
		return true;
	}

	/**
	 * 節点の構文順の整列関数（整形仕様 5.1）
	 * 候補の祖先だけを根から辿り，障壁の役割の部分木の中と木から外れた節点を除いて構文順に並べる
	 * 計算量：候補と其の祖先の数 A と，其の子の数の和 C に対し O(A + C)
	 * @param Syntax 構文モデル
	 * @param Found 候補（重複を含み得る）
	 * @param Barriers 内部を調べない役割
	 * @return 構文順の重複の無い候補
	 */
	std::vector<NodeId> SyntaxNormalizer::InDocumentOrder(
		const SyntaxModel &Syntax,
		const std::vector<NodeId> &Found,
		const std::initializer_list<Role> Barriers
	) {
		// 候補が無ければ祖先集合と根の走査を作らない
		if(Found.empty()) return {};
		// 候補と其の祖先の集合の作成
		std::unordered_set<NodeId> Wanted, OnPath;
		for(const NodeId Node : Found) {
			Syntax.Charge(1);
			if(Node == NoNode || Syntax.IsRemovedNode(Node) || !Wanted.insert(Node).second) continue;
			for(NodeId Up = Node; Up != NoNode && OnPath.insert(Up).second;) {
				Syntax.Charge(1);
				Up = Syntax.ParentOf(Up);
			}
		}
		std::vector<NodeId> Ordered;
		// 根から候補の祖先だけを辿る（障壁の役割の節点の中へ降りない）
		for(std::vector<NodeId> Pending { Syntax.GetRoot() }; !Pending.empty();) {
			const NodeId Node = Pending.back();
			Pending.pop_back();
			Syntax.Charge(1);
			if(
				!OnPath.contains(Node) || std::ranges::any_of(
					Barriers,
					[&Syntax, Node](const Role Barrier) -> bool {
						// 障壁の役割を持つかの返戻
						return Syntax.Is(Node, Barrier);
					}
				)
			) continue;
			if(Wanted.contains(Node)) Ordered.push_back(Node);
			const std::span<const NodeId> Children = Syntax.ChildrenOf(Node);
			for(size_t Index = Children.size(); Index--;) if(OnPath.contains(Children[Index])) Pending.push_back(Children[Index]);
		}
		// 構文順の候補の返戻
		return Ordered;
	}

	/**
	 * 変更に依存する対象の登録関数
	 * 変更した節点の祖先と，直接変更された親の子だけを渡す
	 * 返戻の規則は最内の関数を越えず，別の関数の制御の流れを調べ直さない
	 * 計算量：登録する祖先の数 A と変更した親の子の数の和 C に対し O((A + C) log (A + C))
	 * @param Syntax 構文モデル
	 * @param Changed 書換で変更した節点
	 * @param StopAtFunction 最内の関数の外へ依存を伝えないか
	 */
	void SyntaxNormalizer::SetWorkNodes(
		const SyntaxModel &Syntax,
		const std::span<const NodeId> Changed,
		const bool StopAtFunction
	) {
		// 変更節点からの依存する祖先の収集
		std::unordered_set<NodeId> Ancestors;
		for(const NodeId Id : Changed) {
			if(Id == NoNode || Syntax.IsRemovedNode(Id)) continue;
			for(NodeId Up = Id; Up != NoNode && Ancestors.insert(Up).second; Up = Syntax.ParentOf(Up)) {
				Syntax.Charge(1);
				if(StopAtFunction && (Syntax.Is(Up, Role::FunctionScope) || Syntax.Is(Up, Role::ExplicitResultFunction))) break;
			}
		}
		WorkNodes.assign(Ancestors.begin(), Ancestors.end());
		if(StopAtFunction) {
			std::sort(WorkNodes.begin(), WorkNodes.end());
			// 関数内の変更と其の関数だけの登録の終了
			return;
		}
		// 変更した親の子を含めた作業列の確定
		for(const NodeId Id : Changed) {
			if(Id == NoNode || Syntax.IsRemovedNode(Id)) continue;
			const std::span<const NodeId> Children = Syntax.ChildrenOf(Id);
			WorkNodes.insert(WorkNodes.end(), Children.begin(), Children.end());
			Syntax.Charge(Children.size());
		}
		std::sort(WorkNodes.begin(), WorkNodes.end());
		WorkNodes.erase(std::unique(WorkNodes.begin(), WorkNodes.end()), WorkNodes.end());
		// 終了
		return;
	}

	/**
	 * 役割から選んだ構文順の対象の取得関数
	 * 初回と文書全体の事実を読む規則は役割の索引から，再評価では登録した依存先から選ぶ
	 * @param Syntax 構文モデル
	 * @param Roles 対象の役割
	 * @param Barriers 内部を調べない役割
	 * @param IsWhole 作業列に依らず文書全体から選ぶか
	 * @return 構文順の対象
	 */
	std::vector<NodeId> SyntaxNormalizer::Candidates(
		const SyntaxModel &Syntax,
		const std::initializer_list<Role> Roles,
		const std::initializer_list<Role> Barriers,
		const bool IsWhole
	) const {
		// 役割索引又は変更依存先からの候補の収集
		std::vector<NodeId> Found;
		if(IsWhole || WorkNodes.empty()) {
			for(const Role Wanted : Roles) {
				for(const NodeId Node : Syntax.NodesWith(Wanted)) if(Syntax.Is(Node, Wanted)) Found.push_back(Node);
			}
		} else for(const NodeId Node : WorkNodes) if(
			std::ranges::any_of(
				Roles,
				[&Syntax, Node](const Role Wanted) -> bool {
					// 対象の役割を持つかの返戻
					return Syntax.Is(Node, Wanted);
				}
			)
		) Found.push_back(Node);
		const std::vector<NodeId> Ordered = InDocumentOrder(Syntax, Found, Barriers);
		// 索引からの候補１つの検査を１作業単位とする（整形仕様 9.2）
		Syntax.Charge(Ordered.size());
		// 構文順の対象の返戻
		return Ordered;
	}

	/**
	 * 役割を持つ子の親の構文順の取得関数
	 * @param Syntax 構文モデル
	 * @param ChildRole 子の役割
	 * @param Barriers 内部を調べない役割
	 * @return 重複を除いた親
	 */
	std::vector<NodeId> SyntaxNormalizer::ParentsOfRole(
		const SyntaxModel &Syntax,
		const Role ChildRole,
		const std::initializer_list<Role> Barriers
	) const {
		std::vector<NodeId> Parents;
		for(const NodeId Child : Candidates(Syntax, { ChildRole }, Barriers)) Parents.push_back(Syntax.ParentOf(Child));
		// 構文順の親の返戻
		return InDocumentOrder(Syntax, Parents, {});
	}

	/**
	 * 数値リテラルの型接尾辞の開始位置の取得関数
	 * @param Rules 文法の構文・意味の固定の定義
	 * @param Text 数値リテラルの字面
	 * @return 型接尾辞（Rust の `u8` / `i32` / `f64` 等）の開始位置（接尾辞が無ければ字面の長さ）
	 */
	size_t SyntaxNormalizer::NumericSuffixStart(const SyntaxRules &Rules, const std::string_view Text) {
		const bool IsBasePrefix = Text.size() > 1 && Text[0] == '0' && Rules.BasePrefixLetters.find(Text[1]) != std::string_view::npos;
		for(const std::string_view Suffix : Rules.NumericTypeSuffixes) {
			// 接頭辞付の整数は浮動小数の接尾辞を持たない
			if(IsBasePrefix && Suffix[0] == 'f') continue;
			// 一致した接尾辞の開始位置の返戻
			if(Text.size() > Suffix.size() && Text.substr(Text.size() - Suffix.size()) == Suffix) return Text.size() - Suffix.size();
		}
		// 接尾辞の無い事の返戻
		return Text.size();
	}

	/**
	 * 数値リテラルの正規化関数（整形仕様 5.2）
	 * 揺れの有る接頭辞・接尾辞・指数記号・１６進桁の英字を大文字へ揃え，求める言語では小数点の前後の `0` を補う
	 * @param Rules 文法の構文・意味の固定の定義
	 * @param Text 数値リテラルの字面
	 * @param ShouldPad 小数点の前後の `0` を補うか
	 * @return 正規化した字面
	 */
	std::string SyntaxNormalizer::NormalizeNumericLiteral(
		const SyntaxRules &Rules,
		const std::string_view Text,
		const bool ShouldPad
	) {
		// 数値の頭の符号の長さ
		const size_t Sign = !Text.empty() && (Text[0] == '+' || Text[0] == '-') ? 1 : 0;
		const bool IsZeroLed = Text.size() > Sign + 1 && Text[Sign] == '0';
		const bool IsHexBin =
		ShouldPad && IsZeroLed && (Text[Sign + 1] == 'x' || Text[Sign + 1] == 'X' || Text[Sign + 1] == 'b' || Text[Sign + 1] == 'B');
		const bool KeepsBasePrefix = IsZeroLed && Rules.BasePrefixLetters.find(Text[Sign + 1]) != std::string_view::npos;
		const size_t SuffixStart = NumericSuffixStart(Rules, Text);
		std::string Fixed;
		Fixed.reserve(Text.size() + 1);
		for(size_t Index = 0; Index < Text.size(); ++Index) {
			const char Char = Text[Index];
			// 小数点の前の `0` を符号の後に補う
			if(ShouldPad && !IsHexBin && Index == Sign && Char == '.') Fixed.push_back('0');
			// Rust の接頭辞・型接尾辞と BigInt の `n` を除く英字の大文字化
			Fixed.push_back(
				Index >= SuffixStart || Char == 'n' || KeepsBasePrefix && Index == Sign + 1 ?
				Char :
				static_cast<char>(std::toupper(static_cast<unsigned char>(Char)))
			);
			// 小数点の後の `0` を補う
			if(
				ShouldPad && !IsHexBin && Char == '.' &&
				(Index + 1 == Text.size() || !std::isdigit(static_cast<unsigned char>(Text[Index + 1])))
			) Fixed.push_back('0');
		}
		// 正規化した字面の返戻
		return Fixed;
	}

	/**
	 * 単引用符の文字列の本文を二重引用符の本文へ再表現する関数
	 * 単引用符の逃避を解いた値を，二重引用符の逃避・補間の規則で表し直す（同じ値を表せない形は変換しない）
	 * @param Rules 文法の構文・意味の固定の定義
	 * @param Body 引用符の内側の原文
	 * @param Prefix 引用符の前の接頭辞（Python の `r` / `f` 等，PHP の `b`）
	 * @param IsJsxAttribute 逃避を持たない JSX の属性値か
	 * @return 二重引用符の内側の本文（再表現出来ない場合は空）
	 */
	std::optional<std::string> SyntaxNormalizer::DoubleQuotedBody(
		const SyntaxRules &Rules,
		const std::string_view Body,
		const std::string_view Prefix,
		const bool IsJsxAttribute
	) {
		// Python の接頭辞の区分（生文字列と f 文字列は逃避を解かない）
		bool IsRaw = false, IsFormat = false;
		for(const char Char : Prefix) {
			const char Lower = static_cast<char>(Char | 0X20);
			// 再表現出来ない接頭辞の拒否の返戻
			if(Rules.QuotePrefixes.find(Lower) == std::string_view::npos) return std::nullopt;
			// 逃避の規則を確かめれない接頭辞（Ruby の `%q` 等）の返戻
			IsRaw = IsRaw || Rules.RawQuotePrefixes.find(Lower) != std::string_view::npos;
			IsFormat = IsFormat || Rules.FormatQuotePrefixes.find(Lower) != std::string_view::npos;
		}
		// 逃避を解かない本文（JSX の属性値・生文字列・f 文字列）
		if(IsJsxAttribute || IsRaw || IsFormat) {
			// 二重引用符と，f 文字列の逃避を含む本文は再表現出来ない事の返戻
			if(Body.find('"') != std::string_view::npos || IsFormat && Body.find('\\') != std::string_view::npos) return std::nullopt;
			// 其の儘使える本文の返戻
			return std::string(Body);
		}
		// 単引用符だけが逃避を持つ Ruby・PHP（其の他の `\` は字の儘）
		const bool IsLiteralBackslash = Rules.SingleQuoteBackslashIsLiteral;
		std::string Out;
		Out.reserve(Body.size() + 8);
		for(size_t Pos = 0; Pos < Body.size(); ++Pos) {
			const char Char = Body[Pos];
			// 単引用符の逃避と二重引用符側の逃避への再表現
			if(Char == '\\') {
				// 閉じの無い逃避は再表現出来ない事の返戻
				if(Pos + 1 == Body.size()) return std::nullopt;
				if(const char Next = Body[++Pos]; Next == '\'') Out += '\'';
				else if(IsLiteralBackslash) {
					// 逃避の `\\` と字の儘の `\` は，二重引用符では何れも `\\` と書く
					Out += "\\\\";
					if(Next != '\\') --Pos;
				} else {
					// 引用符に依らず同じ意味の逃避の保持
					Out += '\\';
					Out += Next;
				}
			} else if(Char == '"') Out += "\\\"";
			else if(Rules.InterpolatesDollar && Char == '$') Out += "\\$";
			// 補間を始めない様に逃避した `#`
			else if(Char == '#' && Pos + 1 < Body.size() && Rules.HashInterpolationHeads.find(Body[Pos + 1]) != std::string_view::npos) {
				Out += "\\#";
			} else Out += Char;
		}
		// 再表現した本文の返戻
		return Out;
	}

	/**
	 * Python の三重単引用符の本文を三重二重引用符の本文へ再表現する関数
	 * 閉じに読まれる二重引用符の連なり（３個以上と本文の末尾の物）を逃避し，逃避を解かない生文字列・f 文字列で逃避が要る本文は変換しない
	 * 計算量：本文の長さ N に対し O(N)
	 * @param Rules 文法の構文・意味の固定の定義
	 * @param Body 引用符の内側の原文
	 * @param Prefix 引用符の前の接頭辞
	 * @return 三重二重引用符の内側の本文（再表現出来ない場合は空）
	 */
	std::optional<std::string> SyntaxNormalizer::TripleDoubleQuotedBody(
		const SyntaxRules &Rules,
		const std::string_view Body,
		const std::string_view Prefix
	) {
		// 逃避を解かない接頭辞の有無
		bool IsVerbatim = false;
		for(const char Char : Prefix) {
			const char Lower = static_cast<char>(Char | 0X20);
			// 逃避の規則を確かめれない接頭辞の返戻
			if(Rules.QuotePrefixes.find(Lower) == std::string_view::npos) return std::nullopt;
			IsVerbatim = IsVerbatim || Rules.RawQuotePrefixes.find(Lower) != std::string_view::npos ||
			Rules.FormatQuotePrefixes.find(Lower) != std::string_view::npos;
		}
		std::string Out;
		Out.reserve(Body.size() + 8);
		for(size_t Pos = 0; Pos < Body.size();) {
			// 逃避と逃避した字の組の複写
			if(Body[Pos] == '\\') {
				Out.append(Body.substr(Pos, 2));
				Pos += 2;
				continue;
			}
			if(Body[Pos] != '"') {
				Out += Body[Pos++];
				continue;
			}
			const size_t Run = std::min(Body.find_first_not_of('"', Pos), Body.size()) - Pos;
			// 閉じに読まれない連なりの複写
			if(Run < 3 && Pos + Run < Body.size()) Out.append(Run, '"');
			// 逃避を解かない本文で閉じに読まれる連なりは再表現出来ない事の返戻
			else if(IsVerbatim) return std::nullopt;
			else for(size_t Count = 0; Count < Run; ++Count) Out += "\\\"";
			Pos += Run;
		}
		// 再表現した本文の返戻
		return Out;
	}

	/**
	 * CSS の色と宣言末尾の正規化の収集関数（整形仕様 5.2）
	 * １６進色の桁を大文字へ揃え（選択子の ID と参照は除く），塊の最後の宣言にも `;` を付ける（空のカスタムプロパティの空白の値を保つ）
	 * @param Syntax 構文モデル
	 * @param Id 節点
	 * @param Plan 書換の計画の格納先
	 */
	void SyntaxNormalizer::CollectCssStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) {
		const SyntaxNode &Node = Syntax.GetNode(Id);
		const NodeId Parent = Node.Parent;
		// 空の仮引数を補う解析結果の除去
		if(Syntax.Is(Id, Role::MixinRule) && Syntax.Is(Id, Role::MixinStatement)) {
			for(const NodeId Child : Node.Children) {
				if(!Syntax.Is(Child, Role::MixinParameters) || !Syntax.FirstTokenRole(Child, Role::OpeningParenLead)) continue;
				if(const bool IsEmpty = Syntax.NamedChildCount(Child) == 1 && Syntax.Text(Syntax.NamedChild(Child, 0)).empty(); IsEmpty) {
					const AnchorTarget Previous { Syntax.PrevSibling(Child), true };
					RewriteStep Step { RewriteKind::Remove, Child, {}, { Child, Previous, Previous, Previous } };
					Plan.Steps.push_back(std::move(Step));
				}
				// 最初の括弧の並びだけを見る事の終了
				return;
			}
			// 終了
			return;
		}
		if(Syntax.Is(Id, Role::ColorValue) && Node.Children.empty()) {
			const NodeId Host = Syntax.Is(Parent, Role::CallArguments) ? Syntax.ParentOf(Parent) : Parent;
			// 選択子の中の色の場合の終了
			if(Syntax.Is(Host, Role::SelectorHost)) return;
			std::string Upper = Node.Lexeme;
			for(char &Char : Upper) if(Char >= 'a' && Char <= 'f') Char = static_cast<char>(Char + 'A' - 'a');
			if(Upper != Node.Lexeme) Plan.Steps.push_back({ RewriteKind::SetLexeme, Id, std::move(Upper) });
			// 終了
			return;
		}
		// 型付の宣言でない場合の終了
		if(!Syntax.Is(Id, Role::TypedDeclaration)) return;
		// 塊の最後の宣言か
		const NodeId Next = Syntax.NextSibling(Id);
		// 塊の最後の宣言でない場合の終了
		if(Next != NoNode && (Syntax.IsNamed(Next) || !Syntax.IsTokenRole(Next, Role::ClosingBraceToken))) return;
		if(Node.Children.empty()) {
			// 字面を保つ宣言の末尾への区切りの追加
			const std::string Terminator = GeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::Terminator);
			const std::string Delimiter = GeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::ValueDelimiter);
			if(!Node.Lexeme.ends_with(Terminator)) {
				Plan.Steps.push_back({ RewriteKind::SetLexeme, Id, Node.Lexeme + (Node.Lexeme.ends_with(Delimiter) ? " " : "") + Terminator });
			}
			// 終了
			return;
		}
		const NodeId Last = Node.Children.back();
		// 既に文の終端の語で終わる場合の終了
		if(Syntax.IsWord(Last, Word::StatementEnd)) return;
		// 値の無いカスタムプロパティは空白を値として `;` の前に置く
		const BoundaryKind Boundary = Syntax.IsTokenRole(Last, Role::ColonToken) ? BoundaryKind::Spaced : BoundaryKind::Glued;
		RewriteStep Step { RewriteKind::Insert, Id };
		Step.Index = static_cast<uint32_t>(Node.Children.size());
		Step.Boundary = Boundary;
		Step.Inserted = { GeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::Terminator) };
		Plan.Steps.push_back(std::move(Step));
		// 終了
		return;
	}

	/**
	 * 文字列の二重引用符への正規化の収集関数（整形仕様 5.2）
	 * 単引用符の文字列を値を保つ二重引用符の文字列へ，Python の文書文字列を三重二重引用符へ揃える
	 * @param Syntax 構文モデル
	 * @param Id 文字列の節点（字面を保つ葉）
	 * @param Plan 書換の計画の格納先
	 */
	void SyntaxNormalizer::CollectQuoteStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) {
		const SyntaxNode &Node = Syntax.GetNode(Id);
		const std::string_view Text = Node.Lexeme;
		// 接頭辞の後の引用符
		const size_t QuotePos = std::min(Text.find_first_of("'\""), Text.size());
		const char Quote = QuotePos < Text.size() ? Text[QuotePos] : '\0';
		// 同じ引用符で閉じる Python の三重引用符か
		const bool IsTriple =
		Syntax.Rules().HasTripleQuote && Text.size() >= QuotePos + 6 && Text[QuotePos + 1] == Quote && Text[QuotePos + 2] == Quote;
		// 引用符で閉じない字面の終了
		if(Text.size() < QuotePos + 2 || !Quote || Text.back() != Quote) return;
		const std::string_view Prefix = Text.substr(0, QuotePos);
		const size_t Delimiter = IsTriple ? 3 : 1;
		const std::string_view Body = Text.substr(QuotePos + Delimiter, Text.size() - QuotePos - (Delimiter << 1));
		// 関数・クラス・モジュールの本体の最初の文に為る，f 文字列・バイト列でない文字列（Python の文書文字列）か
		const NodeId Parent = Node.Parent;
		const bool IsDocstring = Syntax.Is(Id, Role::DocumentString) && !IsTriple && std::ranges::none_of(
			Prefix,
			[&Syntax](const char Char) -> bool {
				// 文書文字列に為らない接頭辞かの返戻
				return Syntax.Rules().NonDocumentQuotePrefixes.find(static_cast<char>(Char | 0X20)) != std::string_view::npos;
			}
		) && (
			Syntax.Is(Parent, Role::Module) ||
			Syntax.Is(Parent, Role::LexicalBlock) && Syntax.Is(Syntax.ParentOf(Parent), Role::LeadingStringDocumentOwner)
		) && Syntax.NamedChild(Parent, 0) == Id;
		// 値を保つ二重引用符の本文（二重引用符の文書文字列は其の儘三重にする，JSX の属性値は逃避を持たない）
		const std::optional<std::string> Double =
		Quote == '"' ? IsDocstring ? std::optional<std::string>(Body) : std::nullopt : IsTriple ?
		TripleDoubleQuotedBody(Syntax.Rules(), Body, Prefix) :
		DoubleQuotedBody(Syntax.Rules(), Body, Prefix, Syntax.Is(Parent, Role::UnescapedAttribute));
		// 二重引用符の本文を持たない字面の終了
		if(!Double) return;
		// 引用符を揃えた字面と節点の組直
		const std::string_view Quotes = IsTriple || IsDocstring ? "\"\"\"" : "\"";
		const std::string Quoted = std::string(Prefix).append(Quotes).append(*Double).append(Quotes);
		if(Syntax.Rules().DoubleQuotedDefinition) {
			RewriteStep String { RewriteKind::Reshape, Id };
			String.Inserted = { {}, true, {}, Node.Fields, NoNode, Quoted, Syntax.Rules().DoubleQuotedDefinition };
			String.Inserted.Facts = Node.Facts;
			Plan.Steps.push_back(std::move(String));
		}
		Plan.Steps.push_back({ RewriteKind::SetLexeme, Id, Quoted });
		// 終了
		return;
	}

	/**
	 * 値が使われない文の位置かの判定関数
	 * 増減の前置化と其の検査が同じ位置を見る為，判定を此処だけで定める
	 * @param Syntax 構文モデル
	 * @param Node 判定する式（要素１つの括弧は外側）
	 * @param Parent 式の親
	 * @return 文の位置か for の更新の位置なら true
	 */
	bool SyntaxNormalizer::IsValueUnusedContext(const SyntaxModel &Syntax, const NodeId Node, const NodeId Parent) {
		// 文の直下の返戻
		if(Syntax.Is(Parent, Role::UnusedValueHost)) return true;
		// for の更新の位置（`increment` は JS 系の綴り）かの返戻
		return Syntax.Is(Parent, Role::CountedLoop) &&
		(Syntax.FieldChild(Parent, ChildRole::Update) == Node || Syntax.FieldChild(Parent, ChildRole::Increment) == Node);
	}

	/**
	 * 言語が省略を許す括弧の除去の収集関数（整形仕様 5.2）
	 * 名前１つのラムダの仮引数の括弧（`(x) -> x` → `x -> x`）と，文法が省略を許す空の実引数・仮引数の並びを外す
	 * 注釈・属性・列挙の定数の空の実引数（`@A()` → `@A`），初期化子を伴う生成の空の実引数（`new A() { X = 1 }` → `new A { X = 1 }`），後置のラムダ・クロージャだけを渡す呼出の空の実引数（`f() { }` → `f { }`），副の構築子の無い型の空の主の構築子（`class A()` → `class A`），C++ の修飾・戻値の型の無いラムダの空の仮引数（`[]() { }` → `[] { }`）と空の仮引数の `void`（`f(void)` → `f()`）を対象とする
	 * @param Syntax 構文モデル
	 * @param Id 節点
	 * @param Plan 書換の計画の格納先
	 */
	void SyntaxNormalizer::CollectOptionalParenStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) {
		// 節点と其の子孫の除去（子孫を含む錨は指定の行先へ移す）
		const auto RemoveSubtree = [&Plan](const NodeId Root, const AnchorTarget Target) -> void {
			Plan.Steps.push_back({ RewriteKind::Remove, Root, {}, { Root, Target, Target, Target } });
		};
		// 単一の名前だけの仮引数の括弧の除去
		if(Syntax.Is(Id, Role::ArrowFunction)) {
			const NodeId Parameters = Syntax.FieldChild(Id, ChildRole::Parameters);
			NodeId Parameter = Syntax.NamedChild(Parameters, 0);
			if(Syntax.Is(Parameter, Role::RequiredParameter) && Syntax.ChildCount(Parameter) == 1) Parameter = Syntax.Child(Parameter, 0);
			if(
				Syntax.ChildCount(Parameters) == 3 && Syntax.Is(Parameter, Role::Identifier) &&
				Syntax.FieldChild(Id, ChildRole::ReturnType) == NoNode && !AnyChildOf(
					Syntax,
					Id,
					[&](const NodeId Child) -> bool {
						return Syntax.Is(Child, Role::TypeParameterList);
					}
				)
			) {
				if(Parameter != Syntax.NamedChild(Parameters, 0)) {
					Plan.Steps.push_back({ RewriteKind::Unwrap, Syntax.NamedChild(Parameters, 0) });
				}
				Plan.Steps.push_back({ RewriteKind::Unwrap, Parameters });
			}
			// 終了
			return;
		}
		// 後置の受け手と型の解釈を保てる構築式の空の実引数の除去
		if(Syntax.Is(Id, Role::Construction)) {
			const NodeId Arguments = Syntax.FieldChild(Id, ChildRole::Arguments);
			NodeId Subject = Id, Parent = Syntax.ParentOf(Id);
			while(Syntax.Is(Parent, Role::GroupingParen)) {
				Subject = Parent;
				Parent = Syntax.ParentOf(Parent);
			}
			const bool IsReceiver = Syntax.NamedChild(Parent, 0) == Subject && (
				Syntax.Is(Parent, Role::PostfixExpressionHost) || Syntax.Is(Parent, Role::Construction) ||
				Syntax.Is(Parent, Role::NonNullAssertion)
			);
			const bool IsComparison = Syntax.FieldChild(Parent, ChildRole::Left) == Subject && Syntax.Is(Parent, Role::BinaryOperation) &&
			Syntax.IsTokenRole(Syntax.FieldChild(Parent, ChildRole::Operator), Role::LessThanToken);
			// 後置の表明で終わる被構築の式（外すと表明が構築の結果へ掛かる）
			if(
				const bool IsAssertedCallee = Syntax.Is(Syntax.NamedChild(Id, 0), Role::NonNullAssertion); Syntax.ChildCount(Arguments) == 2 &&
				!IsReceiver && !IsComparison && !IsAssertedCallee && !AnyChildOf(
					Syntax,
					Id,
					[&](const NodeId Child) -> bool {
						return Syntax.Is(Child, Role::TypeArgumentList);
					}
				)
			) RemoveSubtree(Arguments, { Id, true });
			// 終了
			return;
		}
		// 空の基底型と呼出・生成の実引数の除去
		if(Syntax.Is(Id, Role::OptionalEmptySuperclass)) if(
			const NodeId Arguments = Syntax.FirstNamedChildWithRole(Id, Role::ArgumentSequence);
			Arguments != NoNode && !Syntax.NamedChildCount(Arguments)
		) RemoveSubtree(Arguments, { Id, false });
		if(Syntax.Is(Id, Role::OptionalConstructionArguments)) {
			// 呼出と生成の空の実引数の探索
			NodeId Arguments = NoNode;
			if(Syntax.Is(Id, Role::FunctionCallExpression) && Syntax.IsWord(Syntax.FieldChild(Id, ChildRole::Function), Word::BareCall)) {
				if(const NodeId Args = Syntax.FieldChild(Id, ChildRole::Arguments); Args != NoNode && !Syntax.NamedChildCount(Args)) {
					RemoveSubtree(Args, { Id, false });
					Plan.Steps.push_back({ RewriteKind::Unwrap, Id });
				}
			}
			if(Syntax.Is(Id, Role::Attribute)) Arguments = Syntax.FirstNamedChildWithRole(Id, Role::CallArguments);
			if(Syntax.Is(Id, Role::ObjectCreationExpression)) {
				NodeId Owner = Syntax.ParentOf(Id), Value = Id;
				while(Syntax.Is(Owner, Role::ValueParentheses)) {
					Value = Owner;
					Owner = Syntax.ParentOf(Owner);
				}
				if(!Syntax.Is(Owner, Role::ConstructionReceiverHost) || Syntax.NamedChild(Owner, 0) != Value) {
					const NodeId Anonymous = Syntax.FirstNamedChildWithRole(Id, Role::AnonymousClass);
					Arguments = Syntax.FirstNamedChildWithRole(Anonymous == NoNode ? Id : Anonymous, Role::CallArguments);
				}
			}
			if(Arguments != NoNode && !Syntax.NamedChildCount(Arguments)) RemoveSubtree(Arguments, { Id, false });
			if(
				Syntax.Is(Id, Role::ExitStatement) && Syntax.ChildCount(Id) > 2 &&
				Syntax.IsTokenRole(Syntax.Child(Id, 1), Role::OpeningParenToken) &&
				Syntax.IsTokenRole(Syntax.Child(Id, 2), Role::ClosingParenToken)
			) {
				RemoveSubtree(Syntax.Child(Id, 1), { Id, false });
				RemoveSubtree(Syntax.Child(Id, 2), { Id, false });
			}
		}
		if(Syntax.Is(Id, Role::OptionalLambdaDeclarator)) {
			const NodeId Declarator = Syntax.FieldChild(Id, ChildRole::Declarator);
			const NodeId Parameters = Syntax.ChildCount(Declarator) == 1 ? Syntax.Child(Declarator, 0) : NoNode;
			// 仮引数だけの宣言子の空の並びの除去（修飾・戻値の型・制約を伴う省略は C++23 より前では出来ない）
			if(
				Syntax.Is(Parameters, Role::DeclaratorParameters) && Syntax.ChildCount(Parameters) == 2 &&
				Syntax.IsTokenRole(Syntax.Child(Parameters, 0), Role::OpeningParenToken)
			) RemoveSubtree(Declarator, { Syntax.PrevSibling(Declarator), true });
			// 終了
			return;
		}
		// C からも読むヘッダを除く C++ の空の仮引数の `void`
		if(Syntax.Is(Id, Role::OptionalVoidParameter) && Syntax.ChildCount(Id) == 3) {
			if(
				const NodeId Parameter = Syntax.Child(Id, 1);
				Syntax.Is(Parameter, Role::ParameterDeclaration) && Syntax.ChildCount(Parameter) == 1 &&
				Syntax.Is(Syntax.Child(Parameter, 0), Role::PrimitiveType) && Syntax.IsWord(Parameter, Word::VoidType)
			) RemoveSubtree(Parameter, { Syntax.Child(Id, 0), true });
			// 終了
			return;
		}
		if(Syntax.Is(Id, Role::LambdaExpression)) {
			// 推論の仮引数と型の無い仮引数の括弧の判定
			const NodeId Parameters = Syntax.FieldChild(Id, ChildRole::Parameters);
			const NodeId Parameter = Syntax.NamedChild(Parameters, 0);
			if(Syntax.Is(Parameters, Role::InferredParameters)) {
				// 名前１つの推論の仮引数の括弧の除去
				if(Syntax.ChildCount(Parameters) == 3 && Syntax.Is(Parameter, Role::Identifier)) {
					Plan.Steps.push_back({ RewriteKind::Unwrap, Parameters });
				}
			} else if(
				Syntax.Is(Parameters, Role::DeclaratorParameters) && Syntax.ChildCount(Parameters) == 3 &&
				Syntax.Is(Parameter, Role::Parameter) && Syntax.ChildCount(Parameter) == 1 &&
				Syntax.Is(Syntax.Child(Parameter, 0), Role::Identifier) && !AnyChildOf(
					Syntax,
					Id,
					[&](const NodeId Child) -> bool {
						// 仮引数の前の修飾以外の名前付の子（属性・戻値の型は括弧の仮引数だけに付く）かの返戻
						return Syntax.IsNamed(Child) && Syntax.Precedes(Child, Parameters) && !Syntax.Is(Child, Role::Modifier);
					}
				)
			) {
				// 型・修飾の無い仮引数１つの括弧の除去（名前は暗黙の仮引数に為る）
				Plan.Steps.push_back({ RewriteKind::Unwrap, Parameters });
				Plan.Steps.push_back({ RewriteKind::Unwrap, Parameter });
				RewriteStep Implicit { RewriteKind::Reshape, Syntax.Child(Parameter, 0) };
				Implicit.Inserted = { {}, true, {}, 0, NoNode, {}, Syntax.Rules().ImplicitParameterDefinition };
				Plan.Steps.push_back(std::move(Implicit));
			}
			// 終了
			return;
		}
		if(Syntax.Is(Id, Role::LambdaFunctionType)) {
			const NodeId Parameters = Syntax.FirstNamedChildWithRole(Id, Role::LambdaFunctionTypeParameters);
			// 名前だけの仮引数の並びか
			bool IsBare =
			Parameters != NoNode && Syntax.ChildCount(Id) > 2 && Syntax.IsTokenRole(Syntax.Child(Id, 0), Role::OpeningParenToken);
			ForEachNamedChildOf(
				Syntax,
				Parameters,
				[&](const NodeId Parameter) -> void {
					IsBare = IsBare && Syntax.Is(Parameter, Role::LambdaParameter) && Syntax.ChildCount(Parameter) == 1 &&
					Syntax.Is(Syntax.Child(Parameter, 0), Role::SimpleName);
				}
			);
			// 型の無い名前だけのクロージャの仮引数の括弧の除去（錨は仮引数の並びの前後へ移す）
			if(IsBare) {
				const AnchorTarget Before { Parameters, false }, After { Parameters, true };
				const NodeId Open = Syntax.Child(Id, 0), Close = Syntax.NextSibling(Parameters);
				Plan.Steps.push_back({ RewriteKind::Remove, Open, {}, { Open, Before, Before, Before } });
				Plan.Steps.push_back({ RewriteKind::Remove, Close, {}, { Close, After, After, After } });
			}
			// 終了
			return;
		}
		// 末尾のクロージャを別の接尾辞で渡す呼出の空の実引数（被呼出側が呼出なら結付を保つ）
		if(Syntax.Is(Id, Role::CallSuffix) && Syntax.NamedChildCount(Id) == 1) {
			const NodeId Arguments = Syntax.NamedChild(Id, 0), Call = Syntax.ParentOf(Id), Outer = Syntax.ParentOf(Call);
			const NodeId Trailing = Syntax.LastNamedChild(Outer);
			if(
				Syntax.Is(Arguments, Role::ValueArguments) && Syntax.ChildCount(Arguments) == 2 && Syntax.Is(Call, Role::CallExpression) &&
				Syntax.Is(Outer, Role::CallExpression) && Syntax.NamedChild(Outer, 0) == Call && Syntax.Is(Trailing, Role::CallSuffix) &&
				Syntax.Is(Syntax.NamedChild(Trailing, 0), Role::LambdaLiteral) &&
				!Syntax.Is(Syntax.NamedChild(Call, 0), Role::CallExpression) && Syntax.ChildCount(Call) == 2
			) {
				// 空の実引数の接尾辞の除去と，被呼出側だけに為る呼出の包みの除去
				const AnchorTarget Target { Syntax.NamedChild(Call, 0), true };
				for(const NodeId Removed : { Id, Arguments, Syntax.Child(Arguments, 0), Syntax.Child(Arguments, 1) }) {
					Plan.Steps.push_back({ RewriteKind::Remove, Removed, {}, { Removed, Target, Target, Target } });
				}
				Plan.Steps.push_back({ RewriteKind::Unwrap, Call });
				// 終了
				return;
			}
		}
		// 省ける空の並び（実引数の場を持つ注釈・列挙の定数と，構築子の呼出を持つ型の位置でない注釈）
		NodeId List = Syntax.Is(Id, Role::OptionalAnnotationArguments) ? Syntax.FieldChild(Id, ChildRole::Arguments) : NoNode;
		NodeId Invocation = NoNode;
		if(List == NoNode && Syntax.Is(Id, Role::Annotation) && !Syntax.Is(Syntax.ParentOf(Id), Role::TypeModifiers)) {
			// 型の位置の注釈は後続の関数型の仮引数の並びと紛れる為に除く
			Invocation = Syntax.FirstNamedChildWithRole(Id, Role::ConstructorInvocation);
			List = Syntax.FirstNamedChildWithRole(Invocation, Role::ValueArguments);
		} else if(Syntax.Is(Id, Role::Attribute)) List = Syntax.FirstNamedChildWithRole(Id, Role::AttributeArgumentList);
		// 初期化子を伴う生成
		else if(
			Syntax.Is(Id, Role::ObjectCreationExpression) && Syntax.FirstNamedChildWithRole(Id, Role::InitializerExpression) != NoNode
		) List = Syntax.FirstNamedChildWithRole(Id, Role::ArgumentSequence);
		// 後置のラムダ・クロージャを持つ呼出の接尾辞（被呼出側が呼出なら結付を保つ）
		else if(
			Syntax.Is(Id, Role::CallSuffix) && (
				Syntax.FirstNamedChildWithRole(Id, Role::AnnotatedLambda) != NoNode ||
				Syntax.FirstNamedChildWithRole(Id, Role::LambdaLiteral) != NoNode
			) && !Syntax.Is(Syntax.NamedChild(Syntax.ParentOf(Id), 0), Role::CallExpression)
		) List = Syntax.FirstNamedChildWithRole(Id, Role::ValueArguments);
		else if(Syntax.Is(Id, Role::EnumEntry)) List = Syntax.FirstNamedChildWithRole(Id, Role::ValueArguments);
		// 副の構築子の無い型の主の構築子
		else if(Syntax.Is(Id, Role::ClassDeclaration)) {
			NodeId Body = Syntax.FirstNamedChildWithRole(Id, Role::ClassMembers);
			if(Body == NoNode) Body = Syntax.FirstNamedChildWithRole(Id, Role::EnumClassBody);
			if(Syntax.FirstNamedChildWithRole(Body, Role::SecondaryConstructor) == NoNode) {
				List = Syntax.FirstNamedChildWithRole(Id, Role::PrimaryConstructor);
			}
		}
		// 開閉の括弧だけの並び以外の終了
		if(Syntax.ChildCount(List) != 2 || !Syntax.IsTokenRole(Syntax.Child(List, 0), Role::OpeningParenToken)) return;
		// 空の並びの除去（錨は前の兄弟の後，前の兄弟が無ければ後の兄弟の前へ移す）
		const NodeId Previous = Syntax.PrevSibling(List);
		const AnchorTarget Target =
		Previous != NoNode ? AnchorTarget{ Previous, true } : AnchorTarget{ Syntax.NextSibling(List), false };
		// 空の実引数の省略に依って文法上の種別も変わる形式
		if(
			const CanonicalForm *const Form = Syntax.FormOf(Id, FormOperation::OmitEmptyArguments);
			Form && Form->CanOmitEmptyArguments(true) && Invocation == NoNode
		) {
			RewriteStep Step { RewriteKind::Reshape, Id };
			Step.Transfer = { List, Target, Target, Target };
			Step.Inserted.Definition = Form->PartOf(FormPartRole::Result)->Definition;
			Step.Inserted.IsNamed = Step.Inserted.Definition->IsNamed;
			for(const NodeId Child : Syntax.ChildrenOf(Id)) if(Child != List) {
				Step.Inserted.Children.push_back({ {}, false, {}, Syntax.IsNamed(Child) ? Syntax.FieldBit(ChildRole::Name) : 0, Child });
			}
			Plan.Steps.push_back(std::move(Step));
			// 空の実引数を省いて組み直した場合の終了
			return;
		}
		Plan.Steps.push_back({ RewriteKind::Remove, List, {}, { List, Target, Target, Target } });
		for(const NodeId Token : Syntax.ChildrenOf(List)) {
			Plan.Steps.push_back({ RewriteKind::Remove, Token, {}, { Token, Target, Target, Target } });
		}
		// 空の実引数を除いた構築子呼出は型だけを残す為，余分な包みも除く
		if(Invocation != NoNode) Plan.Steps.push_back({ RewriteKind::Unwrap, Invocation });
		// 終了
		return;
	}

	/**
	 * C++ の組込の算術型・ポインタの変数かの判定関数
	 * 多重定義の無い組込の増減だけを前置へ揃える為，同じ関数の中で最も内側の宣言から型を確かめる（別の関数・型の境界は越えない）
	 * @param Syntax 構文モデル
	 * @param Operand 増減の被演算子
	 * @param Memo 名前と探索位置毎の判定の控え
	 * @return 組込の算術型（`bool` を除く）・ポインタ・其の参照として宣言した名前なら true
	 */
	bool SyntaxNormalizer::IsBuiltinScalarName(
		const SyntaxModel &Syntax,
		NodeId Operand,
		std::unordered_map<NodeId, std::unordered_map<std::string, bool>> &Memo
	) {
		// 名前を包む括弧の除去
		while(Syntax.Is(Operand, Role::GroupingParen) && Syntax.NamedChildCount(Operand) == 1) {
			Syntax.Charge(1);
			Operand = Syntax.NamedChild(Operand, 0);
		}
		// 名前でない被演算子の返戻
		if(!Syntax.Is(Operand, Role::Identifier)) return false;
		const std::string &Name = Syntax.GetNode(Operand).Lexeme;
		// 型の指定が組込の算術型か（`bool` は増減を持たない）
		const auto IsArithmetic = [&Syntax](const NodeId Type) -> bool {
			std::string Text = Syntax.Text(Type);
			if(
				const std::string_view Standard = Syntax.Rules().Spelling(Word::StandardNamespace);
				!Standard.empty() && std::string_view(Text).starts_with(Standard)
			) Text.erase(0, Standard.size());
			// 組込の型と標準の整数の別名かの返戻
			return Syntax.Is(Type, Role::PrimitiveType) && !Syntax.Rules().IsSpelling(Text, Word::Boolean) &&
			!Syntax.Rules().IsSpelling(Text, Word::VoidType) || Syntax.Is(Type, Role::SizedTypeSpecifier) ||
			(Syntax.Is(Type, Role::TypeName) || Syntax.Is(Type, Role::QualifiedIdentifier)) &&
			Syntax.Rules().IsSpelling(Text, Word::IntegerAlias);
		};
		// 宣言の中の同じ名前の宣言子の判定（未宣言は空，宣言は組込かの値）
		const auto Declares = [&](const NodeId Declaration) -> std::optional<bool> {
			const NodeId Type = Syntax.FieldChild(Declaration, ChildRole::Type);
			std::optional<bool> Result;
			for(const NodeId Child : Syntax.ChildrenOf(Declaration)) {
				Syntax.Charge(1);
				if(!Syntax.HasField(Child, ChildRole::Declarator)) continue;
				// 宣言子を名前迄解く（ポインタは型に依らず組込，配列と関数は増減を持たない）
				NodeId Declarator = Child;
				bool IsPointer = false, IsAggregate = false;
				while(Declarator != NoNode && !Syntax.Is(Declarator, Role::Identifier)) {
					Syntax.Charge(1);
					IsPointer = IsPointer || Syntax.Is(Declarator, Role::PointerDeclarator);
					IsAggregate = IsAggregate || Syntax.Is(Declarator, Role::ArrayDeclarator) || Syntax.Is(Declarator, Role::FunctionDeclarator);
					const NodeId Inner = Syntax.FieldChild(Declarator, ChildRole::Declarator);
					Declarator = Inner == NoNode ? Syntax.NamedChild(Declarator, 0) : Inner;
				}
				if(Declarator != NoNode && Syntax.GetNode(Declarator).Lexeme == Name) {
					Result = !IsAggregate && (IsPointer || IsArithmetic(Type));
				}
			}
			// 判定結果の返戻
			return Result;
		};
		// 使う位置から関数の見出へ遡る最も内側の宣言の探索
		NodeId Child = Operand;
		std::vector<NodeId> Path;
		bool IsBuiltin = false;
		for(NodeId Scope = Syntax.ParentOf(Operand); Scope != NoNode; Child = Scope, Scope = Syntax.ParentOf(Scope)) {
			Syntax.Charge(1);
			Path.push_back(Child);
			// 同じ位置から遡った判定の控えの再利用
			if(
				const std::unordered_map<NodeId, std::unordered_map<std::string, bool>>::const_iterator Cached = Memo.find(Child);
				Cached != Memo.end()
			) if(const std::unordered_map<std::string, bool>::const_iterator Hit = Cached->second.find(Name); Hit != Cached->second.end()) {
				IsBuiltin = Hit->second;
				break;
			}
			std::optional<bool> Found;
			// 使う文より前の宣言（後の宣言ほど内側）
			if(Syntax.Is(Scope, Role::CompoundBody)) for(const NodeId Statement : Syntax.ChildrenOf(Scope)) {
				Syntax.Charge(1);
				if(Statement == Child) break;
				if(Syntax.Is(Statement, Role::TypedDeclaration)) if(const std::optional<bool> Here = Declares(Statement)) Found = Here;
			} else if(Syntax.Is(Scope, Role::CountedLoop)) {
				// 初期化の宣言
				if(const NodeId Init = Syntax.NamedChild(Scope, 0); Syntax.Is(Init, Role::TypedDeclaration)) Found = Declares(Init);
			} else if(Syntax.Is(Scope, Role::ForRangeLoop)) Found = Declares(Scope);
			else if(Syntax.Is(Scope, Role::FunctionDefinition)) {
				// 仮引数の宣言
				NodeId Declarator = Syntax.FieldChild(Scope, ChildRole::Declarator);
				while(Declarator != NoNode && !Syntax.Is(Declarator, Role::FunctionDeclarator)) {
					Syntax.Charge(1);
					Declarator = Syntax.FieldChild(Declarator, ChildRole::Declarator);
				}
				// 関数の宣言子を持たない定義は型を確かめない
				if(Declarator != NoNode) for(const NodeId Parameter : Syntax.ChildrenOf(Syntax.FieldChild(Declarator, ChildRole::Parameters))) {
					Syntax.Charge(1);
					if(Syntax.Is(Parameter, Role::ParameterDeclaration)) if(const std::optional<bool> Here = Declares(Parameter)) Found = Here;
				}
				// 関数の中に宣言の無い名前（成員・大域）は型を確かめない
				Found = Found.value_or(false);
			} else if(Syntax.Is(Scope, Role::FunctionScope) || Syntax.Is(Scope, Role::ClassBody)) Found = false;
			if(Found) {
				IsBuiltin = *Found;
				break;
			}
		}
		// 探索経路への判定結果の保存
		for(const NodeId Position : Path) Memo[Position][Name] = IsBuiltin;
		// 最も内側の宣言の判定結果の返戻
		return IsBuiltin;
	}

	/**
	 * 値を使わない位置の後置の増減の前置化の収集関数（整形仕様 5.2）
	 * 文の位置と for の更新の位置の後置の増減の演算子を前へ移す（前置を持たない Go と，値が結果に為る文の並びの最後の文は除く）
	 * @param Syntax 構文モデル
	 * @param Id 節点
	 * @param Values 値が使われる枝を持つ式と文の並びの文
	 * @param Memo 名前と探索位置毎の判定の控え
	 * @param Plan 書換の計画の格納先
	 */
	void SyntaxNormalizer::CollectIncrementStep(
		const SyntaxModel &Syntax,
		const NodeId Id,
		ValueUse &Values,
		std::unordered_map<NodeId, std::unordered_map<std::string, bool>> &Memo,
		RewritePlan &Plan
	) {
		// 後置の増減と多重定義の有り得る C++ の組込でない被演算子以外の終了
		if(
			!Syntax.Is(Id, Role::PrefixableIncrement) ||
			Syntax.Is(Id, Role::OverloadableIncrement) && !IsBuiltinScalarName(Syntax, Syntax.NamedChild(Id, 0), Memo)
		) return;
		const uint32_t Count = Syntax.ChildCount(Id);
		const NodeId Operator = Syntax.Child(Id, Count - 1);
		// 増減の後置の演算子で終わらない場合の終了
		if(Count < 2 || Syntax.IsNamed(Operator) || !Syntax.IsTokenRole(Operator, Role::ChangeByOneToken)) return;
		// 要素１つの括弧を外側へ辿った式と其の親
		NodeId Expression = Id;
		for(
			NodeId Up = Syntax.ParentOf(Id);
			Syntax.Is(Up, Role::GroupingParen) && Syntax.NamedChildCount(Up) == 1 && Syntax.ChildCount(Up) == 3;
			Up = Syntax.ParentOf(Up)
		) Expression = Up;
		const NodeId Host = Syntax.ParentOf(Expression);
		// 値が使われる位置の終了
		if(
			!IsValueUnusedContext(Syntax, Expression, Host) || Syntax.Is(Host, Role::ValueStatementSequence) && Values.Contains(Expression)
		) return;
		// Java の switch 式の規則の値は，switch が文の位置に在る時だけ使われない
		if(Syntax.Is(Syntax.ParentOf(Host), Role::ValueSwitchRule)) {
			// switch 式が文の位置に無い場合の終了
			if(!Syntax.Is(Syntax.ParentOf(Syntax.ParentOf(Syntax.ParentOf(Syntax.ParentOf(Host)))), Role::DiscardedBranchValueHost)) return;
		}
		// 後置と前置の種別を分ける文法の前置の種別
		const KindDefinition *const PrefixDefinition = Syntax.Is(Id, Role::PostfixUnaryExpression) ?
		Syntax.Rules().PrefixUnaryDefinition :
		Syntax.Is(Id, Role::PostfixExpression) ? Syntax.Rules().PrefixDefinition : nullptr;
		if(PrefixDefinition) {
			// 前置の式への組直
			RewriteStep Prefix { RewriteKind::Reshape, Id };
			Prefix.Inserted = { {}, true, { { {}, false, {}, {}, Operator } }, 0, NoNode, {}, PrefixDefinition };
			for(uint32_t Index = 0; Index + 1 < Count; ++Index) {
				const NodeId Operand = Syntax.Child(Id, Index);
				Prefix.Inserted.Children.push_back({ {}, false, {}, Syntax.GetNode(Operand).Fields, Operand });
			}
			Plan.Steps.push_back(std::move(Prefix));
			// 終了
			return;
		}
		RewriteStep Move { RewriteKind::Move, Operator };
		Move.Destination = Id;
		Move.Index = 0;
		Plan.Steps.push_back(std::move(Move));
		// 終了
		return;
	}

	/**
	 * 値を使わない位置の後置の増減の前置化の一括収集関数（整形仕様 5.2）
	 * 括弧の要否を前置化した後の木で判ずる為に，他の節点毎の正規化より先の計画として集める（構文誤りと Rust のマクロの字句列の中は書き換えない）
	 * @param Syntax 構文モデル
	 * @param Plan 書換の計画の格納先
	 */
	void SyntaxNormalizer::CollectIncrementSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const {
		// 値が使われる枝を持つ式と文の並びの文
		ValueUse Values { *this, Syntax };
		std::unordered_map<NodeId, std::unordered_map<std::string, bool>> Memo;
		for(const NodeId Id : Candidates(Syntax, { Role::PrefixableIncrement }, { Role::Error, Role::PreservedTokenSequence })) {
			CollectIncrementStep(Syntax, Id, Values, Memo, Plan);
		}
		// 終了
		return;
	}

	/**
	 * 生成形式の部品から節点を組み立てる関数
	 * @param Syntax 構文モデル
	 * @param Form 文法の生成形式
	 * @param Role 組み立てる部品の役割
	 * @param Body 移す本体
	 * @param HasBooleanNames 真偽値の名前を使えるか
	 * @return 部品と其の子を組み立てた節点
	 * @throws FormatLimitExceeded 生成形式が役割の部品を持たない場合（文法の定義の不具合）
	 */
	static GeneratedNode GeneratedPart(
		const SyntaxModel &Syntax,
		const CanonicalForm &Form,
		const FormPartRole Role,
		const NodeId Body,
		const bool HasBooleanNames
	) {
		const FormPart *const Part = Form.PartOf(Role);
		if(!Part) throw FormatLimitExceeded("a generated form has no part for the role", FailureReason::FormatterDefect);
		GeneratedNode Result;
		Result.IsNamed = Part->IsNamed;
		Result.Definition = Part->Definition;
		if(Part->IsNamed) Result.Lexeme = Part->FixedLexeme;
		else Result.Kind = Part->FixedLexeme;
		if(Part->Field != ChildRole::None) Result.Fields = Syntax.FieldBit(Part->Field);
		if(Role == FormPartRole::Body) Result.Existing = Body;
		for(const FormPart &Child : Form.Parts) {
			if(
				Child.Role == Role || Child.Parent != Role || Form.Operation == FormOperation::UnconditionalLoop &&
				(Child.Role == FormPartRole::IntegerValue && HasBooleanNames || Child.Role == FormPartRole::TrueValue && !HasBooleanNames)
			) continue;
			Result.Children.push_back(GeneratedPart(Syntax, Form, Child.Role, Body, HasBooleanNames));
		}
		// 部品から構築した節点の返戻
		return Result;
	}

	/**
	 * 無条件の反復の正規化の収集関数（整形仕様 5.2）
	 * Go の `for true {` の条件を除いて `for {` へ揃え，節の無い `for(;;)` を `while(true)`（C の真偽値の名前の無い位置は `while(1)`）へ，真偽値の名前を持つ言語の `while(1)` を `while(true)` へ，Rust の `while true` を `loop` へ揃える（除く字句の錨は繰返の前へ移す）
	 * @param Syntax 構文モデル
	 * @param Id 節点
	 * @param Plan 書換の計画の格納先
	 */
	void SyntaxNormalizer::CollectLoopStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) {
		const NodeId Body = Syntax.FieldChild(Id, ChildRole::Body);
		const AnchorTarget Head { Id, false };
		// 真の値の名前
		const std::string True(Syntax.Rules().Spelling(Word::True));
		if(Syntax.Is(Id, Role::CapitalBooleanLoop)) {
			if(
				const NodeId Condition = Syntax.FieldChild(Id, ChildRole::Condition);
				Syntax.Is(Condition, Role::IntegerValue) && Syntax.IsWord(Condition, Word::IntegerTruth)
			) {
				RewriteStep Literal { RewriteKind::Reshape, Condition };
				Literal.Inserted = { {}, true, {}, 0, NoNode, True, Syntax.Rules().TrueDefinition };
				Plan.Steps.push_back(std::move(Literal));
				Plan.Steps.push_back({ RewriteKind::SetLexeme, Condition, True });
			}
			// 終了
			return;
		}
		if(Syntax.Is(Id, Role::UnconditionalLoopKeyword)) {
			const NodeId Cond = Syntax.FieldChild(Id, ChildRole::Condition);
			// 真の条件の while 以外の終了
			if(
				!Syntax.Is(Id, Role::WhileExpression) || !Syntax.Is(Cond, Role::BooleanLiteral) || !Syntax.IsWord(Cond, Word::True) ||
				Body == NoNode
			) return;
			// ラベルと本体を保った loop への組直
			RewriteStep Loop { RewriteKind::Reshape, Id };
			Loop.Inserted = { {}, true, {}, 0, NoNode, {}, Syntax.Rules().LoopExpressionDefinition };
			for(const NodeId Child : Syntax.ChildrenOf(Id)) {
				if(Child == Cond || Syntax.Is(Child, Role::KeywordWhile)) continue;
				if(Child == Body) Loop.Inserted.Children.push_back({ std::string(Syntax.Rules().Spelling(Word::Loop)) });
				Loop.Inserted.Children.push_back({ {}, false, {}, Syntax.GetNode(Child).Fields, Child });
			}
			Loop.Transfer = { NoNode, Head, Head, Head };
			Plan.Steps.push_back(std::move(Loop));
			// 終了
			return;
		}
		// 真偽値の名前の綴りを持つ言語の真の条件の字句（C と C からも読むヘッダは <stdbool.h> の取込の後だけ）
		const bool HasBooleanNames = !Syntax.Rules().BooleanNameNeedsDeclaration || Syntax.GetNode(Id).Facts.HasBooleanNames;
		const GeneratedNode TrueLiteral = GeneratedNode{ {}, true, {}, 0, NoNode, True, Syntax.Rules().TrueDefinition };
		const bool HasParenthesizedCondition = Syntax.Is(Id, Role::ParenthesizedLoopCondition);
		if(HasParenthesizedCondition && Syntax.Is(Id, Role::Loop) && !Syntax.Is(Id, Role::CountedLoop) && HasBooleanNames) {
			// 条件の括弧の内側の整数の `1` の真偽値の名前への置換（整数を条件に置けない言語は整数の字句の種別が異なる）
			const NodeId Wrapped = Syntax.FieldChild(Id, ChildRole::Condition);
			const NodeId Inner = Syntax.NamedChildCount(Wrapped) == 1 ? Syntax.NamedChild(Wrapped, 0) : NoNode;
			// 整数の `1` の条件以外の終了
			if(!Syntax.Is(Inner, Role::NumericToken) || !Syntax.IsWord(Inner, Word::IntegerTruth)) return;
			// 字句の種別の組直と字面の置換
			RewriteStep Literal { RewriteKind::Reshape, Inner };
			Literal.Inserted = TrueLiteral;
			Plan.Steps.push_back(std::move(Literal));
			Plan.Steps.push_back({ RewriteKind::SetLexeme, Inner, True });
			// 終了
			return;
		}
		// for 以外の終了
		if(!Syntax.Is(Id, Role::CountedLoop)) return;
		if(HasParenthesizedCondition) {
			// 初期化・条件・更新の節を持つ for の終了（空文の節は節を持たない）
			if(
				Body == NoNode || AnyChildOf(
					Syntax,
					Id,
					[&Syntax, Body](const NodeId Child) -> bool {
						// 本体・空文以外の名前付の子かの返戻
						return Child != Body && Syntax.IsNamed(Child) && !Syntax.Is(Child, Role::EmptyStatement);
					}
				)
			) return;
			RewriteStep Loop { RewriteKind::Reshape, Id };
			const CanonicalForm *const Form = GeneratedFormOf(Syntax, FormOperation::UnconditionalLoop);
			// 無条件の繰返の生成形式を持たない文法の場合の終了
			if(!Form) return;
			Loop.Inserted = GeneratedPart(Syntax, *Form, FormPartRole::Result, Body, HasBooleanNames);
			// for と while は同じ単一文の本体を持ち，包装の生成形式を引き継ぐ
			Loop.Inserted.Facts.Forms = Syntax.GetNode(Id).Facts.Forms;
			Loop.Transfer = { NoNode, Head, Head, Head };
			Plan.Steps.push_back(std::move(Loop));
			// 終了
			return;
		}
		// Go 以外の終了
		if(!Syntax.Is(Id, Role::ClauseOrConditionLoop)) return;
		const NodeId Cond = Syntax.NamedChild(Id, 0);
		// 条件だけを持つ for の真の条件以外の終了
		if(!Syntax.Is(Cond, Role::TrueValue) || Cond == Body) return;
		const AnchorTarget Keyword { Syntax.PrevSibling(Cond), true };
		Plan.Steps.push_back({ RewriteKind::Remove, Cond, {}, { Cond, Keyword, Keyword, Keyword } });
		// 終了
		return;
	}

	/**
	 * 真偽値型の初期値の真偽値の名前への正規化の収集関数（整形仕様 5.2）
	 * C 系の `bool`（C の `_Bool`）の宣言で素の名前の宣言子の初期値 `0` / `1` を `false` / `true` へ揃える（C は <stdbool.h> の取込の後だけ）
	 * @param Syntax 構文モデル
	 * @param Id 節点
	 * @param Plan 書換の計画の格納先
	 */
	void SyntaxNormalizer::CollectBoolLiteralStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) {
		// 真偽値の名前を綴れない位置と C 系の宣言以外の終了
		if(!Syntax.Is(Id, Role::BooleanInitializerDeclaration) || !Syntax.GetNode(Id).Facts.HasBooleanNames) return;
		const NodeId Type = Syntax.FieldChild(Id, ChildRole::Type);
		// bool 型でない宣言の終了
		if(!((Syntax.Is(Type, Role::PrimitiveType) || Syntax.Is(Type, Role::TypeName)) && Syntax.IsWord(Type, Word::Boolean))) return;
		// 整数の `0` / `1` の値の真偽値の名前への組直
		const auto Rewrite = [&Syntax, &Plan](const NodeId Value) -> void {
			const bool IsTrue = Syntax.IsWord(Value, Word::IntegerTruth);
			// 整数の `0` / `1` 以外の終了
			if(!Syntax.Is(Value, Role::NumberLiteral) || !IsTrue && !Syntax.IsWord(Value, Word::IntegerZero)) return;
			const std::string_view Name = Syntax.Rules().Spelling(IsTrue ? Word::True : Word::False);
			RewriteStep Literal { RewriteKind::Reshape, Value };
			Literal.Inserted = { {}, true, {}, 0, NoNode, {}, IsTrue ? Syntax.Rules().TrueDefinition : Syntax.Rules().FalseDefinition };
			Plan.Steps.push_back(std::move(Literal));
			Plan.Steps.push_back({ RewriteKind::SetLexeme, Value, std::string(Name) });
		};
		// 成員の宣言は宣言子で包まずに初期値を名前の後の子に持つ
		NodeId Previous = NoNode;
		ForEachNamedChildOf(
			Syntax,
			Id,
			[&](const NodeId Child) -> void {
				if(Syntax.Is(Previous, Role::FieldName)) Rewrite(Child);
				// 素の名前の宣言子の末尾の初期値（間接型の `0` を真偽値へ変えない）
				else if(
					Syntax.Is(Child, Role::InitDeclarator) && Syntax.NamedChildCount(Child) > 1 &&
					Syntax.Is(Syntax.NamedChild(Child, 0), Role::Identifier)
				) Rewrite(Syntax.LastNamedChild(Child));
				Previous = Child;
			}
		);
		// 終了
		return;
	}

	/**
	 * 型の後の const の型の前への移動の収集関数（整形仕様 5.2）
	 * C 系の宣言で最初の型指定子の後に在る最初の `const` を其の型指定子の直前へ移す（属性・記憶域の前へは出さず，宣言子の記号へ展開するマクロの型は保つ）
	 * @param Syntax 構文モデル
	 * @param Id 節点
	 * @param Plan 書換の計画の格納先
	 */
	void SyntaxNormalizer::CollectConstPositionStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) {
		// C 系の宣言以外の終了
		if(!Syntax.Is(Id, Role::LeadingConstDeclaration) || Syntax.GetNode(Id).Facts.Has(ConstraintKind::AmbiguousDeclaration)) return;
		NodeId Type = NoNode, Qualifier = NoNode;
		ForEachNamedChildOf(
			Syntax,
			Id,
			[&](const NodeId Child) -> void {
				if(const bool IsTypeSpecifier = Syntax.Is(Child, Role::DeclarationTypeSpecifier); IsTypeSpecifier && Type == NoNode) {
					Type = Child;
				} else if(
					!IsTypeSpecifier && Syntax.Is(Child, Role::TypeQualifier) && Type != NoNode && Qualifier == NoNode &&
					Syntax.IsWord(Child, Word::ConstQualifier)
				) Qualifier = Child;
			}
		);
		// 型の後の const が無い宣言と宣言子の記号へ展開するマクロの型の終了
		if(Qualifier == NoNode || Syntax.GetNode(Type).Facts.Has(ConstraintKind::DeclaratorMacro)) return;
		RewriteStep Move { RewriteKind::Move, Qualifier };
		Move.Destination = Id;
		Move.Index = Syntax.GetNode(Type).Index;
		Move.Boundary = BoundaryKind::Spaced;
		Plan.Steps.push_back(std::move(Move));
		// 終了
		return;
	}

	/**
	 * 祖先に構文誤りが在るかの判定を伴う節点毎の正規化の収集関数（整形仕様 5.1, 5.2）
	 * 数値・色・文字列の字形，CSS の宣言末尾と常に真の条件の反復を揃え，構文誤りと Rust のマクロの字句列の中の字句は書き換えない（整形仕様 4.4, 4.6）
	 * 役割索引又は作業列の候補を構文順に調べ，必要な祖先だけを辿る
	 * @param Syntax 構文モデル
	 * @param Plan 書換の計画の格納先
	 */
	void SyntaxNormalizer::CollectNodeSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const {
		// 候補の祖先での誤りと字句列の保護状態の控え
		const bool ShouldPad = Syntax.Rules().PadDecimalPoint;
		std::unordered_map<NodeId, bool> Sealed;
		for(
			const NodeId Id :
			Candidates(
				Syntax,
				{
					Role::Annotation,
					Role::ArgumentSequence,
					Role::ArrowFunction,
					Role::Attribute,
					Role::BareArgumentNotation,
					Role::BooleanInitializerDeclaration,
					Role::CallSuffix,
					Role::CapitalBooleanLoop,
					Role::ClassDeclaration,
					Role::ClauseOrConditionLoop,
					Role::Construction,
					Role::CountedLoop,
					Role::EnumEntry,
					Role::LambdaExpression,
					Role::LambdaFunctionType,
					Role::LambdaParameters,
					Role::LeadingConstDeclaration,
					Role::MethodParameters,
					Role::NamedImports,
					Role::NormalizedNumber,
					Role::NormalizedQuote,
					Role::ObjectCreationExpression,
					Role::OptionalAnnotationArguments,
					Role::OptionalConstructionArguments,
					Role::OptionalEmptySuperclass,
					Role::OptionalLambdaDeclarator,
					Role::OptionalVoidParameter,
					Role::ParenthesizedLoopCondition,
					Role::StyleLexeme,
					Role::TypeofComparison,
					Role::UnconditionalLoopKeyword
				},
				{}
			)
		) {
			std::vector<NodeId> Path;
			NodeId Up = Syntax.ParentOf(Id);
			while(Up != NoNode && !Sealed.contains(Up)) {
				Syntax.Charge(1);
				Path.push_back(Up);
				Up = Syntax.ParentOf(Up);
			}
			if(Up != NoNode) Syntax.Charge(1);
			bool IsSealed = Up != NoNode && Sealed.at(Up);
			for(std::vector<NodeId>::const_reverse_iterator It = Path.rbegin(); It != Path.rend(); ++It) {
				const NodeId Ancestor = *It;
				IsSealed |= Syntax.Is(Ancestor, Role::Error) || Syntax.Is(Ancestor, Role::PreservedTokenSequence);
				Sealed.emplace(Ancestor, IsSealed);
			}
			const SyntaxNode &Node = Syntax.GetNode(Id);
			if(!IsSealed) {
				// 字面の正規化は名前付の値だけ（型の字句 `float`・`string` 等の無名の字句は値でない）
				if(Syntax.Is(Id, Role::NormalizedNumber) && !Node.Facts.Has(ConstraintKind::Verbatim)) {
					if(std::string Fixed = NormalizeNumericLiteral(Syntax.Rules(), Node.Lexeme, ShouldPad); Fixed != Node.Lexeme) {
						Plan.Steps.push_back({ RewriteKind::SetLexeme, Id, std::move(Fixed) });
					}
				}
				if(Syntax.Is(Id, Role::StyleLexeme)) CollectCssStep(Syntax, Id, Plan);
				if(Syntax.Is(Id, Role::NormalizedQuote) && Node.Children.empty()) CollectQuoteStep(Syntax, Id, Plan);
				// 文末の字句の読違えを防ぐ包装と引数の調整
				if(Syntax.Is(Id, Role::BareArgumentNotation)) {
					const NodeId Parent = Syntax.ParentOf(Id);
					NodeId Statement = Id;
					for(
						NodeId Up = Syntax.ParentOf(Statement);
						Up != NoNode && !Syntax.Is(Up, Role::StatementSequence) && Syntax.ParentOf(Up) != NoNode && !Syntax.Is(Up, Role::KeywordBlock);
						Up = Syntax.ParentOf(Statement)
					) {
						// 祖先の１段の検査を１作業単位とする
						Syntax.Charge(1);
						Statement = Up;
					}
					const bool HasFollowing = Syntax.NextNamedSibling(Statement) != NoNode;
					if(Syntax.Is(Id, Role::KeywordWhile)) {
						if(const NodeId Condition = Syntax.FieldChild(Id, ChildRole::Condition); Syntax.Is(Condition, Role::IntegerValue)) {
							RewriteStep Step { RewriteKind::Reshape, Condition };
							Step.Inserted = { {}, true, {}, Syntax.GetNode(Condition).Fields, NoNode, {}, Syntax.Rules().TrueDefinition };
							Plan.Steps.push_back(std::move(Step));
							Plan.Steps.push_back({ RewriteKind::SetLexeme, Condition, std::string(Syntax.Rules().Spelling(Word::True)) });
						}
					}
					if(
						Syntax.Is(Id, Role::Range) && !Syntax.IsNamed(Syntax.Child(Id, Syntax.ChildCount(Id) - 1)) &&
						(Syntax.Is(Parent, Role::AssignmentExpression) && HasFollowing || Syntax.Is(Parent, Role::PatternMembership))
					) {
						RewriteStep Wrap { RewriteKind::Wrap, Id };
						Wrap.Inserted = {
							{},
							true,
							{
								{ GeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallOpen) },
								{ {}, false, {}, 0, Id },
								{ GeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallClose) }
							},
							0,
							NoNode,
							{},
							Syntax.Rules().ParenthesizedStatementsDefinition
						};
						// 包んだ範囲の外の錨は括弧の外へ移す
						Wrap.Transfer.From = Id;
						Plan.Steps.push_back(std::move(Wrap));
					}
					if(Syntax.Is(Id, Role::ArgumentSequence) && !Syntax.IsTokenRole(Syntax.Child(Id, 0), Role::OpeningParenToken)) {
						const NodeId Last = Syntax.NamedChild(Id, Syntax.NamedChildCount(Id) - 1);
						const std::string Text = Syntax.Text(Last);
						if(
							HasFollowing && (
								Syntax.IsWord(Last, Word::BareCapture) ||
								Text.ends_with(GeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::ValueDelimiter))
							)
						) {
							RewriteStep Close { RewriteKind::Insert, Id };
							Close.Index = Syntax.ChildCount(Id);
							Close.Inserted = { GeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallClose) };
							Plan.Steps.push_back(std::move(Close));
							RewriteStep Open { RewriteKind::Insert, Id };
							Open.Index = 0;
							Open.Inserted = { GeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallOpen) };
							Plan.Steps.push_back(std::move(Open));
						}
					}
					const bool IsParameters = Syntax.Is(Id, Role::MethodParameters) || Syntax.Is(Id, Role::LambdaParameters);
					const bool IsEmpty = !Syntax.NamedChildCount(Id);
					const auto IsReceiverCall = [&Syntax, Parent]() -> bool {
						// 受け手の呼出でない事の返戻
						if(!Syntax.Is(Parent, Role::ReceiverCall) || Syntax.FieldChild(Parent, ChildRole::Object) == NoNode) return false;
						const NodeId Function = Syntax.FieldChild(Parent, ChildRole::Function);
						// 被呼出側が定数名でない事の返戻
						return Function != NoNode && !Syntax.Is(Function, Role::ConstantName);
					};
					if(
						IsEmpty && (IsParameters || Syntax.Is(Id, Role::ArgumentSequence) && (Syntax.Is(Parent, Role::YieldValue) || IsReceiverCall()))
					) {
						const AnchorTarget Target { Parent, false };
						Plan.Steps.push_back({ RewriteKind::Remove, Id, {}, { Id, Target, Target, Target } });
					} else if(IsParameters) {
						const bool HasParens = Syntax.IsTokenRole(Syntax.Child(Id, 0), Role::OpeningParenToken);
						bool IsSimple = true, HasBareCapture = false;
						for(const NodeId Child : Syntax.ChildrenOf(Id)) if(Syntax.IsNamed(Child)) {
							IsSimple = IsSimple && Syntax.Is(Child, Role::Identifier);
							HasBareCapture = HasBareCapture || Syntax.IsWord(Child, Word::BareCapture);
						}
						if(Syntax.Is(Id, Role::LambdaParameters) && HasParens && IsSimple) {
							Plan.Steps.push_back({ RewriteKind::Remove, Syntax.Child(Id, 0) });
							Plan.Steps.push_back({ RewriteKind::Remove, Syntax.Child(Id, Syntax.ChildCount(Id) - 1) });
						} else if(Syntax.Is(Id, Role::MethodParameters) && !HasParens && HasBareCapture) {
							RewriteStep Close { RewriteKind::Insert, Id };
							Close.Index = Syntax.ChildCount(Id);
							Close.Inserted = { GeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallClose) };
							Plan.Steps.push_back(std::move(Close));
							RewriteStep Open { RewriteKind::Insert, Id };
							Open.Index = 0;
							Open.Inserted = { GeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallOpen) };
							Plan.Steps.push_back(std::move(Open));
						}
					}
				}
				// typeof の比較演算子の厳密な形への置換
				if(Syntax.Is(Id, Role::TypeofComparison)) {
					NodeId Left = Syntax.FieldChild(Id, ChildRole::Left), Right = Syntax.FieldChild(Id, ChildRole::Right);
					while(Syntax.Is(Left, Role::GroupingParen)) Left = Syntax.NamedChild(Left, 0);
					while(Syntax.Is(Right, Role::GroupingParen)) Right = Syntax.NamedChild(Right, 0);
					const bool IsLeftTypeof = Syntax.HasWordChild(Left, Word::TypeQuery);
					const bool IsRightTypeof = Syntax.HasWordChild(Right, Word::TypeQuery);
					const NodeId Operator = Syntax.FieldChild(Id, ChildRole::Operator);
					const std::string OperatorText = Syntax.Text(Operator);
					if(
						const SyntaxRules::OperatorNotation &Operators = Syntax.Rules().Operators;
						(OperatorText == Operators.Equal || OperatorText == Operators.NotEqual) && (
							IsLeftTypeof && (IsRightTypeof || Syntax.Is(Right, Role::QuotedString)) || IsRightTypeof && Syntax.Is(Left, Role::QuotedString)
						)
					) {
						const std::string Strict(OperatorText == Operators.Equal ? Operators.StrictEqual : Operators.StrictNotEqual);
						RewriteStep Step { RewriteKind::Reshape, Operator };
						Step.Inserted = { Strict };
						Plan.Steps.push_back(std::move(Step));
						Plan.Steps.push_back({ RewriteKind::SetLexeme, Operator, Strict });
					}
				}
				// 名前付の取込の辞書順への整列
				if(Syntax.Is(Id, Role::NamedImports)) {
					std::vector<NodeId> Names;
					for(const NodeId Child : Node.Children) if(Syntax.IsNamed(Child)) Names.push_back(Child);
					std::stable_sort(
						Names.begin(),
						Names.end(),
						[&](const NodeId Left, const NodeId Right) -> bool {
							// 取込名の辞書順の返戻
							const std::string LeftName = Syntax.Text(Left), RightName = Syntax.Text(Right);
							std::string FoldedLeft = LeftName, FoldedRight = RightName;
							ToLowerAscii(FoldedLeft);
							ToLowerAscii(FoldedRight);
							// 大文字・小文字を揃えた名前と元の綴りの順の返戻
							return std::tie(FoldedLeft, LeftName) < std::tie(FoldedRight, RightName);
						}
					);
					size_t Index = 0;
					bool IsOrdered = true;
					for(const NodeId Child : Node.Children) if(Syntax.IsNamed(Child)) IsOrdered = IsOrdered && Child == Names[Index++];
					if(!IsOrdered) {
						RewriteStep Sort { RewriteKind::Reshape, Id };
						Sort.Inserted = { {}, true, {}, 0, NoNode, {}, Node.Kind.Definition };
						Sort.Inserted.Facts = Node.Facts;
						Index = 0;
						for(const NodeId Child : Node.Children) {
							Sort.Inserted.Children.push_back(
								{ {}, false, {}, Syntax.GetNode(Child).Fields, Syntax.IsNamed(Child) ? Names[Index++] : Child }
							);
						}
						Plan.Steps.push_back(std::move(Sort));
					}
				}
				// 節点毎の残る正規化規則の適用
				CollectLoopStep(Syntax, Id, Plan);
				CollectBoolLiteralStep(Syntax, Id, Plan);
				CollectConstPositionStep(Syntax, Id, Plan);
				CollectOptionalParenStep(Syntax, Id, Plan);
			}
		}
		// 終了
		return;
	}

	/**
	 * 単なる区切りの末尾カンマの除去の収集関数（整形仕様 5.2）
	 * 閉じの直前（Java の列挙の宣言部の前を含む）のカンマは要素を区切らない為に除き，其のカンマの錨は前の要素の後へ移す
	 * 単一要素の組のカンマと配列の空要素のカンマ，CSS の空の代替値のカンマは値を表す為に保つ（Go の折返に要るカンマは中間表現が出す）
	 * @param Syntax 構文モデル
	 * @param Plan 書換の計画の格納先
	 */
	void SyntaxNormalizer::CollectTrailingCommaSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const {
		// CSS の `var(--x,)` のカンマは空の代替値を表す為の終了
		if(Syntax.Rules().TrailingCommaHasEmptyValue) return;
		std::vector<NodeId> Hosts = ParentsOfRole(Syntax, Role::CommaToken, { Role::Error, Role::PreservedTokenSequence });
		const std::vector<NodeId> BareLists =
		Candidates(Syntax, { Role::BareListTrailingComma }, { Role::Error, Role::PreservedTokenSequence });
		Hosts.insert(Hosts.end(), BareLists.begin(), BareLists.end());
		Hosts = InDocumentOrder(Syntax, Hosts, {});
		for(const NodeId Id : Hosts) {
			const std::span<const NodeId> Children = Syntax.ChildrenOf(Id);
			// 組の型を決める唯一のカンマか
			uint32_t TupleCommas = 0;
			if(
				Syntax.Is(Id, Role::TupleLike) || Syntax.Is(Id, Role::TupleTypeComma) && (
					Syntax.Is(Id, Role::Subscript) ||
					Syntax.Is(Id, Role::GenericParameter) && Syntax.Is(Syntax.ParentOf(Id), Role::AppliedGenericType) &&
					!Syntax.HasField(Syntax.ParentOf(Syntax.ParentOf(Id)), ChildRole::Left)
				)
			) for(const NodeId Child : Children) TupleCommas += Syntax.IsTokenRole(Child, Role::CommaToken);
			// 括弧の無い並びの末尾のカンマの除去
			if(
				Syntax.Is(Id, Role::BareListTrailingComma) && !Children.empty() && Syntax.IsTokenRole(Children.back(), Role::CommaToken) && (
					Syntax.Is(Id, Role::ExpressionList) || Syntax.Is(Id, Role::PatternList) || Syntax.Is(Id, Role::LambdaParameters) ||
					Syntax.Is(Id, Role::TupleExpression)
				) && (Syntax.NamedChildCount(Id) > 1 || Syntax.Is(Id, Role::LambdaParameters))
			) {
				const NodeId Comma = Children.back();
				const AnchorTarget Target { Children[Children.size() - 2], true };
				Plan.Steps.push_back({ RewriteKind::Remove, Comma, {}, { Comma, Target, Target, Target } });
			}
			// 閉じの前の区切りだけのカンマの除去
			for(uint32_t Index = 1; Index + 1 < Children.size(); ++Index) {
				const NodeId Comma = Children[Index], Next = Children[Index + 1];
				if(Syntax.IsNamed(Comma) || !Syntax.IsTokenRole(Comma, Role::CommaToken)) continue;
				const NodeId NextLead = FirstLeaf(Syntax, Next);
				if(
					const bool IsClosing = !Syntax.IsNamed(Next) && Syntax.IsTokenRole(NextLead, Role::ClosingBracketLead);
					!IsClosing && !Syntax.Is(Next, Role::EnumBodyDeclarations) || TupleCommas == 1 &&
					(!Syntax.Rules().SingletonCommaDeterminesTuple || Syntax.NamedChildCount(Id) == (Syntax.Is(Id, Role::Subscript) ? 2 : 1))
				) continue;
				// 配列の空要素を表すカンマの保持
				if(
					Syntax.IsTokenRole(NextLead, Role::ClosingSquareLead) && (
						Syntax.IsTokenRole(Children[Index - 1], Role::CommaToken) || Syntax.IsTokenRole(Children[Index - 1], Role::OpeningBracketToken)
					)
				) continue;
				const AnchorTarget Previous { Children[Index - 1], true };
				Plan.Steps.push_back({ RewriteKind::Remove, Comma, {}, { Comma, Previous, Previous, Previous } });
			}
		}
		// 終了
		return;
	}

	/**
	 * 改行が代替する文・成員のセミコロンの除去の収集関数
	 * @param Syntax 構文モデル
	 * @param Plan 書換の計画の格納先
	 */
	void SyntaxNormalizer::CollectSeparatorSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const {
		// 最上位の空の式文 `;`（誤読された宣言の終端に為る誤り区画の直後は保つ）
		const std::vector<NodeId> RootChildren = WorkNodes.empty() ?
		std::vector<NodeId>(Syntax.ChildrenOf(Syntax.GetRoot()).begin(), Syntax.ChildrenOf(Syntax.GetRoot()).end()) :
		InDocumentOrder(Syntax, WorkNodes, {});
		Syntax.Charge(RootChildren.size());
		for(const NodeId Child : RootChildren) {
			if(Syntax.ParentOf(Child) != Syntax.GetRoot() || !Syntax.IsNamed(Child)) continue;
			const NodeId PreviousCode = Syntax.PrevNamedSibling(Child);
			if(
				Syntax.Is(Child, Role::ExpressionStatement) && Syntax.IsWord(Child, Word::StatementEnd) &&
				(PreviousCode == NoNode || !Syntax.GetNode(PreviousCode).Facts.Has(ConstraintKind::ContainsError))
			) {
				// 錨は前の文の後，前が無ければ次の文の前へ移す
				const NodeId Next = Syntax.NextNamedSibling(Child);
				const AnchorTarget Target = PreviousCode != NoNode ?
				AnchorTarget{ PreviousCode, true } :
				Next != NoNode ? AnchorTarget{ Next, false } : AnchorTarget{ Syntax.GetRoot(), false };
				Plan.Steps.push_back({ RewriteKind::Remove, Child, {}, { Child, Target, Target, Target } });
			}
		}
		// 対象外の言語には改行で代える区切りが無い
		for(const NodeId Id : Candidates(Syntax, { Role::OptionalSemicolonHost }, { Role::Error })) {
			const std::span<const NodeId> Children = Syntax.ChildrenOf(Id);
			// 原文を保つ境界と除ける区切りの判定
			std::vector<uint8_t> Removed(Children.size(), 0), Preserved(Children.size() + 1, 0);
			for(const SemanticBoundary &Boundary : Syntax.GetNode(Id).Facts.Boundaries.View()) {
				if(Boundary.Kind == BoundaryKind::Preserved && Boundary.Before < Preserved.size()) Preserved[Boundary.Before] = 1;
			}
			for(uint32_t Index = 0; Index < Children.size(); ++Index) {
				const NodeId Child = Children[Index];
				const bool IsSeparator = Syntax.IsWord(Child, Word::StatementEnd) &&
				(!Syntax.IsNamed(Child) || Syntax.Is(Id, Role::EmptySemicolonSequence) && Syntax.Is(Child, Role::EmptyStatement));
				const bool IsProtected = Preserved[Index] || Preserved[Index + 1] || Index && Syntax.Is(Children[Index - 1], Role::Error) ||
				Index + 1 < Children.size() && Syntax.Is(Children[Index + 1], Role::Error);
				Removed[Index] = IsSeparator && !IsProtected;
			}
			// 除去後に続く子と空の文の並びの判定
			std::vector<NodeId> Following(Children.size(), NoNode);
			NodeId NextKept = NoNode;
			for(size_t Index = Children.size(); Index--;) {
				Following[Index] = NextKept;
				if(!Removed[Index]) NextKept = Children[Index];
			}
			const NodeId Parent = Syntax.ParentOf(Id);
			const bool IsOptionalList = Parent != NoNode && (
				Syntax.Is(Parent, Role::LexicalBlock) || Syntax.Is(Parent, Role::ExpressionCase) || Syntax.Is(Parent, Role::DefaultCase) ||
				Syntax.Is(Parent, Role::TypeCase) || Syntax.Is(Parent, Role::CommunicationCase)
			);
			const bool RemovesWholeList =
			Syntax.Is(Id, Role::PlainStatementSequence) && !Children.empty() && NextKept == NoNode && IsOptionalList;
			const AnchorTarget EmptyBody { Parent, false, true, Parent != NoNode && !Syntax.Is(Parent, Role::LexicalBlock) };
			// 残る子を行先とする区切りの除去
			RewriteStep Step { RewriteKind::RemoveSiblings, Id };
			NodeId PreviousKept = NoNode;
			for(uint32_t Index = 0; Index < Children.size(); ++Index) {
				const NodeId Child = Children[Index];
				if(!Removed[Index]) {
					PreviousKept = Child;
					continue;
				}
				const bool IsEmptyStatement = Syntax.Is(Child, Role::EmptyStatement);
				const AnchorTarget Target = RemovesWholeList ?
				EmptyBody :
				IsEmptyStatement && Following[Index] != NoNode ? AnchorTarget{ Following[Index], false } : PreviousKept != NoNode ?
				AnchorTarget{ PreviousKept, true } :
				Following[Index] != NoNode ? AnchorTarget{ Following[Index], false } : AnchorTarget{ Id, false };
				Step.RemovedSiblings.push_back({ Child, Target, Target, Target });
			}
			if(!Step.RemovedSiblings.empty()) Plan.Steps.push_back(std::move(Step));
			if(RemovesWholeList) Plan.Steps.push_back({ RewriteKind::Remove, Id, {}, { Id, EmptyBody, EmptyBody, EmptyBody } });
		}
		// 終了
		return;
	}

	/**
	 * 文・成員の終端と型本体の区切りの収集関数
	 * @param Syntax 構文モデル
	 * @param Plan 書換の計画の格納先
	 */
	void SyntaxNormalizer::CollectTerminatorSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const {
		const std::string Terminator = GeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::Terminator);
		const std::string Separator = GeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::Separator);
		for(
			const NodeId Id :
			Candidates(
				Syntax,
				{ Role::MemberBody, Role::TerminatedStatement, Role::TypeMemberBody },
				{ Role::Error, Role::PreservedTokenSequence }
			)
		) {
			// 区切りの挿入と置換の共通操作
			const auto Insert = [&](const NodeId Host, const uint32_t Index, const std::string &Token) -> void {
				RewriteStep Step { RewriteKind::Insert, Host };
				Step.Index = Index;
				Step.Inserted = { Token };
				Plan.Steps.push_back(std::move(Step));
				// 終了
				return;
			};
			const auto Replace = [&](const NodeId Token, const std::string &Text) -> void {
				RewriteStep Step { RewriteKind::Reshape, Token };
				Step.Inserted = { Text };
				Plan.Steps.push_back(std::move(Step));
				Plan.Steps.push_back({ RewriteKind::SetLexeme, Token, Text });
				// 終了
				return;
			};
			const std::span<const NodeId> Children = Syntax.ChildrenOf(Id);
			// 文の終端を欠く文への区切りの追加
			if(
				Syntax.Is(Id, Role::TerminatedStatement) && !Syntax.GetNode(Id).Facts.Has(ConstraintKind::EndsCode) && !Children.empty() &&
				!Syntax.IsWord(Children.back(), Word::StatementEnd)
			) {
				const NodeId Inner = Syntax.LastNamedChild(Id);
				const bool IsCompleteExport = Syntax.Is(Id, Role::Export) && (
					Syntax.Is(Inner, Role::TerminatedStatement) || Syntax.IsTokenRole(LastLexicalLeaf(Syntax, Id), Role::ClosingBraceToken) && (
						Syntax.Is(Inner, Role::Declaration) || Syntax.Is(Inner, Role::AmbientDeclaration) ||
						Syntax.FieldChild(Id, ChildRole::Value) != NoNode &&
						(Syntax.Is(Inner, Role::FunctionExpression) || Syntax.Is(Inner, Role::Class))
					)
				);
				if(!IsCompleteExport && !Syntax.Is(Inner, Role::Namespace) && !Syntax.GetNode(Id).Facts.Has(ConstraintKind::ContainsError)) {
					Insert(Id, static_cast<uint32_t>(Children.size()), Terminator);
				}
			}
			// 型本体と成員本体の区切りの調整
			if(Syntax.Is(Id, Role::MemberBody) || Syntax.Is(Id, Role::TypeMemberBody)) {
				const bool IsType = Syntax.Is(Id, Role::TypeMemberBody);
				for(uint32_t Index = static_cast<uint32_t>(Children.size()); Index--;) {
					const NodeId Child = Children[Index], Next = Index + 1 < Children.size() ? Children[Index + 1] : NoNode;
					if(Syntax.Is(Child, Role::Error) || Syntax.Is(Next, Role::Error)) continue;
					bool IsProtected = false;
					for(const SemanticBoundary &Boundary : Syntax.GetNode(Id).Facts.Boundaries.View()) {
						IsProtected |= Boundary.Kind == BoundaryKind::Preserved && (Boundary.Before == Index || Boundary.Before == Index + 1);
					}
					if(IsProtected) continue;
					if(IsType) {
						if(Syntax.IsWord(Child, Word::StatementEnd) || Syntax.IsTokenRole(Child, Role::CommaToken)) {
							if(!Index || !Syntax.Is(Children[Index - 1], Role::TypeMember)) continue;
							if(Syntax.IsTokenRole(Next, Role::ClosingBraceToken)) {
								const AnchorTarget Previous { Children[Index - 1], true };
								Plan.Steps.push_back({ RewriteKind::Remove, Child, {}, { Child, Previous, Previous, Previous } });
							} else if(Syntax.Is(Next, Role::TypeMember) && Syntax.IsWord(Child, Word::StatementEnd)) Replace(Child, Separator);
						} else if(Syntax.Is(Child, Role::TypeMember) && Syntax.Is(Next, Role::TypeMember)) Insert(Id, Index + 1, Separator);
					} else if(Syntax.Is(Child, Role::TerminatedMember)) {
						if(Syntax.IsTokenRole(Next, Role::CommaToken)) Replace(Next, Terminator);
						else if(!Syntax.IsWord(Next, Word::StatementEnd)) Insert(Id, Index + 1, Terminator);
					}
				}
			}
		}
		// 終了
		return;
	}

	/**
	 * 引数の無い返戻文の生成関数
	 * @param Syntax 構文モデル
	 * @return 言語の文法の返戻文の節点
	 * @throws FormatLimitExceeded 返戻文の生成形式が無い場合（文法の定義の不具合）
	 */
	GeneratedNode SyntaxNormalizer::ArglessReturn(const SyntaxModel &Syntax) {
		const CanonicalForm *const Form = GeneratedFormOf(Syntax, FormOperation::ArglessReturn);
		if(!Form) throw FormatLimitExceeded("missing generated argless return form", FailureReason::FormatterDefect);
		// 返戻文の返戻
		return GeneratedPart(Syntax, *Form, FormPartRole::Result, NoNode, true);
	}

	/**
	 * 通常関数の末尾の返戻の明示の収集関数（整形仕様 5.2）
	 * 値を返さない通常関数の到達可能な末尾へ言語の返戻文を補う（生成器・式本体・戻らない関数は除く）
	 * 暗黙返戻は一時値・借用・属性・結果構築子の影響を確かめた関数だけ明示する
	 * 計算量：候補の関数に含まれる節点数 V に対し，制御の流れの判定を含め O(V)
	 * @param Syntax 構文モデル
	 * @param Plan 書換の計画の格納先
	 * @throws FormatLimitExceeded 必要な返戻文の生成形式が無い場合（文法の定義の不具合）
	 */
	void SyntaxNormalizer::CollectExplicitReturnSteps(const SyntaxModel &Syntax, RewritePlan &Plan) const {
		const ControlFlow Flow(Syntax);
		const std::string TerminatorLexeme = GeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::Terminator);
		// 返戻の語
		const std::string ReturnWord(Syntax.Rules().Spelling(Word::Return));
		// Rust の展開前の属性の範囲の判定の控え
		std::unordered_map<NodeId, bool> AttributeProtection;
		for(const NodeId Id : Candidates(Syntax, { Role::ExplicitResultFunction }, {})) {
			const std::optional<FunctionEnd> End = Flow.ClassifyFunction(Id);
			// 通常関数でない節点と，戻らない関数・末尾へ到達しない関数・補完の前提を確かめれない関数の読飛
			if(
				!End || End->Kind == ReturnKind::Never || Flow.EndReach(Id, *End) != EndReachability::Reachable ||
				Flow.IsReturnCompletionUncertain(Id, *End, AttributeProtection)
			) continue;
			if(Flow.CanMakeImplicitReturnExplicit(Id, *End)) {
				const CanonicalForm *const Form = GeneratedFormOf(Syntax, FormOperation::ArglessReturn);
				const FormPart *const Result = Form ? Form->PartOf(FormPartRole::Result) : nullptr;
				if(!Result || !Result->Definition) {
					throw FormatLimitExceeded("missing generated value return form", FailureReason::FormatterDefect);
				}
				// 返戻の語と，文法の返戻の値の欄に置く単一式
				RewriteStep Wrap { RewriteKind::Wrap, End->Last };
				Wrap.Inserted = {
					{},
					true,
					{ { ReturnWord }, { {}, false, {}, uint64_t{ 1 } << static_cast<uint8_t>(ChildRole::Result), End->Last } },
					0,
					NoNode,
					{},
					Result->Definition
				};
				Wrap.Boundary = BoundaryKind::Spaced;
				Plan.Steps.push_back(std::move(Wrap));
				continue;
			}
			// 文の並びの閉じの前（閉じを持たない並びは最後）への返戻文の追加
			const uint32_t Count = Syntax.ChildCount(End->Statements);
			const NodeId Closer = Syntax.Child(End->Statements, Count - 1);
			RewriteStep Return { RewriteKind::Insert, End->Statements };
			Return.Index = Count && !Syntax.IsNamed(Closer) && Syntax.IsTokenRole(Closer, Role::ClosingBraceToken) ? Count - 1 : Count;
			Return.Boundary = BoundaryKind::Spaced;
			Return.Inserted = End->Kind == ReturnKind::Void ? ArglessReturn(Syntax) : GeneratedNode{};
			if(End->Kind == ReturnKind::Main) {
				const CanonicalForm *const Form = GeneratedFormOf(Syntax, FormOperation::ReturnWithZero);
				if(!Form) throw FormatLimitExceeded("missing generated return with zero form", FailureReason::FormatterDefect);
				Return.Inserted = GeneratedPart(Syntax, *Form, FormPartRole::Result, NoNode, true);
			}
			// Rust の塊の値を与える末尾式（破棄の順を変え得る一時値を作る物は補完の前提を確かめれない物として上で除いた）
			const NodeId Value = Syntax.Rules().TailExpressionIsValue ? Flow.TailExpression(End->Last) : NoNode;
			// 式の文で包まない末尾式か
			const bool IsBare = Value == End->Last;
			if(Value != NoNode && End->Kind == ReturnKind::Value) {
				// 値の末尾式の返戻の文への変換（塊で終わる式の文は `;` を加える）
				RewriteStep Wrap { RewriteKind::Wrap, Value };
				GeneratedNode Returned {
					{},
					true,
					{ { ReturnWord }, { {}, false, {}, {}, Value } },
					0,
					NoNode,
					{},
					Syntax.Rules().ReturnExpressionDefinition
				};
				if(IsBare) {
					Wrap.Inserted =
					{ {}, true, { std::move(Returned), { TerminatorLexeme } }, 0, NoNode, {}, Syntax.Rules().ExpressionStatementDefinition };
				} else Wrap.Inserted = std::move(Returned);
				Wrap.Boundary = BoundaryKind::Spaced;
				Plan.Steps.push_back(std::move(Wrap));
				if(!IsBare) {
					RewriteStep Terminator { RewriteKind::Insert, End->Last };
					Terminator.Index = Syntax.ChildCount(End->Last);
					Terminator.Inserted = { TerminatorLexeme };
					Plan.Steps.push_back(std::move(Terminator));
				}
			} else if(Value != NoNode && IsBare) {
				// 値を返さない関数の末尾式の文への変換
				RewriteStep Wrap { RewriteKind::Wrap, Value };
				Wrap.Inserted = {
					{},
					true,
					{ { {}, false, {}, {}, Value }, { TerminatorLexeme } },
					0,
					NoNode,
					{},
					Syntax.Rules().ExpressionStatementDefinition
				};
				Wrap.Boundary = BoundaryKind::Spaced;
				Plan.Steps.push_back(std::move(Wrap));
			}
			if(End->Kind == ReturnKind::Void || End->Kind == ReturnKind::Main) Plan.Steps.push_back(std::move(Return));
		}
		// 終了
		return;
	}

	/**
	 * 生成形式の部品から節点を作る関数
	 * @param Form 文法が与えた生成形式
	 * @param Part 生成する部品
	 * @return 種別と固定字句を与えた節点
	 * @throws FormatLimitExceeded 部品が無い場合
	 */
	static GeneratedNode FormNode(const CanonicalForm &Form, const FormPartRole Part) {
		GeneratedNode Result;
		const FormPart *const Selected = Form.PartOf(Part);
		if(!Selected) throw FormatLimitExceeded("a generated node has no grammar form part", FailureReason::FormatterDefect);
		Result.Definition = Selected->Definition;
		Result.Lexeme = Selected->FixedLexeme;
		// 部品の節点の返戻
		return Result;
	}

	/**
	 * 末尾の値を返す関数の返戻を明示する関数
	 * 役割索引又は作業列から関数を選び，生成形式を持つ本体と直下の救済節だけを調べる
	 * @param Syntax 構文モデル
	 * @param Plan 返戻で包む計画
	 */
	void SyntaxNormalizer::CollectTailReturns(const SyntaxModel &Syntax, RewritePlan &Plan) const {
		// 生成形式を持つ関数の本体と直下の救済節だけを処理
		for(const NodeId Id : Candidates(Syntax, { Role::TailValueFunction }, {})) {
			const NodeId Body = Syntax.FieldChild(Id, ChildRole::Body);
			if(!Syntax.Is(Body, Role::StatementSequence)) continue;
			for(const CanonicalForm &Form : Syntax.GetNode(Id).Facts.Forms.View()) {
				if(Form.Operation != FormOperation::ExplicitTailReturn) continue;
				std::vector<NodeId> Bodies { Body };
				for(const NodeId Child : Syntax.ChildrenOf(Body)) {
					Syntax.Charge(1);
					if(Syntax.Is(Child, Role::ExceptionSection)) Bodies.push_back(Syntax.FieldChild(Child, ChildRole::Body));
				}
				for(const NodeId Sequence : Bodies) {
					// 返戻を補う末尾式の選択
					NodeId Last = NoNode;
					bool HasReturn = false;
					ForEachStatementOf(
						Syntax,
						Sequence,
						[&](const NodeId Child) -> void {
							// 救済節と最終節を末尾の文としない事の返戻
							if(Syntax.Is(Child, Role::ExceptionSection) || Syntax.Is(Child, Role::FinalSection)) return;
							HasReturn = HasReturn || Syntax.Is(Child, Role::ReturnKeyword);
							Last = Child;
						}
					);
					if(HasReturn || !Syntax.Is(Last, Role::ReturnValue)) continue;
					// 返戻に変えると読みが変わる末尾式の除外
					if(
						const NodeId Method = Syntax.Is(Last, Role::ReceiverCall) ? Syntax.FieldChild(Last, ChildRole::Function) : Last;
						Syntax.IsWord(Method, Word::SideEffectMethod)
					) continue;
					if(
						Syntax.Is(Last, Role::Binary) && AnyChildOf(
							Syntax,
							Last,
							[&](const NodeId Child) -> bool {
								Syntax.Charge(1);
								// 語の低位演算子かの返戻
								return Syntax.IsWord(Child, Word::LogicalWord);
							}
						)
					) continue;
					if(Syntax.Is(Last, Role::Range) && !Syntax.IsNamed(Syntax.Child(Last, 0))) continue;
					NodeId Head = Last;
					while(Syntax.IsNamed(Syntax.Child(Head, 0))) {
						Syntax.Charge(1);
						Head = Syntax.Child(Head, 0);
					}
					if(Syntax.IsWord(Syntax.Child(Head, 0), Word::Negation) || Syntax.Is(Head, Role::KeywordBlock)) continue;
					const std::string HeadText = Syntax.Text(Head);
					if(
						HeadText.size() > 1 && Syntax.Rules().IsSpelling(std::string_view(HeadText).substr(0, 1), Word::RegexDelimiter) &&
						std::isspace(static_cast<unsigned char>(HeadText[1]))
					) continue;
					// 必要な括弧を伴う返戻文の生成
					GeneratedNode Arguments = FormNode(Form, FormPartRole::Arguments);
					const std::string_view Plus = Syntax.Rules().Operators.Plus;
					bool ShouldWrap = HeadText.starts_with(Plus) ||
					Syntax.FirstTokenRole(Head, Role::OpeningParenToken) && std::string_view(HeadText).substr(1).starts_with(Plus);
					if(HeadText.starts_with(Syntax.Rules().Operators.Minus) && Syntax.Is(Head, Role::UnaryValue)) {
						NodeId Operand = Syntax.NamedChild(Head, 0);
						const bool IsPostfix = Syntax.Is(Operand, Role::ReceiverCall) || Syntax.Is(Operand, Role::ElementReference);
						while(Syntax.Is(Operand, Role::ReceiverCall) || Syntax.Is(Operand, Role::ElementReference)) {
							Syntax.Charge(1);
							Operand = Syntax.NamedChild(Operand, 0);
						}
						ShouldWrap = IsPostfix && Syntax.Is(Operand, Role::NumberLiteral);
					}
					if(ShouldWrap) Arguments.Children.push_back(FormNode(Form, FormPartRole::Open));
					Arguments.Children.push_back({ {}, false, {}, 0, Last });
					if(ShouldWrap) Arguments.Children.push_back(FormNode(Form, FormPartRole::Close));
					GeneratedNode Returned = FormNode(Form, FormPartRole::Result);
					Returned.Children = { FormNode(Form, FormPartRole::Keyword), std::move(Arguments) };
					RewriteStep Step { RewriteKind::Wrap, Last };
					Step.Inserted = std::move(Returned);
					Plan.Steps.push_back(std::move(Step));
				}
			}
		}
		// 終了
		return;
	}

	/**
	 * 単一文本体の後置形式の収集関数
	 * 候補を持つ変更の有効範囲だけを文書順に辿り，候補より前に終わる代入・仮引数が束縛した名前を積んで引く
	 * 計算量：索引の候補数 R，対象の有効範囲の節点数 V，候補の条件・本体の大きさの和 C，調べる祖先の数 A，範囲の根の数 S，書換の数 K に対し O(R + V + C + A + S log S + K log K)
	 * @param Syntax 構文モデル
	 * @param Changed 此の規則群へ届いた変更節点
	 * @param Plan 構造を書き換える計画
	 */
	void SyntaxNormalizer::CollectStatementForms(
		const SyntaxModel &Syntax,
		const std::span<const NodeId> Changed,
		RewritePlan &Plan
	) const {
		// 後置形式を持ち得る構文が無い文法の終了
		if(Syntax.NodesWith(Role::KeywordBlock).empty()) return;
		// 束縛した名前毎の束縛の順（其の時点迄に終えた節点の物）と，束縛の順の並び
		std::unordered_map<std::string, std::vector<size_t>> BoundAt;
		std::vector<std::string> BoundOrder;
		// 束縛を順の並びへ積む処理
		const auto Bind = [&BoundAt, &BoundOrder](std::string Name) -> void {
			BoundAt[Name].push_back(BoundOrder.size());
			BoundOrder.push_back(std::move(Name));
		};
		// 積んだ束縛を指定の数迄戻す処理
		const auto Unbind = [&BoundAt, &BoundOrder](const size_t Size) -> void {
			while(BoundOrder.size() > Size) {
				BoundAt[BoundOrder.back()].pop_back();
				BoundOrder.pop_back();
			}
		};
		// 候補毎の書換（元の節点番号の順に計画へ積む）
		std::vector<std::pair<NodeId, RewriteStep>> Found;
		// 候補の判定（束縛の有効範囲の始まりの順を受け取る）
		const auto Examine = [&](const NodeId Id, const size_t ScopeStart) -> void {
			for(const CanonicalForm &Form : Syntax.GetNode(Id).Facts.Forms.View()) {
				// 後置へ移せる単一の文の本体の判定
				if(Form.Operation != FormOperation::PostfixCondition) continue;
				const NodeId Parent = Syntax.ParentOf(Id), Condition = Syntax.FieldChild(Id, ChildRole::Condition);
				NodeId Body = Syntax.FieldChild(Id, ChildRole::Body);
				if(Body == NoNode) Body = Syntax.FieldChild(Id, ChildRole::Consequence);
				if(
					Syntax.FieldChild(Id, ChildRole::Alternative) != NoNode || !(
						Syntax.Is(Parent, Role::StatementSequence) || Syntax.ParentOf(Parent) == NoNode || Syntax.Is(Parent, Role::ExplicitKeywordBody)
					)
				) continue;
				NodeId Statement = NoNode;
				uint32_t StatementCount = 0;
				ForEachStatementOf(
					Syntax,
					Body,
					[&](const NodeId Child) -> bool {
						Statement = Child;
						return ++StatementCount < 2;
					}
				);
				if(StatementCount != 1) continue;
				// 条件の読取と構文の制約の検査
				bool IsSafe = !Syntax.Is(Statement, Role::KeywordBlock) && !Syntax.LastTokenRole(Statement, Role::OpenRangeToken) &&
				!Syntax.LastTokenRole(Statement, Role::ColonToken);
				std::unordered_set<std::string> Reads, Writes;
				std::vector<NodeId> Stack { Condition };
				while(!Stack.empty()) {
					const NodeId Child = Stack.back();
					Stack.pop_back();
					Syntax.Charge(1);
					IsSafe = IsSafe && !Syntax.Is(Child, Role::AssignmentExpression) && !Syntax.Is(Child, Role::CompoundAssignment) &&
					!Syntax.Is(Child, Role::Regex) && !Syntax.Is(Child, Role::HeredocBeginning);
					if(Syntax.Is(Child, Role::Identifier)) Reads.insert(Syntax.Text(Child));
					for(const NodeId Next : Syntax.ChildrenOf(Child)) Stack.push_back(Next);
				}
				// 本体が束縛する名前（代入の左辺・多重代入の名前，照合の模式の名前，正規表現の名前付の捕捉）
				const auto AddBoundNames = [&](const NodeId Binding) -> void {
					if(Syntax.Is(Binding, Role::Regex)) {
						// 名前付の捕捉の名前（名前は語の字で始まり非 ASCII の字も含む，先読み・後読みの `(?<=` / `(?<!` は名前を持たない）
						const std::string Pattern = Syntax.Text(Binding);
						for(const SyntaxRules::RegexCapture &Capture : Syntax.Rules().RegexNamedCaptures) for(
							size_t At = Pattern.find(Capture.Open);
							At != std::string::npos;
							At = Pattern.find(Capture.Open, At + Capture.Open.size())
						) {
							const size_t Start = At + Capture.Open.size();
							if(Start == Pattern.size() || !IsWordChar(Pattern[Start])) continue;
							const size_t End = Pattern.find(Capture.Close, Start);
							// 名前の閉じの無い捕捉は束縛を判別出来ない為に書き換えない
							if(End == std::string::npos) IsSafe = false;
							else Writes.insert(Pattern.substr(Start, End - Start));
						}
						// 終了
						return;
					}
					// 代入は左辺，照合は模式の全体の名前（受け手・添字の中の名前は束縛しない）
					const NodeId Left = Syntax.FieldChild(Binding, ChildRole::Left);
					std::vector<NodeId> Names { Left == NoNode ? Binding : Left };
					while(!Names.empty()) {
						const NodeId Part = Names.back();
						Names.pop_back();
						Syntax.Charge(1);
						if(Syntax.Is(Part, Role::Identifier)) Writes.insert(Syntax.Text(Part));
						if(Syntax.Is(Part, Role::ElementReference) || Syntax.Is(Part, Role::ReceiverCall)) continue;
						for(const NodeId Next : Syntax.ChildrenOf(Part)) Names.push_back(Next);
					}
					// 終了
					return;
				};
				Stack = { Statement };
				while(!Stack.empty()) {
					const NodeId Child = Stack.back();
					Stack.pop_back();
					Syntax.Charge(1);
					if(Syntax.Is(Child, Role::BindingExpression)) AddBoundNames(Child);
					for(const NodeId Next : Syntax.ChildrenOf(Child)) Stack.push_back(Next);
				}
				// 本体が書き条件が読む名前は，候補より前に同じ有効範囲で束縛して居なければ書換で読みが変わる
				for(const std::string &Name : Writes) {
					const std::unordered_map<std::string, std::vector<size_t>>::const_iterator Hit = BoundAt.find(Name);
					if(
						const bool IsBound = Hit != BoundAt.end() && !Hit->second.empty() && Hit->second.back() >= ScopeStart;
						Reads.contains(Name) && !IsBound
					) IsSafe = false;
				}
				if(!IsSafe) continue;
				// 遅延した本体の移動と後置形式への組直
				uint32_t DeferredIndex = Syntax.GetNode(Id).Index + 1;
				for(const NodeId Child : Syntax.ChildrenOf(Body)) if(Syntax.Is(Child, Role::DeferredLiteralBody)) {
					RewriteStep Move { RewriteKind::Move, Child };
					Move.Destination = Parent;
					Move.Index = DeferredIndex++;
					Move.DestinationFields = 0;
					Found.emplace_back(Id, std::move(Move));
				}
				GeneratedNode Result = FormNode(Form, FormPartRole::Result);
				Result.Fields = Syntax.GetNode(Id).Fields;
				Result.Children = {
					{ {}, false, {}, uint64_t{ 1 } << static_cast<uint8_t>(ChildRole::Body), Statement },
					{ {}, false, {}, 0, Syntax.Child(Id, 0) },
					{ {}, false, {}, uint64_t{ 1 } << static_cast<uint8_t>(ChildRole::Condition), Condition }
				};
				RewriteStep Step { RewriteKind::Reshape, Id };
				Step.Inserted = std::move(Result);
				Found.emplace_back(Id, std::move(Step));
			}
		};
		// 文書順の走査の枠（入る時と出る時，入った時の束縛の数と有効範囲の始まり）
		struct Frame {
			NodeId Node;
			bool IsExit;
			size_t Size;
			size_t ScopeStart;
			NodeId FunctionScope;
		};
		std::unordered_set<NodeId> CandidateIds, ChangedScopes;
		std::unordered_map<NodeId, NodeId> ScopeOf;
		std::vector<NodeId> ScopeRoots;
		// 同じ有効範囲の祖先を再び辿らずに，最も内側の関数を求める
		const auto ScopeFor = [&Syntax, &ScopeOf](const NodeId Start) -> NodeId {
			std::vector<NodeId> Path;
			NodeId Up = Start, Scope = Syntax.GetRoot();
			while(Up != NoNode && !ScopeOf.contains(Up)) {
				Syntax.Charge(1);
				Path.push_back(Up);
				if(Syntax.Is(Up, Role::TailValueFunction)) {
					Scope = Up;
					break;
				}
				Up = Syntax.ParentOf(Up);
			}
			if(Up != NoNode && ScopeOf.contains(Up)) {
				Syntax.Charge(1);
				Scope = ScopeOf.at(Up);
			}
			for(const NodeId Node : Path) ScopeOf.emplace(Node, Scope);
			// 最も内側の関数又は根の返戻
			return Scope;
		};
		std::vector<NodeId> FormCandidates;
		for(const NodeId Id : Syntax.NodesWith(Role::KeywordBlock)) {
			Syntax.Charge(1);
			if(
				std::ranges::any_of(
					Syntax.GetNode(Id).Facts.Forms.View(),
					[](const CanonicalForm &Form) -> bool {
						// 後置形式の有無の返戻
						return Form.Operation == FormOperation::PostfixCondition;
					}
				)
			) FormCandidates.push_back(Id);
		}
		// 生成形式の候補が無い場合の終了
		if(FormCandidates.empty()) return;
		// 変更を含む最内の関数の収集
		for(const NodeId Id : Changed) {
			if(Id == NoNode || Syntax.IsRemovedNode(Id)) continue;
			ChangedScopes.insert(ScopeFor(Id));
		}
		for(const NodeId Id : InDocumentOrder(Syntax, FormCandidates, {})) {
			const NodeId Scope = ScopeFor(Syntax.ParentOf(Id));
			if(!Changed.empty() && !ChangedScopes.contains(Scope)) continue;
			CandidateIds.insert(Id);
			ScopeRoots.push_back(Scope);
		}
		std::sort(ScopeRoots.begin(), ScopeRoots.end());
		ScopeRoots.erase(std::unique(ScopeRoots.begin(), ScopeRoots.end()), ScopeRoots.end());
		// 他の対象範囲に含まれる範囲の除外
		const std::unordered_set<NodeId> AllScopes(ScopeRoots.begin(), ScopeRoots.end());
		std::unordered_map<NodeId, bool> Enclosed;
		std::erase_if(
			ScopeRoots,
			[&Syntax, &AllScopes, &Enclosed](const NodeId Root) -> bool {
				Syntax.Charge(1);
				std::vector<NodeId> Path;
				NodeId Up = Syntax.ParentOf(Root);
				while(Up != NoNode && !AllScopes.contains(Up) && !Enclosed.contains(Up)) {
					Syntax.Charge(1);
					Path.push_back(Up);
					Up = Syntax.ParentOf(Up);
				}
				if(Up != NoNode) Syntax.Charge(1);
				const bool HasOuter = Up != NoNode && (AllScopes.contains(Up) || Enclosed.at(Up));
				for(const NodeId Node : Path) Enclosed.emplace(Node, HasOuter);
				// 他の有効範囲の中に在るかの返戻
				return HasOuter;
			}
		);
		for(const NodeId Root : ScopeRoots) {
			// 文書順の走査に伴う束縛の積上と巻戻
			BoundAt.clear();
			BoundOrder.clear();
			std::vector<Frame> Frames { { Root, false, 0, 0, NoNode } };
			while(!Frames.empty()) {
				const Frame Top = Frames.back();
				Frames.pop_back();
				Syntax.Charge(1);
				if(Top.IsExit) {
					// 語で閉じる塊の中の束縛は，後の兄弟から見えない
					if(Syntax.Is(Top.Node, Role::KeywordBlock)) Unbind(Top.Size);
					else if(Syntax.Is(Top.Node, Role::AssignmentExpression) || Syntax.Is(Top.Node, Role::CompoundAssignment)) {
						Bind(Syntax.Text(Syntax.FieldChild(Top.Node, ChildRole::Left)));
					} else if(Syntax.Is(Top.Node, Role::MethodParameters)) {
						for(const NodeId Parameter : Syntax.ChildrenOf(Top.Node)) Bind(Syntax.Text(Parameter));
					}
					continue;
				}
				// 関数の本体の中の候補は，其の関数の中で束縛した名前だけを見る
				const size_t ScopeStart = Syntax.Is(Syntax.ParentOf(Top.Node), Role::TailValueFunction) ? Top.Size : Top.ScopeStart;
				const NodeId FunctionScope = Syntax.Is(Top.Node, Role::TailValueFunction) ? Top.Node : Top.FunctionScope;
				if(
					CandidateIds.contains(Top.Node) ||
					ChangedScopes.contains(FunctionScope == NoNode ? Syntax.GetRoot() : FunctionScope) && Syntax.Is(Top.Node, Role::KeywordBlock)
				) Examine(Top.Node, ScopeStart);
				Frames.push_back({ Top.Node, true, BoundOrder.size(), ScopeStart, FunctionScope });
				const std::span<const NodeId> Children = Syntax.ChildrenOf(Top.Node);
				for(size_t Index = Children.size(); Index--;) {
					Frames.push_back({ Children[Index], false, BoundOrder.size(), ScopeStart, FunctionScope });
				}
			}
		}
		std::stable_sort(
			Found.begin(),
			Found.end(),
			[](const std::pair<NodeId, RewriteStep> &Lhs, const std::pair<NodeId, RewriteStep> &Rhs) -> bool {
				// 節点番号の比較の返戻
				return Lhs.first < Rhs.first;
			}
		);
		for(std::pair<NodeId, RewriteStep> &Entry : Found) Plan.Steps.push_back(std::move(Entry.second));
		// 終了
		return;
	}

	/**
	 * 構文の正規化関数（整形仕様 3.4，第５章）
	 * 投影済の節点表だけを書き換え，再解析しない
	 * 規則順に変更箇所を再登録し，全ての依存先を処理する迄続ける
	 * @param Syntax 構文モデル
	 * @return 錨の移譲の記録
	 * @throws FormatLimitExceeded 構文の循環又は必要な生成形式の欠落を検出した場合（整形器の不具合）
	 */
	RewriteTrace SyntaxNormalizer::Normalize(SyntaxModel &Syntax) {
		WorkNodes.clear();
		// 規則の対象を役割から選ぶ索引（正規化だけが使い，以後は書換だけが更新する）
		Syntax.BuildRoleIndex();
		enum class Stage : size_t {
			Returns,
			Increments,
			Forms,
			Terminators,
			Split,
			Commas,
			Endings,
			Local,
			Types,
			Tuples,
			Braces,
			Merge,
			Count
		}; // 規則を適用する段（宣言の順は適用順）
		constexpr size_t StageCount = static_cast<size_t>(Stage::Count);
		std::array<bool, StageCount> Pending, Started {};
		Pending.fill(true);
		std::array<std::vector<NodeId>, StageCount> Dirty {};
		RewriteTrace Trace;
		std::shared_ptr<const StateSnapshot> State;
		struct VisitedState {
			std::shared_ptr<const StateSnapshot> Syntax; // 共有する構文の控え
			std::array<std::vector<NodeId>, StageCount> Work; // 未処理の依存先
			std::vector<NodeId> CurrentWork; // 処理中の規則群が読む節点
			std::array<bool, StageCount> Started; // 初回の役割索引の処理を終えた規則群
			uint64_t Pending = 0; // 未処理の規則群
			size_t Position = 0; // 規則順の現在位置
		};
		using VisitedMap = std::unordered_multimap<size_t, VisitedState>;
		VisitedMap Visited;
		const auto PendingMask = [&Pending]() -> uint64_t {
			uint64_t Mask = 0;
			for(size_t Index = 0; Index < StageCount; ++Index) if(Pending[Index]) Mask |= uint64_t{ 1 } << Index;
			// 未処理の規則群の返戻
			return Mask;
		};
		const auto ApplyPlan = [&](const RewritePlan &Plan, const size_t Current) -> bool {
			// 書換の無い計画の返戻
			if(Plan.Steps.empty()) return false;
			if(!State) {
				// 最初の状態は全節点を複写せず，書換が最初に変える節点の変更前の値だけを控える
				Syntax.BeginJournal();
				State = StateOf(Syntax);
				Visited.emplace(State->Hash, VisitedState{ State, Dirty, WorkNodes, Started, PendingMask(), Current });
			}
			const RewriteTrace Part = Syntax.Apply(Plan);
			Trace.Transfers.insert(Trace.Transfers.end(), Part.Transfers.begin(), Part.Transfers.end());
			// 書換後の構文状態の確定
			const std::span<const NodeId> Changed = Syntax.TouchedNodes();
			// 節点を変えなかった計画の返戻
			if(Changed.empty()) return false;
			const std::shared_ptr<const StateSnapshot> Next = StateOf(Syntax, State);
			if(Next->Hash == State->Hash && SameState(Syntax, *State, *Next, true)) {
				State = Next;
				// 構文を変えなかった計画の返戻
				return false;
			}
			// 節点・祖先・兄弟を読む先行規則と同じ規則群を変更箇所だけ再評価する
			for(size_t Rule = 0; Rule <= Current; ++Rule) {
				// 事前分割を宣言の最終統合へ戻さず，互いの逆操作にしない
				if(Rule == static_cast<size_t>(Stage::Split) && Current >= static_cast<size_t>(Stage::Local)) continue;
				Pending[Rule] = true;
				Dirty[Rule].insert(Dirty[Rule].end(), Changed.begin(), Changed.end());
				std::sort(Dirty[Rule].begin(), Dirty[Rule].end());
				Dirty[Rule].erase(std::unique(Dirty[Rule].begin(), Dirty[Rule].end()), Dirty[Rule].end());
			}
			State = Next;
			// 同じ進行状態を伴う構文の循環の検出
			const uint64_t Progress = PendingMask();
			const std::pair<VisitedMap::iterator, VisitedMap::iterator> Earlier = Visited.equal_range(State->Hash);
			for(VisitedMap::iterator Found = Earlier.first; Found != Earlier.second; ++Found) if(
				Found->second.Pending == Progress && Found->second.Position == Current && Found->second.Work == Dirty &&
				Found->second.Started == Started && Found->second.CurrentWork == WorkNodes && SameState(Syntax, *Found->second.Syntax, *State)
			) {
				throw FormatLimitExceeded(
					Current == static_cast<size_t>(Stage::Tuples) ?
					"tuple edits return to an earlier state" :
					"normalization edits return to an earlier state",
					FailureReason::FormatterDefect
				);
			}
			Visited.emplace(State->Hash, VisitedState{ State, Dirty, WorkNodes, Started, Progress, Current });
			// 構文が変更された事の返戻
			return true;
		};
		// 未処理の規則群の選択と変更箇所の引継
		while(true) {
			size_t Current = 0;
			while(Current < StageCount && !Pending[Current]) ++Current;
			if(Current == StageCount) break;
			Pending[Current] = false;
			const bool IsFirst = !Started[Current];
			Started[Current] = true;
			const Stage Rule = static_cast<Stage>(Current);
			std::vector<NodeId> Changed;
			Changed.swap(Dirty[Current]);
			if(!IsFirst && !Changed.empty()) SetWorkNodes(Syntax, Changed, Rule == Stage::Returns);
			RewritePlan Plan;
			// 規則順の書換計画の収集
			switch(Rule) {
			case Stage::Returns:
				Plan.RuleStarts.push_back(Plan.Steps.size());
				CollectExplicitReturnSteps(Syntax, Plan);
				Plan.RuleStarts.push_back(Plan.Steps.size());
				CollectTailReturns(Syntax, Plan);
				break;
			case Stage::Increments:
				CollectIncrementSteps(Syntax, Plan);
				break;
			case Stage::Forms:
				CollectStatementForms(Syntax, Changed, Plan);
				break;
			case Stage::Terminators:
				CollectTerminatorSteps(Syntax, Plan);
				break;
			case Stage::Split:
				if(Syntax.Rules().SplitBindingsBeforeConst) CollectCommaDeclSteps(Syntax, Plan, true);
				break;
			case Stage::Commas:
				CollectTrailingCommaSteps(Syntax, Plan);
				break;
			case Stage::Endings:
				for(const NodeId Id : Candidates(Syntax, { Role::OptionalLoopTerminator }, { Role::Error })) {
					const bool IsEmptyLoop =
					(Syntax.Is(Id, Role::CountedLoop) || Syntax.Is(Id, Role::ForeachStatement)) && Syntax.FieldChild(Id, ChildRole::Body) == NoNode;
					bool HasAltEnd = false;
					for(const NodeId Child : Syntax.ChildrenOf(Id)) HasAltEnd = HasAltEnd || Syntax.Is(Child, Role::AlternativeBodyCloser);
					if((IsEmptyLoop || HasAltEnd) && !Syntax.IsWord(Syntax.Child(Id, Syntax.ChildCount(Id) - 1), Word::StatementEnd)) {
						RewriteStep End { RewriteKind::Insert, Id };
						End.Destination = Id;
						End.Index = Syntax.ChildCount(Id);
						End.Inserted = { GeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::Terminator) };
						Plan.Steps.push_back(std::move(End));
					}
				}
				break;
			case Stage::Local:
				Plan.RuleStarts.push_back(Plan.Steps.size());
				CollectNodeSteps(Syntax, Plan);
				Plan.RuleStarts.push_back(Plan.Steps.size());
				CollectParenSteps(Syntax, Plan);
				Plan.RuleStarts.push_back(Plan.Steps.size());
				CollectFusedPrefixSteps(Syntax, Plan);
				Plan.RuleStarts.push_back(Plan.Steps.size());
				CollectImmutableBindingSteps(Syntax, Plan);
				Plan.RuleStarts.push_back(Plan.Steps.size());
				CollectSeparatorSteps(Syntax, Plan);
				break;
			case Stage::Types:
				if(Syntax.Rules().NormalizeTypeAssociation) CollectTypeAssociationSteps(Syntax, Plan);
				break;
			case Stage::Tuples:
				if(Syntax.Rules().NormalizeTupleWrapping) while(true) {
					RewritePlan Tuples;
					Tuples.RuleStarts.push_back(Tuples.Steps.size());
					CollectTupleSteps(Syntax, Tuples);
					Tuples.RuleStarts.push_back(Tuples.Steps.size());
					CollectTrailingCommaSteps(Syntax, Tuples);
					if(Tuples.Steps.empty() || !ApplyPlan(Tuples, Current)) break;
					// 次の巡は此の書換に依存する対象だけを調べる
					SetWorkNodes(Syntax, Syntax.TouchedNodes(), false);
				}
				break;
			case Stage::Braces:
				while(true) {
					RewritePlan Braces;
					CollectBraceSteps(Syntax, Braces);
					if(Braces.Steps.empty() || !ApplyPlan(Braces, Current)) break;
					// 次の巡は此の書換に依存する対象だけを調べる
					SetWorkNodes(Syntax, Syntax.TouchedNodes(), false);
				}
				break;
			case Stage::Merge:
				CollectCommaDeclSteps(Syntax, Plan);
				break;
			case Stage::Count:
				break;
			}
			ApplyPlan(Plan, Current);
			WorkNodes.clear();
		}
		Syntax.EndJournal();
		Syntax.ReleaseRoleIndex();
		// 正規化の後の文書の同期とコメントの配置が引く，構文の順で後に続く物の索引
		Syntax.BuildFollowingIndex();
		// 移譲の連鎖を最終の行先へ合成する（コメント側は一度だけ引く，整形仕様 7.2）
		Trace.Compose();
		// 錨の移譲の記録の返戻
		return Trace;
	}
} // namespace Core
