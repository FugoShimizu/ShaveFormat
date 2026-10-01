#include "CommentRepository.hpp"
#include "../Util/FormatLimitExceeded.hpp"
#include "../Util/ParseAdapter.hpp"
#include "../Util/CharClass.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 名前迄を記号に含む指示記法かの判定関数（整形仕様 3.2, 4.5）
	 * @param Notation 記法
	 * @return 処理系が名前を定める指示記法の値なら true
	 */
	bool CommentRepository::IsNamedDirective(const CommentNotation Notation) {
		// 名前付の指示記法の値かの返戻
		return Notation >= CommentNotation::GoBuildLine && Notation <= CommentNotation::GccgoExternLine;
	}

	/**
	 * ファイルの頭部へ置く登録定義かの判定関数（整形仕様 4.5）
	 * `package` 等の宣言より前の頭部に置き，後続の宣言へ作用しない登録定義（ビルド条件・実行時設定）を区分する
	 * @param Definition 登録定義
	 * @return 頭部へ置く登録定義なら true
	 */
	bool CommentRepository::IsFileHeadDirective(const DirectiveDefinitionId Definition) {
		// 頭部の登録定義かの返戻
		return Definition == DirectiveDefinitionId::GoBuild || Definition == DirectiveDefinitionId::GoDebug;
	}

	/**
	 * 記号と本文の間の空白を揃える指示記法かの判定関数（整形仕様 3.2）
	 * @param Notation 記法
	 * @return `//` と同じく記号の後をスペース１つに揃える記法なら true（位置の指示の後の空白はファイル名の一部）
	 */
	bool CommentRepository::IsSpacedDirective(const CommentNotation Notation) {
		// 揃える記法かの返戻
		return IsNamedDirective(Notation) && Notation != CommentNotation::GoLineLine && Notation != CommentNotation::GoLineBlock;
	}

	/**
	 * 記法の開始記号の取得関数（整形仕様 3.2）
	 * @param Notation 記法
	 * @return 開始記号（Go の指示記法は名前迄を含む）
	 */
	std::string_view CommentRepository::OpenerOf(const CommentNotation Notation) {
		switch(Notation) {
		case CommentNotation::SlashLine:
			// 一般の行コメントの記号の返戻
			return "//";
		case CommentNotation::TripleSlashLine:
			// ３個のスラッシュの記号の返戻
			return "///";
		case CommentNotation::SlashBangLine:
			// `//!` の記号の返戻
			return "//!";
		case CommentNotation::HashLine:
			// `#` の記号の返戻
			return "#";
		case CommentNotation::HashBangLine:
			// シバンの記号の返戻
			return "#!";
		case CommentNotation::HashColonLine:
			// `#:` の記号の返戻
			return "#:";
		case CommentNotation::Block:
			// 一般の囲みの記号の返戻
			return "/*";
		case CommentNotation::DoubleStarBlock:
			// 文書の囲みの記号の返戻
			return "/**";
		case CommentNotation::StarBangBlock:
			// `/*!` の記号の返戻
			return "/*!";
		case CommentNotation::RubyEmbeddedDocument:
			// 埋込文書の記号の返戻
			return "=begin";
		case CommentNotation::GoBuildLine:
			// ビルド制約の記号の返戻
			return "//go:build";
		case CommentNotation::GoDebugLine:
			// 実行時の設定の記号の返戻
			return "//go:debug";
		case CommentNotation::GoEmbedLine:
			// 埋込資源の記号の返戻
			return "//go:embed";
		case CommentNotation::GoGenerateLine:
			// 生成命令の記号の返戻
			return "//go:generate";
		case CommentNotation::GoLinknameLine:
			// 結合名の記号の返戻
			return "//go:linkname";
		case CommentNotation::GoNoescapeLine:
			// 逃避解析の指示の記号の返戻
			return "//go:noescape";
		case CommentNotation::GoNoinlineLine:
			// 展開禁止の記号の返戻
			return "//go:noinline";
		case CommentNotation::GoNoraceLine:
			// 競合検出の除外の記号の返戻
			return "//go:norace";
		case CommentNotation::GoNosplitLine:
			// 積の検査の除外の記号の返戻
			return "//go:nosplit";
		case CommentNotation::GoUintptrescapesLine:
			// 整数の指す先の逃避の記号の返戻
			return "//go:uintptrescapes";
		case CommentNotation::GoWasmexportLine:
			// WebAssembly の輸出の記号の返戻
			return "//go:wasmexport";
		case CommentNotation::GoWasmimportLine:
			// WebAssembly の輸入の記号の返戻
			return "//go:wasmimport";
		case CommentNotation::GoLineLine:
			// 行の位置の指示の記号の返戻
			return "//line";
		case CommentNotation::GoLineBlock:
			// 囲みの位置の指示の記号の返戻
			return "/*line";
		case CommentNotation::CgoExportLine:
			// 輸出の指示の記号の返戻
			return "//export";
		case CommentNotation::GccgoExternLine:
			break;
		}
		// 外部名の指示の記号の返戻
		return "//extern";
	}

	/**
	 * 囲みの記法かの判定関数
	 * @param Notation 記法
	 * @return 終端記号で閉じる囲みの記法なら true
	 */
	bool CommentRepository::IsBlockNotation(const CommentNotation Notation) {
		// 囲みの記法かの返戻
		return Notation == CommentNotation::Block || Notation == CommentNotation::DoubleStarBlock ||
		Notation == CommentNotation::StarBangBlock || Notation == CommentNotation::GoLineBlock;
	}

	/**
	 * 記法の終端記号の取得関数（整形仕様 3.2）
	 * @param Notation 記法
	 * @return 終端記号（行末迄を本文とする記法は空）
	 */
	std::string_view CommentRepository::CloserOf(const CommentNotation Notation) {
		// 埋込文書の行頭の終端記号の返戻
		if(Notation == CommentNotation::RubyEmbeddedDocument) return "=end";
		// 囲みの終端記号と，行の記法の空の返戻
		return IsBlockNotation(Notation) ? "*/" : "";
	}

	/**
	 * 行頭の語かの判定関数（整形仕様 4.3，行頭の開始・終端の行条件を持つ埋込文書の記号）
	 * 処理系と同じく，語の後に空白（改行・書式送り等を含む）か行末が続く物だけを語とする
	 * @param Line 行の字面
	 * @param Word 語
	 * @return 行が語で始まり，後に空白か行末が続けば true
	 */
	bool CommentRepository::IsLineHeadWord(const std::string_view Line, const std::string_view Word) {
		// 語の後が空白か行末かの返戻
		return Line.starts_with(Word) && (Line.size() == Word.size() || std::isspace(static_cast<unsigned char>(Line[Word.size()])));
	}

	/**
	 * 前の区切空白を除いた字面の取得関数（整形仕様 7.6）
	 * 半角スペース・全角スペース・タブの有無・個数・混在に依らず，行頭の空白の並びを区切空白とする
	 * @param Line 記号の後の字面
	 * @return 行頭の半角スペース・全角スペース・タブを除いた字面
	 */
	std::string_view CommentRepository::TrimLeadingSeparator(std::string_view Line) {
		// 全角スペースの字面
		constexpr std::string_view WideSpace = "\u3000";
		while(true) {
			if(Line.starts_with(' ') || Line.starts_with('\t')) Line.remove_prefix(1);
			else if(Line.starts_with(WideSpace)) Line.remove_prefix(WideSpace.size());
			// 区切空白を除いた字面の返戻
			else return Line;
		}
	}

	/**
	 * 指示子の前の区切空白を除いた字面の取得関数（整形仕様 4.5, 7.6）
	 * 処理系が区切りと読む字だけを除く（TypeScript の三重スラッシュ指示子は全角スペースも区切りと読み，其の他は半角スペースとタブ）
	 * @param Definition 指示子の登録定義（無ければ半角スペースとタブ）
	 * @param Line 記号の後の字面
	 * @return 行頭の区切空白を除いた字面
	 */
	std::string_view CommentRepository::TrimDirectiveSeparator(
		const std::optional<DirectiveDefinitionId> Definition,
		const std::string_view Line
	) {
		// 全角スペースも区切りと読む指示子の返戻
		if(Definition == DirectiveDefinitionId::TypeScriptReference) return TrimLeadingSeparator(Line);
		// 半角スペースとタブを除いた字面の返戻
		return Line.substr(std::min(Line.find_first_not_of(" \t"), Line.size()));
	}

	/**
	 * 後の区切空白を除いた字面の取得関数（整形仕様 7.6）
	 * 半角スペース・全角スペース・タブの有無・個数・混在に依らず，行末の空白の並びを区切空白とする
	 * @param Line 終端記号の前の字面
	 * @return 行末の半角スペース・全角スペース・タブを除いた字面
	 */
	std::string_view CommentRepository::TrimTrailingSeparator(std::string_view Line) {
		// 全角スペースの字面
		constexpr std::string_view WideSpace = "\u3000";
		while(true) {
			if(Line.ends_with(' ') || Line.ends_with('\t')) Line.remove_suffix(1);
			else if(Line.ends_with(WideSpace)) Line.remove_suffix(WideSpace.size());
			// 区切空白を除いた字面の返戻
			else return Line;
		}
	}

	/**
	 * 囲みの内側の論理行の取得関数
	 * 開始記号の後と終端記号の前の区切空白，継続行の飾りの `*` と其の後の空白，飾りの無い継続行の字下げを除く（字下げは記号の前に置く物で，本文に持たない）
	 * @param Interior 開始記号と終端記号の間
	 * @return 論理行（開始記号・終端記号だけの行は空の行）
	 */
	std::vector<std::string> CommentRepository::BlockLines(const std::string_view Interior) {
		// 囲み内の物理行の分割
		std::vector<std::string_view> Physical;
		for(size_t Cursor = 0;;) {
			const size_t End = std::min(Interior.find('\n', Cursor), Interior.size());
			Physical.push_back(Interior.substr(Cursor, End - Cursor));
			if(End == Interior.size()) break;
			Cursor = End + 1;
		}
		std::vector<std::string> Lines;
		Lines.reserve(Physical.size());
		for(size_t Idx = 0; Idx < Physical.size(); ++Idx) {
			std::string_view Line = Physical[Idx];
			// 行頭の空白（開始記号の後の区切空白，継続行の字下げ）
			Line = TrimLeadingSeparator(Line);
			// 継続行の飾りと其の後の区切空白
			if(Idx && Line.starts_with('*')) Line = TrimLeadingSeparator(Line.substr(1));
			// 終端記号の前の区切空白
			if(Idx + 1 == Physical.size()) Line = TrimTrailingSeparator(Line);
			Lines.emplace_back(Line);
		}
		// 論理行の返戻
		return Lines;
	}

	/**
	 * 記号の後の空白を処理系が値として読む指示子の定義かの判定関数（整形仕様 4.5, 7.5, 7.6）
	 * 文書属性・Example の出力注記・cgo の前文・位置指示は記号の後の空白も値に含む為，区切空白を揃えない
	 * @param Definition 指示子の登録定義
	 * @return 記号の後の空白が値に属する定義なら true
	 */
	bool CommentRepository::IsValueSpacing(const DirectiveDefinitionId Definition) {
		// 値に属する空白を持つ定義かの返戻
		return Definition == DirectiveDefinitionId::DocumentAttribute || Definition == DirectiveDefinitionId::ExampleOutput ||
		Definition == DirectiveDefinitionId::CgoPreamble || Definition == DirectiveDefinitionId::LinePosition ||
		Definition == DirectiveDefinitionId::BlockLinePosition;
	}

	/**
	 * 本文を１バイトも変えない記録かの判定関数（整形仕様 4.5, 7.5）
	 * @param Record コメントの記録
	 * @return 処理系が読む指示コメント（空白を揃える Go の指示記法を除く）とシバンなら true
	 */
	bool CommentRepository::IsVerbatimRecord(const CommentRecord &Record) {
		// 本文を其の儘保つ記録かの返戻
		return Record.Notation == CommentNotation::HashBangLine ||
		Record.Role == CommentRole::Directive && !IsSpacedDirective(Record.Notation);
	}

	/**
	 * 記録の記法の定義の取得関数
	 * @param Record 記法の定義を記録したコメントの記録
	 * @return 共有する言語の記法の定義
	 * @throws FormatLimitExceeded 記法の定義を記録していない場合（整形器の不具合）
	 */
	const NotationRules &CommentRepository::RulesOf(const CommentRecord &Record) {
		// 記法の定義を持たない記録は整形器の不具合とする
		if(!Record.Format.Rules) throw FormatLimitExceeded("a comment record has no notation rules", FailureReason::FormatterDefect);
		// 記法の定義の返戻
		return *Record.Format.Rules;
	}

	/**
	 * 開始記号の一部として密着させる本文の頭の取得関数（整形仕様 7.6）
	 * 前の成員の文書の頭（Doxygen の `///<` 等）と区切の記法が開始記号へ密着させる頭だけを記号の一部とし，其の他の記号で始まる本文は区切空白の後の本文とする
	 * @param Record 記法の定義を記録したコメントの記録（此の記法で頭を判定する）
	 * @param Body 開始記号の直後の字面
	 * @return 本文が始まる記号の一部の頭（無ければ空）
	 */
	std::string_view CommentRepository::SymbolHeadOf(const CommentRecord &Record, const std::string_view Body) {
		const NotationRules &Rules = RulesOf(Record);
		if(!Rules.MemberDocumentHead.empty() && Rules.DocumentFormOf(Record.Notation) && Body.starts_with(Rules.MemberDocumentHead)) {
			// 文書の記法の前の成員の文書の頭の返戻
			return Rules.MemberDocumentHead;
		}
		const CommentOutputForm *const Separator = Rules.AdoptedFormOf(CommentUse::Separator);
		// 区切の記法の頭の返戻
		if(
			Separator && !Separator->GluedHead.empty() && Record.Notation == Separator->Notation && Body.starts_with(Separator->GluedHead)
		) return Separator->GluedHead;
		// 頭の無い本文の返戻
		return {};
	}

	/**
	 * 本文の無い記号だけの行かの判定関数
	 * @param Line 論理行
	 * @return ASCII の記号だけから成れば true（空の行を含む）
	 */
	bool CommentRepository::IsSymbolsOnly(const std::string_view Line) {
		// 記号以外の字の有無の返戻
		return std::all_of(
			Line.begin(),
			Line.end(),
			[](const char Char) -> bool {
				// ASCII の記号かの返戻
				return std::ispunct(static_cast<unsigned char>(Char));
			}
		);
	}

	/**
	 * 記号の一部の頭と本文の間の区切空白を揃えた論理行の取得関数（整形仕様 7.6）
	 * 頭は記号の一部の為，頭と本文の間も区切の有無・個数・混在に依らず半角スペース１つとする
	 * @param Line 頭で始まる論理行
	 * @param Head 記号の一部の頭
	 * @return 頭と区切空白と本文を繋いだ論理行
	 */
	std::string CommentRepository::HeadedLine(const std::string_view Line, const std::string_view Head) {
		const std::string_view Rest = TrimLeadingSeparator(Line.substr(Head.size()));
		// 頭の後に本文が在れば半角スペース１つを挟んだ論理行の返戻
		return std::string(Head).append(Rest.empty() ? "" : " ").append(Rest);
	}

	/**
	 * 原文の形の字面から記号と区切空白を除いた本文への分解関数（整形仕様 3.2, 4.3）
	 * 区切空白は記号と本文の間の半角スペース・全角スペース・タブの並び（記号の後の空白を値として読む指示子は半角スペース１つ）とし，区切空白の無い本文の内，記号の一部の頭（前の成員の文書・区切の記法の頭）に密着する本文と本文を其の儘保つ記録の本文は密着として記録する
	 * 字下げは記号の前に置く物で本文に持たない為，本文を其の儘保つ記録を除く各論理行の行頭の空白を除く（本文は記法に依らない）
	 * 行毎の記号を持たない埋込文書は物理行毎の論理行とする（開始記号・終端記号だけの行は空の行）
	 * @param Record 字面を本文に持つコメントの記録（本文・密着・改行の有無を書き換える）
	 */
	void CommentRepository::Decompose(CommentRecord &Record) {
		const std::string Raw = std::move(Record.Text);
		const std::string_view View = Raw, Opener = OpenerOf(Record.Notation);
		const bool IsSpaced = IsSpacedDirective(Record.Notation), IsVerbatim = IsVerbatimRecord(Record);
		// 指示子の登録定義と，記号の後の空白を値として読む指示子か（区切空白は１つだけ）
		const std::optional<DirectiveDefinitionId> Definition =
		Record.Directive ? std::optional<DirectiveDefinitionId>(Record.Directive->Definition) : std::nullopt;
		const bool HasValueSpacing = IsVerbatim && Definition && IsValueSpacing(*Definition);
		Record.Text.clear();
		Record.Format.AttachFirstLine = false;
		Record.Format.SeparateLogicalLines = View.find('\n') != std::string_view::npos;
		const std::string_view Closer = CloserOf(Record.Notation);
		if(Record.Notation == CommentNotation::RubyEmbeddedDocument) {
			// 物理行毎の論理行（開始記号の行の残り，本文の行，行頭の終端記号の行の残り）
			std::vector<std::string_view> Lines;
			for(size_t Cursor = std::min(Opener.size(), View.size());;) {
				const size_t End = std::min(View.find('\n', Cursor), View.size());
				Lines.push_back(View.substr(Cursor, End - Cursor));
				if(End == View.size()) break;
				Cursor = End + 1;
			}
			// ２行目以降の最後の行が空白か行末の続く行頭の終端記号で始まるか
			if(Lines.size() > 1 && IsLineHeadWord(Lines.back(), Closer)) Lines.back().remove_prefix(Closer.size());
			for(size_t Idx = 0; Idx < Lines.size(); ++Idx) {
				std::string_view Line = Lines[Idx];
				// 開始記号・終端記号の後の区切空白と，本文の行の字下げの除去
				Line = TrimLeadingSeparator(Line);
				Record.Text.append(Idx ? "\n" : "").append(Line);
			}
			// 埋込文書の本文の分解の終了
			return;
		}
		if(IsBlockNotation(Record.Notation) && View.size() >= Opener.size() + Closer.size() && View.ends_with(Closer)) {
			const std::string_view Interior = View.substr(Opener.size(), View.size() - Opener.size() - Closer.size());
			if(IsVerbatim) {
				// 指示コメントの内側の返戻
				Record.Text = Interior;
				// 内側の本文の分解の終了
				return;
			}
			const std::string_view Head = SymbolHeadOf(Record, Interior);
			Record.Format.AttachFirstLine = !Head.empty();
			std::vector<std::string> Lines = BlockLines(Interior);
			// 記号の一部の頭と本文の間の区切空白
			if(!Head.empty()) Lines.front() = HeadedLine(Lines.front(), Head);
			for(const std::string &Line : Lines) Record.Text.append(Line).push_back('\n');
			Record.Text.pop_back();
			// 囲みの本文の分解の終了
			return;
		}
		// 行記法の行毎の記号と区切空白の除去
		for(size_t Cursor = 0;;) {
			const size_t End = std::min(View.find('\n', Cursor), View.size());
			std::string_view Line = View.substr(Cursor, End - Cursor);
			if(Line.starts_with(Opener)) Line.remove_prefix(Opener.size());
			if(IsSpaced) {
				// 記号と本文の間と本文の後の全ての空白
				Line.remove_prefix(std::min(Line.find_first_not_of(" \t"), Line.size()));
				Line.remove_suffix(Line.size() - std::min(Line.find_last_not_of(" \t") + 1, Line.size()));
			} else if(!IsVerbatim || !Cursor) {
				// 記号と本文の間の空白の並び（区切空白，指示子は処理系が区切りと読む字）の除去と，区切空白の無い本文の密着の記録（本文を其の儘保つ記録の続きの行は記号の後を其の儘保つ）
				const std::string_view Body = HasValueSpacing ?
				Line.substr(Line.starts_with(' ') ? 1 : 0) :
				IsVerbatim ? TrimDirectiveSeparator(Definition, Line) : TrimLeadingSeparator(Line);
				// 値として読む指示子は其の儘密着させ，其の他の指示子は区切空白を補い，説明は記号の一部の頭だけを密着させる
				if(Body.size() == Line.size() && !Cursor && !Line.empty()) {
					Record.Format.AttachFirstLine = IsVerbatim ? HasValueSpacing : !SymbolHeadOf(Record, Line).empty();
				}
				Line = Body;
			}
			// 記号の一部の頭で始まる最初の論理行は頭と本文の間の区切空白を揃える
			if(!Cursor && Record.Format.AttachFirstLine && !IsVerbatim) Record.Text.append(HeadedLine(Line, SymbolHeadOf(Record, Line)));
			else Record.Text.append(Line);
			if(End == View.size()) break;
			Record.Text.push_back('\n');
			Cursor = End + 1;
		}
		// 終了
		return;
	}

	/**
	 * 記号と本文の合成関数（整形仕様 7.6）
	 * 非空の本文との区切空白をスペース１つとし，本文の無い形と記号の一部の頭に密着する最初の論理行には区切空白を出さない（記号・タブで始まる本文は本文の一部）
	 * 囲みの複数の論理行は，改行を持たない文中の物と，一般の囲みの行末の物・改行を持たない物を閉じた囲みの並びにし，其の他を継続行の `*` を持つ１つの囲みにする
	 * 埋込文書は行頭の開始記号と終端記号の行の間へ本文の行を置く（行頭の終端記号に読める本文の行は字下げする）
	 * @param Record コメントの記録
	 * @return 原文の形の字面
	 */
	std::string CommentRepository::Compose(const CommentRecord &Record) {
		const std::string_view Opener = OpenerOf(Record.Notation), Closer = CloserOf(Record.Notation), Text = Record.Text;
		// 埋込文書の開始行と本文と終端行の合成
		if(Record.Notation == CommentNotation::RubyEmbeddedDocument) {
			std::string Out(Opener);
			for(size_t Cursor = 0; Cursor <= Text.size();) {
				const size_t End = std::min(Text.find('\n', Cursor), Text.size());
				const std::string_view Line = Text.substr(Cursor, End - Cursor);
				// 行頭の終端記号に読める本文の行の字下げ
				Out.append(IsLineHeadWord(Line, Closer) ? "\n\t" : "\n").append(Line);
				Cursor = End + 1;
			}
			// 行頭の開始記号と終端記号の行で本文の行を囲む埋込文書の返戻
			return Out.append("\n").append(Closer);
		}
		const bool IsVerbatim = IsVerbatimRecord(Record);
		// 指示コメントの内側の儘の囲みの返戻
		if(IsVerbatim && IsBlockNotation(Record.Notation)) return std::string(Opener).append(Text).append(Closer);
		std::string Out;
		std::vector<std::string_view> Lines;
		for(size_t Cursor = 0;;) {
			const size_t End = std::min(Text.find('\n', Cursor), Text.size());
			Lines.push_back(Text.substr(Cursor, End - Cursor));
			if(End == Text.size()) break;
			Cursor = End + 1;
		}
		// 記号へ密着する論理行か（文毎に分けた続きの行は最初の論理行と同じ記号の一部の頭で始まる）
		const std::string_view Run = Record.Format.AttachFirstLine ? SymbolHeadOf(Record, Lines.front()) : std::string_view();
		const auto IsGluedLine = [&Lines, &Record, Run](const size_t Idx) -> bool {
			// 密着する論理行かの返戻
			return Record.Format.AttachFirstLine && (!Idx || !Run.empty() && Lines[Idx].starts_with(Run));
		};
		if(IsBlockNotation(Record.Notation)) {
			// １行の囲み（記号へ密着する記号だけの本文は本文を持たない形として終端記号とも密着させる）
			const auto Single = [Opener, Closer](const std::string_view Line, const bool IsGlued) -> std::string {
				// 区切空白を挟んだ囲みの返戻
				return std::string(Opener).append(Line.empty() || IsGlued ? "" : " ").append(Line).append(
					IsSymbolsOnly(Line) && (Line.empty() || IsGlued) ? "" : " "
				).append(Closer);
			};
			// １行の囲みの返戻
			if(Lines.size() == 1) return Single(Lines.front(), IsGluedLine(0));
			// 改行を持たない文中の囲み，行末と改行を持たない一般の囲みの閉じた囲みの並び（行末は文毎に積む）
			if(
				Record.Kind == CommentKind::Inline ?
				!Record.Format.SeparateLogicalLines :
				Record.Notation == CommentNotation::Block && (Record.Kind == CommentKind::Trailing || !Record.Format.SeparateLogicalLines)
			) {
				for(size_t Idx = 0; Idx < Lines.size(); ++Idx) {
					// 開始記号・終端記号だけの行に当たる空の論理行は閉じた囲みにしない
					if(Lines[Idx].empty()) continue;
					Out.append(Out.empty() ? "" : Record.Kind == CommentKind::Inline ? " " : "\n").append(Single(Lines[Idx], IsGluedLine(Idx)));
				}
				// 閉じた囲みの並びの返戻
				return Out;
			}
			Out.append(Opener);
			if(!Lines.front().empty()) Out.append(IsGluedLine(0) ? "" : " ").append(Lines.front());
			for(size_t Idx = 1; Idx < Lines.size(); ++Idx) {
				const bool IsLast = Idx + 1 == Lines.size();
				Out += '\n';
				if(IsLast && Lines[Idx].empty()) Out.append(" ").append(Closer);
				else {
					Out += " *";
					if(!Lines[Idx].empty()) Out.append(" ").append(Lines[Idx]);
					if(IsLast) Out.append(" ").append(Closer);
				}
			}
			// 継続行の `*` を持つ囲みの返戻
			return Out;
		}
		for(size_t Idx = 0; Idx < Lines.size(); ++Idx) {
			if(Idx) Out += '\n';
			Out.append(Opener);
			// 位置指示の必須スペースを置き，其の他は記号の一部の頭に密着する行と本文を其の儘保つ続きの行の区切空白を省く（記号・タブで始まる本文も区切空白の後に置く）
			if(!Lines[Idx].empty()) {
				const bool HasSeparator = Record.Notation == CommentNotation::GoLineLine || !(IsGluedLine(Idx) || Idx && IsVerbatim);
				Out.append(HasSeparator ? " " : "").append(Lines[Idx]);
			}
		}
		// 行記法の返戻
		return Out;
	}

	/**
	 * 位置指示の本文の成立判定関数
	 * 右端から行又は行と列を読み，ファイル名に含まれるコロンを残す
	 * @param Notation 行又は囲みの位置指示の記法
	 * @param Text 開始記号から終端記号迄の字面
	 * @return 処理系が受け付ける位置情報なら true
	 */
	bool CommentRepository::HasValidLinePosition(const CommentNotation Notation, const std::string_view Text) {
		// 位置指示以外の記法の返戻
		if(Notation != CommentNotation::GoLineLine && Notation != CommentNotation::GoLineBlock) return false;
		const std::string_view Opener = OpenerOf(Notation);
		// 必須の区切空白を持たない字面の返戻
		if(!Text.starts_with(Opener) || Text.size() <= Opener.size() || Text[Opener.size()] != ' ') return false;
		std::string_view Body = Text.substr(Opener.size() + 1);
		if(Notation == CommentNotation::GoLineBlock) {
			const std::string_view Closer = CloserOf(Notation);
			// 終端記号の無い囲みの返戻
			if(!Body.ends_with(Closer)) return false;
			Body.remove_suffix(Closer.size());
		} else if(Body.ends_with('\r')) Body.remove_suffix(1);
		// 複数行の囲みと行内の制御文字の返戻
		if(Body.find('\n') != std::string_view::npos || Body.find('\r') != std::string_view::npos) return false;
		// 右端からの行番号と列番号の読取
		const size_t LastColon = Body.rfind(':');
		// 位置区切の無い字面の返戻
		if(LastColon == std::string_view::npos) return false;
		const auto NumberOf = [](const std::string_view Digits) -> std::optional<uint64_t> {
			uint64_t Number = 0;
			const std::from_chars_result Read = std::from_chars(Digits.data(), Digits.data() + Digits.size(), Number);
			// 十進数の全桁を読めた時だけの数値の返戻
			return Read.ec == std::errc() && Read.ptr == Digits.data() + Digits.size() && !Digits.empty() ?
			std::optional<uint64_t>(Number) :
			std::nullopt;
		};
		const std::optional<uint64_t> Last = NumberOf(Body.substr(LastColon + 1));
		// 末尾の数値が成立しない字面の返戻
		if(!Last) return false;
		const size_t PreviousColon = LastColon ? Body.rfind(':', LastColon - 1) : std::string_view::npos;
		const std::optional<uint64_t> Previous = PreviousColon == std::string_view::npos ?
		std::nullopt :
		NumberOf(Body.substr(PreviousColon + 1, LastColon - PreviousColon - 1));
		constexpr uint64_t Maximum = uint64_t(1) << 30;
		const uint64_t LastValue = *Last;
		// 右端の数値と，有れば手前の数値（右端が列の時の行）が範囲内かの返戻
		return LastValue > 0 && LastValue <= Maximum && (!Previous || *Previous > 0 && *Previous <= Maximum);
	}

	/**
	 * 行頭（桁０）へ置くと成立する記法かの判定関数
	 * @param Notation 記法
	 * @param Text 記号から後の字面
	 * @return 行頭の `//go:generate` と，位置情報の成立する行頭の `//line` なら true
	 */
	bool CommentRepository::ActivatesAtLineHead(const CommentNotation Notation, const std::string_view Text) {
		// 行頭で成立する記法と本文かの返戻
		return Notation == CommentNotation::GoGenerateLine ||
		Notation == CommentNotation::GoLineLine && HasValidLinePosition(Notation, Text);
	}

	/**
	 * 複数の論理行を持つ文書の囲みの開始記号と終端記号を独立した行に置く関数（整形仕様 7.6）
	 * @param Record 文書の囲みの記録（本文と改行の有無を書き換える）
	 */
	void CommentRepository::FrameDocument(CommentRecord &Record) {
		// 一行の文書の終了
		if(Record.Text.find('\n') == std::string::npos) return;
		if(!Record.Text.starts_with('\n')) Record.Text.insert(0, 1, '\n');
		if(!Record.Text.ends_with('\n')) Record.Text += '\n';
		Record.Format.SeparateLogicalLines = true;
		// 終了
		return;
	}

	/**
	 * 記録への記法の定義と入力の文書形式の記録関数（整形仕様 3.2）
	 * 説明のコメントが文書に使う記法には其の文書形式を記録し，指示コメントと文書に使わない記法には記録しない
	 * @param Rules 共有する言語の記法の定義
	 * @param Record 記法と役割を定めた記録
	 */
	void CommentRepository::AttachRules(const NotationRules &Rules, CommentRecord &Record) {
		Record.Format.Rules = &Rules;
		Record.Format.InputDocument = Record.Role == CommentRole::Presentation ? Rules.DocumentFormOf(Record.Notation) : std::nullopt;
		// 終了
		return;
	}

	/**
	 * 区切コメントの本文の取得関数（整形仕様 7.4, 7.6）
	 * 等号の列で名前を囲む１行の独立した説明のコメントを区切とし，採用する区切の記法が開始記号へ密着させる本文の頭は除いて読む
	 * @param Record 記法の定義を記録したコメントの記録
	 * @return 前後の空白を除いた等号の列と名前（区切コメントでなければ無し）
	 */
	std::optional<std::string_view> CommentRepository::SeparatorBodyOf(const CommentRecord &Record) {
		// 等号の列の最短の長さ
		constexpr size_t MinRun = 4;
		std::string_view Body = Record.Text;
		// 独立した説明の１行のコメントでない記録の返戻
		if(
			Record.Role != CommentRole::Presentation || Record.Kind != CommentKind::OwnLine || Body.find('\n') != std::string_view::npos
		) return std::nullopt;
		// 採用する区切の記法が開始記号へ密着させる本文の頭と，前後の空白の除去
		if(
			const CommentOutputForm *const Form =
			Record.Format.AttachFirstLine ? RulesOf(Record).AdoptedFormOf(CommentUse::Separator) : nullptr;
			Form && !Form->GluedHead.empty() && Record.Notation == Form->Notation && Body.starts_with(Form->GluedHead)
		) Body.remove_prefix(Form->GluedHead.size());
		Body.remove_prefix(std::min(Body.find_first_not_of(" \t"), Body.size()));
		Body.remove_suffix(Body.size() - std::min(Body.find_last_not_of(" \t") + 1, Body.size()));
		const size_t NameStart = Body.find_first_not_of('='), NameEnd = Body.find_last_not_of('=');
		// 前後の等号の列と，其の間に空白で区切った名前が在る本文の返戻
		return NameStart != std::string_view::npos && NameStart >= MinRun && Body.size() - NameEnd > MinRun && Body[NameStart] == ' ' &&
		Body[NameEnd] == ' ' && Body.substr(NameStart, NameEnd + 1 - NameStart).find_first_not_of(' ') != std::string_view::npos ?
		std::optional<std::string_view>(Body) :
		std::nullopt;
	}

	/**
	 * コメント記法の判定関数
	 * @param Rules 投影済の記法
	 * @param Text コメントの字面
	 * @param IsSourceHead 原文の先頭か
	 * @return 記号に対応する記法
	 */
	CommentNotation CommentRepository::NotationOf(
		const NotationRules &Rules,
		const std::string_view Text,
		const bool IsSourceHead
	) {
		for(const AvailableNotation &Available : Rules.AvailableNotations) {
			if(!IsNamedDirective(Available.Notation)) continue;
			const std::string_view Opener = OpenerOf(Available.Notation);
			if(!Text.starts_with(Opener)) continue;
			const char After = Text.size() == Opener.size() ? '\n' : Text[Opener.size()];
			// 名前迄を開始記号に含む指示記法の返戻
			if(After == '\n' || After == ' ' || Available.IsTabSeparated && After == '\t') return Available.Notation;
		}
		for(const CommentNotation Notation : { CommentNotation::RubyEmbeddedDocument, CommentNotation::StarBangBlock }) {
			// 行頭の埋込文書と，囲みの `!` を本文へ分けない記法（C / C++ の Doxygen，Rust の内部文書，SCSS の出力へ残すコメント）の返戻
			if(Rules.IsAvailable(Notation) && Text.starts_with(OpenerOf(Notation))) return Notation;
		}
		// 囲み記法の場合
		if(Text.starts_with(OpenerOf(CommentNotation::Block))) {
			// 文書の開始記号の後に続く字（空の囲み `/**/` と飾りの `/***` は文書でない）
			const std::string_view Document = OpenerOf(CommentNotation::DoubleStarBlock);
			const char Following = Text.size() > Document.size() ? Text[Document.size()] : '\0';
			// 文書の囲みを持つ言語の専用記法と，本文を持たない空の囲み・飾りを区別した記法の返戻（文書の対象か区切コメントかは別に判定する）
			return Rules.IsAvailable(CommentNotation::DoubleStarBlock) && Text.starts_with(Document) && Following != '/' &&
			Following != '*' ? CommentNotation::DoubleStarBlock : CommentNotation::Block;
		}
		// 行記法 `//` の場合
		if(Text.starts_with(OpenerOf(CommentNotation::SlashLine))) {
			if(Rules.IsAvailable(CommentNotation::SlashBangLine) && Text.starts_with(OpenerOf(CommentNotation::SlashBangLine))) {
				// `!` を本文へ分けない専用記法の返戻
				return CommentNotation::SlashBangLine;
			}
			const std::string_view Triple = OpenerOf(CommentNotation::TripleSlashLine);
			// ３個のスラッシュを記号に定める言語の記法を飾りの罫線（４個以上）と区別した記法の返戻（C / C++ の Doxygen，C#・Java・Rust・Swift の文書，TypeScript の指示子）
			return Rules.IsAvailable(CommentNotation::TripleSlashLine) && Text.starts_with(Triple) &&
			(Text.size() == Triple.size() || Text[Triple.size()] != Triple.back()) ?
			CommentNotation::TripleSlashLine :
			CommentNotation::SlashLine;
		}
		// 原文の先頭のシバンと，Ruby の `#:` を一般の行コメントと区別した記法の返戻
		for(const CommentNotation Notation : { CommentNotation::HashBangLine, CommentNotation::HashColonLine }) if(
			Rules.IsAvailable(Notation) && (IsSourceHead || Notation != CommentNotation::HashBangLine) &&
			Text.starts_with(OpenerOf(Notation))
		) return Notation;
		// 残る行記法 `#` の返戻
		return CommentNotation::HashLine;
	}

	/**
	 * 実行時設定の組の並びの取得関数（整形仕様 4.5）
	 * 処理系と同じく，囲みの印が在れば其の中の区切った組を，無ければ注記全体を一つの組として読み，全体が組に為らない注記は組の並びとしない
	 * 名前は空白・引用符・区切りで終え，値は値を囲む引用符の内側か，引用符・組の区切り・空白の前迄とする
	 * 計算量：コメントの長さ L に対し O(L)
	 * @param Forms 行コメントの登録形の判定条件
	 * @param Text コメント全文
	 * @return 組の並び（組として読めない注記と実行時設定を持たない言語は無し）
	 */
	std::optional<std::vector<MagicCommentPair>> CommentRepository::MagicCommentPairsOf(
		const RegisteredLineForms &Forms,
		std::string_view Text
	) {
		// 組の区切りと引用符を定めない（実行時設定を持たない）言語の返戻
		if(Forms.MagicCommentSeparators.empty() || Forms.MagicCommentQuotes.empty()) return std::nullopt;
		Text.remove_prefix(std::min(OpenerOf(CommentNotation::HashLine).size(), Text.size()));
		// 囲みの印の中だけを組の並びとする
		const std::string_view Marker = Forms.MagicCommentMarker;
		const size_t Open = Text.find(Marker);
		const bool IsModeline = Open != std::string_view::npos;
		if(IsModeline) {
			const size_t Close = Text.find(Marker, Open + Marker.size());
			// 閉じの印の無い囲みの返戻
			if(Close == std::string_view::npos) return std::nullopt;
			Text = Text.substr(Open + Marker.size(), Close - Open - Marker.size());
		}
		const std::string_view Quotes = Forms.MagicCommentQuotes, Separators = Forms.MagicCommentSeparators;
		const std::string_view Delimiters = Forms.MagicCommentDelimiters, Joiners = Forms.MagicCommentNameJoiners;
		// 名前を終える字か
		const auto IsNameEnd = [&](const char Char) -> bool {
			// 空白・引用符・区切りかの返戻
			return std::isspace(static_cast<unsigned char>(Char)) || Quotes.find(Char) != std::string_view::npos ||
			Separators.find(Char) != std::string_view::npos || Delimiters.find(Char) != std::string_view::npos;
		};
		std::vector<MagicCommentPair> Pairs;
		size_t Pos = 0;
		// 空白の読飛
		const auto SkipSpaces = [&Text, &Pos]() -> void {
			while(Pos < Text.size() && std::isspace(static_cast<unsigned char>(Text[Pos]))) ++Pos;
		};
		// 設定名と値の組の読取
		while(Pos < Text.size()) {
			while(Pos < Text.size() && IsNameEnd(Text[Pos])) ++Pos;
			const size_t NameStart = Pos;
			while(Pos < Text.size() && !IsNameEnd(Text[Pos])) ++Pos;
			const size_t NameEnd = Pos;
			SkipSpaces();
			if(Pos == Text.size()) break;
			if(Separators.find(Text[Pos]) == std::string_view::npos) {
				// 注記全体が組に為らない事の返戻
				if(!IsModeline) return std::nullopt;
				continue;
			}
			++Pos;
			SkipSpaces();
			if(Pos == Text.size()) break;
			// 引用符の有無に応じた値の範囲の取得
			std::string_view Value;
			if(Text[Pos] == Quotes.front()) {
				// 引用符の内側（逆斜線の次の字を値に含める）
				const size_t ValueStart = ++Pos;
				while(Pos < Text.size() && Text[Pos] != Quotes.front()) Pos += Text[Pos] == '\\' ? 2 : 1;
				Pos = std::min(Pos, Text.size());
				Value = Text.substr(ValueStart, Pos - ValueStart);
				if(Pos < Text.size()) ++Pos;
			} else {
				const size_t ValueStart = Pos;
				while(
					Pos < Text.size() && Text[Pos] != Quotes.front() && Delimiters.find(Text[Pos]) == std::string_view::npos &&
					!std::isspace(static_cast<unsigned char>(Text[Pos]))
				) ++Pos;
				Value = Text.substr(ValueStart, Pos - ValueStart);
			}
			if(IsModeline) while(
				Pos < Text.size() &&
				(Delimiters.find(Text[Pos]) != std::string_view::npos || std::isspace(static_cast<unsigned char>(Text[Pos])))
			) ++Pos;
			else {
				SkipSpaces();
				// 組の後に続く字を持つ注記の返戻
				if(Pos < Text.size()) return std::nullopt;
			}
			// 大小文字と連結の字を揃えた名前
			std::string Name(Text.substr(NameStart, NameEnd - NameStart));
			ToLowerAscii(Name);
			for(char &Char : Name) if(Joiners.find(Char) != std::string_view::npos) Char = Joiners.back();
			Pairs.push_back({ std::move(Name), Value });
		}
		// 組の並びの返戻
		return Pairs;
	}

	/**
	 * 組として読めない注記の符号化の宣言の判定関数（整形仕様 4.5）
	 * 登録語と区切りと名前を処理系毎の登録定義（登録語の大小文字，区切りの前の空白，名前の字）で探す
	 * 計算量：コメントの長さ L に対し O(L)
	 * @param Forms 行コメントの登録形の判定条件
	 * @param Text コメント全文
	 * @return 登録語と区切りの後に名前が続けば true
	 */
	bool CommentRepository::HasEncodingDeclaration(const RegisteredLineForms &Forms, const std::string_view Text) {
		const std::string_view Word = Forms.EncodingDeclarationWord;
		// 登録語を持たない言語の返戻
		if(Word.empty()) return false;
		// 照合用の複製（大小文字を問わない言語は小文字へ揃える）
		std::string Folded(Text);
		if(Forms.IsEncodingWordCaseless) ToLowerAscii(Folded);
		for(size_t Found = Folded.find(Word); Found != std::string::npos; Found = Folded.find(Word, Found + 1)) {
			size_t Pos = Found + Word.size();
			if(Forms.AllowsEncodingSeparatorSpace) while(Pos < Text.size() && std::isspace(static_cast<unsigned char>(Text[Pos]))) ++Pos;
			if(Pos == Text.size() || Forms.EncodingDeclarationSeparators.find(Text[Pos]) == std::string_view::npos) continue;
			do ++Pos;
			while(Pos < Text.size() && (Text[Pos] == ' ' || Text[Pos] == '\t'));
			// 登録語と区切りの後に名前が続く事の返戻
			if(
				Pos < Text.size() && (
					Forms.EncodingNameChars.empty() ?
					!std::isspace(static_cast<unsigned char>(Text[Pos])) :
					std::isalnum(static_cast<unsigned char>(Text[Pos])) || Forms.EncodingNameChars.find(Text[Pos]) != std::string_view::npos
				)
			) return true;
		}
		// 登録語と区切りと名前を持たない事の返戻
		return false;
	}

	/**
	 * 行コメントの登録形の判定関数（整形仕様 4.5）
	 * 注記を一度だけ実行時設定の組として読み，組の名前が登録語の物を実行時設定の形（値の有効性と読まれる範囲は問わない），組の名前が符号化の名前の物か，組として読めない注記で登録語と区切りと名前が続く物を符号化の指定の形とする
	 * 形だけを見る為，処理系が読む先頭の行に在るかは呼出側が確かめる（Python は先頭の２行，Ruby は１行目とシバンの次の行だけを読む）
	 * 計算量：コメントの長さ L と登録語の数 W に対し O(W · L)
	 * @param Forms 行コメントの登録形の判定条件
	 * @param Text コメント全文
	 * @return 実行時設定の組の並びと，符号化の指定・実行時設定の形か
	 */
	CommentRepository::RegisteredLineForm CommentRepository::RegisteredLineFormOf(
		const RegisteredLineForms &Forms,
		const std::string_view Text
	) {
		RegisteredLineForm Form { MagicCommentPairsOf(Forms, Text) };
		// 名前が表に在る組を持つか
		const auto HasPair = [&Form](const std::vector<std::string_view> &Names) -> bool {
			// 名前の照合結果の返戻
			return Form.Pairs && std::ranges::any_of(
				*Form.Pairs,
				[&Names](const MagicCommentPair &Pair) -> bool {
					// 表に在る名前かの返戻
					return std::ranges::find(Names, Pair.Name) != Names.end();
				}
			);
		};
		Form.IsEncoding = Form.Pairs ? HasPair(Forms.EncodingMagicCommentNames) : HasEncodingDeclaration(Forms, Text);
		Form.IsMagic = HasPair(Forms.MagicCommentNames);
		// 判定結果の返戻
		return Form;
	}

	/**
	 * 論理行の本文を行コメントで保てるかの判定関数（整形仕様 7.6, 7.7）
	 * 行コメントの本文を行の途中で終える字面・改行（字句化前に展開する Unicode の逃避で書いた物を含む）と，行末で次の行を繋ぐ字面を持つ本文は保てない
	 * @param Rules 投影済の記法
	 * @param Body 論理行の本文
	 * @return 行コメントの本文として同じ行で終わるなら true
	 */
	bool CommentRepository::CanHoldInLineComment(const NotationRules &Rules, const std::string_view Body) {
		// 行を終える字面・繋ぐ字面の返戻
		if(
			!Rules.LineCommentTerminator.empty() && Body.find(Rules.LineCommentTerminator) != std::string_view::npos || std::ranges::any_of(
				Rules.LineCommentBreaks,
				[Body](const std::string_view Break) -> bool {
					// 改行の字面を含むかの返戻
					return Body.find(Break) != std::string_view::npos;
				}
			) || std::ranges::any_of(
				Rules.LineSplices,
				[Body](const std::string_view Splice) -> bool {
					// 行末に繋ぐ字面を持つかの返戻
					return Body.ends_with(Splice);
				}
			)
		) return false;
		// 字句化前に LF・CR へ展開する逃避を持たず行コメントで保てるかの返戻
		return !Rules.DecodesUnicodeEscapes || !ParseAdapter::HasEscapedLineBreak(Body);
	}

	/**
	 * 通常コメントの行の字面かの判定関数（整形仕様 4.5, 7.6）
	 * 記法を変えた通常コメントの各行が，別の記法（シバン・三重スラッシュ等）にも処理系の登録形にも為らない事を確かめる
	 * 計算量：字面の長さ L と登録語の数 W に対し O(W · L)
	 * @param Rules 投影済の記法と其の成立条件
	 * @param Text 行コメントの字面（複数行は行毎に見る）
	 * @param Notation 行コメントの記法
	 * @return 全ての行を其の記法の通常コメントとして読むなら true
	 */
	bool CommentRepository::IsPlainLineText(
		const NotationRules &Rules,
		const std::string_view Text,
		const CommentNotation Notation
	) {
		for(size_t Cursor = 0; Cursor <= Text.size();) {
			const size_t End = std::min(Text.find('\n', Cursor), Text.size());
			const std::string_view Line = Text.substr(Cursor, End - Cursor);
			// 行の登録形の判定
			const RegisteredLineForm Form = RegisteredLineFormOf(Rules.OutputForms.Conditions, Line);
			// 別の記法か符号化宣言・実行時設定の登録形に為る行を持つ事の返戻
			if(NotationOf(Rules, Line, true) != Notation || Form.IsEncoding || Form.IsMagic) return false;
			Cursor = End + 1;
		}
		// 全ての行を通常コメントとして読む事の返戻
		return true;
	}

	/**
	 * 区切コメントの記録かの判定関数（整形仕様 7.4）
	 * @param Record 記法の定義を記録したコメントの記録
	 * @return 等号の列で名前を囲む１行の独立した説明のコメントなら true
	 */
	bool CommentRepository::IsSeparator(const CommentRecord &Record) {
		// 区切の本文の有無の返戻
		return SeparatorBodyOf(Record).has_value();
	}

	/**
	 * 区切コメントの字面かの判定関数（整形仕様 7.4）
	 * @param Rules 文法の記法の定義
	 * @param Text 記号を含むコメントの字面
	 * @return 等号の列で名前を囲む１行の説明のコメントなら true
	 */
	bool CommentRepository::IsSeparatorText(const NotationRules &Rules, const std::string_view Text) {
		CommentRecord Record { std::string(Text), { CommentPlacement::Before } };
		Record.Notation = NotationOf(Rules, Text, false);
		AttachRules(Rules, Record);
		Decompose(Record);
		// 記録としての判定の返戻
		return IsSeparator(Record);
	}

	/**
	 * 前の成員の文書かの判定関数
	 * 記号へ密着する本文の頭で前の成員を指す文書（Doxygen の `///<` `//!<` 等の行と囲みの文書）は，次の宣言の文書にも通常コメントにも揃えない
	 * 記号の後に区切空白を置いた頭（`/// <pre>` の要素）は前の成員を指さない
	 * @param Rules 投影済の記法
	 * @param Record コメントの記録
	 * @return 前の成員を指す頭が記号へ密着する本文なら true
	 */
	bool CommentRepository::IsMemberDocument(const NotationRules &Rules, const CommentRecord &Record) {
		// 前の成員を指す頭を持つ文法で其の頭が記号へ密着するかの返戻
		return !Rules.MemberDocumentHead.empty() && Record.Format.AttachFirstLine && Record.Text.starts_with(Rules.MemberDocumentHead);
	}

	/**
	 * 囲みの文書へ揃える行形式の文書かの判定関数（整形仕様 7.6 の記法の選択，記号だけでなく本文の文書形式も照合する）
	 * 採用する囲みの文書と同じ文書形式の独立した説明の行形式の文書を対象とし，C / C++ の成員の後の `///<` `//!<` は行末の文書で除く
	 * @param Record 記法の定義を記録したコメントの記録
	 * @return 囲みの文書へ揃える行形式の文書なら true
	 */
	bool CommentRepository::IsBlockLineDocument(const CommentRecord &Record) {
		const NotationRules &Rules = RulesOf(Record);
		const CommentOutputForm *const Document = Rules.AdoptedFormOf(CommentUse::Document);
		// 採用する囲みの文書と同じ文書形式を持つ独立した行形式の文書かの返戻
		return Record.Kind == CommentKind::OwnLine && !IsBlockNotation(Record.Notation) && Record.Format.InputDocument && Document &&
		IsBlockNotation(Document->Notation) && Record.Format.InputDocument == Rules.DocumentFormOf(Document->Notation) &&
		!IsMemberDocument(Rules, Record);
	}

	/**
	 * 本文を囲みの文書へ入れれるかの判定関数（整形仕様 7.6, 7.7）
	 * 本文が囲みを閉じる記号（入れ子の囲みを持つ記法では開く記号も）を含めば，囲みへ入れると文書が途中で閉じる
	 * @param Rules 文法の記法の定義
	 * @param Text 文書の本文
	 * @return 囲みの文書へ入れても文書が途中で閉じなければ true
	 */
	bool CommentRepository::CanFrame(const NotationRules &Rules, const std::string_view Text) {
		// 囲みの終端と入れ子の開始を本文に持たないかの返戻
		return Text.find(CloserOf(CommentNotation::DoubleStarBlock)) == std::string_view::npos &&
		(!Rules.IsBlockCommentNested || Text.find(OpenerOf(CommentNotation::Block)) == std::string_view::npos);
	}

	/**
	 * 埋込文書の前後の空の行の印の取得関数（整形仕様 7.3, 7.4）
	 * 同じ錨の続く埋込文書の記録の並び毎に，前後の空の行（開始記号・終端記号の行の残りと其れに続く空の行）に印を付ける
	 * 囲みの開始記号・終端記号だけの行と同じく文書の論理行ではなく，通常コメントでは構造として保つ時だけ配置の段が残す
	 * @param Records 原文順の記録の並び
	 * @return 記録毎の埋込文書の前後の空の行か
	 */
	std::vector<bool> CommentRepository::EmbeddedEdgesOf(const std::span<const CommentRecord> Records) {
		std::vector<bool> IsEdge(Records.size(), false);
		for(size_t Begin = 0; Begin < Records.size();) {
			size_t End = Begin;
			while(
				End < Records.size() && Records[End].Notation == CommentNotation::RubyEmbeddedDocument &&
				(End == Begin || Records[End].Anchor == Records[Begin].Anchor)
			) ++End;
			if(End == Begin) {
				++Begin;
				continue;
			}
			// 並びの前後の空の行の印
			for(size_t Pos = Begin; Pos < End && Records[Pos].Text.empty(); ++Pos) IsEdge[Pos] = true;
			for(size_t Pos = End; Pos > Begin && Records[Pos - 1].Text.empty(); --Pos) IsEdge[Pos - 1] = true;
			Begin = End;
		}
		// 前後の空の行の印の返戻
		return IsEdge;
	}

	/**
	 * 出力記法への変え方の取得関数（整形仕様 7.6 の記法の選択）
	 * 区切コメントは採用する区切の記法へ揃える
	 * 囲みの文書へ揃える行形式の文書は１つの囲みの文書へ揃える（囲みを閉じる又は入れ子を開く本文は行形式の儘とする）
	 * 採用する囲みの文書と同じ文書形式の別の囲み（C / C++ の感嘆符付の囲み）は其の文書の記法へ揃える
	 * 行毎の記号を持たない埋込文書は通常コメントの記法へ揃える（行コメントが別の記法・登録形に為る物は除く）
	 * 処理系が読む記録と其の他は入力の記法の儘とする（入力の記法は記録に残し，配置に依る通常コメントの行形式は配置の段が選ぶ）
	 * @param Record 記法の定義を記録したコメントの記録
	 * @return 出力記法への変え方
	 */
	CommentRepository::OutputChange CommentRepository::OutputChangeOf(const CommentRecord &Record) {
		// 入力の記法で出力する処理系が読む記録の返戻
		if(Record.Role == CommentRole::Directive) return OutputChange::None;
		const NotationRules &Rules = RulesOf(Record);
		// 区切の記法へ揃える区切コメントの返戻
		if(SeparatorBodyOf(Record) && Rules.AdoptedFormOf(CommentUse::Separator)) return OutputChange::Separator;
		// 囲みの文書へ揃える行形式の文書の返戻
		if(IsBlockLineDocument(Record) && CanFrame(Rules, Record.Text)) return OutputChange::Frame;
		const CommentOutputForm *const Document = Rules.AdoptedFormOf(CommentUse::Document);
		// 採用する囲みの文書と同じ文書形式の別の囲みの独立した説明の文書（C / C++ の成員の後の `///<` `//!<` は行末の文書で除く）の返戻
		if(
			Record.Kind == CommentKind::OwnLine && !IsMemberDocument(Rules, Record) && Document && IsBlockNotation(Document->Notation) &&
			IsBlockNotation(Record.Notation) && Record.Notation != Document->Notation && Record.Format.InputDocument &&
			Record.Format.InputDocument == Rules.DocumentFormOf(Document->Notation) && !Record.Format.AttachFirstLine
		) return OutputChange::DocumentNotation;
		// 空の論理行も記号付の行で表し，行コメントが別の記法・登録形に為らない埋込文書の返戻（整形仕様 4.5, 7.3, 7.6）
		if(
			const CommentOutputForm *const Plain = Rules.AdoptedFormOf(CommentUse::Plain);
			Record.Notation == CommentNotation::RubyEmbeddedDocument && Plain
		) {
			CommentRecord Line = Record;
			Line.Notation = Plain->Notation;
			// 通常コメントの行の字面に為る埋込文書の返戻
			if(IsPlainLineText(Rules, Compose(Line), Plain->Notation)) return OutputChange::PlainLine;
		}
		// 入力の記法の儘の記録の返戻
		return OutputChange::None;
	}

	/**
	 * 変え方に従う出力記法の取得関数
	 * @param Record 記法の定義を記録したコメントの記録
	 * @param Change 出力記法への変え方
	 * @return 出力する記法
	 */
	CommentNotation CommentRepository::OutputNotationFor(const CommentRecord &Record, const OutputChange Change) {
		switch(Change) {
		case OutputChange::Separator:
			// 区切の採用記法の返戻
			return RulesOf(Record).AdoptedFormOf(CommentUse::Separator)->Notation;
		case OutputChange::Frame:
		case OutputChange::DocumentNotation:
			// 文書の採用記法の返戻
			return RulesOf(Record).AdoptedFormOf(CommentUse::Document)->Notation;
		case OutputChange::PlainLine:
			// 通常コメントの採用記法の返戻
			return RulesOf(Record).AdoptedFormOf(CommentUse::Plain)->Notation;
		case OutputChange::None:
			break;
		}
		// 入力の記法の返戻
		return Record.Notation;
	}

	/**
	 * 出力記法の取得関数（整形仕様 7.6 の記法の選択，記録を写さない）
	 * @param Record 記法の定義を記録したコメントの記録
	 * @return 出力する記法
	 */
	CommentNotation CommentRepository::OutputNotationOf(const CommentRecord &Record) {
		// 変え方に従う出力記法の返戻
		return OutputNotationFor(Record, OutputChangeOf(Record));
	}

	/**
	 * 配置の段が採用する行記法で置く記録の種類の取得関数（整形仕様 7.6 の記法の選択，行毎に保てるかは本文で決める）
	 * 一般の囲みと文書でない位置の文書の囲み，採用記法の行コメントと埋込文書の独立した記録，採用しない行記法（記号へ密着しない物）と文書でない位置の文書の行記法を，採用する行記法で置く
	 * 処理系が読む記録・区切コメントと，採用記法が囲みの文法は揃えない
	 * @param Record コメントの記録
	 * @param IsDocument 文書の位置に在る独立した記録か
	 * @return 行記法で置く記録の種類（揃えない記録は `None`）
	 */
	CommentRepository::LinePlacement CommentRepository::LinePlacementOf(const CommentRecord &Record, const bool IsDocument) {
		const NotationRules &Rules = RulesOf(Record);
		const CommentOutputForm *const Plain = Rules.AdoptedFormOf(CommentUse::Plain);
		if(!Plain || IsBlockNotation(Plain->Notation) || Record.Role == CommentRole::Directive || IsSeparator(Record)) {
			// 揃えない記録の返戻
			return LinePlacement::None;
		}
		// 文書でも区切でもない位置の文書の記法（成員の後の文書 `///<` 等を除いて通常コメントとする）
		const bool IsUndocumented = Rules.DocumentFormOf(Record.Notation) && !IsDocument && !IsMemberDocument(Rules, Record);
		if(Record.Notation == CommentNotation::Block || IsBlockNotation(Record.Notation) && IsUndocumented) {
			// 一般の囲みの返戻
			return LinePlacement::GeneralBlock;
		}
		// 論理行毎に記法を選ぶ採用記法の行コメントと埋込文書の独立した記録の返戻
		if(
			Record.Kind == CommentKind::OwnLine &&
			(Record.Notation == Plain->Notation || Record.Notation == CommentNotation::RubyEmbeddedDocument)
		) return LinePlacement::LineRecord;
		// 採用しない行記法（一般の行記法は記号へ密着しない物，文書でない位置の文書の記法は密着に依らず，密着は配置の段が保つ）の返戻
		if(
			Record.Notation != Plain->Notation && (
				IsUndocumented || !Record.Format.AttachFirstLine &&
				(Record.Notation == CommentNotation::SlashLine || Record.Notation == CommentNotation::HashLine)
			)
		) return LinePlacement::OtherLine;
		// 揃えない記録の返戻
		return LinePlacement::None;
	}

	/**
	 * 群を纏める記法の取得関数（整形仕様 3.3, 7.3, 7.6）
	 * 配置の段が採用する行記法で置く記録は出力で隣接する其の記法の群と並ぶ為に採用する行記法とし，其の他は出力記法とする（囲みの記法は群を作らない）
	 * 囲みの文書へ揃える行形式の文書は，行毎に揃えるかを決めず入力の記法で纏める（投影が１つの文書の記録へ統合する行と同じ群）
	 * @param Record 記法の定義を記録し本文を分解したコメントの記録
	 * @param IsDocument 文書の位置に在る独立した記録か
	 * @return 群を纏める記法
	 */
	CommentNotation CommentRepository::GroupNotationOf(const CommentRecord &Record, const bool IsDocument) {
		if(LinePlacementOf(Record, IsDocument) != LinePlacement::None) {
			// 採用する行記法で置く記録の採用記法の返戻
			return RulesOf(Record).AdoptedFormOf(CommentUse::Plain)->Notation;
		}
		const OutputChange Change = OutputChangeOf(Record);
		// 囲みの文書へ揃える行形式の文書の入力の記法と，其の他の出力記法の返戻
		return Change == OutputChange::Frame ? Record.Notation : OutputNotationFor(Record, Change);
	}

	/**
	 * 群の所属の取得関数（整形仕様 3.3, 7.3）
	 * 投影が記録した原文で続く独立行の説明のコメントの並び（同じ錨の同じ側で空白だけを挟む物）を，文書の位置に従う群を纏める記法の同じ続き毎の群に分ける
	 * 計算量：記録の数 C に対し O(C log C)
	 * @param Records 記録の並び
	 * @param IsDocuments 記録毎の文書の位置に在る独立した記録か
	 * @return 記録毎の群（並びに属さない記録は `NoCommentGroup`）
	 */
	std::vector<CommentGroupId> CommentRepository::MembershipOf(
		const std::span<const CommentRecord> Records,
		const std::span<const uint8_t> IsDocuments
	) {
		// 原文の群と順に従う記録の整列
		std::vector<size_t> Order;
		Order.reserve(Records.size());
		for(size_t Index = 0; Index < Records.size(); ++Index) if(Records[Index].Format.Group != NoCommentGroup) Order.push_back(Index);
		std::stable_sort(
			Order.begin(),
			Order.end(),
			[&Records](const size_t Lhs, const size_t Rhs) -> bool {
				// 並びと原文の順の比較の返戻
				return std::pair(Records[Lhs].Format.Group, Records[Lhs].OriginalOrder) <
				std::pair(Records[Rhs].Format.Group, Records[Rhs].OriginalOrder);
			}
		);
		std::vector<CommentGroupId> Groups(Records.size(), NoCommentGroup);
		CommentGroupId Next = 0;
		CommentNotation PreviousNotation = {};
		// 並びの中の群を纏める記法の同じ続き（囲みの記法は単独）
		for(size_t Position = 0; Position < Order.size(); ++Position) {
			const size_t Index = Order[Position];
			const CommentNotation Notation = GroupNotationOf(Records[Index], IsDocuments[Index]);
			const bool Joins =
			Position && Records[Order[Position - 1]].Format.Group == Records[Index].Format.Group && !IsBlockNotation(Notation) &&
			PreviousNotation == Notation;
			Groups[Index] = Joins ? Groups[Order[Position - 1]] : Next++;
			PreviousNotation = Notation;
		}
		// 記録毎の群の返戻
		return Groups;
	}

	/**
	 * 出力記法の記録の取得関数（整形仕様 7.6 の記法の選択）
	 * @param Record 記法の定義を記録したコメントの記録
	 * @return 出力する記法と本文を持つ記録の写し
	 */
	CommentRecord CommentRepository::OutputFormOf(const CommentRecord &Record) {
		CommentRecord Output = Record;
		const OutputChange Change = OutputChangeOf(Record);
		Output.Notation = OutputNotationFor(Record, Change);
		if(Change == OutputChange::Separator) {
			// 区切の本文と，開始記号へ密着させる本文の頭
			const std::string_view Head = RulesOf(Record).AdoptedFormOf(CommentUse::Separator)->GluedHead;
			const std::string_view Body = *SeparatorBodyOf(Record);
			Output.Text = Head.empty() ? std::string(Body) : std::string(Head).append(" ").append(Body);
			Output.Format.AttachFirstLine = !Head.empty();
		} else if(Change == OutputChange::Frame) {
			// 開始記号と終端記号を独立した行に置く囲みの文書
			Output.Format.AttachFirstLine = false;
			FrameDocument(Output);
		}
		// 出力記法の記録の返戻
		return Output;
	}

	/**
	 * コメントの記法が処理系の読む文書属性に為るかの判定関数（整形仕様 4.5）
	 * 文書の記法を処理系が属性として読む言語（Rust の外側・内側の行と囲みの文書）だけが真に為り，同期と静的検査が文書を改変しない判断に使う
	 * @param Rules 投影済の記法
	 * @param Notation コメントの記法（星を３つ以上並べた飾りの囲みは記法で文書と区別済）
	 * @return 文書属性に為る記法なら true
	 */
	bool CommentRepository::IsDocumentAttribute(const NotationRules &Rules, const CommentNotation Notation) {
		// 文書の記法を属性として読む文法の文書の記法かの返戻
		return Rules.DocumentCommentIsAttribute && (
			Notation == CommentNotation::TripleSlashLine || Notation == CommentNotation::SlashBangLine ||
			Notation == CommentNotation::StarBangBlock || Notation == CommentNotation::DoubleStarBlock
		);
	}

	/**
	 * 錨毎の記録の位置の再構築関数
	 * 計算量：記録の数 K に対し平均 O(K)
	 */
	void CommentRepository::Reindex() {
		ByAnchor.clear();
		for(size_t Index = 0; Index < Records.size(); ++Index) ByAnchor[Records[Index].Anchor].push_back(Index);
		// 終了
		return;
	}

	/**
	 * 全ての記録の取得関数
	 * @return 原文順の記録の読取専用の並び
	 */
	std::span<const CommentRecord> CommentRepository::GetRecords() const {
		// 記録の並びの返戻
		return Records;
	}

	/**
	 * 文書化対象の定義の取得関数
	 * @return 原文順の文書化対象の定義の読取専用の並び
	 */
	std::span<const DocumentationSite> CommentRepository::GetDocumentationSites() const {
		// 文書化対象の定義の並びの返戻
		return DocumentationSites;
	}

	/**
	 * 文書化対象の定義の追加関数（整形仕様 3.2）
	 * @param Site 定義の錨と記法の定義
	 */
	void CommentRepository::AddDocumentationSite(const DocumentationSite &Site) {
		DocumentationSites.push_back(Site);
		// 終了
		return;
	}

	/**
	 * 錨の記録の取得関数（整形仕様 3.2）
	 * 計算量：錨の記録の数 k に対し O(k)
	 * @param Anchor 錨の節点
	 * @return 錨の記録の写し（記録の並びの順）
	 */
	CommentRecordList CommentRepository::Get(const NodeId Anchor) const {
		CommentRecordList Found;
		if(const std::unordered_map<NodeId, std::vector<size_t>>::const_iterator Iter = ByAnchor.find(Anchor); Iter != ByAnchor.end()) {
			for(const size_t Index : Iter->second) Found.push_back(Records[Index]);
		}
		// 錨の記録の返戻
		return Found;
	}

	/**
	 * 記録の追加関数
	 * @param Record 追加する記録（錨の節点を持つ）
	 */
	void CommentRepository::Add(CommentRecord &&Record) {
		ByAnchor[Record.Anchor].push_back(Records.size());
		Records.push_back(std::move(Record));
		// 終了
		return;
	}

	/**
	 * 記録の差替関数
	 * 文書の同期が作り直した記録の並びへ差し替える
	 * @param Replacement 差し替える記録の並び
	 */
	void CommentRepository::Update(CommentRecordList &&Replacement) {
		Records = std::move(Replacement);
		Reindex();
		// 終了
		return;
	}

	/**
	 * 本文の差替関数（整形仕様 3.1，本文の正規化の段は記録の本文だけを変える）
	 * @param Texts 差し替える記録の位置と新しい本文
	 */
	void CommentRepository::UpdateTexts(std::vector<std::pair<size_t, std::string>> &&Texts) {
		for(auto &[Index, Text] : Texts) Records.at(Index).Text = std::move(Text);
		// 終了
		return;
	}

	/**
	 * 群の差替関数（整形仕様 3.3，文書の位置が確定した後の所属）
	 * @param Groups 記録毎の群
	 */
	void CommentRepository::UpdateGroups(const std::vector<CommentGroupId> &Groups) {
		for(size_t Index = 0; Index < Records.size(); ++Index) Records[Index].Format.Group = Groups.at(Index);
		// 終了
		return;
	}

	/**
	 * 錨の移譲の適用関数（整形仕様 7.2）
	 * 構文正規化が合成した最終の行先を記録毎に一度だけ引き，消えた節点の先行・末尾・内部の錨を行先の節点の前後へ移す
	 * 計算量：記録の数 K に対し平均 O(K)
	 * @param Trace 構文正規化の錨の移譲の記録
	 * @throws FormatLimitExceeded 移る先の無い錨が残る場合（構文変換の不具合で，置けないコメントとして隠さない，整形仕様 7.7）
	 */
	void CommentRepository::Remap(const RewriteTrace &Trace) {
		// 移譲の無い時の終了
		if(Trace.Moves.empty()) return;
		// 記録の配置と置き方の対応
		constexpr std::array<std::pair<CommentPlacement::Relation, AnchorSide>, 3> Sides {
			{
				{ CommentPlacement::After, AnchorSide::After },
				{ CommentPlacement::Before, AnchorSide::Before },
				{ CommentPlacement::Inside, AnchorSide::Inside }
			}
		};
		// 記録毎の移譲先と配置関係の更新
		for(CommentRecord &Record : Records) {
			AnchorSide Side = AnchorSide::Before;
			for(const auto &[Placement, Mapped] : Sides) if(Record.Placement.Side == Placement) Side = Mapped;
			const std::unordered_map<uint64_t, AnchorMove>::const_iterator Found =
			Trace.Moves.find(RewriteTrace::MoveKey(Record.Anchor, Side, Record.Kind == CommentKind::Inline));
			if(Found == Trace.Moves.end()) continue;
			const AnchorMove &Move = Found->second;
			if(Move.Node == NoNode) throw FormatLimitExceeded("a comment anchor has no transfer target", FailureReason::FormatterDefect);
			Record.Anchor = Move.Node;
			for(const auto &[Placement, Mapped] : Sides) if(Mapped == Move.Side) Record.Placement.Side = Placement;
			Record.Placement.IsEmptyBody = Record.Placement.IsEmptyBody || Move.IsEmptyBody;
			Record.Placement.IsRemovedCaseBody = Record.Placement.IsRemovedCaseBody || Move.IsRemovedCaseBody;
			if(Move.Reset == TokenReset::None) Record.Placement.TokensBefore = 0;
			if(Move.Reset == TokenReset::AllInsideAnchor) Record.Placement.TokensBefore = std::numeric_limits<uint16_t>::max();
		}
		// 文書化対象の定義の錨は定義の前の錨と同じ移譲に従う
		for(DocumentationSite &Site : DocumentationSites) {
			const std::unordered_map<uint64_t, AnchorMove>::const_iterator Found =
			Trace.Moves.find(RewriteTrace::MoveKey(Site.Anchor, AnchorSide::Before, false));
			if(Found == Trace.Moves.end()) continue;
			if(Found->second.Node == NoNode) {
				throw FormatLimitExceeded("a documentation site has no transfer target", FailureReason::FormatterDefect);
			}
			Site.Anchor = Found->second.Node;
		}
		Reindex();
		// 終了
		return;
	}
} // namespace Core
