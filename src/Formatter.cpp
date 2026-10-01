#include "Formatter.hpp"
#include "Core/Canonicalizer.hpp"
#include "Core/CodeLayout.hpp"
#include "Core/CommentPlacer.hpp"
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
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

/**
 * HTML に埋め込んだコードの整形関数（整形仕様 4.4，単独のコードと同じ工程へ通し，結果を親の原文へ書き戻さず所有する節点の中身とする）
 * 中身の読めないコードは其の儘残し，部分整形の通知の理由を控える
 * 計算量：埋込の中身の長さの和 M に対し，其々の整形と同じ O(M)
 * @param Src 解析済の HTML（整形した中身を埋込の中身の節点で引ける様に控える）
 * @param ShouldLint false で中身の規約検査を飛ばす（中身の検証済の出力を検査し，位置は HTML の最終字面へ後で写す）
 * @param Embeds 文書順の埋込毎の整形結果の格納先
 * @param Result 中身の整形が失敗した場合の理由の格納先
 * @return 中身の整形の失敗を HTML 全体の結果とする場合は其の種別，其れ以外は `Changed`
 */
FormatClassification Formatter::FormatEmbeddedCode(
	ParsedTree &Src,
	const bool ShouldLint,
	std::vector<EmbeddedCode> &Embeds,
	FormatResult &Result
) const {
	FormatClassification Failure = FormatClassification::Changed;
	for(const ParseAdapter::EmbeddedContent &Content : ParseAdapter::EmbeddedContents(Src)) {
		EmbeddedCode &Embed = Embeds.emplace_back();
		// 中身の無い要素と，既に失敗した後の読飛
		if(ts_node_is_null(Content.RawText) || Failure != FormatClassification::Changed) continue;
		// 埋込の中身の単独のコードとしての整形（中身は JS / CSS / JSON で，PHP の後置データを持たない）
		FormatResult Sub;
		Sub.Text = Src.View(Content.RawText);
		EmbeddedOutput Inner;
		const Lang Selected = Lang::Get(Content.Language);
		Sub.Classification = FormatCode(GrammarProfile::Resolve(Selected), Sub.Text, Selected, ShouldLint, Sub, &Inner);
		// 読めない中身は其の儘残し，理由の控え
		if(Sub.Classification == FormatClassification::Skipped) {
			Embed.Note = "Embedded code was left as is: ";
			Embed.Note += Sub.Reason.Text();
			continue;
		}
		// 中身の整形の失敗は HTML 全体の失敗とする（整形仕様 8.3）
		if(
			Sub.Classification != FormatClassification::Changed && Sub.Classification != FormatClassification::Unchanged &&
			Sub.Classification != FormatClassification::Partial
		) {
			Failure = Sub.Classification;
			Result.Reason = Sub.Reason;
			continue;
		}
		// 投影が所有区画とする整形結果と其の逐語の中から始まる行の控え（要素の終わりを動かす整形は採らず中身を其の儘残す）
		const std::shared_ptr<const Core::OwnedRegion> Region = ParseAdapter::OwnEmbeddedOutput(
			Src,
			Content,
			std::move(Sub.Text),
			std::move(Inner.CodeText),
			std::move(Inner.CodeVerbatimLines),
			std::move(Inner.VerbatimLines)
		);
		if(!Region) {
			Embed.Note = "Embedded code was left as is: formatting would move the end of the element";
			continue;
		}
		// 誤り区画を保った中身の部分整形の通知（区画の行は配置の段が原文の儘保つ）
		if(Sub.Classification == FormatClassification::Partial) {
			Embed.Note = "Parser could not read part of the embedded code; formatting is partial";
		}
		// 中身の静的検査の警告（部分整形の通知は要素の位置の通知で表す）と，位置を写す為の中身の字面
		Embed.Warnings = std::move(Sub.Warnings);
		Embed.Text = Region->Text;
	}
	// 整形の結果の返戻
	return Failure;
}

