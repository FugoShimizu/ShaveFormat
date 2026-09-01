#include "Formatter.hpp"
#include "Pass/Edit.hpp"
#include "Pass/Layout.hpp"
#include "Pass/LineSplit.hpp"
#include "Pass/Structure.hpp"
#include "Util/FileIO.hpp"
#include "Util/Postprocess.hpp"
#include "Util/Preprocess.hpp"
#include "Util/SyntaxCheck.hpp"
#include "Util/TsSource.hpp"
#include <algorithm>
#include <cstring>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * 入力の早期振分と `TSSource` 構築関数（`Unknown`・NUL バイトの振分と，行継続の結合等の解析前の原文の調整）
 * @param Source 整形対象のソース（末尾改行正規化済）
 * @param Language 対象言語（`.h` の中身が C++ と判れば書き換える）
 * @param OutSrc 後続パイプラインで使う `TSSource` を直接構築する出力先
 * @param SkipReason 整形を行わなかった理由の格納先（静的な字面を指す）
 * @param IsLangAmbiguous 拡張子だけでは言語が定まらない入力か（`.h` や経路の無い標準入力）
 * @param Marker 原文へ挟んだ解析用の印（未使用時は '\0'）
 * @return 原文返戻で打ち切るなら true，整形処理を継続するなら false
 */
bool Formatter::RejectInvalidInput(
	const std::string &Source,
	Lang &Language,
	std::optional<TSSource> &OutSrc,
	std::string_view &SkipReason,
	const bool IsLangAmbiguous,
	char &Marker
) {
	// 対応外言語の原文返戻（CLI は経路収集と `--lang` 解析で先に弾く為，埋込利用者用の防壁）
	if(Language == Lang::Unknown) {
		SkipReason = "unsupported language";
		// 対応外言語の場合の返戻
		return true;
	}
	// UTF-16 / UTF-32 原本の符号化違いとしての見送
	if(
		Source.starts_with("\xFF\xFE") || Source.starts_with("\xFE\xFF") || Source.starts_with(std::string_view("\0\0\xFE\xFF", 4))
	) {
		SkipReason = "unsupported encoding";
		// 対応外符号化の場合の返戻
		return true;
	}
	// C / C++ の行継続の字句化前結合と前処理コメント・リテラルの補修
	Marker = '\0';
	const std::string Prepared = Language.IsCFamily() ? EditPass::PrepareCSource(Source, Marker) : std::string();
	const std::string &Text = Prepared.empty() ? Source : Prepared;
	// 構文木の初回構築
	OutSrc.emplace(Text, Language.TsLang());
	// 初回解析の失敗
	if(!OutSrc->IsParsed()) {
		SkipReason = OutSrc->HasExpiredParse() ? "parsing exceeds the time limit" : "parser initialization failed";
		// 初回解析失敗時の返戻
		return true;
	}
	// NUL を拒む文法の原文返戻
	if(ts_node_has_error(OutSrc->GetRoot()) && std::memchr(Source.data(), '\0', Source.size())) {
		SkipReason = "parser could not read the source";
		// NUL を含む解析失敗時の返戻
		return true;
	}
	// 共有拡張子だけ破損減少時に C++ へ昇格し，C 利用側も保つ共通の書換に限定
	if(IsLangAmbiguous && Language == Lang::C) {
		const uint32_t CBroken =
		(ts_node_has_error(OutSrc->GetRoot()) ? SyntaxCheck::CountBrokenNodes(*OutSrc, OutSrc->GetRoot()) : 0) +
		SyntaxCheck::CountMalformedCDefinitions(*OutSrc);
		std::optional<TSSource> CppSrc;
		if(CBroken) CppSrc.emplace(Text, Lang::Get(Lang::Cpp).TsLang());
		// C++ の方が破損を減らす入力
		if(CppSrc && CppSrc->IsParsed() && SyntaxCheck::CountBrokenNodes(*CppSrc, CppSrc->GetRoot()) < CBroken) {
			OutSrc = std::move(CppSrc);
			Language = Lang::Get(Lang::Cpp);
			Language.IsCShared = true;
		}
	}
	// Java Unicode エスケープの展開と構文木の実字句への一致
	if(Language == Lang::Java && Source.find("\\u") != std::string::npos) {
		std::string Expanded;
		// javac と同じ字句展開に失敗
		if(!Preprocess::ExpandJavaUnicodeEscapes(*OutSrc, Expanded)) {
			SkipReason = "parser could not read the source";
			// Unicode エスケープ展開失敗時の返戻
			return true;
		}
		if(!Expanded.empty()) OutSrc.emplace(Expanded, Language.TsLang());
	}
	// 原稿補正後の解析失敗
	if(!OutSrc->IsParsed()) {
		SkipReason = OutSrc->HasExpiredParse() ? "parsing exceeds the time limit" : "parser could not read the source";
		// 補正後の解析失敗時の返戻
		return true;
	}
	// 埋込文書を含む Ruby
	if(Language == Lang::Ruby && Source.find("=begin") != std::string::npos) {
		if(std::string Escaped = Preprocess::EscapeRubyEmbeddedDocs(*OutSrc, Marker); !Escaped.empty()) {
			OutSrc.emplace(std::move(Escaped), Language.TsLang());
			// 埋込文書退避後の解析失敗
			if(!OutSrc->IsParsed()) {
				SkipReason = OutSrc->HasExpiredParse() ? "parsing exceeds the time limit" : "parser could not read the source";
				// 埋込文書退避後の解析失敗時の返戻
				return true;
			}
		}
	}
	// 整形続行を呼出側に通知の返戻
	return false;
}

