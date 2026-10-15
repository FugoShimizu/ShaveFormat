#include "Formatter.hpp"

#if defined(SHAVEFMT_PERFORMANCE_PROBE)

#include "PerformanceObservation.hpp"

#endif

#include "Core/Canonicalizer.hpp"
#include "Core/CodeLayout.hpp"
#include "Core/CommentPlacer.hpp"
#include "Core/CoreObservation.hpp"
#include "Core/DocCommentSynchronizer.hpp"
#include "Core/SourceProjector.hpp"
#include "Core/SyntaxNormalizer.hpp"
#include "Util/CommentNormalizer.hpp"
#include "Util/FormatLimitExceeded.hpp"
#include "Util/InputNormalizer.hpp"
#include "Util/OutputFinalizer.hpp"
#include "Util/ParseAdapter.hpp"
#include "Util/ParsedTree.hpp"
#include "Util/SyntaxParser.hpp"
#include "Util/SyntaxValidator.hpp"
#include <tree_sitter/api.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(SHAVEFMT_PERFORMANCE_PROBE)

namespace Core {
	// 製品の行分割の入口と計数の観測を結ぶテスト専用の入口
	struct LineBreakerMeasurement {
		static BrokenIR Measure(const LineBreaker &Breaker, FormatIR IR, CoreObservation *const Observation); // 設定済の行幅での選択と計数
	};

	/**
	 * 製品の設定と入力を使い，行分割の実作業量だけを観測する
	 * 計算量：製品の行分割と同じ
	 * @param Breaker 製品の行幅を持つ行分割の文脈
	 * @param IR 幅非依存の中間表現
	 * @param Observation 製品の選択の観測先，観測しない場合は nullptr
	 * @return 製品と同じ選択を確定した結果
	 * @throws FormatLimitExceeded 行分割の予算超過又は内部の不整合の場合
	 * @throws std::exception 行分割から伝わるメモリ・範囲等のエラーの場合
	 */
	BrokenIR LineBreakerMeasurement::Measure(const LineBreaker &Breaker, FormatIR IR, CoreObservation *const Observation) {
		// 設定済の行幅で選択し，計上済の作業量を取得する
		LineBreaker::WorkMeter Work;
		BrokenIR Result = Breaker.ResolveMeasured(std::move(IR), &Work, Observation);
		PerformanceObservation::Pipeline.Wrapping += Work.Used;
		PerformanceObservation::Pipeline.WrappingLimit += Work.Limit;
		// 製品の行分割の結果の返戻
		return Result;
	}
} // namespace Core

#endif

/**
 * 子の見送時に原文保持を許す理由の判定関数（整形仕様 10.4 の「可」の理由だけを列挙する）
 * 計算量：O(1)
 * @param Reason 子の見送理由（失敗の分類には適用しない）
 * @return 同じ理由の警告を伴って子の原文を保持出来るか
 */
bool Formatter::CanPreserveEmbedded(const FailureReason Reason) {
	// 固定の理由表の照合
	switch(Reason.Val) {
	case FailureReason::HeaderDialect:
	case FailureReason::MarkupDoc:
	case FailureReason::ParserUnreadable:
	case FailureReason::InputSyntaxError:
	case FailureReason::UnrecognizedSyntax:
	case FailureReason::StrayBackslash:
	case FailureReason::BraceAbsorbed:
	case FailureReason::MarkupCommentsUnsupported:
	case FailureReason::NonAssocOperators:
	case FailureReason::UnknownOperatorPrec:
	case FailureReason::ErrorZoneLost:
		// 子の原文保持を許す理由の返戻
		return true;
	default:
		// リソース上限等，親へ伝える理由の返戻
		return false;
	}
}

/**
 * 破損の区分と対処の付加診断の取得関数
 * 計算量：通知長 L に対し O(L)
 * @param Issue 入力エラーが確定したか，未識別構文か
 * @return 部分整形の理由と利用者が確認する条件
 * @throws std::bad_alloc 通知の領域を確保出来ない場合
 */
std::string Formatter::GetParseNotice(const ParseIssue Issue) {
	// 診断の区分の取得
	const FailureReason Reason =
	Issue == ParseIssue::InputSyntax ? FailureReason::InputSyntaxError : FailureReason::UnrecognizedSyntax;
	// 見送と部分整形で共有する区分と対処の返戻
	return std::string(Reason.GetRemedy());
}