/**
 * 中核の工程の適用関数（整形仕様 3.4）
 * 投影 → 構文の正規化と錨の移譲 → 中間表現 → 行分割 → インデントの具体化 → 文書の同期 → 群の所属の確定 → 本文の正規化 → コメントの配置 → 合成・終端の順に，各段は前の段の結果だけを受け取る
 * 計算量：節点数 V とコメントの数 C に対し O(V + C log C)
 * @param Profile 入力で確定した文法
 * @param Input 適応済の入力と補修前の字面
 * @param Parsed 受理した解析結果（投影の後に記憶域を空ける）
 * @param Context 投影が組み立てた検証資料の格納先
 * @param Code 確定した変更不能なコードの出力の格納先（検証が空行の照合基準に使う）
 * @param Expectation 行分割の選択を含む検証用の期待構文の格納先
 * @param Comments 同期・本文正規化済の期待コメントの格納先（検証がコメントの比較元に使う）
 * @param FinalText 終端処理を済ませた最終字面の格納先
 * @param Embedded 子のコード行とコメントの配置計画及び逐語行の格納先（埋込以外は nullptr）
 * @throws FormatLimitExceeded 注釈の節点が中核の工程へ残る場合，錨の不整合と行分割が収束しない場合，出力が大きさの上限を超える場合，コメントだけの所有区画の開始タグを置いて居ない場合
 */
void Formatter::RunCore(
	const GrammarProfile &Profile,
	const ParseInput &Input,
	std::optional<ParsedTree> &Parsed,
	std::optional<ValidationContext> &Context,
	std::shared_ptr<const LaidOutCode> &Code,
	std::optional<Core::OutputExpectation> &Expectation,
	std::optional<Core::CommentRepository> &Comments,
	std::string &FinalText,
	EmbeddedOutput *const Embedded
) const {
	// 投影と構文の正規化による中間表現の構築
	Core::ProjectionResult Projection = Core::SourceProjector::Project(*Parsed, Profile, Input);
	Context.emplace(std::move(Projection.Validation));
	// 投影を終えた入力の解析結果は以後の工程が参照しない為，記憶域を空ける
	Parsed.reset();
	Projection.Comments.Remap(Core::SyntaxNormalizer().Normalize(Projection.Syntax));
	const std::shared_ptr<const Core::SyntaxModel> Normalized = std::make_shared<Core::SyntaxModel>(std::move(Projection.Syntax));
	Core::FormatIR IR = Core::Canonicalizer::Build(Normalized);
	// 構文の正規化と中間表現の構築の予算を閉じ，後の工程の読取を此の予算へ数えない（整形仕様 9.2）
	IR.CloseWork();
	// 中間表現の構造の段数の上限の確認（整形仕様 9.2）
	IR.CheckNesting();
	// 行分割とコード配置の確定
	Core::BrokenIR Broken = Breaker.Resolve(std::move(IR));
	Code = std::make_shared<const LaidOutCode>(Core::CodeLayout::Materialize(Broken));
	// 解決済の要素は配置へ写し終えた為，期待結果だけを残して記憶域を空ける
	Expectation.emplace(std::move(Broken.Expectation));
	Broken = Core::BrokenIR();
	// 文書コメントと本文の正規化及び配置
	Core::DocCommentSynchronizer::Synchronize(*Normalized, Projection.Comments);
	Core::DocCommentSynchronizer::ConfirmGroups(*Code, Projection.Comments);
	CommentNormalizer::Normalize(Projection.Comments);
	Comments.emplace(std::move(Projection.Comments));
	// 最終字面と，埋込の出力を所有区画として親へ置く為のコメントを除いたコード行（其々の逐語の中から始まる行を伴う）
	FinalText = OutputFinalizer::Finalize(
		OutputDocument::Compose(Code, Core::CommentPlacer::Place(*Code, *Comments)),
		Embedded ? &Embedded->VerbatimLines : nullptr
	);
	if(Embedded) Embedded->CodeText = OutputFinalizer::Finalize(OutputDocument::CodeOnly(Code), &Embedded->CodeVerbatimLines);
	// 終了
	return;
}