/**
 * コメント添付準備関数
 * @param Src 解析済の `TSSource`
 * @param Language 対象言語
 * @param Marker 解析の為に原文へ置いた印（Ruby の埋込ドキュメントの本文から除く，置いて居なければ '\0'）
 */
void Formatter::PrepareComments(TSSource &Src, const Lang Language, const char Marker) {
	// コメントを構文木へ添付出来る入力
	if(Src.IsParsed()) {
		if(Language == Lang::HTML) Preprocess::PrepareHtmlComments(Src);
		else Src.AttachComments(Language == Lang::JavaScript || Language.IsJsx, Language.IsScss, Marker);
	}
	// 編集中の添付情報停止と最終構文木への１回だけの再束縛
	Src.SuspendAttachments();
	return;
}

/**
 * 中核パイプライン適用関数（編集 → 構造化 → 宣言統合 → 行分割 → 文書化 → 配置）
 * 計算量：入力の長さ N に対し O(N)
 * @param Src 解析済の `TSSource`（パス適用に応じて再解析される）
 * @param Language 対象言語
 * @param SkipReason 手間の上限超過時の見送理由格納先
 * @return 意味を保存して工程を完了出来れば true
 */
bool Formatter::RunCorePasses(TSSource &Src, const Lang Language, std::string_view &SkipReason) {
	// 構文木を持つ入力だけを変換
	if(Src.IsParsed()) {
		// Python 固有の前処理
		if(Language == Lang::Python) {
			EditPass::ApplyLineContinuations(Src);
			StructurePass::ExpandPythonInlineBlocks(Src);
		}
		// Python / Ruby の括弧内改行の平坦化
		if(Language == Lang::Python || Language == Lang::Ruby) StructurePass::FlattenBracketContinuations(Src, Language);
		// Kotlin の改行境界を補修
		if(Language == Lang::Kotlin) EditPass::ApplyKotlinJoints(Src);
		// C# のキャストと誤読した二項式の補修
		if(Language == Lang::CSharp) EditPass::ApplyCSharpMisreadCasts(Src);
		// 言語共通の編集を一括適用
		EditPass::ApplyConsolidated(Src, Language);
		// 行幅に依らない波括弧編集の構造化前適用と残存空白・改行の組直
		if((Language.IsBraceLang() || Language == Lang::Kotlin) && !EditPass::ApplyBraceEdits(Src, Language)) return false;
		// `Unknown` 以外の正規化と Python だけの行内空白への限定
		if(Language == Lang::Python) StructurePass::NormalizeInlineSpaces(Src, Language);
		// 構造正規化失敗時の返戻
		else if(!StructurePass::NormalizeStructured(Src, Language, SkipReason)) return false;
	}
	// コメント添付の再開
	Src.ResumeAttachments();
	// 紐付を保った変換後の括弧・構造の再整理と反復上限の設定
	for(uint32_t Round = 0; Round < RubyModifierRounds && EditPass::ApplyRubyModifierForm(Src, Language); ++Round) {
		EditPass::ApplyConsolidated(Src, Language);
		// 構造正規化失敗時の返戻
		if(!StructurePass::NormalizeStructured(Src, Language, SkipReason)) return false;
	}
	// 最終編集パスの適用
	EditPass::ApplyNamedImportSort(Src, Language);
	EditPass::ApplyVarDeclMerge(Src, Language);
	// 行分割失敗時の返戻
	if(Language != Lang::CSS && Language != Lang::HTML && !LineSplitPass::Apply(Src, Language, SkipReason)) return false;
	EditPass::ApplyDocComment(Src, Language);
	LayoutPass::FinalizeLayout(Src, Language);
	// 中核工程の成功の返戻
	return true;
}

