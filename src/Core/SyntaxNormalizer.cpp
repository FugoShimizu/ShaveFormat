#include "SyntaxNormalizer.hpp"
#include "ControlFlow.hpp"
#include "SyntaxGeneration.hpp"
#include "../Util/CharClass.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include "../Util/WorkSort.hpp"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace Core {
	/**
	 * 親の結果を継承する正規化障壁の取得関数
	 * 通常側は親の結果を共有し，基準側は各照会で祖先を全て調べる
	 * 計算量：通常側は未収集又は失効した祖先数 A に対し期待値 O(A + 1)，基準側は祖先深度 H に対し O(H + 1)
	 * @param Syntax 現在の収集が読む構文
	 * @param Id 自身を含む障壁を調べるノード
	 * @return 対象自身を含む祖先のエラー・保護領域の有無
	 * @throws FormatLimitExceeded 作業予算超過の場合
	 * @throws std::bad_alloc 障壁又は祖先列のメモリを確保出来ない場合
	 * @throws std::length_error 祖先列又は障壁の要素数上限を超える場合
	 * @throws std::out_of_range 対象又は継承先がノード表に無い場合
	 */
	const SyntaxNormalizer::NormalizationBarrier &SyntaxNormalizer::GetNormalizationBarrierOf(
		const SyntaxModel &Syntax,
		const NodeId Id
	) const {
		// 親の結果を継承する正規化障壁の取得
		#if defined(ShaveFormatRefFacts)
		NormalizationBarrier Barrier;
		bool HasInterpolationBoundary = false;
		for(NodeId Up = Id; Up != NoNode; Up = Syntax.GetParentOf(Up)) {
			Syntax.Charge(1);
			Barrier.HasError |= Syntax.Is(Up, Role::Error);
			Barrier.HasTextBarrier |=
			Syntax.GetNode(Up).Facts.TokenSpacing != TokenSpacingMode::None || Syntax.GetNode(Up).Facts.DoesObserveText;
			Barrier.DoesRequireInlineCode |= Syntax.GetNode(Up).Facts.DoesRequireInlineCode;
			Barrier.IsQuoted |= !HasInterpolationBoundary && Syntax.Is(Up, Role::VerbatimQuotedStr);
			HasInterpolationBoundary |= Syntax.Is(Up, Role::StrInterpolation);
		}
		Barrier.IsProtected = Barrier.HasError || Barrier.HasTextBarrier || Barrier.IsQuoted;
		// 取得済の参照を保つ基準側の障壁（テスト専用の構築だけが持つ）
		const std::shared_ptr<const NormalizationBarrier> Result = std::make_shared<const NormalizationBarrier>(Barrier);
		Syntax.EditNormalizationFacts().RefFacts.push_back(Result);
		// 取得済の参照を保つ照会結果の返戻
		return *Result;
		#else
		NormalizationFacts &Facts = Syntax.EditNormalizationFacts();
		std::vector<NodeId> &Missing = Facts.BarrierPath;
		Missing.clear();
		NormalizationBarrier Added;
		const NormalizationBarrier *Result = nullptr;
		for(NodeId Up = Id; Up != NoNode; Up = Syntax.GetParentOf(Up)) {
			if(
				const std::unordered_map<NodeId, NormalizationBarrier>::const_iterator Found = Facts.NormalizationBarriers.find(Up);
				Found != Facts.NormalizationBarriers.end()
			) {
				Result = &Found->second;
				Added = *Result;
				break;
			}
			Syntax.Charge(1);
			Missing.push_back(Up);
		}
		for(std::vector<NodeId>::const_reverse_iterator Item = Missing.rbegin(); Item != Missing.rend(); ++Item) {
			Added.HasError |= Syntax.Is(*Item, Role::Error);
			Added.HasTextBarrier |=
			Syntax.GetNode(*Item).Facts.TokenSpacing != TokenSpacingMode::None || Syntax.GetNode(*Item).Facts.DoesObserveText;
			Added.DoesRequireInlineCode |= Syntax.GetNode(*Item).Facts.DoesRequireInlineCode;
			Added.IsQuoted = Syntax.Is(*Item, Role::VerbatimQuotedStr) || !Syntax.Is(*Item, Role::StrInterpolation) && Added.IsQuoted;
			Added.IsProtected = Added.HasError || Added.HasTextBarrier || Added.IsQuoted;
			Result = &Facts.NormalizationBarriers.emplace(*Item, Added).first->second;
		}
		// 親から継承した障壁の返戻
		return Result ? *Result : Facts.NormalizationBarriers.at(Id);
		#endif
	}

	/**
	 * 数値リテラルの型接尾辞の開始位置の取得関数
	 * 計算量：登録した型接尾辞の総長 S に対し O(S)
	 * @param Rules 文法の構文・意味の固定の定義
	 * @param Text 数値リテラルのテキスト
	 * @return 型接尾辞（Rust の `u8` / `i32` / `f64` 等）の開始位置（接尾辞が無ければテキストの長さ）
	 */
	size_t SyntaxNormalizer::GetNumericSuffixStart(const SyntaxRules &Rules, const std::string_view Text) {
		// 数値リテラルの型接尾辞の開始位置の取得
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
	 * ノードの構文順の整列関数（整形仕様 5.1）
	 * 候補の祖先だけを根から辿り，木から外れたノードを除いて現在の子の位置で並べる
	 * 祖先の接続と走査の作業列を共有し，順序番号の採番と編集の追跡を起動しない
	 * 計算量：候補数 K，今回と前回の祖先数 P と追加ノード数 A に対し O(K + P log(P + 1) + A)，兄弟の順が揃う場合は O(K + P + A)
	 * @param Syntax 構文モデル
	 * @param Found 候補（重複を含み得る）
	 * @return 構文順の重複の無い候補
	 * @throws FormatLimitExceeded 作業予算を使い切った場合
	 * @throws std::bad_alloc 候補又は経路のメモリを確保出来ない場合
	 * @throws std::length_error 候補又は位置の要素数上限を超える場合
	 * @throws std::out_of_range 候補又は祖先がノード表に無い場合
	 */
	std::vector<NodeId> SyntaxNormalizer::SortInDocOrder(const SyntaxModel &Syntax, const std::vector<NodeId> &Found) const {
		// 空の候補は経路も作業領域も作らず返す
		if(Found.empty()) return {};
		// 前の整列で使った枠だけを消し，ノードの追加分だけ表を伸ばす
		std::vector<DocOrderPath> &Paths = DocOrder.Paths;
		for(const NodeId Id : DocOrder.Visited) Paths[Id] = {};
		DocOrder.Visited.clear();
		if(Paths.size() < Syntax.GetNodeCount()) Paths.resize(Syntax.GetNodeCount());
		const NodeId Root = Syntax.GetRoot();
		DocOrder.Visited.push_back(Root);
		Paths[Root].IsKnown = true;
		std::vector<NodeId> &Missing = DocOrder.Missing;
		std::vector<NodeId> Ordered;
		Ordered.reserve(Found.size());
		for(const NodeId Node : Found) {
			Syntax.Charge(1);
			if(Node == NoNode || Syntax.IsRemovedNode(Node)) continue;
			Missing.clear();
			NodeId Up = Node;
			while(Up != NoNode && !Paths[Up].IsKnown) {
				Syntax.Charge(1);
				Missing.push_back(Up);
				Up = Syntax.GetParentOf(Up);
			}
			if(Up == NoNode) continue;
			NodeId Parent = Up;
			for(std::vector<NodeId>::const_reverse_iterator Child = Missing.crbegin(); Child != Missing.crend(); ++Child) {
				DocOrder.Visited.push_back(*Child);
				Paths[*Child].IsKnown = true;
				Paths[*Child].Next = Paths[Parent].First;
				Paths[Parent].First = *Child;
				Parent = *Child;
			}
			Paths[Node].IsSelected = true;
		}
		// 祖先の経路だけを辿り，葉から根の探索と同じ作業列を再使用する
		Missing.clear();
		Missing.push_back(Root);
		while(!Missing.empty()) {
			const NodeId Id = Missing.back();
			const DocOrderPath &Path = Paths[Id];
			Missing.pop_back();
			Syntax.Charge(1);
			if(Path.IsSelected) Ordered.push_back(Id);
			const size_t Begin = Missing.size();
			for(NodeId Child = Path.First; Child != NoNode; Child = Paths[Child].Next) Missing.push_back(Child);
			const std::span<NodeId> Children = std::span<NodeId>(Missing).subspan(Begin);
			// スタックから構文順に取り出せる兄弟列は，其の儘使う
			const auto IsLater = [&Syntax](const NodeId Left, const NodeId Right) -> bool {
				Syntax.Charge(1);
				// 同じ親の現在の子位置が後かの返戻
				return Syntax.GetNode(Left).Idx > Syntax.GetNode(Right).Idx;
			};
			if(!WorkSort::IsSorted(std::span<const NodeId>(Children), IsLater)) {
				WorkSort::Stable(
					Children,
					IsLater,
					[&Syntax](const size_t Amount) -> void {
						Syntax.Charge(Amount);
					}
				);
			}
		}
		// 構文順の候補の返戻
		return Ordered;
	}

	/**
	 * 対象の祖先が正規化を許さない領域かの判定関数
	 * 対象の親から祖先の保護判定を読む
	 * 計算量：祖先深度 D に対し O(D + 1)，通常側で変更の無い祖先を共有する照会は期待値 O(1)
	 * @param Syntax 構文モデル
	 * @param Id 正規化の対象
	 * @return エラー又は保存するトークン列の祖先が在れば `true`
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 障壁又は祖先列のメモリを確保出来ない場合
	 * @throws std::length_error 祖先列又は障壁の要素数上限を超える場合
	 * @throws std::out_of_range 対象又は継承先がノード表に無い場合
	 */
	bool SyntaxNormalizer::HasProtectedNormalizationAncestor(const SyntaxModel &Syntax, const NodeId Id) const {
		// 対象の祖先が正規化を許さない領域かの判定
		const NodeId Parent = Syntax.GetParentOf(Id);
		// 祖先の障壁の返戻
		return Parent != NoNode && GetNormalizationBarrierOf(Syntax, Parent).IsProtected;
	}

	/**
	 * 投影済のマクロ束縛を部分木で共有する判定関数
	 * マクロを含む候補を書き換えず，含まない候補の書換は新しい束縛を作らない
	 * 計算量：同じ規則内で訪問するノード数 V とノード表の長さ N に対し O(N + V)，マクロ束縛の事実が無ければ O(1)
	 * @param Syntax 構文モデル
	 * @param Id 判定する部分木
	 * @return 展開に依って構造・型・値が変わり得る場合は true
	 * @throws FormatLimitExceeded 走査の作業予算を超える場合
	 * @throws std::bad_alloc 記録又は作業列を確保出来ない場合
	 * @throws std::length_error 記録又は作業列の要素数上限を超える場合
	 * @throws std::out_of_range ノードの番号が表に無い場合
	 */
	bool SyntaxNormalizer::HasMacroBindingIn(const SyntaxModel &Syntax, const NodeId Id) const {
		Syntax.Charge(1);
		// 空の部分木又はマクロ束縛の無いモデルの判定の返戻
		if(Id == NoNode || !HasMacroFacts) return false;
		if(MacroBindings.size() < Syntax.GetNodeCount()) {
			Syntax.Charge(Syntax.GetNodeCount() - MacroBindings.size());
			MacroBindings.resize(Syntax.GetNodeCount(), 0);
		}
		// 同じ部分木の判定済のマクロ束縛の返戻
		if(MacroBindings.at(Id)) return MacroBindings[Id] == 2;
		std::vector<std::pair<NodeId, bool>> &Pending = MacroBindingWork;
		Pending.clear();
		Pending.emplace_back(Id, false);
		while(!Pending.empty()) {
			const auto [Node, IsExit] = Pending.back();
			Pending.pop_back();
			Syntax.Charge(1);
			if(MacroBindings.at(Node)) continue;
			if(Syntax.GetNode(Node).Facts.Has(ConstraintKind::MacroBinding)) {
				MacroBindingNodes.push_back(Node);
				MacroBindings[Node] = 2;
				continue;
			}
			if(IsExit) {
				const bool HasBinding = Core::SyntaxModel::HasChildMatching(
					Syntax,
					Node,
					[&](const NodeId Child) -> bool {
						// 確定した子のマクロ束縛の返戻
						return MacroBindings.at(Child) == 2;
					}
				);
				MacroBindingNodes.push_back(Node);
				MacroBindings[Node] = HasBinding ? 2 : 1;
			} else {
				Pending.emplace_back(Node, true);
				for(const NodeId Child : Syntax.GetChildrenOf(Node)) Pending.emplace_back(Child, false);
			}
		}
		// 部分木の共有した判定の返戻
		return MacroBindings.at(Id) == 2;
	}

	/**
	 * 数値リテラルの正規化関数
	 * 揺れの有る接頭辞・接尾辞・指数記号・１６進桁の英字を大文字へ揃え，求める言語では小数点の前後の `0` を補う
	 * 計算量：テキストの長さ L と登録した型接尾辞の総長 S に対し O(L + S)
	 * @param Rules 文法の構文・意味の固定の定義
	 * @param Text 数値リテラルのテキスト
	 * @param ShouldPad 小数点の前後の `0` を補うか
	 * @return 正規化したテキスト
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	std::string SyntaxNormalizer::NormalizeNumericLiteral(
		const SyntaxRules &Rules,
		const std::string_view Text,
		const bool ShouldPad
	) {
		// 数値の頭の符号の長さ
		const size_t Sign = !Text.empty() && (Text[0] == '+' || Text[0] == '-') ? 1 : 0;
		const bool IsZeroLed = Text.size() > Sign + 1 && Text[Sign] == '0';
		const bool IsHex = IsZeroLed && (Text[Sign + 1] == 'x' || Text[Sign + 1] == 'X');
		const bool IsBinary = IsZeroLed && (Text[Sign + 1] == 'b' || Text[Sign + 1] == 'B');
		// １６進の仮数部は接頭辞の後から２進指数の前迄
		const size_t MantissaStart = Sign + (IsHex ? 2 : 0);
		const size_t MantissaEnd = ShouldPad && IsHex ? Text.find_first_of("pP", MantissaStart) : Text.size();
		const bool CanPad = ShouldPad && !IsBinary && (!IsHex || MantissaEnd != std::string_view::npos);
		const bool ShouldKeepBasePrefix = IsZeroLed && Rules.BasePrefixLetters.find(Text[Sign + 1]) != std::string_view::npos;
		const size_t SuffixStart = GetNumericSuffixStart(Rules, Text);
		std::string Fixed;
		Fixed.reserve(Text.size() + 1);
		for(size_t Idx = 0; Idx < Text.size(); ++Idx) {
			const unsigned char Char = Text[Idx];
			// 空の整数部の `0` の補完（１６進の桁を１０進の桁と区別する）
			if(
				CanPad && Idx == MantissaStart && Char == '.' &&
				(!IsHex || Idx + 1 < MantissaEnd && std::isxdigit(static_cast<unsigned char>(Text[Idx + 1])))
			) Fixed.push_back('0');
			// 登録した接頭辞・型接尾辞と BigInt の `n` を除く英字の大文字化
			Fixed.push_back(
				Idx >= SuffixStart || Char == 'n' || ShouldKeepBasePrefix && Idx == Sign + 1 ? Char : static_cast<char>(std::toupper(Char))
			);
			// 小数点の後の `0` の補完
			if(
				CanPad && Char == '.' && (
					IsHex ?
					Idx > MantissaStart && Idx + 1 == MantissaEnd && std::isxdigit(static_cast<unsigned char>(Text[Idx - 1])) :
					Idx + 1 == Text.size() || !std::isdigit(static_cast<unsigned char>(Text[Idx + 1]))
				)
			) Fixed.push_back('0');
		}
		// 正規化したテキストの返戻
		return Fixed;
	}

	/**
	 * Python の三重単引用符の本文を三重二重引用符の本文へ再表現する関数
	 * ３個以上連続する二重引用符と，本文末尾の二重引用符をエスケープする
	 * エスケープを解かない生文字列・`f` 文字列では，閉じのエスケープが必要な本文を変換しない
	 * 計算量：本文の長さ N と接頭辞の長さ P に対し O(N + P)
	 * @param Rules 文法の構文・意味の固定の定義
	 * @param Body 引用符の内側の原文
	 * @param Prefix 引用符の前の接頭辞
	 * @return 三重二重引用符の内側の本文（再表現出来ない場合は `std::nullopt`）
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	std::optional<std::string> SyntaxNormalizer::TripleDoubleQuotedBody(
		const SyntaxRules &Rules,
		const std::string_view Body,
		const std::string_view Prefix
	) {
		// エスケープを解かない接頭辞の有無
		bool IsVerbatim = false;
		for(const unsigned char Char : Prefix) {
			const unsigned char Lower = static_cast<char>(Char | 0X20);
			// エスケープの規則を確かめれない接頭辞の返戻
			if(Rules.QuotePrefixes.find(Lower) == std::string_view::npos) return std::nullopt;
			IsVerbatim = IsVerbatim || Rules.RawQuotePrefixes.find(Lower) != std::string_view::npos ||
			Rules.FormatQuotePrefixes.find(Lower) != std::string_view::npos;
		}
		std::string Output;
		Output.reserve(Body.size() + 8);
		for(size_t Pos = 0; Pos < Body.size();) {
			// エスケープとエスケープした字の組のコピー
			if(Body[Pos] == '\\') {
				Output.append(Body.substr(Pos, 2));
				Pos += 2;
				continue;
			}
			if(Body[Pos] != '"') {
				Output += Body[Pos++];
				continue;
			}
			const size_t Run = std::min(Body.find_first_not_of('"', Pos), Body.size()) - Pos;
			// 閉じに読まれない連なりのコピー
			if(Run < 3 && Pos + Run < Body.size()) Output.append(Run, '"');
			// エスケープを解かない本文で閉じに読まれる連なりは再表現出来ない事の返戻
			else if(IsVerbatim) return std::nullopt;
			else for(size_t Count = 0; Count < Run; ++Count) Output += "\\\"";
			Pos += Run;
		}
		// 再表現した本文の返戻
		return Output;
	}

	/**
	 * 単引用符の文字列の本文を二重引用符の本文へ再表現する関数
	 * 単引用符のエスケープを解いた値を，二重引用符のエスケープ・補間の規則で表し直す（同じ値を表せない形は変換しない）
	 * 計算量：本文の長さ L と接頭辞の長さ P に対し O(L + P)
	 * @param Rules 文法の構文・意味の固定の定義
	 * @param Body 引用符の内側の原文
	 * @param Prefix 引用符の前の接頭辞（Python の `r` / `f` 等，PHP の `b`）
	 * @param IsJsxAttribute エスケープを持たない JSX の属性値か
	 * @return 二重引用符の内側の本文（再表現出来ない場合は `std::nullopt`）
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	std::optional<std::string> SyntaxNormalizer::DoubleQuotedBody(
		const SyntaxRules &Rules,
		const std::string_view Body,
		const std::string_view Prefix,
		const bool IsJsxAttribute
	) {
		// Python の接頭辞の区分（生文字列と `f` 文字列はエスケープを解かない）
		bool IsRaw = false, IsFormat = false;
		for(const unsigned char Char : Prefix) {
			const unsigned char Lower = static_cast<char>(Char | 0X20);
			// 再表現出来ない接頭辞の拒否の返戻
			if(Rules.QuotePrefixes.find(Lower) == std::string_view::npos) return std::nullopt;
			// エスケープを解かない接頭辞の判定（Ruby の `%q` 等は再表現の対象外）
			IsRaw = IsRaw || Rules.RawQuotePrefixes.find(Lower) != std::string_view::npos;
			IsFormat = IsFormat || Rules.FormatQuotePrefixes.find(Lower) != std::string_view::npos;
		}
		// エスケープを解かない本文（JSX の属性値・生文字列・`f` 文字列）
		if(IsJsxAttribute || IsRaw || IsFormat) {
			// 二重引用符を含む本文，又は `f` 文字列でバックスラッシュを含む本文は再表現出来ない事の返戻
			if(Body.find('"') != std::string_view::npos || IsFormat && Body.find('\\') != std::string_view::npos) return std::nullopt;
			// 其の儘使える本文の返戻
			return std::string(Body);
		}
		// Ruby / PHP の単引用符内で，引用符・バックスラッシュ以外のエスケープを解かない規則の取得
		const bool IsLiteralBackslash = Rules.IsSingleQuoteBackslashLiteral;
		std::string Output;
		Output.reserve(Body.size() + 8);
		for(size_t Pos = 0; Pos < Body.size(); ++Pos) {
			// 単引用符のエスケープと二重引用符側のエスケープへの再表現
			if(const unsigned char Char = Body[Pos]; Char == '\\') {
				// 閉じの無いエスケープは再表現出来ない事の返戻
				if(Pos + 1 == Body.size()) return std::nullopt;
				if(const unsigned char Next = Body[++Pos]; Next == '\'') Output += '\'';
				else if(IsLiteralBackslash) {
					// エスケープの `\\` と字の儘の `\` は，二重引用符では何れも `\\` と書く
					Output += "\\\\";
					if(Next != '\\') --Pos;
				} else {
					// 引用符に依らず同じ意味のエスケープの保持
					Output += '\\';
					Output += Next;
				}
			} else if(Char == '"') Output += "\\\"";
			else if(Rules.DoesInterpolateDollar && Char == '$') Output += "\\$";
			// 補間を始めない様にエスケープした `#`
			else if(Char == '#' && Pos + 1 < Body.size() && Rules.HashInterpolationHeads.find(Body[Pos + 1]) != std::string_view::npos) {
				Output += "\\#";
			} else Output += Char;
		}
		// 再表現した本文の返戻
		return Output;
	}

	/**
	 * 文法に共通の生成形式の取得関数
	 * 計算量：文法の生成形式の数 F に対し O(F)
	 * @param Syntax 構文モデル
	 * @param Operation 生成する操作
	 * @return 対応する形式（無ければ無）
	 */
	const CanonicalForm *SyntaxNormalizer::GetGeneratedFormOf(const SyntaxModel &Syntax, const FormOperation Operation) {
		// 文法に共通の生成形式の取得
		const std::vector<CanonicalForm> &Forms = Syntax.GetRules().GeneratedForms;
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
	 * 生成形式の部品からノードを組み立てる関数
	 * 計算量：生成形式の部品数 P と生成するテキストの総長 B に対し O(P² + B)
	 * @param Syntax 構文モデル
	 * @param Form 文法の生成形式
	 * @param Role 組み立てる部品の役割
	 * @param Body 移す本体
	 * @param HasTrueName 真偽値の名前を使えるか
	 * @return 部品と其の子を組み立てたノード
	 * @throws FormatLimitExceeded 生成形式が役割の部品を持たない場合（文法の定義の不具合）
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	GeneratedNode SyntaxNormalizer::GeneratedPart(
		const SyntaxModel &Syntax,
		const CanonicalForm &Form,
		const FormPartRole Role,
		const NodeId Body,
		const bool HasTrueName
	) {
		// 生成形式の部品からのノードの組立
		const FormPart *const Part = Form.GetPartOf(Role);
		if(!Part) throw FormatLimitExceeded("a generated form has no part for the role", FailureReason::FormatterDefect);
		GeneratedNode Result;
		Result.IsNamed = Part->IsNamed;
		Result.Def = *Part;
		if(Part->IsNamed) Result.Lexeme = Part->FixedLexeme;
		else Result.Kind = Part->FixedLexeme;
		if(Part->Field != ChildRole::None) Result.Fields = Syntax.GetFieldBit(Part->Field);
		if(Role == FormPartRole::Body) Result.Existing = Body;
		for(const FormPart &Child : Form.Parts) {
			if(
				Child.Role == Role || Child.Parent != Role || Form.Operation == FormOperation::UnconditionalLoop &&
				(Child.Role == FormPartRole::IntVal && HasTrueName || Child.Role == FormPartRole::TrueVal && !HasTrueName)
			) continue;
			Result.Children.push_back(GeneratedPart(Syntax, Form, Child.Role, Body, HasTrueName));
		}
		// 部品から構築したノードの返戻
		return Result;
	}

	/**
	 * 候補より前の同じ範囲の束縛の判定関数
	 * 計算量：此の名前の導入・終了の総数 L と名前の長さ B に対し期待値 O(B + log(L + 1))
	 * @param Syntax 作業予算を計上する構文
	 * @param Idx 現在の規則の評価で収集したインデックス
	 * @param Target 見出のノード
	 * @param Name 本体が書き条件が読む名前
	 * @return 同じ有効範囲で既に束縛済なら `true`
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::out_of_range 束縛の履歴の番号が履歴表に無い場合
	 */
	bool SyntaxNormalizer::IsBoundBefore(
		const SyntaxModel &Syntax,
		const StmtBindingIdx &Idx,
		const NodeId Target,
		const std::string &Name
	) {
		// 候補より前の同じ範囲の束縛の判定
		const std::unordered_map<NodeId, BindingPoint>::const_iterator Point = Idx.Points.find(Target);
		// 見出が無い場合の返戻
		if(Point == Idx.Points.end()) return false;
		Syntax.Charge(Name.size() + 1);
		const std::unordered_map<std::string, std::vector<BindingEvent>>::const_iterator Found = Idx.Names.find(Name);
		// 此の名前が１度も導入されて居ない場合の返戻
		if(Found == Idx.Names.end()) return false;
		const std::vector<BindingEvent> &Events = Found->second;
		// 二分探索で調べる変化の計上
		Syntax.Charge(std::bit_width(Events.size()));
		// 候補に入る直前迄の変化の数
		const size_t Begin = static_cast<size_t>(
			std::ranges::upper_bound(Events, Point->second.Event, std::ranges::less(), &BindingEvent::Event) - Events.begin()
		);
		// 此の候補より前の可視束縛が無い場合の返戻
		if(!Begin || Events[Begin - 1].Version == SIZE_MAX) return false;
		// 同じ関数の範囲内に在る前置束縛の返戻
		return Idx.Versions.at(Events[Begin - 1].Version).Depth >= Point->second.ScopeStart;
	}

	/**
	 * 生成形式の部品からノードを作る関数
	 * 計算量：固定の文法の部品又は規則の表に対し O(1)
	 * @param Form 文法が与えた生成形式
	 * @param Part 生成する部品
	 * @return 種別と固定トークンを与えたノード
	 * @throws FormatLimitExceeded 部品が無い場合
	 * @throws std::bad_alloc 固定トークンのコピーのメモリを確保出来ない場合
	 * @throws std::length_error 固定トークンが文字列の長さの上限を超える場合
	 */
	GeneratedNode SyntaxNormalizer::FormNode(const CanonicalForm &Form, const FormPartRole Part) {
		// 生成形式の部品からのノードの生成
		GeneratedNode Result;
		const FormPart *const Selected = Form.GetPartOf(Part);
		if(!Selected) throw FormatLimitExceeded("a generated node has no grammar form part", FailureReason::FormatterDefect);
		Result.Def = *Selected;
		Result.Lexeme = Selected->FixedLexeme;
		// 部品のノードの返戻
		return Result;
	}

	/**
	 * 対象自身を含むエラー障壁の判定関数
	 * 計算量：祖先深度 D に対し O(D + 1)，通常側で変更の無い祖先を共有する照会は期待値 O(1)
	 * @param Syntax 現在の収集が読む構文
	 * @param Id 対象
	 * @return 対象又は祖先がエラー領域なら `true`
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 障壁又は祖先列のメモリを確保出来ない場合
	 * @throws std::length_error 祖先列又は障壁の要素数上限を超える場合
	 * @throws std::out_of_range 対象又は継承先がノード表に無い場合
	 */
	bool SyntaxNormalizer::HasErrorNormalizationAncestor(const SyntaxModel &Syntax, const NodeId Id) const {
		// 対象を含む障壁の返戻
		return Id != NoNode && GetNormalizationBarrierOf(Syntax, Id).HasError;
	}

	/**
	 * 単一文本体の後置形式の収集関数
	 * 収集済の読取・書込名と束縛を使い，対象ノードの条件を単一文の後ろへ移せるかを判定する
	 * 計算量：形式・子・祖先の総探索量 Q，名前と生成テキストの総処理量 B，束縛照会の総費用 I に対し期待値 O(Q + B + I)
	 * @param Syntax 構文モデル
	 * @param Id 現在の規則が評価する１ノード又は１所有者
	 * @param Bindings 確定した後置形式を反映した前置束縛インデックス
	 * @param PostfixStmts 同じ走査で後置形式へ書き換えると決めた文（其の束縛は外側のインデックスへ出さない）
	 * @param Summaries 子が確定した読取・書込名と構文の制約
	 * @param Plan 構造を書き換える計画
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectStmtNode(
		const SyntaxModel &Syntax,
		const NodeId Id,
		const StmtBindingIdx &Bindings,
		const std::unordered_map<NodeId, NodeId> &PostfixStmts,
		const std::unordered_map<NodeId, StmtFacts> &Summaries,
		RewritePlan &Plan
	) const {
		// 候補の判定（束縛の有効範囲の始まりの順を受け取る）
		for(const CanonicalForm &Form : Syntax.GetNode(Id).Facts.Forms.GetView()) {
			// 後置へ移せる単一の文の本体の判定
			if(Form.Operation != FormOperation::PostfixCond) continue;
			const NodeId Parent = Syntax.GetParentOf(Id);
			const NodeId Cond = Syntax.GetFieldChild(Id, ChildRole::Cond);
			NodeId Body = Syntax.GetFieldChild(Id, ChildRole::Body);
			if(Body == NoNode) Body = Syntax.GetFieldChild(Id, ChildRole::Consequence);
			// 単一値のグループの外側で，値を取る語の実引数である事を確認する
			NodeId OuterParen = Parent;
			while(
				Syntax.Is(OuterParen, Role::GroupingParen) && Syntax.GetChildCount(OuterParen) == 3 &&
				Syntax.GetNamedChildCount(OuterParen) == 1 && Syntax.Is(Syntax.GetParentOf(OuterParen), Role::GroupingParen)
			) {
				Syntax.Charge(1);
				OuterParen = Syntax.GetParentOf(OuterParen);
			}
			if(
				Syntax.GetFieldChild(Id, ChildRole::Alternative) != NoNode || !(
					Syntax.Is(Parent, Role::StmtSeq) || Syntax.GetParentOf(Parent) == NoNode || Syntax.Is(Parent, Role::ExplicitKeywordBody) ||
					Syntax.Is(OuterParen, Role::GroupingParen) && Syntax.GetChildCount(OuterParen) == 3 &&
					Syntax.GetNamedChildCount(OuterParen) == 1 && GetControlValParenOwner(Syntax, OuterParen, Id) != NoNode
				)
			) continue;
			NodeId Stmt = NoNode;
			uint32_t StmtCount = 0;
			Core::SyntaxModel::ForEachStmtOf(
				Syntax,
				Body,
				[&](const NodeId Child) -> bool {
					Stmt = Child;
					return ++StmtCount < 2;
				}
			);
			// 条件の読取と構文の制約の検査
			if(
				StmtCount != 1 || !(
					(!Syntax.Is(Stmt, Role::KeywordBlock) || PostfixStmts.contains(Stmt)) && !Syntax.HasLastTokenRole(Stmt, Role::OpenRangeToken) &&
					!Syntax.HasLastTokenRole(Stmt, Role::ColonToken)
				)
			) continue;
			const StmtFacts &CondFacts = Summaries.at(Cond), &BodyFacts = Summaries.at(Body);
			if(!CondFacts.IsCondSafe || !BodyFacts.IsBindingSafe) continue;
			if(CondFacts.Reads && BodyFacts.Writes) {
				const StmtNames &Reads = *CondFacts.Reads, &Writes = *BodyFacts.Writes;
				// 本体で書き換える条件の名前が先に束縛されて居るかの検査
				if(
					const StmtNames &Smaller = Reads.size() < Writes.size() ? Reads : Writes; std::ranges::any_of(
						Smaller,
						// 条件を移せない名前を探す述語
						[&](const std::string &Name) -> bool {
							Syntax.Charge(Name.size() + 1);
							// 条件を移せない書換が在る事の返戻
							return Reads.contains(Name) && Writes.contains(Name) && !IsBoundBefore(Syntax, Bindings, Id, Name);
						}
					)
				) continue;
			}
			// 遅延した本体の移動と後置形式への組直
			for(const NodeId Child : Syntax.GetChildrenOf(Body)) if(Syntax.Is(Child, Role::DeferredLiteralBody)) {
				RewriteStep Move { RewriteKind::Move, Child };
				Move.DstFields = 0;
				Plan.Steps.push_back(std::move(Move));
			}
			// 本体ラッパーの前後・内部のアンカーの単一文への移譲
			Plan.Steps.push_back(RewriteStep{ RewriteKind::Unwrap, Body });
			// 本体より前で除くトークンのアンカーは単一文の前へ移譲
			// 除く閉じの先行アンカーは後続の境界へ移譲
			// 除く閉じの末尾アンカーは後置条件の後へ移譲
			// 除く閉じの内部アンカーは条件の末尾トークンへ移譲
			NodeId Following = Id;
			while(Following != NoNode && Syntax.GetNextSibling(Following) == NoNode) {
				Syntax.Charge(1);
				Following = Syntax.GetParentOf(Following);
			}
			const NodeId Next = Following == NoNode ? NoNode : Syntax.GetNextSibling(Following);
			const NodeId Keyword = Syntax.GetChild(Id, 0);
			const AnchorTarget Before { Syntax.GetFirstEdge(Stmt).Leaf, false }, After { Id, true };
			const AnchorTarget Boundary = Next == NoNode ? After : AnchorTarget{ Syntax.GetFirstEdge(Next).Leaf, false };
			const AnchorTarget Inner { Syntax.GetLastEdge(Cond).Leaf, true };
			for(const NodeId Child : Syntax.GetChildrenOf(Id)) {
				if(Child == Keyword || Child == Cond || Child == Body) continue;
				const bool IsBeforeBody = Syntax.GetNode(Child).Idx < Syntax.GetNode(Body).Idx;
				Plan.Steps.push_back(
					RewriteStep{
						RewriteKind::Remove,
						Child,
						{},
						{ Child, IsBeforeBody ? Before : Boundary, IsBeforeBody ? Before : After, IsBeforeBody ? Before : Inner }
					}
				);
			}
			GeneratedNode Result = FormNode(Form, FormPartRole::Result);
			Result.Fields = Syntax.GetNode(Id).Fields;
			Result.Children = {
				{ {}, false, {}, uint64_t{ 1 } << static_cast<uint8_t>(ChildRole::Body), Stmt },
				{ {}, false, {}, 0, Keyword },
				{ {}, false, {}, uint64_t{ 1 } << static_cast<uint8_t>(ChildRole::Cond), Cond }
			};
			RewriteStep Step { RewriteKind::Reshape, Id };
			Step.Inserted = std::move(Result);
			Plan.Steps.push_back(std::move(Step));
		}
		// 終了
		return;
	}

	/**
	 * 引数の無い返戻文の生成関数
	 * 計算量：固定の文法の部品又は規則の表に対し O(1)
	 * @param Syntax 構文モデル
	 * @return 言語の文法の返戻文のノード
	 * @throws FormatLimitExceeded 返戻文の生成形式又は必要な部品が無い場合（文法の定義の不具合）
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 */
	GeneratedNode SyntaxNormalizer::ArglessReturn(const SyntaxModel &Syntax) {
		// 引数の無い返戻文の生成
		const CanonicalForm *const Form = GetGeneratedFormOf(Syntax, FormOperation::ArglessReturn);
		if(!Form) throw FormatLimitExceeded("missing generated argless return form", FailureReason::FormatterDefect);
		// 返戻文の返戻
		return GeneratedPart(Syntax, *Form, FormPartRole::Result, NoNode, true);
	}

	/**
	 * 値を使わない位置の後置の増減の前置化の収集関数（整形仕様 5.2）
	 * 値が使われない文又は `for` の更新の位置で，後置の増減の演算子を前へ移す
	 * 多重定義が可能な演算子は，組込の算術型又はポインタ型と確認した名前だけを対象とする
	 * 計算量：子・ラッパーの探索費用 Q と被演算子のテキスト長 B に対し O(Q + B)
	 * @param Syntax 構文モデル
	 * @param Id ノード
	 * @param Scalars 現在の有効範囲で可視な名前と組込型の判定
	 * @param ScopeStart 可視な宣言の開始の深さ
	 * @param Plan 書換の計画の格納先
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectIncrementStep(
		const SyntaxModel &Syntax,
		const NodeId Id,
		const std::unordered_map<std::string_view, ScalarBinding> &Scalars,
		const size_t ScopeStart,
		RewritePlan &Plan
	) const {
		// 値を使わない位置の後置の増減の前置化の収集
		const uint32_t Count = Syntax.GetChildCount(Id);
		const NodeId Operator = Syntax.GetChild(Id, Count - 1);
		// 増減の後置の演算子で終わらない場合の終了
		if(
			Count < 2 || Syntax.IsNamed(Operator) || !Syntax.IsTokenRole(Operator, Role::ChangeByOneToken) || HasMacroBindingIn(Syntax, Id)
		) return;
		if(Syntax.Is(Id, Role::OverloadableIncrement)) {
			NodeId Operand = Syntax.GetNamedChild(Id, 0);
			while(Syntax.Is(Operand, Role::GroupingParen) && Syntax.GetNamedChildCount(Operand) == 1) {
				Syntax.Charge(1);
				Operand = Syntax.GetNamedChild(Operand, 0);
			}
			// 型を確認出来ない被演算子の場合の終了
			if(!Syntax.Is(Operand, Role::Identifier)) return;
			const std::string &Name = Syntax.GetNode(Operand).Lexeme;
			Syntax.Charge(Name.size() + 1);
			// 多重定義の有り得る型の場合の終了
			if(
				const std::unordered_map<std::string_view, ScalarBinding>::const_iterator Found = Scalars.find(Name);
				Found == Scalars.end() || Found->second.Depth < ScopeStart || !Found->second.IsBuiltin
			) return;
		}
		// 要素１つの括弧を外側へ辿った式と其の親
		NodeId Expr = Id;
		for(
			NodeId Up = Syntax.GetParentOf(Id);
			Syntax.Is(Up, Role::GroupingParen) && Syntax.GetNamedChildCount(Up) == 1 && Syntax.GetChildCount(Up) == 3;
			Up = Syntax.GetParentOf(Up)
		) Expr = Up;
		// 枝・文・切替の規則で値を使う位置の終了
		if(const NodeId Host = Syntax.GetParentOf(Expr); !Syntax.IsValUnusedCtx(Expr, Host)) return;
		// 後置と前置の種別を分ける文法の前置の種別
		if(
			const FormPart *const PrefixDef = Syntax.Is(Id, Role::PostfixUnaryExpr) ?
			&Syntax.GetRules().PrefixUnaryDef :
			Syntax.Is(Id, Role::PostfixExpr) ? &Syntax.GetRules().PrefixDef : nullptr;
			PrefixDef && PrefixDef->Def
		) {
			// 前置の式への組直
			RewriteStep Prefix { RewriteKind::Reshape, Id };
			Prefix.Inserted = { {}, true, { { {}, false, {}, {}, Operator } }, 0, NoNode, {}, *PrefixDef };
			for(uint32_t Idx = 0; Idx + 1 < Count; ++Idx) {
				const NodeId Operand = Syntax.GetChild(Id, Idx);
				Prefix.Inserted.Children.push_back({ {}, false, {}, Syntax.GetNode(Operand).Fields, Operand });
			}
			Plan.Steps.push_back(std::move(Prefix));
			// 前置の増減式へ組み直した場合の終了
			return;
		}
		RewriteStep Move { RewriteKind::Move, Operator };
		Move.Dst = Id;
		Move.Idx = 0;
		Plan.Steps.push_back(std::move(Move));
		// 終了
		return;
	}

	/**
	 * 役割から選んだ構文順の対象の取得関数
	 * 計算量：候補数 C と選択した経路のノード数 P に対し O(C + P log(P + 1))
	 * @param Syntax 構文モデル
	 * @param Roles 対象の役割
	 * @return 構文順の対象
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	std::vector<NodeId> SyntaxNormalizer::GetCandidates(const SyntaxModel &Syntax, const std::initializer_list<Role> Roles) const {
		// 役割から選んだ構文順の対象の取得
		std::vector<NodeId> Found;
		for(const Role Wanted : Roles) {
			const std::span<const NodeId> Indexed = Syntax.GetNodesWith(Wanted);
			Syntax.Charge(Indexed.size());
			Found.insert(Found.end(), Indexed.begin(), Indexed.end());
		}
		// 役割インデックスから選んだ構文順の対象の返戻
		return SortInDocOrder(Syntax, Found);
	}

	/**
	 * 有効範囲の前置束縛インデックスの取得関数
	 * 導入と範囲終了を名前毎に１度記録する
	 * 書換計画を集める時は，子の読取・書込名の集合を大きい側へ統合し，子の要約の参照を解放する
	 * 計算量：ノード数 V とトークン長 B に対し O(V + B log(V + 1))，ネストした単一文本体は O(V + B)
	 * @param Syntax 現在の構文
	 * @param Scope 最内の関数又は根
	 * @param Plan 同じ走査で後置形式への書換を集める計画（照会だけの場合は無）
	 * @return 各見出の直前の束縛と，名前毎の導入・範囲終了の履歴
	 * @throws FormatLimitExceeded 集約又は事実の収集が作業予算を超えた場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	SyntaxNormalizer::StmtBindingIdx SyntaxNormalizer::BuildStmtBindingIdx(
		const SyntaxModel &Syntax,
		const NodeId Scope,
		RewritePlan *const Plan
	) const {
		// 有効範囲の前置束縛インデックスの取得
		StmtBindingIdx Idx;
		std::unordered_map<NodeId, StmtFacts> Summaries;
		// 文が読む又は書く名前の集合の統合
		const auto MergeNames = [&](const std::shared_ptr<StmtNames> &Input, std::shared_ptr<StmtNames> &Target) -> void {
			// 名前を持たない部分木の場合の終了
			if(!Input) return;
			if(!Target) Target = Input;
			else if(Target != Input) {
				std::shared_ptr<StmtNames> Smaller = Input;
				if(Target->size() < Input->size()) {
					Smaller = Target;
					Target = Input;
				}
				for(const std::string &Name : *Smaller) {
					Syntax.Charge(Name.size() + 1);
					Target->insert(Name);
				}
			}
			// 終了
			return;
		};
		std::unordered_map<NodeId, NodeId> PostfixStmts;
		const size_t FirstStep = Plan ? Plan->Steps.size() : 0;
		// 束縛のインデックスの時点を登録する走査の状態
		struct Frame {
			NodeId Node;
			bool IsExit; // 有効範囲を離れる枠か
			size_t Size; // 範囲を離れる時に復元する可視束縛の積上数
			size_t ScopeStart; // 現在の関数の束縛の開始位置
		};
		std::vector<size_t> Active;
		std::unordered_map<std::string, size_t> Visible;
		size_t Event = 0;
		// 束縛のインデックスの時点の更新
		const auto Advance = [&]() -> void {
			Syntax.Charge(1);
			if(Event == SIZE_MAX) throw FormatLimitExceeded("binding events exhausted", FailureReason::FormatterDefect);
			++Event;
			// 終了
			return;
		};
		// 現在可視な名前の束縛と其の時点のインデックスへの登録
		const auto Bind = [&](const std::string Name) -> void {
			Advance();
			Syntax.Charge(Name.size());
			Syntax.Charge(1);
			const std::unordered_map<std::string, size_t>::const_iterator Prev = Visible.find(Name);
			const size_t Version = Idx.Versions.size();
			Idx.Versions.push_back({ Name, Prev == Visible.end() ? SIZE_MAX : Prev->second, Active.size() });
			Visible.insert_or_assign(Name, Version);
			Idx.Names[Name].push_back({ Event, Version });
			Active.push_back(Version);
			// 終了
			return;
		};
		std::vector<Frame> Frames { { Scope, false, 0, 0 } };
		while(!Frames.empty()) {
			const Frame Top = Frames.back();
			Frames.pop_back();
			Syntax.Charge(1);
			if(Top.IsExit) {
				if(Plan) {
					StmtFacts &Summary = Summaries[Top.Node];
					Summary.IsCondSafe = !Syntax.Is(Top.Node, Role::AssignmentExpr) && !Syntax.Is(Top.Node, Role::CompoundAssignment) &&
					!Syntax.Is(Top.Node, Role::Regex) && !Syntax.Is(Top.Node, Role::HeredocBeginning);
					if(Syntax.Is(Top.Node, Role::Identifier)) {
						Summary.Reads = std::make_shared<StmtNames>();
						Summary.Reads->insert(Syntax.GetText(Top.Node));
					}
					// 本体が束縛する名前（代入の左辺・多重代入の名前，照合のパターンの名前，正規表現の名前付の捕捉）
					const auto AddBoundNames = [&](const NodeId Binding) -> void {
						if(Syntax.Is(Binding, Role::Regex)) {
							// 名前付の捕捉の名前（名前は語の字で始まり非 ASCII の字も含む，先読・後読の `(?<=` / `(?<!` は名前を持たない）
							const std::string Pattern = Syntax.GetText(Binding);
							for(const SyntaxRules::RegexCapture &Capture : Syntax.GetRules().RegexNamedCaptures) for(
								size_t At = Pattern.find(Capture.Open);
								At != std::string::npos;
								At = Pattern.find(Capture.Open, At + Capture.Open.size())
							) {
								const size_t Start = At + Capture.Open.size();
								if(Start == Pattern.size() || !IsWordChar(Pattern[Start])) continue;
								// 名前の閉じの無い捕捉は束縛を判別出来ない為に書き換えない
								if(const size_t End = Pattern.find(Capture.Close, Start); End == std::string::npos) Summary.IsBindingSafe = false;
								else Summary.Writes->insert(Pattern.substr(Start, End - Start));
							}
							// 正規表現の捕捉名を収集した場合の終了
							return;
						}
						// 代入は左辺，照合はパターンの全体の名前（受手・添字の中の名前は束縛しない）
						const NodeId Left = Syntax.GetFieldChild(Binding, ChildRole::Left);
						std::vector<NodeId> Names { Left == NoNode ? Binding : Left };
						while(!Names.empty()) {
							const NodeId Part = Names.back();
							Names.pop_back();
							Syntax.Charge(1);
							if(Syntax.Is(Part, Role::Identifier)) Summary.Writes->insert(Syntax.GetText(Part));
							if(Syntax.Is(Part, Role::ElementRef) || Syntax.Is(Part, Role::ReceiverCall)) continue;
							for(const NodeId Next : Syntax.GetChildrenOf(Part)) Names.push_back(Next);
						}
						// 終了
						return;
					};
					if(Syntax.Is(Top.Node, Role::BindingExpr)) {
						Summary.Writes = std::make_shared<StmtNames>();
						AddBoundNames(Top.Node);
					}
				}
				if(Plan && Syntax.Is(Top.Node, Role::KeywordBlock) && !HasErrorNormalizationAncestor(Syntax, Top.Node)) {
					const size_t First = Plan->Steps.size();
					CollectStmtNode(Syntax, Top.Node, Idx, PostfixStmts, Summaries, *Plan);
					if(First != Plan->Steps.size()) PostfixStmts.emplace(Top.Node, Top.Node);
				}
				if(Plan) {
					StmtFacts &Summary = Summaries.at(Top.Node);
					for(const NodeId Child : Syntax.GetChildrenOf(Top.Node)) {
						Syntax.Charge(1);
						const std::unordered_map<NodeId, StmtFacts>::const_iterator Found = Summaries.find(Child);
						if(Found == Summaries.end()) continue;
						const StmtFacts &Inner = Found->second;
						Summary.IsCondSafe &= Inner.IsCondSafe;
						Summary.IsBindingSafe &= Inner.IsBindingSafe;
						MergeNames(Inner.Reads, Summary.Reads);
						MergeNames(Inner.Writes, Summary.Writes);
						Summaries.erase(Found);
					}
				}
				if(Syntax.Is(Top.Node, Role::KeywordBlock) && !PostfixStmts.contains(Top.Node)) while(Active.size() > Top.Size) {
					Advance();
					const BoundVersion &Version = Idx.Versions.at(Active.back());
					Active.pop_back();
					Syntax.Charge(Version.Name.size());
					Syntax.Charge(1);
					if(Version.Prev == SIZE_MAX) Visible.erase(Version.Name);
					else Visible.insert_or_assign(Version.Name, Version.Prev);
					Idx.Names[Version.Name].push_back({ Event, Version.Prev });
				} else if(Syntax.Is(Top.Node, Role::AssignmentExpr) || Syntax.Is(Top.Node, Role::CompoundAssignment)) {
					Bind(Syntax.GetText(Syntax.GetFieldChild(Top.Node, ChildRole::Left)));
				} else if(Syntax.Is(Top.Node, Role::MethodParams)) for(const NodeId Param : Syntax.GetChildrenOf(Top.Node)) {
					Bind(Syntax.GetText(Param));
				}
				continue;
			}
			if(Top.Node != Scope && (Syntax.Is(Top.Node, Role::TailValFunction) || Syntax.Is(Top.Node, Role::ExplicitResultFunction))) {
				continue;
			}
			const size_t ScopeStart = Syntax.Is(Syntax.GetParentOf(Top.Node), Role::TailValFunction) ? Top.Size : Top.ScopeStart;
			if(Syntax.Is(Top.Node, Role::KeywordBlock)) Idx.Points.emplace(Top.Node, BindingPoint{ Event, ScopeStart });
			Frames.push_back({ Top.Node, true, Active.size(), ScopeStart });
			const std::span<const NodeId> Children = Syntax.GetChildrenOf(Top.Node);
			for(size_t Child = Children.size(); Child--;) Frames.push_back({ Children[Child], false, Active.size(), ScopeStart });
		}
		// 後送本文は，除く本体へ途中で入れず，値のラッパーも越えて文の並びへ直接移す
		if(Plan) {
			std::unordered_map<NodeId, uint32_t> Insertions;
			std::vector<NodeId> Path;
			for(size_t Pos = FirstStep; Pos < Plan->Steps.size(); ++Pos) {
				Syntax.Charge(1);
				RewriteStep &Step = Plan->Steps[Pos];
				if(Step.Kind != RewriteKind::Move || !Syntax.Is(Step.Target, Role::DeferredLiteralBody)) continue;
				Path.clear();
				NodeId Owner = Syntax.GetParentOf(Syntax.GetParentOf(Step.Target));
				while(true) {
					Syntax.Charge(1);
					if(
						const std::unordered_map<NodeId, NodeId>::const_iterator Found = PostfixStmts.find(Owner);
						Found != PostfixStmts.end() && Found->second != Owner
					) {
						Owner = Found->second;
						break;
					}
					Path.push_back(Owner);
					const NodeId Parent = Syntax.GetParentOf(Owner), Above = Syntax.GetParentOf(Parent);
					if(PostfixStmts.contains(Above)) Owner = Above;
					else if(
						Parent == NoNode || Syntax.Is(Parent, Role::StmtHost) || Syntax.Is(Parent, Role::StmtSeq) ||
						Syntax.Is(Parent, Role::ExplicitKeywordBody) || Syntax.Is(Parent, Role::KeywordBlock)
					) break;
					else Owner = Parent;
				}
				Step.Dst = Syntax.GetParentOf(Owner);
				Step.Idx = Syntax.GetNode(Owner).Idx + 1;
				for(const NodeId Part : Path) {
					Syntax.Charge(1);
					PostfixStmts.insert_or_assign(Part, Owner);
				}
				// 先行する本文の挿入が，同じ並びの後続の文の位置をずらす分も計画へ含める
				Step.Idx += Insertions[Step.Dst]++;
			}
		}
		// 現在の規則の評価の前置束縛インデックスの返戻
		return Idx;
	}

	/**
	 * 数値リテラル１ノードの正規化の収集関数
	 * 規則が選ぶ対象だけを読み，異なるテキストの書換を収集する
	 * 計算量：障壁取得の費用 A と数値トークンの長さ B に対し O(A + B)，同じ規則の評価の A はノード当たり期待値償却 O(1)
	 * @param Syntax 規則が読む構文モデル
	 * @param Id 役割インデックスから選んだ対象
	 * @param Plan 現在の収集の書換計画
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectNumStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const {
		// 対象でない場合の終了
		if(HasProtectedNormalizationAncestor(Syntax, Id)) return;
		const SyntaxNode &Node = Syntax.GetNode(Id);
		// 自身が逐語領域の場合の終了
		if(Node.Facts.Has(ConstraintKind::Verbatim)) return;
		Syntax.Charge(Node.Lexeme.size());
		if(
			std::string Fixed = NormalizeNumericLiteral(Syntax.GetRules(), Node.Lexeme, Syntax.GetRules().ShouldPadDecPoint);
			Fixed != Node.Lexeme
		) Plan.Steps.push_back({ RewriteKind::SetLexeme, Id, std::move(Fixed) });
		// 終了
		return;
	}

	/**
	 * CSS の色と宣言末尾の正規化の収集関数（整形仕様 5.2）
	 * １６進色の桁を大文字へ揃え（セレクタの ID と参照は除く），ブロックの最後の宣言にも `;` を付ける（空のカスタムプロパティの空白の値を保つ）
	 * 計算量：参照するノードと祖先の総数 N，テキストの総長 L に対し O(N + L)
	 * @param Syntax 構文モデル
	 * @param Id ノード
	 * @param Plan 書換の計画の格納先
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectCssStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) {
		// CSS の色と宣言末尾の正規化の収集
		const SyntaxNode &Node = Syntax.GetNode(Id);
		const NodeId Parent = Node.Parent;
		// 空の並びと，幅０の仮引数を補う解析結果の括弧の除去
		if(Syntax.Is(Id, Role::MixinRule)) {
			for(const NodeId Child : Node.Children) {
				Syntax.Charge(1);
				if(
					!Syntax.IsAny(Child, { Role::CallArgs, Role::MixinParams }) ||
					!Syntax.IsEdgeCharRole(Syntax.GetFirstEdge(Child), Role::OpeningParenLead)
				) continue;
				const uint32_t NamedChildCount = Syntax.GetNamedChildCount(Child);
				if(
					const bool IsEmpty = !NamedChildCount || NamedChildCount == 1 && Syntax.GetText(Syntax.GetNamedChild(Child, 0)).empty();
					IsEmpty
				) {
					const AnchorTarget Prev { Syntax.GetPrevSibling(Child), true };
					RewriteStep Step { RewriteKind::Remove, Child, {}, { Child, Prev, Prev, Prev } };
					Plan.Steps.push_back(std::move(Step));
				}
				// 最初の括弧の並びだけを見る事の終了
				return;
			}
			// 空のミックスイン仮引数又は実引数の省略条件を確認した場合の終了
			return;
		}
		if(Syntax.Is(Id, Role::ColorVal) && Node.Children.empty()) {
			// セレクタの中の色の場合の終了
			if(
				const NodeId Host = Syntax.Is(Parent, Role::CallArgs) ? Syntax.GetParentOf(Parent) : Parent;
				Syntax.Is(Host, Role::SelectorHost)
			) return;
			Syntax.Charge(Node.Lexeme.size());
			std::string Upper = Node.Lexeme;
			for(char &Char : Upper) {
				Syntax.Charge(1);
				if(Char >= 'a' && Char <= 'f') Char = static_cast<char>(Char + 'A' - 'a');
			}
			if(Upper != Node.Lexeme) Plan.Steps.push_back({ RewriteKind::SetLexeme, Id, std::move(Upper) });
			// 色のテキストを正規化した場合の終了
			return;
		}
		// 型付の宣言でない場合の終了
		if(!Syntax.Is(Id, Role::TypedDecl)) return;
		// ブロックの最後の宣言でない場合の終了
		if(
			const NodeId Next = Syntax.GetNextSibling(Id);
			Next != NoNode && (Syntax.IsNamed(Next) || !Syntax.IsTokenRole(Next, Role::ClosingBraceToken))
		) return;
		if(Node.Children.empty()) {
			// テキストを保つ宣言の末尾への区切の追加
			const std::string Terminator = Generation::GetGeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::Terminator);
			if(!Node.Lexeme.ends_with(Terminator)) Plan.Steps.push_back({ RewriteKind::SetLexeme, Id, Node.Lexeme + Terminator });
			// テキストだけの宣言へ区切を補った場合の終了
			return;
		}
		const NodeId Last = Node.Children.back();
		// 既に文の終端の語で終わる場合の終了
		if(Syntax.IsWord(Last, Word::StmtEnd)) return;
		RewriteStep Step { RewriteKind::Insert, Id };
		Step.Idx = static_cast<uint32_t>(Node.Children.size());
		Step.Boundary = BoundaryKind::Glued;
		Step.Inserted = { Generation::GetGeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::Terminator) };
		Plan.Steps.push_back(std::move(Step));
		// 終了
		return;
	}

	/**
	 * 文字列の二重引用符への正規化の収集関数（整形仕様 5.2）
	 * 単引用符の文字列を，値を保つ二重引用符の文字列へ揃える
	 * Python の三重単引用符とドキュメンテーション文字列は三重二重引用符へ揃える
	 * 計算量：参照するノードと祖先の総数 N，テキストの総長 L に対し O(N + L)
	 * @param Syntax 構文モデル
	 * @param Id 文字列のノード（テキストを保つ葉）
	 * @param Plan 書換の計画の格納先
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectQuoteStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const {
		// 文字列の二重引用符への正規化の収集
		const SyntaxNode &Node = Syntax.GetNode(Id);
		const std::string Expanded = Node.Children.empty() ? std::string() : Syntax.GetText(Id);
		const std::string_view Text = Node.Children.empty() ? Node.Lexeme : Expanded;
		// 接頭辞の後の引用符
		const size_t QuotePos = std::min(Text.find_first_of("'\""), Text.size());
		const unsigned char Quote = QuotePos < Text.size() ? Text[QuotePos] : '\0';
		// 同じ引用符が３個並ぶ開始記号を持つ文字列か
		const bool IsTriple =
		Syntax.GetRules().HasTripleQuote && Text.size() > QuotePos + 5 && Text[QuotePos + 1] == Quote && Text[QuotePos + 2] == Quote;
		// 引用符で閉じないテキストの終了
		if(Text.size() < QuotePos + 2 || !Quote || Text.back() != Quote) return;
		const std::string_view Prefix = Text.substr(0, QuotePos);
		const size_t Delimiter = IsTriple ? 3 : 1;
		const std::string_view Body = Text.substr(QuotePos + Delimiter, Text.size() - QuotePos - (Delimiter << 1));
		// 関数・クラス・モジュールの本体の最初の文に為る，`f` 文字列・バイト列でない文字列（Python のドキュメンテーション文字列）か
		const NodeId Parent = Node.Parent;
		const bool IsDocstring = !IsTriple && ControlFlow::GetDocstring(Syntax, Id, DocstringValues) == Id;
		// 値を保つ二重引用符の本文の取得
		// 二重引用符のドキュメンテーション文字列の本文は其の儘使う
		// JSX の属性値はエスケープを持たない規則で再表現する
		const std::optional<std::string> Double =
		Quote == '"' ? IsDocstring ? std::optional<std::string>(Body) : std::nullopt : IsTriple ?
		TripleDoubleQuotedBody(Syntax.GetRules(), Body, Prefix) :
		DoubleQuotedBody(Syntax.GetRules(), Body, Prefix, Syntax.Is(Parent, Role::UnescapedAttribute));
		// 二重引用符の本文を持たないテキストの終了
		if(!Double) return;
		// 引用符を揃えたテキストとノードの組直
		const std::string_view Quotes = IsTriple || IsDocstring ? "\"\"\"" : "\"";
		if(!Node.Children.empty()) {
			// 補間の構造と内部の引用符を保ち，本文を再表現しない時だけ外側の区切を変更する
			if(*Double == Body) {
				Plan.Steps.push_back({ RewriteKind::SetLexeme, Node.Children.front(), std::string(Prefix).append(Quotes) });
				Plan.Steps.push_back({ RewriteKind::SetLexeme, Node.Children.back(), std::string(Quotes) });
			}
			// 補間を葉へ潰さず終了
			return;
		}
		const std::string Quoted = std::string(Prefix).append(Quotes).append(*Double).append(Quotes);
		if(Syntax.GetRules().DoubleQuotedDef.Def) {
			RewriteStep Str { RewriteKind::Reshape, Id };
			Str.Inserted = { {}, true, {}, Node.Fields, NoNode, Quoted, Syntax.GetRules().DoubleQuotedDef };
			Str.Inserted.Facts = Node.Facts;
			Plan.Steps.push_back(std::move(Str));
		} else Plan.Steps.push_back({ RewriteKind::SetLexeme, Id, Quoted });
		// 終了
		return;
	}

	/**
	 * 現在の役割インデックスからの規則対象の取得関数
	 * 通常側は役割インデックスを使い，基準側だけが全ノードを走査する
	 * 計算量：候補数 T，共有経路のノード数 P と基準側の走査量 R に対し期待値 O(R + T + P log(P + 1))
	 * @param Syntax 現在の構文
	 * @param Rule 対象の役割と所有関係
	 * @return 重複の無い構文順の対象
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 候補又は構文経路のメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	std::vector<NodeId> SyntaxNormalizer::GetRuleTargets(const SyntaxModel &Syntax, const RuleDescriptor &Rule) const {
		// 現在の役割インデックスからの規則対象の取得
		std::vector<NodeId> Found;
		#if defined(ShaveFormatRefFacts)
		const bool HasRootOwner = std::holds_alternative<RootCollectEntry>(Rule.Collect);
		std::vector<NodeId> Pending { Syntax.GetRoot() };
		while(!Pending.empty()) {
			const NodeId Node = Pending.back();
			Pending.pop_back();
			Syntax.Charge(1);
			for(const Role Wanted : Rule.TargetRoles) if(Syntax.Is(Node, Wanted)) {
				Found.push_back(HasRootOwner ? Syntax.GetRoot() : Node);
				break;
			}
			for(const Role Wanted : Rule.ChildRoles) if(Syntax.Is(Node, Wanted)) {
				if(const NodeId Parent = Syntax.GetParentOf(Node); HasRootOwner || Parent != NoNode) {
					Found.push_back(HasRootOwner ? Syntax.GetRoot() : Parent);
				}
				break;
			}
			const std::span<const NodeId> Children = Syntax.GetChildrenOf(Node);
			Pending.insert(Pending.end(), Children.rbegin(), Children.rend());
		}
		#else
		if(std::holds_alternative<RootCollectEntry>(Rule.Collect)) {
			// 根の候補は１つだけであり，役割が見付かった後の照会と整列を要しない
			// 根を所有する規則の対象の返戻
			for(const Role Wanted : Rule.TargetRoles) if(!Syntax.GetNodesWith(Wanted).empty()) return { Syntax.GetRoot() };
			// 子の役割から選ぶ根の返戻
			for(const Role Wanted : Rule.ChildRoles) if(!Syntax.GetNodesWith(Wanted).empty()) return { Syntax.GetRoot() };
		} else {
			for(const Role Wanted : Rule.TargetRoles) {
				const std::span<const NodeId> Indexed = Syntax.GetNodesWith(Wanted);
				Syntax.Charge(Indexed.size());
				Found.insert(Found.end(), Indexed.begin(), Indexed.end());
			}
			for(const Role Wanted : Rule.ChildRoles) for(const NodeId Child : Syntax.GetNodesWith(Wanted)) {
				Syntax.Charge(1);
				const NodeId Parent = Syntax.GetParentOf(Child);
				if(Parent == NoNode) continue;
				Found.push_back(Parent);
			}
		}
		#endif
		// 内側の最終形に依存する規則は，子を先に１度だけ評価する
		Found = SortInDocOrder(Syntax, Found);
		if(Rule.IsInnerFirst) std::reverse(Found.begin(), Found.end());
		// 規則の依存順の対象の返戻
		return Found;
	}

	/**
	 * 通常関数の末尾の返戻の明示の収集関数（整形仕様 5.2）
	 * 値を返さない通常関数の到達可能な末尾へ言語の返戻文を補う（ジェネレータ・式本体・戻らない関数は除く）
	 * 暗黙返戻は一時値・借用・属性・リザルトビルダーの影響を確かめた関数だけ明示する
	 * 計算量：到達可能性・返戻補完の判定費用 F，障壁取得費用 A と生成・計画費用 G に対し O(F + A + G)
	 * @param Syntax 構文モデル
	 * @param Id 現在の規則が評価する１ノード又は１所有者
	 * @param Plan 書換の計画の格納先
	 * @throws FormatLimitExceeded 必要な返戻文の生成形式が無い場合，又は構文の探索・事実の収集が作業予算を超えた場合
	 * @throws std::bad_alloc 計画又は本体の接続の控えのメモリを確保出来ない場合
	 * @throws std::length_error 計画又は接続の控えの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectExplicitReturnNode(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const {
		// エラー障壁の内側の場合の終了
		if(HasErrorNormalizationAncestor(Syntax, Id)) return;
		// 必要な対象だけで，書換迄の制御の流れの事実を生成する
		if(!Syntax.EditNormalizationFacts().FlowFacts) {
			Syntax.EditNormalizationFacts().FlowFacts = std::make_shared<ControlFlow>(Syntax);
		}
		const ControlFlow &Flow = *Syntax.EditNormalizationFacts().FlowFacts;
		// Rust の展開前の属性の範囲の判定の控え
		#if defined(ShaveFormatRefFacts)
		std::unordered_map<NodeId, bool> AttributeProtection;
		#else
		std::unordered_map<NodeId, bool> &AttributeProtection = Syntax.EditNormalizationFacts().ReturnAttributeProtection;
		#endif
		const std::optional<FunctionEnd> End = Flow.ClassifyFunction(Id, std::nullopt, true);
		// 通常関数でないノードと，末尾の返戻が既に明示された関数・戻らない関数・末尾へ到達しない関数の終了
		// 末尾式の変換も暗黙返戻の明示化も無い値返戻の関数では，補完の計画を作らない
		if(
			!End || End->Kind == ReturnKind::Never ||
			End->Kind == ReturnKind::Val && !Syntax.GetRules().IsTailExprVal && !Syntax.Is(Id, Role::ImplicitReturnCandidate) ||
			Flow.EndReach(Id, *End) != EndReachability::Reachable
		) return;
		const bool HasSingleImplicitResult = Flow.IsSingleImplicitReturnBody(Id, *End);
		const bool CanMakeExplicit = Flow.CanMakeImplicitReturnExplicit(Id, *End, HasSingleImplicitResult);
		// 同じ明示化の判断で補完の前提を確かめれない関数の終了
		if(Flow.IsReturnCompletionUncertain(Id, *End, HasSingleImplicitResult, CanMakeExplicit, AttributeProtection)) return;
		// 補完の計画で使う返戻の語
		const std::string ReturnWord(Syntax.GetRules().GetSpelling(Word::Return));
		if(CanMakeExplicit) {
			const CanonicalForm *const Form = GetGeneratedFormOf(Syntax, FormOperation::ArglessReturn);
			const FormPart *const Result = Form ? Form->GetPartOf(FormPartRole::Result) : nullptr;
			if(!Result || !Result->Def) throw FormatLimitExceeded("missing generated value return form", FailureReason::FormatterDefect);
			// 返戻の語と，文法の返戻の値のフィールドに置く単一式
			RewriteStep Wrap { RewriteKind::Wrap, End->Last };
			Wrap.Transfer.From = End->Last;
			Wrap.Inserted = {
				{},
				true,
				{ { ReturnWord }, { {}, false, {}, uint64_t{ 1 } << static_cast<uint8_t>(ChildRole::Result), End->Last } },
				0,
				NoNode,
				{},
				*Result
			};
			Wrap.Boundary = BoundaryKind::Spaced;
			Plan.Steps.push_back(std::move(Wrap));
			// 単一式の返戻のラッパーの終了
			return;
		}
		const std::string TerminatorLexeme =
		Generation::GetGeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::Terminator);
		// 文の並びの閉じの前（閉じを持たない並びは最後）への返戻文の追加
		const uint32_t Count = Syntax.GetChildCount(End->Stmts);
		const NodeId Closer = Syntax.GetChild(End->Stmts, Count - 1);
		RewriteStep Return { RewriteKind::Insert, End->Stmts };
		Return.Idx = Count && !Syntax.IsNamed(Closer) && Syntax.IsTokenRole(Closer, Role::ClosingBraceToken) ? Count - 1 : Count;
		Return.Boundary = BoundaryKind::Spaced;
		Return.Inserted = End->Kind == ReturnKind::Void ? ArglessReturn(Syntax) : GeneratedNode{};
		if(End->Kind == ReturnKind::Main) {
			const CanonicalForm *const Form = GetGeneratedFormOf(Syntax, FormOperation::ReturnWithZero);
			if(!Form) throw FormatLimitExceeded("missing generated return with zero form", FailureReason::FormatterDefect);
			Return.Inserted = GeneratedPart(Syntax, *Form, FormPartRole::Result, NoNode, true);
		}
		// Rust のブロックの値を与える末尾式（破棄の順を変え得る一時値を作る物は補完の前提を確かめれない物として上で除いた）
		const NodeId Val = Syntax.GetRules().IsTailExprVal ? Flow.GetTailExpr(End->Last) : NoNode;
		// 式の文で包まない末尾式か
		const bool IsBare = Val == End->Last;
		if(Val != NoNode && End->Kind == ReturnKind::Val) {
			// 値の末尾式の返戻の文への変換（ブロックで終わる式の文は `;` を加える）
			RewriteStep Wrap { RewriteKind::Wrap, Val };
			Wrap.Transfer.From = Val;
			GeneratedNode Returned {
				{},
				true,
				{ { ReturnWord }, { {}, false, {}, {}, Val } },
				0,
				NoNode,
				{},
				Syntax.GetRules().ReturnExprDef
			};
			if(IsBare) {
				Wrap.Inserted = { {}, true, { std::move(Returned), { TerminatorLexeme } }, 0, NoNode, {}, Syntax.GetRules().ExprStmtDef };
			} else Wrap.Inserted = std::move(Returned);
			Wrap.Boundary = BoundaryKind::Spaced;
			Plan.Steps.push_back(std::move(Wrap));
			if(!IsBare) {
				RewriteStep Terminator { RewriteKind::Insert, End->Last };
				Terminator.Idx = Syntax.GetChildCount(End->Last);
				Terminator.Inserted = { TerminatorLexeme };
				Plan.Steps.push_back(std::move(Terminator));
			}
		} else if(Val != NoNode && IsBare) {
			// 値を返さない関数の末尾式の文への変換
			RewriteStep Wrap { RewriteKind::Wrap, Val };
			Wrap.Transfer.From = Val;
			Wrap.Inserted =
			{ {}, true, { { {}, false, {}, {}, Val }, { TerminatorLexeme } }, 0, NoNode, {}, Syntax.GetRules().ExprStmtDef };
			Wrap.Boundary = BoundaryKind::Spaced;
			Plan.Steps.push_back(std::move(Wrap));
		}
		if(End->Kind == ReturnKind::Void || End->Kind == ReturnKind::Main) Plan.Steps.push_back(std::move(Return));
		// 終了
		return;
	}

	/**
	 * 末尾の値を返す関数の返戻を明示する関数
	 * 関数の末尾の通常経路と救済経路を調べ，最終節・別関数・通常のブロックへ降りない
	 * 計算量：末尾の各経路の直下のノード数 N と表記を確認する末尾式の総量 Q に対し O(N + Q)
	 * @param Syntax 構文モデル
	 * @param Id 現在の規則が評価する１ノード又は１所有者
	 * @param Plan 返戻で包む計画
	 * @throws FormatLimitExceeded 経路又は式の探索が作業予算を超える場合，又は必要な生成部品が無い場合
	 * @throws std::bad_alloc 経路又は生成部品のメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectTailReturnNode(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const {
		// エラー障壁の内側の場合の終了
		if(HasErrorNormalizationAncestor(Syntax, Id)) return;
		// 生成形式を持つ関数の値を決める経路の処理
		const NodeId Body = Syntax.GetFieldChild(Id, ChildRole::Body);
		// 文の並びの本体を持たない関数の終了
		if(!Syntax.Is(Body, Role::StmtSeq)) return;
		for(const CanonicalForm &Form : Syntax.GetNode(Id).Facts.Forms.GetView()) {
			if(Form.Operation != FormOperation::ExplicitTailReturn) continue;
			std::vector<NodeId> Bodies { Body };
			while(!Bodies.empty()) {
				const NodeId Sequence = Bodies.back();
				Bodies.pop_back();
				bool HasElse = false;
				for(const NodeId Child : Syntax.GetChildrenOf(Sequence)) {
					Syntax.Charge(1);
					if(Syntax.Is(Child, Role::ExceptionSection)) Bodies.push_back(Syntax.GetFieldChild(Child, ChildRole::Body));
					else if(Syntax.IsNamed(Child) && Syntax.Is(Child, Role::Else)) {
						Bodies.push_back(Child);
						HasElse = true;
					}
				}
				// 通常経路の値が `else` で決まる場合は其の本体へ進む
				if(HasElse) continue;
				// 返戻を補う末尾式の選択
				NodeId Last = NoNode;
				bool HasReturn = false;
				Core::SyntaxModel::ForEachStmtOf(
					Syntax,
					Sequence,
					[&](const NodeId Child) -> void {
						// 救済節と最終節を末尾の文としない事の終了
						if(Syntax.Is(Child, Role::ExceptionSection) || Syntax.Is(Child, Role::FinalSection)) return;
						HasReturn = HasReturn || Syntax.Is(Child, Role::ReturnKeyword);
						Last = Child;
					}
				);
				if(HasReturn) continue;
				if(Syntax.Is(Last, Role::ExplicitKeywordBody)) {
					Bodies.push_back(Last);
					continue;
				}
				// 値を返さない文の除外
				if(!Syntax.Is(Last, Role::ReturnVal)) continue;
				// 単一値の括弧を透過して，返戻の実引数の読みを決める構造の取得
				NodeId Val = Last;
				while(Syntax.Is(Val, Role::GroupingParen) && Syntax.GetChildCount(Val) == 3 && Syntax.GetNamedChildCount(Val) == 1) {
					Syntax.Charge(1);
					Val = Syntax.GetNamedChild(Val, 0);
				}
				NodeId Head = Last;
				while(Syntax.IsNamed(Syntax.GetChild(Head, 0))) {
					Syntax.Charge(1);
					Head = Syntax.GetChild(Head, 0);
				}
				const bool HasControlVal = IsControlValArg(Syntax, Val) || Head != Val && IsControlValArg(Syntax, Head);
				const bool HasLogicalWord = Syntax.Is(Val, Role::Binary) && Core::SyntaxModel::HasChildMatching(
					Syntax,
					Val,
					[&](const NodeId Child) -> bool {
						Syntax.Charge(1);
						// 語の低位論理演算子かの返戻
						return Syntax.IsWord(Child, Word::LogicalWord);
					}
				);
				// 大きな制御本体又はラムダを複写せず，先頭のトークンだけを読む
				const NodeId HeadLead = Syntax.GetFirstEdge(Head).Leaf;
				const std::string_view HeadText = Syntax.GetLexemeOf(HeadLead);
				// 必要な括弧を伴う返戻文の生成
				GeneratedNode Args = FormNode(Form, FormPartRole::Args);
				const std::string_view Plus = Syntax.GetRules().Operators.Plus;
				bool ShouldWrap = HeadText.starts_with(Plus) || Syntax.IsTokenRole(HeadLead, Role::OpeningParenToken) &&
				Syntax.GetLexemeOf(Syntax.GetFirstEdge(Syntax.GetChild(Head, 1)).Leaf).starts_with(Plus);
				if(HeadText.starts_with(Syntax.GetRules().Operators.Minus) && Syntax.Is(Head, Role::UnaryVal)) {
					NodeId Operand = Syntax.GetNamedChild(Head, 0);
					const bool IsPostfix = Syntax.Is(Operand, Role::ReceiverCall) || Syntax.Is(Operand, Role::ElementRef);
					while(Syntax.Is(Operand, Role::ReceiverCall) || Syntax.Is(Operand, Role::ElementRef)) {
						Syntax.Charge(1);
						Operand = Syntax.GetNamedChild(Operand, 0);
					}
					ShouldWrap = IsPostfix && Syntax.Is(Operand, Role::NumLiteral);
				}
				// 始端の無い範囲の括弧は実引数の区切で生成し，元のラッパーは後続の括弧正規化で外す
				ShouldWrap = ShouldWrap || Syntax.Is(Val, Role::Range) && !Syntax.IsNamed(Syntax.GetChild(Val, 0));
				if(
					Val == Last && (HasControlVal || HasLogicalWord) || ShouldWrapArg(Syntax, Head, false) ||
					Syntax.Is(Last, Role::AssignmentOperation) || Syntax.Is(Last, Role::CompoundAssignment)
				) {
					GeneratedNode Parens {
						{},
						true,
						{ FormNode(Form, FormPartRole::Open), { {}, false, {}, 0, Last }, FormNode(Form, FormPartRole::Close) },
						0,
						NoNode,
						{},
						Syntax.GetRules().ParenthesizedStmtsDef
					};
					Args.Children.push_back(std::move(Parens));
				} else {
					if(ShouldWrap) Args.Children.push_back(FormNode(Form, FormPartRole::Open));
					Args.Children.push_back({ {}, false, {}, 0, Last });
					if(ShouldWrap) Args.Children.push_back(FormNode(Form, FormPartRole::Close));
				}
				GeneratedNode Returned = FormNode(Form, FormPartRole::Result);
				Returned.Children.reserve(2);
				Returned.Children.push_back(FormNode(Form, FormPartRole::Keyword));
				Returned.Children.push_back(std::move(Args));
				// 制御式のグループを実引数の区切へ読み替えさせない境界の記録
				if(HasControlVal) Returned.Facts.Boundaries.Edit().push_back({ 1, BoundaryKind::RequiredSpace });
				RewriteStep Step { RewriteKind::Wrap, Last };
				Step.Transfer.From = Last;
				Step.Inserted = std::move(Returned);
				Plan.Steps.push_back(std::move(Step));
			}
		}
		// 終了
		return;
	}

	/**
	 * 値を使わない位置の後置の増減の前置化の一括収集関数（整形仕様 5.2）
	 * 括弧の要否を前置化した後の木で判ずる為に，他のノード毎の正規化より先の計画として集める（構文エラーと Rust のマクロのトークン列の中は書き換えない）
	 * 計算量：走査ノード数 N，宣言型の照会量 Q と名前の総テキスト長 B に対し O(N + Q + B)
	 * @param Syntax 構文モデル
	 * @param Plan 書換の計画の格納先
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 可視名又は走査列のメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectIncrementBatch(const SyntaxModel &Syntax, RewritePlan &Plan) const {
		// 有効範囲を離れる時の束縛の復元記録
		struct BindingUndo {
			std::string_view Name;
			std::optional<ScalarBinding> Prev; // 上書する前の可視束縛（無ければ無）
		};
		// 算術型の束縛の有効範囲の走査の状態
		struct Frame {
			NodeId Node;
			bool IsExit = false; // 此の有効範囲を離れて束縛を復元する時点か
			size_t Start = 0; // 範囲に入る前の束縛の復元記録の要素数
			size_t ScopeStart = 0; // 現在の関数の束縛の開始位置
		};
		std::unordered_map<std::string_view, ScalarBinding> Scalars;
		std::vector<BindingUndo> Undo;
		size_t ScopeStart = 0;
		// 現在可視な名前の束縛の登録
		const auto Bind = [&](const std::string_view Name, const bool IsBuiltin) -> void {
			Syntax.Charge(Name.size() + 1);
			const std::unordered_map<std::string_view, ScalarBinding>::const_iterator Prev = Scalars.find(Name);
			Undo.push_back({ Name, Prev == Scalars.end() ? std::nullopt : std::optional<ScalarBinding>(Prev->second) });
			Scalars.insert_or_assign(Name, ScalarBinding{ IsBuiltin, Undo.size() });
			// 終了
			return;
		};
		// 宣言の算術型の束縛の登録
		const auto AddDecl = [&](const NodeId Decl, const NodeId Selected) -> void {
			const NodeId Type = Syntax.GetFieldChild(Decl, ChildRole::Type);
			const bool IsArithmetic = !HasMacroBindingIn(Syntax, Type) && Syntax.IsBuiltinArithmeticType(Type);
			const std::span<const NodeId> Children =
			Selected == NoNode ? Syntax.GetChildrenOf(Decl) : std::span<const NodeId>(&Selected, 1);
			for(const NodeId Child : Children) {
				Syntax.Charge(1);
				if(!Syntax.HasField(Child, ChildRole::Declarator)) continue;
				NodeId Declarator = Child;
				bool IsPtr = false, IsAggregate = false;
				while(Declarator != NoNode && !Syntax.Is(Declarator, Role::Identifier)) {
					Syntax.Charge(1);
					if(Syntax.Is(Declarator, Role::StructuredBindingDeclarator)) {
						for(const NodeId Name : Syntax.GetChildrenOf(Declarator)) {
							Syntax.Charge(1);
							if(Syntax.Is(Name, Role::Identifier)) Bind(Syntax.GetNode(Name).Lexeme, false);
						}
						Declarator = NoNode;
						break;
					}
					IsPtr |= Syntax.Is(Declarator, Role::PtrDeclarator);
					IsAggregate |= Syntax.Is(Declarator, Role::ArrayDeclarator) || Syntax.Is(Declarator, Role::FunctionDeclarator);
					const NodeId Inner = Syntax.GetFieldChild(Declarator, ChildRole::Declarator);
					Declarator = Inner == NoNode ? Syntax.GetNamedChild(Declarator, 0) : Inner;
				}
				if(Declarator == NoNode) continue;
				Bind(
					Syntax.GetNode(Declarator).Lexeme,
					!IsAggregate && !HasMacroBindingIn(Syntax, Type) && !HasMacroBindingIn(Syntax, Child) && (IsPtr || IsArithmetic)
				);
			}
			// 終了
			return;
		};
		std::vector<Frame> Pending { { Syntax.GetRoot() } };
		while(!Pending.empty()) {
			const Frame Cur = Pending.back();
			Pending.pop_back();
			Syntax.Charge(1);
			if(Cur.IsExit) {
				while(Undo.size() > Cur.Start) {
					const BindingUndo &Prev = Undo.back();
					Syntax.Charge(Prev.Name.size() + 1);
					if(Prev.Prev) Scalars.insert_or_assign(Prev.Name, *Prev.Prev);
					else Scalars.erase(Prev.Name);
					Undo.pop_back();
				}
				ScopeStart = Cur.ScopeStart;
				continue;
			}
			const NodeId Node = Cur.Node;
			if(HasProtectedNormalizationAncestor(Syntax, Node)) continue;
			const bool IsFunction = Syntax.Is(Node, Role::FunctionScope) || Syntax.Is(Node, Role::FunctionDef);
			if(
				const bool IsBoundary = IsFunction || Syntax.Is(Node, Role::ClassBody);
				IsBoundary || Syntax.Is(Node, Role::CompoundBody) || Syntax.Is(Node, Role::CountedLoop) ||
				Syntax.Is(Node, Role::ForRangeLoop) || Syntax.Is(Node, Role::Conditional) || Syntax.Is(Node, Role::WhileLoop) ||
				Syntax.Is(Node, Role::CatchClause)
			) {
				Pending.push_back({ Node, true, Undo.size(), ScopeStart });
				if(IsBoundary) ScopeStart = Undo.size() + 1;
			}
			// 各宣言子の初期化子へ入る前の導入名の登録
			// 範囲反復の導入名は反復対象の評価後，本体へ入る時に登録
			if(
				const NodeId Parent = Syntax.GetParentOf(Node);
				Syntax.HasField(Node, ChildRole::Declarator) && (Syntax.Is(Parent, Role::TypedDecl) || Syntax.Is(Parent, Role::ParamDecl))
			) AddDecl(Parent, Node);
			else if(Syntax.HasField(Node, ChildRole::Body) && Syntax.Is(Parent, Role::ForRangeLoop)) AddDecl(Parent, NoNode);
			if(Syntax.Is(Node, Role::PrefixableIncrement)) CollectIncrementStep(Syntax, Node, Scalars, ScopeStart, Plan);
			const std::span<const NodeId> Children = Syntax.GetChildrenOf(Node);
			for(size_t Idx = Children.size(); Idx--;) Pending.push_back({ Children[Idx] });
		}
		// 終了
		return;
	}

	/**
	 * 無条件の反復の正規化の収集関数（整形仕様 5.2）
	 * 節の無い `for(;;)` を `while(true)` へ揃える（C で真偽値の名前が見えない位置は `while(1)` へ揃える）
	 * 真偽値の名前が見える条件では，真と確定した整数値を真の名前へ揃える
	 * Go では真の条件又は空の節を省き，Rust では真の条件の `while` を `loop` へ揃える
	 * 除くトークンのアンカーは繰返の前へ移す
	 * 計算量：参照するノードと祖先の総数 N，テキストの総長 L に対し O(N + L)
	 * @param Syntax 構文モデル
	 * @param Id ノード
	 * @param Plan 書換の計画の格納先
	 * @throws FormatLimitExceeded 作業予算を超える場合，又は必要な生成部品が無い場合
	 * @throws std::bad_alloc 障壁・生成ノード又は計画のメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード又は祖先の番号がノード表に無い場合
	 */
	void SyntaxNormalizer::CollectLoopStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const {
		// 保護された祖先の内側の場合の終了
		if(HasProtectedNormalizationAncestor(Syntax, Id)) return;
		const NodeId Body = Syntax.GetFieldChild(Id, ChildRole::Body);
		const AnchorTarget Head { Id, false };
		// 真の値の名前
		const std::string True(Syntax.GetRules().GetSpelling(Word::True));
		const bool HasTrueName = Syntax.GetNode(Id).Facts.HasTrueName;
		const bool HasParenthesizedCond = Syntax.Is(Id, Role::ParenthesizedLoopCond);
		NodeId TruthCond = Syntax.GetFieldChild(Id, ChildRole::Cond);
		// 条件フィールドの構文上の括弧と，式の省略可能な括弧の順次透過
		if(
			HasParenthesizedCond && Syntax.IsTokenRole(Syntax.GetChild(TruthCond, 0), Role::OpeningParenToken) &&
			Syntax.GetNamedChildCount(TruthCond) == 1
		) TruthCond = Syntax.GetNamedChild(TruthCond, 0);
		while(Syntax.Is(TruthCond, Role::GroupingParen)) TruthCond = Syntax.GetNamedChild(TruthCond, 0);
		if(
			TruthCond != NoNode && Syntax.GetNode(TruthCond).Facts.HasTrueName && !Syntax.Is(Id, Role::ClauseOrCondLoop) &&
			!Syntax.Is(Id, Role::UnconditionalLoopKeyword) &&
			(Syntax.Is(Id, Role::WhileLoop) || Syntax.Is(Id, Role::PostCondLoop) || Syntax.Is(Id, Role::CountedLoop)) &&
			(Syntax.Is(TruthCond, Role::IntVal) || Syntax.Is(TruthCond, Role::NumericToken)) &&
			(Syntax.IsWord(TruthCond, Word::IntTruth) || Syntax.GetRules().IsEveryIntCondTrue && Syntax.Is(TruthCond, Role::IntVal))
		) {
			RewriteStep Literal { RewriteKind::Reshape, TruthCond };
			Literal.Inserted = { {}, true, {}, 0, NoNode, True, Syntax.GetRules().TrueDef };
			Plan.Steps.push_back(std::move(Literal));
		}
		if(Syntax.Is(Id, Role::UnconditionalLoopKeyword)) {
			const NodeId Cond = Syntax.GetFieldChild(Id, ChildRole::Cond);
			NodeId LocalCond = Cond;
			while(Syntax.Is(LocalCond, Role::GroupingParen)) LocalCond = Syntax.GetNamedChild(LocalCond, 0);
			// 真の条件の `while` 以外の終了
			if(
				!Syntax.Is(Id, Role::WhileExpr) || !Syntax.Is(LocalCond, Role::BoolLiteral) || !Syntax.IsWord(LocalCond, Word::True) ||
				Body == NoNode
			) return;
			// ラベルと本体を保った `loop` への組直
			RewriteStep Loop { RewriteKind::Reshape, Id };
			Loop.Inserted = { {}, true, {}, 0, NoNode, {}, Syntax.GetRules().LoopExprDef };
			for(const NodeId Child : Syntax.GetChildrenOf(Id)) {
				if(Child == Cond || Syntax.Is(Child, Role::KeywordWhile)) continue;
				if(Child == Body) Loop.Inserted.Children.push_back({ std::string(Syntax.GetRules().GetSpelling(Word::Loop)) });
				Loop.Inserted.Children.push_back({ {}, false, {}, Syntax.GetNode(Child).Fields, Child });
			}
			Loop.Transfer = { NoNode, Head, Head, Head };
			Plan.Steps.push_back(std::move(Loop));
			// 無条件の反復を `loop` へ組み直した場合の終了
			return;
		}
		// `for` 以外の終了
		if(!Syntax.Is(Id, Role::CountedLoop)) return;
		if(HasParenthesizedCond) {
			// 初期化・条件・更新の節を持つ `for`（空文の節は節を持たない）の終了
			if(
				Body == NoNode || Core::SyntaxModel::HasChildMatching(
					Syntax,
					Id,
					[&Syntax, Body](const NodeId Child) -> bool {
						// 本体・空文以外の名前付の子かの返戻
						return Child != Body && Syntax.IsNamed(Child) && !Syntax.Is(Child, Role::EmptyStmt);
					}
				)
			) return;
			RewriteStep Loop { RewriteKind::Reshape, Id };
			const CanonicalForm *const Form = GetGeneratedFormOf(Syntax, FormOperation::UnconditionalLoop);
			// 無条件の繰返の生成形式を持たない文法の場合の終了
			if(!Form) return;
			Loop.Inserted = GeneratedPart(Syntax, *Form, FormPartRole::Result, Body, HasTrueName);
			// `for` と `while` は同じ単一文の本体を持ち，ラッパーの生成形式を引き継ぐ
			Loop.Inserted.Facts.Forms = Syntax.GetNode(Id).Facts.Forms;
			Loop.Transfer = { NoNode, Head, Head, Head };
			Plan.Steps.push_back(std::move(Loop));
			// 節を持たない `for` を無条件の反復へ組み直した場合の終了
			return;
		}
		// Go 以外の終了
		if(!Syntax.Is(Id, Role::ClauseOrCondLoop)) return;
		NodeId LocalCond = Syntax.GetNamedChild(Id, 0);
		while(Syntax.Is(LocalCond, Role::GroupingParen)) LocalCond = Syntax.GetNamedChild(LocalCond, 0);
		// 節も条件も持たない `for` と，空の節又は真の条件以外の終了
		if(
			LocalCond == Body ||
			!(Syntax.Is(LocalCond, Role::ForClause) && !Syntax.GetNamedChildCount(LocalCond)) && !Syntax.Is(LocalCond, Role::TrueVal)
		) return;
		const NodeId Cond = Syntax.GetNamedChild(Id, 0);
		const AnchorTarget Keyword { Syntax.GetPrevSibling(Cond), true };
		Plan.Steps.push_back({ RewriteKind::Remove, Cond, {}, { Cond, Keyword, Keyword, Keyword } });
		// 終了
		return;
	}

	/**
	 * 後置条件の一括収集関数
	 * 計算量：対象選択量 S，共有インデックスの構築量 I と個別判定の総量 Q に対し O(S + I + Q)
	 * @param Syntax 編集前の同じ構文
	 * @param Plan 規則順位・構文順・元の操作順を保つ計画
	 * @throws FormatLimitExceeded 取得・共通前提又は作業予算の確認が失敗した場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectStmtBatch(const SyntaxModel &Syntax, RewritePlan &Plan) const {
		// 後置条件の一括収集
		const NodeId Root = Syntax.GetRoot();
		std::unordered_set<NodeId> Selected;
		std::unordered_map<NodeId, NodeId> Scopes;
		for(const NodeId Target : GetCandidates(Syntax, { Role::KeywordBlock })) {
			std::vector<NodeId> Path;
			NodeId Up = Syntax.GetParentOf(Target);
			while(Up != NoNode && !Scopes.contains(Up) && !Syntax.Is(Up, Role::TailValFunction)) {
				Syntax.Charge(1);
				Path.push_back(Up);
				Up = Syntax.GetParentOf(Up);
			}
			const NodeId Scope = Up == NoNode ? Root : Scopes.contains(Up) ? Scopes.at(Up) : Up;
			for(const NodeId Ancestor : Path) Scopes.emplace(Ancestor, Scope);
			if(!Selected.insert(Scope).second) continue;
			BuildStmtBindingIdx(Syntax, Scope, &Plan);
		}
		// 終了
		return;
	}

	/**
	 * 文・メンバの終端と型本体の区切の収集関数
	 * 計算量：子の数 C，境界の数 B，障壁取得・生成・境界移譲の総費用 Q に対し O(C + B + Q)
	 * @param Syntax 構文モデル
	 * @param Id 現在の規則が評価する１ノード又は１所有者
	 * @param Plan 書換の計画の格納先
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectTerminatorNode(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const {
		// 保護された祖先の内側の場合の終了
		if(HasProtectedNormalizationAncestor(Syntax, Id)) return;
		const std::string Terminator = Generation::GetGeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::Terminator);
		const std::string Separator = Generation::GetGeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::Separator);
		// 区切の挿入と置換の共通操作
		const std::span<const NodeId> Children = Syntax.GetChildrenOf(Id);
		std::vector<std::string> Insertions;
		NodeId Removed = NoNode;
		// トークンの挿入の計画の追加
		const auto Insert = [&](const uint32_t Idx, const std::string &Token) -> void {
			if(Insertions.empty()) Insertions.resize(Children.size() + 1);
			Insertions[Idx] = Token;
			// 終了
			return;
		};
		// トークンの置換の計画の追加
		const auto Replace = [&](const NodeId Token, const std::string &Text) -> void {
			RewriteStep Step { RewriteKind::Reshape, Token };
			Step.Inserted = { Text };
			Plan.Steps.push_back(std::move(Step));
			// 終了
			return;
		};
		// 文の終端を欠く文への区切の追加
		if(
			Syntax.Is(Id, Role::TerminatedStmt) && !Syntax.GetNode(Id).Facts.Has(ConstraintKind::EndsCode) && !Children.empty() &&
			!Syntax.IsWord(Children.back(), Word::StmtEnd)
		) {
			const NodeId Inner = Syntax.GetLastNamedChild(Id);
			if(
				const bool IsCompleteExport = Syntax.Is(Id, Role::Export) && (
					Syntax.Is(Inner, Role::TerminatedStmt) || Syntax.IsTokenRole(Syntax.GetLastEdge(Id).Leaf, Role::ClosingBraceToken) && (
						Syntax.Is(Inner, Role::Decl) || Syntax.Is(Inner, Role::AmbientDecl) ||
						Syntax.GetFieldChild(Id, ChildRole::Val) != NoNode && (Syntax.Is(Inner, Role::FunctionExpr) || Syntax.Is(Inner, Role::Class))
					)
				);
				!IsCompleteExport && !Syntax.Is(Inner, Role::Namespace) && !Syntax.GetNode(Id).Facts.Has(ConstraintKind::ContainsError)
			) Insert(static_cast<uint32_t>(Children.size()), Terminator);
		}
		// 型本体とメンバ本体の区切の調整
		if(Syntax.Is(Id, Role::MemberBody) || Syntax.Is(Id, Role::TypeMemberBody)) {
			const bool IsType = Syntax.Is(Id, Role::TypeMemberBody) && !Syntax.Is(Id, Role::MemberBody);
			const std::span<const SemanticBoundary> Boundaries = Syntax.GetNode(Id).Facts.Boundaries.GetView();
			size_t BoundaryIdx = Boundaries.size();
			bool IsProtectedAfter = false;
			for(uint32_t Idx = static_cast<uint32_t>(Children.size()); Idx--;) {
				Syntax.Charge(1);
				const NodeId Child = Children[Idx], Next = Idx + 1 < Children.size() ? Children[Idx + 1] : NoNode;
				const bool HasError = Syntax.Is(Child, Role::Error) || Syntax.Is(Next, Role::Error);
				// 子と境界を逆順に辿り，子の前の保持境界を次の子の後の判定へ渡す
				bool IsProtected = IsProtectedAfter;
				IsProtectedAfter = false;
				while(BoundaryIdx) {
					Syntax.Charge(1);
					const SemanticBoundary &Boundary = Boundaries[BoundaryIdx - 1];
					if(Boundary.Before < Idx) break;
					--BoundaryIdx;
					IsProtected |= Boundary.Kind == BoundaryKind::Preserved && (Boundary.Before == Idx || Boundary.Before == Idx + 1);
					IsProtectedAfter |= Boundary.Kind == BoundaryKind::Preserved && Boundary.Before == Idx;
				}
				if(HasError || IsProtected) continue;
				if(IsType) {
					if(Syntax.IsWord(Child, Word::StmtEnd) || Syntax.IsTokenRole(Child, Role::CommaToken)) {
						if(!Idx || !Syntax.Is(Children[Idx - 1], Role::TypeMember)) continue;
						if(Syntax.IsTokenRole(Next, Role::ClosingBraceToken)) {
							const AnchorTarget Prev { Children[Idx - 1], true };
							Removed = Child;
							Plan.Steps.push_back({ RewriteKind::Remove, Child, {}, { Child, Prev, Prev, Prev } });
						} else if(Syntax.Is(Next, Role::TypeMember) && Syntax.IsWord(Child, Word::StmtEnd)) Replace(Child, Separator);
					} else if(Syntax.Is(Child, Role::TypeMember) && Syntax.Is(Next, Role::TypeMember)) Insert(Idx + 1, Separator);
				} else if(Syntax.Is(Child, Role::TerminatedMember)) {
					if(Syntax.IsTokenRole(Next, Role::CommaToken)) Replace(Next, Terminator);
					else if(!Syntax.IsWord(Next, Word::StmtEnd)) Insert(Idx + 1, Terminator);
				}
			}
		}
		if(!Insertions.empty()) {
			RewriteStep Combined { RewriteKind::RebuildChildren, Id };
			for(size_t Idx = 0; Idx < Insertions.size(); ++Idx) {
				Syntax.Charge(1);
				if(!Insertions[Idx].empty()) Combined.Inserted.Children.push_back({ Insertions[Idx] });
				if(Idx < Children.size() && Children[Idx] != Removed) {
					const NodeId Child = Children[Idx];
					Combined.Inserted.Children.push_back({ {}, false, {}, Syntax.GetNode(Child).Fields, Child });
				}
			}
			Combined.Inserted.Src = Id;
			Syntax.RemapGeneratedBoundaries(Combined.Inserted);
			Plan.Steps.push_back(std::move(Combined));
		}
		// 終了
		return;
	}

	/**
	 * 単なる区切の末尾カンマの除去の収集関数（整形仕様 5.2）
	 * 括弧の無い並びの末尾と，閉じの直前のカンマは要素を区切らない為に除く
	 * Java の列挙では宣言部の前のカンマも除き，除いたカンマのアンカーは前の要素の後へ移す
	 * 単一要素の組のカンマと配列の空要素のカンマ，CSS の空の代替値のカンマは値を表す為に保つ（Go の折返に要るカンマは中間表現が出す）
	 * 計算量：参照するノードと祖先の総数 N，テキストの総長 L に対し O(N + L)
	 * @param Syntax 構文モデル
	 * @param Id 現在の規則が評価する１ノード
	 * @param Plan 書換の計画の格納先
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectTrailingCommaNode(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const {
		// エラー・保護障壁の内側と，空の代替値を持ち得る並びを除き，リスト・仮引数・include の区切を扱う
		if(
			GetNormalizationBarrierOf(Syntax, Id).IsProtected ||
			Syntax.GetRules().HasTrailingCommaEmptyVal && !Syntax.IsAny(Id, { Role::MixinParams, Role::TupleLike }) &&
			!Syntax.Is(Syntax.GetParentOf(Id), Role::IncludeStmtNode)
		) return;
		const std::span<const NodeId> Children = Syntax.GetChildrenOf(Id);
		// 組の型を決める唯一のカンマか
		uint32_t TupleCommas = 0;
		if(
			Syntax.Is(Id, Role::TupleLike) || Syntax.Is(Id, Role::TupleTypeComma) && (
				Syntax.Is(Id, Role::Subscript) ||
				Syntax.Is(Id, Role::GenericParam) && Syntax.Is(Syntax.GetParentOf(Id), Role::AppliedGenericType) &&
				!Syntax.HasField(Syntax.GetParentOf(Syntax.GetParentOf(Id)), ChildRole::Left)
			)
		) for(const NodeId Child : Children) TupleCommas += Syntax.IsTokenRole(Child, Role::CommaToken);
		// 括弧の無い並びの末尾のカンマの除去
		if(
			Syntax.Is(Id, Role::BareListTrailingComma) && !Children.empty() && Syntax.IsTokenRole(Children.back(), Role::CommaToken) && (
				Syntax.Is(Id, Role::ExprList) || Syntax.Is(Id, Role::PatternList) || Syntax.Is(Id, Role::LambdaParams) ||
				Syntax.Is(Id, Role::TupleExpr)
			) && (Syntax.GetNamedChildCount(Id) > 1 || Syntax.Is(Id, Role::LambdaParams))
		) {
			const NodeId Comma = Children.back();
			const AnchorTarget Target { Children[Children.size() - 2], true };
			Plan.Steps.push_back({ RewriteKind::Remove, Comma, {}, { Comma, Target, Target, Target } });
		}
		// 閉じの前の区切だけのカンマの除去（単一要素の組と配列の空要素を表すカンマは保持）
		for(uint32_t Idx = 1; Idx + 1 < Children.size(); ++Idx) {
			const NodeId Comma = Children[Idx], Next = Children[Idx + 1];
			if(Syntax.IsNamed(Comma) || !Syntax.IsTokenRole(Comma, Role::CommaToken)) continue;
			const NodeId NextLead = Syntax.GetFirstEdge(Next).Leaf;
			if(
				const bool IsClosing = !Syntax.IsNamed(Next) && Syntax.IsTokenRole(NextLead, Role::ClosingBracketLead);
				!IsClosing && !Syntax.Is(Next, Role::EnumBodyDecls) || TupleCommas == 1 && (
					!Syntax.GetRules().DoesSingletonCommaDetermineTuple || Syntax.GetNamedChildCount(Id) == (Syntax.Is(Id, Role::Subscript) ? 2 : 1)
				) || Syntax.IsTokenRole(NextLead, Role::ClosingSquareLead) &&
				(Syntax.IsTokenRole(Children[Idx - 1], Role::CommaToken) || Syntax.IsTokenRole(Children[Idx - 1], Role::OpeningBracketToken))
			) continue;
			const AnchorTarget Prev { Children[Idx - 1], true };
			Plan.Steps.push_back({ RewriteKind::Remove, Comma, {}, { Comma, Prev, Prev, Prev } });
		}
		// 終了
		return;
	}

	/**
	 * 省けない反復末尾の区切１ノードの収集関数
	 * 対象の役割とエラー障壁を此の収集関数で確認する
	 * 計算量：対象の子の数 C と障壁取得の費用 A に対し O(C + A)，通常側で変更の無い障壁を共有する照会は期待値 O(1)
	 * @param Syntax 構文モデル
	 * @param Id 役割インデックスから選んだ反復の対象
	 * @param Plan 現在の収集の書換計画
	 * @throws FormatLimitExceeded 作業予算を超えた場合，又は固定生成形式が不足した場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectLoopTerminatorStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const {
		// 対象外又はエラー障壁が在る場合の終了
		if(HasErrorNormalizationAncestor(Syntax, Id)) return;
		const bool IsEmptyLoop =
		(Syntax.Is(Id, Role::CountedLoop) || Syntax.Is(Id, Role::ForeachStmt)) && Syntax.GetFieldChild(Id, ChildRole::Body) == NoNode;
		bool HasAlternativeEnd = false;
		for(const NodeId Child : Syntax.GetChildrenOf(Id)) {
			Syntax.Charge(1);
			HasAlternativeEnd = HasAlternativeEnd || Syntax.Is(Child, Role::AlternativeBodyCloser);
		}
		if((IsEmptyLoop || HasAlternativeEnd) && !Syntax.IsWord(Syntax.GetChild(Id, Syntax.GetChildCount(Id) - 1), Word::StmtEnd)) {
			RewriteStep End { RewriteKind::Insert, Id };
			End.Dst = Id;
			End.Idx = Syntax.GetChildCount(Id);
			End.Inserted = { Generation::GetGeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::Terminator) };
			Plan.Steps.push_back(std::move(End));
		}
		// 終了
		return;
	}

	/**
	 * 独立した数値トークンの正規化の一括収集関数
	 * 計算量：候補選択費用 Q，数値の総トークン長 B と障壁取得費用 A に対し O(Q + B + A)
	 * @param Syntax 現在の構文
	 * @param Plan 独立した数値の書換の格納先
	 * @throws FormatLimitExceeded 候補選択又はトークンの確認が作業予算を超えた場合
	 * @throws std::bad_alloc 候補・障壁・書換のメモリを確保出来ない場合
	 * @throws std::length_error 候補・障壁・書換の要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectNumBatch(const SyntaxModel &Syntax, RewritePlan &Plan) const {
		// 数値は互いのトークンを読まず，書換で接続・役割・障壁を変えない
		for(const NodeId Num : GetCandidates(Syntax, { Role::NormalizedNum })) CollectNumStep(Syntax, Num, Plan);
		// 終了
		return;
	}

	/**
	 * スタイルのトークン・区切の正規化の収集関数
	 * 計算量：トークン長 B，子数 C と障壁取得量 A に対し O(B + C + A)
	 * @param Syntax 現在の構文
	 * @param Id 選別済のスタイルの対象
	 * @param Plan 書換の格納先
	 * @throws FormatLimitExceeded 作業予算を超えるか生成形式が不足する場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectStyleNodeStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const {
		// 保護障壁の内側の場合の終了
		if(HasProtectedNormalizationAncestor(Syntax, Id)) return;
		CollectCssStep(Syntax, Id, Plan);
		// 終了
		return;
	}

	/**
	 * 引用文字列１ノードの収集関数
	 * 計算量：トークンの長さ B と障壁取得の費用 A に対し O(B + A)，通常側で変更の無い障壁を共有する照会は期待値 O(1)
	 * @param Syntax 現在の収集が読む構文
	 * @param Id 対象
	 * @param Plan 現在の計画
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectQuotedNodeStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const {
		// 対象外又は保護祖先の場合の終了
		if(
			Syntax.GetNode(Id).Facts.DoesPreserveQuoteForm ||
			!Syntax.GetNode(Id).Children.empty() && !Syntax.Is(Id, Role::VerbatimQuotedStr) || HasProtectedNormalizationAncestor(Syntax, Id)
		) return;
		Syntax.Charge(Syntax.GetNode(Id).Lexeme.size());
		CollectQuoteStep(Syntax, Id, Plan);
		// 終了
		return;
	}

	/**
	 * 文末のトークンの読違えを防ぐラッパーと引数の調整関数（整形仕様 5.2）
	 * 計算量：対象の探索・判定・計画の総量 Q に対し O(Q)
	 * @param Syntax 現在の構文
	 * @param Id 選別済の対象
	 * @param Plan 書換の格納先
	 * @throws FormatLimitExceeded 作業予算又は生成形式の確認が失敗する場合
	 * @throws std::bad_alloc 判定又は計画のメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectBareNotationNode(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const {
		// 保護された祖先の内側の場合の終了
		if(HasProtectedNormalizationAncestor(Syntax, Id)) return;
		const NodeId Parent = Syntax.GetParentOf(Id);
		// 文末の読みを使う候補だけ，既存の祖先の共有表から所属を取得する
		// 計算量：未取得の祖先数 H に対し O(H)，同じ所属の照会は O(1)
		// @return 同じ本体に後続の文が在る場合 `true`
		// @throws FormatLimitExceeded 祖先の探索又は共有表の参照が作業予算を超える場合
		// @throws std::bad_alloc 祖先の共有表又は作業列を確保出来ない場合
		// @throws std::length_error 祖先の共有表又は作業列の要素数上限を超える場合
		// @throws std::out_of_range 祖先のノードが表に無い場合
		const auto HasFollowing = [&]() -> bool {
			const NodeId Stmt = GetParenAncestorOf(Syntax, Id, ParenAncestorKind::BareStatement).Child;
			// 共有した文の後続の有無の返戻
			return Syntax.GetNextNamedSibling(Stmt) != NoNode;
		};
		if(
			Syntax.Is(Id, Role::Range) && !Syntax.IsNamed(Syntax.GetChild(Id, Syntax.GetChildCount(Id) - 1)) &&
			(Syntax.Is(Parent, Role::AssignmentExpr) && HasFollowing() || Syntax.Is(Parent, Role::PatternMembership))
		) {
			RewriteStep Wrap { RewriteKind::Wrap, Id };
			Wrap.Inserted = {
				{},
				true,
				{
					{ Generation::GetGeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallOpen) },
					{ {}, false, {}, 0, Id },
					{ Generation::GetGeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallClose) }
				},
				0,
				NoNode,
				{},
				Syntax.GetRules().ParenthesizedStmtsDef
			};
			// 包んだ範囲の外のアンカーの括弧の外への移動
			Wrap.Transfer.From = Id;
			Plan.Steps.push_back(std::move(Wrap));
		}
		if(Syntax.Is(Id, Role::ArgSeq) && !Syntax.IsTokenRole(Syntax.GetChild(Id, 0), Role::OpeningParenToken)) if(
			const NodeId Last = Syntax.GetNamedChild(Id, Syntax.GetNamedChildCount(Id) - 1);
			HasFollowing() && (
				Syntax.IsWord(Last, Word::BareCapture) || Syntax.GetText(Last).ends_with(
					Generation::GetGeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::ValDelimiter)
				)
			)
		) {
			RewriteStep Close { RewriteKind::Insert, Id };
			Close.Idx = Syntax.GetChildCount(Id);
			Close.Inserted = { Generation::GetGeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallClose) };
			Plan.Steps.push_back(std::move(Close));
			RewriteStep Open { RewriteKind::Insert, Id };
			Open.Idx = 0;
			Open.Inserted = { Generation::GetGeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallOpen) };
			Plan.Steps.push_back(std::move(Open));
		}
		const bool IsParams = Syntax.Is(Id, Role::MethodParams) || Syntax.Is(Id, Role::LambdaParams);
		// 受手を持つ呼出かの判定
		if(
			const auto IsReceiverCall = [&Syntax, Parent]() -> bool {
				// 受手の呼出でない事の返戻
				if(!Syntax.Is(Parent, Role::ReceiverCall) || Syntax.GetFieldChild(Parent, ChildRole::Object) == NoNode) return false;
				const NodeId Function = Syntax.GetFieldChild(Parent, ChildRole::Function);
				// 被呼出側が定数名でない事の返戻
				return Function != NoNode && !Syntax.Is(Function, Role::ConstantName);
			}; !Syntax.GetNamedChildCount(Id) &&
			(IsParams || Syntax.Is(Id, Role::ArgSeq) && (Syntax.Is(Parent, Role::YieldVal) || IsReceiverCall()))
		) {
			const AnchorTarget Target { Parent, false };
			Plan.Steps.push_back({ RewriteKind::Remove, Id, {}, { Id, Target, Target, Target } });
		} else if(IsParams) {
			const bool HasParens = Syntax.IsTokenRole(Syntax.GetChild(Id, 0), Role::OpeningParenToken);
			bool IsSimple = true, HasBareCapture = false;
			for(const NodeId Child : Syntax.GetChildrenOf(Id)) if(Syntax.IsNamed(Child)) {
				IsSimple = IsSimple && Syntax.Is(Child, Role::Identifier);
				HasBareCapture = HasBareCapture || Syntax.IsWord(Child, Word::BareCapture);
			}
			if(Syntax.Is(Id, Role::LambdaParams) && HasParens && IsSimple) {
				const NodeId Open = Syntax.GetChild(Id, 0);
				const NodeId Close = Syntax.GetChild(Id, Syntax.GetChildCount(Id) - 1);
				const AnchorTarget Before { Syntax.GetFirstEdge(Syntax.GetNamedChild(Id, 0)).Leaf, false };
				const AnchorTarget After { Syntax.GetLastEdge(Syntax.GetLastNamedChild(Id)).Leaf, true };
				Plan.Steps.push_back({ RewriteKind::Remove, Open, {}, { Open, Before, Before, Before } });
				Plan.Steps.push_back({ RewriteKind::Remove, Close, {}, { Close, After, After, After } });
			} else if(Syntax.Is(Id, Role::MethodParams) && !HasParens && HasBareCapture) {
				RewriteStep Close { RewriteKind::Insert, Id };
				Close.Idx = Syntax.GetChildCount(Id);
				Close.Inserted = { Generation::GetGeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallClose) };
				Plan.Steps.push_back(std::move(Close));
				RewriteStep Open { RewriteKind::Insert, Id };
				Open.Idx = 0;
				Open.Inserted = { Generation::GetGeneratedFixed(Syntax, FormOperation::GeneratedLexemes, FormPartRole::CallOpen) };
				Plan.Steps.push_back(std::move(Open));
			}
		}
		// 終了
		return;
	}

	/**
	 * 所属を保つ裸の引数の書換の一括収集関数
	 * 空の実引数・仮引数とラッパーの編集は，文の並び・語の本体・後続の文を変えない
	 * 候補の採否は所属と自身の実引数から決まり，別の候補の編集を読む必要が無い
	 * 計算量：候補選択費用 Q，候補の子数 C と共有する祖先数 A に対し O(Q + C + A)
	 * @param Syntax 現在の構文
	 * @param Plan 所属を保つ書換の格納先
	 * @throws FormatLimitExceeded 候補・祖先の探索又は計画の生成が作業予算を超える場合
	 * @throws std::bad_alloc 候補・祖先・計画のメモリを確保出来ない場合
	 * @throws std::length_error 候補・祖先・計画の要素数上限を超える場合
	 * @throws std::out_of_range 候補又は祖先のノードが表に無い場合
	 */
	void SyntaxNormalizer::CollectBareNotationBatch(const SyntaxModel &Syntax, RewritePlan &Plan) const {
		// 共通の祖先の障壁・所属と，一括適用の失効走査を共有する
		for(const NodeId Id : GetCandidates(Syntax, { Role::BareArgNotation })) {
			Syntax.Charge(1);
			// 個別適用と同じ引用・トークン列の障壁を守る
			if(!Syntax.IsRemovedNode(Id) && Syntax.GetNode(Id).Facts.TokenSpacing == TokenSpacingMode::None) {
				CollectBareNotationNode(Syntax, Id, Plan);
			}
		}
		// 終了
		return;
	}

	/**
	 * 型照会の比較演算子の厳密な形の収集関数（整形仕様 5.2）
	 * 計算量：対象の探索・判定・計画の総量 Q に対し O(Q)
	 * @param Syntax 現在の構文
	 * @param Id 選別済の対象
	 * @param Plan 書換の格納先
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 判定又は計画のメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectTypeQueryNode(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const {
		// 保護された祖先の内側の場合の終了
		if(HasProtectedNormalizationAncestor(Syntax, Id)) return;
		NodeId Left = Syntax.GetFieldChild(Id, ChildRole::Left);
		NodeId Right = Syntax.GetFieldChild(Id, ChildRole::Right);
		while(Syntax.Is(Left, Role::GroupingParen)) Left = Syntax.GetNamedChild(Left, 0);
		while(Syntax.Is(Right, Role::GroupingParen)) Right = Syntax.GetNamedChild(Right, 0);
		const bool IsRightTypeof = Syntax.HasWordChild(Right, Word::TypeQuery);
		const NodeId Operator = Syntax.GetFieldChild(Id, ChildRole::Operator);
		const std::string OperatorText = Syntax.GetText(Operator);
		if(
			const SyntaxRules::OperatorNotation &Operators = Syntax.GetRules().Operators;
			(OperatorText == Operators.Equal || OperatorText == Operators.NotEqual) && (
				Syntax.HasWordChild(Left, Word::TypeQuery) && (IsRightTypeof || Syntax.Is(Right, Role::QuotedStr)) ||
				IsRightTypeof && Syntax.Is(Left, Role::QuotedStr)
			)
		) {
			const std::string Strict(OperatorText == Operators.Equal ? Operators.StrictEqual : Operators.StrictNotEqual);
			RewriteStep Step { RewriteKind::Reshape, Operator };
			Step.Inserted = { Strict };
			Plan.Steps.push_back(std::move(Step));
		}
		// 終了
		return;
	}

	/**
	 * 名前付のインポートの辞書順の収集関数（整形仕様 5.2）
	 * 計算量：対象の探索・判定・計画の総量 Q に対し O(Q)
	 * @param Syntax 現在の構文
	 * @param Id 選別済の対象
	 * @param Plan 書換の格納先
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 判定又は計画のメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectImportOrderNode(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const {
		// 保護された祖先の内側の場合の終了
		if(HasProtectedNormalizationAncestor(Syntax, Id)) return;
		const SyntaxNode &Node = Syntax.GetNode(Id);
		std::vector<NodeId> Names;
		for(const NodeId Child : Node.Children) if(Syntax.IsNamed(Child)) Names.push_back(Child);
		// 並べ替えるインポート名と比較用の綴り
		struct ImportName {
			NodeId Node;
			std::string Name;
			std::string Folded; // 大文字小文字を無視する比較用の綴り
		};
		std::vector<ImportName> Keys;
		for(const NodeId Child : Names) {
			const NodeId Name = Syntax.GetFieldChild(Child, ChildRole::Name);
			std::string Spelling = Syntax.GetCmpName(Name == NoNode ? Child : Name);
			Syntax.Charge((Spelling.size() << 1) + 1);
			std::string Folded = Spelling;
			ToLowerAscii(Folded);
			Keys.push_back({ Child, std::move(Spelling), std::move(Folded) });
		}
		WorkSort::Stable(
			std::span(Keys),
			[&Syntax](const ImportName &Left, const ImportName &Right) -> bool {
				Syntax.Charge(1);
				// 大文字・小文字を揃えたインポート名と元の綴りの順の返戻
				return std::tie(Left.Folded, Left.Name) < std::tie(Right.Folded, Right.Name);
			},
			[&Syntax](const size_t Amount) -> void {
				Syntax.Charge(Amount);
			}
		);
		for(size_t Pos = 0; Pos < Keys.size(); ++Pos) Names[Pos] = Keys[Pos].Node;
		size_t Idx = 0;
		bool IsOrdered = true;
		for(const NodeId Child : Node.Children) if(Syntax.IsNamed(Child)) IsOrdered = IsOrdered && Child == Names[Idx++];
		if(!IsOrdered) {
			RewriteStep Sort { RewriteKind::Reshape, Id };
			Sort.Inserted = { {}, true, {}, 0, NoNode, {}, Syntax.GetGenerationPartOf(Id) };
			Sort.Inserted.Facts = Node.Facts;
			Idx = 0;
			for(const NodeId Child : Node.Children) {
				Sort.Inserted.Children.push_back({ {}, false, {}, Syntax.GetNode(Child).Fields, Syntax.IsNamed(Child) ? Names[Idx++] : Child });
			}
			Plan.Steps.push_back(std::move(Sort));
		}
		// 終了
		return;
	}

	/**
	 * 真偽値型の初期値の真偽値の名前への正規化の収集関数（整形仕様 5.2）
	 * C 系の `bool`（C の `_Bool`）の宣言で，素の名前の宣言子の初期値 `0` / `1` を `false` / `true` へ揃える
	 * 使用位置で真偽値の名前を解決出来る場合だけ書き換える
	 * 計算量：参照するノードと祖先の総数 N，テキストの総長 L に対し O(N + L)
	 * @param Syntax 構文モデル
	 * @param Id ノード
	 * @param Plan 書換の計画の格納先
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectBoolLiteralStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const {
		// 保護された祖先の内側の終了
		if(HasProtectedNormalizationAncestor(Syntax, Id)) return;
		// `bool` 型でない宣言と，束縛を確定出来ない宣言の終了
		if(
			const NodeId Type = Syntax.GetFieldChild(Id, ChildRole::Type);
			!((Syntax.Is(Type, Role::PrimitiveType) || Syntax.Is(Type, Role::TypeName)) && Syntax.IsWord(Type, Word::Boolean)) ||
			HasMacroBindingIn(Syntax, Id)
		) return;
		// 宣言子の実効型と，其の初期値の役割を１度確認する
		std::vector<NodeId> Pending;
		const auto Rewrite = [&Syntax, Id, &Pending, &Plan](NodeId Name, const NodeId Val) -> void {
			bool IsArray = false;
			while(Syntax.IsAny(Name, { Role::ArrayDeclarator, Role::ParenthesizedDeclarator })) {
				Syntax.Charge(1);
				IsArray |= Syntax.Is(Name, Role::ArrayDeclarator);
				Name = Syntax.GetNamedChild(Name, 0);
			}
			// 間接型と，仮引数でポインタへ調整する配列の除外
			if(
				!Syntax.IsAny(Name, { Role::FieldName, Role::Identifier, Role::QualifiedIdentifier }) &&
				!(Name == NoNode && Syntax.Is(Id, Role::DefaultParam)) || IsArray && Syntax.Is(Id, Role::DefaultParam)
			) return;
			Pending.push_back(Val);
			while(!Pending.empty()) {
				const NodeId Cur = Pending.back();
				Pending.pop_back();
				Syntax.Charge(1);
				if(Syntax.Is(Cur, Role::GroupingParen)) Pending.push_back(Syntax.GetNamedChild(Cur, 0));
				else if(Syntax.Is(Cur, Role::InitializerExpr) || Cur == Val && Syntax.Is(Cur, Role::ArgSeq)) {
					// 子の確認・値の選択・要素数の計数を同じ走査で行う
					const size_t Start = Pending.size();
					NodeId Value = NoNode;
					// スタックへ逆順に積み，書換を構文順に収集する
					for(const NodeId Child : std::views::reverse(Syntax.GetChildrenOf(Cur))) {
						Syntax.Charge(1);
						if(!Syntax.IsNamed(Child)) continue;
						if(Syntax.GetNode(Child).Fields & Syntax.GetFieldBit(ChildRole::Val)) {
							// 指示付初期化は値だけを読み，インデックスを初期値へ含めない
							Value = Child;
							Pending.resize(Start);
						}
						if(Value == NoNode || Child == Value) Pending.push_back(Child);
					}
					// スカラーの初期化子列は単一要素だけを受理する
					if(Value == NoNode && !IsArray && Pending.size() - Start != 1) Pending.resize(Start);
				} else {
					const bool IsTrue = Syntax.IsWord(Cur, Word::IntTruth);
					// 整数の `0` / `1` 以外と，生成先の名前へ解決出来ない値の除外
					if(!Syntax.Is(Cur, Role::NumLiteral) || !IsTrue && !Syntax.IsWord(Cur, Word::IntZero)) continue;
					const SyntaxFacts &Facts = Syntax.GetNode(Cur).Facts;
					if(IsTrue ? !Facts.HasTrueName : !Facts.HasFalseName) continue;
					RewriteStep Literal { RewriteKind::Reshape, Cur };
					Literal.Inserted = {
						{},
						true,
						{},
						0,
						NoNode,
						std::string(Syntax.GetRules().GetSpelling(IsTrue ? Word::True : Word::False)),
						IsTrue ? Syntax.GetRules().TrueDef : Syntax.GetRules().FalseDef
					};
					Plan.Steps.push_back(std::move(Literal));
				}
			}
		};
		// 仮引数の既定値は宣言子の外側で評価する
		if(Syntax.Is(Id, Role::DefaultParam)) {
			NodeId Name = NoNode, Val = NoNode;
			Core::SyntaxModel::ForEachNamedChildOf(
				Syntax,
				Id,
				[&](const NodeId Child) -> void {
					if(Name == NoNode && Syntax.GetNode(Child).Fields & Syntax.GetFieldBit(ChildRole::Declarator)) Name = Child;
					Val = Child;
				}
			);
			Rewrite(Name, Val);
		} else {
			// メンバの宣言は宣言子で包まずに初期値を名前の後の子に持つ
			NodeId Prev = NoNode;
			Core::SyntaxModel::ForEachNamedChildOf(
				Syntax,
				Id,
				[&](const NodeId Child) -> void {
					if(Syntax.Is(Child, Role::InitDeclarator)) {
						NodeId Name = NoNode, Val = NoNode;
						Core::SyntaxModel::ForEachNamedChildOf(
							Syntax,
							Child,
							[&](const NodeId Part) -> void {
								const uint64_t Fields = Syntax.GetNode(Part).Fields;
								if(Name == NoNode && Fields & Syntax.GetFieldBit(ChildRole::Declarator)) Name = Part;
								if(Val == NoNode && Fields & Syntax.GetFieldBit(ChildRole::Val)) Val = Part;
							}
						);
						Rewrite(Name, Val);
					} else if(Prev != NoNode && Syntax.GetNode(Prev).Fields & Syntax.GetFieldBit(ChildRole::Declarator)) Rewrite(Prev, Child);
					Prev = Child;
				}
			);
		}
		// 終了
		return;
	}

	/**
	 * 型の後の `const` の型の前への移動の収集関数（整形仕様 5.2）
	 * C 系の宣言で最初の型指定子の後に在る最初の `const` を，其の型指定子の直前へ移す
	 * 型の前の属性・修飾子は動かさず，宣言子の記号へ展開するマクロの型は保つ
	 * 計算量：参照するノードと祖先の総数 N，テキストの総長 L に対し O(N + L)
	 * @param Syntax 構文モデル
	 * @param Id ノード
	 * @param Plan 書換の計画の格納先
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectConstPosStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const {
		// 保護された祖先の内側と，C 系の宣言以外の終了
		if(
			HasProtectedNormalizationAncestor(Syntax, Id) || HasMacroBindingIn(Syntax, Id) ||
			Syntax.GetNode(Id).Facts.Has(ConstraintKind::AmbiguousDecl)
		) return;
		NodeId Type = NoNode, Qualifier = NoNode;
		Core::SyntaxModel::ForEachNamedChildOf(
			Syntax,
			Id,
			[&](const NodeId Child) -> void {
				if(const bool IsTypeSpecifier = Syntax.Is(Child, Role::DeclTypeSpecifier); IsTypeSpecifier && Type == NoNode) Type = Child;
				else if(
					!IsTypeSpecifier && Syntax.Is(Child, Role::TypeQualifier) && Type != NoNode && Qualifier == NoNode &&
					Syntax.IsWord(Child, Word::ConstQualifier)
				) Qualifier = Child;
			}
		);
		// 型の後の `const` が無い宣言と宣言子の記号へ展開するマクロの型の終了
		if(Qualifier == NoNode || Syntax.GetNode(Type).Facts.Has(ConstraintKind::DeclaratorMacro)) return;
		RewriteStep Move { RewriteKind::Move, Qualifier };
		Move.Dst = Id;
		Move.Idx = Syntax.GetNode(Type).Idx;
		Move.Boundary = BoundaryKind::Spaced;
		Plan.Steps.push_back(std::move(Move));
		// 終了
		return;
	}

	/**
	 * 言語が省略を許す括弧の除去の収集関数（整形仕様 5.2）
	 * 名前１つのラムダの仮引数の括弧（`(x) -> x` → `x -> x`）と，文法が省略を許す空の実引数・仮引数の並びを外す
	 * アノテーション・属性・列挙の定数では，空の実引数を省く（`@A()` → `@A`）
	 * 初期化子を伴う生成では，空の実引数を省く（`new A() { X = 1 }` → `new A { X = 1 }`）
	 * 後置のラムダ・クロージャだけを渡す呼出では，空の実引数を省く（`f() { }` → `f { }`）
	 * 副のコンストラクタの無い型では，空の主のコンストラクタを省く（`class A()` → `class A`）
	 * C++ の修飾・戻値の型の無いラムダでは，空の仮引数を省く（`[]() { }` → `[] { }`）
	 * C からも読むヘッダを除く C++ では，空の仮引数の `void` を省く（`f(void)` → `f()`）
	 * 計算量：参照するノードと祖先の総数 N，テキストの総長 L に対し O(N + L)
	 * @param Syntax 構文モデル
	 * @param Id ノード
	 * @param Plan 書換の計画の格納先
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectOptionalParenStep(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const {
		// 保護された祖先の内側の場合の終了
		if(HasProtectedNormalizationAncestor(Syntax, Id) || HasMacroBindingIn(Syntax, Id)) return;
		// ノードと其の子孫の除去（子孫を含むアンカーは指定の行先へ移す）
		const auto RemoveSubtree = [&Plan](const NodeId Root, const AnchorTarget Target) -> void {
			Plan.Steps.push_back({ RewriteKind::Remove, Root, {}, { Root, Target, Target, Target } });
		};
		// 単一の名前だけの仮引数の括弧の除去
		if(Syntax.Is(Id, Role::ArrowFunction)) {
			const NodeId Params = Syntax.GetFieldChild(Id, ChildRole::Params);
			NodeId Param = Syntax.GetNamedChild(Params, 0);
			if(Syntax.Is(Param, Role::RequiredParam) && Syntax.GetChildCount(Param) == 1) Param = Syntax.GetChild(Param, 0);
			if(
				Syntax.GetChildCount(Params) == 3 && Syntax.Is(Param, Role::Identifier) &&
				Syntax.GetFieldChild(Id, ChildRole::ReturnType) == NoNode && !Core::SyntaxModel::HasChildMatching(
					Syntax,
					Id,
					[&](const NodeId Child) -> bool {
						return Syntax.Is(Child, Role::TypeParamList);
					}
				)
			) {
				if(Param != Syntax.GetNamedChild(Params, 0)) Plan.Steps.push_back({ RewriteKind::Unwrap, Syntax.GetNamedChild(Params, 0) });
				Plan.Steps.push_back({ RewriteKind::Unwrap, Params });
			}
			// ラムダの仮引数の省略条件を確認した場合の終了
			return;
		}
		if(
			Syntax.Is(Id, Role::ArgSeq) && Syntax.Is(Id, Role::UnparenthesizedBindingValSlot) &&
			Syntax.Is(Syntax.GetParentOf(Id), Role::CallNode) && Syntax.GetNamedChildCount(Id) == 1
		) {
			NodeId Val = Syntax.GetNamedChild(Id, 0);
			std::vector<NodeId> Wrappers;
			while(Syntax.Is(Val, Role::GroupingParen) && Syntax.GetChildCount(Val) == 3 && Syntax.GetNamedChildCount(Val) == 1) {
				Syntax.Charge(1);
				Wrappers.push_back(Val);
				Val = Syntax.GetNamedChild(Val, 0);
			}
			if(Syntax.Is(Val, Role::GeneratorExpr)) {
				Plan.Steps.push_back({ RewriteKind::Unwrap, Id });
				for(const NodeId Wrapper : Wrappers) Plan.Steps.push_back({ RewriteKind::Unwrap, Wrapper });
				// 生成式の括弧を唯一の実引数の区切として使う場合の終了
				return;
			}
		}
		// 後置の受手と型の解釈を保てる構築式の空の実引数の除去
		if(Syntax.Is(Id, Role::Construction)) {
			const NodeId Args = Syntax.GetFieldChild(Id, ChildRole::Args);
			NodeId Subject = Id, Parent = Syntax.GetParentOf(Id);
			while(Syntax.Is(Parent, Role::GroupingParen)) {
				Subject = Parent;
				Parent = Syntax.GetParentOf(Parent);
			}
			const bool IsReceiver = Syntax.GetNamedChild(Parent, 0) == Subject && (
				Syntax.Is(Parent, Role::PostfixExprHost) || Syntax.Is(Parent, Role::Construction) || Syntax.Is(Parent, Role::NonNullAssertion)
			);
			// 後置のアサーションで終わる被構築の式（外すとアサーションが構築の結果へ掛かる）
			if(
				const bool IsCmp = Syntax.GetFieldChild(Parent, ChildRole::Left) == Subject && Syntax.Is(Parent, Role::BinaryOperation) &&
				Syntax.IsTokenRole(Syntax.GetFieldChild(Parent, ChildRole::Operator), Role::LessThanToken),
				IsAssertedCallee = Syntax.Is(Syntax.GetNamedChild(Id, 0), Role::NonNullAssertion); Syntax.GetChildCount(Args) == 2 &&
				!IsReceiver && !IsCmp && !IsAssertedCallee && !Core::SyntaxModel::HasChildMatching(
					Syntax,
					Id,
					[&](const NodeId Child) -> bool {
						return Syntax.Is(Child, Role::TypeArgList);
					}
				)
			) RemoveSubtree(Args, { Id, true });
			// 構築式の実引数の省略条件を確認した場合の終了
			return;
		}
		// 空の基底型と呼出・生成の実引数の除去
		if(Syntax.Is(Id, Role::OptionalEmptySuperclass)) {
			if(
				const NodeId Args = Syntax.GetFirstNamedChildWithRole(Id, Role::ArgSeq);
				Args != NoNode && !Syntax.GetNamedChildCount(Args)
			) RemoveSubtree(Args, { Id, false });
		}
		if(Syntax.Is(Id, Role::OptionalConstructionArgs)) {
			// 呼出と生成の空の実引数の探索
			NodeId Args = NoNode;
			if(Syntax.Is(Id, Role::FunctionCallExpr) && Syntax.IsWord(Syntax.GetFieldChild(Id, ChildRole::Function), Word::BareCall)) if(
				const NodeId LocalArgs = Syntax.GetFieldChild(Id, ChildRole::Args);
				LocalArgs != NoNode && !Syntax.GetNamedChildCount(LocalArgs)
			) {
				RemoveSubtree(LocalArgs, { Id, false });
				Plan.Steps.push_back({ RewriteKind::Unwrap, Id });
			}
			if(Syntax.Is(Id, Role::Attribute)) Args = Syntax.GetFirstNamedChildWithRole(Id, Role::CallArgs);
			if(Syntax.Is(Id, Role::ObjectCreationExpr)) {
				NodeId Owner = Syntax.GetParentOf(Id), Val = Id;
				while(Syntax.Is(Owner, Role::ValParens)) {
					Val = Owner;
					Owner = Syntax.GetParentOf(Owner);
				}
				if(!Syntax.Is(Owner, Role::ConstructionReceiverHost) || Syntax.GetNamedChild(Owner, 0) != Val) {
					const NodeId Anonymous = Syntax.GetFirstNamedChildWithRole(Id, Role::AnonymousClass);
					Args = Syntax.GetFirstNamedChildWithRole(Anonymous == NoNode ? Id : Anonymous, Role::CallArgs);
				}
			}
			if(Args != NoNode && !Syntax.GetNamedChildCount(Args)) RemoveSubtree(Args, { Id, false });
			if(
				Syntax.Is(Id, Role::ExitStmt) && Syntax.GetChildCount(Id) > 2 &&
				Syntax.IsTokenRole(Syntax.GetChild(Id, 1), Role::OpeningParenToken) &&
				Syntax.IsTokenRole(Syntax.GetChild(Id, 2), Role::ClosingParenToken)
			) {
				RemoveSubtree(Syntax.GetChild(Id, 1), { Id, false });
				RemoveSubtree(Syntax.GetChild(Id, 2), { Id, false });
			}
		}
		if(Syntax.Is(Id, Role::OptionalLambdaDeclarator)) {
			const NodeId Declarator = Syntax.GetFieldChild(Id, ChildRole::Declarator);
			// 仮引数だけの宣言子の空の並びの除去（修飾・戻値の型・制約を伴う省略は C++23 より前では出来ない）
			if(
				const NodeId Params = Syntax.GetChildCount(Declarator) == 1 ? Syntax.GetChild(Declarator, 0) : NoNode;
				Syntax.Is(Params, Role::DeclaratorParams) && Syntax.GetChildCount(Params) == 2 &&
				Syntax.IsTokenRole(Syntax.GetChild(Params, 0), Role::OpeningParenToken)
			) RemoveSubtree(Declarator, { Syntax.GetPrevSibling(Declarator), true });
			// ラムダ宣言子の省略条件を確認した場合の終了
			return;
		}
		// C からも読むヘッダを除く C++ の空の仮引数の `void`
		if(Syntax.Is(Id, Role::OptionalVoidParam) && Syntax.GetChildCount(Id) == 3) {
			if(
				const NodeId Param = Syntax.GetChild(Id, 1);
				Syntax.Is(Param, Role::ParamDecl) && Syntax.GetChildCount(Param) == 1 &&
				Syntax.Is(Syntax.GetChild(Param, 0), Role::PrimitiveType) && Syntax.IsWord(Param, Word::VoidType)
			) RemoveSubtree(Param, { Syntax.GetChild(Id, 0), true });
			// 空の仮引数の `void` の省略条件を確認した場合の終了
			return;
		}
		if(Syntax.Is(Id, Role::LambdaExpr)) {
			// 推論の仮引数と型の無い仮引数の括弧の判定
			const NodeId Params = Syntax.GetFieldChild(Id, ChildRole::Params);
			const NodeId Param = Syntax.GetNamedChild(Params, 0);
			if(Syntax.Is(Params, Role::InferredParams)) {
				// 名前１つの推論の仮引数の括弧の除去
				if(Syntax.GetChildCount(Params) == 3 && Syntax.Is(Param, Role::Identifier)) {
					Plan.Steps.push_back({ RewriteKind::Unwrap, Params });
				}
			} else if(
				Syntax.Is(Params, Role::DeclaratorParams) && Syntax.GetChildCount(Params) == 3 && Syntax.Is(Param, Role::Param) &&
				Syntax.GetChildCount(Param) == 1 && Syntax.Is(Syntax.GetChild(Param, 0), Role::Identifier) &&
				!Core::SyntaxModel::HasChildMatching(
					Syntax,
					Id,
					[&](const NodeId Child) -> bool {
						// 仮引数の前の修飾以外の名前付の子（属性・戻値の型は括弧の仮引数だけに付く）かの返戻
						return Syntax.IsNamed(Child) && Syntax.IsBefore(Child, Params) && !Syntax.Is(Child, Role::Modifier);
					}
				)
			) {
				// 型・修飾の無い仮引数１つの括弧の除去（名前は暗黙の仮引数に為る）
				Plan.Steps.push_back({ RewriteKind::Unwrap, Params });
				Plan.Steps.push_back({ RewriteKind::Unwrap, Param });
				RewriteStep Implicit { RewriteKind::Reshape, Syntax.GetChild(Param, 0) };
				Implicit.Inserted = { {}, true, {}, 0, NoNode, {}, Syntax.GetRules().ImplicitParamDef };
				Plan.Steps.push_back(std::move(Implicit));
			}
			// ラムダの仮引数の括弧の省略条件を確認した場合の終了
			return;
		}
		if(Syntax.Is(Id, Role::LambdaFunctionType)) {
			const NodeId Params = Syntax.GetFirstNamedChildWithRole(Id, Role::LambdaFunctionTypeParams);
			// 名前だけの仮引数の並びか
			bool IsBare =
			Params != NoNode && Syntax.GetChildCount(Id) > 2 && Syntax.IsTokenRole(Syntax.GetChild(Id, 0), Role::OpeningParenToken);
			Core::SyntaxModel::ForEachNamedChildOf(
				Syntax,
				Params,
				[&](const NodeId Param) -> void {
					IsBare = IsBare && Syntax.Is(Param, Role::LambdaParam) && Syntax.GetChildCount(Param) == 1 &&
					Syntax.Is(Syntax.GetChild(Param, 0), Role::SimpleName);
				}
			);
			// 型の無い名前だけのクロージャの仮引数の括弧の除去（アンカーは仮引数の並びの前後へ移す）
			if(IsBare) {
				const AnchorTarget Before { Params, false }, After { Params, true };
				const NodeId Open = Syntax.GetChild(Id, 0), Close = Syntax.GetNextSibling(Params);
				Plan.Steps.push_back({ RewriteKind::Remove, Open, {}, { Open, Before, Before, Before } });
				Plan.Steps.push_back({ RewriteKind::Remove, Close, {}, { Close, After, After, After } });
			}
			// 裸のクロージャ仮引数の括弧の省略条件を確認した場合の終了
			return;
		}
		// 末尾のクロージャを別の接尾辞で渡す呼出の空の実引数（被呼出側が呼出なら結付を保つ）
		if(Syntax.Is(Id, Role::CallSuffix) && Syntax.GetNamedChildCount(Id) == 1) {
			const NodeId Args = Syntax.GetNamedChild(Id, 0), Call = Syntax.GetParentOf(Id);
			const NodeId Outer = Syntax.GetParentOf(Call);
			if(
				const NodeId Trailing = Syntax.GetLastNamedChild(Outer);
				Syntax.Is(Args, Role::ValArgs) && Syntax.GetChildCount(Args) == 2 && Syntax.Is(Call, Role::CallExpr) &&
				Syntax.Is(Outer, Role::CallExpr) && Syntax.GetNamedChild(Outer, 0) == Call && Syntax.Is(Trailing, Role::CallSuffix) &&
				Syntax.Is(Syntax.GetNamedChild(Trailing, 0), Role::LambdaLiteral) &&
				!Syntax.Is(Syntax.GetNamedChild(Call, 0), Role::CallExpr) && Syntax.GetChildCount(Call) == 2
			) {
				// 空の実引数の接尾辞の除去と，被呼出側だけに為る呼出ラッパーの除去
				const AnchorTarget Target { Syntax.GetNamedChild(Call, 0), true };
				for(const NodeId Removed : { Id, Args, Syntax.GetChild(Args, 0), Syntax.GetChild(Args, 1) }) {
					Plan.Steps.push_back({ RewriteKind::Remove, Removed, {}, { Removed, Target, Target, Target } });
				}
				Plan.Steps.push_back({ RewriteKind::Unwrap, Call });
				// 後置クロージャの空の実引数を除いた場合の終了
				return;
			}
		}
		// アノテーション・列挙の定数の実引数のフィールドから，省略候補の並びを選ぶ
		// 型の位置でないアノテーションでは，コンストラクタ呼出の実引数も候補とする
		NodeId List = Syntax.Is(Id, Role::OptionalAnnotationArgs) ? Syntax.GetFieldChild(Id, ChildRole::Args) : NoNode;
		NodeId Invocation = NoNode;
		if(List == NoNode && Syntax.Is(Id, Role::Annotation) && !Syntax.Is(Syntax.GetParentOf(Id), Role::TypeModifiers)) {
			// 型の位置のアノテーションは後続の関数型の仮引数の並びと紛れる為，省略の対象外とする
			Invocation = Syntax.GetFirstNamedChildWithRole(Id, Role::ConstructorInvocation);
			List = Syntax.GetFirstNamedChildWithRole(Invocation, Role::ValArgs);
		} else if(Syntax.Is(Id, Role::Attribute)) List = Syntax.GetFirstNamedChildWithRole(Id, Role::AttributeArgList);
		// 初期化子を伴う生成
		else if(Syntax.Is(Id, Role::ObjectCreationExpr) && Syntax.GetFirstNamedChildWithRole(Id, Role::InitializerExpr) != NoNode) {
			List = Syntax.GetFirstNamedChildWithRole(Id, Role::ArgSeq);
		// 列挙の項目又は後置のラムダ・クロージャを持つ呼出の接尾辞の実引数の選択
		// 被呼出側が呼出の場合は結付の保持
		} else if(
			Syntax.Is(Id, Role::CallSuffix) && (
				Syntax.GetFirstNamedChildWithRole(Id, Role::AnnotatedLambda) != NoNode ||
				Syntax.GetFirstNamedChildWithRole(Id, Role::LambdaLiteral) != NoNode
			) && !Syntax.Is(Syntax.GetNamedChild(Syntax.GetParentOf(Id), 0), Role::CallExpr) || Syntax.Is(Id, Role::EnumEntry)
		) List = Syntax.GetFirstNamedChildWithRole(Id, Role::ValArgs);
		// 副のコンストラクタの無い型の主のコンストラクタ
		else if(Syntax.Is(Id, Role::ClassDecl)) {
			NodeId Body = Syntax.GetFirstNamedChildWithRole(Id, Role::ClassMembers);
			if(Body == NoNode) Body = Syntax.GetFirstNamedChildWithRole(Id, Role::EnumClassBody);
			if(Syntax.GetFirstNamedChildWithRole(Body, Role::SecondaryConstructor) == NoNode) {
				List = Syntax.GetFirstNamedChildWithRole(Id, Role::PrimaryConstructor);
			}
		}
		// 開閉の括弧だけの並び以外の終了
		if(Syntax.GetChildCount(List) != 2 || !Syntax.IsTokenRole(Syntax.GetChild(List, 0), Role::OpeningParenToken)) return;
		// 空の並びの除去（アンカーは前の兄弟の後，前の兄弟が無ければ後の兄弟の前へ移す）
		const NodeId Prev = Syntax.GetPrevSibling(List);
		const AnchorTarget Target = Prev != NoNode ? AnchorTarget{ Prev, true } : AnchorTarget{ Syntax.GetNextSibling(List), false };
		// 空の実引数の省略に依って文法上の種別も変わる形式
		if(
			const CanonicalForm *const Form = Syntax.GetFormOf(Id, FormOperation::OmitEmptyArgs);
			Form && Form->CanOmitEmptyArgs(true) && Invocation == NoNode
		) {
			RewriteStep Step { RewriteKind::Reshape, Id };
			Step.Transfer = { List, Target, Target, Target };
			Step.Inserted.Def = *Form->GetPartOf(FormPartRole::Result);
			Step.Inserted.IsNamed = Step.Inserted.Def.IsNamed;
			for(const NodeId Child : Syntax.GetChildrenOf(Id)) if(Child != List) {
				Step.Inserted.Children.push_back({ {}, false, {}, Syntax.IsNamed(Child) ? Syntax.GetFieldBit(ChildRole::Name) : 0, Child });
			}
			Plan.Steps.push_back(std::move(Step));
			// 空の実引数を省いて組み直した場合の終了
			return;
		}
		Plan.Steps.push_back({ RewriteKind::Remove, List, {}, { List, Target, Target, Target } });
		for(const NodeId Token : Syntax.GetChildrenOf(List)) {
			Plan.Steps.push_back({ RewriteKind::Remove, Token, {}, { Token, Target, Target, Target } });
		}
		// 空の実引数を除いたコンストラクタ呼出は型だけを残す為，余分なラッパーも除く
		if(Invocation != NoNode) Plan.Steps.push_back({ RewriteKind::Unwrap, Invocation });
		// 終了
		return;
	}

	/**
	 * 改行が代替する文・メンバのセミコロンの除去の収集関数
	 * 計算量：参照するノードと祖先の総数 N，テキストの総長 L に対し O(N + L)
	 * @param Syntax 構文モデル
	 * @param Id 現在の規則が評価する１ノード又は１所有者
	 * @param Plan 書換の計画の格納先
	 * @throws FormatLimitExceeded 構文の探索・事実の収集又は書換が作業予算を超える場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::CollectSeparatorNode(const SyntaxModel &Syntax, const NodeId Id, RewritePlan &Plan) const {
		// 最上位の空の式文 `;`（誤読された宣言の終端に為るエラー領域の直後は保つ）
		Syntax.Charge(1);
		if(Syntax.GetParentOf(Id) == Syntax.GetRoot() && Syntax.IsNamed(Id)) if(
			const NodeId PrevCode = Syntax.GetPrevNamedSibling(Id);
			Syntax.Is(Id, Role::ExprStmt) && Syntax.IsWord(Id, Word::StmtEnd) &&
			(PrevCode == NoNode || !Syntax.GetNode(PrevCode).Facts.Has(ConstraintKind::ContainsError))
		) {
			// 同じ親の空文と，逐次除去した場合の最終のアンカーの行先を１度で収集
			RewriteStep Step { RewriteKind::RemoveSiblings, Syntax.GetRoot() };
			NodeId PrevKept = NoNode;
			for(const NodeId Child : Syntax.GetChildrenOf(Syntax.GetRoot())) {
				Syntax.Charge(1);
				if(!Syntax.IsNamed(Child)) continue;
				if(
					Syntax.Is(Child, Role::ExprStmt) && Syntax.IsWord(Child, Word::StmtEnd) &&
					(PrevKept == NoNode || !Syntax.GetNode(PrevKept).Facts.Has(ConstraintKind::ContainsError))
				) {
					const AnchorTarget Dst = PrevKept == NoNode ? AnchorTarget{ Syntax.GetRoot(), false } : AnchorTarget{ PrevKept, true };
					Step.RemovedSiblings.push_back({ Child, Dst, Dst, Dst });
				} else {
					// 先頭の連続した空文のアンカーは最初に残る名前付の子の前へ置く
					if(PrevKept == NoNode) for(AnchorTransfer &Transfer : Step.RemovedSiblings) {
						Syntax.Charge(1);
						const AnchorTarget Dst { Child, false };
						Transfer.Leading = Dst;
						Transfer.Trailing = Dst;
						Transfer.Inner = Dst;
					}
					PrevKept = Child;
				}
			}
			Plan.Steps.push_back(std::move(Step));
		}
		// 改行で代える区切を持たないノードと，エラー障壁の内側の場合の終了
		if(!Syntax.Is(Id, Role::OptionalSemicolonHost) || HasErrorNormalizationAncestor(Syntax, Id)) return;
		const bool IsInlineCode = GetNormalizationBarrierOf(Syntax, Id).DoesRequireInlineCode;
		const std::span<const NodeId> Children = Syntax.GetChildrenOf(Id);
		// 原文を保つ境界と除ける区切の判定
		std::vector<uint8_t> Removed(Children.size(), 0), Preserved(Children.size() + 1, 0);
		for(const SemanticBoundary &Boundary : Syntax.GetNode(Id).Facts.Boundaries.GetView()) {
			if(Boundary.Kind == BoundaryKind::Preserved && Boundary.Before < Preserved.size()) Preserved[Boundary.Before] = 1;
		}
		for(uint32_t Idx = 0; Idx < Children.size(); ++Idx) {
			const NodeId Child = Children[Idx];
			const bool IsSeparator = Syntax.IsWord(Child, Word::StmtEnd) &&
			(!Syntax.IsNamed(Child) || Syntax.Is(Id, Role::EmptySemicolonSeq) && Syntax.Is(Child, Role::EmptyStmt));
			const bool IsProtected = Preserved[Idx] || Preserved[Idx + 1] || Idx && Syntax.Is(Children[Idx - 1], Role::Error) ||
			Idx + 1 < Children.size() && Syntax.Is(Children[Idx + 1], Role::Error);
			Removed[Idx] = IsSeparator && !IsProtected;
		}
		// 除去後に続く子と空の文の並びの判定
		std::vector<NodeId> Following(Children.size(), NoNode);
		NodeId NextKept = NoNode;
		for(size_t Idx = Children.size(); Idx--;) {
			Following[Idx] = NextKept;
			if(!Removed[Idx]) NextKept = Children[Idx];
		}
		const NodeId Parent = Syntax.GetParentOf(Id);
		const bool IsOptionalList = Parent != NoNode && (
			Syntax.Is(Parent, Role::LexicalBlock) || Syntax.Is(Parent, Role::ExprCase) || Syntax.Is(Parent, Role::DefaultCase) ||
			Syntax.Is(Parent, Role::TypeCase) || Syntax.Is(Parent, Role::CommunicationCase)
		);
		const bool ShouldRemoveWholeList =
		Syntax.Is(Id, Role::PlainStmtSeq) && !Children.empty() && NextKept == NoNode && IsOptionalList;
		const AnchorTarget EmptyBody { Parent, false, true, Parent != NoNode && !Syntax.Is(Parent, Role::LexicalBlock) };
		// 残る子を行先とする区切の除去
		RewriteStep Step { RewriteKind::RemoveSiblings, Id };
		NodeId PrevKept = NoNode;
		for(uint32_t Idx = 0; Idx < Children.size(); ++Idx) {
			const NodeId Child = Children[Idx];
			if(
				!Removed[Idx] || IsInlineCode && PrevKept != NoNode && !Syntax.IsWord(PrevKept, Word::StmtEnd) && Following[Idx] != NoNode &&
				!Syntax.IsTokenRole(Following[Idx], Role::ClosingBracketLead)
			) {
				PrevKept = Child;
				continue;
			}
			const AnchorTarget Dst = ShouldRemoveWholeList ?
			EmptyBody :
			Syntax.Is(Child, Role::EmptyStmt) && Following[Idx] != NoNode ? AnchorTarget{ Following[Idx], false } : PrevKept != NoNode ?
			AnchorTarget{ PrevKept, true } :
			Following[Idx] != NoNode ? AnchorTarget{ Following[Idx], false } : AnchorTarget{ Id, false };
			Step.RemovedSiblings.push_back({ Child, Dst, Dst, Dst });
		}
		if(!Step.RemovedSiblings.empty()) Plan.Steps.push_back(std::move(Step));
		if(ShouldRemoveWholeList) Plan.Steps.push_back({ RewriteKind::Remove, Id, {}, { Id, EmptyBody, EmptyBody, EmptyBody } });
		// 終了
		return;
	}

	/**
	 * 正規化と比較テストで共有する変更不能な規則表の取得関数
	 * 計算量：固定の文法の部品又は規則の表に対し O(1)
	 * @return 名前・対象の役割・収集処理を実行順に並べた固定の規則表
	 * @throws std::bad_alloc 規則表の初期化でメモリを確保出来ない場合
	 * @throws std::length_error 規則表の初期化で要素数上限を超える場合
	 */
	const std::vector<SyntaxNormalizer::RuleDescriptor> &SyntaxNormalizer::GetRules() {
		// 正規化と比較テストで共有する変更不能な規則表の取得
		static const std::vector<RuleDescriptor> Rules {
			{
				"ExplicitReturn",
				{ Role::ExplicitResultFunction },
				{},
				NodeCollectEntry(&SyntaxNormalizer::CollectExplicitReturnNode),
				true
			},
			{ "TailReturn", { Role::TailValFunction }, {}, NodeCollectEntry(&SyntaxNormalizer::CollectTailReturnNode) },
			{ "Increment", { Role::PrefixableIncrement }, {}, RootCollectEntry(&SyntaxNormalizer::CollectIncrementBatch) },
			{
				"Loop",
				{
					Role::ClauseOrCondLoop,
					Role::CountedLoop,
					Role::ParenthesizedLoopCond,
					Role::PostCondLoop,
					Role::UnconditionalLoopKeyword,
					Role::WhileLoop
				},
				{},
				NodeCollectEntry(&SyntaxNormalizer::CollectLoopStep)
			},
			{ "StatementForms", { Role::KeywordBlock }, {}, RootCollectEntry(&SyntaxNormalizer::CollectStmtBatch) },
			{
				"Terminator",
				{ Role::MemberBody, Role::TerminatedStmt, Role::TypeMemberBody },
				{},
				NodeCollectEntry(&SyntaxNormalizer::CollectTerminatorNode)
			},
			{
				"TrailingComma",
				{ Role::BareListTrailingComma },
				{ Role::CommaToken },
				NodeCollectEntry(&SyntaxNormalizer::CollectTrailingCommaNode)
			},
			{ "LoopTerminator", { Role::OptionalLoopTerminator }, {}, NodeCollectEntry(&SyntaxNormalizer::CollectLoopTerminatorStep) },
			{ "Number", { Role::NormalizedNum }, {}, RootCollectEntry(&SyntaxNormalizer::CollectNumBatch) },
			{ "Style", { Role::StyleLexeme }, {}, NodeCollectEntry(&SyntaxNormalizer::CollectStyleNodeStep) },
			{ "Quote", { Role::NormalizedQuote }, {}, NodeCollectEntry(&SyntaxNormalizer::CollectQuotedNodeStep) },
			{ "BareNotation", { Role::BareArgNotation }, {}, RootCollectEntry(&SyntaxNormalizer::CollectBareNotationBatch) },
			{ "TypeQuery", { Role::TypeofCmp }, {}, NodeCollectEntry(&SyntaxNormalizer::CollectTypeQueryNode) },
			{ "ImportOrder", { Role::NamedImports }, {}, NodeCollectEntry(&SyntaxNormalizer::CollectImportOrderNode) },
			{ "BooleanInitializer", { Role::BoolInitializerDecl }, {}, NodeCollectEntry(&SyntaxNormalizer::CollectBoolLiteralStep) },
			{ "ConstPosition", { Role::LeadingConstDecl }, {}, NodeCollectEntry(&SyntaxNormalizer::CollectConstPosStep) },
			{
				"OptionalParentheses",
				{
					Role::Annotation,
					Role::ArgSeq,
					Role::ArrowFunction,
					Role::Attribute,
					Role::CallSuffix,
					Role::ClassDecl,
					Role::Construction,
					Role::EnumEntry,
					Role::LambdaExpr,
					Role::LambdaFunctionType,
					Role::LambdaParams,
					Role::MethodParams,
					Role::ObjectCreationExpr,
					Role::OptionalAnnotationArgs,
					Role::OptionalConstructionArgs,
					Role::OptionalEmptySuperclass,
					Role::OptionalLambdaDeclarator,
					Role::OptionalVoidParam
				},
				{},
				NodeCollectEntry(&SyntaxNormalizer::CollectOptionalParenStep)
			},
			{
				"Parentheses",
				{ Role::GroupingParen, Role::GroupingTypeParen, Role::OperatorCall, Role::RemovablePatternParen },
				{},
				NodeCollectEntry(&SyntaxNormalizer::CollectParenNode),
				false,
				true
			},
			{ "FusedPrefix", { Role::UnaryPreOrUpdate }, {}, NodeCollectEntry(&SyntaxNormalizer::CollectFusedPrefixNodeStep) },
			{ "Separator", { Role::ExprStmt, Role::OptionalSemicolonHost }, {}, NodeCollectEntry(&SyntaxNormalizer::CollectSeparatorNode) },
			{ "TypeAssociation", { Role::TypeCombination }, {}, NodeCollectEntry(&SyntaxNormalizer::CollectTypeAssociationNodeStep) },
			{ "Tuple", { Role::TuplePattern, Role::TupleVal }, {}, NodeCollectEntry(&SyntaxNormalizer::CollectTupleNode) },
			{
				"Braces",
				{ Role::BodyOwner, Role::ElseClause, Role::If },
				{ Role::ValBranching },
				RootCollectEntry(&SyntaxNormalizer::CollectBraceBatch)
			},
			{
				"Declarations",
				{ Role::CommaDecl, Role::FinalizableLocal, Role::Iteration, Role::MutablePatternBinding, Role::ReplaceableBindingKeyword },
				{},
				RootCollectEntry(&SyntaxNormalizer::CollectDeclBatch)
			}
		};
		// 参照先の寿命を呼出後も保つ固定の共通規則表の返戻
		return Rules;
	}

	/**
	 * 固定の規則の対象選択・収集・適用関数
	 * 計算量：対象選択と判定・書換の総量 Q に対し O(Q)
	 * @param Syntax 現在の構文
	 * @param Rule 固定の規則
	 * @param HasFollowingRule 後続の規則が役割インデックスを読むか
	 * @param Trace アンカーの移譲の格納先
	 * @param HasMacroBindings マクロ束縛の事実を含み得るか（単独の規則では true）
	 * @throws FormatLimitExceeded 作業予算を超える場合
	 * @throws std::bad_alloc 対象・計画又はアンカーのメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	void SyntaxNormalizer::RunRule(
		SyntaxModel &Syntax,
		const RuleDescriptor &Rule,
		const bool HasFollowingRule,
		RewriteTrace &Trace,
		const bool HasMacroBindings
	) const {
		const size_t Before = RuleWork ? Syntax.GetWorkAccount()->Used : 0;
		for(const NodeId Node : MacroBindingNodes) {
			Syntax.Charge(1);
			MacroBindings[Node] = 0;
		}
		MacroBindingNodes.clear();
		HasMacroFacts = HasMacroBindings;
		DocstringValues.clear();
		// 固定の規則の対象選択・収集・適用
		Syntax.Charge(1);
		const std::vector<NodeId> Targets = GetRuleTargets(Syntax, Rule);
		if(!HasFollowingRule && Targets.empty()) Syntax.ReleaseRoleIdx();
		const RootCollectEntry *const RootCollect = std::get_if<RootCollectEntry>(&Rule.Collect);
		const NodeCollectEntry *const NodeCollect = std::get_if<NodeCollectEntry>(&Rule.Collect);
		// 末尾式と暗黙返戻を変えない追加は，別の関数の束縛・返戻区分・終端を変えない
		const bool IsBatch = Rule.CanBatchReturnCompletion && !Syntax.GetRules().IsTailExprVal && std::none_of(
			Targets.begin(),
			Targets.end(),
			[&](const NodeId Target) -> bool {
				Syntax.Charge(1);
				// 暗黙返戻の明示化を持ち得る対象かの返戻
				return Syntax.Is(Target, Role::ImplicitReturnCandidate);
			}
		);
		RewritePlan Plan;
		for(const NodeId Target : Targets) {
			Syntax.Charge(1);
			if(!IsBatch) Plan.Steps.clear();
			const NodeId Parent = Syntax.GetParentOf(Target);
			// 未展開のトークン列は正規化せず，引用本文は外側の引用符だけを正規化する
			if(
				!Syntax.IsRemovedNode(Target) && Syntax.GetNode(Target).Facts.TokenSpacing == TokenSpacingMode::None &&
				(Parent == NoNode || !GetNormalizationBarrierOf(Syntax, Parent).IsQuoted)
			) {
				if(RootCollect) (*RootCollect)(*this, Syntax, Plan);
				else (*NodeCollect)(*this, Syntax, Target, Plan);
			}
			// 最後の読取を終えたインデックスの，次の読取の無い更新の前の解放
			if(!HasFollowingRule && Target == Targets.back()) Syntax.ReleaseRoleIdx();
			// 独立した返戻の判定中に書き換えず，構文順の採番と範囲の表を共有する
			if(Plan.Steps.empty() || IsBatch && Target != Targets.back()) continue;
			// 全対象の判定を終えた返戻の計画では，後続で読まない制御の要約を失効走査の前に解放する
			if(IsBatch) Syntax.EditNormalizationFacts().FlowFacts.reset();
			const RewriteTrace Part = Syntax.Apply(Plan);
			Trace.Transfers.insert(Trace.Transfers.end(), Part.Transfers.begin(), Part.Transfers.end());
		}
		// 規則内で共有した制御の流れと括弧の事実の解放
		Syntax.EditNormalizationFacts().FlowFacts.reset();
		Syntax.EditNormalizationFacts().ReturnAttributeProtection.clear();
		Syntax.EditNormalizationFacts().Parens.reset();
		if(RuleWork) RuleWork->emplace_back(Rule.Name, Syntax.GetWorkAccount()->Used - Before);
		// 終了
		return;
	}

	/**
	 * 構文の正規化関数（整形仕様 3.4，第５章）
	 * 投影済のノード表だけを書き換え，再解析しない
	 * 固定の規則順に各対象を１度だけ評価し，独立した返戻の追加だけは構文順に纏めて適用する
	 * 計算量：対象選択・意味判定・書換・アンカー合成の総費用 Q に対し O(Q)
	 * @param Syntax 構文モデル
	 * @return アンカーの移譲の記録
	 * @throws FormatLimitExceeded 作業予算を超えるか，必要な生成形式が欠ける場合
	 * @throws std::bad_alloc 照会・生成又は書換で用いるメモリを確保出来ない場合
	 * @throws std::length_error テキスト又はメモリの要素数上限を超える場合
	 * @throws std::out_of_range 照会するノード・テキストの位置又は事実のキーが範囲外の場合
	 */
	RewriteTrace SyntaxNormalizer::Normalize(SyntaxModel &Syntax) {
		// 固定の規則表と意味前提からの役割集合の初期化関数
		// 計算量：規則表の役割数 R に対し O(R)
		// @return 正規化が照会する役割の集合
		// @throws std::bad_alloc 規則表のメモリを確保出来ない場合
		// @throws std::length_error 規則表の要素数上限を超える場合
		static const RoleSet WantedRoles = []() -> RoleSet {
			RoleSet Selected;
			for(const RuleDescriptor &Rule : GetRules()) {
				for(const Role Wanted : Rule.TargetRoles) Selected.set(static_cast<size_t>(Wanted));
				for(const Role Wanted : Rule.ChildRoles) Selected.set(static_cast<size_t>(Wanted));
			}
			for(
				const Role Wanted :
				{
					Role::ArgReturn,
					Role::BindingName,
					Role::BodyAmbiguousControl,
					Role::CallExpr,
					Role::Export,
					Role::FlowReturn,
					Role::GeneratorMarker,
					Role::GotoStmt,
					Role::Identifier,
					Role::ImportStmt,
					Role::InnerFunctionStmts,
					Role::StmtSeq
				}
			) Selected.set(static_cast<size_t>(Wanted));
			// 正規化が照会する役割の集合の返戻
			return Selected;
		}();
		const std::vector<RuleDescriptor> &Rules = GetRules();
		// 束縛の無い候補の書換はマクロ束縛を新設せず，元の事実の有無を全規則で共有出来る
		const bool HasMacroBindings = std::any_of(
			Syntax.Nodes.begin(),
			Syntax.Nodes.end(),
			[&Syntax](const SyntaxNode &Node) -> bool {
				Syntax.Charge(1);
				// 投影済のマクロ束縛の有無の返戻
				return Node.Facts.Has(ConstraintKind::MacroBinding);
			}
		);
		Syntax.BuildRoleIdx(WantedRoles);
		RewriteTrace Trace;
		for(size_t Idx = 0; Idx < Rules.size(); ++Idx) {
			const size_t Before = RuleWork ? Syntax.GetWorkAccount()->Used : 0;
			try {
				RunRule(Syntax, Rules[Idx], Idx + 1 < Rules.size(), Trace, HasMacroBindings);
			} catch(const FormatLimitExceeded &) {
				if(RuleWork) RuleWork->emplace_back(Rules[Idx].Name, Syntax.GetWorkAccount()->Used - Before);
				throw;
			}
		}
		Syntax.ReleaseFacts();
		Syntax.BuildFollowingIdx();
		Trace.Compose(Syntax);
		// 確定したアンカーの移譲の返戻
		return Trace;
	}
} // namespace Core