/**
 * 中核の工程の適用関数（整形仕様 3.4）
 * 投影 → 構文の正規化とアンカーの移譲 → 中間表現 → 行分割 → インデントの具体化 → ドキュメントの同期 → グループの所属の確定 → 本文の正規化 → コメントの配置 → 合成・終端の順に，各段は前の段の結果だけを受け取る
 * 計算量：入力・出力長 N，ノード数 V，コメント数 C に依る各工程の費用 T_i に対し O(Σ T_i)
 * @param Profile 入力で確定した文法
 * @param Input 適応済の入力と修復前のテキスト
 * @param Parsed 受理した解析結果（投影の後にメモリを空ける）
 * @param Ctx 投影が組み立てた検証データの格納先
 * @param Code 確定した変更不能なコードの出力の格納先（検証が空行の照合基準に使う）
 * @param Expectation 行分割の選択を含む検証用の期待構文の格納先
 * @param Comments 同期・本文正規化済の期待コメントの格納先（検証がコメントの比較元に使う）
 * @param FinalText 終端処理を済ませた最終テキストの格納先
 * @param Observed 検査指定時の中核の読取専用な観測の格納先
 * @param Diagnostics Main が表示する中核の診断の格納先
 * @param Embedded 子のコードの所有経路の格納先（埋込以外は nullptr）
 * @throws FormatLimitExceeded コメントのノードが中核の工程へ残る場合，アンカーの不整合又は行分割の作業上限を超えた場合，出力が大きさの上限を超える場合，子の所有経路又は配置枠の対応が無い場合
 * @throws std::bad_alloc 投影・中間表現・配置・コメント又は出力のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 */
void Formatter::RunCore(
	const GrammarProfile &Profile,
	ParseInput &Input,
	std::optional<ParsedTree> &Parsed,
	std::optional<ValidationContext> &Ctx,
	std::shared_ptr<const LaidOutCode> &Code,
	std::optional<Core::OutputExpectation> &Expectation,
	std::optional<Core::CommentRepository> &Comments,
	std::string &FinalText,
	std::string &Observed,
	std::vector<std::string> &Diagnostics,
	EmbeddedOutput *const Embedded
) const {
	// 投影と構文の正規化に依る中間表現の構築
	Core::ProjectionResult Projection = Core::SourceProjector::Project(*Parsed, Profile, Input);
	// 子の検証期待と文脈は投影へ移した経路だけで保持し，適応側の共有を此処で手放す
	Input.Owned = Core::OwnedInputRoutes();
	Ctx.emplace(std::move(Projection.Validation));
	#if defined(SHAVEFMT_PERFORMANCE_PROBE)
	PerformanceObservation::Pipeline.Relations += Parsed->GetRelationWork();
	PerformanceObservation::Pipeline.CastRepairVisits += Parsed->GetCastRepairWork();
	#endif
	// 投影を終えた入力の解析結果は以後の工程が参照しない為，メモリを空ける
	Parsed.reset();
	std::optional<Core::CoreObservation> Observation;
	if(ShouldObserve) Observation.emplace(Projection.Syntax);
	else if(ShouldObserveSelections) Observation.emplace();
	{
		const Core::RewriteTrace Trace = Core::SyntaxNormalizer().Normalize(Projection.Syntax);
		if(ShouldObserve) Observation->ObserveSyntax(Projection.Syntax, Trace);
		Projection.Comments.Remap(Trace);
	} // 移譲の表を使い終え，中間表現と重ねて保持しない
	const std::shared_ptr<Core::SyntaxModel> Normalized = std::make_shared<Core::SyntaxModel>(std::move(Projection.Syntax));
	Core::FormatIR IR = Core::Canonicalizer::Build(Normalized);
	// 構文の正規化と中間表現の構築の予算を閉じ，後の工程の構文読取を此の予算へ数えない（整形仕様 9.2）
	IR.CloseWork();
	#if defined(SHAVEFMT_PERFORMANCE_PROBE)
	PerformanceObservation::Pipeline.Structure += Normalized->GetWorkAccount()->Used;
	PerformanceObservation::Pipeline.Base += Normalized->GetWorkAccount()->Base;
	PerformanceObservation::Pipeline.Limit += Normalized->GetWorkLimit();
	#endif
	// 行分割とコード配置の確定
	if(ShouldObserve) Observation->ObserveFormat(IR);
	#if defined(SHAVEFMT_PERFORMANCE_PROBE)
	Core::BrokenIR Broken = Core::LineBreakerMeasurement::Measure(Breaker, std::move(IR), Observation ? &*Observation : nullptr);
	#else
	Core::BrokenIR Broken = Breaker.Resolve(std::move(IR), Observation ? &*Observation : nullptr);
	#endif
	if(ShouldObserve) Observation->ObserveBroken(Broken);
	Code = std::make_shared<const LaidOutCode>(Core::CodeLayout::Materialize(Broken));
	if(ShouldObserve) Observation->ObserveLayout(*Code);
	// 解決済の要素は配置へコピーし終えた為，期待結果だけを残してメモリを空ける
	Expectation.emplace(std::move(Broken.Expectation));
	Broken = Core::BrokenIR();
	// ドキュメントコメントと本文の正規化及び配置
	Core::DocCommentSynchronizer::Synchronize(*Normalized, Projection.Comments);
	Core::DocCommentSynchronizer::ConfirmGroups(*Code, Projection.Comments);
	CommentNormalizer::Normalize(Projection.Comments);
	// 同期が使った束縛と制御の共有事実は，構造・名前・期待コメントの照合では参照しない
	Normalized->ReleaseFacts();
	Comments.emplace(std::move(Projection.Comments));
	#if defined(SHAVEFMT_PERFORMANCE_PROBE)
	PerformanceObservation::Pipeline.PlacementTargets += Comments->GetRecords().size();
	#endif
	// 全てのコメントを配置枠へ割り当てた２入力の合成と終端処理
	if(!Embedded) {
		FinalText = OutputFinalizer::Finalize(
			OutputDocument::Compose(Code, Core::CommentPlacer::Place(*Code, *Comments, ShouldWarn ? &Diagnostics : nullptr))
		);
	}
	if(ShouldObserve) {
		Observed = Observation->Take();
		Observed.append("[work] structure " + std::to_string(Normalized->GetWorkAccount()->Used) + "\n");
	}
	if(Embedded) Embedded->Code = Code;
	// 終了
	return;
}