/**
 * 整形結果の確定関数（整形後の構文誤り検査・規約検査・後処理・末尾空白除去）
 * 計算量：入力の長さ N に対し O(N)
 * @param Src 整形パス適用済の `TSSource`
 * @param Language 対象言語
 * @param Source 末尾改行正規化済の入力（整形結果との比較に用いる）
 * @param Output 整形結果の格納先（変更有の時のみ設定する）
 * @param Warnings 検出された規約警告の格納先
 * @param SkipsLint true で規約検査を飛ばす
 * @param InputBrokenCount 入力時点で壊れていたノードの個数（整形後の増加判定に用いる）
 * @param IsTailChanged 末尾の改行の個数や改行様式が正規化で変わった場合 true（其れだけの違反も変更として扱う）
 * @param Marker 解析の為に原文へ置いた印（C / C++ は `*` へ戻す，置いて居なければ '\0'）
 * @return 整形結果の種別（`Changed` / `Unchanged` / `SyntaxGuard`）
 */
FormatOutcome Formatter::FinalizeOutput(
	TSSource &Src,
	const Lang Language,
	const std::string &Source,
	std::string &Output,
	std::vector<LintWarning> &Warnings,
	const bool SkipsLint,
	const uint32_t InputBrokenCount,
	const bool IsTailChanged,
	const char Marker
) {
	// 言語毎に必要な最終補修の適用
	switch(Language.Id) {
	case Lang::Go:
		// Go の複数行コンテナの末尾カンマ補修
		Postprocess::FixGoTrailingComma(Src);
		break;
	case Lang::CSS:
		// CSS 宣言の終端セミコロン補完
		Postprocess::FixCssMissingSemicolon(Src);
		break;
	case Lang::HTML:
		// HTML 空要素の自己終端スラッシュ除去
		Postprocess::FixHtmlSelfClosing(Src);
		break;
	case Lang::JavaScript:
	case Lang::TypeScript:
		// JSX 本文端の改行隣接空白だけの式保持
		Postprocess::FixJsxEdgeSpaces(Src);
		break;
	default:
		break;
	}
	// 各行末の空白の最終除去
	Postprocess::TrimTrailingWhitespace(Src, Language);
	// 有効位置へ移った符号化宣言の復元
	Postprocess::KeepEncodingDeclaration(Src, Source, Language);
	// 入力と同じ補修後構文木での破損増加検査
	std::optional<TSSource> Reparsed;
	if(Language == Lang::Ruby && Src.IsParsed() && Src.find("=begin") != std::string::npos) {
		char DocMarker = '\0';
		if(std::string Escaped = Preprocess::EscapeRubyEmbeddedDocs(Src, DocMarker); !Escaped.empty()) {
			Reparsed.emplace(std::move(Escaped), Language.TsLang());
		}
	}
	// 印付構文木の構築失敗時の解析済原稿による検査
	const TSSource &Checked = Reparsed && Reparsed->IsParsed() ? *Reparsed : Src;
	if(
		Checked.IsParsed() && ts_node_has_error(Checked.GetRoot()) && (
			InputBrokenCount ?
			SyntaxCheck::CountBrokenNodes(Checked, Checked.GetRoot()) > InputBrokenCount :
			SyntaxCheck::HasErrorDescendant(Checked, Checked.GetRoot())
		)
		// 構文破壊を検出した事の返戻
	) return FormatOutcome::SyntaxGuard;
	// 規約警告の収集
	if(!SkipsLint && Checked.IsParsed()) Warnings = LintPass::Run(Checked, Language);
	// 構文守衛・規約検査後の C / C++ 前処理リテラルの復元
	std::string Restored;
	if(Marker && Language.IsCFamily()) {
		Restored = Src;
		EditPass::RestoreLinkageGuards(Restored, Marker);
		std::replace(Restored.begin(), Restored.end(), Marker, '*');
	}
	// 末尾改行を除く入力と整形結果の比較
	const std::string &Result = Restored.empty() ? static_cast<const std::string &>(Src) : Restored;
	// 比較範囲の算出
	size_t ResultLen = Result.size(), SourceLen = Source.size();
	while(ResultLen && Result[ResultLen - 1] == '\n') --ResultLen;
	while(SourceLen && Source[SourceLen - 1] == '\n') --SourceLen;
	if(!IsTailChanged && ResultLen == SourceLen && !std::memcmp(Result.data(), Source.data(), ResultLen)) {
		// 変更無の返戻
		return FormatOutcome::Unchanged;
	}
	// 事前の領域確保に依る確保失敗時の格納先保全
	Output.reserve(ResultLen + 1);
	Output.assign(Result.data(), ResultLen);
	if(!Output.empty()) Output += '\n';
	// 正規化済の整形結果を格納し，変更有を呼出側に通知の返戻
	return FormatOutcome::Changed;
}

