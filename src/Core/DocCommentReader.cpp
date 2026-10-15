#include "DocCommentReader.hpp"
#include "CommentFormat.hpp"
#include "CommentRepository.hpp"
#include "SignatureExtractor.hpp"
#include "SyntaxModel.hpp"
#include "SyntaxRole.hpp"
#include "../Util/CommentNormalizer.hpp"
#include "../Util/TextEdit.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
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
	 * godoc がドキュメントの本文から除く Go のディレクティブの行の本文かの判定関数（go/ast の isDirective と同じ形）
	 * `//` へ密着した英小文字・数字の名前と `:` が続く行をディレクティブとする（`//line ` / `//extern ` / `//export ` は登録記法として先に識別する）
	 * 計算量：ディレクティブを表すテキストの長さ L に対し O(L)
	 * @param Body `//` の直後からの本文
	 * @return ドキュメントの本文に数えないディレクティブの行の本文なら true
	 */
	bool DocCommentReader::IsDirectiveBody(const std::string_view Body) {
		// ディレクティブ名の英小文字・数字の走査
		size_t Pos = 0;
		while(
			Pos < Body.size() &&
			(std::islower(static_cast<unsigned char>(Body[Pos])) || std::isdigit(static_cast<unsigned char>(Body[Pos])))
		) ++Pos;
		// 名前の後に `:` と英小文字・数字が続く形かの返戻
		return Pos && Pos + 1 < Body.size() && Body[Pos] == ':' &&
		(std::islower(static_cast<unsigned char>(Body[Pos + 1])) || std::isdigit(static_cast<unsigned char>(Body[Pos + 1])));
	}

	/**
	 * タグの語の消費関数（View を語の直後へ進める）
	 * 計算量：候補語の長さの和 L に対し O(L)
	 * @param Rules 投影済の記法
	 * @param Words 認めるタグの語（同義の語を含む）
	 * @param View 走査対象の string_view（破壊的に先頭を進める）
	 * @return 登録した語境界でタグを消費したら true（別のタグ `@paramXxx` 等は View 不変で false）
	 */
	bool DocCommentReader::HasConsumedTagWord(
		const NotationRules &Rules,
		const std::span<const std::string_view> Words,
		std::string_view &View
	) {
		// 候補語と本文の先頭語の照合
		for(const std::string_view Word : Words) {
			const size_t Length = DocTagWordLength(Rules, View, Word);
			if(!Length) continue;
			View.remove_prefix(Length);
			// 消費成功の返戻
			return true;
		}
		// タグでない事の返戻
		return false;
	}

	/**
	 * View 先頭空白（ASCII の空白／タブ）の破壊的削除関数
	 * 計算量：取り除く先頭空白の長さ L に対し O(L)
	 * @param View 走査対象の string_view（破壊的に先頭を進める）
	 */
	void DocCommentReader::SkipSpace(std::string_view &View) {
		// View からの先頭除去
		while(!View.empty() && (View.front() == ' ' || View.front() == '\t')) View.remove_prefix(1);
		// 終了
		return;
	}

	/**
	 * 入子・引用・エスケープを含む括弧範囲の破壊的削除関数
	 * 計算量：読飛する括弧範囲のバイト数 N に対し O(N + 1)，追加メモリ O(1)
	 * @param Open 開括弧文字
	 * @param Close 閉括弧文字
	 * @param View 走査対象（対応する閉じの直後へ進め，閉じが無ければ全範囲を消費する）
	 */
	void DocCommentReader::SkipBracketed(const unsigned char Open, const unsigned char Close, std::string_view &View) {
		// 対象括弧が無い場合の終了
		if(View.empty() || View.front() != Open) return;
		size_t Depth = 1, Pos = 1;
		unsigned char Quote = 0;
		bool IsEscaped = false;
		for(; Pos < View.size() && Depth; ++Pos) {
			const unsigned char Char = View[Pos];
			if(IsEscaped) IsEscaped = false;
			else if(Char == '\\') IsEscaped = true;
			else if(Quote) {
				if(Char == Quote) Quote = 0;
			} else if(Char == '"' || Char == '\'' || Char == '`') Quote = Char;
			else if(Char == Open) ++Depth;
			else if(Char == Close) --Depth;
		}
		View.remove_prefix(Pos);
		// 終了
		return;
	}

	/**
	 * View 先頭非空白トークン（最初の空白／タブ又は終端迄）長の取得関数
	 * 計算量：先頭の語の長さ L に対し O(L)
	 * @param View 走査対象の string_view
	 * @return 先頭トークンの長さ
	 */
	size_t DocCommentReader::TokenLen(const std::string_view View) {
		// 空白・タブの手前迄の先頭トークンの終わり
		size_t End = 0;
		while(End < View.size() && View[End] != ' ' && View[End] != '\t') ++End;
		// トークン長の返戻
		return End;
	}

	/**
	 * 仮引数のタグの見出と Doxygen `[in]` / YARD `[型]` / JSDoc `{型}` の消費関数（View を引数名先頭へ進める）
	 * 計算量：見出の長さ L，候補語と修飾子の長さの和 W，括弧の読飛の総費用 Q に対し O(L W + Q)
	 * @param Rules 投影済の記法
	 * @param View 走査対象の論理行（行頭に空白を持たず，破壊的に先頭を進める）
	 * @return 消費成功なら true，仮引数のタグでないか別タグなら false
	 * @throws std::bad_alloc 入出力指定の作業文字列のメモリを確保出来ない場合
	 * @throws std::length_error 入出力指定の作業文字列の長さが上限を超える場合
	 */
	bool DocCommentReader::HasConsumedParamHeader(const NotationRules &Rules, std::string_view &View) {
		// 仮引数のタグ（Doxygen の `\param`，JSDoc の `@arg` を含む）でなければ消費失敗の返戻
		// 条件に該当する値の返戻
		if(!ConsumeDocItemTag(Rules, DocDescription::Param, View)) return false;
		// Doxygen の入出力指定 (`[in]` / `[out]` / `[in,out]`) の消費
		SkipSpace(View);
		if(
			const size_t Close = View.starts_with('[') ? View.find(']') : std::string_view::npos;
			HasDocumentForm(Rules, DocumentFormId::Doxygen) && Close != std::string_view::npos
		) {
			std::string Direction;
			for(const unsigned char Char : View.substr(1, Close - 1)) if(Char != ' ' && Char != '\t') {
				Direction += static_cast<char>(std::tolower(Char));
			}
			if(
				const std::array<std::string_view, 5> &Qualifiers = VocabularyOf(Rules.TagVocabulary, DocDescription::Param).Qualifiers;
				std::find(Qualifiers.begin(), Qualifiers.end(), std::string_view(Direction)) != Qualifiers.end()
			) {
				View.remove_prefix(Close + 1);
				SkipSpace(View);
			}
		}
		// YARD の型の消費（其の他の言語の `[` は JSDoc の省略可能な仮引数名）
		if(Rules.HasBracketedDocType) {
			SkipBracketed('[', ']', View);
			SkipSpace(View);
		}
		// JSDoc の型の消費
		if(HasDocumentForm(Rules, DocumentFormId::JSDoc)) SkipBracketed('{', '}', View);
		SkipSpace(View);
		// 仮引数名のマーカーか可変長・参照渡のマーカーで始まるか
		// 裸の型を前置するドキュメントだけ型を仮引数名のマーカー迄読み飛ばし，他言語の説明内の同じ字は保持
		if(
			const auto HasParamPrefix = [&Rules](const std::string_view Text) -> bool {
				// 仮引数名を始めるマーカーで始まるかの返戻
				return !Rules.DocParamSigil.empty() && Text.starts_with(Rules.DocParamSigil) || std::ranges::any_of(
					Rules.DocParamMarks,
					[&Text](const std::string_view Mark) -> bool {
						// マーカーで始まるかの返戻
						return Text.starts_with(Mark);
					}
				);
			}; Rules.HasBareDocType && !View.empty() && !HasParamPrefix(View)
		) {
			// 型名走査ビューの準備
			std::string_view Scan = View;
			// 引数名迄の型トークンの読飛
			do {
				Scan.remove_prefix(TokenLen(Scan));
				SkipSpace(Scan);
				if(Scan.empty() || HasParamPrefix(Scan)) break;
			} while(!Scan.empty());
			if(!Scan.empty()) View = Scan;
		}
		// 仮引数のタグと名前の前の区切の消費成功の返戻
		return true;
	}

	/**
	 * ドキュメントの区切又はバッククォートの引用に従う先頭の名前の取得関数
	 * 計算量：調べる先頭部分の長さ L に対し O(L)
	 * @param View 走査対象
	 * @param Consumed 先頭空白と引用の閉じを含む消費範囲の格納先（不要なら nullptr）
	 * @return Unicode の名前又は引用の内側（閉じの無い引用は空）
	 */
	std::string_view DocCommentReader::GetLeadingName(std::string_view View, size_t *const Consumed) {
		const size_t Size = View.size();
		SkipSpace(View);
		size_t Begin = 0, End = 0;
		if(View.starts_with('`')) {
			const size_t Close = View.find('`', 1);
			if(Consumed) *Consumed = Size - View.size() + (Close == std::string_view::npos ? View.size() : Close + 1);
			// 閉じの無い引用は名前を持たない事の返戻
			if(Close == std::string_view::npos) return {};
			Begin = 1;
			End = Close;
		} else {
			// 名前の文字種でなく，説明項目と要素の区切で範囲を確定
			End = std::min(View.find_first_of("\t\n\r /:=>"), View.size());
			if(Consumed) *Consumed = Size - View.size() + End;
		}
		// 引用を除いた名前の範囲の返戻
		return View.substr(Begin, End - Begin);
	}

	/**
	 * Swift の説明項目のラベルの取得関数
	 * 計算量：説明項目の先頭部分の長さ L に対し O(L)
	 * @param Line 論理行
	 * @param Rest 語の後の残りの格納先
	 * @return 行頭の箇条のマーカー (`-` / `*` / `+`) に続く語（箇条でなければ空）
	 */
	std::string_view DocCommentReader::Callout(const std::string_view Line, std::string_view &Rest) {
		// 箇条でない行の返戻
		if(Line.size() < 2 || Line[0] != '-' && Line[0] != '*' && Line[0] != '+' || Line[1] != ' ' && Line[1] != '\t') return {};
		std::string_view View = Line.substr(2);
		SkipSpace(View);
		size_t Consumed = 0;
		const std::string_view Name = GetLeadingName(View, &Consumed);
		Rest = View.substr(Consumed);
		// 箇条の語の返戻
		return Name;
	}

	/**
	 * 仮引数のタグの論理行の引数名取得関数
	 * 計算量：見出の長さ L，仮引数記号の長さの和 M，仮引数見出の判定費用 Q に対し O(L + M + Q)
	 * @param Rules 投影済の記法
	 * @param LogicalLine 記号除去済のドキュメント論理行
	 * @return 引数名（JSDoc の `[名=既定値]` は名前，PHP の `...` / `&` は除く，Doxygen の `x,y` は其の儘，仮引数のタグでなければ空）
	 * @throws std::bad_alloc 入出力指定の作業文字列のメモリを確保出来ない場合
	 * @throws std::length_error 入出力指定の作業文字列の長さが上限を超える場合
	 */
	std::string_view DocCommentReader::GetParamTagName(const NotationRules &Rules, const std::string_view LogicalLine) {
		// 引数タグの走査ビューの準備
		std::string_view View = LogicalLine;
		// 仮引数のタグで始まらない行の空の返戻
		if(!HasConsumedParamHeader(Rules, View)) return {};
		if(HasDocumentForm(Rules, DocumentFormId::JSDoc) && View.starts_with('[')) {
			// JSDoc の省略可能な仮引数の既定値と空白を除いた名前
			std::string_view Name = View.substr(1, View.find_first_of("=]") - 1);
			while(!Name.empty() && (Name.back() == ' ' || Name.back() == '\t')) Name.remove_suffix(1);
			SkipSpace(Name);
			// 省略可能な仮引数名の返戻
			return Name.starts_with('`') ? GetLeadingName(Name) : Name;
		}
		// 参照渡・可変長のマーカーの文法の順の除去（`&...$x` 等）
		for(const std::string_view Mark : Rules.DocParamMarks) if(View.starts_with(Mark)) View.remove_prefix(Mark.size());
		// 名前トークンの返戻
		return View.starts_with('`') ?
		GetLeadingName(View) :
		View.substr(0, std::min(TokenLen(View), View.find_first_of(Rules.DocParamNameTerminators)));
	}

	/**
	 * 戻値のタグの見出と JSDoc `{型}` の消費関数（View を説明文又は YARD ･ PHPDoc の型の先頭へ進める）
	 * @param Rules 投影済の記法
	 * @param View 走査対象の論理行（行頭に空白を持たず，破壊的に先頭を進める）
	 * @return 消費成功なら true，戻値のタグでないか別タグなら false
	 */
	bool DocCommentReader::HasConsumedReturnHeader(const NotationRules &Rules, std::string_view &View) {
		// 戻値のタグ（Doxygen の `\return` / `\returns` / `\result` を含む）でなければ消費失敗の返戻
		if(!ConsumeDocItemTag(Rules, DocDescription::Return, View)) return false;
		SkipSpace(View);
		if(HasDocumentForm(Rules, DocumentFormId::JSDoc)) SkipBracketed('{', '}', View);
		SkipSpace(View);
		// 戻値のタグと型の囲みの消費成功の返戻
		return true;
	}

	/**
	 * 項目の行内テキストの取得関数
	 * 計算量：O(1)
	 * @param Item 同じ本文世代の行内範囲を持つ項目
	 * @param Lines ドキュメントの論理行列
	 * @param Idx 項目に属する論理行の位置
	 * 制約：Item.Begin ≤ Idx < Item.End，行内位置は該当行のバイト範囲内
	 * @return 項目の開始・終端で区切った行のテキスト
	 */
	std::string_view DocCommentReader::GetLineOf(const DocItem &Item, const std::vector<std::string> &Lines, const size_t Idx) {
		// 項目の最初と最後の行内の範囲
		const size_t Begin = Idx == Item.Begin ? Item.BeginOffset : 0;
		// 行内のテキストの返戻
		return std::string_view(
			Lines[Idx].data() + Begin,
			std::min(Idx + 1 == Item.End ? Item.EndOffset : std::string_view::npos, Lines[Idx].size()) - Begin
		);
	}

	/**
	 * 論理行の本文開始のコード判定に従う項目への分割関数
	 * 項目は其の続きの行を含み，並替時もコード例の帰属を保つ
	 * 計算量：論理行数 N，本文の長さの和 L，シグネチャの仮引数名の長さの和 P，語彙の照合・コード領域の判定の総費用 Q に対し期待 O(N + L + P + Q)
	 * @param Rules 投影済の記法
	 * @param Lines ドキュメントの論理行列（記号除去済）
	 * @param FunctionSig 実シグネチャ
	 * @param CodeStarts 項目の解析から除くコード行の並び（閉じないコード領域を持つドキュメントは無い）
	 * @param GroupStarts 原文のドキュメント毎の最初の論理行（空なら１つのドキュメント）
	 * @param ParamIdx 同じシグネチャの本文世代と静的解析で共有する名前のインデックス（初回の照合時に構築）
	 * 制約：ParamIdx が参照する FunctionSig.Params のテキストは，インデックスの使用中は生存し，変更しない
	 * 制約：返戻する Name は Lines のテキストを参照する為，利用中は Lines を生存させ，テキストを変更しない
	 * 制約：CodeStarts を持つ時は CodeStarts->Code.size() = Lines.size()
	 * @return 原文の順の項目（最初は概要，同期出来ないドキュメントは空）
	 * @throws std::bad_alloc 項目又はインデックスのメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	std::optional<std::vector<DocItem>> DocCommentReader::ItemsOf(
		const NotationRules &Rules,
		const std::vector<std::string> &Lines,
		const Signature &FunctionSig,
		const std::optional<DocReading> &CodeStarts,
		const std::span<const size_t> GroupStarts,
		DocParamIdx &ParamIdx
	) {
		// ドキュメントの項目の書式
		const DocItemForm Form = VocabularyOf(Rules.Vocabulary, DocDescription::Other).Form;
		// 未閉鎖のコードへ項目を挿入すると説明ではなくコード例の内容に為る為，ドキュメントを保つ事の返戻
		if(!CodeStarts) return std::nullopt;
		// 内側のシグネチャを持つドキュメント形式（未知のタグを持つ其の他の形式へ適用しない）
		const bool HasNestedSigs = Form == DocItemForm::Tag && std::ranges::any_of(
			Rules.DocumentForms,
			[](const DocumentNotation &Entry) -> bool {
				// 内側のシグネチャを持つドキュメント形式の判定
				switch(Entry.Form) {
				case DocumentFormId::JSDoc:
				case DocumentFormId::Yard:
					// 内側のシグネチャを持つ形式である事の返戻
					return true;
				default:
					// 其の他の形式である事の返戻
					return false;
				}
			}
		);
		// コードの範囲と収集する項目
		const std::vector<bool> &Code = CodeStarts->Code;
		std::vector<DocItem> Items;
		Items.push_back({ 0, Lines.size(), DocItemKind::Summary, {} });
		// 仮引数・戻値の項目を見たか（其れより前の其の他の項目は位置を保つ）
		bool HasParamOrReturn = false;
		// 項目を始める行の登録
		const auto Start =
		[&Items, &Lines, &HasParamOrReturn](const size_t Idx, const DocItemKind Kind, const std::string_view Name, const size_t Offset)
		-> void {
			// 仮引数・戻値の項目の有無の記録
			HasParamOrReturn =
			HasParamOrReturn || Kind == DocItemKind::ParamList || Kind == DocItemKind::Param || Kind == DocItemKind::Return ||
			Kind == DocItemKind::TypeParam;
			Items.back().End = Idx + (Offset ? 1 : 0);
			Items.back().EndOffset = Offset ? Offset : std::string_view::npos;
			Items.push_back({ Idx, Lines.size(), Kind == DocItemKind::Other && !HasParamOrReturn ? DocItemKind::Lead : Kind, Name });
			Items.back().BeginOffset = Offset;
			// 終了
			return;
		};
		// 語の英字の大小を区別しない照合（Swift の項目ラベル，継承のタグ）
		const auto IsWord = [](const std::string_view Word, const std::string_view Lower) -> bool {
			// 小文字の語との一致の返戻
			return std::equal(
				Word.begin(),
				Word.end(),
				Lower.begin(),
				Lower.end(),
				[](const unsigned char Left, const unsigned char Right) -> bool {
					// 小文字へ揃えた一致の返戻
					return std::tolower(Left) == Right;
				}
			);
		};
		// ドキュメント形式の語彙へ共通項目を対応付ける判定
		const auto IsItemWord = [&Rules, &IsWord, Form](const std::string_view Word, const DocDescription Item) -> bool {
			// 項目の同義の綴りの何れかとの一致の返戻
			return std::ranges::any_of(
				VocabularyOf(Rules.Vocabulary, Item).Aliases,
				[&IsWord, Form, Word](const std::string_view Alias) -> bool {
					// 綴りとの一致の返戻
					return !Alias.empty() && (Form == DocItemForm::List || Form == DocItemForm::Tag ? IsWord(Word, Alias) : Word == Alias);
				}
			);
		};
		// 空白の後に `:` が続くか（Swift の項目ラベルの区切）
		const auto IsColonNext = [](std::string_view View) -> bool {
			// 本文の先頭空白の読飛
			SkipSpace(View);
			// `:` で始まるかの返戻
			return View.starts_with(':');
		};
		enum class AttributePhase : uint8_t { Name, Equals, Val }; // 属性名・等号・引用された値を待つ段
		// XML のタグ・属性・要素の状態を論理行間で引き継ぐ
		AttributePhase Phase = AttributePhase::Name;
		size_t Depth = 0, TagLine = 0, TagPos = 0, ValLine = 0, ValStart = 0;
		std::string_view Element, ParamName;
		unsigned char Quote = 0, LastTagChar = 0;
		bool IsInTag = false, IsClosingTag = false, IsTopLevelTag = false, IsNameAttribute = false, IsInCdata = false;
		bool IsInXmlComment = false, IsInArgs = false, IsInParamList = false, IsInNestedSig = false, IsReferenceAttribute = false;
		bool HasReference = false;
		// Swift の仮引数の箇条の照合と，本文変更後の照合・静的解析が同じシグネチャのインデックスを読む
		const auto IsParamName = [&FunctionSig, &ParamIdx](const std::string_view Name) -> bool {
			// シグネチャの仮引数名のインデックスの構築
			if(!ParamIdx) ParamIdx.emplace(FunctionSig.Params.begin(), FunctionSig.Params.end());
			// シグネチャの仮引数名の有無の返戻
			return ParamIdx->contains(Name);
		};
		// 次の元のドキュメントの開始位置
		size_t Group = !GroupStarts.empty() && !GroupStarts.front() ? 1 : 0, InlineDepth = 0;
		// 論理行毎のドキュメント形式に応じた項目の識別
		for(size_t Idx = 0; Idx < Lines.size(); ++Idx) {
			// 別のドキュメントの概要・箇条・内側のシグネチャを，前の項目の説明へ取り込まない
			if(Group < GroupStarts.size() && GroupStarts[Group] == Idx) {
				// 前のドキュメントの未閉鎖の XML を，次のドキュメントで閉じない事の返戻
				if(IsInTag || Quote || IsInCdata || IsInXmlComment || Depth) return std::nullopt;
				Items.back().End = Idx;
				Items.push_back({ Idx, Lines.size(), DocItemKind::Summary, {} });
				HasParamOrReturn = IsInArgs = IsInParamList = IsInNestedSig = false;
				InlineDepth = 0;
				++Group;
			}
			// 論理行の本文（本文はインデントを持たない，整形仕様 7.6）
			const std::string_view Body = Lines[Idx];
			if(Body.empty()) continue;
			if(Form == DocItemForm::XmlElement) {
				// コード例の行は XML の項目として読まず，前の行から続くタグ・属性・CDATA の解析だけを継ぐ
				std::string_view CodeBody = Body;
				SkipSpace(CodeBody);
				if(
					Code[Idx] && !IsInTag && !IsInCdata && !IsInXmlComment && !CodeBody.starts_with("<!--") && !CodeBody.starts_with("<![CDATA[")
				) {
					if(Depth && !Items.back().HasXmlDescription && Body.find_first_not_of(" \t\r") != std::string_view::npos) {
						Items.back().HasXmlDescription = true;
					}
					continue;
				}
				for(size_t Pos = 0; Pos < Body.size();) {
					if(IsInCdata || IsInXmlComment) {
						const std::string_view ClosingMarker = IsInCdata ? "]]>" : "-->";
						const size_t Close = Body.find(ClosingMarker, Pos);
						// CDATA の内容だけを説明に数え，XML コメントと空白だけの内容は数えない
						if(
							IsInCdata && (Depth || Items.back().OpenLine == std::string_view::npos) &&
							Body.substr(Pos, std::min(Close, Body.size()) - Pos).find_first_not_of(" \t\r") != std::string_view::npos
						) Items.back().HasXmlDescription = true;
						if(Close == std::string_view::npos) break;
						IsInCdata = false;
						IsInXmlComment = false;
						Pos = Close + ClosingMarker.size();
						continue;
					}
					if(!IsInTag) {
						if(Body[Pos] != '<') {
							if(
								!Items.back().HasXmlDescription && (Depth || Items.back().OpenLine == std::string_view::npos) && Body[Pos] != ' ' &&
								Body[Pos] != '\t' && Body[Pos] != '\r'
							) Items.back().HasXmlDescription = true;
							++Pos;
							continue;
						}
						const std::string_view Rest = Body.substr(Pos);
						if(Rest.starts_with("<![CDATA[")) {
							IsInCdata = true;
							Pos += std::string_view("<![CDATA[").size();
							continue;
						}
						if(Rest.starts_with("<!--")) {
							IsInXmlComment = true;
							Pos += std::string_view("<!--").size();
							continue;
						}
						IsClosingTag = Rest.starts_with("</");
						Element = GetLeadingName(Rest.substr(IsClosingTag ? 2 : 1));
						IsTopLevelTag = !Depth && !IsClosingTag && !Code[Idx] && !Element.empty();
						TagLine = Idx;
						TagPos = Pos;
						ParamName = {};
						Phase = AttributePhase::Name;
						LastTagChar = 0;
						IsNameAttribute = false;
						IsReferenceAttribute = HasReference = false;
						IsInTag = true;
						Pos += Element.size() + (IsClosingTag ? 2 : 1);
						continue;
					}
					const unsigned char Char = Body[Pos];
					if(Quote) {
						// 参照先を持つ要素は，説明文を持たない自己終了形式でも説明として扱う
						if(IsReferenceAttribute && Char != Quote && Char != ' ' && Char != '\t' && Char != '\r') HasReference = true;
						if(Char == Quote) {
							if(IsNameAttribute) {
								// 行を跨いだ対象名はシグネチャと対応付けれない為，同期しない事の返戻
								if(ValLine != Idx) return std::nullopt;
								ParamName = Body.substr(ValStart, Pos - ValStart);
							}
							Quote = 0;
							Phase = AttributePhase::Name;
						}
						++Pos;
						continue;
					}
					// 未完了のタグに別のタグが現れたドキュメントは，残りを探し直さず保持する事の返戻
					if(Char == '<') return std::nullopt;
					if(Char == '>') {
						if(Depth && !IsClosingTag && HasReference) Items.back().HasXmlDescription = true;
						if(IsClosingTag) {
							// 対応しない閉じを持つドキュメントは同期しない事の返戻
							if(!Depth) return std::nullopt;
							if(!--Depth) {
								Items.back().CloseLine = TagLine;
								Items.back().CloseOffset = TagPos;
							}
						} else if(!Element.empty() && LastTagChar != '/') ++Depth;
						if(IsTopLevelTag) {
							// 外部のドキュメントを宣言した要素は同期しない事の返戻
							if(IsItemWord(Element, DocDescription::Inheritance)) return std::nullopt;
							if(IsItemWord(Element, DocDescription::Summary)) Start(TagLine, DocItemKind::Summary, {}, TagPos);
							else if(IsItemWord(Element, DocDescription::Param) && !ParamName.empty()) Start(TagLine, DocItemKind::Param, ParamName, TagPos);
							else if(IsItemWord(Element, DocDescription::TypeParam) && !ParamName.empty()) {
								Start(TagLine, DocItemKind::TypeParam, ParamName, TagPos);
							} else if(IsItemWord(Element, DocDescription::Return)) Start(TagLine, DocItemKind::Return, {}, TagPos);
							else Start(TagLine, DocItemKind::Other, {}, TagPos);
							Items.back().OpenLine = Idx;
							Items.back().OpenOffset = Pos;
							Items.back().IsSelfClosing = LastTagChar == '/';
						}
						IsInTag = false;
						++Pos;
						continue;
					}
					if(Char == ' ' || Char == '\t' || Char == '\r') {
						++Pos;
						continue;
					}
					LastTagChar = Char;
					// 属性の読取段階毎の消費
					switch(Phase) {
					case AttributePhase::Name:
						{
							if(!std::isalpha(Char) && Char != '_') break;
							const size_t Begin = Pos;
							while(
								Pos < Body.size() && (
									std::isalnum(static_cast<unsigned char>(Body[Pos])) || Body[Pos] == '_' || Body[Pos] == ':' || Body[Pos] == '-' ||
									Body[Pos] == '.'
								)
							) ++Pos;
							const std::string_view Attribute = Body.substr(Begin, Pos - Begin);
							IsNameAttribute = Attribute == VocabularyOf(
								Rules.Vocabulary,
								IsItemWord(Element, DocDescription::TypeParam) ? DocDescription::TypeParam : DocDescription::Param
							).Attribute;
							IsReferenceAttribute =
							std::ranges::find(Rules.DocReferenceAttributes, std::pair(Element, Attribute)) != Rules.DocReferenceAttributes.end();
							Phase = AttributePhase::Equals;
							continue;
						}
					case AttributePhase::Equals:
						if(Char != '=') break;
						Phase = AttributePhase::Val;
						++Pos;
						continue;
					}
					if(Char == '\"' || Char == '\'') {
						Quote = Char;
						ValStart = Pos + 1;
						ValLine = Idx;
						IsNameAttribute = IsNameAttribute && Phase == AttributePhase::Val;
						IsReferenceAttribute = IsReferenceAttribute && Phase == AttributePhase::Val;
					} else Phase = AttributePhase::Name;
					++Pos;
				}
				continue;
			}
			if(Code[Idx]) continue;
			if(Form == DocItemForm::Heading) {
				if(Body.size() > 1 && Body[0] == '#' && (Body[1] == ' ' || Body[1] == '\t')) {
					// 第１階層の見出の題
					std::string_view Title = Body.substr(1);
					SkipSpace(Title);
					while(!Title.empty() && (Title.back() == ' ' || Title.back() == '\t')) Title.remove_suffix(1);
					// 採用したドキュメント項目だけの識別（其の他の見出風の本文は説明文）
					const bool IsArgs = IsItemWord(Title, DocDescription::ParamList), IsReturn = IsItemWord(Title, DocDescription::Return);
					if(IsArgs || IsReturn || IsItemWord(Title, DocDescription::Failure)) {
						IsInArgs = IsArgs;
						Start(Idx, IsArgs ? DocItemKind::ParamList : IsReturn ? DocItemKind::Return : DocItemKind::Other, {}, 0);
						continue;
					}
				}
				if(!IsInArgs || Body.size() < 2 || Body[0] != '*' && Body[0] != '-' && Body[0] != '+' || Body[1] != ' ' && Body[1] != '\t') {
					continue;
				}
				// 箇条の先頭の仮引数名
				if(const std::string_view Name = GetLeadingName(Body.substr(2)); !Name.empty()) Start(Idx, DocItemKind::Param, Name, 0);
				continue;
			}
			// インデントの無い箇条の，項目ラベルより先のシグネチャの仮引数名の照合
			if(Form == DocItemForm::List) {
				// 項目ラベルと後続の本文
				std::string_view Rest;
				const std::string_view Word = Callout(Body, Rest);
				if(Word.empty()) continue;
				if(const bool HasColon = IsColonNext(Rest); IsItemWord(Word, DocDescription::Param) && !HasColon) {
					// 単独の仮引数の説明項目が指定する仮引数名
					size_t Consumed = 0;
					const std::string_view Name = GetLeadingName(Rest, &Consumed);
					if(Name.empty() || !IsColonNext(Rest.substr(Consumed))) continue;
					IsInParamList = false;
					Start(Idx, DocItemKind::Param, Name, 0);
				} else if(!HasColon) continue;
				else if(IsInParamList && IsParamName(Word)) Start(Idx, DocItemKind::Param, Word, 0);
				else if(IsItemWord(Word, DocDescription::ParamList)) {
					IsInParamList = true;
					Start(Idx, DocItemKind::ParamList, {}, 0);
				} else if(IsItemWord(Word, DocDescription::Return)) {
					IsInParamList = false;
					Start(Idx, DocItemKind::Return, {}, 0);
				} else if(IsItemWord(Word, DocDescription::Failure) || IsItemWord(Word, DocDescription::Other)) {
					IsInParamList = false;
					Start(Idx, DocItemKind::Other, {}, 0);
				} else if(IsInParamList) Start(Idx, DocItemKind::Param, Word, 0);
				continue;
			}
			if(Form != DocItemForm::Tag) continue;
			// 外側のシグネチャと照合しない内側のシグネチャの開始と項目の，原文順を保った分類
			if(
				std::string_view Nested = Body;
				HasNestedSigs && HasConsumedTagWord(Rules, VocabularyOf(Rules.Vocabulary, DocDescription::NestedSig).Aliases, Nested)
			) {
				Start(Idx, DocItemKind::NestedSig, {}, 0);
				IsInNestedSig = true;
				continue;
			}
			// コード例を除いた行頭のタグ（内外の項目と同義の綴りの統一で共有）
			const bool IsTag = Rules.CodeExamples.TagMarks.find(Body[0]) != std::string_view::npos && Body.size() > 1 &&
			std::isalpha(static_cast<unsigned char>(Body[1]));
			if(IsInNestedSig) {
				if(IsTag) Start(Idx, DocItemKind::NestedSig, {}, 0);
				continue;
			}
			// 説明を継承するタグの綴り
			const std::string_view Inheritance = VocabularyOf(Rules.Vocabulary, DocDescription::Inheritance).Aliases[0];
			const std::vector<unsigned char> &Protected = CodeStarts->Protected[Idx];
			// 保護範囲と別のインラインタグの中身を除き，タグの境界を照合する
			for(size_t Pos = 0; Pos < Body.size(); ++Pos) {
				if(Protected[Pos]) continue;
				const size_t End = Pos + Inheritance.size();
				const bool IsInline = InlineDepth == 1 && Pos && Body[Pos - 1] == '{';
				// 説明を継承するドキュメントを同期から除く事の返戻
				if(
					Body[Pos] == Inheritance.front() && End <= Body.size() && (!Pos || IsInline) &&
					(End == Body.size() || IsInline && Body[End] == '}' || std::isspace(static_cast<unsigned char>(Body[End]))) &&
					IsWord(Body.substr(Pos + 1, Inheritance.size() - 1), Inheritance.substr(1)) && std::ranges::none_of(
						std::span<const unsigned char>(Protected).subspan(Pos, Inheritance.size()),
						[](const unsigned char Mask) -> bool {
							return Mask;
						}
					)
				) {
					std::string_view Rest = Body.substr(End);
					SkipSpace(Rest);
					// インラインタグは閉じ迄を照合した場合だけ継承指定として扱う
					if(!IsInline || Rest.starts_with('}')) return std::nullopt;
				}
				if(Body[Pos] == '{' && (InlineDepth || Pos + 1 < Body.size() && Body[Pos + 1] == '@')) ++InlineDepth;
				else if(Body[Pos] == '}' && InlineDepth) --InlineDepth;
			}
			// Doxygen ･ Javadoc ･ KDoc ･ PHPDoc ･ JSDoc ･ YARD のタグ（タグ・ディレクティブを始める字の後に英字が続く行，Doxygen の `\` の命令を含む）
			if(!IsTag) continue;
			// 仮引数名と見出を除く前の本文
			std::string_view TypeParam = Body;
			if(ConsumeDocItemTag(Rules, DocDescription::TypeParam, TypeParam)) {
				// 型の制約と空白を除いた型仮引数名（複数名は項目内の順を保つ）
				SkipSpace(TypeParam);
				SkipBracketed('{', '}', TypeParam);
				SkipSpace(TypeParam);
				size_t NameLen = TokenLen(TypeParam);
				while(NameLen && TypeParam[NameLen - 1] == ',') {
					// カンマの後の空白を含めた名前列（説明文は取り込まない）
					std::string_view Rest = TypeParam.substr(NameLen);
					SkipSpace(Rest);
					if(Rest.empty()) break;
					NameLen = TypeParam.size() - Rest.size() + TokenLen(Rest);
				}
				if(NameLen) Start(Idx, DocItemKind::TypeParam, TypeParam.substr(0, NameLen), 0);
				else Start(Idx, DocItemKind::Other, {}, 0);
				continue;
			}
			const std::string_view Name = GetParamTagName(Rules, Body);
			std::string_view Rest = Body;
			if(!Name.empty()) Start(Idx, DocItemKind::Param, Name, 0);
			else if(std::string_view ReturnText = Body; HasConsumedReturnHeader(Rules, ReturnText)) Start(Idx, DocItemKind::Return, {}, 0);
			else {
				const bool IsSummary = !HasParamOrReturn && ConsumeDocItemTag(Rules, DocDescription::Summary, Rest);
				Start(Idx, IsSummary ? DocItemKind::Summary : DocItemKind::Other, {}, 0);
			}
		}
		// 未完了の XML の構造を持つドキュメントは同期しない事の返戻
		if(IsInTag || IsInCdata || IsInXmlComment || Depth) return std::nullopt;
		// 原文の順の項目の返戻
		return Items;
	}

	/**
	 * 仮引数のメンバの項目の親の名前の取得関数
	 * 計算量：項目名のバイト数 N に対し O(N + 1)
	 * @param ItemName 項目の仮引数名
	 * @return JSDoc の `opts.name` / `opts[].name` の `opts`（メンバの項目でなければ空）
	 */
	std::string_view DocCommentReader::GetParentName(const std::string_view ItemName) {
		// メンバ名を区切る字の探索
		const size_t Cut = ItemName.find_first_of(".[");
		// メンバの区切の前の名前の返戻
		return Cut == std::string_view::npos ? std::string_view() : ItemName.substr(0, Cut);
	}

	/**
	 * 項目の解析から除くコード行の判定関数
	 * 計算量：行のバイト数 N，コード要素名の長さの和 K に対し O(N(K + 1))
	 * @param IsXmlDoc 最上位の要素を XML として読むドキュメントか
	 * @param Line ドキュメントの論理行
	 * @param Protected 同じ行の共有済の保護マスク
	 * @param CodeElements テキストを保つコード要素の登録名
	 * 制約：Protected.size() = Line.size()
	 * @return コードの行で項目の解析を行わない場合は true
	 */
	bool DocCommentReader::IsCodeLine(
		const bool IsXmlDoc,
		const std::string_view Line,
		const std::span<const unsigned char> Protected,
		const std::span<const std::string_view> CodeElements
	) {
		// 行の最初の非空白字のコード判定
		const size_t First = Line.find_first_not_of(" \t");
		// コードで始まらない行の返戻
		if(First == std::string_view::npos || !Protected[First]) return false;
		// XML 以外の行頭の判定の返戻
		if(!IsXmlDoc) return true;
		// XML の同行のコード要素の後に続く項目の識別
		for(size_t Idx = First; Idx < Line.size(); ++Idx) {
			// コード要素外のテキスト，又は深さを継ぐコード要素の開閉を持つ行の返戻
			if(
				!Protected[Idx] && Line[Idx] != ' ' && Line[Idx] != '\t' || Line[Idx] == '<' && Idx + 1 < Line.size() &&
				std::ranges::find(CodeElements, GetLeadingName(Line.substr(Idx + (Line[Idx + 1] == '/' ? 2 : 1)))) != CodeElements.end()
			) return false;
		}
		// コードだけの行の返戻
		return true;
	}

	/**
	 * 仮引数・戻値・失敗条件の項目の説明の有無の判定関数
	 * 計算量：XML は共有する内容の有無を参照して O(1)，其の他は本文の長さの和 L，項目見出の判定費用 Q に対し O(L + Q)
	 * @param Rules 投影済の記法
	 * @param Item 仮引数・戻値・失敗条件の項目
	 * @param Lines ドキュメントの論理行列（記号除去済）
	 * 制約：参照する項目は 0 ≤ Item.Begin < Item.End ≤ Lines.size()
	 * @return タグ・見出・項目ラベルの後か続きの行に説明が有れば `true`
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	bool DocCommentReader::HasItemDescription(
		const NotationRules &Rules,
		const DocItem &Item,
		const std::vector<std::string> &Lines
	) {
		// 説明の有無は展開後の値で確定した同じ項目から読む
		if(Item.ReadingItem) return HasItemDescription(Rules, *Item.ReadingItem, *Item.ReadingLines);
		// XML は項目の読取で確定した内容を共有し，引用された属性を本文として読み直さない事の返戻
		if(VocabularyOf(Rules.Vocabulary, DocDescription::Other).Form == DocItemForm::XmlElement) return Item.HasXmlDescription;
		// 項目の最初の論理行の取得
		const std::string_view First = GetLineOf(Item, Lines, Item.Begin);
		// 続きの行に本文が有るか
		const auto HasContinuation = [&Lines, &Item]() -> bool {
			// 続きの行の説明の探索
			for(size_t Idx = Item.Begin + 1; Idx < Item.End; ++Idx) if(Lines[Idx].find_first_not_of(" \t") != std::string::npos) {
				// 本文を持つ行が有る事の返戻
				return true;
			}
			// 続きの行に本文の無い事の返戻
			return false;
		};
		switch(VocabularyOf(Rules.Vocabulary, DocDescription::Other).Form) {
		case DocItemForm::Heading:
			if(Item.Kind == DocItemKind::Param) {
				// 箇条のマーカーと仮引数名の後の区切（`-` / `:` / `–` / `—`）の読飛
				std::string_view View = First.substr(1);
				SkipSpace(View);
				size_t Consumed = 0;
				GetLeadingName(View, &Consumed);
				View.remove_prefix(Consumed);
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
				// 項目ラベルの `:` の後に同じ行の説明が在る場合の返戻
				if(
					const size_t Colon = First.find(':');
					Colon != std::string_view::npos && First.find_first_not_of(" \t", Colon + 1) != std::string_view::npos
				) return true;
				return HasContinuation();
			}
		default:
			{
				std::string_view View = First;
				if(ConsumeDocItemTag(Rules, DocDescription::Failure, View)) {
					// 送出例外の型（JSDoc の `{型}`，YARD の `[型]`，其の他のタグの後の型名）の読飛
					SkipSpace(View);
					if(View.starts_with('{')) SkipBracketed('{', '}', View);
					else if(View.starts_with('[')) SkipBracketed('[', ']', View);
					else if(!Rules.HasOptionalFailureType) View.remove_prefix(TokenLen(View));
					SkipSpace(View);
					// 同じ行か続きの行の発生条件の説明の有無の返戻
					return !View.empty() || HasContinuation();
				}
				if(HasConsumedParamHeader(Rules, View)) {
					// 名前（JSDoc の省略可能な仮引数は括弧毎）の読飛
					if(View.starts_with('[')) SkipBracketed('[', ']', View);
					else if(View.starts_with('`')) {
						size_t Consumed = 0;
						GetLeadingName(View, &Consumed);
						View.remove_prefix(Consumed);
					} else View.remove_prefix(std::min(TokenLen(View), View.find_first_of(Rules.DocParamNameTerminators)));
					if(!View.empty() && Rules.DocParamNameTerminators.find(View.front()) != std::string_view::npos) View.remove_prefix(1);
					SkipSpace(View);
				} else {
					View = First;
					// 戻値のタグでない行の返戻
					if(!HasConsumedReturnHeader(Rules, View)) return false;
					// PHPDoc の戻値の型の読飛
					if(Rules.HasBareDocType) View.remove_prefix(TokenLen(View));
					SkipSpace(View);
				}
				// YARD の名前の後の型の読飛
				if(Rules.HasBracketedDocType) SkipBracketed('[', ']', View);
				SkipSpace(View);
				// タグの後又は続きの行の説明文有無の返戻
				return !View.empty() || HasContinuation();
			}
		}
	}

	/**
	 * 型のメンバの並びかの判定関数（整形仕様 7.4）
	 * @param Syntax 読取専用の期待構文
	 * @param Node ノード
	 * @return 型の本体（関数の本体を兼ねる物を除く）・フィールドの並び・型のメンバの本体・列挙子の並びなら true
	 * @throws FormatLimitExceeded 作業予算を超えた場合
	 */
	bool DocCommentReader::IsTypeMemberSeq(const SyntaxModel &Syntax, const NodeId Node) {
		// メンバを並べる型の本体かの返戻
		return Syntax.Is(Node, Role::ClassBodyContainer) && !Syntax.Is(Node, Role::DefBody) || Syntax.Is(Node, Role::FieldList) ||
		Syntax.Is(Node, Role::TypeMemberBody) || Syntax.Is(Node, Role::EnumeratorSeq);
	}

	/**
	 * ノードの所属する宣言の取得関数（整形仕様 7.4）
	 * 宣言子・仮引数・値として作る物のメンバ等の宣言の部分からは所属する宣言へ辿り，文・値の位置の式・並びの宣言でない項目で止める
	 * 呼出と代入はアンカー自身が宣言として働く物だけを採り，名前だけのノードは型の列挙項目だけを採る
	 * 計算量：名前の探索と宣言の判定で読むノード数 N に対し O(N + 1)
	 * @param Syntax 読取専用の期待構文
	 * @param Node ノード
	 * @param IsAnchor アンカー自身か（子から辿り着いた物でないか）
	 * @param Inherited 親のノードへ子から辿り着いた時の宣言
	 * @return 所属する宣言（ドキュメントの位置でなければ無い）
	 * @throws FormatLimitExceeded 構文の照会・走査が作業予算を超える場合
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	NodeId DocCommentReader::GetDeclTargetOf(
		const SyntaxModel &Syntax,
		const NodeId Node,
		const bool IsAnchor,
		const NodeId Inherited
	) {
		// 宣言の親と役割の取得
		const NodeId Parent = Syntax.GetParentOf(Node);
		// 値の位置の式・文と，閉じを別行に置く型・名前空間の本体の終端境界の返戻
		if(
			Syntax.Is(Node, Role::ExprStmt) || Syntax.Is(Node, Role::Control) ||
			Syntax.Is(Node, Role::CloserToken) && (Syntax.Is(Parent, Role::ClassLike) || Syntax.Is(Parent, Role::NamespaceLike)) &&
			Syntax.ShouldSeparateBeforeClose(Parent)
		) return NoNode;
		// 初期値の先頭に付く説明は宣言の部分とし，其の値の内側の式へ所属を引き継がない
		if(Syntax.HasField(Node, ChildRole::Val) && !IsTypeMemberSeq(Syntax, Node)) return IsAnchor ? Inherited : NoNode;
		// 値として作る物のメンバの返戻
		if(Syntax.Is(Parent, Role::ObjectVal)) return Node;
		if(Syntax.Is(Node, Role::DocumentableDecl) || Syntax.Is(Node, Role::ImportItem)) {
			if(Syntax.Is(Node, Role::AssignmentExpr)) {
				// 代入はアンカー自身の定数の束縛だけが宣言と為る事の返戻
				return IsAnchor && Syntax.Is(Syntax.GetNamedChild(Node, 0), Role::ConstantName) ? Node : NoNode;
			}
			// アンカー自身が宣言として働く呼出名かの返戻
			if(Syntax.Is(Node, Role::ReceiverCall)) return IsAnchor && Syntax.Is(Node, Role::MemberDocumentableCall) ? Node : NoNode;
			if(!Syntax.GetNamedChildCount(Node)) {
				// 名前だけのノードは型の列挙項目かの返戻
				return Syntax.Is(Parent, Role::ClassBodyContainer) && Syntax.Is(Syntax.GetParentOf(Parent), Role::ClassLike) ? Node : NoNode;
			}
			// 型・メンバ・最上位の宣言の返戻
			return Node;
		}
		// 並びの宣言でない項目と，宣言の部分の所属する宣言の返戻
		return Parent == NoNode || Syntax.Is(Parent, Role::DocHostSeq) ? NoNode : Inherited;
	}

	/**
	 * 定義の見出を終える子かの判定関数（整形仕様 7.4）
	 * 本体と，本体のノードを持たない定義（Ruby の空の `def ... end` 等）の閉じのトークンから後は見出でない
	 * 空の本体を見出と同じ行へ畳む定義の閉じのトークンは，其の前の物が定義の前へ移る為に見出とする（移動で成立する統合）
	 * @param Syntax 読取専用の期待構文
	 * @param Def 定義
	 * @param Child 定義の子
	 * @return 本体又は畳めない定義の閉じのトークンなら true
	 * @throws FormatLimitExceeded 構文の照会・走査が作業予算を超える場合
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	bool DocCommentReader::IsHeaderEnd(const SyntaxModel &Syntax, const NodeId Def, const NodeId Child) {
		// 本体か畳めない定義の閉じのトークンかの返戻
		return Syntax.IsNamed(Child) && Syntax.Is(Child, Role::DefBody) ||
		Syntax.Is(Child, Role::CloserToken) && Syntax.ShouldSeparateBeforeClose(Def);
	}

	/**
	 * 型を宣言するノードかの判定関数（整形仕様 7.4 のローカルの型）
	 * 型・メンバの並びを持つ宣言，型の名前を宣言する宣言（別名・型定義），型の部分にメンバの並びを持つ宣言と，其れ等を名前付の子に持つ宣言を型の宣言とする
	 * 計算量：宣言の子と名前の探索で読むノード数 N，名前のバイト数 B に対し O(N + B + 1)
	 * @param Syntax 読取専用の期待構文
	 * @param Node ノード
	 * @return 型を宣言するノードなら true
	 * @throws FormatLimitExceeded 構文の照会・走査が作業予算を超える場合
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	bool DocCommentReader::IsTypeDecl(const SyntaxModel &Syntax, const NodeId Node) {
		// 型かメンバの並びを子に持つノードか
		const auto HasTypeBody = [&Syntax](const NodeId Part) -> bool {
			// 型かメンバの並びを持つかの返戻
			return Syntax.Is(Part, Role::ClassLike) || std::ranges::any_of(
				Syntax.GetChildrenOf(Part),
				[&Syntax](const NodeId Child) -> bool {
					// メンバの並びかの返戻
					return IsTypeMemberSeq(Syntax, Child);
				}
			);
		};
		// 型を宣言する宣言か
		const auto IsType = [&Syntax, &HasTypeBody](const NodeId Decl) -> bool {
			// 型の名前・型の本体・型の部分の本体を持つ宣言かの返戻
			return Syntax.Is(Decl, Role::DocumentableDecl) && (
				Syntax.Is(Syntax.GetFieldChild(Decl, ChildRole::Name), Role::TypeName) ||
				Syntax.Is(Syntax.GetFieldChild(Decl, ChildRole::Declarator), Role::TypeName) || HasTypeBody(Decl) ||
				HasTypeBody(Syntax.GetFieldChild(Decl, ChildRole::Type))
			);
		};
		// 自身か名前付の子が型の宣言かの返戻
		return IsType(Node) || std::ranges::any_of(
			Syntax.GetChildrenOf(Node),
			[&Syntax, &IsType](const NodeId Child) -> bool {
				// 型の部分の宣言かの返戻
				return Syntax.IsNamed(Child) && IsType(Child);
			}
		);
	}

	/**
	 * 記録の出力がドキュメント専用の囲みの記法かの判定関数
	 * 計算量：本文の長さ B に対し O(B)
	 * @param Record 投影済のコメント
	 * @return ドキュメント専用の囲記法 `DoubleStarBlock`・`StarBangBlock` なら `true`
	 */
	bool DocCommentReader::IsDocBlock(const CommentRecord &Record) {
		// 出力記法の囲みの種類の判定
		switch(CommentRepository::GetOutputNotationOf(Record)) {
		case CommentNotation::DoubleStarBlock:
		case CommentNotation::StarBangBlock:
			// 区切と空の縁を除くドキュメントの囲みかの返戻
			return !Record.IsEmbeddedEdge;
		default:
			// ドキュメントの囲みでない記法の返戻
			return false;
		}
	}

	/**
	 * 記録の出力のドキュメントの行記号の取得関数
	 * godoc のディレクティブの形の行を除き，`#:` の行は YARD の `#` とする
	 * 計算量：本文の長さ B に対し O(B)
	 * @param Record 投影済のコメント
	 * @return 行形式のドキュメントの記号（其れ以外は空）
	 * @throws FormatLimitExceeded 記法の定義が無い場合
	 */
	std::string_view DocCommentReader::GetDocLineMarker(const CommentRecord &Record) {
		// 記録の出力記法とドキュメント形式の取得
		const NotationRules &Rules = CommentRepository::GetRulesOf(Record);
		const CommentNotation Notation = CommentRepository::GetOutputNotationOf(Record);
		const bool IsLineDoc =
		!Record.IsEmbeddedEdge && !CommentRepository::IsBlockNotation(Notation) && Rules.GetDocFormOf(Notation) && !(
			Rules.ShouldExcludeDirectiveLines && Notation == CommentNotation::SlashLine && Record.Format.ShouldAttachFirstLine &&
			IsDirectiveBody(Record.Text)
		);
		// 行形式のドキュメント記号の返戻
		return IsLineDoc ?
		CommentRepository::GetOpenerOf(Notation == CommentNotation::HashColonLine ? CommentNotation::HashLine : Notation) :
		std::string_view();
	}

	/**
	 * 新しいドキュメントを書く行形式の記号の取得関数（整形仕様 7.4, 7.6）
	 * @param Rules 投影済の記法
	 * @return 採用するドキュメントの記法が行形式なら其の記号（囲みのドキュメントとドキュメントを作らない文法は空）
	 */
	std::string_view DocCommentReader::GetNewDocMarker(const NotationRules &Rules) {
		// 新しいドキュメントの採用記法の取得
		const CommentOutputForm *const Doc = Rules.GetAdoptedFormOf(CommentUse::Doc);
		// 行形式の採用記法の記号の返戻
		return Doc && !CommentRepository::IsBlockNotation(Doc->Notation) ?
		CommentRepository::GetOpenerOf(Doc->Notation) :
		std::string_view();
	}

	/**
	 * XML 属性の文字参照をデコードした比較名の取得関数（原文の属性値は変更しない）
	 * 計算量：属性値のバイト数 N に対し O(N)
	 * @param Name 引用符を除いた属性値
	 * @return 比較名（未知の実体参照・不正な文字参照を持つ場合は空）
	 * @throws std::bad_alloc 比較名のメモリを確保出来ない場合
	 * @throws std::length_error 比較名が文字列の上限を超える場合
	 */
	std::optional<std::string> DocCommentReader::DecodeXmlName(const std::string_view Name) {
		// XML で宣言無に使用出来る実体参照（名前順）
		static constexpr std::pair<std::string_view, char32_t> Entities[] =
		{ { "amp", '&' }, { "apos", '\'' }, { "gt", '>' }, { "lt", '<' }, { "quot", '"' } };
		std::string Result;
		Result.reserve(Name.size());
		// 属性値を原文の順に読み，文字参照だけを置換する
		for(size_t Pos = 0; Pos < Name.size();) {
			if(Name[Pos] != '&') {
				Result.push_back(Name[Pos++]);
				continue;
			}
			const size_t End = Name.find(';', Pos + 1);
			// 終端の無い文字参照の返戻
			if(End == std::string_view::npos) return std::nullopt;
			std::string_view Ref = Name.substr(Pos + 1, End - Pos - 1);
			char32_t Point = 0;
			if(Ref.starts_with('#')) {
				Ref.remove_prefix(1);
				const bool IsHex = Ref.starts_with('x');
				if(IsHex) Ref.remove_prefix(1);
				uint32_t Value = 0;
				const std::from_chars_result Parsed = std::from_chars(Ref.data(), Ref.data() + Ref.size(), Value, IsHex ? 16 : 10);
				// 未完了・範囲外及び XML で許されないコードポイントの拒否
				if(
					Parsed.ec != std::errc() || Parsed.ptr != Ref.data() + Ref.size() || !(
						Value == 9 || Value == 10 || Value == 13 || Value > 0X1F && Value < 0XD800 || Value > 0XDFFF && Value < 0XFFFE ||
						Value > 0XFFFF && Value < 0X110000
					)
				) return std::nullopt;
				Point = static_cast<char32_t>(Value);
			} else {
				for(const std::pair<std::string_view, char32_t> &Entity : Entities) if(Entity.first == Ref) {
					Point = Entity.second;
					break;
				}
				// 未知の実体参照の返戻
				if(!Point) return std::nullopt;
			}
			TextEdit::EncodeUtf8(Point, Result);
			Pos = End + 1;
		}
		// 所有する比較名の返戻
		return Result;
	}

	/**
	 * ドキュメントの論理行の項目への分割関数（整形仕様 7.4）
	 * 同じ本文世代の共有構造から取り出したコード例の判定に従って分ける
	 * 計算量：論理行の長さの和 B とシグネチャ照合の費用 S に対し O(B + S)
	 * @param Rules 投影済の記法
	 * @param Lines ドキュメントの論理行列（記号除去済）
	 * @param FunctionSig 実シグネチャ
	 * @param CodeStarts 同じ本文世代の論理行毎のコード判定
	 * @param ParamIdx 同じシグネチャの本文世代と静的解析で共有する仮引数名のインデックス
	 * @param GroupStarts 原文のドキュメント毎の最初の論理行（空なら１つのドキュメント）
	 * 制約：ParamIdx が参照する FunctionSig.Params のテキストは，インデックスの使用中は生存し，変更しない
	 * 制約：返戻する Name ･ Names ･ Parent は項目が所有する読取行・デコード済の比較名又は Lines を参照する為，利用中は項目と Lines を生存させ，テキストを変更しない
	 * @return 原文の順の項目（最初は概要，同期出来ないドキュメントは空）
	 * @throws std::bad_alloc 項目・照合名・インデックスのメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	std::optional<std::vector<DocItem>> DocCommentReader::ParseItems(
		const NotationRules &Rules,
		const std::vector<std::string> &Lines,
		const Signature &FunctionSig,
		const std::optional<DocReading> &CodeStarts,
		DocParamIdx &ParamIdx,
		const std::span<const size_t> GroupStarts
	) {
		// 既存ドキュメントの項目の分割
		const std::vector<std::string> &ReadingLines = CodeStarts && CodeStarts->Lines ? *CodeStarts->Lines : Lines;
		const std::span<const size_t> ReadingGroups =
		CodeStarts && CodeStarts->Lines ? std::span<const size_t>(CodeStarts->GroupStarts) : GroupStarts;
		std::optional<std::vector<DocItem>> Items = ItemsOf(Rules, ReadingLines, FunctionSig, CodeStarts, ReadingGroups, ParamIdx);
		if(Items) for(DocItem &Item : *Items) if(Item.Kind == DocItemKind::Param || Item.Kind == DocItemKind::TypeParam) {
			// XML の属性の原文を論理行へ残し，比較に使う名前だけを１度デコードする
			if(
				VocabularyOf(Rules.Vocabulary, DocDescription::Other).Form == DocItemForm::XmlElement &&
				Item.Name.find('&') != std::string_view::npos
			) {
				std::optional<std::string> Name = DecodeXmlName(Item.Name);
				// 解釈出来ない参照の名前は原文の綴りを保ち，他の項目の照合と不足判定を妨げない
				if(Name) {
					Item.DecodedName = std::make_shared<const std::string>(std::move(*Name));
					Item.Name = *Item.DecodedName;
				}
			}
			Item.Parent = GetParentName(Item.Name);
			for(size_t Pos = 0; Pos < Item.Name.size() + 1;) {
				const size_t End = std::min(Item.Name.find(',', Pos), Item.Name.size());
				std::string_view Part = Item.Name.substr(Pos, End - Pos);
				SkipSpace(Part);
				while(!Part.empty() && (Part.back() == ' ' || Part.back() == '\t')) Part.remove_suffix(1);
				Item.Names.push_back(Part);
				Pos = End + 1;
			}
		}
		// JSDoc の分解の仮引数は，項目の順に付けた名前を仮引数全体の導入名へ対応付ける
		if(Items && FunctionSig.HasUnnamedParam && HasDocumentForm(Rules, DocumentFormId::JSDoc)) {
			std::unordered_map<std::string_view, size_t> Owners, Aliases;
			const std::vector<ParamShape> &Shapes = FunctionSig.ParamShapes;
			for(size_t Idx = 0; Idx < Shapes.size(); ++Idx) {
				const size_t End = Idx + 1 < Shapes.size() ? Shapes[Idx + 1].NameBegin : FunctionSig.Params.size();
				for(size_t Name = Shapes[Idx].NameBegin; Name < End; ++Name) Owners.emplace(FunctionSig.Params[Name], Idx);
			}
			size_t Next = 0;
			for(DocItem &Item : *Items) {
				if(Item.Kind != DocItemKind::Param || !Item.Parent.empty() || Item.Names.size() != 1) continue;
				if(const std::unordered_map<std::string_view, size_t>::const_iterator Owner = Owners.find(Item.Name); Owner != Owners.end()) {
					Next = std::max(Next, Owner->second + 1);
					continue;
				}
				const auto [Alias, Added] = Aliases.try_emplace(Item.Name, Next);
				if(Added && Next < Shapes.size()) ++Next;
				const size_t Idx = Alias->second;
				if(Idx == Shapes.size() || !Shapes[Idx].IsDestructure) continue;
				Item.ParamOrdinal = Shapes[Idx].Ordinal;
			}
		}
		// 読取範囲を所有し，出力用の範囲だけを元の綴りへ対応付ける
		if(Items && CodeStarts && CodeStarts->Lines) for(DocItem &Item : *Items) {
			Item.ReadingLines = CodeStarts->Lines;
			Item.ReadingItem = std::make_shared<const DocItem>(Item);
			const auto Position = [&CodeStarts](const size_t Line, const size_t Offset) -> std::pair<size_t, size_t> {
				const std::vector<std::pair<size_t, size_t>> &Positions = CodeStarts->OriginalPositions[Line];
				// 指定無の終端は読取行の末尾へ対応付ける事の返戻
				return Positions[Offset == std::string_view::npos ? Positions.size() - 1 : Offset];
			};
			if(Item.Begin == Item.End) {
				const std::pair<size_t, size_t> Empty = Position(Item.Begin, Item.BeginOffset);
				Item.Begin = Item.End = Empty.first;
				Item.BeginOffset = Item.EndOffset = Empty.second;
				continue;
			}
			const std::pair<size_t, size_t> Begin = Position(Item.Begin, Item.BeginOffset), End = Position(Item.End - 1, Item.EndOffset);
			Item.Begin = Begin.first;
			Item.BeginOffset = Begin.second;
			Item.End = End.first + 1;
			Item.EndOffset = End.second;
			if(Item.OpenLine != std::string_view::npos) {
				const std::pair<size_t, size_t> Open = Position(Item.OpenLine, Item.OpenOffset);
				Item.OpenLine = Open.first;
				Item.OpenOffset = Open.second;
			}
			if(Item.CloseLine != std::string_view::npos) {
				const std::pair<size_t, size_t> Close = Position(Item.CloseLine, Item.CloseOffset);
				Item.CloseLine = Close.first;
				Item.CloseOffset = Close.second;
			}
		}
		// コード例と名前の構造を共有する項目の返戻
		return Items;
	}

	/**
	 * 項目の解析から除くコード行の判定関数（整形仕様 3.3, 7.3, 7.4）
	 * 元のドキュメント毎の論理行を１つの記録として，正規化と同じ本文構造の判定 (`CommentNormalizer::Inspect`) で読む
	 * 統合・整合で論理行が変わる度に判定し直す
	 * 計算量：行数 L，本文長 B，コード要素名長の和 K，１記録の Inspect の費用 I に対し O(L + B(K + 1) + I)
	 * @param Rules 投影済の記法
	 * @param Lines ドキュメントの論理行列
	 * @param Previous 共有する本文世代の構造（無ければ初回）
	 * @param GroupStarts 原文のドキュメント毎の最初の論理行（空なら１つのドキュメント）
	 * @return 項目の解析から除くコード行なら true の並び（閉じないコード領域を持つドキュメントは無い）
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	std::optional<DocReading> DocCommentReader::CodeLineStarts(
		const NotationRules &Rules,
		const std::vector<std::string> &Lines,
		const CommentStructure *Previous,
		const std::span<const size_t> GroupStarts
	) {
		// 元のドキュメント毎に本文とコード例の範囲を判定し，別のドキュメントへ未閉鎖の範囲を継がない
		std::vector<CommentRecord> Docs;
		Docs.push_back({ {}, { CommentPlacement::Before } });
		Docs.back().Format.Rules = &Rules;
		size_t Group = !GroupStarts.empty() && !GroupStarts.front() ? 1 : 0;
		for(size_t Idx = 0; Idx < Lines.size(); ++Idx) {
			if(Group < GroupStarts.size() && GroupStarts[Group] == Idx) {
				Docs.push_back({ {}, { CommentPlacement::Before } });
				Docs.back().Format.Rules = &Rules;
				++Group;
			} else if(Idx) Docs.back().Text += '\n';
			Docs.back().Text += Lines[Idx];
		}
		const CommentStructure Structure = CommentNormalizer::Inspect(ReadOnlyCommentView(Docs), Previous);
		// 未閉鎖のコード例を持つドキュメントの返戻
		for(size_t Idx = 0; Idx < Docs.size(); ++Idx) if(Structure.HasUnclosedCode(Idx)) return std::nullopt;
		// 本文のコードの範囲と論理行毎の判定
		const bool IsXmlDoc = VocabularyOf(Rules.Vocabulary, DocDescription::Other).Form == DocItemForm::XmlElement;
		bool HasReading = false;
		for(size_t Idx = 0; Idx < Docs.size(); ++Idx) if(Structure.GetReadBody(Idx)) HasReading = true;
		DocReading Result;
		Result.Code.reserve(Lines.size());
		if(HasReading) {
			std::shared_ptr<std::vector<std::string>> Values = std::make_shared<std::vector<std::string>>();
			size_t FirstLine = 0;
			for(size_t Doc = 0; Doc < Docs.size(); ++Doc) {
				Result.GroupStarts.push_back(Values->size());
				const std::optional<CommentStructure::ReadBody> Reading = Structure.GetReadBody(Doc);
				const std::string_view Text = Reading ? Reading->Text : std::string_view(Docs[Doc].Text);
				const std::span<const unsigned char> Protected = Reading ? Reading->Code : Structure.GetCodeMask(Doc);
				size_t RawPos = 0, RawLine = FirstLine, RawStart = 0;
				const auto Position = [&](const size_t At) -> std::pair<size_t, size_t> {
					const size_t Offset = Reading ? Reading->OriginalOffsets[At] - Reading->SourceOffset : At;
					while(RawPos < Offset) if(Docs[Doc].Text[RawPos++] == '\n') {
						++RawLine;
						RawStart = RawPos;
					}
					// 元の論理行と其の行内の位置の返戻
					return { RawLine, Offset - RawStart };
				};
				for(size_t Pos = 0;;) {
					const size_t End = std::min(Text.find('\n', Pos), Text.size());
					Values->emplace_back(Text.substr(Pos, End - Pos));
					Result.Protected.emplace_back(Protected.begin() + Pos, Protected.begin() + End);
					Result.Code.push_back(IsCodeLine(IsXmlDoc, Values->back(), Protected.subspan(Pos, End - Pos), Rules.CodeExamples.CodeElements));
					std::vector<std::pair<size_t, size_t>> &Positions = Result.OriginalPositions.emplace_back();
					Positions.reserve(End - Pos + 1);
					for(size_t At = Pos; At <= End; ++At) Positions.push_back(Position(At));
					// 元の綴りで改行のエスケープを持つ終端も，前の項目の範囲へ残す
					if(Reading && End < Text.size() && Reading->OriginalOffsets[End + 1] - Reading->OriginalOffsets[End] > 1) {
						Positions.back() = Position(End + 1);
					}
					if(End == Text.size()) break;
					Pos = End + 1;
				}
				FirstLine += std::ranges::count(Docs[Doc].Text, '\n') + 1;
			}
			Result.Lines = std::move(Values);
		} else {
			size_t Offset = 0, Doc = 0;
			Group = !GroupStarts.empty() && !GroupStarts.front() ? 1 : 0;
			// ドキュメントの論理行走査
			for(size_t Idx = 0; Idx < Lines.size(); ++Idx) {
				if(Group < GroupStarts.size() && GroupStarts[Group] == Idx) {
					++Doc;
					++Group;
					Offset = 0;
				}
				const std::string &Line = Lines[Idx];
				const std::span<const unsigned char> Protected = Structure.GetCodeMask(Doc);
				Result.Protected.emplace_back(Protected.begin() + Offset, Protected.begin() + Offset + Line.size());
				Result.Code.push_back(IsCodeLine(IsXmlDoc, Line, Protected.subspan(Offset, Line.size()), Rules.CodeExamples.CodeElements));
				Offset += Line.size() + 1;
			}
		}
		// 各行のコード判定の返戻
		return Result;
	}

	/**
	 * 説明対象を求めるノード毎の表の構築関数（整形仕様 7.4）
	 * 子から辿り着いた時の所属する宣言，定義の本体の中か，ドキュメント化対象の定義のアンカー・ラッパーと見出の中のノード毎の其の定義を１度の走査で求める
	 * 見出（前置の属性と本体の手前迄の子の部分木）の中のノードは，同期が見出のドキュメントと通常コメントを統合する最も外の定義を説明対象とする
	 * 計算量：ノード数 V，子の数の和 E，定義・宣言・アンカー・ラッパー・見出の判定で再び調べるノード・辺の延数 A と語の照合長の和 B に対し O(V + E + A + B)
	 * @param Syntax 読取専用の構文
	 * @param Work 所属の表が訪ねたノードとアンカーの数（無ければ計上しない）
	 * @return ノード毎の表
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws FormatLimitExceeded 構文の照会・走査が作業予算を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	DocCommentReader::TargetTables DocCommentReader::TablesOf(const SyntaxModel &Syntax, size_t *const Work) {
		// ノード毎の所属の表の準備
		TargetTables Tables;
		Tables.Decls.assign(Syntax.GetNodeCount(), NoNode);
		Tables.Enclosing.assign(Syntax.GetNodeCount(), Syntax.GetRoot());
		Tables.IsInDefBody.assign(Syntax.GetNodeCount(), 0);
		Tables.IsDefs.assign(Syntax.GetNodeCount(), 0);
		Tables.Anchors.assign(Syntax.GetNodeCount(), NoNode);
		// ドキュメント化対象の定義のアンカーと其れを先頭に持つラッパー毎の其の定義と，見出の中のノード毎の最も外の定義
		std::vector<std::pair<NodeId, NodeId>> Sites;
		std::vector<NodeId> Headers(Syntax.GetNodeCount(), NoNode);
		// ノード毎のドキュメントを同期する定義か
		const DocumentationFact RootFact = SignatureExtractor::GetDocumentationFact(Syntax, Syntax.GetRoot());
		Tables.IsDefs[Syntax.GetRoot()] = RootFact.IsDocumentable;
		Tables.Anchors[Syntax.GetRoot()] = RootFact.Anchor;
		for(std::vector<NodeId> Pending { Syntax.GetRoot() }; !Pending.empty();) {
			if(Work) ++*Work;
			const NodeId Node = Pending.back();
			Pending.pop_back();
			const NodeId Parent = Syntax.GetParentOf(Node);
			Tables.Decls[Node] = GetDeclTargetOf(Syntax, Node, false, Parent == NoNode ? NoNode : Tables.Decls[Parent]);
			// ローカルの型と値として作る物の本体から先はメンバの並びとして本体の外に戻す（整形仕様 7.4）
			// 型・名前空間・ラッパーの宣言（TypeScript の `declare global`）の本体と同じノードの定義の本体は，宣言を並べる場として定義の本体に数えない
			// 定義の見出の後の閉じのトークン（Ruby の `def ... end` の `end`）は本体の終端境界の為，其処のアンカーは本体の中の物とする
			const NodeId Scope = Syntax.GetParentOf(Parent);
			if(Parent != NoNode) {
				Tables.Enclosing[Node] = Syntax.Is(Parent, Role::DocHostSeq) || Syntax.Is(Parent, Role::DefBody) ?
				Scope == NoNode ? Parent : Scope :
				Tables.Enclosing[Parent];
			}
			// 未展開のトークン列から，実在する宣言の本体への所属は確定しない
			if(Syntax.GetNode(Node).Facts.TokenSpacing != TokenSpacingMode::None) Tables.Enclosing[Node] = NoNode;
			Tables.IsInDefBody[Node] = Parent != NoNode && !Syntax.Is(Parent, Role::ClassLike) && !IsTypeMemberSeq(Syntax, Parent) &&
			!Syntax.Is(Parent, Role::ObjectVal) && (
				Syntax.Is(Parent, Role::DefBody) && !Syntax.Is(Scope, Role::ClassLike) && !Syntax.Is(Scope, Role::NamespaceLike) &&
				!Syntax.Is(Scope, Role::DocWrapper) || Tables.IsInDefBody[Parent] ||
				Tables.IsDefs[Parent] && Headers[Node] == NoNode && Syntax.Is(Node, Role::CloserToken)
			);
			if(Tables.IsDefs[Node]) {
				NodeId Up = Tables.Anchors[Node];
				Sites.emplace_back(Up, Node);
				while(true) {
					const NodeId Parent = Syntax.GetParentOf(Up);
					if(Parent == NoNode || Syntax.GetNode(Up).Idx) break;
					Sites.emplace_back(Up = Parent, Node);
				}
			}
			// 本体の手前迄の子の見出への所属（見出の中のノードの子は外の定義の見出の儘）
			bool IsHeader = Tables.IsDefs[Node], HasDefs = false;
			const std::span<const NodeId> Children = Syntax.GetChildrenOf(Node);
			for(const NodeId Child : Children) {
				const DocumentationFact Fact = SignatureExtractor::GetDocumentationFact(Syntax, Child);
				Tables.IsDefs[Child] = Fact.IsDocumentable;
				Tables.Anchors[Child] = Fact.Anchor;
				HasDefs = HasDefs || Tables.IsDefs[Child];
				if(IsHeader && IsHeaderEnd(Syntax, Node, Child)) IsHeader = false;
				Headers[Child] = Headers[Node] != NoNode ? Headers[Node] : IsHeader ? Node : NoNode;
				Pending.push_back(Child);
			}
			// 定義の子が在る親だけの，前置属性（兄弟の装飾子等）の見出への所属替
			if(HasDefs) {
				for(const NodeId Child : Children) {
					if(!Tables.IsDefs[Child] || Headers[Child] != NoNode) continue;
					for(
						NodeId Prefix = SignatureExtractor::GetLeadOf(Syntax, Child);
						Prefix != Child;
						Prefix = Syntax.GetNextNamedSibling(Prefix)
					) Headers[Prefix] = Child;
				}
			}
		}
		// 見出に属さないアンカーだけの補完（同じ位置は後に記録した定義を採る）
		for(std::vector<std::pair<NodeId, NodeId>>::const_reverse_iterator Site = Sites.rbegin(); Site != Sites.rend(); ++Site) {
			if(Work) ++*Work;
			if(Headers[Site->first] == NoNode) Headers[Site->first] = Site->second;
		}
		Tables.Defs = std::move(Headers);
		// ノード毎の表の返戻
		return Tables;
	}

	/**
	 * アンカーのドキュメントの説明対象の取得関数（整形仕様 7.4）
	 * ドキュメントを同期する定義（其のアンカー・ラッパーと見出の中のノードを含む）と，定義の本体の外で型・メンバ・最上位の宣言に所属するアンカーをドキュメントの位置とし，ラッパーは包む宣言と同じ対象とする
	 * 計算量：宣言・ラッパー・祖先・先行の属性・型の判定で読むノード・辺の延数 A と語の照合長の和 B に対し O(A + B + 1)
	 * @param Syntax 読取専用の構文
	 * @param Tables 説明対象を求めるノード毎の表
	 * 制約：Anchor < Syntax.NodeCount()，Tables は同じ構文の全ノードを覆う
	 * @param Anchor アンカー
	 * @return 説明対象の宣言（ドキュメントの位置でなければ無い）
	 * @throws FormatLimitExceeded 構文の照会・走査が作業予算を超える場合
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	NodeId DocCommentReader::GetDocTargetOf(const SyntaxModel &Syntax, const TargetTables &Tables, const NodeId Anchor) {
		// 未展開のトークン列を，囲む宣言の見出又は本体として解釈しない事の返戻
		if(Syntax.GetNode(Anchor).Facts.TokenSpacing != TokenSpacingMode::None) return NoNode;
		// アンカーの所属する定義の取得
		NodeId Target = Tables.Defs[Anchor];
		if(Target == NoNode) {
			// 本体の中でもドキュメントを持てるドキュメントを同期する定義
			if(Tables.IsDefs[Anchor]) Target = Anchor;
			else {
				Target = Anchor;
				// 先行する属性の後の宣言又は宣言でない並びの先頭・宣言を包む式文（TypeScript の名前空間）の最初の項目への移動
				while(
					Syntax.Is(Target, Role::AttachPrefix) ||
					Syntax.Is(Target, Role::DocHostSeq) && !Syntax.Is(Target, Role::DocumentableDecl) && !IsTypeMemberSeq(Syntax, Target) ||
					Syntax.Is(Target, Role::ExprStmt) && Syntax.Is(Syntax.GetNamedChild(Target, 0), Role::DocumentableDecl)
				) Target = Syntax.Is(Target, Role::AttachPrefix) ? Syntax.GetNextNamedSibling(Target) : Syntax.GetNamedChild(Target, 0);
				// 項目の無いアンカーと，定義の本体の中のローカルの型でない宣言はドキュメントの位置でない
				if(Target != NoNode && Tables.IsInDefBody[Target] && !IsTypeDecl(Syntax, Target)) Target = NoNode;
				// アンカー自身から所属する宣言
				else if(Target != NoNode) {
					const NodeId Parent = Syntax.GetParentOf(Target);
					Target = GetDeclTargetOf(Syntax, Target, true, Parent == NoNode ? NoNode : Tables.Decls[Parent]);
				}
			}
		}
		while(Target != NoNode) {
			const NodeId Parent = Syntax.GetParentOf(Target);
			if(!Syntax.Is(Parent, Role::DocWrapper)) break;
			Target = Parent;
		}
		// 説明対象の返戻
		return Target;
	}

	/**
	 * 確定した配置でノードが行の最初の内容から始まるかの判定関数（整形仕様 7.6, 8.1）
	 * 出力に現れないノードは行の途中に置かれない為に行の最初とする
	 * @param Code 変更不能なコードの出力
	 * 制約：Node < Code.GetAnchors().Nodes.size()
	 * @param Node ノード
	 * @return 出力に現れないノードか，ノードの始まりが其の行のインデントの直後なら `true`
	 */
	bool DocCommentReader::IsAtLineStart(const LaidOutCode &Code, const NodeId Node) {
		// 出力のアンカーと対象ノードの範囲
		const AnchorPositionMap &Anchors = Code.GetAnchors();
		const NodeSpan &Span = Anchors.Nodes[Node];
		// 出力に現れないノードか，始まりが行のインデントの直後かの返戻
		return !Span.IsPlaced || Span.Start == Code.GetFragments()[Span.StartFragment].Start + Anchors.FragmentTabs[Span.StartFragment];
	}

	/**
	 * ドキュメントへ統合し得る独立コメント又は同期する既存ドキュメントかの判定関数
	 * 行コメントにすると別の記法・登録形に為る為に入力の囲みの儘とする埋込ドキュメント（整形仕様 7.6）は，ドキュメントの行へ統合しない
	 * @param Comment コメントの記録
	 * @return ディレクティブコメント・埋込ドキュメントを除く独立コメント又は既存ドキュメントなら true
	 */
	bool DocCommentReader::IsMergeable(const CommentRecord &Comment) {
		// 説明を統合する独立コメント又は同期するドキュメントで，埋込記法でないかの返戻
		return Comment.Role == CommentRole::Presentation && (Comment.Kind == CommentKind::OwnLine || IsDoc(Comment)) &&
		CommentRepository::GetOutputNotationOf(Comment) != CommentNotation::RubyEmbeddedDocument;
	}

	/**
	 * 記録の出力がドキュメントかの判定関数
	 * 計算量：本文の長さ B に対し O(B)
	 * @param Record 投影済のコメント
	 * @return 囲み・行形式のドキュメントか，godoc がドキュメントとする Go のブロックコメントなら `true`
	 * @throws FormatLimitExceeded 記法の定義が無い場合
	 */
	bool DocCommentReader::IsDoc(const CommentRecord &Record) {
		// 囲み・行形式のドキュメントか，一般の囲みをドキュメントに使う文法の囲みかの返戻
		const NotationRules &Rules = CommentRepository::GetRulesOf(Record);
		const std::string_view Marker = GetDocLineMarker(Record);
		// 通常記法と共通の行末・文中コメントを除いたドキュメントの返戻
		return IsDocBlock(Record) || !Marker.empty() && (Record.Kind == CommentKind::OwnLine || Marker != GetNewDocMarker(Rules)) ||
		Record.Kind == CommentKind::OwnLine && !Record.IsEmbeddedEdge &&
		CommentRepository::GetOutputNotationOf(Record) == CommentNotation::Block && Rules.GetDocFormOf(CommentNotation::Block);
	}

	/**
	 * 通常コメントと区別する独立したドキュメントかの判定関数
	 * 計算量：本文のテキスト長 B に対し O(B)
	 * @param Rules 投影済の記法
	 * @param Comment 投影済のコメント
	 * @return 囲みのドキュメント又は通常コメントと異なる記号の行ドキュメントなら true
	 * @throws FormatLimitExceeded 記法の定義が無い場合
	 */
	bool DocCommentReader::IsDedicatedDoc(const NotationRules &Rules, const CommentRecord &Comment) {
		// 独立した説明以外の返戻
		if(Comment.Kind != CommentKind::OwnLine || Comment.Role == CommentRole::Directive) return false;
		const std::string_view Marker = GetDocLineMarker(Comment);
		// 通常コメントと区別するドキュメントかの返戻
		return IsDocBlock(Comment) || !Marker.empty() && Marker != GetNewDocMarker(Rules);
	}

	/**
	 * ドキュメントの論理行の範囲の訪問関数
	 * 計算量：記録のテキスト長 L と訪問の総費用 Q に対し O(L + Q)
	 * @param Record 本文を持つドキュメントの記録
	 * @param Write 論理行の開始と長さを受け取る処理
	 * @throws std::bad_alloc 訪問先のメモリを確保出来ない場合
	 * @throws std::length_error 訪問先の要素数が格納先の上限を超える場合
	 */
	template<class Append> void DocCommentReader::ForLogicalLines(const CommentRecord &Record, Append &&Write) {
		const std::string_view Text = CommentRepository::GetOutputTextOf(Record);
		const bool IsBlock = CommentRepository::IsBlockNotation(CommentRepository::GetOutputNotationOf(Record));
		bool HasLines = false;
		for(size_t Cursor = 0;;) {
			const size_t End = std::min(Text.find('\n', Cursor), Text.size());
			// ブロックの先頭の空行と最後の空行だけを除く
			if((!IsBlock || HasLines || End > Cursor) && (!IsBlock || End > Cursor || End < Text.size())) {
				Write(Cursor, End - Cursor);
				HasLines = true;
			}
			if(End == Text.size()) break;
			Cursor = End + 1;
		}
		// 終了
		return;
	}

	/**
	 * 同じ論理行の連続範囲をコピーせずに取得する関数
	 * 計算量：囲みの先頭の空行数 E に対し O(E + 1)，其の他の記法は O(1)，追加メモリ O(1)
	 * @param Record 本文を持つドキュメントの記録
	 * @param Work 範囲の端で読んだバイト数の計上先（無ければ計上しない）
	 * @return 記法に従う論理行範囲の参照（ブロックの開始・終端の空行だけを除き，本文の寿命と世代に従う）
	 */
	std::string_view DocCommentReader::GetLogicalText(const CommentRecord &Record, size_t *const Work) {
		const std::string_view Text = CommentRepository::GetOutputTextOf(Record);
		// 行形式と埋込ドキュメントは全範囲を共有し，論理行毎に本文を探し直さない
		if(!CommentRepository::IsBlockNotation(CommentRepository::GetOutputNotationOf(Record))) return Text;
		// 囲みの最初の内容と最後の空行だけで連続範囲を確定
		size_t First = 0;
		while(First < Text.size()) {
			if(Work) ++*Work;
			if(Text[First] != '\n') break;
			++First;
		}
		// 内容を持たない囲みは元の本文末尾の空範囲を共有
		if(First == Text.size()) return Text.substr(First);
		if(Work) ++*Work;
		const size_t End = Text.size() - (Text.back() == '\n');
		// 中間の空行を含む元の本文の範囲の返戻
		return Text.substr(First, End - First);
	}

	/**
	 * ドキュメントの論理行を格納先へ直接追加する関数
	 * 計算量：記録のテキスト長 L に対し O(L)
	 * @param Record 本文を持つドキュメントの記録
	 * @param Lines 開始・終端記号の空行を除いた論理行の追加先
	 * @throws std::bad_alloc 論理行のメモリを確保出来ない場合
	 * @throws std::length_error 論理行の要素数が格納先の上限を超える場合
	 */
	void DocCommentReader::AppendLogicalLines(const CommentRecord &Record, std::vector<std::string> &Lines) {
		const std::string_view Text = CommentRepository::GetOutputTextOf(Record);
		ForLogicalLines(
			Record,
			[&](const size_t Begin, const size_t Len) -> void {
				Lines.emplace_back(Text.substr(Begin, Len));
				// 終了
				return;
			}
		);
		// 終了
		return;
	}

	/**
	 * 論理行の範囲を共有の列へ追記する関数
	 * 計算量：記録のテキスト長 L に対し O(L)
	 * @param Record 本文を持つドキュメントの記録
	 * @param Out 論理行の開始と長さを追記する列
	 * @throws std::bad_alloc 論理行の範囲のメモリを確保出来ない場合
	 * @throws std::length_error 論理行の要素数が格納先の上限を超える場合
	 */
	void DocCommentReader::AppendLogicalLineRanges(const CommentRecord &Record, std::vector<std::pair<size_t, size_t>> &Out) {
		const std::string_view Text = CommentRepository::GetOutputTextOf(Record);
		ForLogicalLines(
			Record,
			[&](const size_t Begin, const size_t Len) -> void {
				if(Text.substr(Begin, Len).find_first_not_of(" \t\r") != std::string_view::npos) Out.emplace_back(Begin, Len);
				// 終了
				return;
			}
		);
		// 終了
		return;
	}

	/**
	 * 共有する本文構造からドキュメントの論理行のコード判定を取り出す関数
	 * 計算量：本文長 B，コード要素名長の和 K に対し O(B(K + 1))
	 * @param Record 最終テキストから投影したドキュメントの記録
	 * @param Protected 同じ記録の共有構造の保護マスク
	 * @param HasUnclosedCode 同じグループが閉じないコード領域を持つか
	 * @return 開始・終端の空の論理行を除いたコード判定と保護範囲（閉じないグループは無）
	 * @throws std::bad_alloc 論理行の判定のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	std::optional<DocReading> DocCommentReader::DocCodeStarts(
		const CommentRecord &Record,
		const std::span<const unsigned char> Protected,
		const bool HasUnclosedCode
	) {
		// 閉じないグループの返戻
		if(HasUnclosedCode) return std::nullopt;
		const std::string_view Text = CommentRepository::GetOutputTextOf(Record);
		const bool IsBlock = CommentRepository::IsBlockNotation(CommentRepository::GetOutputNotationOf(Record));
		const NotationRules &Rules = CommentRepository::GetRulesOf(Record);
		const bool IsXmlDoc = VocabularyOf(Rules.Vocabulary, DocDescription::Other).Form == DocItemForm::XmlElement;
		DocReading Result;
		for(size_t Cursor = 0;;) {
			const size_t End = std::min(Text.find('\n', Cursor), Text.size());
			if(!IsBlock || !Result.Code.empty() || End > Cursor) {
				Result.Protected.emplace_back(Protected.begin() + Cursor, Protected.begin() + End);
				Result.Code.push_back(
					IsCodeLine(
						IsXmlDoc,
						Text.substr(Cursor, End - Cursor),
						Protected.subspan(Cursor, End - Cursor),
						Rules.CodeExamples.CodeElements
					)
				);
			}
			if(End == Text.size()) break;
			Cursor = End + 1;
		}
		if(IsBlock && Text.ends_with('\n') && !Result.Code.empty()) {
			Result.Code.pop_back();
			Result.Protected.pop_back();
		}
		// 論理行毎のコード判定の返戻
		return Result;
	}

	/**
	 * ドキュメントの概要の有無の判定関数
	 * 計算量：項目数 M，概要の連結後の長さ B，関数名の長さ N に対し XML は O(M)，其の他は O(M + B + N)
	 * @param Rules 投影済の記法
	 * @param Lines ドキュメントの論理行列（記号除去済）
	 * 制約：各項目は 0 ≤ Begin ≤ End ≤ Lines.size()
	 * @param Items ドキュメントの項目
	 * @param Name 関数名（godoc の書出の名前は概要に数えない）
	 * @param Work 概要の照合で訪問した項目と行のバイト数の上界（無ければ計上しない）
	 * @return 概要の本文（先頭の地の文，XML のドキュメントの `<summary>`，`@brief` 等のタグの後）が有れば true
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	bool DocCommentReader::HasSummary(
		const NotationRules &Rules,
		const std::vector<std::string> &Lines,
		const std::vector<DocItem> &Items,
		const std::string_view Name,
		size_t *const Work
	) {
		const DocVocabularyEntry &Summary = VocabularyOf(Rules.Vocabulary, DocDescription::Summary);
		const bool HasTag = VocabularyOf(Rules.Vocabulary, DocDescription::Other).Form == DocItemForm::Tag;
		// XML の概要も項目の内容の判定を共有し，属性付・自己終了の外側のタグを説明に数えない
		if(Summary.Form == DocItemForm::XmlElement) {
			for(const DocItem &Item : Items) {
				if(Work) ++*Work;
				// 解析済の概要の内容が在る事の返戻
				if(Item.Kind == DocItemKind::Summary && Item.HasXmlDescription) return true;
			}
			// 概要の本文が無い事の返戻
			return false;
		}
		// タグの概要は，連結した空白を除く本文の有無だけを参照する
		if(Summary.Policy != DocStylePolicy::NameSummary) {
			for(const DocItem &Item : Items) {
				if(Work) ++*Work;
				if(Item.Kind != DocItemKind::Summary) continue;
				const DocItem &Reading = Item.ReadingItem ? *Item.ReadingItem : Item;
				const std::vector<std::string> &Text = Item.ReadingLines ? *Item.ReadingLines : Lines;
				for(size_t Idx = Reading.Begin; Idx < Reading.End; ++Idx) {
					std::string_view Line = GetLineOf(Reading, Text, Idx);
					if(Work) *Work += Line.size() + 1;
					if(Idx == Reading.Begin && HasTag) ConsumeDocItemTag(Rules, DocDescription::Summary, Line);
					// 概要の本文が在る事の返戻
					if(Line.find_first_not_of(" \t") != std::string_view::npos) return true;
				}
			}
			// 概要の本文が無い事の返戻
			return false;
		}
		// godoc の書出は連結した範囲を照合する
		std::string Text;
		for(const DocItem &Item : Items) {
			if(Work) ++*Work;
			if(Item.Kind != DocItemKind::Summary) continue;
			for(size_t Idx = Item.Begin; Idx < Item.End; ++Idx) {
				std::string_view Line = GetLineOf(Item, Lines, Idx);
				if(Work) *Work += Line.size() + 1;
				// 概要のタグの語の除去
				if(Idx == Item.Begin && VocabularyOf(Rules.Vocabulary, DocDescription::Other).Form == DocItemForm::Tag) {
					ConsumeDocItemTag(Rules, DocDescription::Summary, Line);
				}
				Text.append(Line).push_back(' ');
			}
		}
		std::string_view View = Text;
		SkipSpace(View);
		// godoc の書出の名前の除去
		if(
			VocabularyOf(Rules.Vocabulary, DocDescription::Summary).Policy == DocStylePolicy::NameSummary && !Name.empty() &&
			View.starts_with(Name) &&
			(View.size() == Name.size() || !std::isalnum(static_cast<unsigned char>(View[Name.size()])) && View[Name.size()] != '_')
		) View.remove_prefix(Name.size());
		// 本文の有無の返戻
		return View.find_first_not_of(" \t") != std::string_view::npos;
	}

	/**
	 * 記録毎のドキュメントの説明対象の取得関数（整形仕様 7.4）
	 * ドキュメントと認識する記録は記法と，移譲済のアンカーの宣言への所属の双方から決め（配置先の行では決めない），ラッパーは包む宣言と同じ対象とする
	 * 配置のドキュメントの判定と，期待コメントと最終テキストの双方からのドキュメントの説明対象の保持の検証（整形仕様 8.1）が同じ規則を用いる
	 * 計算量：`TablesOf` の費用 T，記録数 C，`GetDocTargetOf` の費用 A，出力記法の判定費用 B に対し平均 O(T + C + A + B)
	 * @param Syntax 読取専用の構文
	 * @param Comments アンカーのノード毎のコメント
	 * @param CollectedTables 新しく構築した表の受渡先（無ければ此の呼出だけで使う）
	 * @param Work 読んだ記録と，新しく作った所属表の訪問回数（無ければ計上しない）
	 * @return 記録毎の説明対象（ドキュメントと認識しない記録は無い）
	 * @throws FormatLimitExceeded 読む記録に記法の定義が無い場合，又は構文の照会・走査が作業予算を超える場合
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	std::vector<NodeId> DocCommentReader::GetDocTargets(
		const SyntaxModel &Syntax,
		const CommentRepository &Comments,
		std::optional<DocTargetTables> *const CollectedTables,
		size_t *const Work
	) {
		// 説明対象を求める表（ドキュメントの記法の記録が有る時に初めて作る）
		std::optional<TargetTables> ProjectedTables;
		const TargetTables *Tables = Comments.TargetTables ? &*Comments.TargetTables : nullptr;
		// 共有済のアンカーのインデックスで記録を取り出し，同じアンカーの説明対象を１度だけ求める
		const std::span<const CommentRecord> Records = Comments.GetRecords();
		const NotationRules &Rules = Syntax.GetRules().Notation;
		std::vector<NodeId> Targets(Records.size(), NoNode);
		for(const auto &[Anchor, Indices] : Comments.AnchorIdx) {
			std::optional<NodeId> Target;
			for(const size_t Idx : Indices) {
				if(Work) ++*Work;
				const CommentRecord &Record = Records[Idx];
				if(
					Record.Role != CommentRole::Presentation || Record.Placement.Side == CommentPlacement::Inside ||
					Record.Placement.Side != CommentPlacement::Before && !Rules.IsInnerDoc(Record.Notation) || Record.IsEmbeddedEdge ||
					!Rules.GetDocFormOf(CommentRepository::GetOutputNotationOf(Record)) || Record.Kind != CommentKind::OwnLine && !IsDoc(Record)
				) continue;
				if(!Tables) {
					ProjectedTables = TablesOf(Syntax, Work);
					Tables = &*ProjectedTables;
				}
				if(Rules.IsInnerDoc(Record.Notation)) Targets[Idx] = Tables->Enclosing[Anchor];
				else {
					if(!Target) Target = GetDocTargetOf(Syntax, *Tables, Anchor);
					Targets[Idx] = *Target;
				}
			}
		}
		// 投影は此の構文から作った表を所有者へ保存し，静的解析へ渡す
		if(CollectedTables && ProjectedTables) *CollectedTables = std::move(ProjectedTables);
		// 記録毎の説明対象の返戻
		return Targets;
	}

	/**
	 * 所属確定済の説明を関数の定義へ対応付ける取得関数
	 * 説明対象のラッパーを共通の所属表の定義へ対応付け，同じ表で所属を求める
	 * 計算量：所属表の構築費用 T，記録数 C に対し O(T + C)
	 * @param Syntax 読取専用の構文
	 * @param Comments 同じ構文の所属を確定済のコメント
	 * 制約：説明対象の数は記録数と同じ
	 * @param Work 読んだ記録と，新しく作った所属表の訪問回数（無ければ計上しない）
	 * @return 記録毎の所属する関数の定義（関数を説明しない記録は無い）
	 * @throws FormatLimitExceeded 記法の定義が無い場合，又は構文の照会・走査が作業予算を超える場合
	 * @throws std::bad_alloc 必要な記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 記録・作業表の要素数が格納先の上限を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	std::vector<NodeId> DocCommentReader::GetDocDefinitionTargets(
		const SyntaxModel &Syntax,
		const CommentRepository &Comments,
		size_t *const Work
	) {
		// 同じ構文世代の所属表と定義への対応
		std::optional<TargetTables> ProjectedTables;
		const TargetTables *Tables = Comments.TargetTables ? &*Comments.TargetTables : nullptr;
		const std::span<const CommentRecord> Records = Comments.GetRecords();
		const std::span<const NodeId> Recognized = Comments.GetDocTargets();
		std::vector<NodeId> Targets(Records.size(), NoNode);
		for(const auto &[Anchor, Indices] : Comments.AnchorIdx) for(const size_t Idx : Indices) {
			if(Work) ++*Work;
			if(Recognized[Idx] == NoNode) continue;
			if(!Tables) {
				ProjectedTables = TablesOf(Syntax, Work);
				Tables = &*ProjectedTables;
			}
			const NodeId Target = Recognized[Idx];
			NodeId Def = Tables->Defs[Target];
			if(Def == NoNode && Tables->IsDefs[Target]) Def = Target;
			Targets[Idx] = Def;
		}
		// 関数の定義への対応の返戻
		return Targets;
	}

	/**
	 * 確定した配置でドキュメントとして置く記録毎の説明対象の取得関数（整形仕様 7.6）
	 * 独立行のドキュメントは，説明対象（前置の属性を含む）が出力の行の最初の内容から始まる時だけドキュメントとし，畳んだ本体のメンバ等の前に独立行を持てない物は通常コメントとする
	 * 計算量：記録数 C と，前置の属性の頭を求める時に読む名前付・未命名の先行兄弟の延数 A に対し O(C + A)
	 * @param Code 変更不能なコードの出力
	 * @param Records 記録の並び
	 * 制約：`Targets.size() = Records.size()`，`Targets` は `Code` の構文に対する `GetDocTargets` の結果
	 * 制約：`Code` のアンカーが持つ構文ポインタは `nullptr` でなく，構文は読取中に生存する
	 * @param Targets 記録毎のドキュメントの説明対象（`GetDocTargets` の結果）
	 * @return 記録毎のドキュメントとして置く説明対象（ドキュメントとして置かない記録は無い）
	 * @throws FormatLimitExceeded 構文の照会・走査が作業予算を超える場合
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	std::vector<NodeId> DocCommentReader::PlacedTargets(
		const LaidOutCode &Code,
		const std::span<const CommentRecord> Records,
		std::vector<NodeId> Targets
	) {
		// 確定した配置の構文の取得
		const SyntaxModel &Syntax = *Code.GetAnchors().Syntax;
		for(size_t Idx = 0; Idx < Targets.size(); ++Idx) {
			if(
				Targets[Idx] != NoNode && Records[Idx].Kind == CommentKind::OwnLine && !IsAtLineStart(
					Code,
					CommentRepository::GetRulesOf(Records[Idx]).IsInnerDoc(Records[Idx].Notation) ?
					Records[Idx].Anchor :
					SignatureExtractor::GetLeadOf(Syntax, Targets[Idx])
				)
			) Targets[Idx] = NoNode;
		}
		// 記録毎のドキュメントとして置く説明対象の返戻
		return Targets;
	}
} // namespace Core