/**
 * 中核の出力の観測のテキストの取得関数
 * 検査用の指定の時だけ，コメントを置かない配置計画で合成・終端処理したテキストを行毎に１６進で表す（整形仕様 10.4, 10.5, 11.1）
 * 整形結果・検証・分類は変えず，デコードすると再入力出来るテキストにする
 * 計算量：コードの断片数 F，テキストの長さ N に対し O(F + N)
 * @param Code `CodeLayout` が確定した変更不能なコードの出力
 * @return 標準エラーへ出す観測の行（検査用の指定が無ければ空）
 * @throws FormatLimitExceeded 中核のテキストが出力の大きさの上限を超える場合
 * @throws std::bad_alloc 合成結果又は観測のテキストの確保に失敗した場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
std::string Formatter::ObservedCore(const std::shared_ptr<const LaidOutCode> &Code) const {
	// 検査用の指定の無い時の返戻
	if(!ShouldObserve) return {};
	// コメントを除いた配置結果の取得
	const std::string Text = OutputFinalizer::Finalize(OutputDocument::CodeOnly(Code));
	std::string Observed;
	// 行毎の出力（制御文字等を表示へ出さず，テキストを其の儘再入力出来る様にバイト毎の１６進で表す，整形仕様 10.4）
	for(size_t Start = 0; Start < Text.size();) {
		const size_t End = std::min(Text.find('\n', Start), Text.size());
		Observed.append("[layout] ");
		for(size_t Pos = Start; Pos < End; ++Pos) {
			const unsigned char Byte = static_cast<unsigned char>(Text[Pos]);
			Observed.push_back("0123456789ABCDEF"[Byte >> 4]);
			Observed.push_back("0123456789ABCDEF"[Byte & 0XF]);
		}
		Observed.push_back('\n');
		Start = End + 1;
	}
	// 観測の行の返戻
	return Observed;
}

/**
 * 整形結果の確定関数（最終テキストの独立した検証・静的解析・入力との比較）
 * 計算量：入力・出力長 N，検証費用 V，親子の静的解析費用 L，警告位置のマッピング費用 M に対し O(N + V + L + M)
 * @param Profile 入力で確定した文法
 * @param Ctx 所有権を引き取り，検証後に解放する入力側のデータ（エラー領域・入力の破損数・読めなかったか）
 * @param Code `CodeLayout` が確定した変更不能なコードの出力（空行の照合基準で，検証へ渡して手放す）
 * @param Src 正規化した入力（整形結果との比較に用いる）
 * @param Comments 所有権を引き取り，検証後に解放する同期・本文正規化済の期待コメント
 * @param Expectation 所有権を引き取り，検証後に解放する正規化後の構文と選択した表現
 * @param FinalText 終端処理後の最終テキスト
 * @param ShouldLint false で規約検査を飛ばす
 * @param IsFormChanged 改行様式が正規化で変わった場合 true（其れだけの違反も変更として扱う）
 * @param IsPartialNoticed 最初の破損位置へ部分整形の通知を加えるか（許容したエラー領域を保って部分整形する入力で，埋込の中身は要素の位置の通知で表す為に除く）
 * @param InputIssue 入力エラーと未識別構文の区分
 * @param Embeds HTML に埋め込んだコードの整形結果（ドキュメント順，其の他の言語は空）
 * @param Warnings 検出された規約警告と部分整形の通知の格納先
 * @param Diagnostics 区分と対処を１度だけ表示する付加診断の格納先
 * @param Reason 検証が捉えた見送又は失敗の理由の格納先
 * @return 整形結果の種別（`Changed` / `Unchanged` / `Failed`，検証が捉えた見送は `Skipped`）
 * @throws FormatLimitExceeded 検証の構造・所有対応の不変条件に違反する場合
 * @throws std::bad_alloc 検証・静的解析又は警告のメモリを確保出来ない場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 * @throws std::out_of_range 照会する添字又は識別子が表に無い場合
 * @throws std::system_error 呼出先の入出力又は同期が失敗した場合
 */
