#include "Preprocess.hpp"
#include "HtmlTag.hpp"
#include "NodeKind.hpp"
#include "FileIO.hpp"
#include "Postprocess.hpp"
#include "TextEdit.hpp"
#include <algorithm>
#include <cctype>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * 末尾の改行の数の取得関数
 * 末尾の改行の過不足も整形の対象（ファイル末尾は改行を正確に１個）で，正規化で消える前に数える
 * @param Source 原文（正規化前）
 * @return 末尾に連続する改行（CRLF・単独の CR を含む）のうち LF の数
 */
size_t Preprocess::CountTailNewlines(const std::string_view Source) {
	size_t Count = 0;
	// 末尾改行列の逆走
	for(size_t Idx = Source.size(); Idx && (Source[Idx - 1] == '\n' || Source[Idx - 1] == '\r'); --Idx) {
		if(Source[Idx - 1] == '\n') ++Count;
	}
	// 末尾の LF の数の返戻
	return Count;
}

/**
 * 改行を値其の物として保つ範囲の収集関数
 * リテラル（文字列・ヒアドキュメント等の逐語保持する葉）と PHP の地の文の範囲を原文順に集める
 * @param Source 原文
 * @param Language 対象言語
 * @param ShouldIncludeBlockComments 囲みコメントも保つ範囲に含めるか
 * @return 範囲（開始・終了のバイト位置）の並び
 */
std::vector<std::pair<uint32_t, uint32_t>> Preprocess::CollectLiteralRanges(
	const std::string &Source,
	const Lang Language,
	const bool ShouldIncludeBlockComments
) {
	// 逐語保持する範囲列
	std::vector<std::pair<uint32_t, uint32_t>> Literals;
	const TSLanguage *const TsLang = Language.TsLang();
	// 構文木を持たない言語は保つ範囲も持たない事の返戻
	if(!TsLang) return Literals;
	// 範囲収集用の構文木
	const TSSource Raw(Source, TsLang);
	// 構文木を得た場合
	if(Raw.IsParsed()) {
		WalkChildrenCursor(
			Raw.GetRoot(),
			[&](const TSNode Node) -> bool {
				const std::string_view Type = ts_node_type(Node);
				// PHP の地の文も出力其の物の為，リテラルと同じく改行を保つ
				if(
					const bool IsBlock = ShouldIncludeBlockComments && NodeKind::Comment.Contains(Type) && Raw.View(Node).starts_with("/*");
					!IsBlock && (
						!NodeKind::Leaf.Contains(Type) && !NodeKind::StringLikeInnerPreserve.Contains(Type) &&
						!(Language == Lang::PHP && NodeKind::PhpInlineHtml.Contains(Type)) || NodeKind::Comment.Contains(Type)
					)
					// リテラル以外の内側へ降りる事の返戻
				) return true;
				// 逐語範囲の記録
				Literals.emplace_back(Raw.Start(Node), Raw.End(Node));
				// リテラルの内側へ降りない事の返戻
				return false;
			}
		);
	}
	// 保つ範囲の並びの返戻
	return Literals;
}

/**
 * 改行様式が混在する入力の正規化関数
 * リテラル（文字列・ヒアドキュメント等の逐語保持する葉）の内側の CRLF は値其の物の為に保ち，其れ以外の CRLF を LF へ畳む
 * @param Source 正規化対象の原文（CRLF と LF が混在する，結果で上書きされる）
 * @param Language 対象言語
 * @return リテラルの外の CRLF を畳んだ場合 true
 */
bool Preprocess::NormalizeMixedNewlines(std::string &Source, const Lang Language) {
	// 保護範囲列
	const std::vector<std::pair<uint32_t, uint32_t>> Literals = CollectLiteralRanges(Source, Language, false);
	std::string Normalized;
	Normalized.reserve(Source.size());
	size_t LiteralIdx = 0;
	bool IsFolded = false;
	// 原文バイトの走査
	for(size_t Idx = 0; Idx < Source.size(); ++Idx) {
		while(LiteralIdx < Literals.size() && Literals[LiteralIdx].second <= Idx) ++LiteralIdx;
		// リテラルの外の CRLF だけを LF へ畳む
		if(
			Source[Idx] == '\r' && Idx + 1 < Source.size() && Source[Idx + 1] == '\n' &&
			!(LiteralIdx < Literals.size() && Literals[LiteralIdx].first <= Idx)
		) {
			IsFolded = true;
			continue;
		}
		Normalized += Source[Idx];
	}
	// 末尾の連続改行の剥がしと非空時の改行１個の付直
	while(!Normalized.empty() && (Normalized.back() == '\n' || Normalized.back() == '\r')) Normalized.pop_back();
	if(!Normalized.empty()) Normalized += '\n';
	// 正規化結果への置換
	Source = std::move(Normalized);
	// リテラルの外の CRLF を畳んだか（書戻で改行様式が変わるか）の返戻
	return IsFolded;
}

/**
 * 改行コードと末尾改行の LF 基準の正規化関数（行分割→再結合の往復と等価）
 * 改行様式が混在する入力は，リテラルの内側の改行（値其の物）を原文の儘保ち，其れ以外の CRLF を LF へ揃える（書戻は LF の儘）
 * @param Source 正規化対象の原文（結果で上書きされる）
 * @param Language 対象言語
 * @param MixedTail 混在する入力の末尾の改行の並びの格納先（混在しない入力では空の儘）
 * @return 末尾の改行の数以外の改行の姿（CRLF・単独の CR）を変えた場合 true
 */