/**
 * 整形結果の確定関数（最終字面の独立した検証・静的検査・入力との比較）
 * 計算量：入力の長さ N に対し O(N)
 * @param Profile 入力で確定した文法
 * @param Code `CodeLayout` が確定した変更不能なコードの出力（空行の照合基準で，検証へ渡して手放す）
 * @param Context 検証へ渡す入力側の資料（誤り区画・入力の破損数・読めなかったか）
 * @param Source 正規化した入力（整形結果との比較に用いる）
 * @param FinalText 終端処理後の最終字面
 * @param Expectation 正規化後の構文と選択した表現
 * @param Comments 同期・本文正規化済の期待コメント（検証がコメントの比較元に使う）
 * @param ShouldLint false で規約検査を飛ばす
 * @param IsFormChanged 改行様式が正規化で変わった場合 true（其れだけの違反も変更として扱う）
 * @param IsPartialNoticed 最初の破損位置へ部分整形の通知を加えるか（許容した誤り区画を保って部分整形する入力で，埋込の中身は要素の位置の通知で表す為に除く）
 * @param Embeds HTML に埋め込んだコードの整形結果（文書順，其の他の言語は空）
 * @param Warnings 検出された規約警告と部分整形の通知の格納先
 * @param HasEmbedNote 埋込コードを整形し切れなかった通知を加えたかの格納先
 * @param Reason 検証が捉えた見送又は失敗の理由の格納先
 * @return 整形結果の種別（`Changed` / `Unchanged` / `Failed`，検証が捉えた見送は `Skipped`）
 * @throws FormatLimitExceeded 解析の印が出力に残る場合
 */
FormatClassification Formatter::FinalizeOutput(
	const GrammarProfile &Profile,
	std::shared_ptr<const LaidOutCode> &&Code,
	const ValidationContext &Context,
	const std::string &Source,
	const std::string &FinalText,
	const Core::OutputExpectation &Expectation,
	const Core::CommentRepository &Comments,
	const bool ShouldLint,
	const bool IsFormChanged,
	const bool IsPartialNoticed,
	const std::vector<EmbeddedCode> &Embeds,
	std::vector<LintWarning> &Warnings,
	bool &HasEmbedNote,
	FailureReason &Reason
) {
	// 最終字面の独立した検証（整形仕様 8.1）
	const SyntaxValidator::ValidationResult Validation =
	SyntaxValidator::Validate(Profile, std::move(Code), FinalText, Expectation, Comments, Context);
	if(Validation.Status != SyntaxValidator::ValidationStatus::Verified) {
		Reason = Validation.Reason;
		// 検証が捉えた見送又は失敗の返戻
		return Validation.Status == SyntaxValidator::ValidationStatus::Failed ?
		FormatClassification::Failed :
		FormatClassification::Skipped;
	}
	const Core::ValidatedOutput &Final = *Validation.Output;
	// 規約検査は検証を通った候補の投影と位置を読むだけ（整形仕様 8.4）
	if(ShouldLint) Warnings = LintAnalyzer::Analyze(Final);
	// 入力破損の最初の破損位置での部分整形の通知
	if(IsPartialNoticed) {
		Warnings.push_back({ Final.Locations.FirstBroken.Row, Final.Locations.FirstBroken.Column, std::string(SyntaxErrorWarning) });
	}
	// 埋込コードの通知を要素の位置へ，中身の静的検査の警告を HTML の最終字面の位置へ写す（整形仕様 4.4, 8.4）
	for(size_t Ordinal = 0; Ordinal < Embeds.size() && Ordinal < Final.Locations.Embedded.size(); ++Ordinal) {
		const EmbeddedCode &Embed = Embeds[Ordinal];
		if(!Embed.Note.empty()) {
			const Core::OutputPoint Element = Final.Locations.Embedded[Ordinal].first;
			Warnings.push_back({ Element.Row, Element.Column, Embed.Note });
			HasEmbedNote = true;
		}
		// 埋込コードの警告位置の親文書への写像
		if(Embed.Warnings.empty()) continue;
		std::vector<Core::OutputPoint> Inner;
		for(const LintWarning &Warning : Embed.Warnings) Inner.push_back({ Warning.Row, Warning.Column });
		const std::vector<Core::OutputPoint> Mapped = Final.Locations.EmbeddedPoints(Ordinal, Embed.Text, std::move(Inner));
		for(size_t Index = 0; Index < Mapped.size(); ++Index) {
			Warnings.push_back({ Mapped[Index].Row, Mapped[Index].Column, Embed.Warnings[Index].Message, Embed.Warnings[Index].Rule });
		}
	}
	// 終端契約を満たす最終字面と正規化した入力の比較（解析用の印は投影で本来の字句へ復号済）の結果の返戻
	return !IsFormChanged && FinalText == Source ? FormatClassification::Unchanged : FormatClassification::Changed;
}