FormatClassification Formatter::FinalizeOutput(
	const GrammarProfile &Profile,
	ValidationContext Ctx,
	std::shared_ptr<const LaidOutCode> &&Code,
	const std::string &Src,
	Core::CommentRepository Comments,
	Core::OutputExpectation Expectation,
	const std::string &FinalText,
	const bool ShouldLint,
	const bool IsFormChanged,
	const bool IsPartialNoticed,
	const ParseIssue InputIssue,
	const std::vector<EmbeddedCode> &Embeds,
	std::vector<LintWarning> &Warnings,
	std::vector<FormatDiagnostic> &Diagnostics,
	FailureReason &Reason
) const {
	// 最終テキストの独立した検証（整形仕様 8.1）
	#if defined(SHAVEFMT_PERFORMANCE_PROBE)
	const PerformanceObservation::ParserWork Before = PerformanceObservation::Parsing;
	#endif
	SyntaxValidator::ValidationResult Validation =
	Validator.Validate(Profile, std::move(Code), FinalText, std::move(Expectation), Comments, std::move(Ctx));
	#if defined(SHAVEFMT_PERFORMANCE_PROBE)
	PerformanceObservation::Pipeline.ValidationCalls += PerformanceObservation::Parsing.Calls - Before.Calls;
	PerformanceObservation::Pipeline.ValidationBytes += PerformanceObservation::Parsing.InputBytes - Before.InputBytes;
	#endif
	if(Validation.GetStatus() != SyntaxValidator::ValidationStatus::Verified) {
		Reason = Validation.GetReason();
		// 検証が捉えた見送又は失敗の返戻
		return Validation.GetStatus() == SyntaxValidator::ValidationStatus::Failed ?
		FormatClassification::Failed :
		FormatClassification::Skipped;
	}
	// 入力側の照合期待は最終候補の事実と独立して居り，静的解析へ持ち込まない
	Expectation = Core::OutputExpectation();
	Comments = Core::CommentRepository();
	Core::ValidatedOutput &Final = *Validation.GetOutput();
	#if defined(SHAVEFMT_PERFORMANCE_PROBE)
	PerformanceObservation::ObserveValidated(Final);
	#endif
	// 静的解析は検証済の候補に専用の事実を補い，本文を変更せず警告を求める
	if(ShouldLint) Warnings = LintAnalyzer::Analyze(Final);
	// 入力破損の最初の破損位置での部分整形の通知
	if(IsPartialNoticed) {
		Warnings.push_back({ Final.Locations.FirstBroken.Row, Final.Locations.FirstBroken.Column, std::string(SyntaxErrorWarning) });
		Diagnostics.push_back({ Final.Locations.FirstBroken, GetParseNotice(InputIssue) });
	}
	// 埋込コードの通知を要素の位置へ，中身の静的解析の警告を HTML の最終テキストの位置へコピーする（整形仕様 4.4, 8.4）
	for(size_t Ordinal = 0; Ordinal < Embeds.size(); ++Ordinal) {
		if(const EmbeddedCode &Embed = Embeds[Ordinal]; !Embed.Note.empty()) {
			const Core::OutputPoint Element = Final.Locations.Embedded[Ordinal];
			Warnings.push_back({ Element.Row, Element.Column, Embed.Note });
			if(!Embed.Diagnostic.empty()) Diagnostics.push_back({ Element, Embed.Diagnostic });
		}
	}
	// 子の最終候補の投影の静的解析と，親の最終候補上の実位置へのマッピング
	if(ShouldLint) for(SyntaxValidator::ValidatedChild &Child : Validation.GetChildren()) {
		for(const LintWarning &Warning : LintAnalyzer::Analyze(Child.Output)) {
			const std::vector<uint32_t> &Starts = Child.Output.Locations.LineStarts;
			const Core::OutputPoint Point = Final.Locations.GetPointAt(Child.Start + Starts[Warning.Row] + Warning.Column);
			Warnings.push_back({ Point.Row, Point.Column, Warning.Msg, Warning.Rule, Warning.Target, Warning.Vals });
		}
	}
	// 終端契約を満たす最終テキストと正規化した入力の比較（解析用のマーカーは投影で本来のトークンへデコード済）の結果の返戻
	return !IsFormChanged && FinalText == Src ? FormatClassification::Unchanged : FormatClassification::Changed;
}