bool Preprocess::NormalizeNewlines(std::string &Source, const Lang Language, std::string &MixedTail) {
	// 改行表現を変えた印
	bool IsFormChanged = false;
	// 単独 CR を改行と読む言語だけ LF 化し，空白・地の文・リテラルの CR は保持
	const bool IsLoneCrNewline = Language != Lang::Ruby && Language != Lang::Go && Language != Lang::Rust && Language != Lang::PHP;
	// C# の NEL・行区切り・段落区切りはリテラル・囲みコメントの外だけ LF 化
	static constexpr std::string_view CsNewlines[] = { "\xC2\x85", "\xE2\x80\xA8", "\xE2\x80\xA9" };
	const bool IsCsSeparator = Language == Lang::CSharp && std::any_of(
		std::begin(CsNewlines),
		std::end(CsNewlines),
		[&Source](const std::string_view Separator) -> bool {
			// 改行に読む区切りを含むかの返戻
			return Source.find(Separator) != std::string::npos;
		}
	);
	bool IsLoneCr = false;
	for(
		size_t Pos = IsLoneCrNewline ? Source.find('\r') : std::string::npos;
		Pos != std::string::npos && !IsLoneCr;
		Pos = Source.find('\r', Pos + 1) // 単独 CR の検出
	) IsLoneCr = Pos + 1 >= Source.size() || Source[Pos + 1] != '\n';
	// 個別の改行列を含む場合
	if(IsLoneCr || IsCsSeparator) {
		// 保護範囲列
		const std::vector<std::pair<uint32_t, uint32_t>> Literals = CollectLiteralRanges(Source, Language, IsCsSeparator);
		// 個別改行を LF 化する出力
		std::string Normalized;
		Normalized.reserve(Source.size());
		size_t LiteralIdx = 0;
		// 原文バイトの走査
		for(size_t Idx = 0; Idx < Source.size(); ++Idx) {
			while(LiteralIdx < Literals.size() && Literals[LiteralIdx].second <= Idx) ++LiteralIdx;
			// 保護範囲内の場合
			if(LiteralIdx < Literals.size() && Literals[LiteralIdx].first <= Idx) {
				Normalized += Source[Idx];
				continue;
			}
			if(IsLoneCr && Source[Idx] == '\r' && (Idx + 1 >= Source.size() || Source[Idx + 1] != '\n')) {
				Normalized += '\n';
				IsFormChanged = true;
				continue;
			}
			if(IsCsSeparator) if(
				const std::string_view *const Separator = std::find_if(
					std::begin(CsNewlines),
					std::end(CsNewlines),
					[&Source, Idx](const std::string_view Candidate) -> bool {
						// 此処から改行に読む区切りが始まるかの返戻
						return !Source.compare(Idx, Candidate.size(), Candidate);
					}
				); Separator != std::end(CsNewlines)
			) {
				// C# 区切りの LF 化
				Normalized += '\n';
				Idx += Separator->size() - 1;
				IsFormChanged = true;
				continue;
			}
			Normalized += Source[Idx];
		}
		Source = std::move(Normalized);
	}
	if(FileIO::IsNewlineMixed(Source)) {
		// 原文末尾改行列の退避
		MixedTail.assign(Source, Source.find_last_not_of("\r\n") + 1);
		// 混在の正規化で CRLF を畳んだかも改行の姿の変化に含めた結果の返戻
		return NormalizeMixedNewlines(Source, Language) || IsFormChanged;
	}
	// 単一様式の LF 正規化
	FileIO::Normalize(Source, false);
	// 改行の姿を変えたかの返戻
	return IsFormChanged;
}

/**
 * 埋込生テキストのコメント正規化関数
 * @param Raw script/style の内側の生テキスト
 * @param BaseStart 親要素（HTML 等）の中での Raw の開始バイト位置
 * @param SubLang 中身を解釈する tree-sitter 言語
 * @param Edits 出力編集列（絶対バイト位置 = BaseStart + 相対で追記する）
 */
void Preprocess::NormalizeEmbeddedRawText(
	const std::string_view Raw,
	const uint32_t BaseStart,
	const TSLanguage *const SubLang,
	std::vector<TextEdit> &Edits
) {
	// 言語未指定又は空テキストが有る場合の返戻
	if(!SubLang || Raw.empty()) return;
	// 埋込解析用パーサのスレッド局所保持と再利用（タグ毎の生成破棄を避ける為，言語は呼出毎に設定）
	struct ParserDeleter {
		/**
		 * 保持パーサ解放関数（スレッド終了時に呼出）
		 * @param Parser 解放対象のパーサ
		 */
		void operator()(TSParser *const Parser) const {
			// 保持パーサの解放
			ts_parser_delete(Parser);
			// 終了
			return;
		}
	};
	// 走脈毎の解析器
	thread_local const std::unique_ptr<TSParser, ParserDeleter> ParserHolder(ts_parser_new());
	TSParser *const Parser = ParserHolder.get();
	// 前の文法が残った解析器で別言語を処理しない
	if(!ts_parser_set_language(Parser, SubLang)) return;
	bool IsExpired = false;
	TSTree *const Tree = ParseWithDeadline(Parser, nullptr, Raw, IsExpired);
	// 解析成功時
	if(Tree) {
		struct EmbeddedComment {
			uint32_t Start;
			uint32_t End;
			CommentAttach Comment;
		};
		// 埋込本文内のコメント列
		std::vector<EmbeddedComment> Comments;
		WalkChildrenCursor(
			ts_tree_root_node(Tree),
			[&](const TSNode Node) -> bool {
				const char *const Type = ts_node_type(Node);
				const bool IsComment = Type && std::string_view(Type) == "comment";
				if(IsComment) {
					if(const uint32_t Start = ts_node_start_byte(Node), End = ts_node_end_byte(Node); Start < End && End <= Raw.size()) {
						Comments.push_back({ Start, End, { std::string(Raw.substr(Start, End - Start)), false } });
					}
				}
				// コメントノードの内部には降りない事の返戻
				return !IsComment;
			}
		);
		// コメント群の走査
		for(size_t Begin = 0; Begin < Comments.size();) {
			size_t End = Begin + 1;
			// 連結可能な後続の探索
			while(End < Comments.size()) {
				const EmbeddedComment &Previous = Comments[End - 1], &Next = Comments[End];
				if(
					Next.Start < Previous.End ||
					!Postprocess::CanJoinComments(Previous.Comment.Text, Next.Comment.Text, Raw.substr(Previous.End, Next.Start - Previous.End))
				) break;
				++End;
			}
			std::vector<CommentAttach *> Group;
			for(size_t Idx = Begin; Idx < End; ++Idx) Group.push_back(&Comments[Idx].Comment);
			// コメント群の正規化
			Postprocess::NormalizeCommentGroup(Group);
			for(size_t Idx = Begin; Idx < End; ++Idx) {
				EmbeddedComment &Comment = Comments[Idx];
				if(Comment.Comment.Text != Raw.substr(Comment.Start, Comment.End - Comment.Start)) {
					TextEdit::Push(BaseStart + Comment.Start, BaseStart + Comment.End, std::move(Comment.Comment.Text), Edits);
				}
			}
			Begin = End;
		}
		// 埋込構文木の解放
		ts_tree_delete(Tree);
	}
	// 終了
	return;
}

/**
 * HTML のコメント準備関数
 * コメントは描画されない節点として文書の中の位置に留める（取り出すと前後の本文と空白が繋がり，文字参照の読みや描画が変わる）
 * 位置を保つ為に改行を作らず，本文の表記だけを其の場で整え，埋込言語（`<script>` の JavaScript / `<style>` の CSS）のコメントも正規化する
 * 内容を文字として読む要素（`<textarea>` 等）の内側はコメントではない為，何れも行わない
 * @param Src 対象の TSSource（HTML, 解析済である事を前提とする）
 */