/**
 * コードの整形関数
 * @param Source 整形対象のソース（先ず末尾改行が正規化され，変更時は整形結果で上書きされる）
 * @param Language 対象言語
 * @param Result 警告・見送理由・入力の構文破損の格納先
 * @param SkipsLint true で規約検査を飛ばす
 * @param IsLangAmbiguous 拡張子だけでは言語が定まらない入力か（`.h` や経路の無い標準入力）
 * @return 整形結果の種別（末尾改行の過不足だけでも `Changed` とする）
 */
FormatOutcome Formatter::FormatCode(
	std::string &Source,
	const Lang Language,
	FormatResult &Result,
	const bool SkipsLint,
	const bool IsLangAmbiguous
) {
	// tree-sitter の３２ビット位置を超える入力の解析拒否
	if(Source.size() > FileIO::MaxInputBytes) {
		Result.SkipReason = "input exceeds size limit";
		// 入力上限超過時の返戻
		return FormatOutcome::Skipped;
	}
	// 末尾正規化の復元情報
	const size_t TailNewlines = Preprocess::CountTailNewlines(Source);
	const bool KeepsFormOnNormalize =
	Source.find('\r') == std::string::npos && (Source.empty() || TailNewlines == 1 && Source.back() == '\n');
	// 見送時の原稿復元処理
	std::string OriginalForm;
	bool IsRestorable = false;
	const auto Skip = [&Source, &OriginalForm, &IsRestorable, &Result](const std::string_view Reason) -> FormatOutcome {
		// 見送状態の反映
		if(IsRestorable) Source.swap(OriginalForm);
		Result.SkipReason = Reason;
		// 見送結果の返戻
		return FormatOutcome::Skipped;
	};
	// 記憶域・走脈不足時は原文保持で失敗報告（書換は正規化と最終格納だけの為，其の前の失敗は正規化前の控えから復元）
	try {
		if(!KeepsFormOnNormalize) {
			OriginalForm = Source;
			IsRestorable = true;
		}
		// 比較基準の LF・末尾１改行への正規化
		std::string MixedTail;
		// 解析の二乗化を招く深さ・未閉じコメント・長いコメント列の見送
		if(
			const std::string_view Reason =
			Language == Lang::HTML ? Preprocess::HtmlSkipReason(Source) : Preprocess::CommentRunSkipReason(Source, Language);
			!Reason.empty()
			// 解析する前に見送った事の返戻
		) return Skip(Reason);
		// 改行の正規化
		const bool IsFormChanged = Preprocess::NormalizeNewlines(Source, Language, MixedTail);
		// 末尾改行数の差異に依る整形要否の記録
		const bool IsTailChanged = Source.empty() ? TailNewlines : TailNewlines != 1;
		// `.h` の Objective-C 宣言の検出と誤った部分整形の見送
		if(IsLangAmbiguous && Language == Lang::C && Preprocess::LooksObjectiveC(Source)) return Skip("Objective-C header");
		// `.ts` の Qt Linguist XML の検出と訳文を壊す部分整形の見送
		if(Language == Lang::TypeScript && Preprocess::LooksXmlDocument(Source)) return Skip("XML document");
		// `Unknown`・NUL バイトの早期振分と構文木の構築に用いる状態
		std::optional<TSSource> SrcOpt;
		std::string_view SkipReason;
		Lang Effective = Language;
		char Marker = '\0';
		// 早期振分で打ち切る場合は原文の儘で無変更とする事の返戻
		if(RejectInvalidInput(Source, Effective, SrcOpt, SkipReason, IsLangAmbiguous, Marker)) return Skip(SkipReason);
		// 後続工程の共通原稿
		TSSource &Src = *SrcOpt;
		// 実引数を逐語保持するマクロと，文へ展開して本体の波括弧を要するマクロを収集
		if(Effective.IsCFamily()) Src.CollectMacros();
		// 前処理自身が壊した節点も最終の構文検査で見付ける為，前処理の前に壊れて居た節点の数を控える（健全な入力では数えない）
		const uint32_t PreparedBroken = ts_node_has_error(Src.GetRoot()) ? SyntaxCheck::CountBrokenNodes(Src, Src.GetRoot()) : 0;
		// 入力検査・添付前の言語別誤読の補修と本文・コメントの消失防止
		if(Src.IsParsed()) switch(Effective.Id) {
		case Lang::Ruby:
			EditPass::ApplyRubyCommandArguments(Src);
			break;
		case Lang::JavaScript:
		case Lang::TypeScript:
			EditPass::ApplyJsJoints(Src);
			break;
		case Lang::Kotlin:
			EditPass::ApplyKotlinSemicolons(Src);
			break;
		case Lang::Swift:
			// 密着コメントの再解析が上限を超えて整形しない事の返戻
			if(!EditPass::ApplySwiftMisreads(Src)) return Skip("comments glued to operators exceed parser limit");
			break;
		default:
			break;
		}
		// 構文木の１回走査に依る入力エラーの収集
		bool HasInputError = false;
		const bool IsInputAccepted = SyntaxCheck::ScanInputErrorStates(Src, Effective, HasInputError, SkipReason);
		Result.HasInputError = HasInputError;
		// 構文誤りで整形しない事の返戻
		if(!IsInputAccepted) return Skip(SkipReason);
		// 入力破損時だけの前処理前後の少ないエラー件数の採用
		const uint32_t InputBrokenCount =
		Result.HasInputError ? std::min(PreparedBroken, SyntaxCheck::CountBrokenNodes(Src, Src.GetRoot())) : 0;
		// PHP の地の文・Ruby の `__END__` 以後の逐語保持
		std::string PhpLead, PhpTail, PhpBody;
		bool IsDetached = false;
		const bool KeepsTail = Preprocess::DetachOuterText(Src, Effective, PhpLead, PhpTail, PhpBody, IsDetached);
		// 本文・コメントの正規化とコメント添付に依る分離
		PrepareComments(Src, Effective, Marker);
		// 中核処理の適用と式結合の最終検査
		if(!RunCorePasses(Src, Effective, SkipReason)) {
			// 上限超過時の原文保持による見送
			if(!SkipReason.empty()) return Skip(SkipReason);
			if(IsRestorable) Source.swap(OriginalForm);
			// 式の結合を守れなかった事の返戻
			return FormatOutcome::SyntaxGuard;
		}
		// 整形後の構文誤り検査・規約検査・後処理・末尾空白除去
		std::string Output;
		const FormatOutcome Outcome = FinalizeOutput(
			Src,
			Effective,
			IsDetached ? PhpBody : Source,
			Output,
			Result.Warnings,
			SkipsLint,
			InputBrokenCount,
			IsTailChanged && !KeepsTail || IsFormChanged,
			Marker
		);
		// 構文守衛差戻時の正規化前状態への復元
		if(Outcome == FormatOutcome::SyntaxGuard) {
			if(IsRestorable) Source.swap(OriginalForm);
			// 構文を壊す整形を差し戻した事の返戻
			return Outcome;
		}
		// 入力破損の最初の破損位置での通知
		if(Result.HasInputError) {
			const TSPoint At = SyntaxCheck::FirstBrokenPoint(Src);
			Result.Warnings.push_back({ At.row, At.column, std::string(SyntaxErrorWarning) });
		}
		// 変更有無の判定
		const bool IsChanged = Outcome == FormatOutcome::Changed;
		// 切り離した地の文の空白の逐語復元
		if(IsChanged && IsDetached) Postprocess::ReattachOuterText(Output, PhpLead, PhpTail);
		// 末尾改行の復元と整形結果の反映
		if(KeepsTail) Postprocess::RestoreTailNewlines(IsChanged ? Output : Source, TailNewlines, MixedTail);
		if(IsChanged) Source.swap(Output);
		// 整形結果の返戻
		return Outcome;
		// 制限超過時の原文復元と結果分類
	} catch(const FormatLimitExceeded &E) {
		Result.Warnings.clear();
		// 到達不能防壁の原文返戻と不具合終了への区別
		if(E.IsDefect) {
			Skip(E.Reason);
			// 整形器の不具合で整形を行わなかった事の返戻
			return FormatOutcome::Defect;
		}
		// 時間・手間・出力の何れかの上限に達し，整形を見送った事の返戻
		return Skip(E.Reason);
	} catch(const std::exception &) {
		if(IsRestorable) Source.swap(OriginalForm);
		Result.Warnings.clear();
	}
	// 資源を使い切り整形を完了出来なかった事の返戻
	return FormatOutcome::Failed;
}

