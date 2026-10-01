#include "CommentPlacer.hpp"
#include "../Util/CharClass.hpp"
#include "../Util/CommentNormalizer.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include "../Util/TextEdit.hpp"
#include "DocCommentSynchronizer.hpp"
#include "SyntaxModel.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 独立・行末の通常コメントの規約の行形式の字面の取得関数（整形仕様 7.6 の記法の選択）
	 * 一般の囲み，文書でない位置の文書の記法，採用記法と異なる一般の行記法（PHP の `#`）の本文の論理行を採用する行記法で合成し，開始記号・終端記号だけの行を落とす
	 * 採用記法の行コメントと埋込文書の独立した記録も，整形仕様 7.3 で正規化した論理行を行コメントで保てない物は論理行毎に記法を選ぶ
	 * 行コメントで保てない論理行は同じ種別で合法な囲み（一般の囲み，無い文法は独立行の埋込文書）へ移し，合法な囲みの無い物は整形仕様 7.7 に委ねる
	 * 揃える記法で記号の一部と為る頭（前の成員の文書の頭・区切の記法の頭）だけを密着させ，其の他の記号で始まる本文は区切空白の後へ置く
	 * 揃えた行が別の記法・処理系の登録形に為る論理行は，密着を外して避け，避けれなければ行コメントで保てない行とする
	 * @param Rules 投影済の記法
	 * @param Record コメントの記録
	 * @param Text 出力記法の本文（論理行が保つ空の論理行の印と対応する）
	 * @param IsDocument 文書か（文書の二重星の囲みは揃えない）
	 * @param KeptBlanks 出力記法の本文の論理行毎の構造として保つ空の論理行の印（コード例の中の行と，段落とコード例を分ける行）
	 * @return 行形式の字面（既に行形式で保てる記録と行形式で本文を保てない１つの囲みの記録は空）
	 */
	std::string CommentPlacer::LineFormOf(
		const NotationRules &Rules,
		const CommentRecord &Record,
		const std::string_view Text,
		const bool IsDocument,
		const std::span<const uint8_t> KeptBlanks
	) {
		// 採用する行記法で置く記録の種類（群を纏める記法と同じ定義）
		const CommentRepository::LinePlacement Placement = CommentRepository::LinePlacementOf(Record, IsDocument);
		// 揃えない記録の返戻
		if(Placement == CommentRepository::LinePlacement::None) return {};
		// 通常の独立・行末コメントの採用記法
		const CommentOutputForm *const Plain = Rules.AdoptedFormOf(CommentUse::Plain);
		const bool IsGeneralBlock = Placement == CommentRepository::LinePlacement::GeneralBlock;
		const bool IsLineRecord = Placement == CommentRepository::LinePlacement::LineRecord;
		// 論理行の本文と，其れが構造として保つ空の論理行か
		std::vector<std::string_view> Bodies;
		std::vector<uint8_t> IsKept;
		for(size_t Cursor = 0, Index = 0; Cursor <= Text.size(); ++Index) {
			const size_t End = std::min(Text.find('\n', Cursor), Text.size());
			std::string_view Body = Text.substr(Cursor, End - Cursor);
			Cursor = End + 1;
			while(!Body.empty() && (Body.back() == ' ' || Body.back() == '\t')) Body.remove_suffix(1);
			// 開始記号だけの行と，通常コメントの空の論理行の読飛（内容の無い通常のコメント行は置かず，文書の空の論理行と構造として保つ空の論理行は保つ，整形仕様 7.3, 7.7）
			// 構造として保つ空の論理行は，開始記号・終端記号だけの行でも群の前後の記録との間の構造を成す為に保つ
			if(Body.empty() && !KeptBlanks[Index] && (!IsDocument || Bodies.empty())) continue;
			Bodies.push_back(Body);
			IsKept.push_back(KeptBlanks[Index]);
		}
		// 終端記号だけの行の除去
		while(!Bodies.empty() && Bodies.back().empty() && !IsKept.back()) {
			Bodies.pop_back();
			IsKept.pop_back();
		}
		// 本文を持たない囲みは行形式にすると本文を失う為に揃えない事の返戻
		if(Bodies.empty()) return {};
		// 論理行毎の行コメントの字面と，行コメントで本文を保てるか
		std::vector<std::string> Lines;
		std::vector<uint8_t> IsLineSafe;
		for(size_t Index = 0; Index < Bodies.size(); ++Index) {
			CommentRecord Part { std::string(Bodies[Index]), Record.Placement };
			Part.Kind = Record.Kind;
			Part.Notation = Plain->Notation;
			Part.Format.Rules = Record.Format.Rules;
			// 揃える行記法で記号の一部と為る頭だけを密着させる
			Part.Format.AttachFirstLine = !CommentRepository::SymbolHeadOf(Part, Bodies[Index]).empty();
			std::string Line = CommentRepository::Compose(Part);
			// 密着で別の記法・登録形に為る行は密着を外す
			if(Part.Format.AttachFirstLine && !CommentRepository::IsPlainLineText(Rules, Line, Plain->Notation)) {
				Part.Format.AttachFirstLine = false;
				Line = CommentRepository::Compose(Part);
			}
			IsLineSafe.push_back(
				CommentRepository::CanHoldInLineComment(Rules, Bodies[Index]) &&
				CommentRepository::IsPlainLineText(Rules, Line, Plain->Notation)
			);
			Lines.push_back(std::move(Line));
		}
		const bool HasUnsafe = std::find(IsLineSafe.begin(), IsLineSafe.end(), 0) != IsLineSafe.end();
		// 全ての論理行を行コメントで保てる行記法の記録は其の儘とする事の返戻
		if(IsLineRecord && !HasUnsafe && Record.Notation == Plain->Notation) return {};
		// 論理行毎に記法を選ぶ記録（一般の囲みの閉じた囲みの並びは次の整形でも論理行毎の別のコメントとして読む）
		const bool IsSequence =
		IsLineRecord || IsGeneralBlock && (Record.Kind == CommentKind::Trailing || !Record.Format.SeparateLogicalLines);
		// 行コメントへ揃える最初の論理行（行末では行コメントの後の字面が其の本文へ続く為，行コメントで保てる最後の連続だけを揃える）
		size_t First = 0;
		if(IsSequence && Record.Kind == CommentKind::Trailing) {
			First = Bodies.size();
			while(First && IsLineSafe[First - 1]) --First;
		}
		// １つの囲みの論理行は一括で揃える為，保てない行を含む記録の返戻
		if(!IsSequence && HasUnsafe) return {};
		// 揃える論理行が無い事の返戻（行記法の記録は全ての論理行を囲みへ移す）
		if(
			First == Bodies.size() || IsSequence && !IsLineRecord && Record.Kind != CommentKind::Trailing &&
			std::find(IsLineSafe.begin(), IsLineSafe.end(), 1) == IsLineSafe.end()
		) return {};
		// 行コメントで保てない論理行を置く同じ種別で合法な囲み（一般の囲み，無い文法は独立行の埋込文書）
		const CommentNotation Enclosing =
		Rules.IsAvailable(CommentNotation::Block) ? CommentNotation::Block : CommentNotation::RubyEmbeddedDocument;
		// 合法な囲みを持たない保てない記録は其の記法の儘とする事の返戻（保持の違反は最終検証が拒否する，整形仕様 8.3）
		if(
			HasUnsafe &&
			(!Rules.IsAvailable(Enclosing) || !CommentRepository::IsBlockNotation(Enclosing) && Record.Kind != CommentKind::OwnLine)
		) return {};
		// 論理行毎の行記法と囲み記法の合成
		std::string Out;
		for(size_t Index = 0; Index < Bodies.size();) {
			if(!Out.empty()) Out += '\n';
			// 行コメントで保てる論理行の行コメントへの揃え
			if(Index >= First && IsLineSafe[Index]) {
				Out += Lines[Index++];
				continue;
			}
			// 行コメントで保てない論理行は論理行毎に閉じた囲みとする（投影は埋込文書も論理行毎の記録に分ける為，次の整形でも同じ形に為る）
			CommentRecord Part { std::string(Bodies[Index]), Record.Placement };
			Part.Kind = Record.Kind;
			Part.Notation = Enclosing;
			Part.Format.Rules = Record.Format.Rules;
			// 移す囲みで記号の一部と為る頭だけを密着させる
			Part.Format.AttachFirstLine = !CommentRepository::SymbolHeadOf(Part, Bodies[Index]).empty();
			++Index;
			Out += CommentRepository::Compose(Part);
		}
		// 行形式の字面の返戻
		return Out;
	}

	/**
	 * 通常コメントの空の論理行の除去関数（整形仕様 7.7）
	 * 囲みの記法の最初と最後の空の論理行は，論理行でなく開始記号・終端記号だけの行を表す為に残す
	 * 構造として保つ空の論理行（整形仕様 7.3 のコード例の中の行と，段落とコード例を分ける行）も残す
	 * 計算量：本文の長さ N に対し O(N)
	 * @param Record 通常コメントの出力記法の記録（本文を書き換える）
	 * @param KeptBlanks 論理行毎の構造として保つ空の論理行の印
	 */
	void CommentPlacer::DropEmptyLogicalLines(CommentRecord &Record, const std::span<const uint8_t> KeptBlanks) {
		const bool IsBlock = CommentRepository::IsBlockNotation(Record.Notation);
		const std::string_view Text = Record.Text;
		std::string Kept;
		Kept.reserve(Text.size());
		bool HasKept = false;
		for(size_t Cursor = 0, Index = 0; Cursor <= Text.size(); ++Index) {
			const size_t End = std::min(Text.find('\n', Cursor), Text.size());
			const std::string_view Line = Text.substr(Cursor, End - Cursor);
			// 内容の在る論理行と，囲みの開始記号・終端記号だけの行と，構造として保つ空の論理行の保持
			if(Line.find_first_not_of(" \t") != std::string_view::npos || IsBlock && (!Cursor || End == Text.size()) || KeptBlanks[Index]) {
				if(HasKept) Kept += '\n';
				Kept.append(Line);
				HasKept = true;
			}
			Cursor = End + 1;
		}
		Record.Text = std::move(Kept);
		// 終了
		return;
	}

	/**
	 * 行末迄を本文とするコメントかの判定関数
	 * 行コメントは一度置くと後続の挿入が種別に依らず其の本文へ吸われる為，並べて良いかの判定に用いる
	 * @param Text コメント本文
	 * @return `//` / `#` で始まるなら true
	 */
	bool CommentPlacer::OpensLineComment(const std::string_view Text) {
		// 行末迄を本文とするコメントかの返戻
		return Text.starts_with(CommentRepository::OpenerOf(CommentNotation::SlashLine)) ||
		Text.starts_with(CommentRepository::OpenerOf(CommentNotation::HashLine));
	}

	/**
	 * 行末へ積む形への変換関数（整形仕様 7.6）
	 * 文毎に分けた行末コメントの物理行が其々閉じたコメントなら，原文の順に空白を挟んで１行へ並べる
	 * 行コメントの後の物は其の本文へ続くが，文毎に記号を持つ為に文の区切りを保つ
	 * 計算量：本文の長さ N に対し O(N)
	 * @param Text コメント本文（複数行を含み得る）
	 * @return １行へ積んだ本文（閉じない囲みの行や空の行を含み積めない場合は空）
	 */
	std::string CommentPlacer::StackLineEnd(const std::string_view Text) {
		std::string Result;
		const std::string_view Opener = CommentRepository::OpenerOf(CommentNotation::Block);
		const std::string_view Closer = CommentRepository::CloserOf(CommentNotation::Block);
		for(size_t Pos = 0; Pos <= Text.size();) {
			// 物理行の範囲
			const size_t End = std::min(Text.find('\n', Pos), Text.size());
			std::string_view Line = Text.substr(Pos, End - Pos);
			Pos = End + 1;
			TextEdit::TrimView(Line);
			// 其の行で閉じる囲みの並びと，最後の行コメントだけを積める
			for(std::string_view Rest = Line; !Rest.empty();) {
				// 行末迄を本文とする行コメントで行を終える
				if(OpensLineComment(Rest)) break;
				// 囲みで始まらない行は積めない事の返戻
				if(!Rest.starts_with(Opener)) return {};
				// 同じ行の閉じ
				const size_t Close = Rest.find(Closer, Opener.size());
				// 行を越える囲みは積めない事の返戻
				if(Close == std::string_view::npos) return {};
				Rest.remove_prefix(Close + Closer.size());
				TextEdit::TrimView(Rest);
			}
			// 空の行は積めない事の返戻
			if(Line.empty()) return {};
			if(!Result.empty()) Result += ' ';
			Result += Line;
		}
		// 積んだ本文の返戻
		return Result;
	}

	/**
	 * 置けないコメントの脱落の診断関数
	 * 通常コメントは種別・所属・コードの意味を保てる位置が無ければ除き，其の事実を診断用設定で確かめれる様にする（整形仕様 7.7, 10.5）
	 * 診断は理由と字面の長さだけを出し，コメントの本文を出力しない
	 * @param Text 除くコメントの字面
	 * @param Reason 置けない理由
	 */
	void CommentPlacer::ReportDrop(const std::string_view Text, const std::string_view Reason) {
		// 環境変数 `SHAVEFMT_WARNINGS` が在る時だけの脱落の診断（値の中身は問わない）
		if(std::getenv("SHAVEFMT_WARNINGS")) {
			std::fprintf(stderr, "[warn] comment dropped (%.*s): %zu bytes\n", static_cast<int>(Reason.size()), Reason.data(), Text.size());
		}
		// 終了
		return;
	}

	/**
	 * 配置枠に応じた出力記法の本文の取得関数（整形仕様 7.6）
	 * 通常の独立・行末コメントの出力記法を規約の行形式へ揃え，行末コメントは文毎の記号を保って行末へ積む
	 * 文中と指示子と文書の記法は其の儘とする
	 * @param Rules 投影済の記法
	 * @param Found 置くコメント（元の記録，記号と本文を合成した字面，文書か）
	 * @param Where 配置枠の種類
	 * @param IsOpaque 行形式へ揃えると読み直せ無く為る範囲へ置くか（前処理指令の本文・読めなかった区画）
	 * @param Buffer 揃えた本文の格納先（返戻の参照先に為り得る）
	 * @return 出力する本文
	 */
	std::string_view CommentPlacer::RenderBody(
		const NotationRules &Rules,
		const Candidate &Found,
		const CommentSlotKind Where,
		const bool IsOpaque,
		std::string &Buffer
	) {
		const CommentRecord &Record = *Found.Record;
		const std::string_view Text = Found.Text;
		// 文中の枠の行コメント（PHP の閉じタグの前へ置いた物）は，行末と同じく揃えて積む
		const bool IsInlineLineComment = Where == CommentSlotKind::Inline && OpensLineComment(Text);
		// 揃える対象で無い記録の返戻
		if(Where == CommentSlotKind::Inline && !IsInlineLineComment || Record.Role == CommentRole::Directive) return Text;
		// 行形式へ揃えた本文（揃えた本文を行コメントとして読み直すと別の本文へ正規化される場合は，本文を保てない為に元の記法を保つ）
		std::string LineForm =
		IsOpaque ? std::string() : LineFormOf(Rules, Record, Found.Output->Text, Found.IsDocument, Found.KeptBlanks);
		const std::string_view Chosen = LineForm.empty() ? Text : std::string_view(LineForm);
		if(Where != CommentSlotKind::LineEnd && !IsInlineLineComment || Chosen.find('\n') == std::string_view::npos) {
			Buffer = std::move(LineForm);
			// 独立した行と一行の行末の記録の返戻
			return Buffer.empty() ? Text : std::string_view(Buffer);
		}
		// 文毎に分けた行末の記録は，行形式を優先して各文の記号の儘行末へ積む（積めない行を含む物は元の記法の儘，配置の判定へ委ねる）
		for(const std::string_view Option : { Chosen, Text }) {
			std::string Stacked = StackLineEnd(Option);
			if(Stacked.empty()) continue;
			Buffer = std::move(Stacked);
			// 積んだ本文の返戻（正規化済の論理行は句点を持たず，積んだ記号の前で文を終えない為，読み直しても同じ本文に為る）
			return Buffer;
		}
		// 積めない記録の元の記法の返戻
		return Text;
	}

	/**
	 * 行末へ置けない行末コメントかの判定関数（整形仕様 7.6, 7.7）
	 * 一行の本文は行末へ積み，行コメントの後の物は其の本文へ続く（処理系の読むコメントの本文を変える積み方はしない）
	 * 積めない複数行の本文は，行コメントの後なら後の行がコードに化け，二行目から別のコメントが始まる物は其のコメントが独立コメントに変わる
	 * @param Text 元の記録の字面（脱落の診断に使う）
	 * @param Body 出力する本文
	 * @param IsVerbatim 本文を原文の儘描く記録か（処理系が読むコメント）
	 * @param IsAfterLineComment 同じ行で先に行コメントを開いたか
	 * @param IsLineCommentVerbatim 先に開いた行コメントが処理系の読むコメントか
	 * @return 置けないなら true（脱落を診断する）
	 */
	bool CommentPlacer::IsUnplaceableLineEnd(
		const std::string_view Text,
		const std::string_view Body,
		const bool IsVerbatim,
		const bool IsAfterLineComment,
		const bool IsLineCommentVerbatim
	) {
		const size_t Newline = Body.find('\n');
		if(IsAfterLineComment) {
			// 行コメントの後に一行の説明を置ける事の返戻
			if(!IsVerbatim && !IsLineCommentVerbatim && Newline == std::string_view::npos) return false;
		} else {
			std::string_view Next = Newline == std::string_view::npos ? std::string_view() : Body.substr(Newline + 1);
			while(!Next.empty() && (Next.front() == ' ' || Next.front() == '\t')) Next.remove_prefix(1);
			// 次の行が新しいコメントを開かない事の返戻
			if(!OpensLineComment(Next) && !Next.starts_with(CommentRepository::OpenerOf(CommentNotation::Block))) return false;
		}
		// 脱落の診断（整形仕様 7.7, 10.5）
		ReportDrop(Text, "no line-end position");
		// 置けない事の返戻
		return true;
	}

	/**
	 * コメント本文の描画関数
	 * 合成したコメントの字面を，錨の深さのインデントを付けた物理行の並びへ写す（処理系が読むコメントの続きの行は原文の儘）
	 * @param TextView コメント本文
	 * @param Tabs 継続行（と `IsIndentFirst` の時の先頭行）へ付けるタブの段数
	 * @param IsIndentFirst 先頭行にもインデントを付けるか
	 * @param IsVerbatim 本文の続きの行を原文の儘描くか（処理系が読むコメントは，続きの行の字下げと `*` を揃えない）
	 * @param HasHeadFrames 行頭（桁０）の開始・終端の行条件を持つ埋込文書を含み得るか（其の開始・終端の行は字下げせず，間の本文の行は `*` を揃えない）
	 * @param Written 出力の大きさの見積（コードと描画済のコメントの字数，描画する行の字数を加える）
	 * @return 物理行の並び
	 * @throws FormatLimitExceeded 字下げを付けた行で出力が再入力と同じ上限を超える場合
	 */
	std::vector<std::string> CommentPlacer::RenderLines(
		const std::string_view TextView,
		const size_t Tabs,
		const bool IsIndentFirst,
		const bool IsVerbatim,
		const bool HasHeadFrames,
		size_t &Written
	) {
		const std::string_view Opener = CommentRepository::OpenerOf(CommentNotation::RubyEmbeddedDocument);
		const std::string_view Closer = CommentRepository::CloserOf(CommentNotation::RubyEmbeddedDocument);
		std::vector<std::string> Lines;
		// 埋込文書の開始の行の後で終端の行の前か
		bool IsInFrame = false;
		for(size_t Cursor = 0;;) {
			// 改行区切毎の本文の行
			const size_t NewlinePos = TextView.find('\n', Cursor);
			std::string_view Line =
			TextView.substr(Cursor, NewlinePos == std::string_view::npos ? std::string_view::npos : NewlinePos - Cursor);
			const size_t Emitted = Lines.size();
			// 埋込文書の開始・終端の行と本文の行
			const bool IsFrameLine = HasHeadFrames && Line == (IsInFrame ? Closer : Opener), IsFrameBody = IsInFrame && !IsFrameLine;
			if(IsFrameLine) IsInFrame ^= true;
			// 最初の行と，処理系が読むコメントでない物の続きの行のインデント（処理系が読むコメントの続きの行と，埋込文書の開始・終端の行は原文の儘）
			const bool IsIndented = !IsFrameLine && (Emitted ? IsFrameBody || !IsVerbatim : IsIndentFirst);
			// 字下げを作る前の出力の大きさの見積と，再入力と同じ 16 MiB の上限の確認（整形仕様 9.1）
			Written += (IsIndented ? Tabs + 1 : 0) + Line.size() + 1;
			if(Written > MaxInputBytes) throw FormatLimitExceeded("output exceeds size limit");
			std::string &Out = Lines.emplace_back();
			if(IsIndented) {
				// `*` で始まる継続行の文書化コメント位置への整合
				const size_t First = Line.find_first_not_of(" \t");
				const bool IsStarLine = Emitted && !IsVerbatim && !IsFrameBody && First != std::string_view::npos && Line[First] == '*';
				if(IsStarLine) Line.remove_prefix(First);
				Out.append(Tabs, '\t');
				if(IsStarLine) Out += ' ';
			}
			Out.append(Line);
			// 最終行の描画後の終了
			if(NewlinePos == std::string_view::npos) break;
			// 次の本文行への走査位置更新
			Cursor = NewlinePos + 1;
		}
		// 物理行の並びの返戻
		return Lines;
	}

	/**
	 * 配置計画の構築関数
	 * 描画済のコメントを断片・種類・位置・描画の順に並べ，コメント行の列の条件を満たせないコメントは整形仕様 7.7 に従って除く
	 * @param Code 配置枠を与えた出力
	 * @param Items 描画済のコメント
	 * @return コメントの配置計画
	 * @throws FormatLimitExceeded 文書の物理行が記法の条件を満たせない場合（文書は脱落させず，文書の保持違反の失敗とする，整形仕様 7.7, 8.3）
	 */
	CommentPlacementPlan CommentPlacer::BuildPlan(const LaidOutCode &Code, std::vector<RenderedComment> &&Items) {
		// 合成の順（断片・種類・位置・計画の順）への整列
		std::sort(
			Items.begin(),
			Items.end(),
			[](const RenderedComment &Lhs, const RenderedComment &Rhs) -> bool {
				// 左右要素の比較の返戻
				return std::tie(Lhs.Slot.Fragment, Lhs.Slot.Kind, Lhs.Slot.Offset, Lhs.Sequence) <
				std::tie(Rhs.Slot.Fragment, Rhs.Slot.Kind, Rhs.Slot.Offset, Rhs.Sequence);
			}
		);
		// コメント行の成立判定と配置計画への追加
		CommentPlacementPlan Plan(Code);
		for(RenderedComment &Item : Items) {
			std::optional<CommentBlock> Block = CommentBlock::Make(Code.GetAnchors().Syntax->Rules().Notation, std::move(Item.Lines));
			if(!Block) {
				// 文書は脱落させずに整形器の不具合とする（整形仕様 7.7）
				if(Item.IsRequired) {
					throw FormatLimitExceeded("documentation comment lines do not satisfy the notation", FailureReason::DocumentChanged);
				}
				// 脱落の診断（整形仕様 7.7, 10.5）
				ReportDrop(*Item.Text, "comment lines do not satisfy the notation");
				continue;
			}
			Plan.Add(Item.Slot, std::move(*Block));
		}
		// 配置計画の返戻
		return Plan;
	}

	/**
	 * 字面の位置を含む断片の取得関数
	 * @param Code 変更不能なコードの出力
	 * @param Offset 字面の位置
	 * @return 位置を含む断片（行末の位置は其の行）
	 */
	uint32_t CommentPlacer::FragmentAt(const LaidOutCode &Code, const uint32_t Offset) {
		const std::span<const CodeFragment> Fragments = Code.GetFragments();
		const std::span<const CodeFragment>::iterator After = std::upper_bound(
			Fragments.begin(),
			Fragments.end(),
			Offset,
			[](const uint32_t Value, const CodeFragment &Fragment) -> bool {
				// 断片の始まりとの比較の返戻
				return Value < Fragment.Start;
			}
		);
		// 位置以前に始まる最後の断片の返戻
		return After == Fragments.begin() ? 0 : static_cast<uint32_t>(After - Fragments.begin() - 1);
	}

	/**
	 * 字面の位置より前に始まる最後の字句片の取得関数
	 * @param Code 変更不能なコードの出力
	 * @param Offset 字面の位置
	 * @return 位置より前に始まる最後の字句片（無ければ無し）
	 */
	const CodePiece *CommentPlacer::PieceBefore(const LaidOutCode &Code, const uint32_t Offset) {
		const std::vector<CodePiece> &Pieces = Code.GetAnchors().Pieces;
		const std::vector<CodePiece>::const_iterator After = std::lower_bound(
			Pieces.begin(),
			Pieces.end(),
			Offset,
			[](const CodePiece &Piece, const uint32_t Position) -> bool {
				// 字句片の始まりとの比較の返戻
				return Piece.Start < Position;
			}
		);
		// 位置より前に始まる最後の字句片の返戻
		return After == Pieces.begin() ? nullptr : &*(After - 1);
	}

	/**
	 * 字面の位置の字を含む字句片の取得関数
	 * @param Code 変更不能なコードの出力
	 * @param Offset 字面の位置
	 * @return 位置の字を含む字句片（区切の空白・インデント・改行の位置は無し）
	 */
	const CodePiece *CommentPlacer::PieceCovering(const LaidOutCode &Code, const uint32_t Offset) {
		const CodePiece *const Found = Code.PieceFrom(Offset);
		// 位置の字を含む字句片の返戻
		return Found && Found->Start <= Offset ? Found : nullptr;
	}

	/**
	 * 字句片の形の判定関数
	 * @param Piece 字句片（無しを許す）
	 * @param Shapes 求める形の何れか (`CodePiece::Shape`)
	 * @return 字句片が形の何れかを持てば true
	 */
	bool CommentPlacer::HasShape(const CodePiece *const Piece, const uint16_t Shapes) {
		// 形の有無の返戻
		return Piece && Piece->Shapes & Shapes;
	}

	/**
	 * 同じ行の位置以後の最初のコードの位置の取得関数
	 * @param Code 変更不能なコードの出力
	 * @param Offset 字面の位置
	 * @return 位置以後で区切の空白でない最初の位置（同じ行にコードが続かなければ行末）
	 */
	uint32_t CommentPlacer::ContentFrom(const LaidOutCode &Code, const uint32_t Offset) {
		const CodeFragment &Line = Code.GetFragments()[FragmentAt(Code, Offset)];
		const CodePiece *const Next = Code.PieceFrom(Offset);
		// 同じ行のコードの位置か行末の返戻
		return Next && Next->Start < Line.End ? std::max(Offset, Next->Start) : std::max(Offset, Line.End);
	}

	/**
	 * 字面の位置の直前が空の波括弧かの判定関数
	 * @param Code 変更不能なコードの出力
	 * @param Offset 字面の位置
	 * @return 位置の前の２字が `{}` なら true
	 */
	bool CommentPlacer::EndsWithEmptyBraces(const LaidOutCode &Code, const uint32_t Offset) {
		const CodePiece *const Last = Offset ? PieceCovering(Code, Offset - 1) : nullptr;
		// 位置で終わる字句片の無い事の返戻
		if(!Last || Last->End != Offset) return false;
		const CodePiece *const Open = Last->Start ? PieceCovering(Code, Last->Start - 1) : nullptr;
		// 開き波括弧の字句片に密着した閉じ波括弧だけの字句片の返戻
		return Last->End - Last->Start == 1 && Last->Shapes & CodePiece::StartsClosingBrace && Open && Open->End == Last->Start &&
		Open->Shapes & CodePiece::EndsOpeningBrace;
	}

	/**
	 * 字面の位置の直前の字の種類の取得関数
	 * @param Code 変更不能なコードの出力
	 * @param Offset 字面の位置（行のインデントの後の位置以後）
	 * @param Content 行のインデントの後の位置
	 * @return 位置の直前の字の種類（区切の空白，インデントのタブ，開き括弧，其の他のコード）
	 */
	CommentPlacer::Adjacent CommentPlacer::AdjacentBefore(const LaidOutCode &Code, const uint32_t Offset, const uint32_t Content) {
		if(const CodePiece *const Piece = Offset ? PieceCovering(Code, Offset - 1) : nullptr) {
			// 字句片の最後の字の種類の返戻
			return Piece->End == Offset && Piece->Shapes & CodePiece::EndsOpeningBracket ? Adjacent::OpenBracket : Adjacent::Code;
		}
		// 区切の空白かインデントのタブの返戻
		return Offset > Content ? Adjacent::Space : Adjacent::Tab;
	}

	/**
	 * 描画したコメントの最後の字の種類の取得関数
	 * @param Char コメントの物理行の最後の字
	 * @return 字の種類
	 */
	CommentPlacer::Adjacent CommentPlacer::AdjacentOf(const char Char) {
		// 字の種類の返戻
		return Char == ' ' ? Adjacent::Space : Char == '\t' ?
		Adjacent::Tab :
		Char == '\n' ? Adjacent::Newline : IsOpenBracketChar(Char) ? Adjacent::OpenBracket : Adjacent::Code;
	}

	/**
	 * 字面の位置の前に同じ行のコードが在るかの判定関数
	 * @param Code 変更不能なコードの出力
	 * @param Offset 字面の位置
	 * @return 行頭のタブの後から位置迄に空白以外の字が在れば true
	 */
	bool CommentPlacer::IsAfterCode(const LaidOutCode &Code, const uint32_t Offset) {
		const CodeFragment &Line = Code.GetFragments()[FragmentAt(Code, Offset)];
		const CodePiece *const First = Code.PieceFrom(Line.Start);
		// 行の中の位置より前に始まる字句片の有無の返戻（字句片は行を跨がない）
		return First && First->Start < Offset && First->Start < Line.End;
	}

	/**
	 * 字面の位置の後に同じ行のコードが続くかの判定関数
	 * @param Code 変更不能なコードの出力
	 * @param Offset 字面の位置
	 * @return 位置から行末迄に空白以外の字が在れば true
	 */
	bool CommentPlacer::IsFollowedByCode(const LaidOutCode &Code, const uint32_t Offset) {
		const CodeFragment &Line = Code.GetFragments()[FragmentAt(Code, Offset)];
		const CodePiece *const Next = Code.PieceFrom(Offset);
		// 行の中の位置より後で終わる字句片の有無の返戻（字句片は区切の空白を含まない）
		return Next && Next->Start < Line.End;
	}

	/**
	 * 錨の直前に続く無名の字句の並びの始まりの取得関数
	 * 原文でコメントと錨の間に在った字句の数だけ遡り，閉じ括弧・名前付の節点と錨を含まない節点の字句で止める
	 * @param Code 変更不能なコードの出力
	 * @param Syntax 読取専用の期待構文
	 * @param Anchor 錨の節点
	 * @param Limit 遡る字句の数の上限
	 * @return 遡った字句の始まり（遡れなければ錨の始まり）
	 */
	uint32_t CommentPlacer::TokenRunStart(
		const LaidOutCode &Code,
		const SyntaxModel &Syntax,
		const NodeId Anchor,
		const uint16_t Limit
	) {
		const std::vector<NodeSpan> &Spans = Code.GetAnchors().Nodes;
		uint32_t Start = Spans[Anchor].Start;
		uint16_t Taken = 0;
		for(NodeId Current = Anchor; Taken < Limit;) {
			NodeId Previous = Syntax.PrevSibling(Current);
			while(Previous == NoNode) {
				const NodeId Parent = Syntax.ParentOf(Current);
				if(Parent == NoNode) break;
				Current = Parent;
				Previous = Syntax.PrevSibling(Current);
			}
			// 出力に現れない字句を飛ばす
			while(Previous != NoNode && (Previous >= Spans.size() || !Spans[Previous].IsPlaced) && !Syntax.IsNamed(Previous)) {
				Previous = Syntax.PrevSibling(Previous);
			}
			if(
				Previous == NoNode || Syntax.IsNamed(Previous) || Syntax.ChildCount(Previous) ||
				Syntax.IsTokenRole(Previous, Role::ClosingParenToken) || Syntax.IsTokenRole(Previous, Role::ClosingSquareToken) ||
				Syntax.IsTokenRole(Previous, Role::ClosingBraceToken)
			) break;
			Start = Spans[Previous].Start;
			Current = Previous;
			++Taken;
		}
		// 遡った字句の始まりの返戻
		return Start;
	}

	/**
	 * 錨の後の文中コメントの置場の取得関数（整形仕様 7.6）
	 * 原文でコメントと錨の間に在った無名の字句を同じ行の中で飛ばし，錨の後に同じ行のコードが続けば錨の直後へ置く
	 * 続かなければ錨を終える区切り・閉じの字句の前，其れも無ければ錨の直前へ置く
	 * @param Code 変更不能なコードの出力
	 * @param Syntax 読取専用の期待構文
	 * @param Record コメントの記録
	 * @param HasInner 錨の中の節点に紐付いた他のコメントが有るか
	 * @return 置場の字面の位置（置けなければ無し）
	 */
	std::optional<uint32_t> CommentPlacer::InlineAfter(
		const LaidOutCode &Code,
		const SyntaxModel &Syntax,
		const CommentRecord &Record,
		const bool HasInner
	) {
		const std::vector<NodeSpan> &Spans = Code.GetAnchors().Nodes;
		const NodeId Anchor = Record.Anchor;
		const NodeSpan &Span = Spans[Anchor];
		// 錨の中の無名の字句の前（数が減った時は最後の字句の前）
		if(Record.Placement.Side == CommentPlacement::Inside) {
			std::vector<uint32_t> Tokens;
			for(const NodeId Child : Syntax.ChildrenOf(Anchor)) {
				if(!Syntax.IsNamed(Child) && Child < Spans.size() && Spans[Child].IsPlaced) Tokens.push_back(Spans[Child].Start);
			}
			// 字句の前か，字句が無い事の返戻
			return Tokens.empty() ?
			std::nullopt :
			std::optional<uint32_t>(Tokens[std::min<size_t>(Record.Placement.TokensBefore, Tokens.size() - 1)]);
		}
		// 錨の後に同じ行で続く同じ親の無名の字句を飛ばした後（閉じ字句と行末の前で止める）
		if(Record.Placement.TokensBefore) {
			std::optional<uint32_t> RunEnd;
			uint16_t Taken = 0;
			for(
				NodeId Next = Syntax.NextSibling(Anchor);
				Next != NoNode && Taken < Record.Placement.TokensBefore;
				Next = Syntax.NextSibling(Next)
			) {
				if(Next >= Spans.size() || !Spans[Next].IsPlaced) continue;
				if(Syntax.IsNamed(Next) || Syntax.Is(Next, Role::CloserToken) || Spans[Next].StartFragment != Span.EndFragment) break;
				if(!IsFollowedByCode(Code, Spans[Next].End)) break;
				RunEnd = Spans[Next].End;
				++Taken;
			}
			// 飛ばした字句の後の返戻
			if(RunEnd) return RunEnd;
		}
		// 錨の後に同じ行のコードが続く場合は錨の直後（空の括弧が続けば其の中）
		if(IsFollowedByCode(Code, Span.End)) {
			const uint32_t After = ContentFrom(Code, Span.End);
			const CodePiece *const Next = Code.PieceAt(After);
			const bool IsEmptyParens = HasShape(Next, CodePiece::StartsEmptyParens) ||
			HasShape(Next, CodePiece::IsOpenParen) && HasShape(Code.PieceAt(Next->End), CodePiece::StartsCloseParen);
			// 錨の直後か空の括弧の中の返戻
			return IsEmptyParens ? After + 1 : Span.End;
		}
		// 錨の最後の字句（文字列等の字面は１つの字句）
		NodeId Last = Anchor;
		while(true) {
			const uint32_t ChildCount = Syntax.ChildCount(Last);
			if(!ChildCount || Syntax.Is(Last, Role::LiteralInterior)) break;
			NodeId Child = NoNode;
			for(uint32_t Index = ChildCount; Index-- && Child == NoNode;) {
				if(const NodeId Candidate = Syntax.Child(Last, Index); Candidate < Spans.size() && Spans[Candidate].IsPlaced) Child = Candidate;
			}
			if(Child == NoNode) break;
			Last = Child;
		}
		// 最後の字句が同じ行のコードの後の錨を終える区切り・閉じの字句か，錨の中に他のコメントが有れば其の字句の前
		if(const NodeSpan &LastSpan = Spans[Last]; Last != Anchor && LastSpan.Start > Span.Start && IsAfterCode(Code, LastSpan.Start)) {
			const bool IsTerminator = !Syntax.IsNamed(Last) && (
				Syntax.IsTokenRole(Last, Role::StatementTerminatorToken) || Syntax.IsTokenRole(Last, Role::CommaToken) ||
				Syntax.IsTokenRole(Last, Role::ClosingParenToken) || Syntax.IsTokenRole(Last, Role::ClosingSquareToken) ||
				Syntax.IsTokenRole(Last, Role::ClosingBraceToken)
			);
			// 其の字句の前の返戻
			if(HasInner || IsTerminator) return LastSpan.Start;
		}
		// 錨の直前か，錨の中に他のコメントが有り順を保てない事の返戻
		return HasInner || Anchor == Syntax.GetRoot() ? std::nullopt : std::optional<uint32_t>(Span.Start);
	}

	/**
	 * 文中コメントを置く行頭の後の文を解析器が読めるかの判定関数（整形仕様 7.7）
	 * tree-sitter-swift は文を終えた改行の次の行の頭に在る囲みコメントの後の文を読めない為，Swift では其処へ置かない
	 * 前に同じ行のコードが在る所，演算子・閉じ字句で前の式へ続く所，節の見出の前，文の並びの外，前の字句が文を終えない所は読める
	 * @param Code 変更不能なコードの出力
	 * @param Syntax 読取専用の期待構文
	 * @param Anchor 錨
	 * @param At 置場の字面の位置
	 * @return 置いても読めるなら true
	 */
	bool CommentPlacer::IsStatementHeadReadable(
		const LaidOutCode &Code,
		const SyntaxModel &Syntax,
		const NodeId Anchor,
		const uint32_t At
	) {
		// 改行で文を終えない言語と，行の途中と，演算子・閉じ字句で前の式へ続く所の返戻
		if(
			!Syntax.Rules().Notation.BlockCommentInterruptsStatementHead || IsAfterCode(Code, At) ||
			HasShape(Code.PieceAt(At), CodePiece::StartsOperator | CodePiece::StartsClosingBracket)
		) return true;
		const std::vector<NodeSpan> &Spans = Code.GetAnchors().Nodes;
		// 位置から始まる祖先を辿った最初の位置より前から始まる祖先
		NodeId Up = Anchor;
		while(Up != NoNode && Up < Spans.size() && Spans[Up].IsPlaced && Spans[Up].Start >= At) {
			// 節の見出で始まる所の返戻
			if(Syntax.Is(Up, Role::SectionHeader)) return true;
			Up = Syntax.ParentOf(Up);
		}
		// 文の並びの外の返戻
		if(Up == NoNode || !Syntax.Is(Up, Role::StatementHost)) return true;
		const CodePiece *const Previous = PieceBefore(Code, At);
		// 前の字句が無いか文を終えない（演算子・開き括弧・区切りの後で，後置の増減の後でない）事の返戻
		return !Previous || Previous->Shapes & CodePiece::EndsContinuation;
	}

	/**
	 * 配置枠の決定関数（整形仕様 7.6）
	 * 錨の節点の出力上の範囲と，原文での種類・所属から配置枠を決める
	 * 独立コメントは原文で後に続いた字句の在る行（無ければ後続のコード行）の前へ其の行の深さで，行末コメントは錨の終わる行の行末，文中コメントは同じ構文の字句の境界へ置く
	 * @param Code 変更不能なコードの出力
	 * @param Syntax 読取専用の期待構文（錨の所属を引く）
	 * @param Comments 錨の節点毎のコメント
	 * @param Outputs 記録毎の出力記法の記録
	 * @param Structure 出力記法の記録の本文構造
	 * @return 配置枠を決めたコメント（原文順）
	 * @throws FormatLimitExceeded 錨の節点が出力に現れない場合（整形器の不具合で，置けないコメントとして隠さない，整形仕様 7.7）
	 */
	std::vector<CommentPlacer::Candidate> CommentPlacer::CollectCandidates(
		const LaidOutCode &Code,
		const SyntaxModel &Syntax,
		const CommentRepository &Comments,
		const std::span<const CommentRecord> Outputs,
		const CommentStructure &Structure
	) {
		const AnchorPositionMap &Anchors = Code.GetAnchors();
		const std::span<const CodeFragment> Fragments = Code.GetFragments();
		const NotationRules &Rules = Syntax.Rules().Notation;
		const std::span<const CommentRecord> Records = Comments.GetRecords();
		// 行末・文中のコメントの有無（其の種類だけが読む索引は在る時だけ作る）
		bool HasTrailing = false, HasInline = false;
		for(const CommentRecord &Record : Records) {
			HasTrailing = HasTrailing || Record.Kind == CommentKind::Trailing;
			HasInline = HasInline || Record.Kind == CommentKind::Inline;
			if(HasTrailing && HasInline) break;
		}
		// 行末のコメントだけが読む，本文へ取り込まない為の開きの行毎の要素の索引と，コードの閉じの出力順の位置表（コメント毎に構文全体を走査しない）
		std::unordered_map<uint32_t, std::vector<NodeId>> MarkupByLine;
		std::vector<std::pair<uint32_t, uint32_t>> CodeBoundaries;
		if(HasTrailing) {
			for(NodeId Id = 0; Id < Anchors.Nodes.size(); ++Id) {
				const NodeSpan &Span = Anchors.Nodes[Id];
				if(!Span.IsPlaced) continue;
				if(Syntax.Is(Id, Role::MarkupElement)) MarkupByLine[Span.StartFragment].push_back(Id);
				if(Syntax.Is(Id, Role::CodeBoundary)) CodeBoundaries.emplace_back(Span.Start, Span.StartFragment);
			}
			std::sort(CodeBoundaries.begin(), CodeBoundaries.end());
		}
		// 記録毎に祖先を辿らない為に一度の走査で求める節点毎の表（整形仕様 9.2）
		// 最も近い埋込の式・語の塊か節（自身を含む），隣接する文書を取り込む並び（Go の最上位）の中の最上位の祖先（自身を含む），最も近い埋込の要素（自身を含む）
		std::vector<NodeId> NearestMarkup(Syntax.NodeCount(), NoNode), NearestKeyword(Syntax.NodeCount(), NoNode);
		std::vector<NodeId> TopLevel(Syntax.NodeCount(), NoNode), NearestElement(Syntax.NodeCount(), NoNode);
		for(std::vector<NodeId> Pending { Syntax.GetRoot() }; !Pending.empty();) {
			const NodeId Node = Pending.back();
			Pending.pop_back();
			const NodeId Parent = Syntax.ParentOf(Node);
			NearestMarkup[Node] = Syntax.Is(Node, Role::MarkupExpression) ? Node : Parent == NoNode ? NoNode : NearestMarkup[Parent];
			NearestKeyword[Node] = Syntax.Is(Node, Role::KeywordBlock) || Syntax.Is(Node, Role::KeywordSection) ?
			Node :
			Parent == NoNode ? NoNode : NearestKeyword[Parent];
			TopLevel[Node] = Syntax.Is(Parent, Role::AdjacentDocumentSequence) ? Node : Parent == NoNode ? NoNode : TopLevel[Parent];
			NearestElement[Node] = Syntax.Is(Node, Role::MarkupElement) ? Node : Parent == NoNode ? NoNode : NearestElement[Parent];
			for(const NodeId Child : Syntax.ChildrenOf(Node)) Pending.push_back(Child);
		}
		// 記録毎の文書の説明対象と，確定した配置で文書として置く物（文書と認識しない記録と置かない記録は無し，整形仕様 7.4, 7.6）
		const std::vector<NodeId> Recognized = DocCommentSynchronizer::DocumentTargets(Syntax, Comments);
		const std::vector<NodeId> Documents = DocCommentSynchronizer::PlacedTargets(Code, Comments.GetRecords(), Recognized);
		// 錨の中の節点に紐付いた他のコメントの有る錨（末尾の物と，錨の頭でない先行の物，文中のコメントだけが読む）
		std::vector<uint8_t> HasInner(HasInline ? Anchors.Nodes.size() : 0, 0);
		// コードの出力を持たない根に結び付いたコメントだけのファイルの物か（文書の独立した出力位置を用いる）
		const auto IsDocumentComment = [&](const CommentRecord &Record) -> bool {
			// 出力に現れない根の錨かの返戻
			return Record.Anchor == Syntax.GetRoot() && !Anchors.Nodes[Record.Anchor].IsPlaced;
		};
		// 節点毎の自身へ紐付いた記録の要約（先行でない物が在るか，先行の物の錨の最も後の始まり）
		std::vector<uint8_t> HasOwnAfter(HasInner.size(), 0);
		std::vector<int64_t> OwnBeforeStart(HasInner.size(), -1);
		for(const CommentRecord &Record : Records) {
			if(IsDocumentComment(Record)) continue;
			if(!Anchors.Nodes[Record.Anchor].IsPlaced) {
				throw FormatLimitExceeded("a comment anchor has no output position", FailureReason::FormatterDefect);
			}
			if(!HasInline || Record.Placement.IsRemovedCaseBody && Record.Kind == CommentKind::Inline) continue;
			if(Record.Placement.Side != CommentPlacement::Before) HasOwnAfter[Record.Anchor] = 1;
			else OwnBeforeStart[Record.Anchor] = std::max<int64_t>(OwnBeforeStart[Record.Anchor], Anchors.Nodes[Record.Anchor].Start);
		}
		// 子孫の要約を後順に集め，先行でない物か，祖先より後から始まる錨の先行の物を子孫に持つ祖先へ印を付ける（記録毎に祖先を辿らない，整形仕様 9.2）
		std::vector<uint8_t> HasSubAfter(HasInner.size(), 0);
		std::vector<int64_t> SubBeforeStart(HasInner.size(), -1);
		if(HasInline) for(std::vector<std::pair<NodeId, bool>> Pending { { Syntax.GetRoot(), false } }; !Pending.empty();) {
			const auto [Node, IsDone] = Pending.back();
			Pending.pop_back();
			if(!IsDone) {
				Pending.emplace_back(Node, true);
				for(const NodeId Child : Syntax.ChildrenOf(Node)) Pending.emplace_back(Child, false);
				continue;
			}
			for(const NodeId Child : Syntax.ChildrenOf(Node)) {
				HasSubAfter[Node] = HasSubAfter[Node] || HasOwnAfter[Child] || HasSubAfter[Child];
				SubBeforeStart[Node] = std::max({ SubBeforeStart[Node], OwnBeforeStart[Child], SubBeforeStart[Child] });
			}
			HasInner[Node] = HasSubAfter[Node] || SubBeforeStart[Node] > static_cast<int64_t>(Anchors.Nodes[Node].Start);
		}
		// 文書の論理行でない埋込文書の前後の空の行（囲みの開始記号・終端記号だけの行と同じく，構造として保つ時だけ置く）
		const std::vector<bool> EmbeddedEdges = CommentRepository::EmbeddedEdgesOf(Records);
		// 置かない内容の無い通常のコメント行か（文書の空の論理行は文書の側で保ち，コード例の中の行と段落とコード例を分ける行は構造として保つ，整形仕様 7.3, 7.7）
		const auto IsContentless = [&Outputs, &Structure](const size_t Index) -> bool {
			// 説明以外の記録は本文の空白を調べず残す
			if(Outputs[Index].Role != CommentRole::Presentation) return false;
			// 本文を持つ記録は保つ空の行を調べず残す
			if(Outputs[Index].Text.find_first_not_of(" \t\n") != std::string::npos) return false;
			const std::span<const uint8_t> KeptBlanks = Structure.GetKeptBlanks(Index);
			// 説明の本文が空で，保つ空の論理行を持たないかの返戻
			return std::ranges::find(KeptBlanks, 1) == KeptBlanks.end();
		};
		std::vector<Candidate> Candidates;
		// 断片毎に其の終わりの改行が地の文の中に在るか（使う時に初めて作る）
		std::optional<std::vector<uint8_t>> BreaksInText;
		for(size_t RecordIndex = 0; RecordIndex < Records.size(); ++RecordIndex) {
			const CommentRecord &Record = Records[RecordIndex];
			if(IsDocumentComment(Record)) {
				// 内容の無い通常コメントの読飛（整形仕様 7.7）
				if(IsContentless(RecordIndex)) continue;
				// 文書の末尾（コードの断片の後）の独立した行
				Candidate Found;
				Found.Record = &Record;
				Found.Output = &Outputs[RecordIndex];
				Found.KeptBlanks = Structure.GetKeptBlanks(RecordIndex);
				CommentRecord Output = Outputs[RecordIndex];
				if(Record.Role == CommentRole::Presentation) DropEmptyLogicalLines(Output, Found.KeptBlanks);
				Found.Text = CommentRepository::Compose(Output);
				Found.IsVerbatim = Record.Role == CommentRole::Directive;
				Found.Where = CommentSlotKind::DocumentEnd;
				Found.Fragment = static_cast<uint32_t>(Fragments.size());
				Candidates.push_back(std::move(Found));
				continue;
			}
			const NodeSpan &Span = Anchors.Nodes[Record.Anchor];
			// １論理行へ繋いだ前処理指令（条件の塊を成す指令は其の語の見出の行）の中の物は，独立行の種別を保てず，改行を含む物は指令を途中で終える為に除く（整形仕様 7.7）
			if(
				Record.Placement.Side == CommentPlacement::Inside && (
					Syntax.Is(Record.Anchor, Role::PreprocessorLine) ||
					!Syntax.IsNamed(Record.Anchor) && Syntax.Is(Syntax.ParentOf(Record.Anchor), Role::PreprocessorBranch)
				)
			) {
				ReportDrop(CommentRepository::Compose(Outputs[RecordIndex]), "no position inside a joined preprocessor directive");
				continue;
			}
			// 空の case 本体で後続の字句を失った文中コメントは種別を変えずに除く
			if(Record.Placement.IsRemovedCaseBody && Record.Kind == CommentKind::Inline) {
				if(Record.Role == CommentRole::Presentation) {
					ReportDrop(CommentRepository::Compose(Outputs[RecordIndex]), "no mid-line position in an empty case body");
				}
				// 指示コメントの候補欠落は独立した検証器が失敗へ分類する
				continue;
			}
			Candidate Found;
			Found.Record = &Record;
			Found.Output = &Outputs[RecordIndex];
			Found.KeptBlanks = Structure.GetKeptBlanks(RecordIndex);
			CommentRecord Output = Outputs[RecordIndex];
			// 文書と認識する記録は記法と移譲済の錨の宣言への所属の双方から決め（整形仕様 7.4），説明対象の前に独立行を持てない物は通常コメントとして描く（整形仕様 7.6）
			const bool IsPlacedDocument = Documents[RecordIndex] != NoNode;
			Found.IsDocument = IsPlacedDocument && Record.Kind == CommentKind::OwnLine;
			Found.IsRequired = Recognized[RecordIndex] != NoNode && Record.Kind == CommentKind::OwnLine;
			// 文書でない内容の無い通常コメントと埋込文書の前後の空の行の読飛と，通常コメントの空の論理行の除去（整形仕様 7.7）
			if((!Found.IsDocument || EmbeddedEdges[RecordIndex]) && IsContentless(RecordIndex)) continue;
			if(Record.Role == CommentRole::Presentation && !IsPlacedDocument && !CommentRepository::IsMemberDocument(Rules, Record)) {
				DropEmptyLogicalLines(Output, Found.KeptBlanks);
				// 文書でも区切でもない文書の囲みの記法は，行形式で保てない時に選ぶ同じ種別の囲みを一般の囲みとする（整形仕様 7.6）
				if(
					!CommentRepository::IsSeparator(Record) && CommentRepository::IsBlockNotation(Output.Notation) &&
					Rules.DocumentFormOf(Output.Notation)
				) Output.Notation = CommentNotation::Block;
			}
			// 文書の位置の囲みの文書は，複数の論理行の開始記号と終端記号を独立した行に置く
			if(
				Record.Kind == CommentKind::OwnLine && Record.Role == CommentRole::Presentation &&
				Rules.IsAvailable(CommentNotation::DoubleStarBlock) && !Output.Format.AttachFirstLine &&
				(Output.Notation == CommentNotation::DoubleStarBlock || Output.Notation == CommentNotation::StarBangBlock) && IsPlacedDocument
			) CommentRepository::FrameDocument(Output);
			Found.Text = CommentRepository::Compose(Output);
			Found.IsVerbatim = Record.Role == CommentRole::Directive;
			const bool IsBefore = Record.Placement.Side == CommentPlacement::Before;
			// 空の式には文中の囲みを戻し，其の他は種別を保って本文の外の配置枠へ移す（整形仕様 7.7）
			NodeId Expression = NearestMarkup[Record.Anchor];
			// 埋込の式の祖先だけを辿り，錨の行を内側に含む最も近い物を選ぶ（要素の本文を跨いで外の式へ遡らず，其の本文の中の物は要素の経路で置く）
			if(Record.Kind == CommentKind::OwnLine) for(NodeId Up = Expression; Up != NoNode;) {
				if(Anchors.Nodes[Up].StartFragment < Span.StartFragment && Span.StartFragment < Anchors.Nodes[Up].EndFragment) {
					Expression = Up;
					break;
				}
				const NodeId Parent = Syntax.ParentOf(Up), Element = Parent == NoNode ? NoNode : NearestElement[Parent];
				Up = Parent == NoNode || Element != NoNode && NearestMarkup[Syntax.ParentOf(Element)] == NearestMarkup[Parent] ?
				NoNode :
				NearestMarkup[Parent];
			}
			if(
				Expression != NoNode && Record.Role != CommentRole::Directive && !(
					Record.Kind == CommentKind::OwnLine && Anchors.Nodes[Expression].StartFragment < Span.StartFragment &&
					Span.StartFragment < Anchors.Nodes[Expression].EndFragment
				)
			) {
				const NodeSpan &ExpressionSpan = Anchors.Nodes[Expression];
				if(
					!Syntax.NamedChildCount(Expression) && Record.Kind == CommentKind::Inline && CommentRepository::IsBlockNotation(Record.Notation)
				) {
					// 空の式の中の文中の囲み（本文へ混ぜない JSX のコメントの形）は種別と記法を其の儘に空の式の中へ置く
					Found.Text = CommentRepository::Compose(Record);
					Found.Where = CommentSlotKind::Inline;
					Found.Offset = ExpressionSpan.Start + 1;
					Found.Fragment = FragmentAt(Code, Found.Offset);
				} else if(Record.Kind == CommentKind::Inline && !OpensLineComment(Found.Text)) {
					Found.Where = CommentSlotKind::Inline;
					Found.Offset =
					IsBefore ? Span.Start : InlineAfter(Code, Syntax, Record, HasInner[Record.Anchor]).value_or(ExpressionSpan.End - 1);
					Found.Fragment = FragmentAt(Code, Found.Offset);
				} else if(Record.Kind == CommentKind::Trailing) {
					NodeId Statement = Expression;
					while(true) {
						const NodeId Parent = Syntax.ParentOf(Statement);
						if(Parent == NoNode || Syntax.Is(Parent, Role::StatementHost) || Syntax.Is(Parent, Role::MemberBody)) break;
						Statement = Parent;
					}
					// 行末の物は文の中で式を含む最も外の要素の終わる行（其の行の末は本文の外）の行末へ置く（整形仕様 7.7）
					NodeId Markup = Expression;
					for(NodeId Up = Expression; Up != Statement; Up = Syntax.ParentOf(Up)) {
						if(Syntax.Is(Up, Role::MarkupElement) || Syntax.Is(Up, Role::MarkupOpen)) Markup = Up;
					}
					Found.Where = CommentSlotKind::LineEnd;
					Found.Fragment = Anchors.Nodes[Markup].EndFragment;
				} else {
					// 独立した物は式を含む要素の内，始まる行の頭（其の行で始まる最も外の節点の親の中）が要素の本文・札の外に在る最も内の物の其の行の前へ置く（整形仕様 7.6, 7.7）
					NodeId Markup = NearestElement[Expression];
					while(true) {
						NodeId Front = Markup;
						while(true) {
							const NodeId Parent = Syntax.ParentOf(Front);
							if(Parent == NoNode || Anchors.Nodes[Parent].StartFragment != Anchors.Nodes[Markup].StartFragment) break;
							Front = Parent;
						}
						const NodeId Context = Syntax.ParentOf(Front), Element = Context == NoNode ? NoNode : NearestElement[Context];
						// 行の頭が埋込の式か要素の外（コードの文脈）に在る場合の打切
						if(Element == NoNode || NearestMarkup[Context] != NoNode && NearestMarkup[Syntax.ParentOf(Element)] != NearestMarkup[Context]) {
							break;
						}
						Markup = Element;
					}
					Found.Where = CommentSlotKind::BeforeLine;
					Found.Fragment = Anchors.Nodes[Markup].StartFragment;
					Found.Tabs = Anchors.FragmentTabs[Found.Fragment];
				}
				Candidates.push_back(std::move(Found));
				continue;
			}
			switch(Record.Kind) {
			case CommentKind::OwnLine:
				if(NearestKeyword[Record.Anchor] != NoNode && Record.Placement.IsEmptyBody && !IsBefore) {
					// 語で閉じる構文の空の本体の物は，原文で其の後に続いた字句（錨の中の後の節か閉じの語，無ければ構文の順で錨の後に続く物）の在る行の前へ置く（整形仕様 7.6）
					// 錨の中は語を出さない本体 (`do` / `then`) の中も前順に探す
					NodeId Successor = NoNode;
					const std::span<const NodeId> Children = Syntax.ChildrenOf(Record.Anchor);
					for(std::vector<NodeId> Pending(Children.rbegin(), Children.rend()); Successor == NoNode && !Pending.empty();) {
						const NodeId Node = Pending.back();
						Pending.pop_back();
						if(Syntax.Is(Node, Role::TransparentSection)) {
							const std::span<const NodeId> Inner = Syntax.ChildrenOf(Node);
							Pending.insert(Pending.end(), Inner.rbegin(), Inner.rend());
						} else if(Syntax.Is(Node, Role::KeywordSection) || Syntax.Is(Node, Role::CloserToken)) Successor = Node;
					}
					if(Successor == NoNode) Successor = Syntax.FollowingOf(Record.Anchor);
					Found.Where = CommentSlotKind::OwnLine;
					Found.Fragment = Span.EndFragment;
					if(Successor != NoNode && Anchors.Nodes[Successor].IsPlaced) {
						Found.Where = CommentSlotKind::BeforeLine;
						Found.Fragment = Anchors.Nodes[Successor].StartFragment;
					}
					break;
				}
				// 独立した行（錨が行内へ移っても行内コメントにしない）
				if(IsBefore) {
					if(
						Syntax.Is(Syntax.ParentOf(Record.Anchor), Role::AdjacentDocumentSequence) &&
						Syntax.GetNode(Record.Anchor).Facts.AbsorbsLeadingComment && CommentRepository::IsSeparator(Record)
					) {
						// Go の宣言の前の区切コメントは，空行を挟まずに続く其の宣言の文書へ取り込まれる為に除く（整形仕様 7.6, 7.7）
						ReportDrop(Found.Text, "a separator would join the next Go declaration's doc");
						continue;
					}
					// 原文でコメントの後に在った字句を戻した位置（畳んで空に為る本体の中の物は錨の頭でなく錨の終わりの `{}` の位置）の在る行の前へ置く
					// 閉じ括弧・節の見出で始まる行も例外としない（整形仕様 7.6）
					const uint32_t Dest = Record.Placement.IsEmptyBody && EndsWithEmptyBraces(Code, Span.End) ?
					Span.End - 2 :
					Record.Placement.TokensBefore ? TokenRunStart(Code, Syntax, Record.Anchor, Record.Placement.TokensBefore) : Span.Start;
					Found.Where = CommentSlotKind::BeforeLine;
					Found.Fragment = FragmentAt(Code, Dest);
				} else {
					Found.Where = CommentSlotKind::OwnLine;
					Found.Fragment = Span.EndFragment;
					// 原文でコメントの後に在った閉じ・節（構文の順で錨の後に続く物）が錨の終わる行に在る物（`} else {` の `else`，畳んだ `f(1);` の `)`）は，行の後へ置くと後続の別の文の物に為る為，其の行の前へ置く（整形仕様 7.6）
					// 閉じでない無名の字句は後置へ変えた構文の見出（`b if a` の `if`）で原文ではコメントより前に在った為に除く
					// 整形で加えた部分木（補った返戻文等）は原文に無い為に飛ばし，其の後の原文の境界の在る行の前へ置く
					// 後に原文の境界が無ければ，飛ばした部分木の後の独立した行とする（字下げで閉じる本体の末尾の物）
					NodeId Successor = Syntax.FollowingOf(Record.Anchor), Skipped = NoNode;
					for(; Successor != NoNode && Syntax.GetNode(Successor).IsInserted; Successor = Syntax.FollowingOf(Successor)) {
						Skipped = Successor;
					}
					if(
						Successor != NoNode && (Syntax.IsNamed(Successor) || Syntax.Is(Successor, Role::CloserToken)) &&
						Anchors.Nodes[Successor].IsPlaced && (Skipped != NoNode || Anchors.Nodes[Successor].StartFragment == Span.EndFragment)
					) {
						Found.Where = CommentSlotKind::BeforeLine;
						Found.Fragment = Anchors.Nodes[Successor].StartFragment;
					} else if(Skipped != NoNode && Anchors.Nodes[Skipped].IsPlaced) Found.Fragment = Anchors.Nodes[Skipped].EndFragment;
				}
				break;
			case CommentKind::Trailing:
				Found.Where = CommentSlotKind::LineEnd;
				Found.Fragment = Span.EndFragment;
				if(IsBefore) {
					// 原文で錨との間に在った字句を戻した位置の前の行末（其の位置が行頭なら其の前のコードの行，行の途中なら其の行）
					const uint32_t Run =
					Record.Placement.TokensBefore ? TokenRunStart(Code, Syntax, Record.Anchor, Record.Placement.TokensBefore) : Span.Start;
					Found.Fragment = FragmentAt(Code, Run);
					if(!IsAfterCode(Code, Run)) {
						uint32_t Previous = Found.Fragment;
						while(Previous && Fragments[Previous - 1].IsBlank) --Previous;
						if(!Previous) {
							ReportDrop(Found.Text, "no line end with code");
							continue;
						}
						Found.Fragment = Previous - 1;
					}
				}
				break;
			case CommentKind::Inline:
				// 同じ構文の字句の境界（錨の前は原文でコメントと錨の間に在った字句の前）
				Found.Where = CommentSlotKind::Inline;
				if(
					const std::optional<uint32_t> At = IsBefore ?
					std::optional<uint32_t>(
						Record.Placement.TokensBefore ? TokenRunStart(Code, Syntax, Record.Anchor, Record.Placement.TokensBefore) : Span.Start
					) :
					InlineAfter(Code, Syntax, Record, HasInner[Record.Anchor]);
					At && !OpensLineComment(Found.Text) && IsStatementHeadReadable(Code, Syntax, Record.Anchor, *At)
				) {
					Found.Offset = *At;
					Found.Fragment = FragmentAt(Code, *At);
				} else {
					ReportDrop(Found.Text, "no mid-line position");
					continue;
				}
				break;
			}
			// 行末コメントの配置行の補正
			if(Found.Where == CommentSlotKind::LineEnd) {
				// 代入の右辺の配列の値の終わる行への移動
				if(
					const NodeId Parent = Syntax.ParentOf(Record.Anchor), Value = Syntax.FieldChild(Parent, ChildRole::Right);
					Syntax.Is(Parent, Role::AssignmentExpression) && Syntax.Is(Value, Role::ArrayValue)
				) Found.Fragment = Anchors.Nodes[Value].EndFragment;
				// 錨の後に同じ行で開いた要素の終わる行への移動（本文へ取り込まない為）
				if(
					const std::unordered_map<uint32_t, std::vector<NodeId>>::const_iterator Elements = MarkupByLine.find(Span.EndFragment);
					Elements != MarkupByLine.end()
				) for(const NodeId Id : Elements->second) {
					if(const NodeSpan &Markup = Anchors.Nodes[Id]; Markup.Start >= Span.End && Markup.EndFragment > Found.Fragment) {
						Found.Fragment = Markup.EndFragment;
					}
				}
			}
			// コード境界と重なる行末コメントの文中への移動
			if(!CodeBoundaries.empty() && Record.Kind == CommentKind::Trailing) {
				const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator Boundary = std::lower_bound(
					CodeBoundaries.begin(),
					CodeBoundaries.end(),
					std::pair<uint32_t, uint32_t>{ IsBefore ? Span.Start : Span.End, 0 }
				);
				if(Boundary != CodeBoundaries.end() && Boundary->second == Found.Fragment) {
					Found.Where = CommentSlotKind::Inline;
					Found.Offset = Boundary->first;
				}
			}
			// 逐語の中の改行で終わる断片の行末は差込禁止区画の為，逐語の終わる行末へ移す
			if(Found.Where == CommentSlotKind::LineEnd || Found.Where == CommentSlotKind::OwnLine) {
				while(Anchors.VerbatimBreaks[Found.Fragment] & AnchorPositionMap::EndsInVerbatim) ++Found.Fragment;
			}
			// 独立した行の物は後続のコードの行（閉じ括弧・節の見出を含む）を錨として其の直前へ其の深さで置き，後続が無い物は行の後へ最上位の深さで置く（整形仕様 7.6）
			if(Found.Where == CommentSlotKind::OwnLine) {
				if(const uint32_t Next = Code.NextCodeFragment(Found.Fragment); Next < Fragments.size()) {
					Found.Where = CommentSlotKind::BeforeLine;
					Found.Fragment = Next;
				}
			}
			// 逐語の中から始まる行の前は差込禁止区画の為，逐語の始まる行の前へ移す
			if(
				Found.Where == CommentSlotKind::BeforeLine && Found.Fragment &&
				Anchors.VerbatimBreaks[Found.Fragment - 1] & AnchorPositionMap::NextInVerbatim
			) {
				if(!BreaksInText) {
					BreaksInText.emplace(Fragments.size(), 0);
					// 地の文毎に其の中で終わる断片（出力の順に並ぶ断片の最初の物を二分探索で求める）の印
					for(NodeId Id = 0; Id < Anchors.Nodes.size(); ++Id) if(Anchors.Nodes[Id].IsPlaced && Syntax.Is(Id, Role::InlineText)) {
						const NodeSpan &Text = Anchors.Nodes[Id];
						for(
							std::span<const CodeFragment>::iterator Fragment = std::ranges::lower_bound(Fragments, Text.Start, {}, &CodeFragment::End);
							Fragment != Fragments.end() && Fragment->End < Text.End;
							++Fragment
						) (*BreaksInText)[static_cast<size_t>(Fragment - Fragments.begin())] = 1;
					}
				}
				// 遡る間に地の文を跨ぐか
				bool CrossesText = false;
				while(Found.Fragment && Anchors.VerbatimBreaks[Found.Fragment - 1] & AnchorPositionMap::NextInVerbatim) {
					CrossesText = CrossesText || (*BreaksInText)[Found.Fragment - 1];
					--Found.Fragment;
				}
				// 地の文（PHP の `?>`~`<?php`）の続きの行の前の物は，地の文へ差し込めず，遡ると所属する文の外へ出る為に除く（整形仕様 7.7）
				if(CrossesText) {
					ReportDrop(Found.Text, "no position outside inline text");
					continue;
				}
			}
			if(Found.Where == CommentSlotKind::BeforeLine) {
				Found.Tabs = Anchors.FragmentTabs[Found.Fragment];
				// Go の最上位の宣言の行の前の物は其の宣言の文書・cgo の前文へ取り込まれる為，其の宣言の中の錨の前の物（先行コメントと空の本体の物）でなければ空行の前に留め，空行が無ければ除く（整形仕様 7.6, 7.7）
				if(
					const NodeId Top = TopLevel[Record.Anchor],
					Declaration = Top == NoNode || Anchors.Nodes[Top].StartFragment == Found.Fragment ? Top : Syntax.NextNamedSibling(Top);
					Declaration != NoNode && !(Declaration == Top && IsBefore) && Anchors.Nodes[Declaration].IsPlaced &&
					Anchors.Nodes[Declaration].StartFragment == Found.Fragment && Syntax.GetNode(Declaration).Facts.AbsorbsLeadingComment
				) {
					uint32_t Previous = Found.Fragment;
					while(Previous && Fragments[Previous - 1].IsBlank) --Previous;
					if(Previous == Found.Fragment || !Previous) {
						ReportDrop(Found.Text, "only a blank line keeps it out of the next Go declaration's doc");
						continue;
					}
					Found.Where = CommentSlotKind::OwnLine;
					Found.Fragment = Previous - 1;
				}
			}
			Candidates.push_back(std::move(Found));
		}
		// 断片・種類・位置・原文順への整列
		std::stable_sort(
			Candidates.begin(),
			Candidates.end(),
			[](const Candidate &Lhs, const Candidate &Rhs) -> bool {
				// 左右要素の比較の返戻
				return std::tie(Lhs.Fragment, Lhs.Where, Lhs.Offset, Lhs.Record->OriginalOrder) <
				std::tie(Rhs.Fragment, Rhs.Where, Rhs.Offset, Rhs.Record->OriginalOrder);
			}
		);
		// 配置枠を決めたコメントの返戻
		return Candidates;
	}

	/**
	 * コメント配置関数（整形仕様 7.6）
	 * 確定したコードの配置枠へ，分離して置いたコメントを原文での種類の儘割り当てる
	 * コードの字面と空行は読むだけで変えず，配置枠とコメント行の列の組だけを返す
	 * 計算量：断片の数 L とコメントの数 C に対し O(L + C log C)
	 * @param Code 変更不能なコードの出力
	 * @param Comments 錨の節点毎のコメント
	 * @return コメントの配置計画
	 * @throws FormatLimitExceeded 出力が再入力と同じ上限を超える場合と，文書の配置枠が無い場合（文書は脱落させず，文書の保持違反の失敗とする，整形仕様 7.7, 8.3）
	 */
	CommentPlacementPlan CommentPlacer::Place(const LaidOutCode &Code, const CommentRepository &Comments) {
		// コメントの無い入力の空の配置計画の返戻
		if(Comments.GetRecords().empty()) return CommentPlacementPlan(Code);
		const AnchorPositionMap &Anchors = Code.GetAnchors();
		const SyntaxModel &Syntax = *Anchors.Syntax;
		const NotationRules &Rules = Syntax.Rules().Notation;
		const std::span<const CodeFragment> Fragments = Code.GetFragments();
		// 記録毎の出力記法の記録（群と順は入力の儘）と，記法を変えた記録を判定し直した本文構造（整形仕様 3.3, 7.3）
		std::vector<CommentRecord> Outputs;
		Outputs.reserve(Comments.GetRecords().size());
		for(const CommentRecord &Record : Comments.GetRecords()) Outputs.push_back(CommentRepository::OutputFormOf(Record));
		const CommentStructure Structure = CommentNormalizer::Inspect(ReadOnlyCommentView(Outputs));
		const std::vector<Candidate> Candidates = CollectCandidates(Code, Syntax, Comments, Outputs, Structure);
		// 行頭の開始・終端の行条件を持つ埋込文書を描き得るか
		const bool HasHeadFrames = Rules.IsAvailable(CommentNotation::RubyEmbeddedDocument);
		// 行形式へ揃えるとコメントとして読み直せ無く為る範囲（行末迄を本文が飲む節点と読めなかった区画，位置の順に並べて重なりを統合する）
		std::vector<std::pair<uint32_t, uint32_t>> OpaqueRanges;
		for(NodeId Id = 0; Id < Anchors.Nodes.size(); ++Id) if(Anchors.Nodes[Id].IsPlaced && Syntax.Is(Id, Role::OpaqueLineText)) {
			OpaqueRanges.emplace_back(Anchors.Nodes[Id].Start, Anchors.Nodes[Id].End);
		}
		std::sort(OpaqueRanges.begin(), OpaqueRanges.end());
		size_t Merged = 0;
		for(const std::pair<uint32_t, uint32_t> &Range : OpaqueRanges) {
			if(Merged && Range.first <= OpaqueRanges[Merged - 1].second) {
				OpaqueRanges[Merged - 1].second = std::max(OpaqueRanges[Merged - 1].second, Range.second);
			} else OpaqueRanges[Merged++] = Range;
		}
		OpaqueRanges.resize(Merged);
		// 置場が範囲の内か終わりに在り元の記法を保つか（計算量：範囲の数 R に対し O(log R)）
		const auto IsOpaqueAt = [&OpaqueRanges](const uint32_t Dest) -> bool {
			const std::vector<std::pair<uint32_t, uint32_t>>::const_iterator After = std::upper_bound(
				OpaqueRanges.begin(),
				OpaqueRanges.end(),
				Dest,
				[](const uint32_t Position, const std::pair<uint32_t, uint32_t> &Range) -> bool {
					// 範囲の始まりとの比較の返戻
					return Position < Range.first;
				}
			);
			// 置場以前に始まる最後の範囲の内か終わりに在るかの返戻
			return After != OpaqueRanges.begin() && Dest <= std::prev(After)->second;
		};
		std::vector<RenderedComment> Items;
		Items.reserve(Candidates.size());
		uint32_t Sequence = 0;
		// 出力の大きさの見積（コードの断片と其の間の改行に，描画するコメントの行を加える）
		size_t Written = Fragments.empty() ? 0 : Fragments.size() - 1;
		for(const CodeFragment &Fragment : Fragments) Written += Fragment.Text.size();
		// 描画済のコメントの追加（配置枠を与えれない位置のコメントは整形仕様 7.7 に従って除く）
		const auto AddItem =
		[&](const Candidate &Found, const std::optional<CommentSlotId> Slot, std::vector<std::string> &&Lines) -> void {
			if(!Slot) {
				// 文書は脱落させずに整形器の不具合とする（整形仕様 7.7）
				if(Found.IsRequired) throw FormatLimitExceeded("a documentation comment has no slot", FailureReason::DocumentChanged);
				// 脱落の診断（整形仕様 7.7, 10.5）
				ReportDrop(Found.Text, "no slot");
				// 終了
				return;
			}
			Items.push_back({ *Slot, std::move(Lines), &Found.Text, Sequence++, Found.IsRequired });
		};
		// 行末へ置いたコメントが行コメントを開いた断片（後続の本文は其の行コメントへ続く）と，其れが処理系の読むコメントか
		uint32_t LineCommentFragment = std::numeric_limits<uint32_t>::max();
		bool IsLineCommentVerbatim = false;
		for(size_t Index = 0; Index < Candidates.size();) {
			if(Candidates[Index].Where == CommentSlotKind::DocumentEnd) {
				// コードの断片の後の文書の独立した行（所属する根の深さ）
				const Candidate &Found = Candidates[Index++];
				std::string Buffer;
				const std::string_view Body = RenderBody(Rules, Found, CommentSlotKind::OwnLine, false, Buffer);
				AddItem(
					Found,
					Code.GetSlots().Find(CommentSlotKind::DocumentEnd, 0, 0),
					RenderLines(Body, 0, true, Found.IsVerbatim, HasHeadFrames, Written)
				);
				continue;
			}
			const uint32_t Fragment = Candidates[Index].Fragment, LineStart = Fragments[Fragment].Start, LineEnd = Fragments[Fragment].End;
			const size_t Tabs = Anchors.FragmentTabs[Fragment];
			// 断片の中の同じ種類の候補の列挙
			const auto Next = [&](const CommentSlotKind Where) -> const Candidate * {
				// 同じ断片・種類の次の候補の返戻
				return Index < Candidates.size() && Candidates[Index].Fragment == Fragment && Candidates[Index].Where == Where ?
				&Candidates[Index++] :
				nullptr;
			};
			// 此の行の先行コメントを錨と同じ深さで本文より先に置く（空行は整形仕様 1.2 に従いコメントの内容で変えない）
			while(const Candidate *const Found = Next(CommentSlotKind::BeforeLine)) {
				std::string Buffer;
				const std::string_view Body = RenderBody(Rules, *Found, CommentSlotKind::BeforeLine, IsOpaqueAt(LineStart), Buffer);
				AddItem(
					*Found,
					Code.GetSlots().Find(CommentSlotKind::BeforeLine, Fragment, 0),
					RenderLines(Body, Found->Tabs, true, Found->IsVerbatim, HasHeadFrames, Written)
				);
			}
			// 行の中へ差し込む文中のコメント（前後の空白は字句へ密着させる所を除いて１つ置く）
			uint32_t CopyFrom = LineStart + static_cast<uint32_t>(Tabs);
			bool IsInlineLineOpen = false;
			uint32_t InlineLinePosition = 0;
			const uint32_t Content = LineStart + static_cast<uint32_t>(Tabs);
			Adjacent Previous = Tabs ? Adjacent::Tab : Adjacent::Newline;
			while(const Candidate *const Found = Next(CommentSlotKind::Inline)) {
				const uint32_t Dest = std::max(Found->Offset, CopyFrom);
				if(
					!Rules.LineCommentTerminator.empty() && IsInlineLineOpen && InlineLinePosition == Dest &&
					(Found->Text.find(Rules.LineCommentTerminator) != std::string::npos || Found->Text.find('\n') != std::string::npos)
				) {
					ReportDrop(Found->Text, "a preceding line comment would expose a PHP close tag");
					continue;
				}
				if(Dest > CopyFrom) Previous = AdjacentBefore(Code, Dest, Content);
				std::string Buffer;
				const std::string_view Body = RenderBody(Rules, *Found, CommentSlotKind::Inline, IsOpaqueAt(Dest), Buffer);
				if(OpensLineComment(Body)) {
					IsInlineLineOpen = true;
					InlineLinePosition = Dest;
				}
				std::vector<std::string> Lines = RenderLines(Body, Tabs, false, Found->IsVerbatim, HasHeadFrames, Written);
				// 開き括弧の直後を除く区切空白
				if(Previous == Adjacent::Code) Lines.front().insert(0, 1, ' ');
				// 後続のコードがコメントへ密着しない様に区切りの空白の配置（閉じ括弧の前は元から空白が在った時だけ置く）
				if(
					Dest < LineEnd && PieceCovering(Code, Dest) && !HasShape(Code.PieceAt(Dest), CodePiece::StartsSeparator) &&
					(!HasShape(Code.PieceAt(Dest), CodePiece::StartsClosingBracket) || AdjacentBefore(Code, Dest, Content) == Adjacent::Space) &&
					!Found->Record->Placement.IsGluedAfter
				) Lines.back() += ' ';
				Previous = AdjacentOf(Lines.back().back());
				CopyFrom = Dest;
				AddItem(*Found, Code.GetSlots().Find(CommentSlotKind::Inline, Fragment, Dest), std::move(Lines));
			}
			// 行末の字（文中のコメントで終わる行は其のコメントの末尾）
			Adjacent Tail =
			CopyFrom == LineEnd ? Previous : LineEnd > LineStart ? AdjacentBefore(Code, LineEnd, Content) : Adjacent::Newline;
			// 此の行末に対応する行末と後の独立した行のコメント
			while(Index < Candidates.size() && Candidates[Index].Fragment == Fragment) {
				const Candidate &Found = Candidates[Index++];
				std::string Buffer;
				const std::string_view Body = RenderBody(Rules, Found, Found.Where, IsOpaqueAt(LineEnd), Buffer);
				// 錨の行の後の独立した行
				if(Found.Where == CommentSlotKind::OwnLine) {
					AddItem(
						Found,
						Code.GetSlots().Find(CommentSlotKind::OwnLine, Fragment, 0),
						RenderLines(Body, Found.Tabs, true, Found.IsVerbatim, HasHeadFrames, Written)
					);
					continue;
				}
				if(
					!Rules.LineCommentTerminator.empty() && LineCommentFragment == Fragment &&
					Found.Text.find(Rules.LineCommentTerminator) != std::string::npos
				) {
					ReportDrop(Found.Text, "a preceding line comment would expose a PHP close tag");
					continue;
				}
				if(IsUnplaceableLineEnd(Found.Text, Body, Found.IsVerbatim, LineCommentFragment == Fragment, IsLineCommentVerbatim)) continue;
				std::vector<std::string> Lines = RenderLines(Body, Tabs, false, Found.IsVerbatim, HasHeadFrames, Written);
				// 前置区切り（記録したタブの数が正なら其の数のタブ，０なら半角スペース１つ，整形仕様 7.6）
				if(Tail != Adjacent::Space && Tail != Adjacent::Newline) {
					const size_t Leading = Found.Record->Format.LeadingTabCount;
					Lines.front().insert(0, Leading ? Leading : 1, Leading ? '\t' : ' ');
				}
				Tail = AdjacentOf(Lines.back().back());
				// 最後の行が行コメントを開いた断片の記録（積んだ後の物は先に開いた行コメントの本文へ続く）
				if(LineCommentFragment != Fragment && OpensLineComment(Body.substr(Body.rfind('\n') + 1))) {
					LineCommentFragment = Fragment;
					IsLineCommentVerbatim = Found.IsVerbatim;
				}
				AddItem(Found, Code.GetSlots().Find(CommentSlotKind::LineEnd, Fragment, 0), std::move(Lines));
			}
		}
		// 配置計画の返戻
		return BuildPlan(Code, std::move(Items));
	}
} // namespace Core