void Preprocess::PrepareHtmlComments(TSSource &Src) {
	// HTML 内コメントの編集列
	std::vector<TextEdit> Edits;
	WalkChildrenCursor(
		Src.GetRoot(),
		[&](const TSNode Node) -> bool {
			const std::string_view TypeView(ts_node_type(Node));
			// 内容を文字として読む要素の内部には降りない事の返戻
			if(TypeView == "element" && HtmlVerbatimOf(Src, Node) == HtmlVerbatim::RawContent) return false;
			if(NodeKind::HtmlRawTextElement.Contains(TypeView)) {
				// HTML の raw_text は JS / CSS の時だけ専用文法で部分解析し，他の type は逐語保持
				if(const TSNode RawText = FirstNamedChildOfType(Node, "raw_text"); !ts_node_is_null(RawText) && DoesHtmlEmbedCode(Src, Node)) {
					NormalizeEmbeddedRawText(
						Src.View(RawText),
						Src.Start(RawText),
						Lang::Get(TypeView == "script_element" ? Lang::JavaScript : Lang::CSS).TsLang(),
						Edits
					);
				}
				// script / style の内部（処理済の raw_text）には降りない事の返戻
				return false;
			}
			// コメント以外の子孫走査を続ける事の返戻
			if(TypeView != "comment") return true;
			// 先行の扱いにしない（句点で改行を作らず読点へ退避し，本文の空白の並びを変えない）
			CommentAttach Comment { std::string(Src.View(Node)), false }, *Target = &Comment;
			// コメント本文の正規化
			Postprocess::NormalizeCommentGroup({ &Target, 1 });
			// 差異の登録
			if(Comment.Text != Src.View(Node)) TextEdit::Push(Src.Start(Node), Src.End(Node), std::move(Comment.Text), Edits);
			// コメントの内部へ降りない事の返戻
			return false;
		}
	);
	// コメント編集の一括適用
	TextEdit::Apply(Src, Edits);
	// 終了
	return;
}

/**
 * Java の字句化前 Unicode エスケープの展開関数
 * @param Src 解析済の原文
 * @param Out 展開後の原文（展開する物が無ければ空の儘）
 * @return 桁不足のエスケープ，又はリテラル・コメントの外で対を成さないサロゲートのエスケープ（javac が拒む字句誤り）が在れば false
 */
bool Preprocess::ExpandJavaUnicodeEscapes(const TSSource &Src, std::string &Out) {
	// 解析済原文
	const std::string &Source = Src;
	// 逃避の開始位置と桁を読み，値を返す（桁不足なら範囲外の値）
	const auto ReadEscape = [&Source](size_t &Pos) -> uint32_t {
		// 重ねた u の読飛し
		while(Pos < Source.size() && Source[Pos] == 'u') ++Pos;
		// 桁不足を範囲外の値で示す返戻
		if(Pos + 4 > Source.size()) return 0X110000;
		// １６進４桁の値
		uint32_t Value = 0;
		// 各１６進桁の走査
		for(size_t Digit = 0; Digit < 4; ++Digit) {
			// 現在の桁
			const char Char = Source[Pos + Digit];
			const int Hex = Char >= '0' && Char <= '9' ?
			Char - '0' :
			Char >= 'a' && Char <= 'f' ? Char + 10 - 'a' : Char >= 'A' && Char <= 'F' ? Char + 10 - 'A' : -1;
			// 桁不足の返戻
			if(Hex < 0) return 0X110000;
			// 次の桁の取込
			Value = Value << 4 | static_cast<uint32_t>(Hex);
		}
		// 逃避終端への移動
		Pos += 4;
		// 読んだ値の返戻
		return Value;
	};
	// 逃避１個（サロゲートの対は１文字に纏める）の原文の範囲・値・全て展開した原文での位置
	struct Escape {
		size_t Start;
		size_t End;
		size_t Expanded; // 逃避を全て展開した本文内の開始バイト位置
		uint32_t Value;
	};
	// 展開候補の逃避列
	std::vector<Escape> Escapes;
	// 原文字面の走査
	for(size_t Pos = 0; Pos < Source.size();) {
		// 逆斜線以外の場合
		if(Source[Pos] != '\\') {
			// 次のバイトへ移動
			++Pos;
			continue;
		}
		// 逆斜線列の先頭
		const size_t Begin = Pos;
		// 逆斜線列の終端探索
		while(Pos < Source.size() && Source[Pos] == '\\') ++Pos;
		// 偶数本の逆斜線は互いに逃避し合い，最後の逆斜線も展開の対象外
		if(!((Pos - Begin) % 2) || Pos >= Source.size() || Source[Pos] != 'u') continue;
		// 展開対象の逆斜線位置
		const size_t Start = Pos - 1;
		// 最初の UTF-16 値
		uint32_t Value = ReadEscape(Pos);
		// 上位サロゲートは直後の下位サロゲートの逃避と組んで１文字に為る
		if(Value > 0XD7FF && Value < 0XDC00 && Pos + 1 < Source.size() && Source[Pos] == '\\' && Source[Pos + 1] == 'u') {
			// 下位サロゲートの u 位置
			size_t Next = Pos + 1;
			if(const uint32_t Low = ReadEscape(Next); Low > 0XDBFF && Low < 0XE000) {
				// 上位の偏り (0XD800 << 10) と下位の偏り 0XDC00 を除いて 0X10000 を足す（定数部を畳んだ値）
				Value = (Value << 10) + Low - 0X35FDC00;
				// 対の終端への移動
				Pos = Next;
			}
		}
		// 桁不足は，リテラル・コメントの中でも字句化の前の展開で javac が拒む字句誤りの為，失敗の返戻
		if(Value > 0X10FFFF) return false;
		// 逃避範囲と値の記録
		Escapes.push_back({ .Start = Start, .End = Pos, .Expanded = 0, .Value = Value });
	}
	// 昇順の逃避と文書順のリテラル・コメントを１度突合して型を取得（コード内は空，逃避毎の祖先走査を回避）
	const auto ContextsOf = [&Escapes](const TSNode Root, const bool IsInExpanded) -> std::vector<std::string_view> {
		// 逃避位置に対応する字句種別の収集
		std::vector<std::string_view> Contexts;
		Contexts.reserve(Escapes.size());
		WalkChildrenCursor(
			Root,
			[&](const TSNode Node) -> bool {
				// 逃避を包む字句節点の選別
				const std::string_view Type = ts_node_type(Node);
				// 包む字句以外の子孫走査を続ける事の返戻
				if(!NodeKind::JavaEscapeHost.Contains(Type)) return true;
				const size_t Start = ts_node_start_byte(Node), End = ts_node_end_byte(Node);
				// 未割当逃避の走査
				for(size_t Next = Contexts.size(); Next < Escapes.size(); ++Next) {
					const size_t At = IsInExpanded ? Escapes[Next].Expanded : Escapes[Next].Start;
					if(At + 1 > End) break;
					// 行コメントの開きを成す逃避は，コメント内部でなくコードとして展開
					Contexts.push_back(At < Start || Type == "line_comment" && At < Start + 2 ? std::string_view() : Type);
				}
				// 包む字句の内側へ降りない事の返戻
				return false;
			}
		);
		Contexts.resize(Escapes.size());
		// 逃避毎の場所の返戻
		return Contexts;
	};
	// 其の場所で区切り・逃避・コメントの終端を作らず，字句の境界を変えない値かの判定
	const auto IsBenign = [](const std::string_view Context, const uint32_t Value) -> bool {
		// 字句の境界を変えない事の判定結果の返戻
		return Context.ends_with("literal") ?
		Value != '"' && Value != '\'' && Value != '\\' && Value != '\r' && Value != '\n' :
		Context == "line_comment" ? Value != '\r' && Value != '\n' : Context == "block_comment" && Value != '*' && Value != '/';
	};
	// 展開前の木で全て境界を変えなければ，先に在る展開が後の場所を変える事も無い為，原文の儘で確定する
	const std::vector<std::string_view> Before = ContextsOf(Src.GetRoot(), false);
	// 先頭から安全な逃避数
	size_t Benign = 0;
	while(Benign < Escapes.size() && IsBenign(Before[Benign], Escapes[Benign].Value)) ++Benign;
	// 展開不要の返戻
	if(Benign == Escapes.size()) return true;
	// javac の字句化と同じく全て展開した原文を解析し直し，其の木で場所を判定する
	std::string Full;
	Full.reserve(Source.size());
	size_t Copied = 0;
	for(Escape &Sequence : Escapes) {
		// 逃避間の原文と代替字面の結合
		Full.append(Source, Copied, Sequence.Start - Copied);
		Sequence.Expanded = Full.size();
		// 対を成さないサロゲートの代替文字に依る位置保持
		TextEdit::EncodeUtf8(Full, Sequence.Value > 0XD7FF && Sequence.Value < 0XE000 ? 0XFFFD : Sequence.Value);
		Copied = Sequence.End;
	}
	Full.append(Source, Copied, std::string::npos);
	const TSSource Reparsed(std::move(Full), Lang::Get(Lang::Java).TsLang());
	// 解析器を用意出来ないのは資源の枯渇で，整形を諦める（期限に達した解析は見送りの理由を分けて伝える）
	if(!Reparsed.IsParsed()) {
		if(Reparsed.HasExpiredParse()) throw FormatLimitExceeded("parsing exceeds the time limit");
		throw std::bad_alloc();
	}
	const std::vector<std::string_view> After = ContextsOf(Reparsed.GetRoot(), true);
	Copied = 0;
	// 各逃避の走査
	for(size_t Idx = 0; Idx < Escapes.size(); ++Idx) {
		const Escape &Sequence = Escapes[Idx];
		if(IsBenign(After[Idx], Sequence.Value)) {
			// 解析器はリテラルの中の u を重ねた逃避を逃避と読まない為，u を１個に揃える（値は変わらない）
			if(
				!After[Idx].ends_with("literal") ||
				std::string_view(Source).substr(Sequence.Start, Sequence.End - Sequence.Start).find("uu") == std::string_view::npos
			) continue;
			Out.append(Source, Copied, Sequence.Start - Copied);
			// 対を成す各逃避の正規化
			for(size_t Pos = Sequence.Start; Pos < Sequence.End; Pos += 4) {
				Pos = Source.find_first_not_of('u', Pos + 1);
				Out += "\\u";
				Out.append(Source, Pos, 4);
			}
			Copied = Sequence.End;
			continue;
		}
		// 対を成さないサロゲートはリテラル・コメント内で保持し，外への不正な展開は失敗返戻
		if(Sequence.Value > 0XD7FF && Sequence.Value < 0XE000) return false;
		// javac と同じ UTF-8 で展開し，解析器がコメント内で改行と読めない CR は LF 化（直後の LF と組む物は１改行へ）
		Out.append(Source, Copied, Sequence.Start - Copied);
		if(
			const bool IsCrBeforeLf = Sequence.Value == '\r' && (
				Sequence.End < Source.size() && Source[Sequence.End] == '\n' ||
				Idx + 1 < Escapes.size() && Escapes[Idx + 1].Start == Sequence.End && Escapes[Idx + 1].Value == '\n'
			);
			!IsCrBeforeLf
		) TextEdit::EncodeUtf8(Out, Sequence.Value == '\r' ? '\n' : Sequence.Value);
		Copied = Sequence.End;
	}
	if(Copied) Out.append(Source, Copied, std::string::npos);
	// 字句誤りが無かった事の返戻
	return true;
}