/**
 * 中核の出力の観測の字面の取得関数
 * 検査用の指定の時だけ，コメントを置かない配置計画で合成・終端処理した字面を行毎に１６進で表す（整形仕様 10.4, 10.5, 11.1）
 * 整形結果・検証・分類は変えず，復号すると再入力出来る字面にする
 * @param Code `CodeLayout` が確定した変更不能なコードの出力
 * @return 標準エラーへ出す観測の行（検査用の指定が無ければ空）
 */
std::string Formatter::ObservedCore(const std::shared_ptr<const LaidOutCode> &Code) {
	// 検査用の指定の無い時の返戻
	if(const char *const Verify = std::getenv("SHAVEFMT_VERIFY_COMMENT_SEPARATION"); !Verify || !*Verify) return {};
	// コメントを除いた配置結果の取得
	const std::string Text = OutputFinalizer::Finalize(OutputDocument::CodeOnly(Code));
	std::string Observed;
	// 行毎の出力（制御文字等を表示へ出さず，字面を其の儘再入力出来る様にバイト毎の１６進で表す，整形仕様 10.4）
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
 * コードの整形関数
 * @param Profile 入力で選んだ文法
 * @param Source 整形対象のソース（呼出側が入力上限を確かめた字面で，検証を通った整形結果が原文と異なる時だけ其の結果で上書きされる）
 * @param Language 入力指定時の旗を含む言語
 * @param ShouldLint false で規約検査を飛ばす
 * @param Result 警告・見送理由・入力の構文破損の格納先
 * @param Embedded HTML に埋め込んだ子の出力の格納先（中核の出力の観測は埋込先の出力だけで行う，其の他は nullptr）
 * @return 整形結果の種別（末尾改行の過不足だけでも `Changed`，許容した誤り区画を保った成功は `Partial`，整形仕様 8.3）
 */
FormatClassification Formatter::FormatCode(
	const GrammarProfile &Profile,
	std::string &Source,
	const Lang Language,
	const bool ShouldLint,
	FormatResult &Result,
	EmbeddedOutput *const Embedded
) const {
	const auto Skip = [&Result](const FailureReason Reason) -> FormatClassification {
		Result.Reason = Reason;
		// 原文を書き換えずに見送る結果の返戻
		return FormatClassification::Skipped;
	};
	// 記憶域・走脈不足時は原文保持で失敗報告（原文を書き換えるのは検証を通った整形結果の格納だけ）
	try {
		// 入力の受理の判定と表現の正規化（整形仕様 3.4 の Normalize，原文は書き換えない）
		InputNormalizer::NormalizedInput Normalized = InputNormalizer::Normalize(Profile, Source);
		// 受理しない入力の見送の返戻
		if(Normalized.Reason != FailureReason::None) return Skip(Normalized.Reason);
		// 解析用の入力の適応（整形仕様 3.4 の Adapt，通常の見送と解析器故障を分ける）
		ParseAdapter::ParseInput Adapted = ParseAdapter::Adapt(Profile, Normalized);
		// 通常見送の返戻
		if(Adapted.Decision == ParseDecision::Skip) return Skip(Adapted.Failure);
		if(Adapted.Decision == ParseDecision::Fail) {
			Result.Reason = Adapted.Failure;
			// 解析器故障の返戻
			return FormatClassification::Failed;
		}
		// 後続工程の共通原稿と確定した言語
		const GrammarProfile &Effective = *Adapted.Profile;
		// 構文木の１回走査に依る受理しない構文誤りの検査（整形仕様 3.4）
		std::optional<ParsedTree> Parsed(SyntaxParser::Parse(Effective, Adapted));
		const bool HasInputError = Parsed->ContainsInputError();
		const ParseDecision Decision = Parsed->GetParseDecision();
		// 構文誤りで整形しない事の返戻
		if(Decision == ParseDecision::Skip) return Skip(Parsed->GetParseFailure());
		if(Decision == ParseDecision::Fail) {
			Result.Reason = Parsed->GetParseFailure();
			// 受理失敗の返戻
			return FormatClassification::Failed;
		}
		// 適応時の木は判定済の写しと独立して居る為，先に記憶域を空ける
		Adapted.Src.reset();
		// HTML に埋め込んだコードの単独のコードと同じ工程での整形（整形仕様 4.4）
		std::vector<EmbeddedCode> Embeds;
		// 中身の整形の失敗を HTML 全体の結果とする事の返戻
		if(Parsed->GetReading().HasEmbeddedScripts) if(
			const FormatClassification Failure = FormatEmbeddedCode(*Parsed, ShouldLint, Embeds, Result);
			Failure != FormatClassification::Changed
		) return Failure;
		// 中核処理の適用と式結合の最終検査
		std::optional<ValidationContext> Context;
		std::shared_ptr<const LaidOutCode> Code;
		std::optional<Core::OutputExpectation> Expectation;
		std::optional<Core::CommentRepository> Comments;
		// 終端処理を済ませた最終字面（入力の解析結果とは別の値）
		std::string FinalText;
		RunCore(Effective, Adapted, Parsed, Context, Code, Expectation, Comments, FinalText, Embedded);
		// 独立検証の前の出力上限の確認（整形仕様 9.1）
		if(FinalText.size() > MaxInputBytes) throw FormatLimitExceeded("output exceeds size limit");
		// 検証が捉えた見送の理由と，埋込コードを整形し切れなかった通知を加えたか
		FailureReason ValidationReason = FailureReason::None;
		bool HasEmbedNote = false;
		// 中核の出力の読取専用な観測（整形仕様 10.5，配置を検証へ渡して手放す前に字面を作り，検証を通った時だけ出す）
		const std::string Observed = Embedded ? std::string() : ObservedCore(Code);
		// 最終字面の独立した検証・静的検査・入力との比較による整形結果の確定
		const FormatClassification Outcome = FinalizeOutput(
			Effective,
			std::move(Code),
			*Context,
			Normalized.Text,
			FinalText,
			*Expectation,
			*Comments,
			ShouldLint,
			Normalized.IsFormChanged,
			HasInputError && !Embedded,
			Embeds,
			Result.Warnings,
			HasEmbedNote,
			ValidationReason
		);
		// 保護区画を保てない事を検証で捉えた部分整形の見送（整形仕様 8.3 は此れを整形器の不具合として通知しない）
		if(Outcome == FormatClassification::Skipped) {
			Result.Warnings.clear();
			Result.Reason = ValidationReason;
			// 検証が捉えた理由での返戻（入力の段階の見送と区別する）
			return FormatClassification::Skipped;
		}
		if(Outcome == FormatClassification::Failed) {
			Result.Reason = ValidationReason;
			// 意味を壊す整形を差し戻した事の返戻
			return Outcome;
		}
		// 検証を通った中核の出力の観測の表示
		std::fputs(Observed.c_str(), stderr);
		// 静的検査の警告と部分整形の通知を最終字面の位置と警告一覧の行順に並べる（整形仕様 8.4, 10.4）
		std::stable_sort(
			Result.Warnings.begin(),
			Result.Warnings.end(),
			[](const LintWarning &Left, const LintWarning &Right) -> bool {
				// 位置又は一覧の行順の異なる警告の前後の返戻
				if(std::tie(Left.Row, Left.Column, Left.Rule) != std::tie(Right.Row, Right.Column, Right.Rule)) {
					// 警告の位置と種類に依る順序の返戻
					return std::tie(Left.Row, Left.Column, Left.Rule) < std::tie(Right.Row, Right.Column, Right.Rule);
				}
				// 同じ識別子の置換値の順（一覧外の警告と通知は検出の順）の前後の返戻
				return Left.Rule != LintRule::Other && Left.Message < Right.Message;
			}
		);
		// 変更有無の判定
		const bool IsChanged = Outcome == FormatClassification::Changed;
		if((HasInputError || HasEmbedNote) && IsChanged && Language.IsAssumed) {
			Result.Warnings.clear();
			// 拡張子から定まらず指定だけで仮定した言語で読めない所の有る入力は，取違に依る内容の破壊を防ぐ為に書き換えない事の返戻（整形仕様 10.4）
			return Skip(FailureReason::AssumedLanguageMismatch);
		}
		// 検証した字面の其の儘の反映
		if(IsChanged) Source = std::move(FinalText);
		// 許容した誤り区画を保持した儘外側を整形し検証した事の返戻（整形仕様 8.3 の部分整形）
		if(HasInputError || HasEmbedNote) return FormatClassification::Partial;
		// 完全整形の結果の返戻
		return Outcome;
	} catch(const FormatLimitExceeded &E) {
		// 制限超過時の原文復元と結果分類
		Result.Warnings.clear();
		Result.Reason = { E.Kind, E.Reason };
		// 資源上限は見送，到達不能防壁と保持契約の不成立は失敗として原文を返す分類の返戻
		return E.Kind == FailureReason::LimitExceeded ? FormatClassification::Skipped : FormatClassification::Failed;
	} catch(const std::exception &) {
		Result.Warnings.clear();
		// 途中の段が控えた理由を資源枯渇又は内部の誤りの通知へ持ち込まない
		Result.Reason = FailureReason::ResourceFailure;
	}
	// 資源を使い切り整形を完了出来なかった事の返戻
	return FormatClassification::Failed;
}

/**
 * コンストラクタ（行分割器の受取）
 * 行幅は此の器だけが知り，整形の途中で変わらない
 * @param LineBreaking 行幅を持つ行分割器
 */
Formatter::Formatter(const Core::LineBreaker &LineBreaking) : Breaker(LineBreaking) {}

/**
 * ソース整形関数
 * 計算量：入力の長さ N に対し概ね O(N)（解析と各工程が N に比例し，行分割は幅の超過が有る箇所だけ巡る）
 * @param RawSource 整形対象のソース（読取専用で，結果は `FormatResult::Text` が持つ）
 * @param Language 対象言語
 * @param ShouldLint false で規約検査を飛ばす
 * @return 整形結果と診断情報（末尾改行の過不足だけでも `Changed`，許容した誤り区画を保った成功は `Partial`，整形仕様 8.3）
 */
FormatResult Formatter::Format(const std::string &RawSource, const Lang Language, const bool ShouldLint) const {
	FormatResult Result;
	std::string &Source = Result.Text;
	Source = RawSource;
	// 入力上限と未指定言語は文法の選択より先に判定する
	if(Source.size() > MaxInputBytes) {
		Result.Classification = FormatClassification::Skipped;
		Result.Reason = FailureReason::InputSizeExceeded;
		// 入力上限を超えた原文の返戻
		return Result;
	}
	if(Language == Lang::Unknown) {
		Result.Classification = FormatClassification::Skipped;
		Result.Reason = FailureReason::UnsupportedLanguage;
		// 対応外言語の原文の返戻
		return Result;
	}
	try {
		Result.Classification = FormatCode(GrammarProfile::Resolve(Language), Source, Language, ShouldLint, Result, nullptr);
	} catch(const FormatLimitExceeded &E) {
		Result.Classification = E.Kind == FailureReason::LimitExceeded ? FormatClassification::Skipped : FormatClassification::Failed;
		Result.Reason = { E.Kind, E.Reason };
	} catch(const std::exception &) {
		// 予期しない例外の失敗分類
		Result.Classification = FormatClassification::Failed;
		Result.Reason = FailureReason::ResourceFailure;
	}
	// 整形結果の返戻
	return Result;
}
