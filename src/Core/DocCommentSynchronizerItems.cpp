#include "DocCommentSynchronizer.hpp"
#include "../Util/CommentNormalizer.hpp"
#include "../Util/TextEdit.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * View 先頭空白（ASCII の空白／タブ）の破壊的削除関数
	 * @param View 走査対象の string_view（破壊的に先頭を進める）
	 */
	void DocCommentSynchronizer::SkipSpace(std::string_view &View) {
		// View からの先頭除去
		while(!View.empty() && (View.front() == ' ' || View.front() == '\t')) View.remove_prefix(1);
		// 終了
		return;
	}

	/**
	 * 括弧範囲の破壊的削除関数（View 先頭が `Open` で始まれば `Close` 直後迄を削除，Doxygen `[in]` / JSDoc `{T}` の読飛用）
	 * @param Open 開括弧文字
	 * @param Close 閉括弧文字
	 * @param View 走査対象の string_view（破壊的に先頭を進める）
	 */
	void DocCommentSynchronizer::SkipBracketed(const char Open, const char Close, std::string_view &View) {
		// 対象括弧の有無の判定
		if(!View.empty() && View.front() == Open) {
			const size_t Pos = View.find(Close);
			View.remove_prefix(Pos == std::string_view::npos ? View.size() : Pos + 1);
		}
		// 終了
		return;
	}

	/**
	 * View 先頭非空白トークン（最初の空白／タブ又は終端迄）長の取得関数
	 * @param View 走査対象の string_view
	 * @return 先頭トークンの長さ
	 */
	size_t DocCommentSynchronizer::TokenLength(const std::string_view View) {
		// 空白・タブの手前迄の先頭トークンの終わり
		size_t End = 0;
		while(End < View.size() && View[End] != ' ' && View[End] != '\t') ++End;
		// トークン長の返戻
		return End;
	}

	/**
	 * 札の語の消費関数（View を語の直後へ進める）
	 * @param Words 認める札の語（同義の語を含む）
	 * @param View 走査対象の string_view（破壊的に先頭を進める）
	 * @return 語の直後が空白・`[`・`{`・終端の語を消費したら true（別の札 `@paramXxx` 等は View 不変で false）
	 */
	bool DocCommentSynchronizer::ConsumeTagWord(const std::span<const std::string_view> Words, std::string_view &View) {
		for(const std::string_view Word : Words) {
			if(Word.empty()) continue;
			if(!View.starts_with(Word)) continue;
			const std::string_view Rest = View.substr(Word.size());
			// 語の直後が区切りで無い別の札の読飛
			if(!Rest.empty() && Rest.front() != ' ' && Rest.front() != '\t' && Rest.front() != '[' && Rest.front() != '{') continue;
			View = Rest;
			// 消費成功の返戻
			return true;
		}
		// 札で無い事の返戻
		return false;
	}

	/**
	 * 仮引数の札の見出と Doxygen `[in]` / YARD `[型]` / JSDoc `{型}` の消費関数（View を引数名先頭へ進める）
	 * @param Rules 投影済の記法
	 * @param View 走査対象の論理行（行頭に空白を持たず，破壊的に先頭を進める）
	 * @return 消費成功なら true，仮引数の札でないか別タグなら false
	 */
	bool DocCommentSynchronizer::ConsumeParamHeader(const NotationRules &Rules, std::string_view &View) {
		// 仮引数の札（Doxygen の `\param`，JSDoc の `@arg` / `@argument` を含む）でなければ消費失敗の返戻
		if(!ConsumeTagWord(VocabularyOf(DocumentationStyle::AtTag, DocDescription::Param).Aliases, View)) return false;
		// Doxygen の入出力指定 (`[in]` / `[out]` / `[in,out]`) の消費
		SkipSpace(View);
		if(const size_t Close = View.starts_with('[') ? View.find(']') : std::string_view::npos; Close != std::string_view::npos) {
			std::string Direction;
			for(const char Char : View.substr(1, Close - 1)) {
				if(Char != ' ' && Char != '\t') Direction += static_cast<char>(std::tolower(static_cast<unsigned char>(Char)));
			}
			if(
				const std::array<std::string_view, 5> &Qualifiers = VocabularyOf(DocumentationStyle::AtTag, DocDescription::Param).Qualifiers;
				std::find(Qualifiers.begin(), Qualifiers.end(), std::string_view(Direction)) != Qualifiers.end()
			) {
				View.remove_prefix(Close + 1);
				SkipSpace(View);
			}
		}
		// YARD の型の消費（其の他の言語の `[` は JSDoc の省略可能な仮引数名）
		if(Rules.HasBracketedDocumentType) {
			SkipBracketed('[', ']', View);
			SkipSpace(View);
		}
		// JSDoc の型の消費
		SkipBracketed('{', '}', View);
		SkipSpace(View);
		// 仮引数名の印か可変長・参照渡の印で始まるか
		const auto StartsParameter = [&Rules](const std::string_view Text) -> bool {
			// 仮引数名を始める印で始まるかの返戻
			return !Rules.DocumentParameterSigil.empty() && Text.starts_with(Rules.DocumentParameterSigil) || std::ranges::any_of(
				Rules.DocumentParameterMarks,
				[&Text](const std::string_view Mark) -> bool {
					// 印で始まるかの返戻
					return Text.starts_with(Mark);
				}
			);
		};
		// 裸の型を前置する文書だけ型を仮引数名の印迄読み飛ばし，他言語の説明内の同じ字は保持
		if(Rules.HasBareDocumentType && !View.empty() && !StartsParameter(View)) {
			// 型名走査ビューの準備
			std::string_view Scan = View;
			// 引数名迄の型字句の読飛
			do {
				Scan.remove_prefix(TokenLength(Scan));
				SkipSpace(Scan);
				if(Scan.empty() || StartsParameter(Scan)) break;
			} while(!Scan.empty());
			if(!Scan.empty()) View = Scan;
		}
		// 消費成功の返戻
		return true;
	}

	/**
	 * 戻値の札の見出と JSDoc `{型}` の消費関数（View を説明文又は YARD・PHPDoc の型の先頭へ進める）
	 * @param View 走査対象の論理行（行頭に空白を持たず，破壊的に先頭を進める）
	 * @return 消費成功なら true，戻値の札でないか別タグなら false
	 */
	bool DocCommentSynchronizer::ConsumeReturnHeader(std::string_view &View) {
		// 戻値の札（Doxygen の `\return` / `\returns` / `\result` を含む）でなければ消費失敗の返戻
		if(!ConsumeTagWord(VocabularyOf(DocumentationStyle::AtTag, DocDescription::Return).Aliases, View)) return false;
		SkipSpace(View);
		SkipBracketed('{', '}', View);
		SkipSpace(View);
		// 消費成功の返戻
		return true;
	}

	/**
	 * 仮引数の札の論理行の引数名取得関数
	 * @param Rules 投影済の記法
	 * @param LogicalLine マーカ除去済のドキュメント論理行
	 * @return 引数名（JSDoc の `[名=既定値]` は名前，PHP の `...` / `&` は除く，Doxygen の `x,y` は其の儘，仮引数の札でなければ空）
	 */
	std::string_view DocCommentSynchronizer::ParamTagName(const NotationRules &Rules, const std::string_view LogicalLine) {
		// 引数札の走査ビューの準備
		std::string_view View = LogicalLine;
		// 仮引数の札で始まらない行の空の返戻
		if(!ConsumeParamHeader(Rules, View)) return {};
		if(View.starts_with('[')) {
			// JSDoc の省略可能な仮引数の既定値と空白を除いた名前
			std::string_view Name = View.substr(1, View.find_first_of("=]") - 1);
			while(!Name.empty() && (Name.back() == ' ' || Name.back() == '\t')) Name.remove_suffix(1);
			SkipSpace(Name);
			// 省略可能な仮引数名の返戻
			return Name;
		}
		View = View.substr(0, TokenLength(View));
		// 参照渡・可変長の印の文法の順の除去（`&...$x` 等）
		for(const std::string_view Mark : Rules.DocumentParameterMarks) if(View.starts_with(Mark)) View.remove_prefix(Mark.size());
		// 名前トークンの返戻
		return View;
	}

	/**
	 * 戻値タグ行の判定関数
	 * @param LogicalLine マーカ除去済のドキュメント論理行
	 * @return 戻値タグ行なら true
	 */
	bool DocCommentSynchronizer::IsReturnTagLine(const std::string_view LogicalLine) {
		// 返戻札の走査ビューの準備
		std::string_view View = LogicalLine;
		// 戻値の札で始まるかの返戻
		return ConsumeReturnHeader(View);
	}

	/**
	 * 項目を指す字面の組立関数
	 * @param Dialect 文書の項目の書式
	 * @param Name 仮引数名
	 * @param IsParam 仮引数の項目か（偽は戻値の項目）
	 * @param Word 札・呼出の実際の語（`@returns` / `\param` / `Parameter` 等，空なら書式の既定の語）
	 * @return 静的検査の通知で項目を指す字面
	 */
	std::string DocCommentSynchronizer::Label(
		const DocumentationStyle Dialect,
		const std::string_view Name,
		const bool IsParam,
		const std::string_view Word
	) {
		const std::string NameText(Name), WordText(Word);
		const DocVocabularyEntry &Entry = VocabularyOf(Dialect, IsParam ? DocDescription::Param : DocDescription::Return);
		const std::string_view TagWord =
		VocabularyOf(DocumentationStyle::AtTag, IsParam ? DocDescription::Param : DocDescription::Return).Canonical;
		switch(Entry.Form) {
		case DocItemForm::XmlElement:
			// XML Doc の要素の返戻
			return IsParam ? '`' + std::string(Entry.Prefix) + NameText + "\">`" : '`' + std::string(Entry.Prefix) + '`';
		case DocItemForm::Heading:
		case DocItemForm::HeadingList:
			// 見出と箇条の返戻
			return IsParam ?
			'`' + NameText + "` in `" + std::string(VocabularyOf(Dialect, DocDescription::ParamList).Prefix) + '`' :
			'`' + std::string(Entry.Prefix) + '`';
		case DocItemForm::List:
			// `- Parameters:` の下の箇条（語が名前其の物）と呼出の返戻
			if(IsParam && Word == Name) return "`- " + NameText + '`';
			// 単独の仮引数と戻値の呼出の返戻
			return "`- " + (Word.empty() ? std::string(Entry.Canonical) : WordText) + (IsParam ? ' ' + NameText : std::string()) + '`';
		default:
			// 札の返戻
			return (Word.empty() ? std::string(TagWord) : WordText) + (IsParam ? ' ' + NameText : std::string());
		}
	}

	/**
	 * 仮引数の成員の項目の親の名前の取得関数
	 * @param ItemName 項目の仮引数名
	 * @return JSDoc の `opts.name` / `opts[].name` の `opts`（成員の項目で無ければ空）
	 */
	std::string_view DocCommentSynchronizer::ParentName(const std::string_view ItemName) {
		const size_t Cut = ItemName.find_first_of(".[");
		// 成員の区切りの前の名前の返戻
		return Cut == std::string_view::npos ? std::string_view() : ItemName.substr(0, Cut);
	}

	/**
	 * 項目が仮引数を説明するかの判定関数
	 * @param ItemName 項目の仮引数名（Doxygen の `x,y` の様にカンマで複数を並べた物を含む）
	 * @param Param 仮引数名
	 * @return 項目の名前の何れかが仮引数名と一致すれば true
	 */
	bool DocCommentSynchronizer::CoversParam(const std::string_view ItemName, const std::string_view Param) {
		for(size_t Pos = 0; Pos <= ItemName.size();) {
			const size_t Comma = std::min(ItemName.find(',', Pos), ItemName.size());
			std::string_view Part = ItemName.substr(Pos, Comma - Pos);
			SkipSpace(Part);
			while(!Part.empty() && (Part.back() == ' ' || Part.back() == '\t')) Part.remove_suffix(1);
			// 一致する名前が有る事の返戻
			if(Part == Param) return true;
			Pos = Comma + 1;
		}
		// 一致する名前の無い事の返戻
		return false;
	}

	/**
	 * 型仮引数の項目かの判定関数
	 * @param ItemName 項目の仮引数名
	 * @param Sig 実シグネチャ
	 * @return Javadoc の `<T>` か，KDoc の様に型仮引数名を仮引数の札で説明する物なら true
	 */
	bool DocCommentSynchronizer::IsTypeParamItem(const std::string_view ItemName, const Signature &Sig) {
		// 型仮引数の項目かの返戻
		return ItemName.starts_with('<') || std::find(Sig.TypeParams.begin(), Sig.TypeParams.end(), ItemName) != Sig.TypeParams.end();
	}

	/**
	 * 項目が署名の仮引数・型仮引数・仮引数の成員を説明するかの判定関数
	 * @param ItemName 項目の仮引数名
	 * @param Sig 実シグネチャ
	 * @return 何れかの仮引数・型仮引数又は仮引数の成員 (`opts.name`) を説明すれば true
	 */
	bool DocCommentSynchronizer::CoversAnyParam(const std::string_view ItemName, const Signature &Sig) {
		// 仮引数又は其の成員を説明する事の返戻
		return IsTypeParamItem(ItemName, Sig) || std::any_of(
			Sig.Params.begin(),
			Sig.Params.end(),
			[ItemName](const std::string &Param) -> bool {
				// 仮引数か其の成員の項目かの返戻
				return CoversParam(ItemName, Param) || ParentName(ItemName) == Param;
			}
		);
	}

	/**
	 * 先頭の名前の取得関数
	 * @param View 走査対象
	 * @return 先頭の英数字と `_` の名前（逆引用符で囲む名前は其の内側，無ければ空）
	 */
	std::string_view DocCommentSynchronizer::LeadingName(std::string_view View) {
		SkipSpace(View);
		if(View.starts_with('`')) {
			const size_t Close = View.find('`', 1);
			// 閉じの無い囲みは名前を持たない事の返戻
			if(Close == std::string_view::npos) return {};
			// 囲みの内側の返戻
			return View.substr(1, Close - 1);
		}
		size_t End = 0;
		while(End < View.size() && (std::isalnum(static_cast<unsigned char>(View[End])) || View[End] == '_')) ++End;
		// 名前の字句の返戻
		return View.substr(0, End);
	}

	/**
	 * XML の要素の開始札の属性値の取得関数
	 * @param Tag 開始札の字面
	 * @param Attribute 属性名
	 * @return 引用符の内側の値（属性が無ければ空）
	 */
	std::string_view DocCommentSynchronizer::XmlAttribute(const std::string_view Tag, const std::string_view Attribute) {
		// 属性名と引用された値の探索
		for(size_t Pos = Tag.find(Attribute); Pos != std::string_view::npos; Pos = Tag.find(Attribute, Pos + 1)) {
			// 別の属性名の一部（`xname` 等）の読飛（行を跨ぐ開始札の続きの行は行頭も区切り）
			if(Pos && Tag[Pos - 1] != ' ' && Tag[Pos - 1] != '\t') continue;
			std::string_view Rest = Tag.substr(Pos + Attribute.size());
			SkipSpace(Rest);
			if(!Rest.starts_with('=')) continue;
			Rest.remove_prefix(1);
			SkipSpace(Rest);
			if(Rest.empty() || Rest.front() != '"' && Rest.front() != '\'') continue;
			const size_t Close = Rest.find(Rest.front(), 1);
			// 閉じの無い値は属性値を持たない事の返戻
			if(Close == std::string_view::npos) return {};
			// 引用符の内側の返戻
			return Rest.substr(1, Close - 1);
		}
		// 属性の無い事の返戻
		return {};
	}

	/**
	 * XML の要素の深さの更新関数
	 * 開始札で深め，閉じ札で浅め，自己終了の要素と `<![CDATA[` の区画の中は数えない
	 * @param Line 論理行
	 * @param Depth 要素の深さ（破壊的に更新）
	 * @param IsInCdata `<![CDATA[` の区画の中か（破壊的に更新）
	 */
	void DocCommentSynchronizer::ScanXmlDepth(const std::string_view Line, int &Depth, bool &IsInCdata) {
		// CDATA と要素の境界に応じた深さの更新
		for(size_t Pos = 0; Pos < Line.size();) {
			if(IsInCdata) {
				const size_t End = Line.find("]]>", Pos);
				if(End == std::string_view::npos) break;
				IsInCdata = false;
				Pos = End + 3;
				continue;
			}
			if(Line[Pos] != '<') {
				++Pos;
				continue;
			}
			const std::string_view Rest = Line.substr(Pos);
			if(Rest.starts_with("<![CDATA[")) {
				IsInCdata = true;
				Pos += 9;
				continue;
			}
			// 行を跨ぐ札は数えない
			const size_t Close = Line.find('>', Pos);
			if(Close == std::string_view::npos) break;
			if(Rest.starts_with("</")) Depth = std::max(Depth - 1, 0);
			else if(Rest.size() > 1 && std::isalpha(static_cast<unsigned char>(Rest[1])) && Line[Close - 1] != '/') ++Depth;
			Pos = Close + 1;
		}
		// 終了
		return;
	}

	/**
	 * Swift の呼出の語の取得関数
	 * @param Line 論理行
	 * @param Rest 語の後の残りの格納先
	 * @return 行頭の箇条の印 (`-` / `*` / `+`) に続く語（箇条で無ければ空）
	 */
	std::string_view DocCommentSynchronizer::Callout(const std::string_view Line, std::string_view &Rest) {
		// 箇条で無い行の返戻
		if(Line.size() < 2 || Line[0] != '-' && Line[0] != '*' && Line[0] != '+' || Line[1] != ' ' && Line[1] != '\t') return {};
		std::string_view View = Line.substr(2);
		SkipSpace(View);
		size_t End = 0;
		while(End < View.size() && (std::isalnum(static_cast<unsigned char>(View[End])) || View[End] == '_')) ++End;
		Rest = View.substr(End);
		// 箇条の語の返戻
		return View.substr(0, End);
	}

	/**
	 * 仮引数の新しい項目の論理行の生成関数
	 * @param Dialect 文書の項目の書式
	 * @param Name 仮引数名
	 * @return 説明を持たない項目の論理行
	 */
	std::string DocCommentSynchronizer::NewParamLine(const DocumentationStyle Dialect, const std::string_view Name) {
		const DocVocabularyEntry &Entry = VocabularyOf(Dialect, DocDescription::Param);
		// 単独の仮引数の呼出の返戻
		if(Entry.Form == DocItemForm::List) return std::string(Entry.Prefix) + std::string(Name) + std::string(Entry.Suffix);
		const DocVocabularyEntry &Form =
		Entry.Form == DocItemForm::XmlElement ? Entry : VocabularyOf(DocumentationStyle::AtTag, DocDescription::Param);
		// 書式の語彙で囲んだ仮引数の返戻
		return std::string(Form.Prefix) + std::string(Name) + std::string(Form.Suffix);
	}

	/**
	 * 戻値の新しい項目の論理行の取得関数
	 * @param Dialect 文書の項目の書式
	 * @return 説明を持たない項目の論理行（採用する札の綴り）
	 */
	std::string_view DocCommentSynchronizer::NewReturnLine(const DocumentationStyle Dialect) {
		const std::string_view Skeleton = VocabularyOf(Dialect, DocDescription::Return).Skeleton;
		// 形式が骨格を持たない時に札形式の語彙を使った骨格の返戻
		return Skeleton.empty() ? VocabularyOf(DocumentationStyle::AtTag, DocDescription::Return).Skeleton : Skeleton;
	}

	/**
	 * 同義の札の規約の綴りへの統一関数（整形仕様 7.6）
	 * 文書形式が同義と定める札（Doxygen の `\\param` / `\\return` / `@result`，JSDoc の `@arg` / `@returns`，Javadoc・KDoc の `@exception` 等）だけを採用する札の綴りへ揃える
	 * @param Rules 投影済の記法
	 * @param Line 項目の最初の論理行（破壊的に書き換えられる）
	 */
	void DocCommentSynchronizer::NormalizeTagSynonym(const NotationRules &Rules, std::string &Line) {
		const size_t Start = std::min(Line.find_first_not_of(" \t"), Line.size());
		for(const auto &[From, To] : Rules.DocumentTagSynonyms) {
			const size_t After = Start + From.size();
			if(
				Line.compare(Start, From.size(), From) ||
				After < Line.size() && Line[After] != ' ' && Line[After] != '\t' && Line[After] != '[' && Line[After] != '{'
			) continue;
			Line.replace(Start, From.size(), To);
			// 揃えた後の終了
			return;
		}
		// 終了
		return;
	}

	/**
	 * 文書へ統合し得る独立コメントかの判定関数（整形仕様 7.4）
	 * 行コメントにすると別の記法・登録形に為る為に入力の囲みの儘とする埋込文書（整形仕様 7.6）は，文書の行へ統合しない
	 * @param Comment コメントの記録
	 * @return 区切コメント・指示コメント・行コメントで保てない記録を除く独立コメントなら true
	 */
	bool DocCommentSynchronizer::IsMergeable(const CommentRecord &Comment) {
		// 独立した説明のコメントで，区切コメントでも行コメントで保てない記録でも無いかの返戻
		return Comment.Kind == CommentKind::OwnLine && Comment.Role == CommentRole::Presentation &&
		!CommentRepository::IsSeparator(Comment) &&
		CommentRepository::OutputFormOf(Comment).Notation != CommentNotation::RubyEmbeddedDocument;
	}

	/**
	 * 新しい文書を書く行形式の記号の取得関数（整形仕様 7.4, 7.6）
	 * @param Rules 投影済の記法
	 * @return 採用する文書の記法が行形式なら其の記号（囲みの文書と文書を作らない文法は空）
	 */
	std::string_view DocCommentSynchronizer::NewDocumentMarker(const NotationRules &Rules) {
		const CommentOutputForm *const Document = Rules.AdoptedFormOf(CommentUse::Document);
		// 行形式の採用記法の記号の返戻
		return Document && !CommentRepository::IsBlockNotation(Document->Notation) ?
		CommentRepository::OpenerOf(Document->Notation) :
		std::string_view();
	}

	/**
	 * 文書化コメントブロックの判定関数（Doxygen 形式の開始記号で始まり，区切コメントを除く）
	 * @param Rules 投影済の記法（感嘆符付の囲みを文書の記法とするかが言語で変わる）
	 * @param Text コメント全文
	 * @return 文書化コメントブロックなら true
	 */
	bool DocCommentSynchronizer::IsDocBlock(const NotationRules &Rules, const std::string_view Text) {
		// 記号の形は整形仕様 3.2 の記法で決め，同じ判定を工程毎に書かない
		const CommentNotation Notation = CommentRepository::NotationOf(Rules, Text, false);
		// 文書の記法で且つ区切コメントで無い事の返戻
		return (Notation == CommentNotation::DoubleStarBlock || Notation == CommentNotation::StarBangBlock) &&
		!CommentRepository::IsSeparatorText(Rules, Text);
	}

	/**
	 * 並びの中で文書とするブロックの位置の取得関数
	 * 生成する側と検査する側が同じ物を文書と見る為，選び方を此処だけで定める
	 * @param Rules 投影済の記法
	 * @param Texts 錨の手前に続くコメントの本文（原文の順）
	 * @return 最後の文書ブロックの位置（無ければ要素数）
	 */
	size_t DocCommentSynchronizer::LastDocBlockIndex(const NotationRules &Rules, const std::span<const std::string_view> Texts) {
		size_t Found = Texts.size();
		for(size_t Idx = 0; Idx < Texts.size(); ++Idx) if(IsDocBlock(Rules, Texts[Idx])) Found = Idx;
		// 最後の文書ブロックの位置の返戻
		return Found;
	}

	/**
	 * godoc が文書の本文から除く Go の指示子の行かの判定関数（go/ast の isDirective と同じ形）
	 * `//` の直後に英小文字・数字の名前と `:` が続く行を指示子とする（`//line ` / `//extern ` / `//export ` は登録記法として先に識別する）
	 * @param Text コメント全文
	 * @return 文書の本文に数えない指示子の行なら true
	 */
	bool DocCommentSynchronizer::IsDirectiveLine(const std::string_view Text) {
		// `//` の直後の名前と `:` の走査
		const std::string_view Opener = CommentRepository::OpenerOf(CommentNotation::SlashLine);
		// 行コメントでない字面の返戻
		if(!Text.starts_with(Opener)) return false;
		size_t Pos = Opener.size();
		while(
			Pos < Text.size() &&
			(std::islower(static_cast<unsigned char>(Text[Pos])) || std::isdigit(static_cast<unsigned char>(Text[Pos])))
		) ++Pos;
		// 名前の後に `:` と英小文字・数字が続く形かの返戻
		return Pos > Opener.size() && Pos + 1 < Text.size() && Text[Pos] == ':' &&
		(std::islower(static_cast<unsigned char>(Text[Pos + 1])) || std::isdigit(static_cast<unsigned char>(Text[Pos + 1])));
	}

	/**
	 * 行形式の文書化コメントの記号の取得関数
	 * 文書の生成器が読む記号（C / C++ の Doxygen の `///` / `//!`，Java の Markdown の `///` (JDK 23)，Swift・C#・Rust の `///`，Ruby の YARD の `#`，Go の godoc の `//`）
	 * 飾りの罫線 (`////...`) と区切コメントは文書に数えない（rustdoc と同じ扱い）
	 * @param Rules 投影済の記法
	 * @param Text コメント全文
	 * @return 文書の記号（文書で無ければ空）
	 */
	std::string_view DocCommentSynchronizer::DocLineMarker(const NotationRules &Rules, const std::string_view Text) {
		// 記号の形は整形仕様 3.2 の記法で決める（飾りの罫線 `////` は一般の行コメントの記法に為る）
		const CommentNotation Notation = CommentRepository::NotationOf(Rules, Text, false);
		// 文書に使う行の記法の記号（処理系が属性として読む文法の囲む項目の文書 `//!` と godoc の指示子の形を除き，`#:` の行は YARD の `#`）
		const bool IsLineDocument = !CommentRepository::IsBlockNotation(Notation) && Rules.DocumentFormOf(Notation) &&
		!(Notation == CommentNotation::SlashBangLine && Rules.DocumentCommentIsAttribute) &&
		!(Rules.ExcludesDirectiveLines && IsDirectiveLine(Text));
		const std::string_view Marker = !IsLineDocument ?
		std::string_view() :
		CommentRepository::OpenerOf(Notation == CommentNotation::HashColonLine ? CommentNotation::HashLine : Notation);
		// 区切コメントを除く文書の記号の返戻（整形仕様 7.4）
		return Marker.empty() || CommentRepository::IsSeparatorText(Rules, Text) ? std::string_view() : Marker;
	}

	/**
	 * 文書化コメントの判定関数
	 * @param Rules 投影済の記法
	 * @param Text コメント全文
	 * @return 区切コメントを除き，囲み・行形式の文書化コメントか，godoc が文書とする Go の囲みコメントなら true
	 */
	bool DocCommentSynchronizer::IsDocComment(const NotationRules &Rules, const std::string_view Text) {
		// 囲み・行形式の文書の返戻
		if(IsDocBlock(Rules, Text) || !DocLineMarker(Rules, Text).empty()) return true;
		// 一般の囲みを文書に使う文法 (godoc) の区切でない囲みコメントかの返戻
		return Rules.DocumentFormOf(CommentNotation::Block) &&
		CommentRepository::NotationOf(Rules, Text, false) == CommentNotation::Block && !CommentRepository::IsSeparatorText(Rules, Text);
	}

	/**
	 * 通常コメントと区別する記法の独立した文書かの判定関数
	 * 通常コメントと同じ記号を文書にも使う記法（godoc の `//`，YARD の `#`）は定義の直前だけで文書に為る為，見出の内側の物を文書としない
	 * @param Rules 投影済の記法
	 * @param Comment コメントの記録
	 * @return 独立行に置く説明の文書で，囲みの文書又は通常コメントと異なる記号の行形式の文書なら true
	 */
	bool DocCommentSynchronizer::IsDedicatedDocument(const NotationRules &Rules, const CommentRecord &Comment) {
		// 独立行でないコメントと指示コメントの返戻
		if(Comment.Kind != CommentKind::OwnLine || Comment.Role == CommentRole::Directive) return false;
		const std::string Form = CommentRepository::Compose(CommentRepository::OutputFormOf(Comment));
		const std::string_view Marker = DocLineMarker(Rules, Form);
		// 囲みの文書か，通常コメントと異なる記号の行形式の文書かの返戻
		return IsDocBlock(Rules, Form) || !Marker.empty() && Marker != NewDocumentMarker(Rules);
	}

	/**
	 * 文書の記録の論理行の範囲の取得関数
	 * @param Record 本文を持つ文書の記録
	 * @return 本文の中の論理行の始まりと終わり（囲みの開始記号の後と終端記号の前の空の行を除き，中間の空の行を保つ）
	 */
	std::vector<std::pair<size_t, size_t>> DocCommentSynchronizer::LogicalLineRanges(const CommentRecord &Record) {
		std::vector<std::pair<size_t, size_t>> Ranges;
		const std::string_view Text = Record.Text;
		for(size_t Cursor = 0;;) {
			const size_t End = std::min(Text.find('\n', Cursor), Text.size());
			// 囲みの開始記号の後の空の行の読飛
			if(!CommentRepository::IsBlockNotation(Record.Notation) || !Ranges.empty() || End > Cursor) Ranges.emplace_back(Cursor, End);
			if(End == Text.size()) break;
			Cursor = End + 1;
		}
		// 囲みの終端記号の前の空の行の除去
		if(CommentRepository::IsBlockNotation(Record.Notation) && !Ranges.empty() && Ranges.back().first == Ranges.back().second) {
			Ranges.pop_back();
		}
		// 論理行の範囲の返戻
		return Ranges;
	}

	/**
	 * 文書の記録の論理行の取得関数
	 * @param Record 本文を持つ文書の記録
	 * @return 論理行（囲みの開始記号の後と終端記号の前の空の行を除き，中間の空の行を保つ）
	 */
	std::vector<std::string> DocCommentSynchronizer::LogicalLines(const CommentRecord &Record) {
		std::vector<std::string> Lines;
		for(const auto &[Start, End] : LogicalLineRanges(Record)) Lines.push_back(Record.Text.substr(Start, End - Start));
		// 論理行の返戻
		return Lines;
	}

	/**
	 * 論理行の本文開始がコード内かの判定関数（整形仕様 3.3, 7.3, 7.4）
	 * 文書の論理行を１つの記録として，正規化と同じ本文構造の判定 (`CommentNormalizer::Inspect`) で読む
	 * 統合・整合で論理行が変わる度に判定し直す
	 * @param Rules 投影済の記法
	 * @param Lines 文書の論理行列
	 * @return 各行の最初の非空白文字がコード内なら true の並び（閉じないコード区画を持つ文書は無し）
	 */
	std::optional<std::vector<bool>> DocCommentSynchronizer::CodeLineStarts(
		const NotationRules &Rules,
		const std::vector<std::string> &Lines
	) {
		CommentRecord Document { {}, { CommentPlacement::Before } };
		for(size_t Index = 0; Index < Lines.size(); ++Index) Document.Text.append(Index ? "\n" : "").append(Lines[Index]);
		Document.Format.Rules = &Rules;
		const CommentStructure Structure = CommentNormalizer::Inspect(ReadOnlyCommentView({ &Document, 1 }));
		// 閉じないコード区画を持つ文書の返戻
		if(Structure.HasUnclosedCode(0)) return std::nullopt;
		const std::span<const unsigned char> Protected = Structure.GetCodeMask(0);
		std::vector<bool> Result;
		Result.reserve(Lines.size());
		size_t Offset = 0;
		// 文書の論理行走査
		for(const std::string &Line : Lines) {
			const size_t First = Line.find_first_not_of(" \t");
			Result.push_back(First != std::string::npos && Protected[Offset + First]);
			Offset += Line.size() + 1;
		}
		// 各行のコード判定の返戻
		return Result;
	}

	/**
	 * 論理行の本文開始のコード判定に従う項目への分割関数
	 * 項目は其の続きの行を含み，並替時もコード例の帰属を保つ
	 * @param Rules 投影済の記法
	 * @param Lines 文書の論理行列（記号除去済）
	 * @param CodeStarts 各行の最初の非空白文字がコード内かの並び（閉じないコード区画を持つ文書は無し）
	 * @param Sig 実シグネチャ
	 * @return 原文の順の項目（最初は概要，同期出来ない文書は空）
	 */
	std::optional<std::vector<DocItem>> DocCommentSynchronizer::ItemsOf(
		const NotationRules &Rules,
		const std::vector<std::string> &Lines,
		const std::optional<std::vector<bool>> &CodeStarts,
		const Signature &Sig
	) {
		const DocumentationStyle Dialect = Rules.Documentation;
		const DocItemForm Form = VocabularyOf(Dialect, DocDescription::Other).Form;
		// 未閉鎖のコードへ項目を挿入すると説明ではなくコード例の内容に為る為，文書を保つ事の返戻
		if(!CodeStarts) return std::nullopt;
		const std::vector<bool> &Code = *CodeStarts;
		std::vector<DocItem> Items;
		Items.push_back({ 0, Lines.size(), DocItemKind::Summary, {} });
		// 仮引数・戻値の項目を見たか（其れより前の其の他の項目は位置を保つ）
		bool SawParamOrReturn = false;
		// 項目を始める行の登録
		const auto Start =
		[&Items, &Lines, &SawParamOrReturn](const size_t Index, const DocItemKind Kind, const std::string_view Name) -> void {
			SawParamOrReturn =
			SawParamOrReturn || Kind == DocItemKind::ParamList || Kind == DocItemKind::Param || Kind == DocItemKind::Return;
			Items.back().End = Index;
			Items.push_back({ Index, Lines.size(), Kind == DocItemKind::Other && !SawParamOrReturn ? DocItemKind::Lead : Kind, Name });
		};
		// 語の英字の大小を区別しない照合（Swift の呼出の語，継承の札）
		const auto IsWord = [](const std::string_view Word, const std::string_view Lower) -> bool {
			// 小文字の語との一致の返戻
			return std::equal(
				Word.begin(),
				Word.end(),
				Lower.begin(),
				Lower.end(),
				[](const char Left, const char Right) -> bool {
					// 小文字へ揃えた一致の返戻
					return std::tolower(static_cast<unsigned char>(Left)) == Right;
				}
			);
		};
		// 文書形式の語彙へ共通項目を対応付ける判定
		const auto IsItemWord = [&IsWord, Dialect, Form](const std::string_view Word, const DocDescription Item) -> bool {
			// 項目の同義の綴りの何れかとの一致の返戻
			return std::ranges::any_of(
				VocabularyOf(Dialect, Item).Aliases,
				[&IsWord, Form, Word](const std::string_view Alias) -> bool {
					// 綴りとの一致の返戻
					return !Alias.empty() && (Form == DocItemForm::List || Form == DocItemForm::Tag ? IsWord(Word, Alias) : Word == Alias);
				}
			);
		};
		// 空白の後に `:` が続くか（Swift の呼出の区切）
		const auto IsColonNext = [](std::string_view View) -> bool {
			SkipSpace(View);
			// `:` で始まるかの返戻
			return View.starts_with(':');
		};
		// XML の要素の深さ
		int Depth = 0;
		// XML の `<![CDATA[` の区画の中か，Rust の `# Arguments` の節の中か，Swift の `- Parameters:` の箇条の中か
		bool IsInCdata = false, IsInArguments = false, IsInParamList = false;
		// 論理行毎の文書形式に応じた項目の識別
		for(size_t Index = 0; Index < Lines.size(); ++Index) {
			// 論理行の本文（本文は字下げを持たない，整形仕様 7.6）
			const std::string_view Body = Lines[Index];
			if(Body.empty()) continue;
			if(Form == DocItemForm::XmlElement) {
				// 要素の外側の行頭の開始札だけが項目を始める（コード例の中の札も深さへ数える）
				const bool IsTopLevel = !Depth && !IsInCdata;
				ScanXmlDepth(Body, Depth, IsInCdata);
				if(!IsTopLevel || Code[Index] || Body.size() < 2 || Body[0] != '<' || !std::isalpha(static_cast<unsigned char>(Body[1]))) {
					continue;
				}
				const std::string_view Element = LeadingName(Body.substr(1));
				// 継承・外部の文書を宣言した要素は同期しない事の返戻
				if(IsItemWord(Element, DocDescription::Inheritance)) return std::nullopt;
				// 仮引数の要素の名前（開始札が行を跨ぐ場合は閉じ迄の行から探す）
				std::string_view Name;
				if(IsItemWord(Element, DocDescription::Param)) {
					const std::string_view Attribute = VocabularyOf(Dialect, DocDescription::Param).Attribute;
					for(size_t Next = Index; Name.empty() && Next < Lines.size(); ++Next) {
						const std::string_view Part = Next == Index ? Body : std::string_view(Lines[Next]);
						const size_t Close = Part.find('>');
						Name = XmlAttribute(Part.substr(0, Close), Attribute);
						if(Close != std::string_view::npos) break;
					}
				}
				if(IsItemWord(Element, DocDescription::Summary)) Start(Index, DocItemKind::Summary, {});
				else if(!Name.empty()) Start(Index, DocItemKind::Param, Name);
				else Start(Index, IsItemWord(Element, DocDescription::Return) ? DocItemKind::Return : DocItemKind::Other, {});
				continue;
			}
			if(Code[Index]) continue;
			if(Form == DocItemForm::Heading) {
				if(Body.size() > 1 && Body[0] == '#' && (Body[1] == ' ' || Body[1] == '\t')) {
					// 第１階層の見出の題
					std::string_view Title = Body.substr(1);
					SkipSpace(Title);
					while(!Title.empty() && (Title.back() == ' ' || Title.back() == '\t')) Title.remove_suffix(1);
					IsInArguments = IsItemWord(Title, DocDescription::ParamList);
					const DocItemKind Kind =
					IsInArguments ? DocItemKind::ParamList : IsItemWord(Title, DocDescription::Return) ? DocItemKind::Return : DocItemKind::Other;
					Start(Index, Kind, {});
					continue;
				}
				if(
					!IsInArguments || Body.size() < 2 || Body[0] != '*' && Body[0] != '-' && Body[0] != '+' || Body[1] != ' ' && Body[1] != '\t'
				) continue;
				// 箇条の先頭の仮引数名
				if(const std::string_view Name = LeadingName(Body.substr(2)); !Name.empty()) Start(Index, DocItemKind::Param, Name);
				continue;
			}
			// 字下げの無い箇条では署名の仮引数名を呼出の語より先に判定する
			if(Form == DocItemForm::List) {
				std::string_view Rest;
				const std::string_view Word = Callout(Body, Rest);
				if(Word.empty()) continue;
				if(const bool HasColon = IsColonNext(Rest); IsItemWord(Word, DocDescription::Param) && !HasColon) {
					// 単独の仮引数の呼出の名前
					const std::string_view Name = LeadingName(Rest);
					if(Name.empty() || !IsColonNext(Rest.substr(Rest.find(Name) + Name.size()))) continue;
					IsInParamList = false;
					Start(Index, DocItemKind::Param, Name);
				} else if(!HasColon) continue;
				else if(IsInParamList && std::ranges::find(Sig.Params, Word) != Sig.Params.end()) Start(Index, DocItemKind::Param, Word);
				else if(IsItemWord(Word, DocDescription::ParamList)) {
					IsInParamList = true;
					Start(Index, DocItemKind::ParamList, {});
				} else if(IsItemWord(Word, DocDescription::Return)) {
					IsInParamList = false;
					Start(Index, DocItemKind::Return, {});
				} else if(IsItemWord(Word, DocDescription::Failure) || IsItemWord(Word, DocDescription::Other)) {
					IsInParamList = false;
					Start(Index, DocItemKind::Other, {});
				} else if(IsInParamList) Start(Index, DocItemKind::Param, Word);
				continue;
			}
			if(Form != DocItemForm::Tag) continue;
			// 説明を継承する札の綴り
			const std::string_view Inheritance = VocabularyOf(Dialect, DocDescription::Inheritance).Aliases[0];
			for(size_t Pos = Body.find(Inheritance.front()); Pos != std::string_view::npos; Pos = Body.find(Inheritance.front(), Pos + 1)) {
				// 説明を継承する札（Javadoc の `{@inheritDoc}`，JSDoc・TSDoc・PHPDoc の `@inheritdoc`）を持つ文書は同期しない事の返戻
				if(IsWord(Body.substr(Pos + 1, Inheritance.size() - 1), Inheritance.substr(1))) return std::nullopt;
			}
			// Doxygen・Javadoc・KDoc・PHPDoc・JSDoc・YARD の札（札・指令を始める字の後に英字が続く行，Doxygen の `\` の命令を含む）
			if(
				Rules.CodeExamples.TagMarks.find(Body[0]) == std::string_view::npos || Body.size() < 2 ||
				!std::isalpha(static_cast<unsigned char>(Body[1]))
			) continue;
			const std::string_view Name = ParamTagName(Rules, Body);
			std::string_view Rest = Body;
			if(!Name.empty()) Start(Index, DocItemKind::Param, Name);
			else if(IsReturnTagLine(Body)) Start(Index, DocItemKind::Return, {});
			else {
				const bool IsSummary = !SawParamOrReturn && ConsumeTagWord(VocabularyOf(Dialect, DocDescription::Summary).Aliases, Rest);
				Start(Index, IsSummary ? DocItemKind::Summary : DocItemKind::Other, {});
			}
		}
		// 原文の順の項目の返戻
		return Items;
	}

	/**
	 * 文書の論理行の項目への分割関数（整形仕様 7.4）
	 * 論理行を１つの記録として判定したコード例に従って分ける（統合・整合で論理行が変わる度に判定し直す）
	 * @param Rules 投影済の記法
	 * @param Lines 文書の論理行列（記号除去済）
	 * @param Sig 実シグネチャ
	 * @return 原文の順の項目（最初は概要，同期出来ない文書は空）
	 */
	std::optional<std::vector<DocItem>> DocCommentSynchronizer::ParseItems(
		const NotationRules &Rules,
		const std::vector<std::string> &Lines,
		const Signature &Sig
	) {
		// コード例の判定に従う項目の返戻
		return ItemsOf(Rules, Lines, CodeLineStarts(Rules, Lines), Sig);
	}

	/**
	 * 説明の末尾の論理行の取得関数
	 * @param Rules 投影済の記法
	 * @param Lines 文書の論理行列（記号除去済）
	 * @param Sig 実シグネチャ
	 * @return 概要の次の項目の最初の論理行（項目が無ければ論理行の数）
	 */
	size_t DocCommentSynchronizer::FirstItemLine(
		const NotationRules &Rules,
		const std::vector<std::string> &Lines,
		const Signature &Sig
	) {
		const std::optional<std::vector<DocItem>> Items = ParseItems(Rules, Lines, Sig);
		// 概要の次の項目の位置の返戻
		return Items && Items->size() > 1 ? (*Items)[1].Begin : Lines.size();
	}

	/**
	 * 定義の直前の通常コメントの文書の説明への統合関数（整形仕様 7.4）
	 * 文書より前の物は説明の先頭へ，其れ以外は説明の末尾へ置き，概要を要素で表す形式では概要の要素の内容とする
	 * 置く位置は統合前の文書の構造で決め，統合した本文が札の様に読める事で順を変えない
	 * 計算量：論理行の長さの和 N に対し O(N)
	 * @param Rules 投影済の記法
	 * @param Sig 実シグネチャ
	 * @param Before 文書より前の通常コメントの論理行
	 * @param After 文書より後の通常コメントの論理行
	 * @param Lines 文書の論理行列（記号除去済，文書が無ければ空，統合した行を加える）
	 * @return 統合後の説明の末尾の論理行（見出の中のコメントを置く位置）
	 */
	size_t DocCommentSynchronizer::MergeDescription(
		const NotationRules &Rules,
		const Signature &Sig,
		const std::vector<std::string> &Before,
		const std::vector<std::string> &After,
		std::vector<std::string> &Lines
	) {
		const DocVocabularyEntry &Summary = VocabularyOf(Rules.Documentation, DocDescription::Summary);
		// 通常の概要への前後の説明の統合
		if(Summary.Form != DocItemForm::XmlElement || Before.empty() && After.empty()) {
			const size_t End = FirstItemLine(Rules, Lines, Sig);
			Lines.insert(Lines.begin() + static_cast<std::ptrdiff_t>(End), After.begin(), After.end());
			Lines.insert(Lines.begin(), Before.begin(), Before.end());
			// 統合前の説明の末尾を統合した行の数だけ進めた位置の返戻
			return End + Before.size() + After.size();
		}
		// 概要の要素の内容（開始札の後から終端札の前迄）と，置き換える論理行の範囲と終端札以降の字面
		std::vector<std::string> Content = Before;
		size_t At = 0, End = 0;
		std::string Tail(Summary.Suffix);
		const std::optional<std::vector<DocItem>> Items = ParseItems(Rules, Lines, Sig);
		for(const DocItem &Item : Items ? *Items : std::vector<DocItem>()) {
			if(Item.Kind != DocItemKind::Summary || Item.Begin == Item.End) continue;
			if(const std::string &First = Lines[Item.Begin]; !First.starts_with(Summary.Prefix)) continue;
			// 終端札の行と位置（閉じない要素は項目の最後の行迄）
			size_t Last = Item.Begin, Close = std::string::npos;
			for(size_t Index = Item.Begin; Close == std::string::npos && Index < Item.End; ++Index) {
				Close = Lines[Index].find(Summary.Suffix, Index == Item.Begin ? Summary.Prefix.size() : 0);
				Last = Index;
			}
			for(size_t Index = Item.Begin; Index <= Last; ++Index) {
				const size_t From = Index == Item.Begin ? Summary.Prefix.size() : 0;
				const size_t To = Index == Last ? std::min(Close, Lines[Index].size()) : Lines[Index].size();
				std::string Part = Lines[Index].substr(From, To - From);
				// 開始札・終端札だけの行の空の内容の読飛
				if((Index == Item.Begin || Index == Last) && Part.find_first_not_of(" \t") == std::string::npos) continue;
				Content.push_back(std::move(Part));
			}
			Tail = Close == std::string::npos ? std::string() : Lines[Last].substr(Close);
			At = Item.Begin;
			End = Last + 1;
			break;
		}
		Content.insert(Content.end(), After.begin(), After.end());
		Content.front().insert(0, Summary.Prefix);
		Content.back() += Tail;
		Lines.erase(Lines.begin() + static_cast<std::ptrdiff_t>(At), Lines.begin() + static_cast<std::ptrdiff_t>(End));
		Lines.insert(Lines.begin() + static_cast<std::ptrdiff_t>(At), Content.begin(), Content.end());
		// 統合した本文を要素の中に閉じた構造の説明の末尾の返戻
		return FirstItemLine(Rules, Lines, Sig);
	}

	/**
	 * 仮引数・戻値・失敗条件の項目の説明の有無の判定関数
	 * @param Rules 投影済の記法
	 * @param Item 仮引数・戻値・失敗条件の項目
	 * @param Lines 文書の論理行列（記号除去済）
	 * @return 札・見出・呼出の後か続きの行に説明が有れば true
	 */
	bool DocCommentSynchronizer::HasItemDescription(
		const NotationRules &Rules,
		const DocItem &Item,
		const std::vector<std::string> &Lines
	) {
		std::string_view First = Lines[Item.Begin];
		// 続きの行に本文が有るか
		const auto HasContinuation = [&Lines, &Item]() -> bool {
			for(size_t Index = Item.Begin + 1; Index < Item.End; ++Index) {
				// 本文を持つ行が有る事の返戻
				if(Lines[Index].find_first_not_of(" \t") != std::string::npos) return true;
			}
			// 続きの行に本文の無い事の返戻
			return false;
		};
		switch(VocabularyOf(Rules.Documentation, DocDescription::Other).Form) {
		case DocItemForm::XmlElement:
			{
				// 要素の内容（開始札の後から閉じ札の前迄）
				std::string Text;
				for(size_t Index = Item.Begin; Index < Item.End; ++Index) Text.append(Lines[Index]).push_back(' ');
				const size_t Open = Text.find('>');
				// 自己終了の要素と閉じの無い開始札の返戻
				if(Open == std::string::npos || Open && Text[Open - 1] == '/') return false;
				const size_t Close = Text.rfind("</"), End = Close == std::string::npos || Close < Open ? Text.size() : Close;
				// 内容の有無の返戻
				return Text.find_first_not_of(" \t", Open + 1) < End;
			}
		case DocItemForm::Heading:
			if(Item.Kind == DocItemKind::Param) {
				// 箇条の印と仮引数名の後の区切（`-` / `:` / `–` / `—`）の読飛
				std::string_view View = First.substr(1);
				SkipSpace(View);
				View.remove_prefix(LeadingName(View).size() + (View.starts_with('`') ? 2 : 0));
				for(SkipSpace(View); !View.empty() && (View.front() == '-' || View.front() == ':'); SkipSpace(View)) View.remove_prefix(1);
				if(View.starts_with("–") || View.starts_with("—")) View.remove_prefix(std::string_view("—").size());
				SkipSpace(View);
				// 同じ行の説明の返戻
				if(!View.empty()) return true;
			}
			// 続きの行の説明の返戻
			return HasContinuation();
		case DocItemForm::List:
			{
				// 呼出の `:` の後に同じ行の説明が在る場合の返戻
				if(
					const size_t Colon = First.find(':');
					Colon != std::string_view::npos && First.find_first_not_of(" \t", Colon + 1) != std::string_view::npos
				) return true;
				// 続きの行の説明の返戻
				return HasContinuation();
			}
		default:
			{
				std::string_view View = First;
				if(ConsumeTagWord(VocabularyOf(DocumentationStyle::AtTag, DocDescription::Failure).Aliases, View)) {
					// 送出例外の型（JSDoc の `{型}`，YARD の `[型]`，其の他の札の後の型名）の読飛
					SkipSpace(View);
					if(View.starts_with('{')) SkipBracketed('{', '}', View);
					else if(View.starts_with('[')) SkipBracketed('[', ']', View);
					else if(!Rules.HasOptionalFailureType) View.remove_prefix(TokenLength(View));
					SkipSpace(View);
					// 同じ行か続きの行の発生条件の説明の有無の返戻
					return !View.empty() || HasContinuation();
				}
				if(ConsumeParamHeader(Rules, View)) {
					// 名前（JSDoc の省略可能な仮引数は括弧毎）の読飛
					if(View.starts_with('[')) SkipBracketed('[', ']', View);
					else View.remove_prefix(TokenLength(View));
					SkipSpace(View);
				} else {
					View = First;
					// 戻値の札で無い行の返戻
					if(!ConsumeReturnHeader(View)) return false;
					// PHPDoc の戻値の型の読飛
					if(Rules.HasBareDocumentType) View.remove_prefix(TokenLength(View));
					SkipSpace(View);
				}
				// YARD の名前の後の型の読飛
				if(Rules.HasBracketedDocumentType) SkipBracketed('[', ']', View);
				SkipSpace(View);
				// 札の後の説明文有無の返戻
				return !View.empty();
			}
		}
	}

	/**
	 * 文書の概要の有無の判定関数
	 * @param Dialect 文書の項目の書式
	 * @param Lines 文書の論理行列（記号除去済）
	 * @param Items 文書の項目
	 * @param Name 関数名（godoc の書出しの名前は概要に数えない）
	 * @return 概要の本文（先頭の地の文，XML Doc の `<summary>`，`@brief` 等の札の後）が有れば true
	 */
	bool DocCommentSynchronizer::HasSummary(
		const DocumentationStyle Dialect,
		const std::vector<std::string> &Lines,
		const std::vector<DocItem> &Items,
		const std::string_view Name
	) {
		// 概要の項目の本文
		std::string Text;
		for(const DocItem &Item : Items) {
			if(Item.Kind != DocItemKind::Summary) continue;
			for(size_t Index = Item.Begin; Index < Item.End; ++Index) {
				std::string_view Line = Lines[Index];
				// 概要の札の語の除去
				if(Index == Item.Begin && VocabularyOf(Dialect, DocDescription::Other).Form == DocItemForm::Tag) {
					ConsumeTagWord(VocabularyOf(Dialect, DocDescription::Summary).Aliases, Line);
				}
				Text.append(Line).push_back(' ');
			}
		}
		// XML Doc の概要の要素の札の除去
		if(VocabularyOf(Dialect, DocDescription::Summary).Form == DocItemForm::XmlElement) {
			const DocVocabularyEntry &Summary = VocabularyOf(Dialect, DocDescription::Summary);
			for(const std::string_view Tag : { Summary.Prefix, Summary.Suffix }) {
				for(size_t Pos = Text.find(Tag); Pos != std::string::npos; Pos = Text.find(Tag, Pos)) Text.replace(Pos, Tag.size(), " ");
			}
		}
		std::string_view View = Text;
		SkipSpace(View);
		// godoc の書出しの名前の除去
		if(
			VocabularyOf(Dialect, DocDescription::Summary).Policy == DocStylePolicy::NameSummary && !Name.empty() &&
			View.starts_with(Name) &&
			(View.size() == Name.size() || !std::isalnum(static_cast<unsigned char>(View[Name.size()])) && View[Name.size()] != '_')
		) View.remove_prefix(Name.size());
		// 本文の有無の返戻
		return View.find_first_not_of(" \t") != std::string_view::npos;
	}

	/**
	 * 静的検査の通知で既存の項目を指す字面の取得関数
	 * @param Dialect 文書の項目の書式
	 * @param Lines 文書の論理行列（記号除去済）
	 * @param Item 仮引数又は戻値の項目
	 * @return 項目の実際の札・呼出の語を用いた字面
	 */
	std::string DocCommentSynchronizer::ItemLabel(
		const DocumentationStyle Dialect,
		const std::vector<std::string> &Lines,
		const DocItem &Item
	) {
		const std::string_view First = Lines[Item.Begin];
		std::string_view Word, Rest;
		if(const DocItemForm Form = VocabularyOf(Dialect, DocDescription::Other).Form; Form == DocItemForm::List) {
			Word = Callout(First, Rest);
		} else if(Form == DocItemForm::Tag) Word = First.substr(0, First.find_first_of(" \t[{"));
		// 実際の語を用いた字面の返戻
		return Label(Dialect, Item.Name, Item.Kind == DocItemKind::Param, Word);
	}

	/**
	 * 型の成員の並びかの判定関数（整形仕様 7.4）
	 * @param Syntax 読取専用の期待構文
	 * @param Node 節点
	 * @return 型の本体（関数の本体を兼ねる物を除く）・欄の並び・型の成員の本体・列挙子の並びなら true
	 */
	bool DocCommentSynchronizer::IsTypeMemberSequence(const SyntaxModel &Syntax, const NodeId Node) {
		// 成員を並べる型の本体かの返戻
		return Syntax.Is(Node, Role::ClassBodyContainer) && !Syntax.Is(Node, Role::DefinitionBody) ||
		Syntax.Is(Node, Role::FieldList) || Syntax.Is(Node, Role::TypeMemberBody) || Syntax.Is(Node, Role::EnumeratorSequence);
	}

	/**
	 * 型を宣言する節点かの判定関数（整形仕様 7.4 の局所の型）
	 * 型・成員の並びを持つ宣言，型の名前を宣言する宣言（別名・型定義），型の部分に成員の並びを持つ宣言と，其れ等を名前付の子に持つ宣言を型の宣言とする
	 * @param Syntax 読取専用の期待構文
	 * @param Node 節点
	 * @return 型を宣言する節点なら true
	 */
	bool DocCommentSynchronizer::DeclaresType(const SyntaxModel &Syntax, const NodeId Node) {
		// 型か成員の並びを子に持つ節点か
		const auto HasTypeBody = [&Syntax](const NodeId Part) -> bool {
			// 型か成員の並びを持つかの返戻
			return Syntax.Is(Part, Role::ClassLike) || std::ranges::any_of(
				Syntax.ChildrenOf(Part),
				[&Syntax](const NodeId Child) -> bool {
					// 成員の並びかの返戻
					return IsTypeMemberSequence(Syntax, Child);
				}
			);
		};
		// 型を宣言する宣言か
		const auto IsType = [&Syntax, &HasTypeBody](const NodeId Declaration) -> bool {
			// 型の名前・型の本体・型の部分の本体を持つ宣言かの返戻
			return Syntax.Is(Declaration, Role::DocumentableDeclaration) && (
				Syntax.Is(Syntax.FieldChild(Declaration, ChildRole::Name), Role::TypeName) ||
				Syntax.Is(Syntax.FieldChild(Declaration, ChildRole::Declarator), Role::TypeName) || HasTypeBody(Declaration) ||
				HasTypeBody(Syntax.FieldChild(Declaration, ChildRole::Type))
			);
		};
		// 自身か名前付の子が型の宣言かの返戻
		return IsType(Node) || std::ranges::any_of(
			Syntax.ChildrenOf(Node),
			[&Syntax, &IsType](const NodeId Child) -> bool {
				// 型の部分の宣言かの返戻
				return Syntax.IsNamed(Child) && IsType(Child);
			}
		);
	}

	/**
	 * 節点の所属する宣言の取得関数（整形仕様 7.4）
	 * 宣言子・仮引数・値として作る物の成員等の宣言の部分からは所属する宣言へ辿り，文・値の位置の式・並びの宣言でない項目で止める
	 * 呼出と代入は錨自身が宣言として働く物だけを採り，名前だけの節点は型の列挙項目だけを採る
	 * @param Syntax 読取専用の期待構文
	 * @param Node 節点
	 * @param IsAnchor 錨自身か（子から辿り着いた物でないか）
	 * @param Inherited 親の節点へ子から辿り着いた時の宣言
	 * @return 所属する宣言（文書の位置でなければ無し）
	 */
	NodeId DocCommentSynchronizer::DeclarationTargetOf(
		const SyntaxModel &Syntax,
		const NodeId Node,
		const bool IsAnchor,
		const NodeId Inherited
	) {
		const NodeId Parent = Syntax.ParentOf(Node);
		if(Syntax.HasField(Node, ChildRole::Value) || Syntax.Is(Node, Role::ExpressionStatement) || Syntax.Is(Node, Role::Control)) {
			// 値の位置の式と文の返戻
			return NoNode;
		}
		// 値として作る物の成員の返戻
		if(Syntax.Is(Parent, Role::ObjectValue)) return Node;
		if(Syntax.Is(Node, Role::DocumentableDeclaration)) {
			if(Syntax.Is(Node, Role::AssignmentExpression)) {
				// 代入は錨自身の定数の束縛だけが宣言と為る事の返戻
				return IsAnchor && Syntax.Is(Syntax.NamedChild(Node, 0), Role::ConstantName) ? Node : NoNode;
			}
			if(Syntax.Is(Node, Role::ReceiverCall)) {
				// 錨自身が宣言として働く呼出名かの返戻
				return IsAnchor && Syntax.IsWord(Syntax.NamedChild(Node, 0), Word::DocumentableCall) ? Node : NoNode;
			}
			if(!Syntax.NamedChildCount(Node)) {
				// 名前だけの節点は型の列挙項目かの返戻
				return Syntax.Is(Parent, Role::ClassBodyContainer) && Syntax.Is(Syntax.ParentOf(Parent), Role::ClassLike) ? Node : NoNode;
			}
			// 型・成員・最上位の宣言の返戻
			return Node;
		}
		// 並びの宣言でない項目と，宣言の部分の所属する宣言の返戻
		return Parent == NoNode || Syntax.Is(Parent, Role::DocHostSequence) ? NoNode : Inherited;
	}

	/**
	 * 節点毎の文書を同期する定義かの取得関数（整形仕様 3.2, 7.4）
	 * 直前に文書を置く名前付の関数等の文書を同期する定義を構文から決め，投影の文書化候補と同期の説明対象が同じ判定を共有する
	 * 計算量：節点数 V と子の数の和 E に対し O(V + E)
	 * @param Syntax 読取専用の構文
	 * @return 節点毎の文書を同期する定義か
	 */
	std::vector<uint8_t> DocCommentSynchronizer::DefinitionsOf(const SyntaxModel &Syntax) {
		std::vector<uint8_t> Definitions(Syntax.NodeCount(), 0);
		for(NodeId Node = 0; Node < Syntax.NodeCount(); ++Node) Definitions[Node] = SignatureExtractor::IsDocumentable(Syntax, Node);
		// 節点毎の文書を同期する定義かの返戻
		return Definitions;
	}

	/**
	 * 定義の見出を終える子かの判定関数（整形仕様 7.4）
	 * 本体と，本体の節点を持たない定義（Ruby の空の `def ... end` 等）の閉じの字句から後は見出でない
	 * 空の本体を見出と同じ行へ畳む定義の閉じの字句は，其の前の物が定義の前へ移る為に見出とする（移動で成立する統合）
	 * @param Syntax 読取専用の期待構文
	 * @param Definition 定義
	 * @param Child 定義の子
	 * @return 本体又は畳めない定義の閉じの字句なら true
	 */
	bool DocCommentSynchronizer::EndsHeader(const SyntaxModel &Syntax, const NodeId Definition, const NodeId Child) {
		// 本体か畳めない定義の閉じの字句かの返戻
		return Syntax.IsNamed(Child) && Syntax.Is(Child, Role::DefinitionBody) ||
		Syntax.Is(Child, Role::CloserToken) && Syntax.NeedsSeparatorToClose(Definition);
	}

	/**
	 * 説明対象を求める節点毎の表の構築関数（整形仕様 7.4）
	 * 子から辿り着いた時の所属する宣言，定義の本体の中か，文書化対象の定義の錨・包装と見出の中の節点毎の其の定義を一度の走査で求める
	 * 見出（前置の属性と本体の手前迄の子の部分木）の中の節点は，同期が見出の文書と通常コメントを統合する最も外の定義を説明対象とする
	 * 計算量：節点数 V と子の数の和 E に対し O(V + E)
	 * @param Syntax 読取専用の構文
	 * @return 節点毎の表
	 */
	DocCommentSynchronizer::TargetTables DocCommentSynchronizer::TablesOf(const SyntaxModel &Syntax) {
		TargetTables Tables { std::vector<NodeId>(Syntax.NodeCount(), NoNode), std::vector<uint8_t>(Syntax.NodeCount(), 0), {} };
		// 文書化対象の定義の錨と其れを先頭に持つ包装毎の其の定義と，見出の中の節点毎の最も外の定義
		std::vector<NodeId> Sites(Syntax.NodeCount(), NoNode), Headers(Syntax.NodeCount(), NoNode);
		// 節点毎の文書を同期する定義か
		const std::vector<uint8_t> IsDefinitions = DefinitionsOf(Syntax);
		for(std::vector<NodeId> Pending { Syntax.GetRoot() }; !Pending.empty();) {
			const NodeId Node = Pending.back();
			Pending.pop_back();
			const NodeId Parent = Syntax.ParentOf(Node);
			Tables.Declarations[Node] = DeclarationTargetOf(Syntax, Node, false, Parent == NoNode ? NoNode : Tables.Declarations[Parent]);
			// 局所の型と値として作る物の本体から先は成員の並びとして本体の外に戻す（整形仕様 7.4）
			// 型・名前空間・包装の宣言（TypeScript の `declare global`）の本体と同じ節点の定義の本体は，宣言を並べる場として定義の本体に数えない
			// 定義の見出の後の閉じの字句（Ruby の `def ... end` の `end`）は本体の終端境界の為，其処の錨は本体の中の物とする
			const NodeId Scope = Syntax.ParentOf(Parent);
			Tables.IsInDefinitionBody[Node] =
			Parent != NoNode && !Syntax.Is(Parent, Role::ClassLike) && !IsTypeMemberSequence(Syntax, Parent) &&
			!Syntax.Is(Parent, Role::ObjectValue) && (
				Syntax.Is(Parent, Role::DefinitionBody) && !Syntax.Is(Scope, Role::ClassLike) && !Syntax.Is(Scope, Role::NamespaceLike) &&
				!Syntax.Is(Scope, Role::DocWrapper) || Tables.IsInDefinitionBody[Parent] ||
				IsDefinitions[Parent] && Headers[Node] == NoNode && Syntax.Is(Node, Role::CloserToken)
			);
			if(IsDefinitions[Node]) {
				NodeId Up = SignatureExtractor::DocAnchor(Syntax, Node);
				Sites[Up] = Node;
				while(true) {
					const NodeId Parent = Syntax.ParentOf(Up);
					if(Parent == NoNode || Syntax.GetNode(Up).Index) break;
					Sites[Up = Parent] = Node;
				}
			}
			// 本体の手前迄の子を見出とする（見出の中の節点の子は外の定義の見出の儘）
			bool IsHeader = IsDefinitions[Node];
			for(const NodeId Child : Syntax.ChildrenOf(Node)) {
				if(IsHeader && EndsHeader(Syntax, Node, Child)) IsHeader = false;
				Headers[Child] = Headers[Node] != NoNode ? Headers[Node] : IsHeader ? Node : NoNode;
				Pending.push_back(Child);
			}
			// 定義の前置の属性（兄弟の装飾子等）も其の定義の見出とする
			for(const NodeId Child : Syntax.ChildrenOf(Node)) {
				if(!IsDefinitions[Child] || Headers[Child] != NoNode) continue;
				for(NodeId Prefix = SignatureExtractor::LeadOf(Syntax, Child); Prefix != Child; Prefix = Syntax.NextNamedSibling(Prefix)) {
					Headers[Prefix] = Child;
				}
			}
		}
		// 見出に属さない錨への定義の対応付け
		for(NodeId Node = 0; Node < Syntax.NodeCount(); ++Node) if(Headers[Node] == NoNode) Headers[Node] = Sites[Node];
		Tables.Definitions = std::move(Headers);
		// 節点毎の表の返戻
		return Tables;
	}

	/**
	 * 錨の文書の説明対象の取得関数（整形仕様 7.4）
	 * 文書を同期する定義（其の錨・包装と見出の中の節点を含む）と，定義の本体の外で型・成員・最上位の宣言に所属する錨を文書の位置とし，包装は包む宣言と同じ対象とする
	 * @param Syntax 読取専用の構文
	 * @param Tables 説明対象を求める節点毎の表
	 * @param Anchor 錨
	 * @return 説明対象の宣言（文書の位置でなければ無し）
	 */
	NodeId DocCommentSynchronizer::DocumentTargetOf(const SyntaxModel &Syntax, const TargetTables &Tables, const NodeId Anchor) {
		NodeId Target = Tables.Definitions[Anchor];
		if(Target == NoNode) {
			// 本体の中でも文書を持てる文書を同期する定義
			if(Syntax.Is(Anchor, Role::DocumentedDefinition) && SignatureExtractor::IsDocumentable(Syntax, Anchor)) Target = Anchor;
			else {
				Target = Anchor;
				// 先行する属性を錨とする場合は続く宣言迄進み，宣言でない並びの先頭と宣言を包む式文（TypeScript の名前空間）を錨とする場合は最初の項目迄進む
				while(
					Syntax.Is(Target, Role::AttachPrefix) ||
					Syntax.Is(Target, Role::DocHostSequence) && !Syntax.Is(Target, Role::DocumentableDeclaration) ||
					Syntax.Is(Target, Role::ExpressionStatement) && Syntax.Is(Syntax.NamedChild(Target, 0), Role::DocumentableDeclaration)
				) Target = Syntax.Is(Target, Role::AttachPrefix) ? Syntax.NextNamedSibling(Target) : Syntax.NamedChild(Target, 0);
				// 項目の無い錨と，定義の本体の中の局所の型でない宣言は文書の位置でない
				if(Target != NoNode && Tables.IsInDefinitionBody[Target] && !DeclaresType(Syntax, Target)) Target = NoNode;
				// 錨自身から所属する宣言
				else if(Target != NoNode) {
					const NodeId Parent = Syntax.ParentOf(Target);
					Target = DeclarationTargetOf(Syntax, Target, true, Parent == NoNode ? NoNode : Tables.Declarations[Parent]);
				}
			}
		}
		while(Target != NoNode) {
			const NodeId Parent = Syntax.ParentOf(Target);
			if(!Syntax.Is(Parent, Role::DocWrapper)) break;
			Target = Parent;
		}
		// 説明対象の返戻
		return Target;
	}

	/**
	 * 記録毎の文書の説明対象の取得関数（整形仕様 7.4）
	 * 文書と認識する記録は記法と，移譲済の錨の宣言への所属の双方から決め（配置先の行では決めない），包装は包む宣言と同じ対象とする
	 * 配置の文書の判定と，期待コメントと最終字面の双方からの文書の説明対象の保持の検証（整形仕様 8.1）が同じ規則を用いる
	 * 計算量：節点数 V，子の数の和 E，記録の数 C に対し O(V + E + C)
	 * @param Syntax 読取専用の構文
	 * @param Comments 錨の節点毎のコメント
	 * @return 記録毎の説明対象（文書と認識しない記録は無し）
	 */
	std::vector<NodeId> DocCommentSynchronizer::DocumentTargets(const SyntaxModel &Syntax, const CommentRepository &Comments) {
		// 説明対象を求める表（文書の記法の記録が在る時に初めて作る）
		std::optional<TargetTables> Tables;
		const std::span<const CommentRecord> Records = Comments.GetRecords();
		std::vector<NodeId> Targets(Records.size(), NoNode);
		for(size_t Index = 0; Index < Records.size(); ++Index) {
			const CommentRecord &Record = Records[Index];
			if(
				Record.Role != CommentRole::Presentation || Record.Placement.Side != CommentPlacement::Before ||
				CommentRepository::IsSeparator(Record) ||
				!Syntax.Rules().Notation.DocumentFormOf(CommentRepository::OutputFormOf(Record).Notation)
			) continue;
			if(!Tables) Tables.emplace(TablesOf(Syntax));
			Targets[Index] = DocumentTargetOf(Syntax, *Tables, Record.Anchor);
		}
		// 記録毎の説明対象の返戻
		return Targets;
	}

	/**
	 * 確定した配置で節点が行の最初の内容から始まるかの判定関数（整形仕様 7.6, 8.1）
	 * 出力に現れない節点は行の途中に置かれない為に行の最初とする
	 * @param Code 変更不能なコードの出力
	 * @param Node 節点
	 * @return 節点の始まりが其の行の字下げの直後なら true
	 */
	bool DocCommentSynchronizer::StartsLine(const LaidOutCode &Code, const NodeId Node) {
		const AnchorPositionMap &Anchors = Code.GetAnchors();
		const NodeSpan &Span = Anchors.Nodes[Node];
		// 出力に現れない節点か，始まりが行の字下げの直後かの返戻
		return !Span.IsPlaced || Span.Start == Code.GetFragments()[Span.StartFragment].Start + Anchors.FragmentTabs[Span.StartFragment];
	}

	/**
	 * 確定した配置で文書として置く記録毎の説明対象の取得関数（整形仕様 7.6）
	 * 独立行の文書は，説明対象（前置の属性を含む）が出力の行の最初の内容から始まる時だけ文書とし，畳んだ本体の成員等の前に独立行を持てない物は通常コメントとする
	 * 計算量：記録の数 C と前置の属性の数の和 A に対し O(C + A)
	 * @param Code 変更不能なコードの出力
	 * @param Records 記録の並び
	 * @param Targets 記録毎の文書の説明対象 (`DocumentTargets`)
	 * @return 記録毎の文書として置く説明対象（文書として置かない記録は無し）
	 */
	std::vector<NodeId> DocCommentSynchronizer::PlacedTargets(
		const LaidOutCode &Code,
		const std::span<const CommentRecord> Records,
		std::vector<NodeId> Targets
	) {
		const SyntaxModel &Syntax = *Code.GetAnchors().Syntax;
		for(size_t Index = 0; Index < Targets.size(); ++Index) if(
			Targets[Index] != NoNode && Records[Index].Kind == CommentKind::OwnLine &&
			!StartsLine(Code, SignatureExtractor::LeadOf(Syntax, Targets[Index]))
		) Targets[Index] = NoNode;
		// 記録毎の文書として置く説明対象の返戻
		return Targets;
	}

	/**
	 * 静的検査の通知で不足の項目を指す字面の取得関数
	 * @param Dialect 文書の項目の書式
	 * @param Name 仮引数名（空は戻値の項目）
	 * @return 同期が補う形の字面
	 */
	std::string DocCommentSynchronizer::MissingLabel(const DocumentationStyle Dialect, const std::string_view Name) {
		const std::string_view ReturnWord = VocabularyOf(Dialect, DocDescription::Return).Canonical;
		// 書式の既定の語を用いた字面の返戻
		return Label(
			Dialect,
			Name,
			!Name.empty(),
			VocabularyOf(Dialect, DocDescription::Other).Form == DocItemForm::Tag && Name.empty() ? ReturnWord : std::string_view()
		);
	}

	/**
	 * 失敗条件の項目の語の取得関数
	 * @param Dialect 文書の項目の書式
	 * @param Lines 文書の論理行列（記号除去済）
	 * @param Item 文書の項目
	 * @return 項目を始める札・要素・見出・呼出の語（`@throws` / `exception` / `Errors` / `Panics` / `Throws` 等，失敗条件の項目で無ければ空）
	 */
	std::string_view DocCommentSynchronizer::FailureTitle(
		const DocumentationStyle Dialect,
		const std::vector<std::string> &Lines,
		const DocItem &Item
	) {
		// 概要・仮引数・戻値の項目の返戻
		if(Item.Kind != DocItemKind::Other && Item.Kind != DocItemKind::Lead) return {};
		std::string_view First = Lines[Item.Begin];
		switch(VocabularyOf(Dialect, DocDescription::Other).Form) {
		case DocItemForm::XmlElement:
			{
				const std::string_view Open = VocabularyOf(Dialect, DocDescription::Failure).Prefix;
				// 送出例外の要素の返戻
				return First.starts_with(Open) &&
				(First.size() == Open.size() || std::string_view(" \t>/").find(First[Open.size()]) != std::string_view::npos) ?
				Open.substr(1) :
				std::string_view();
			}
		case DocItemForm::Heading:
			{
				// 第１階層の見出で無い項目の返戻
				if(First.size() < 2 || First[0] != '#' || First[1] != ' ' && First[1] != '\t') return {};
				std::string_view Title = First.substr(1);
				SkipSpace(Title);
				while(!Title.empty() && (Title.back() == ' ' || Title.back() == '\t')) Title.remove_suffix(1);
				// 失敗の見出の返戻
				for(const std::string_view Alias : VocabularyOf(Dialect, DocDescription::Failure).Aliases) {
					if(!Alias.empty() && Title == Alias) return Title;
				}
				// 失敗条件以外の見出の返戻
				return {};
			}
		case DocItemForm::List:
			{
				std::string_view Rest;
				const std::string_view Word = Callout(First, Rest);
				const std::string_view Throws = VocabularyOf(Dialect, DocDescription::Failure).Aliases[0];
				// 送出の呼出（大小文字を区別しない）の返戻
				return std::equal(
					Word.begin(),
					Word.end(),
					Throws.begin(),
					Throws.end(),
					[](const char Left, const char Right) -> bool {
						// 小文字へ揃えた一致の返戻
						return std::tolower(static_cast<unsigned char>(Left)) == Right;
					}
				) ? Word : std::string_view();
			}
		case DocItemForm::Tag:
			{
				std::string_view Rest = First;
				// 失敗条件の札の返戻
				const bool IsFailure = ConsumeTagWord(VocabularyOf(Dialect, DocDescription::Failure).Aliases, Rest);
				// 失敗条件の札又は空の返戻
				return IsFailure ? First.substr(0, First.size() - Rest.size()) : std::string_view();
			}
		default:
			// 失敗条件の項目を持たない書式の返戻
			return {};
		}
	}

	/**
	 * 静的検査の通知で失敗条件の項目を指す字面の組立関数
	 * @param Dialect 文書の項目の書式
	 * @param Title 項目の札・要素・見出・呼出の語
	 * @return 静的検査の通知で項目を指す字面
	 */
	std::string DocCommentSynchronizer::FailureLabel(const DocumentationStyle Dialect, const std::string_view Title) {
		const std::string Text(Title);
		switch(VocabularyOf(Dialect, DocDescription::Other).Form) {
		case DocItemForm::XmlElement:
			// XML Doc の要素の返戻
			return "`<" + Text + ">`";
		case DocItemForm::Heading:
			// 見出の返戻
			return "`# " + Text + '`';
		case DocItemForm::List:
			// 呼出の返戻
			return "`- " + Text + '`';
		default:
			// 札の返戻
			return Text;
		}
	}

	/**
	 * 既存ドキュメント論理行とシグネチャからの論理行整合関数（実シグネチャと照合し不足の仮引数・戻値の項目を補正）
	 * 説明を持つ項目は，対応する仮引数・戻値が無くても削除せず静的検査へ委ねる
	 * @param Rules 投影済の記法
	 * @param Existing 既存ドキュメントの論理行列（マーカ除去済）
	 * @param Sig 実シグネチャ（引数名列・戻値有無）
	 * @return 補正後の論理行列（概要・仮引数・戻値・其の他の順）
	 */
	std::vector<std::string> DocCommentSynchronizer::ReconcileLogicalLines(
		const NotationRules &Rules,
		const std::vector<std::string> &Existing,
		const Signature &Sig
	) {
		const DocumentationStyle Dialect = Rules.Documentation;
		const DocVocabularyEntry &Summary = VocabularyOf(Dialect, DocDescription::Summary);
		// 項目を持たない godoc は，文書が無ければ関数名で書き出す
		// 文書が無い場合に宣言名で書き出す本文書式の概要の返戻
		if(Summary.Policy == DocStylePolicy::NameSummary) return Existing.empty() ? std::vector<std::string>{ Sig.Name } : Existing;
		const std::optional<std::vector<DocItem>> Items = ParseItems(Rules, Existing, Sig);
		// 同期出来ない文書を保つ事の返戻
		if(!Items) return Existing;
		// 項目の札の同義の語の規約の綴りへの統一
		std::vector<std::string> Lines = Existing;
		const DocItemForm OtherForm = VocabularyOf(Dialect, DocDescription::Other).Form;
		for(const DocItem &Item : *Items) if(
			OtherForm == DocItemForm::Tag && Item.Kind != DocItemKind::Summary && Item.Kind != DocItemKind::Lead &&
			Item.Begin < Lines.size()
		) NormalizeTagSynonym(Rules, Lines[Item.Begin]);
		// 項目の分類先
		const DocItem *ParamList = nullptr;
		std::vector<const DocItem *> Params, Returns, Other;
		std::vector<std::string> Result;
		// 元の説明行と不足の項目を１度で収めれる容量を確保する
		Result.reserve(Existing.size() + Sig.Params.size() + 4);
		const auto Append = [&Result, &Lines](const DocItem &Item) -> void {
			// 既存の論理行群の転写
			Result.insert(Result.end(), Lines.begin() + Item.Begin, Lines.begin() + Item.End);
		};
		// 概要と仮引数より前の項目の転写，其の他の項目の分類
		for(const DocItem &Item : *Items) {
			if(Item.Kind == DocItemKind::Summary || Item.Kind == DocItemKind::Lead) Append(Item);
			else if(Item.Kind == DocItemKind::Param) Params.push_back(&Item);
			else if(Item.Kind == DocItemKind::Return) Returns.push_back(&Item);
			else if(Item.Kind == DocItemKind::ParamList && !ParamList) ParamList = &Item;
			else Other.push_back(&Item);
		}
		// 概要の枠（XML Doc は概要の要素）
		if(Result.empty() && Summary.Form == DocItemForm::XmlElement) Result.emplace_back(Summary.Skeleton);
		else if(Result.empty()) Result.emplace_back();
		// Swift の `- Parameters:` の箇条の単独の仮引数の呼出への書換と，本文を持たない見出の除去（本文は字下げを持たず見出の下の入れ子を表せない為，仮引数毎の呼出の形に揃える）
		if(
			const DocVocabularyEntry &ParamEntry = VocabularyOf(Dialect, DocDescription::Param);
			ParamList && ParamEntry.Form == DocItemForm::List
		) {
			for(const DocItem *const Item : Params) {
				std::string_view Rest;
				if(Callout(Lines[Item->Begin], Rest) == Item->Name) {
					Lines[Item->Begin] = std::string(ParamEntry.Prefix) + std::string(Item->Name) + std::string(Rest);
				}
			}
			std::string_view Rest;
			Callout(Lines[ParamList->Begin], Rest);
			if(ParamList->End == ParamList->Begin + 1 && Rest.find_first_not_of(" \t:") == std::string_view::npos) ParamList = nullptr;
		}
		// 仮引数の項目の並び（末尾の空行は並べた項目の後へ移し，項目の間へ残さない）
		std::vector<std::string> ParamLines;
		std::vector<bool> IsPlaced(Params.size(), false);
		size_t PendingBlank = 0;
		const auto AppendParam = [&](const size_t Idx) -> void {
			const DocItem &Item = *Params[Idx];
			ParamLines.insert(ParamLines.end(), PendingBlank, std::string());
			size_t End = Item.End;
			while(End > Item.Begin + 1 && Lines[End - 1].find_first_not_of(" \t") == std::string::npos) --End;
			ParamLines.insert(ParamLines.end(), Lines.begin() + Item.Begin, Lines.begin() + End);
			PendingBlank = Item.End - End;
			IsPlaced[Idx] = true;
		};
		// 型仮引数の項目を先に並べる
		for(size_t Idx = 0; Idx < Params.size(); ++Idx) {
			if(DocCommentSynchronizer::IsTypeParamItem(Params[Idx]->Name, Sig)) AppendParam(Idx);
		}
		// シグネチャ順の仮引数の項目（同じ仮引数の項目は全て，成員の項目は親の後）
		for(const std::string &Name : Sig.Params) {
			bool IsCovered = false;
			for(size_t Idx = 0; Idx < Params.size(); ++Idx) {
				if(!DocCommentSynchronizer::CoversParam(Params[Idx]->Name, Name)) continue;
				IsCovered = true;
				if(!IsPlaced[Idx]) AppendParam(Idx);
			}
			for(size_t Idx = 0; Idx < Params.size(); ++Idx) if(!IsPlaced[Idx] && ParentName(Params[Idx]->Name) == Name) AppendParam(Idx);
			if(!IsCovered) ParamLines.push_back(NewParamLine(Dialect, Name));
		}
		// 対応する仮引数の無い項目は，説明を持つ物と，照合が不完全な無名の仮引数を持つ署名の物だけを残す
		for(size_t Idx = 0; Idx < Params.size(); ++Idx) {
			if(!IsPlaced[Idx] && (Sig.HasUnnamedParam || HasItemDescription(Rules, *Params[Idx], Lines))) AppendParam(Idx);
		}
		ParamLines.insert(ParamLines.end(), PendingBlank, std::string());
		// 仮引数の項目の見出（項目が無ければ見出も除く）と項目
		if(ParamList && !ParamLines.empty()) Append(*ParamList);
		Result.insert(Result.end(), ParamLines.begin(), ParamLines.end());
		// 戻値がシグネチャで確定しない場合は，既存の説明を推論だけで消さない
		if(Sig.HasReturn || !Sig.ReturnsFromSignature) {
			for(const DocItem *const Item : Returns) Append(*Item);
			if(Returns.empty() && Sig.HasReturn) Result.emplace_back(NewReturnLine(Dialect));
		} else for(const DocItem *const Item : Returns) if(HasItemDescription(Rules, *Item, Lines)) Append(*Item);
		// 其の他の項目の転写
		for(const DocItem *const Item : Other) Append(*Item);
		// 補正後の論理行列の返戻
		return Result;
	}
} // namespace Core