/**
 * Ruby の埋込ドキュメントの誤読の回避関数
 * Ruby と解析器で異なる埋込ドキュメントの終端判定を補正する
 * 早く閉じると残りの本文をコードと読み，整形で構文を壊す為，本文中の `=end` の `=` の直後へ原文に無い制御文字を挟んで解析し直す
 * 挟んだ文字はコメントの分離で本文から除く（`TSSource::AttachComments`）
 * @param Src 解析済の原文
 * @param Marker 挟んだ制御文字の格納先（挟まない場合は '\0'）
 * @return 印を挟んだ原文（挟む箇所が無い場合と，原文が候補の制御文字を全て含み選べない場合は空）
 */
std::string Preprocess::EscapeRubyEmbeddedDocs(const TSSource &Src, char &Marker) {
	// 解析済原文
	const std::string &Source = Src;
	Marker = '\0';
	// 行頭の `=begin` / `=end` の語の終わり（空白又は原文の終端が続く）かの判定
	const auto EndsWord = [&Source](const size_t Pos) -> bool {
		// 語の終わりかの返戻
		return Pos >= Source.size() || Source[Pos] == ' ' || Source[Pos] == '\t' || Source[Pos] == '\n' || Source[Pos] == '\r' ||
		Source[Pos] == '\f' || Source[Pos] == '\v';
	};
	std::vector<size_t> Escapes;
	size_t DocEnd = 0;
	WalkChildrenCursor(
		Src.GetRoot(),
		[&](const TSNode Node) -> bool {
			// コメント以外の子孫走査を続ける事の返戻
			if(!NodeKind::Comment.Contains(Node)) return true;
			const size_t Start = Src.Start(Node);
			if(Start < DocEnd || Start && Source[Start - 1] != '\n' || Source.compare(Start, 6, "=begin") || !EndsWord(Start + 6)) {
				// 前の埋込ドキュメントの内側と，行頭の `=begin` で始まらないコメントは対象外の返戻
				return false;
			}
			const size_t Found = Escapes.size();
			for(size_t LineEnd = Source.find('\n', Start); LineEnd != std::string::npos;) {
				const size_t Head = LineEnd + 1;
				LineEnd = Source.find('\n', Head);
				if(!Source.compare(Head, 4, "=end") && EndsWord(Head + 4)) {
					DocEnd = Head;
					// Ruby が閉じる行頭の `=end` に行着いた場合の走査打切の返戻
					return false;
				}
				// 行の中だけを探す（行に無い時に文書の終わり迄探すと，長い埋込ドキュメントで二乗の費用に為る）
				const std::string_view Line(Source.data() + Head, (LineEnd == std::string::npos ? Source.size() : LineEnd) - Head);
				for(size_t Pos = Line.find("=end"); Pos != std::string_view::npos; Pos = Line.find("=end", Pos + 4)) {
					Escapes.push_back(Head + Pos + 1);
				}
			}
			// 閉じの無い埋込ドキュメント（Ruby の構文誤り）は挟まない為，控えを取り消す
			Escapes.resize(Found);
			// コメントの内側へ降りない事の返戻
			return false;
		}
	);
	// 印を挟んだ再解析用原文
	std::string Escaped;
	// 挟む箇所が無い場合の空の返戻
	if(Escapes.empty()) return Escaped;
	// 印は原文に無い制御文字から選び，本文へ戻す時に原文の字面と取り違えない様にする
	Marker = TextEdit::AbsentControlChar(Source);
	// 印を選べない場合の空の返戻
	if(!Marker) return Escaped;
	// 印の数を含む領域確保
	Escaped.reserve(Source.size() + Escapes.size());
	size_t Copied = 0;
	for(const size_t Pos : Escapes) {
		Escaped.append(Source, Copied, Pos - Copied);
		Escaped += Marker;
		Copied = Pos;
	}
	Escaped.append(Source, Copied, std::string::npos);
	// 印を挟んだ原文の返戻
	return Escaped;
}