/**
 * HTML に埋め込んだコードの整形関数（整形仕様 4.4，単独のコードと同じ工程へ通し，結果を親の原文へ書き戻さず所有するノードの中身とする）
 * 中身の読めないコードは其の儘残し，部分整形の通知の理由を控える
 * 計算量：親のノード数 V，埋込数 E，中身の総長 M，各中身の整形費用 T_i に対し O(V + E + M + Σ T_i)
 * @param Src 解析済の HTML（整形した中身を埋込の中身のノードで引ける様に控える）
 * @param Embeds ドキュメント順の埋込毎の整形結果の格納先
 * @param Owned 子の３所有経路を投影へ渡す入力の所有者
 * @param Result 中身の整形が失敗した場合の理由の格納先
 * @return 子の見送又は失敗を親へ伝える場合は其の分類，成功時は原文保持又は子の部分整形を含めば `Partial`，他は `Changed`
 * @throws std::bad_alloc 埋込のテキスト・通知又は所有記録の確保に失敗した場合
 * @throws std::invalid_argument 呼出先の引数の契約を満たさない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
FormatClassification Formatter::FormatEmbeddedCode(
	ParsedTree &Src,
	std::vector<EmbeddedCode> &Embeds,
	Core::OwnedInputRoutes &Owned,
	FormatResult &Result
) const {
	// 対象範囲の走査
	bool HasPartial = false;
	for(const ParseAdapter::EmbeddedContent &Content : ParseAdapter::EmbeddedContents(Src)) {
		EmbeddedCode &Embed = Embeds.emplace_back();
		if(Content.Lang == Lang::Unknown) {
			// 未対応内容は親の文法で確定した保持境界が在る場合だけ部分整形する（整形仕様 4.4, 8.3）
			if(!ParseAdapter::HasPreservedEmbeddedEnd(Src, Content)) {
				Result.Reason = FailureReason::ParserUnreadable;
				// 保持境界を確定出来ない入力の見送の返戻
				return FormatClassification::Skipped;
			}
			if(!ts_node_is_null(Content.RawText)) {
				Embed.Note = "Embedded code was left as is: unsupported embedded content";
				HasPartial = true;
			}
			continue;
		}
		// 中身の無い要素の読飛
		if(ts_node_is_null(Content.RawText)) continue;
		// 埋込の中身の単独のコードとしての整形（中身は JS / CSS / JSON で，PHP の後置データを持たない）
		FormatResult Sub;
		Sub.Text = Src.GetView(Content.RawText);
		EmbeddedOutput Inner;
		Sub.Classification = FormatCode(GrammarProfile::Resolve(Content.Lang), Sub.Text, false, Sub, &Inner);
		// 読めない中身は其の儘残し，理由の控え
		if(Sub.Classification == FormatClassification::Skipped && CanPreserveEmbedded(Sub.Reason)) {
			Embed.Note = "Embedded code was left as is: ";
			Embed.Note += Sub.Reason.GetText();
			Embed.Diagnostic = Sub.Reason.GetRemedy();
			HasPartial = true;
			continue;
		}
		// 中身の整形の失敗は HTML 全体の失敗とする（整形仕様 8.3）
		if(
			Sub.Classification != FormatClassification::Changed && Sub.Classification != FormatClassification::Unchanged &&
			Sub.Classification != FormatClassification::Partial
		) {
			Result.Reason = Sub.Reason;
			// 中身の整形が失敗した結果の返戻
			return Sub.Classification;
		}
		HasPartial = HasPartial || Sub.Classification == FormatClassification::Partial;
		Embed.Observed = std::move(Inner.Observed);
		Embed.Normalization = Sub.Normalization;
		// 子のコードだけの所有領域の控え（コメントと検証期待は別の経路）
		Src.SetOwnedRegion(Content.RawText, ParseAdapter::OwnEmbeddedOutput(Inner.Code));
		// エラー領域を保った中身の部分整形の通知（領域の行は配置の段が原文の儘保つ）
		if(Sub.Classification == FormatClassification::Partial && Sub.InputIssue != ParseIssue::None) {
			Embed.Note = "Embedded code: " + std::string(SyntaxErrorWarning);
			Embed.Diagnostic = GetParseNotice(Sub.InputIssue);
		}
		// 親の配置後の実候補から改めて検証・静的解析する子の所有経路の控え
		Owned.Add(
			*Inner.Profile,
			std::move(Inner.Code),
			std::move(Inner.Expectation),
			std::move(Inner.Comments),
			std::move(Inner.Ctx),
			Embeds.size() - 1
		);
	}
	// 子の原文保持又は部分整形を含む処理状態の返戻
	return HasPartial ? FormatClassification::Partial : FormatClassification::Changed;
}

/**
 * コードの整形関数
 * 計算量：テキストの総長 N，各工程・子整形の総費用 T，警告数 W，最大警告長 L に対し O(N + T + (L + 1) W log²(W + 1))
 * @param Profile 入力で選んだ文法
 * @param Src 整形対象のソース（呼出側が入力上限を確かめたテキストで，検証を通った整形結果が原文と異なる時だけ其の結果で上書きされる）
 * @param ShouldLint false で規約検査を飛ばす
 * @param Result 警告・見送理由・入力の構文破損の格納先
 * @param Embedded HTML に埋め込んだ子の出力の格納先（中核の出力の観測は埋込先の出力だけで行う，其の他は nullptr）
 * @param IsAcceptedInput 元の入口条件で両上限以内と受理済か（埋込の完成候補は独立検証でも受理する）
 * @return 整形結果の種別（末尾改行の過不足だけでも `Changed`，許容したエラー領域を保った成功は `Partial`，整形仕様 8.3）
 */