/**
 * ソース整形関数
 * 計算量：入力の長さ N に対し概ね O(N)（解析と各工程が N に比例し，行分割は幅の超過が有る箇所だけ巡る）
 * @param Source 整形対象のソース（先ず末尾改行が正規化され，変更時は整形結果で上書きされる）
 * @param Language 対象言語
 * @param SkipsLint true で規約検査を飛ばす
 * @param IsLangAmbiguous 拡張子だけでは言語が定まらない入力か（`.h` や経路の無い標準入力）
 * @return 整形結果と診断情報（末尾改行の過不足だけでも `Changed` とする）
 */
FormatResult Formatter::Format(std::string &Source, const Lang Language, const bool SkipsLint, const bool IsLangAmbiguous) {
	// 呼出毎の診断情報初期化
	FormatResult Result;
	// PHP の `__halt_compiler()` 以後の分離
	size_t DataStart = std::string::npos;
	bool IsCrLfUniform = false;
	std::string Code;
	try {
		if(Language == Lang::PHP && Source.size() <= FileIO::MaxInputBytes) DataStart = Preprocess::PhpHaltDataStart(Source);
		if(DataStart != std::string::npos) {
			IsCrLfUniform = Source.find("\r\n") != std::string::npos && !FileIO::IsNewlineMixed(Source);
			// 原本の改行様式による仮置きと整形後の除去
			Code.assign(Source, 0, DataStart);
			Code += IsCrLfUniform ? "\r\n" : "\n";
		}
	} catch(const std::exception &) {
		// 後置データ分離失敗の記録
		Result.Warnings.clear();
		Result.Outcome = FormatOutcome::Failed;
		// 資源を使い切り整形を完了出来なかった事の返戻
		return Result;
	}
	// 後置データ有無に応じた整形経路の分岐
	if(DataStart == std::string::npos) {
		Result.Outcome = FormatCode(Source, Language, Result, SkipsLint, IsLangAmbiguous);
		// データを持たない入力の整形結果の返戻
		return Result;
	}
	// 後置データを除くコード部分の整形
	Result.Outcome = FormatCode(Code, Language, Result, SkipsLint, IsLangAmbiguous);
	// 整形しなかった場合は原文の儘とする事の返戻
	if(Result.Outcome != FormatOutcome::Changed && Result.Outcome != FormatOutcome::Unchanged) return Result;
	// 整形後コードの末尾改行除去とデータの逐語接続
	while(!Code.empty() && Code.back() == '\n') Code.pop_back();
	try {
		Source.replace(0, DataStart, Code);
	} catch(const std::exception &) {
		Result.Warnings.clear();
		Result.Outcome = FormatOutcome::Failed;
		// 資源を使い切り整形を完了出来なかった事の返戻
		return Result;
	}
	// CRLF 原本データの LF への畳込
	if(IsCrLfUniform) FileIO::FoldCrLf(Source, Code.size(), false);
	// 整形結果の種別の返戻
	return Result;
}