/**
 * Objective-C 判定関数（`.h` が C と共有する拡張子で，C の文法では解析出来ない宣言を見分ける）
 * @param Source 整形対象のソース（末尾改行正規化済）
 * @return Objective-C の印が行頭に現れたら true
 */
bool Preprocess::LooksObjectiveC(const std::string &Source) {
	// 囲みコメントを除いた行頭で，別語との部分一致を避け `#import`・`@interface` 系を検出
	static constexpr std::string_view Markers[] = { "#import", "@implementation", "@interface", "@protocol" };
	bool IsInBlockComment = false;
	// 原文の行走査
	for(size_t LineStart = 0; LineStart < Source.size();) {
		const size_t LineEnd = Source.find('\n', LineStart);
		const std::string_view Line(Source.data() + LineStart, (LineEnd == std::string::npos ? Source.size() : LineEnd) - LineStart);
		// 前処理指令も `@interface` も行頭インデントが許される為，先頭の空白を読み飛ばしてから照合する
		size_t Head = 0;
		while(Head < Line.size() && (Line[Head] == ' ' || Line[Head] == '\t')) ++Head;
		const std::string_view Code = Line.substr(Head);
		if(!IsInBlockComment) for(const std::string_view Marker : Markers) if(Code.starts_with(Marker)) {
			// Objective-C の印を発見した事の返戻
			if(const size_t After = Marker.size(); After >= Code.size() || !IsIdentifierChar(Code[After])) return true;
		}
		// ブロックコメントの開閉を数え，次行へ状態を持ち越す（行コメントは其の行の残りを覆うだけで持ち越さない）
		for(size_t Pos = 0; Pos + 1 < Line.size(); ++Pos) {
			if(IsInBlockComment && Line[Pos] == '*' && Line[Pos + 1] == '/') {
				IsInBlockComment = false;
				++Pos;
			} else if(!IsInBlockComment && Line[Pos] == '/' && Line[Pos + 1] == '*') {
				IsInBlockComment = true;
				++Pos;
			} else if(!IsInBlockComment && Line[Pos] == '/' && Line[Pos + 1] == '/') break;
		}
		if(LineEnd == std::string::npos) break;
		LineStart = LineEnd + 1;
	}
	// 印が無い事の返戻
	return false;
}

/**
 * XML 文書判定関数（`.ts` が Qt Linguist の翻訳ファイルと共有する拡張子で，XML を TypeScript として整形しない為に見分ける）
 * @param Source 整形対象のソース
 * @return XML の宣言・文書型で始まれば true（TypeScript のソースは `<?` / `<!` で始まれない）
 */
bool Preprocess::LooksXmlDocument(const std::string &Source) {
	// 最初の非空白位置
	const size_t Head = Source.find_first_not_of(" \t\r\n");
	// 宣言・文書型で始まるかの返戻
	return Head != std::string::npos && (!Source.compare(Head, 5, "<?xml") || !Source.compare(Head, 9, "<!DOCTYPE"));
}

/**
 * HTML の要素のネストが構文解析器の追える深さ・手間を超えるかの判定関数
 * 計算量：原稿の長さ N に対し O(N)（字句を一度だけ読み，開いた要素の並びは深さの分だけ伸び縮みする）
 * @param Source 整形対象のソース (HTML)
 * @return 見送る理由（構文解析器の追えない深さの要素を閉じるか解析の手間が上限を超えるネスト，又は閉じないコメント，見送らなければ空）
 */