FormatClassification Formatter::FormatCode(
	const GrammarProfile &Profile,
	std::string &Src,
	const bool ShouldLint,
	FormatResult &Result,
	EmbeddedOutput *const Embedded,
	const bool IsAcceptedInput
) const {
	// 原文を保つ見送結果の構築
	const auto Skip = [&Result](const FailureReason Reason) -> FormatClassification {
		// 対象の状態の更新
		Result.Reason = Reason;
		// 原文を書き換えずに見送る結果の返戻
		return FormatClassification::Skipped;
	};
	// メモリ・スレッド不足時は原文保持で失敗報告（原文を書き換えるのは検証を通った整形結果の格納だけ）
	try {
		// 入力の受理の判定と表現の正規化（整形仕様 3.4 の Normalize，原文は書き換えない）
		InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, Src, !Embedded, IsAcceptedInput);
		if(Normalized.Reason == FailureReason::WorkingMemoryExceeded) {
			Result.Reason = Normalized.Reason;
			// 受理走査の作業域超過の失敗の返戻
			return FormatClassification::Failed;
		}
		Result.ReasonPoint = Normalized.ReasonPoint;
		// 受理しない入力の見送の返戻
		if(Normalized.Reason != FailureReason::None) return Skip(Normalized.Reason);
		// 解析用の入力の適応（整形仕様 3.4 の Adapt，通常の見送とパーサー故障を分ける）
		ParseInput Adapted = ParseAdapter::Adapt(Profile, Normalized);
		Result.ReasonPoint = Normalized.GetPoint(Adapted.IssueOffset);
		// 通常見送の返戻
		if(Adapted.Decision == ParseDecision::Skip) return Skip(Adapted.Failure);
		if(Adapted.Decision == ParseDecision::Fail) {
			Result.Reason = Adapted.Failure;
			// パーサー故障の返戻
			return FormatClassification::Failed;
		}
		// 後続工程の共通ソースと確定した言語
		const GrammarProfile &Effective = *Adapted.Profile;
		// 構文木の１回走査に依る受理しない構文エラーの検査（整形仕様 3.4）
		std::optional<ParsedTree> Parsed(SyntaxParser::Parse(Effective, Adapted));
		const bool HasInputError = Parsed->HasOriginalInputError();
		Result.InputIssue = Parsed->GetParseIssue();
		const ParseDecision Decision = Parsed->GetParseDecision();
		if(Decision == ParseDecision::Skip) {
			Result.ReasonPoint = Normalized.GetPoint(Parsed->GetIssueOffset());
			// 解析で得た位置を伴う見送の返戻
			return Skip(Parsed->GetParseFailure());
		}
		if(Decision == ParseDecision::Fail) {
			Result.Reason = Parsed->GetParseFailure();
			// 受理失敗の返戻
			return FormatClassification::Failed;
		}
		// HTML に埋め込んだコードの単独のコードと同じ工程での整形（整形仕様 4.4）
		std::vector<EmbeddedCode> Embeds;
		Core::OwnedInputRoutes Owned;
		bool HasEmbeddedPartial = false;
		if(Parsed->GetReading().HasEmbeddedScripts) {
			const FormatClassification EmbeddedClass = FormatEmbeddedCode(*Parsed, Embeds, Owned, Result);
			// 子の見送又は失敗を親へ伝える処理状態の返戻
			if(EmbeddedClass == FormatClassification::Skipped || EmbeddedClass == FormatClassification::Failed) return EmbeddedClass;
			HasEmbeddedPartial = EmbeddedClass == FormatClassification::Partial;
		}
		Adapted.Owned = std::move(Owned);
		// 中核処理の適用と式結合の最終検査
		std::optional<ValidationContext> Ctx;
		std::shared_ptr<const LaidOutCode> Code;
		std::optional<Core::OutputExpectation> Expectation;
		std::optional<Core::CommentRepository> Comments;
		// 終端処理を済ませた最終テキスト（入力の解析結果とは別の値）
		std::string FinalText, Observed;
		std::vector<std::string> CoreDiagnostics;
		RunCore(Effective, Adapted, Parsed, Ctx, Code, Expectation, Comments, FinalText, Observed, CoreDiagnostics, Embedded);
		Ctx->EntryProfile = &Profile;
		Ctx->IsEmbedded = Embedded != nullptr;
		for(std::string &Diagnostic : CoreDiagnostics) Result.Diagnostics.push_back({ std::nullopt, std::move(Diagnostic) });
		// 子は所有経路だけを渡し，親が具体化した最終候補を１度検証する
		if(Embedded) {
			Embedded->Profile = &Effective;
			Embedded->Expectation = std::make_shared<Core::OutputExpectation>(std::move(*Expectation));
			Embedded->Comments = std::make_shared<const Core::CommentRepository>(std::move(*Comments));
			Embedded->Ctx = std::make_shared<ValidationContext>(std::move(*Ctx));
			Observed.append(ObservedCore(Code));
			Embedded->Observed = std::move(Observed);
			Result.Normalization = Normalized.Changes;
			// 親の最終検証へ渡す子の処理状態の返戻
			return HasInputError || HasEmbeddedPartial ? FormatClassification::Partial : FormatClassification::Changed;
		}
		// 検証が捉えた見送の理由
		FailureReason ValidationReason = FailureReason::None;
		// 中核の出力の読取専用な観測（整形仕様 10.5，配置を検証へ渡して手放す前にテキストを作り，検証を通った時だけ出す）
		Observed.append(ObservedCore(Code));
		for(size_t Idx = 0; Idx < Embeds.size(); ++Idx) if(!Embeds[Idx].Observed.empty()) {
			Observed.append("[embedded] " + std::to_string(Idx) + " begin\n");
			Observed.append(Embeds[Idx].Observed);
			Observed.append("[embedded] " + std::to_string(Idx) + " end\n");
		}
		// 最終テキストの独立した検証・静的解析・入力との比較に依る整形結果の確定
		const FormatClassification Outcome = FinalizeOutput(
			Effective,
			std::move(*Ctx),
			std::move(Code),
			Normalized.Text,
			std::move(*Comments),
			std::move(*Expectation),
			FinalText,
			ShouldLint,
			Normalized.IsFormChanged,
			HasInputError,
			Result.InputIssue,
			Embeds,
			Result.Warnings,
			Result.Diagnostics,
			ValidationReason
		);
		// 保護領域を保てない事を検証で捉えた部分整形の見送（整形仕様 8.3 は此れをフォーマッターの不具合として通知しない）
		if(Outcome == FormatClassification::Skipped) {
			Result.Warnings.clear();
			Result.Diagnostics.clear();
			Result.Reason = ValidationReason;
			// 検証が捉えた理由での返戻（入力の段階の見送と区別する）
			return FormatClassification::Skipped;
		}
		if(Outcome == FormatClassification::Failed) {
			Result.Diagnostics.clear();
			Result.Reason = ValidationReason;
			// 意味を壊す整形を差し戻した事の返戻
			return Outcome;
		}
		// 静的解析の警告と部分整形の通知を最終テキストの位置と警告一覧の行順に並べる（整形仕様 8.4, 10.4）
		std::stable_sort(Result.Warnings.begin(), Result.Warnings.end());
		Result.Normalization = Normalized.Changes;
		for(const EmbeddedCode &Embed : Embeds) {
			InputNormalization &Changes = Result.Normalization;
			Changes.HasFoldedCrLf = Changes.HasFoldedCrLf || Embed.Normalization.HasFoldedCrLf;
			Changes.HasNormalizedCrOnly = Changes.HasNormalizedCrOnly || Embed.Normalization.HasNormalizedCrOnly;
			Changes.HasNormalizedUnicodeSeparator =
			Changes.HasNormalizedUnicodeSeparator || Embed.Normalization.HasNormalizedUnicodeSeparator;
			Changes.HasMixedNewlines = Changes.HasMixedNewlines || Embed.Normalization.HasMixedNewlines;
		}
		const FormatClassification Classification = HasInputError || HasEmbeddedPartial ? FormatClassification::Partial : Outcome;
		if(!Observed.empty()) Observed.append("[classification] " + std::to_string(static_cast<unsigned>(Classification)) + "\n");
		Result.Observed = std::move(Observed);
		// 送出し得る通知の構築を終えた検証済のテキストの変更有無の判定と反映
		if(Outcome == FormatClassification::Changed) Src = std::move(FinalText);
		// 検証済の完全整形又は部分整形の結果の返戻
		return Classification;
	} catch(const FormatLimitExceeded &E) {
		// 制限超過時の原文復元と結果分類
		Result.Warnings.clear();
		Result.Diagnostics.clear();
		Result.Reason = { E.Kind, E.Reason };
		// リソース上限は見送，到達不能防壁と保持契約の不成立は失敗として原文を返す分類の返戻
		return E.Kind == FailureReason::LimitExceeded ? FormatClassification::Skipped : FormatClassification::Failed;
	} catch(const std::exception &E) {
		Result.Warnings.clear();
		Result.Diagnostics.clear();
		// 途中の段が控えた理由をリソース枯渇又は内部のエラーの通知へ持ち込まない
		Result.Reason = FailureReason::FromException(E);
	}
	// 例外に依って整形を完了出来なかった事の返戻
	return FormatClassification::Failed;
}