std::string_view Preprocess::HtmlSkipReason(const std::string &Source) {
	// 見送る理由の文面
	static constexpr std::string_view Nesting = "nesting exceeds parser limit", Broken = "parser could not read the source";
	// 直列化領域・要素数欄の寸法と，固定猶予＋入力比例の解析費用上限
	static constexpr size_t Limit = 0X400, Header = 4, Budget = 0X8000000, DepthPerByteShift = 4;
	// 大小文字を区別しない名前の一致の判定
	const auto SameName = [](const std::string_view Lhs, const std::string_view Rhs) -> bool {
		// 英字の大小を揃えて一致するかの返戻
		return Lhs.size() == Rhs.size() && std::equal(
			Lhs.begin(),
			Lhs.end(),
			Rhs.begin(),
			[](const char Left, const char Right) -> bool {
				// 英字の大小を揃えた字の一致の返戻
				return std::tolower(static_cast<unsigned char>(Left)) == std::tolower(static_cast<unsigned char>(Right));
			}
		);
	};
	// 未閉鎖要素の積重ねが開始タグの要素を含めるかの判定（tree-sitter-html の tag_can_contain を写す，何方も小文字のタグ名）
	const auto CanContain = [](const std::string_view Parent, const std::string_view Child) -> bool {
		// 段落は区切りの要素の開始タグで閉じる事の返戻
		if(Parent == "p") return !NodeKind::HtmlParagraphCloser.Contains(Child);
		// 列の組は列だけを含む事の返戻
		if(Parent == "colgroup") return Child == "col";
		// 表のセルは行の開始タグでも閉じる事の返戻
		if(Child == "tr" && NodeKind::HtmlTableCellTag.Contains(Parent)) return false;
		for(
			const NodeKind::NodeTypeSet *const Group :
			{ &NodeKind::HtmlDefinitionTag, &NodeKind::HtmlRubyTag, &NodeKind::HtmlTableCellTag }
		) if(Group->Contains(Parent)) return !Group->Contains(Child); // 同じ要素群に属する子を含めるかの返戻
		// 其の他の閉じタグを省ける要素 (li / optgroup / tr) は同じ名前の開始タグで閉じる事の返戻
		return Parent != Child || !NodeKind::HtmlOptionalEndTag.Contains(Parent);
	};
	const size_t Size = Source.size(), Cap = std::max(Budget, Size << DepthPerByteShift);
	// 未閉鎖要素の積重ね
	struct OpenElement {
		std::string_view Name;
		size_t Serialized;
		bool IsLost;
	};
	std::vector<OpenElement> Open;
	size_t Work = 0;
	for(size_t Text = 0, Pos = 0; Text < Size; Text = Pos + 1) {
		Pos = std::min(Source.find('<', Text), Size);
		// 前のタグから此の `<` 迄の本文の字句（字の並びと，其れを区切る文字参照・`>`）と `<` の字句
		size_t Tokens = (Source.find_first_not_of(" \t\n\v\f\r", Text) < Pos) + (Pos < Size);
		for(size_t At = Text; At < Pos; ++At) if(Source[At] == '&' || Source[At] == '>') Tokens += 2;
		// 字句毎に開いた要素の並びを戻す手間
		Work += Open.size() * Tokens;
		// 解析の手間が上限を超える事の返戻
		if(Work > Cap) return Nesting;
		if(Pos + 1 >= Size) break;
		if(!Source.compare(Pos, 4, "<!--")) {
			const size_t Close = Source.find("-->", Pos + 4);
			// 閉じないコメントの返戻
			if(Close == std::string::npos) return Broken;
			Pos = Close + 2;
			continue;
		}
		const bool IsClosing = Source[Pos + 1] == '/';
		const size_t NameStart = Pos + IsClosing + 1;
		size_t NameEnd = NameStart;
		while(
			NameEnd < Size &&
			(std::isalnum(static_cast<unsigned char>(Source[NameEnd])) || Source[NameEnd] == '-' || Source[NameEnd] == ':')
		) ++NameEnd;
		// tree-sitter-html はタグの名前を英数字・`-`・`:` の何れで始まる物も読む
		if(NameEnd == NameStart) continue;
		const std::string_view Name(Source.data() + NameStart, NameEnd - NameStart);
		// 属性を数え値内の終端記号を無視し，引用符外の `<` は自己終端にせず次のタグとして処理
		size_t End = NameEnd;
		Tokens = 2;
		// 引用符の無い値の中か，直前の字が空白か
		bool IsUnquoted = false, IsSpaced = false;
		for(char Quote = '\0', Last = '\0'; End < Size; ++End) {
			const char Char = Source[End];
			if(Quote) {
				if(Char == Quote) Quote = '\0';
			} else if(Char == '>' || Char == '<') break;
			else if(std::isspace(static_cast<unsigned char>(Char))) {
				IsUnquoted = false;
				IsSpaced = true;
			} else {
				if(Last != '=') Tokens += Char == '=' || IsSpaced;
				else if(Char == '"' || Char == '\'') Quote = Char;
				else IsUnquoted = true;
				Last = Char;
				IsSpaced = false;
			}
		}
		// 未閉じの値を持つタグは残り全体を含む為，次の `<` から再探索せず終了
		if(End >= Size) break;
		// `>` を補われたタグの判定
		const bool IsCut = Source[End] == '<';
		Pos = End - IsCut;
		if(IsClosing) {
			// 閉じタグは対応する開いた要素迄の内側の要素を暗黙に閉じ，対応する物が無ければ読み捨てられる
			for(size_t Depth = Open.size(); Depth--;) if(SameName(Open[Depth].Name, Name)) {
				// 名前を失った要素を閉じる事の返戻
				if(Open[Depth].IsLost) return Nesting;
				// 暗黙の閉じと閉じタグの名前の字句毎に並びを戻す手間（閉じる毎に１つ減る開いた要素の数の和）
				Work += (Open.size() + Depth + 1) * (Open.size() - Depth) >> 1;
				Open.resize(Depth);
				break;
			}
		} else {
			HtmlNameBuffer Buffer, ParentBuffer;
			const std::string_view Lower = HtmlLowerName(Name, Buffer);
			// 未閉鎖要素の積重ねが開始タグの要素を含めない間，其の要素を暗黙に閉じる（名前を失った要素は何れの要素も含める）
			while(!Open.empty() && !Open.back().IsLost && !CanContain(HtmlLowerName(Open.back().Name, ParentBuffer), Lower)) {
				Work += Open.size();
				Open.pop_back();
			}
			if(NodeKind::HtmlRawTextTag.Contains(Lower)) {
				// 生の本文は閉じタグ迄を字として読む
				for(Pos = Source.find('<', End); Pos != std::string::npos; Pos = Source.find('<', Pos + 1)) {
					if(
						Pos + Lower.size() + 2 <= Size && Source[Pos + 1] == '/' &&
						SameName(std::string_view(Source.data() + Pos + 2, Lower.size()), Lower)
					) break;
				}
				if(Pos == std::string::npos) break;
				--Pos;
				++Tokens;
			} else if(
				(IsCut || Source[End - 1] != '/' || IsUnquoted) && !NodeKind::HtmlVoidTag.Contains(Lower) &&
				!NodeKind::HtmlObsoleteVoidTag.Contains(Lower)
			) {
				// 既知名は種別１バイト，其の他は長さと名前を記録（領域不足後は無名種別１バイト）
				const size_t Before = Open.empty() ? Header : Open.back().Serialized;
				const size_t Bytes = NodeKind::HtmlKnownTag.Contains(Lower) ? 1 : std::min<size_t>(Name.size(), 0XFF) + 2;
				const bool IsLost = Before + Bytes >= Limit;
				Open.push_back({ Name, Before + (IsLost ? 1 : Bytes), IsLost });
			}
		}
		// タグの字句毎に並びを戻す手間
		Work += Open.size() * Tokens;
	}
	// 追える深さと手間に収まらなければネストの理由の返戻
	return Work > Cap ? Nesting : std::string_view();
}

/**
 * コメントの並びが構文解析器の手間の上限を超えるかの判定関数
 * 計算量：原稿の長さ N に対し O(N)（行を一度だけ読み，並び毎の手間は数えるだけで読み直さない）
 * @param Source 整形対象のソース（改行の正規化前）
 * @param Language 対象言語（Python / JavaScript / TypeScript / Kotlin / PHP 以外は見送らない）
 * @return 見送る理由（見送らなければ空）
 */
std::string_view Preprocess::CommentRunSkipReason(const std::string &Source, const Lang Language) {
	// 見送る理由の文面
	static constexpr std::string_view Limit = "comments exceed parser limit";
	static constexpr size_t Budget = 0X8000000, WorkPerByteShift = 4;
	// 見送らない言語の返戻
	if(!Language.IsJsTs() && Language != Lang::Python && Language != Lang::Kotlin && Language != Lang::PHP) return {};
	// `#` の行コメントを持つか・`//` の行コメントと `/* */` の囲みコメントを持つか
	const bool HasHash = Language == Lang::Python || Language == Lang::PHP, HasSlash = Language != Lang::Python;
	// 走査対象の原文
	const std::string_view Text = Source;
	const size_t Size = Text.size(), Cap = std::max(Budget, Size << WorkPerByteShift);
	// 位置以後の最初の行の終わり（無ければ末尾）
	const auto LineEndFrom = [Text, Size](const size_t From) -> size_t {
		// 行の終わりの返戻
		return std::min(Text.find_first_of("\r\n", From), Size);
	};
	// 現在位置からの行コメント開始判定
	const auto OpensLineComment = [Text, Size, HasHash, HasSlash](const size_t At) -> bool {
		// `#` / `//` かの返戻
		return HasHash && Text[At] == '#' || HasSlash && Text[At] == '/' && At + 1 < Size && Text[At + 1] == '/';
	};
	// 手間の和と，並びのコメントの数・始まり・終わり，最初のコードの後か
	size_t Work = 0, Count = 0, RunStart = 0, RunEnd = 0;
	bool IsAfterCode = false;
	// コメント列の閉鎖と費用への加算
	const auto CloseRun = [&Work, &Count, &RunStart, &RunEnd]() -> void {
		// 遅延処理の実行
		Work += Count * (RunEnd - RunStart) >> 1;
		Count = 0;
	};
	// 多行文字列内のコメント形も閉じ行の前迄計数し，開きの誤読に依る見逃しを防止
	const auto CountInside = [&](const size_t From, const size_t To) -> void {
		// 多行文字列内のコメント形の計数
		for(size_t Head = From; Head < To;) {
			while(
				Head < To && (
					Text[Head] == ' ' || Text[Head] == '\t' || Text[Head] == '\n' || Text[Head] == '\r' || Text[Head] == '\f' || Text[Head] == '\v'
				)
			) ++Head;
			const size_t HeadEnd = LineEndFrom(Head);
			if(HeadEnd >= To) break;
			if(OpensLineComment(Head)) {
				if(!Count++) RunStart = Head;
				RunEnd = HeadEnd;
			} else CloseRun();
			Head = HeadEnd;
		}
		CloseRun();
	};
	// 原文の字句走査
	for(size_t Pos = 0; Pos < Size;) {
		const char Char = Text[Pos], Next = Pos + 1 < Size ? Text[Pos + 1] : '\0';
		if(Char == ' ' || Char == '\t' || Char == '\n' || Char == '\r' || Char == '\f' || Char == '\v') {
			++Pos;
			continue;
		}
		// コメント・Python の行継続の終端取得（非該当は０，未閉じ `/*` は行末，行継続は最初のコードの前も再読される為に計数）
		const bool IsContinuation = Language == Lang::Python && Char == '\\' && (Next == '\n' || Next == '\r');
		const bool IsBlock = HasSlash && Char == '/' && Next == '*';
		const size_t Close = IsBlock ? Text.find("*/", Pos + 2) : 0;
		if(
			const size_t End = OpensLineComment(Pos) || IsBlock && Close == std::string_view::npos ?
			LineEndFrom(Pos) :
			IsBlock ? Close + 2 : IsContinuation ? Pos + 2 : 0;
			End
		) {
			if((IsAfterCode || IsContinuation) && !Count++) RunStart = Pos;
			RunEnd = End;
			Pos = End;
			continue;
		}
		// コードの行は並びを閉じて行の終わり迄読み飛ばす
		CloseRun();
		// 手間が上限を超えた事の返戻
		if(Work > Cap) return Limit;
		IsAfterCode = true;
		size_t LineEnd = LineEndFrom(Pos);
		// 文字列・囲みコメント・正規表現内の記号を行コメントとせず，行コメント以後は読飛し
		for(size_t At = Pos; At < LineEnd;) {
			const char Here = Text[At];
			if(OpensLineComment(At)) break;
			// 囲みは閉じの行末迄を１字句扱い（未閉じの誤読は行末で切り後続の並びを保護）
			if(HasSlash && Here == '/' && At + 1 < Size && Text[At + 1] == '*') {
				const size_t Close = Text.find("*/", At + 2);
				At = Close == std::string_view::npos ? LineEnd : Close + 2;
				if(At > LineEnd) LineEnd = LineEndFrom(At);
				continue;
			}
			// JS / TS の `/` を前字句で除算と正規表現に分け，正規表現内のコメント形を保護
			if(Language.IsJsTs() && Here == '/') {
				size_t Prev = At;
				while(Prev > Pos && (Text[Prev - 1] == ' ' || Text[Prev - 1] == '\t')) --Prev;
				size_t Word = Prev;
				while(
					Word > Pos && (std::isalnum(static_cast<unsigned char>(Text[Word - 1])) || Text[Word - 1] == '_' || Text[Word - 1] == '$')
				) --Word;
				if(
					Prev == Pos || (
						Word < Prev ?
						NodeKind::JsRegexPrefixWord.Contains(Text.substr(Word, Prev - Word)) :
						Text[Prev - 1] != ')' && Text[Prev - 1] != ']' && Text[Prev - 1] != '}'
					)
				) {
					size_t Close = At + 1;
					for(bool IsInClass = false; Close < LineEnd && (IsInClass || Text[Close] != '/'); ++Close) switch(Text[Close]) {
					case '\\':
						++Close;
						break;
					case '[':
						IsInClass = true;
						break;
					case ']':
						IsInClass = false;
						break;
					}
					// 行の中で閉じなければ除算と看做す
					At = Close < LineEnd ? Close + 1 : At + 1;
					continue;
				}
			}
			if(Here != '"' && Here != '\'' && Here != '`') {
				++At;
				continue;
			}
			const char Triple[] = { Here, Here, Here };
			const bool IsTriple = (Language == Lang::Python || Language == Lang::Kotlin) && Here != '`' && !Text.compare(At, 3, Triple, 3);
			// 引用符の閉じ（逃がした字を飛ばす，三重引用符・テンプレート文字列は行を跨いで探し，其の他は行の中だけ）
			const size_t Width = IsTriple ? 3 : 1, Bound = IsTriple || Language.IsJsTs() && Here == '`' ? Size : LineEnd;
			size_t Close = At + Width;
			while(Close < Bound && (Text[Close] != Here || IsTriple && Text.compare(Close, 3, Triple, 3))) {
				Close += Text[Close] == '\\' ? 2 : 1;
			}
			At = std::min(Close + Width, Size);
			if(At <= LineEnd) continue;
			// 多行文字列内のコメント形も並びに数え，閉じの行末迄を其の行として処理
			CountInside(LineEnd, At);
			// 手間が上限を超えた事の返戻
			if(Work > Cap) return Limit;
			LineEnd = LineEndFrom(At);
		}
		// 行末の囲みコメントも次行との再読対象に数え，其の行内だけ逆走して線形化
		for(size_t Tail = LineEnd; HasSlash;) {
			while(Tail > Pos && (Text[Tail - 1] == ' ' || Text[Tail - 1] == '\t')) --Tail;
			if(Tail < Pos + 4 || Text[Tail - 1] != '/' || Text[Tail - 2] != '*') break;
			size_t Open = Tail - 2;
			while(Open > Pos + 1 && !(Text[Open - 2] == '/' && Text[Open - 1] == '*')) --Open;
			if(Open <= Pos + 1) break;
			if(!Count++) RunEnd = Tail;
			RunStart = Open - 2;
			Tail = Open - 2;
		}
		Pos = LineEnd;
	}
	CloseRun();
	// 末尾の並びを加えた手間が上限を超えるかの返戻
	return Work > Cap ? Limit : std::string_view();
}