/**
 * ソース整形関数
 * 計算量：入力長 N，文法選択と整形の費用 T に対し O(N + T)
 * @param RawSrc 原文（結果は `FormatResult::Text` が持つ）
 * @param Lang 呼出側で確定した対応言語
 * @param ShouldLint false で規約検査を飛ばす
 * @param IsAcceptedInput 同じ原文と入口条件で両上限以内と受理済か（独立した呼出は此処で受理する）
 * @return 整形結果と診断情報（末尾改行の過不足だけでも `Changed`，許容したエラー領域を保った成功は `Partial`，整形仕様 8.3）
 * @throws std::bad_alloc 整形の例外境界へ入る前の原文のコピーの確保に失敗した場合
 * @throws std::length_error 原文のコピーがテキストの保持領域の上限を超える場合
 */
FormatResult Formatter::FormatAccepted(
	const std::string &RawSrc,
	const ::Lang Lang,
	const bool ShouldLint,
	const bool IsAcceptedInput
) const {
	// 結果の格納先
	FormatResult Result;
	std::string &Src = Result.Text;
	Src = RawSrc;
	try {
		const GrammarProfile &Profile = GrammarProfile::Resolve(Lang);
		Result.Classification = FormatCode(Profile, Src, ShouldLint, Result, nullptr, IsAcceptedInput);
	} catch(const std::exception &E) {
		// 予期しない例外の失敗分類
		Result.Classification = FormatClassification::Failed;
		Result.Reason = FailureReason::FromException(E);
	}
	// 整形結果の返戻
	return Result;
}