/**
 * PHP の実行を止める `__halt_compiler()` の文かの判定関数
 * @param Src ソースコード
 * @param Stmt 判定対象の文
 * @return `__halt_compiler()` の呼出の式文なら true
 */
bool Preprocess::IsPhpHaltCall(const TSSource &Src, const TSNode Stmt) {
	// 式文の先頭呼出候補
	const TSNode Call = ts_node_named_child(Stmt, 0);
	if(
		std::string_view(ts_node_type(Stmt)) != "expression_statement" || ts_node_is_null(Call) ||
		std::string_view(ts_node_type(Call)) != "function_call_expression"
	) return false; // 関数呼出式文でない事の返戻
	// 関数名の大小無視
	static constexpr std::string_view Name = "__halt_compiler";
	const std::string_view Callee = Src.View(TSSource::FieldChild(Call, "function"));
	// 名前の一致の返戻
	return std::equal(
		Callee.begin(),
		Callee.end(),
		Name.begin(),
		Name.end(),
		[](const char Left, const char Right) -> bool {
			// 小文字へ揃えた一致の返戻
			return std::tolower(static_cast<unsigned char>(Left)) == Right;
		}
	);
}

/**
 * PHP の実行を止める `__halt_compiler()` の後に続くデータの開始位置の取得関数
 * データは `__COMPILER_HALT_OFFSET__` の位置から原文其の物として読まれる為，整形の対象から外す
 * @param Source 整形対象のソース（正規化前）
 * @return `__halt_compiler()` を終える `;` / `?>` の直後の位置（呼出が無ければ `std::string::npos`）
 */
size_t Preprocess::PhpHaltDataStart(const std::string &Source) {
	// 関数名が字面に無い場合の構文解析省略
	static constexpr std::string_view Name = "__halt_compiler";
	if(
		std::search(
			Source.begin(),
			Source.end(),
			Name.begin(),
			Name.end(),
			[](const char Left, const char Right) -> bool {
				// 小文字へ揃えた一致の返戻
				return std::tolower(static_cast<unsigned char>(Left)) == Right;
			}
		) == Source.end()
	) return std::string::npos; // halt 名の不在の返戻
	const TSSource Raw(Source, Lang::Get(Lang::PHP).TsLang());
	// 構文木を得れない場合の返戻
	if(!Raw.IsParsed()) return std::string::npos;
	size_t DataStart = std::string::npos;
	// 呼出は最も外側の文に限られる為，根の直下だけを見る
	ForEachNamedChild(
		Raw.GetRoot(),
		[&](const TSNode Stmt) -> bool {
			// 他の文を飛ばす事の返戻
			if(!IsPhpHaltCall(Raw, Stmt)) return true;
			// `;` で終えた文は其の直後，`?>` で終えた文は閉じタグの直後からデータ
			if(Raw.View(ts_node_child(Stmt, ts_node_child_count(Stmt) - 1)) == ";") DataStart = Raw.End(Stmt);
			else if(const TSNode Next = ts_node_next_named_sibling(Stmt); !ts_node_is_null(Next)) {
				if(const TSNode Close = FirstNamedChildOfType(Next, "php_end_tag"); !ts_node_is_null(Close)) DataStart = Raw.End(Close);
			}
			// 実行が止まる以降の文を見ない事の返戻
			return false;
		}
	);
	// データの開始位置の返戻
	return DataStart;
}

/**
 * 構文木の外の本文の切離関数
 * PHP の地の文は出力其の物で，構文木の外に在る空白（最初の字句の前と，地の文で終わるファイルの最後の字句の後）も出力に現れる
 * 整形の工程は構文木の範囲を組み直す為，其の空白を切り離して本体だけを整形させる
 * @param Src 整形対象（切り離した場合は本体で置き換える）
 * @param Language 対象言語
 * @param Lead 切り離した先頭の空白の格納先
 * @param Tail 切り離した末尾の空白の格納先
 * @param Body 切り離した本体の格納先（比較の基準に使う）
 * @param IsDetached 切り離したかの格納先
 * @return 末尾が出力其の物（PHP の地の文・Ruby の `__END__` 以降のデータ）で，末尾の改行も原文の数を保つなら true
 */
bool Preprocess::DetachOuterText(
	TSSource &Src,
	const Lang Language,
	std::string &Lead,
	std::string &Tail,
	std::string &Body,
	bool &IsDetached
) {
	// 切離状態の初期化
	IsDetached = false;
	// 構文木範囲の取得
	const TSNode Root = Src.GetRoot();
	const uint32_t RootCount = ts_node_named_child_count(Root);
	if(Language == Lang::Ruby) {
		// DATA が読むデータで終わるかの返戻
		return RootCount && std::string_view(ts_node_type(ts_node_named_child(Root, RootCount - 1))) == "uninterpreted";
	}
	// 地の文を持たない言語の返戻
	if(Language != Lang::PHP) return false;
	// 地の文で終わる本体は末尾空白を覆う根でなく最後の字句で閉鎖（`?>` 直後の空白も地の文，開始タグ無は全体が地の文）
	bool KeepsTail = true;
	uint32_t BodyEnd = Src.End(Root);
	if(RootCount) {
		// 最終名前付子からの後続断片の切離
		const TSNode Last = ts_node_named_child(Root, RootCount - 1);
		KeepsTail = NodeKind::PhpInlineHtml.Contains(Last) && ts_node_is_null(FirstNamedChildOfType(Last, "php_tag"));
		BodyEnd = Src.End(Last);
	}
	const uint32_t RootStart = std::min<uint32_t>(Src.Start(Root), static_cast<uint32_t>(Src.size()));
	// 末尾の改行１個は正規化で付く物と同じ為，其れを超える空白が有る時だけ切り離す
	if(
		const uint32_t RootEnd = KeepsTail ? std::max(RootStart, BodyEnd) : static_cast<uint32_t>(Src.size());
		RootStart || RootEnd + 1 < Src.size()
	) {
		// 外側本文と本体の分割
		Lead.assign(Src, 0, RootStart);
		Tail.assign(Src, RootEnd);
		Body.assign(Src, RootStart, RootEnd - RootStart);
		if(!Body.empty() && Body.back() != '\n') Body += '\n';
		Src.Assign(Body);
		IsDetached = true;
	}
	// 末尾が地の文かの返戻
	return KeepsTail;
}