/**
 * コンストラクタ（`LineBreaker` の受取）
 * 行幅は此の器だけが知り，整形の途中で変わらない
 * 計算量：O(1)
 * @param LineBreaking 行幅を持つ `LineBreaker`
 * @param Observe 中核の読取専用な観測の指定
 * @param Warn コメント配置の補助診断の指定
 * @param Diagnose 独立検証の構造の差の補助診断の指定
 */
Formatter::Formatter(
	const Core::LineBreaker &LineBreaking,
	const bool Observe,
	const bool Warn,
	const bool Diagnose
) : ShouldObserveSelections(Observe || Diagnose),
ShouldObserve(Observe),
ShouldWarn(Warn),
Validator(Diagnose),
Breaker(LineBreaking) {}

/**
 * 原文・言語・静的解析設定を受け取るフォーマッターの入口
 * 計算量：整形と検証の総費用 Q に対し O(Q)
 * @param RawSrc 原文
 * @param Lang 元の入口の言語条件
 * @param ShouldLint 静的解析を行うか
 * @return 整形の本文・分類・通知
 * @throws std::bad_alloc 原文の返戻領域を確保出来ない場合
 * @throws std::length_error 本文又は作業列の要素数上限を超える場合
 */
FormatResult Formatter::Format(const std::string &RawSrc, const ::Lang Lang, const bool ShouldLint) const {
	// 独立した入口の整形結果の返戻
	return FormatAccepted(RawSrc, Lang, ShouldLint, false);
}
