#include "DocCommentSynchronizer.hpp"
#include "CommentRepository.hpp"
#include "ControlFlow.hpp"
#include "SignatureExtractor.hpp"
#include "SyntaxModel.hpp"
#include "SyntaxRole.hpp"
#include "../Util/CommentNormalizer.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Core {
	/**
	 * 同義のタグの規約の綴りへの統一関数（整形仕様 7.6）
	 * ドキュメント形式が同義と定めるタグだけを，採用する綴りへ揃える
	 * Doxygen の同義のタグ：`\param` / `\return` / `@result`
	 * JSDoc の同義のタグ：`@arg` / `@returns`
	 * Javadoc ･ KDoc の同義のタグ：`@exception` 等
	 * 計算量：タグの同義語数 S，行の長さ L に対し O(S L)
	 * @param Rules 投影済の記法
	 * @param Line 項目の最初の論理行（破壊的に書き換えられる）
	 * @throws std::bad_alloc タグの置換先のメモリを確保出来ない場合
	 * @throws std::length_error タグの置換先が格納先の上限を超える場合
	 */
	void DocCommentSynchronizer::NormalizeTagSynonym(const NotationRules &Rules, std::string &Line) {
		// 先頭のタグの同義語の探索
		const size_t Start = std::min(Line.find_first_not_of(" \t"), Line.size());
		for(const DocTagSynonym &Synonym : Rules.DocTagSynonyms) {
			const std::string_view From = Synonym.From, To = Synonym.To;
			if(!DocTagWordLength(Rules, std::string_view(Line).substr(Start), From)) continue;
			Line.replace(Start, From.size(), To);
			// 揃えた後の終了
			return;
		}
		// 終了
		return;
	}

	/**
	 * 仮引数の新しい項目の論理行の生成関数
	 * 計算量：仮引数名と項目記号のバイト数の和 N に対し O(N + 1)
	 * @param Rules Profile が登録した記法と語彙
	 * @param Name 仮引数名
	 * @return 説明を持たない項目の論理行
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	std::string DocCommentSynchronizer::NewParamLine(const NotationRules &Rules, const std::string_view Name) {
		// 仮引数の項目の語彙の取得
		const DocVocabularyEntry &Entry = VocabularyOf(Rules.Vocabulary, DocDescription::Param);
		if(Entry.Form == DocItemForm::List || Entry.Form == DocItemForm::HeadingList) {
			// 単独の仮引数の説明項目の返戻
			return std::string(Entry.Prefix) + std::string(Name) + std::string(Entry.Suffix);
		}
		const DocVocabularyEntry &Form =
		Entry.Form == DocItemForm::XmlElement ? Entry : VocabularyOf(Rules.TagVocabulary, DocDescription::Param);
		// 書式の語彙で囲んだ仮引数の返戻
		return std::string(Form.Prefix) + std::string(Name) + std::string(Form.Suffix);
	}

	/**
	 * 戻値の新しい項目の論理行の取得関数
	 * @param Rules Profile が登録した記法と語彙
	 * @return 説明を持たない項目の論理行（採用するタグの綴り）
	 */
	std::string_view DocCommentSynchronizer::GetNewReturnLine(const NotationRules &Rules) {
		// 戻値の項目の骨組の取得
		const std::string_view Skeleton = VocabularyOf(Rules.Vocabulary, DocDescription::Return).Skeleton;
		// 形式が骨格を持たない時にタグ形式の語彙を使った骨格の返戻
		return Skeleton.empty() ? VocabularyOf(Rules.TagVocabulary, DocDescription::Return).Skeleton : Skeleton;
	}

	/**
	 * 通常の説明のドキュメント形式へのエスケープ関数
	 * 既存ドキュメントのタグ・文字参照は此の経路へ渡さず，通常の本文が項目へ読み替えられる事を防ぐ
	 * 計算量：論理行のバイト数の和 N と行数 L に対し O(N + L)
	 * @param Rules 投影済の記法
	 * @param Previous 共有する本文構造（無ければ新しい本文から判定する）
	 * @param Lines 通常コメントから取った論理行
	 * @return 採用形式で同じ説明を表す論理行
	 * @throws std::bad_alloc テキストと論理行のメモリを確保出来ない場合
	 * @throws std::length_error テキスト又は論理行数が格納先の上限を超える場合
	 */
	std::vector<std::string> DocCommentSynchronizer::EncodeDescription(
		const NotationRules &Rules,
		const CommentStructure *Previous,
		const std::vector<std::string> &Lines
	) {
		std::vector<std::string> Result;
		Result.reserve(Lines.size());
		// 通常コメントでもコード例の保持境界を使い，タグに見えるコードを説明へ変換しない
		std::vector<bool> ProtectedHeads(Lines.size());
		struct Replacement {
			size_t Begin; // 原文の開始位置
			size_t End; // 原文の終端位置
			std::string Text; // 同じ説明を表すテキスト
		};
		std::vector<std::vector<Replacement>> Escapes(Lines.size());
		bool HasReadingLines = false;
		if(Rules.DocTextEscapes.empty() && !Lines.empty()) {
			const std::optional<DocReading> Reading = CodeLineStarts(Rules, Lines, Previous);
			if(!Reading) std::fill(ProtectedHeads.begin(), ProtectedHeads.end(), true);
			else if(Reading->Lines) {
				HasReadingLines = !Rules.DocHeadEscapes.empty() && Rules.ShouldDecodeUnicodeEscapes;
				// Java の論理行の先頭を原文へ戻し，Unicode のエスケープの適格性を変えずにマーカーだけを表す
				if(HasReadingLines) for(size_t Row = 0; Row < Reading->Lines->size(); ++Row) {
					const std::string &Line = (*Reading->Lines)[Row];
					const size_t Head = Line.find_first_not_of(" \t");
					if(Head == std::string::npos || Reading->Code[Row] || Reading->Protected[Row][Head]) continue;
					const char Char = Line[Head];
					const std::vector<DocHeadEscape>::const_iterator Escape = std::ranges::find_if(
						Rules.DocHeadEscapes,
						[Char](const DocHeadEscape &Entry) -> bool {
							// 説明の先頭の置換対象との一致の返戻
							return Entry.From == Char;
						}
					);
					if(Escape == Rules.DocHeadEscapes.end()) continue;
					size_t End = Head + 1;
					if(Escape->ShouldRepeat) while(End < Line.size() && Line[End] == Char) ++End;
					const auto [RawLine, Begin] = Reading->OriginalPositions[Row][Head];
					const auto [EndLine, RawEnd] = Reading->OriginalPositions[Row][End];
					if(RawLine != EndLine) continue;
					std::string Text;
					for(size_t Pos = Head; Pos < End; ++Pos) Text += Escape->To;
					Escapes[RawLine].push_back({ Begin, RawEnd, std::move(Text) });
				}
				std::vector<size_t> Heads;
				Heads.reserve(Lines.size());
				for(const std::string &Line : Lines) Heads.push_back(Line.find_first_not_of(" \t"));
				// 展開後のコードの境界を原文の行へ１度ずつ対応付ける
				for(size_t Row = 0; Row < Reading->Protected.size(); ++Row) for(size_t Pos = 0; Pos < Reading->Protected[Row].size(); ++Pos) {
					if(!Reading->Code[Row] && !Reading->Protected[Row][Pos]) continue;
					const auto [Line, Offset] = Reading->OriginalPositions[Row][Pos];
					if(Offset == Heads[Line]) ProtectedHeads[Line] = true;
				}
			} else for(size_t Row = 0; Row < Lines.size(); ++Row) {
					const size_t Head = Lines[Row].find_first_not_of(" \t");
					ProtectedHeads[Row] = Reading->Code[Row] || Head < Lines[Row].size() && Reading->Protected[Row][Head];
			}
		}
		for(const std::string &Line : Lines) {
			std::string Text;
			// XML のテキストへ変換し，元から在る文字参照も其の綴りを説明として保つ
			if(!Rules.DocTextEscapes.empty()) for(const char Char : Line) {
				const std::vector<std::pair<char, std::string_view>>::const_iterator Escape = std::ranges::find_if(
					Rules.DocTextEscapes,
					[Char](const std::pair<char, std::string_view> &Entry) -> bool {
						// 文字参照へ変える字との一致の返戻
						return Entry.first == Char;
					}
				);
				if(Escape == Rules.DocTextEscapes.end()) Text += Char;
				else Text += Escape->second;
			} else if(HasReadingLines) {
				// 元の残りの Unicode の綴りを保持し，各論理行のマーカーだけを置換する
				size_t Pos = 0;
				for(const Replacement &Escape : Escapes[Result.size()]) {
					Text.append(Line, Pos, Escape.Begin - Pos);
					Text += Escape.Text;
					Pos = Escape.End;
				}
				Text.append(Line, Pos, Line.size() - Pos);
			} else {
				Text = Line;
				const size_t Start = Text.find_first_not_of(" \t");
				if(Start != std::string::npos && !ProtectedHeads[Result.size()]) {
					const char Head = Text[Start];
					const std::vector<DocHeadEscape>::const_iterator Escape = std::ranges::find_if(
						Rules.DocHeadEscapes,
						[Head](const DocHeadEscape &Entry) -> bool {
							// 説明の先頭の置換対象との一致の返戻
							return Entry.From == Head;
						}
					);
					if(Escape != Rules.DocHeadEscapes.end()) {
						size_t End = Start + 1;
						if(Escape->ShouldRepeat) while(End < Text.size() && Text[End] == Head) ++End;
						std::string Escaped;
						for(size_t Pos = Start; Pos < End; ++Pos) Escaped += Escape->To;
						Text.replace(Start, End - Start, Escaped);
					}
				}
			}
			Result.push_back(std::move(Text));
		}
		// エスケープした説明の返戻
		return Result;
	}

	/**
	 * 定義の直前の通常コメントのドキュメントの説明への統合関数（整形仕様 7.4）
	 * ドキュメントより前の物は説明の先頭へ，其れ以外は説明の末尾へ置き，概要を要素で表す形式では概要の要素の内容とする
	 * 置く位置は統合前のドキュメントの構造で決め，統合した本文がタグの様に読める事で順を変えない
	 * 計算量：論理行の長さの和 N に対し O(N)
	 * @param Rules 投影済の記法
	 * @param Previous 共有する本文構造（無ければ新しい本文から判定する）
	 * @param Items 統合前の本文世代の項目
	 * @param Before ドキュメントより前の通常コメントの論理行
	 * @param After ドキュメントより後の通常コメントの論理行
	 * @param Lines ドキュメントの論理行列（記号除去済，ドキュメントが無ければ空，統合した行を加える）
	 * @param GroupStarts 原文のドキュメント毎の最初の論理行（統合した行数に従い移す）
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	void DocCommentSynchronizer::MergeDescription(
		const NotationRules &Rules,
		const CommentStructure *Previous,
		const std::optional<std::vector<DocItem>> &Items,
		const std::vector<std::string> &Before,
		const std::vector<std::string> &After,
		std::vector<std::string> &Lines,
		std::vector<size_t> &GroupStarts
	) {
		// 通常説明だけを採用形式で表し，既存項目とは別の入力として統合する
		const std::vector<std::string> HeadDescription = EncodeDescription(Rules, Previous, Before);
		const std::vector<std::string> TailDescription = EncodeDescription(Rules, Previous, After);
		// 論理行の置換と，其の後に在る原文のドキュメント境界の移動
		const auto Replace =
		[&Lines, &GroupStarts](const size_t At, const size_t End, const std::vector<std::string> &Content) -> void {
			for(size_t &Start : GroupStarts) if(Start && Start >= End && (Start > At || At == End)) {
				Start = Start - End + At + Content.size();
			}
			Lines.erase(Lines.begin() + static_cast<std::ptrdiff_t>(At), Lines.begin() + static_cast<std::ptrdiff_t>(End));
			Lines.insert(Lines.begin() + static_cast<std::ptrdiff_t>(At), Content.begin(), Content.end());
			// 終了
			return;
		};
		// ドキュメント形式の概要の語彙の取得
		const DocVocabularyEntry &Summary = VocabularyOf(Rules.Vocabulary, DocDescription::Summary);
		// 通常の概要への前後の説明の統合（後の説明は最後の概要へ所属させる）
		if(Summary.Form != DocItemForm::XmlElement) {
			size_t End = Lines.size();
			if(Items) for(const DocItem &Tag : *Items) if(Tag.Kind == DocItemKind::Summary) End = Tag.End;
			if(!TailDescription.empty()) Replace(End, End, TailDescription);
			if(!HeadDescription.empty()) Replace(0, 0, HeadDescription);
			// 通常の概要への統合の終了
			return;
		}
		// 最初と最後の概要の選択（行内の複数の要素も，項目の開始位置で区別する）
		const DocItem *First = nullptr, *Last = nullptr;
		if(Items) for(const DocItem &Tag : *Items) {
			if(Tag.Kind != DocItemKind::Summary || Tag.Begin == Tag.End) continue;
			if(!First) First = &Tag;
			Last = &Tag;
		}
		// １つの概要の内容へ説明を加え，既存の属性と同じ行の他の要素を保つ
		const auto Merge = [&Lines, &Summary, &Replace](
			const DocItem &Tag,
			const std::vector<std::string> &HeadLines,
			const std::vector<std::string> &TailLines
		) -> void {
			// 要素を持たない既存の概要も同じ位置で概要の要素に収め，後の説明を前の要素へ戻さない
			if(Tag.OpenLine == std::string_view::npos) {
				std::vector<std::string> Content = HeadLines;
				for(size_t Idx = Tag.Begin; Idx < Tag.End; ++Idx) Content.emplace_back(GetLineOf(Tag, Lines, Idx));
				Content.insert(Content.end(), TailLines.begin(), TailLines.end());
				Content.front().insert(0, Summary.Prefix);
				Content.back() += Summary.Suffix;
				Replace(Tag.Begin, Tag.End, Content);
				// 既存の概要を要素として統合した処理の終了
				return;
			}
			// 項目の読取で確定した開始タグの終端と自己終了を共有する
			const size_t OpenLine = Tag.OpenLine, Open = Tag.OpenOffset;
			const bool IsEmpty = Tag.IsSelfClosing;
			size_t CloseLine = OpenLine, Close = Open + 1;
			if(!IsEmpty) {
				// 閉位置を持たない項目は変更しない事の返戻
				if(Tag.CloseLine == std::string_view::npos) return;
				// 項目の読取で確定した閉じを共有し，CDATA や XML コメント内の綴りを探し直さない
				CloseLine = Tag.CloseLine;
				Close = Tag.CloseOffset;
			}
			std::string Head = Lines[OpenLine].substr(0, Open + 1);
			std::string Tail = Lines[CloseLine].substr(Close);
			if(IsEmpty) {
				Head.erase(Head.size() - 2, 1);
				Tail.insert(0, Summary.Suffix);
			}
			std::vector<std::string> Content = HeadLines;
			if(!IsEmpty) for(size_t Idx = OpenLine; Idx <= CloseLine; ++Idx) {
				const size_t From = Idx == OpenLine ? Open + 1 : 0, To = Idx == CloseLine ? Close : Lines[Idx].size();
				std::string Part = Lines[Idx].substr(From, To - From);
				if((Idx == OpenLine || Idx == CloseLine) && Part.find_first_not_of(" \t") == std::string::npos) continue;
				Content.push_back(std::move(Part));
			}
			Content.insert(Content.end(), TailLines.begin(), TailLines.end());
			Content.front().insert(0, Head);
			Content.back() += Tail;
			Replace(OpenLine, CloseLine + 1, Content);
			// 終了
			return;
		};
		if(First) {
			const std::vector<std::string> Empty;
			// 同じ要素なら１度で統合し，別の要素なら後から変えて最初の開始位置を保つ
			if(First == Last) Merge(*First, HeadDescription, TailDescription);
			else {
				if(!TailDescription.empty()) Merge(*Last, Empty, TailDescription);
				if(!HeadDescription.empty()) Merge(*First, HeadDescription, Empty);
			}
		} else {
			// 概要が無いドキュメントへ説明の要素を補う
			std::vector<std::string> Content = HeadDescription;
			Content.insert(Content.end(), TailDescription.begin(), TailDescription.end());
			Content.front().insert(0, Summary.Prefix);
			Content.back() += Summary.Suffix;
			Replace(0, 0, Content);
		}
		// 終了
		return;
	}

	/**
	 * 既存ドキュメント論理行とシグネチャからの論理行整合関数（実シグネチャと照合し不足の仮引数・戻値の項目を補正）
	 * 説明を持つ項目は，対応する仮引数・戻値が無くても削除せず静的解析へ委ねる
	 * 計算量：ドキュメントの項目数 I，型を含むシグネチャの仮引数数 P，論理行と名前の長さの和 L に対し平均 O(I + P + L)
	 * @param Rules 投影済の記法
	 * @param Existing 既存ドキュメントの論理行列（記号除去済）
	 * @param FunctionSig 実シグネチャ（引数名列・戻値有無）
	 * @param Items 同じ本文世代の項目
	 * @param Completion 他の配置位置にも無い補完対象（無ければ実シグネチャ全体）
	 * @return 補正後の論理行列（概要より前の項目・概要・型仮引数・値仮引数・戻値・其の他の順）
	 * @throws std::bad_alloc 論理行又はインデックスの領域を確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	std::vector<std::string> DocCommentSynchronizer::ReconcileLogicalLines(
		const NotationRules &Rules,
		const std::vector<std::string> &Existing,
		const Signature &FunctionSig,
		const std::optional<std::vector<DocItem>> &Items,
		const Signature *const Completion
	) {
		// ドキュメントの項目の書式と概要の語彙
		const DocVocabularyEntry &Summary = VocabularyOf(Rules.Vocabulary, DocDescription::Summary);
		// 同期出来ないドキュメントを保つ事の返戻
		if(!Items) return Existing;
		const DocItemForm OtherForm = VocabularyOf(Rules.Vocabulary, DocDescription::Other).Form;
		// 項目の分類先
		const DocItem *ParamList = nullptr;
		std::vector<const DocItem *> Params, Returns, Other, FollowingLeads;
		const bool HasSummary = std::ranges::any_of(
			*Items,
			[](const DocItem &Item) -> bool {
				// 空でない概要が在るかの返戻
				return Item.Kind == DocItemKind::Summary && Item.Begin != Item.End;
			}
		);
		std::vector<std::string> Result;
		// 元の説明行と不足の項目を１度で収めれる容量の確保
		Result.reserve(Existing.size() + FunctionSig.Params.size() + 4); // 概要・戻値等の項目用の余裕
		// 原文を共有し，採用する項目の転写先でだけタグの同義語を統一するローカル関数
		const auto AppendLines =
		[&Rules, &Existing, OtherForm](const DocItem &Item, const size_t End, std::vector<std::string> &Output) -> void {
			// 同じ本文世代の行内範囲に従う転写
			const size_t Begin = Output.size();
			for(size_t Idx = Item.Begin; Idx < End; ++Idx) {
				const std::string_view Line = GetLineOf(Item, Existing, Idx);
				if(Line.find_first_not_of(" \t\r") != std::string_view::npos) Output.emplace_back(Line);
			}
			if(OtherForm == DocItemForm::Tag && Item.Kind != DocItemKind::Lead && Begin < Output.size()) {
				NormalizeTagSynonym(Rules, Output[Begin]);
			}
			// 終了
			return;
		};
		// 既存項目の全論理行を転写するローカル関数
		const auto Append = [&Result, &AppendLines](const DocItem &Item) -> void {
			AppendLines(Item, Item.End, Result);
			// 終了
			return;
		};
		// 概要と仮引数より前の項目の転写，其の他の項目の分類
		for(const DocItem &Item : *Items) switch(Item.Kind) {
		case DocItemKind::Summary:
			Append(Item);
			break;
		case DocItemKind::Lead:
			if(HasSummary && Summary.Form != DocItemForm::XmlElement) FollowingLeads.push_back(&Item);
			else Append(Item);
			break;
		case DocItemKind::Param:
		case DocItemKind::TypeParam:
			Params.push_back(&Item);
			break;
		case DocItemKind::Return:
			Returns.push_back(&Item);
			break;
		case DocItemKind::ParamList:
			if(!ParamList) ParamList = &Item;
			else Other.push_back(&Item);
			break;
		case DocItemKind::NestedSig:
		case DocItemKind::Other:
			Other.push_back(&Item);
			break;
		}
		// 説明が先行するタグの後へ，別のドキュメントの概要を置いて其のタグの説明にしない
		for(const DocItem *const Item : FollowingLeads) Append(*Item);
		// 概要の枠（XML のドキュメントは概要の要素）
		if(Result.empty() && Summary.Form == DocItemForm::XmlElement) Result.emplace_back(Summary.Skeleton);
		else if(Result.empty()) Result.emplace_back();
		// Swift の `- Params:` の箇条から仮引数毎の独立した説明項目への書換
		// 本文にインデントを持たず見出の下のネストを表せない事に依る，本文を持たない見出の除去
		const DocVocabularyEntry &ParamEntry = VocabularyOf(Rules.Vocabulary, DocDescription::Param);
		const bool ShouldRelabelParams = ParamList && ParamEntry.Form == DocItemForm::List;
		if(ShouldRelabelParams) {
			std::string_view Rest;
			Callout(Existing[ParamList->Begin], Rest);
			if(ParamList->End == ParamList->Begin + 1 && Rest.find_first_not_of(" \t:") == std::string_view::npos) ParamList = nullptr;
		}
		// 仮引数の項目の並び
		std::vector<std::string> ParamLines;
		std::vector<bool> IsPlaced(Params.size(), false);
		// 仮引数の項目を原文の行順に転写するローカル関数
		const auto AppendParam = [&](const size_t Idx) -> void {
			// 仮引数の項目の取得
			const DocItem &Item = *Params[Idx];
			const size_t Begin = ParamLines.size();
			AppendLines(Item, Item.End, ParamLines);
			std::string_view Rest;
			if(ShouldRelabelParams && Callout(Existing[Item.Begin], Rest) == Item.Name) {
				std::string_view Body = std::string_view(Existing[Item.Begin]).substr(2);
				SkipSpace(Body);
				ParamLines[Begin] = std::string(ParamEntry.Prefix) + std::string(Body);
			}
			IsPlaced[Idx] = true;
			// 終了
			return;
		};
		// シグネチャの名前毎に項目列を引く為の項目名の１度のインデックス化
		using ParamItems = std::unordered_map<std::string_view, std::vector<size_t>>;
		ParamItems CoveredItems, MemberItems, TypeItems, CompletionItems;
		// 本文の説明名と仮引数全体の対応から，導入名毎の照合先を１度だけ定める
		std::unordered_map<uint32_t, std::string_view> WholeParams;
		for(const DocItem *const Item : Params) if(Item->ParamOrdinal) WholeParams.try_emplace(Item->ParamOrdinal, Item->Name);
		std::unordered_map<std::string_view, std::string_view> ParamAliases;
		if(!WholeParams.empty()) for(size_t Idx = 0; Idx < FunctionSig.ParamShapes.size(); ++Idx) {
			const ParamShape &Shape = FunctionSig.ParamShapes[Idx];
			const std::unordered_map<uint32_t, std::string_view>::const_iterator Whole = WholeParams.find(Shape.Ordinal);
			if(Whole == WholeParams.end()) continue;
			const size_t End =
			Idx + 1 < FunctionSig.ParamShapes.size() ? FunctionSig.ParamShapes[Idx + 1].NameBegin : FunctionSig.Params.size();
			for(size_t Name = Shape.NameBegin; Name < End; ++Name) ParamAliases.emplace(FunctionSig.Params[Name], Whole->second);
		}
		for(const std::string &Name : FunctionSig.Params) {
			const std::unordered_map<std::string_view, std::string_view>::const_iterator Alias = ParamAliases.find(Name);
			CoveredItems.try_emplace(Alias == ParamAliases.end() ? std::string_view(Name) : Alias->second);
		}
		for(const std::string &Name : FunctionSig.TypeParams) TypeItems.try_emplace(Name);
		if(Completion) for(const std::string &Name : Completion->Params) CompletionItems.try_emplace(Name);
		for(size_t Idx = 0; Idx < Params.size(); ++Idx) {
			if(Params[Idx]->Kind == DocItemKind::Param) {
				for(const std::string_view Part : Params[Idx]->Names) {
					if(const ParamItems::iterator Covered = CoveredItems.find(Part); Covered != CoveredItems.end()) Covered->second.push_back(Idx);
				}
				if(!Params[Idx]->Parent.empty()) MemberItems[Params[Idx]->Parent].push_back(Idx);
			}
			// `<名前>` と裸の名前を同じ型仮引数へ対応付け，同名項目の原文順を保存
			for(std::string_view TypeName : Params[Idx]->Names) {
				const bool IsBracketed = TypeName.starts_with('<') && TypeName.ends_with('>');
				if(IsBracketed) TypeName = TypeName.substr(1, TypeName.size() - 2);
				// 通常の仮引数と同名の裸のタグは値の説明を優先
				if(Params[Idx]->Kind != DocItemKind::TypeParam && !IsBracketed && CoveredItems.contains(TypeName)) continue;
				if(const ParamItems::iterator Type = TypeItems.find(TypeName); Type != TypeItems.end()) Type->second.push_back(Idx);
			}
		}
		// シグネチャ順の型仮引数の項目（説明を持つ同名項目も其の対象へ纏める）
		for(const std::string &Name : FunctionSig.TypeParams) for(const size_t Idx : TypeItems.at(Name)) if(!IsPlaced[Idx]) {
			AppendParam(Idx);
		}
		// シグネチャと対応しない囲んだ名前の項目は，既存の扱いと原文順を保持
		for(size_t Idx = 0; Idx < Params.size(); ++Idx) {
			if(!IsPlaced[Idx] && (Params[Idx]->Name.starts_with('<') || Params[Idx]->Kind == DocItemKind::TypeParam)) AppendParam(Idx);
		}
		// シグネチャ順の仮引数の項目（同じ仮引数の項目は全て，メンバの項目は親の後）
		std::unordered_set<std::string_view> PlacedWholeParams;
		for(const std::string &Name : FunctionSig.Params) {
			const std::unordered_map<std::string_view, std::string_view>::const_iterator Alias = ParamAliases.find(Name);
			const std::string_view DocName = Alias == ParamAliases.end() ? std::string_view(Name) : Alias->second;
			if(Alias != ParamAliases.end() && !PlacedWholeParams.insert(DocName).second) continue;
			const std::vector<size_t> &Covered = CoveredItems.at(DocName);
			const ParamItems::const_iterator Members = MemberItems.find(DocName);
			for(const size_t Idx : Covered) if(!IsPlaced[Idx]) AppendParam(Idx);
			if(Members != MemberItems.end()) for(const size_t Idx : Members->second) if(!IsPlaced[Idx]) AppendParam(Idx);
			if(Covered.empty() && (!Completion || CompletionItems.contains(Name))) ParamLines.push_back(NewParamLine(Rules, Name));
		}
		// 対応する仮引数の無い項目は，説明を持つ物と，照合が不完全な無名の仮引数を持つシグネチャの物だけを残す
		for(size_t Idx = 0; Idx < Params.size(); ++Idx) {
			if(!IsPlaced[Idx] && (FunctionSig.HasUnnamedParam || HasItemDescription(Rules, *Params[Idx], Existing))) AppendParam(Idx);
		}
		// 仮引数の項目の見出（項目が無ければ見出も除く）と項目
		if(!ParamLines.empty()) {
			if(ParamList) Append(*ParamList);
			else if(const std::string_view Header = VocabularyOf(Rules.Vocabulary, DocDescription::ParamList).Skeleton; !Header.empty()) {
				Result.emplace_back(Header);
			}
		}
		for(std::string &Line : ParamLines) Result.push_back(std::move(Line));
		// 呼出結果が未確定なら既存の項目を保ち，結果を持つ場合だけ骨格を補う
		if(FunctionSig.HasReturn || !FunctionSig.Return.HasCallResult.has_value()) {
			for(const DocItem *const Item : Returns) Append(*Item);
			if(Returns.empty() && FunctionSig.HasReturn && (!Completion || Completion->HasReturn)) {
				Result.emplace_back(GetNewReturnLine(Rules));
			}
		} else for(const DocItem *const Item : Returns) if(HasItemDescription(Rules, *Item, Existing)) Append(*Item);
		// 其の他の項目の転写
		for(const DocItem *const Item : Other) Append(*Item);
		// 補正後の論理行列の返戻
		return Result;
	}

	/**
	 * 既存項目の補完済の対象の記録関数
	 * 計算量：仮引数名の総バイト数 B と項目数 I に対し，ハッシュ操作の期待値で O(B + I)
	 * @param Items 同期で読んだ既存項目
	 * @param Coverage 区間間で共有する補完済の対象（名前を所有し，項目の寿命に依存しない）
	 * @throws std::bad_alloc 仮引数名と集合のメモリを確保出来ない場合
	 * @throws std::length_error 名前と集合の要素数が格納先の上限を超える場合
	 */
	void DocCommentSynchronizer::CoverItems(const std::optional<std::vector<DocItem>> &Items, DocCoverage &Coverage) {
		// シグネチャの整合に使う項目から補完済の対象を共有
		if(Items) for(const DocItem &Item : *Items) {
			if(Item.Kind == DocItemKind::Return) Coverage.HasReturn = true;
			if(Item.Kind == DocItemKind::Param) for(const std::string_view Name : Item.Names) Coverage.Names.emplace(Name);
		}
		// 終了
		return;
	}

	/**
	 * 本文世代毎の項目を共有する説明の統合・整合関数
	 * 計算量：本文長 B，構造判定費用 I，項目整合費用 R に対し O(B + I + R)
	 * @param Rules 投影済の記法
	 * @param Previous 共有する本文世代の構造（無ければ初回）
	 * @param FunctionSig 実シグネチャ
	 * @param Lines 統合前のドキュメントの論理行
	 * @param Before ドキュメントより前の説明
	 * @param After ドキュメントより後の説明
	 * @param GroupStarts 原文のドキュメント毎の最初の論理行（統合した行数に従い移す）
	 * @param Completion 他の配置位置にも無い補完対象（無ければ実シグネチャ全体）
	 * @param Coverage 区間間で共有する補完済の対象（無ければ記録しない）
	 * @return 統合・整合後の論理行
	 * @throws std::bad_alloc 本文と項目のメモリを確保出来ない場合
	 * @throws FormatLimitExceeded 記法の定義が無い場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 */
	std::vector<std::string> DocCommentSynchronizer::SynchronizeLines(
		const NotationRules &Rules,
		const CommentStructure *Previous,
		const Signature &FunctionSig,
		std::vector<std::string> &&Lines,
		const std::vector<std::string> &Before,
		const std::vector<std::string> &After,
		std::vector<size_t> GroupStarts,
		const Signature *const Completion,
		DocCoverage *const Coverage
	) {
		// 宣言名で始めるドキュメント形式の概要の補完
		if(VocabularyOf(Rules.Vocabulary, DocDescription::Summary).Policy == DocStylePolicy::NameSummary) {
			Lines.insert(Lines.begin(), Before.begin(), Before.end());
			Lines.insert(Lines.end(), After.begin(), After.end());
			// 項目を持たない名前の概要の返戻
			return std::ranges::all_of(
				Lines,
				[](const std::string &Line) -> bool {
					// 内容の無い論理行かの返戻
					return Line.find_first_not_of(" \t\r") == std::string::npos;
				}
			) ? std::vector<std::string>{ FunctionSig.Name } : std::move(Lines);
		}
		// 本文の統合に依らず，同じシグネチャのインデックスを共有する
		DocParamIdx ParamIdx;
		std::optional<std::vector<DocItem>> Items =
		ParseItems(Rules, Lines, FunctionSig, CodeLineStarts(Rules, Lines, Previous, GroupStarts), ParamIdx, GroupStarts);
		if(!Before.empty() || !After.empty()) {
			MergeDescription(Rules, Previous, Items, Before, After, Lines, GroupStarts);
			// 通常説明をエスケープしてから，新しい本文世代の項目を読む
			Items = ParseItems(Rules, Lines, FunctionSig, CodeLineStarts(Rules, Lines, Previous, GroupStarts), ParamIdx, GroupStarts);
		}
		// 整合で使う同じ項目の，区間間の補完判定への共有
		if(Coverage) CoverItems(Items, *Coverage);
		// 同じ項目を使った整合結果の返戻
		return ReconcileLogicalLines(Rules, Lines, FunctionSig, Items, Completion);
	}

	/**
	 * 埋込ドキュメントの記法を要する後続文の所属の分離関数
	 * 読取専用の文境界と共有するコード判定から分離し，句点・読点・空白の正規化はグループ確定後の段へ残す
	 * 行記法で設定を有効化する文はドキュメントへ取り込まず，前の説明文とは別の記録にする
	 * 計算量：記録数 C，対象本文長 N，文境界の収集費用 Q と固定した登録定義の照合費用 R に対し O(C + N + Q + R)
	 * @param Comments 分解した本文と其の共有構造を持つ記録
	 * @param Work 判定・本文複写の作業計数（無ければ計上しない）
	 * @throws std::bad_alloc 記録・テキスト又は共有構造のメモリを確保出来ない場合
	 * @throws std::length_error 記録・テキスト又は共有構造が格納先の上限を超える場合
	 * @throws FormatLimitExceeded 記法の定義が無いか，作業計数が上限を超える場合
	 */
	void DocCommentSynchronizer::SplitEmbedded(CommentRepository &Comments, StructureWorkAccount *const Work) {
		// 分離する記録と文境界の位置
		struct Partitions {
			size_t Record; // 分ける元の記録
			std::vector<size_t> Starts; // 後続文の開始位置
		};
		std::vector<Partitions> Selected;
		for(size_t Idx = 0; Idx < Comments.Records.size(); ++Idx) {
			if(Work) Work->Charge(1);
			const CommentRecord &Record = Comments.Records[Idx];
			const std::string &Text = Record.Text;
			if(
				Record.Notation != CommentNotation::RubyEmbeddedDocument || Text.find('\n') < Text.size() ||
				Comments.GetOutputStructure().HasUnclosedCode(Idx)
			) continue;
			const NotationRules &Rules = CommentRepository::GetRulesOf(Record);
			const std::vector<size_t> Boundaries =
			CommentNormalizer::GetSentenceStarts(Text, Comments.GetOutputStructure().GetCodeMask(Idx));
			if(Work) Work->Charge(Text.size() + Boundaries.size());
			// 採用する `# ` の行記法は登録形だけを照合し，文毎の末尾の合成を省く
			std::vector<size_t> Starts =
			CommentRepository::GetRegisteredSentenceStarts(Rules.OutputForms.Conditions, Text, Boundaries, Work);
			if(!Starts.empty()) Selected.push_back({ Idx, std::move(Starts) });
		}
		// 分離が不要な場合の終了
		if(Selected.empty()) return;
		CommentRecordList Split;
		Split.reserve(Comments.Records.size());
		for(size_t Idx = 0, Selection = 0; Idx < Comments.Records.size(); ++Idx) {
			CommentRecord &Record = Comments.Records[Idx];
			if(Selection == Selected.size() || Selected[Selection].Record != Idx) {
				Split.push_back(std::move(Record));
				continue;
			}
			std::vector<size_t> &Starts = Selected[Selection++].Starts;
			Starts.push_back(Record.Text.size());
			// 元の本文と出力本文を分離片毎にコピーせず，確定した区間だけを複写する
			const std::string Text = std::move(Record.Text);
			Record.Text.clear();
			Record.Output.reset();
			size_t Begin = 0;
			for(const size_t End : Starts) {
				if(Work) Work->Charge(End - Begin + 1);
				CommentRecord &Part = Split.emplace_back(Record);
				Part.Text = std::string_view(Text).substr(Begin, End - Begin);
				CommentRepository::ResolveOutput(Part);
				Begin = End;
			}
		}
		Comments.AnchorIdx.clear();
		Comments.Records = std::move(Split);
		Comments.SetStructure(CommentNormalizer::Inspect(ReadOnlyCommentView(Comments.GetRecords()), Comments.GetStructure()));
		// 終了
		return;
	}

	/**
	 * ドキュメントコメントの生成と補正関数（整形仕様 7.4）
	 * 関数のシグネチャと既存のドキュメントを突合し，不足の仮引数・戻値の項目を補い余分を除く（説明文は保持する）
	 * 定義の直前の通常コメントはドキュメントの説明へ統合する
	 * D はシグネチャ抽出，記録の安定分割・所属判定，本文構造判定，項目統合・整合，本文の描画・コピーの費用を表す
	 * D には見出からコピーして再処理する記録の費用も含む
	 * 計算量：TablesOf の費用 T，アンカー・見出の延照合数 A，原文の記録数 C，触れた記録の総数 K とドキュメント毎の処理費用の和 D に対し，ハッシュ操作の期待値で O(T + C + A + K log(K + 1) + D)
	 * @param Syntax 正規化後の構文
	 * @param Comments アンカーのノード毎のコメント
	 * @param Work 所属表を構築する訪問回数（無ければ計上しない）
	 * @param InitialIndexWork 初回の本文構造の確定で再登録した記録数（無ければ計上しない）
	 * @throws FormatLimitExceeded 読む記録に記法の定義が無い場合，又は構文の照会・走査が作業予算を超える場合
	 * @throws std::bad_alloc 記録又はドキュメントのメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	void DocCommentSynchronizer::Synchronize(
		const SyntaxModel &Syntax,
		CommentRepository &Comments,
		size_t *const Work,
		size_t *const InitialIndexWork
	) {
		// 初回の本文構造は，投影で確定したアンカーのインデックスを共有して確定する
		if(!Comments.GetStructure()) {
			Comments.SetStructure(CommentNormalizer::Inspect(ReadOnlyCommentView(Comments.GetRecords())), InitialIndexWork);
		}
		// 記録の分離・差替に入る時点でアンカーのインデックスを無効化する
		Comments.AnchorIdx.clear();
		// 記録と同期する定義が無ければ，所属表と同期の作業列は不要
		if(Comments.GetRecords().empty() && Comments.GetDocumentationSites().empty()) return;
		const ControlFlow Flow(Syntax);
		SplitEmbedded(Comments);
		// 組み直すドキュメントのグループ番号も，同じ記録の訪問で確定
		CommentGroupId NextGroup = 0;
		for(size_t Idx = 0; Idx < Comments.Records.size(); ++Idx) {
			CommentRecord &Record = Comments.Records[Idx];
			if(Record.Format.Group != NoCommentGroup) NextGroup = std::max(NextGroup, Record.Format.Group + 1);
		}
		// ドキュメントの記法と項目書式の取得
		const NotationRules &Rules = Syntax.GetRules().Notation;
		// 同期した内容の有る論理行だけの本文の組立
		const auto MakeDocBody = [](const std::vector<std::string> &Lines) -> std::string {
			std::string Body;
			for(const std::string &Line : Lines) if(!Line.empty()) Body.append(Body.empty() ? "" : "\n").append(Line);
			// 記号を含まない本文の返戻
			return Body;
		};
		// アンカー毎の作業中の先行の記録と最後に戻した順（最初に触れた時にリポジトリから読む）
		struct LeadingRecords {
			std::vector<CommentRecord> Records; // 先行の記録
			size_t Added = 0; // 最後に戻した順（戻して居なければ０）
		};
		using LeadingMap = std::unordered_map<NodeId, LeadingRecords>; // アンカーから先行の記録への対応
		LeadingMap LeadingOf;
		std::vector<LeadingRecords *> Encountered;
		size_t AddCount = 0;
		// アンカーの作業中の先行の記録
		const auto LeadingAt = [&LeadingOf, &Encountered](const NodeId Anchor) -> LeadingRecords & {
			// 先行記録の格納先とアンカーの登録
			const std::pair<LeadingMap::iterator, bool> Inserted = LeadingOf.try_emplace(Anchor);
			if(Inserted.second) Encountered.push_back(&Inserted.first->second);
			// アンカーの先行の記録の返戻
			return Inserted.first->second;
		};
		// アンカーの先行の記録の取出
		const auto TakeLeading = [&LeadingOf](const NodeId Anchor) -> std::vector<CommentRecord> {
			// 先行記録の格納先の探索
			const LeadingMap::iterator Found = LeadingOf.find(Anchor);
			// 取り出した記録（記録の無いアンカーなら空）の返戻
			return Found == LeadingOf.end() ? std::vector<CommentRecord>() : std::exchange(Found->second.Records, {});
		};
		// アンカーへの先行の記録の追加（後の取出の対象にもする）
		const auto AddLeading = [&LeadingAt, &AddCount](const NodeId Anchor, std::vector<CommentRecord> &&Added) -> void {
			// 記録を戻さない場合の終了
			if(Added.empty()) return;
			LeadingRecords &Target = LeadingAt(Anchor);
			for(CommentRecord &Record : Added) {
				if(Record.Kind == CommentKind::OwnLine) Record.Anchor = Anchor;
				Record.Placement.Side = CommentPlacement::Before;
				Target.Records.push_back(std::move(Record));
			}
			Target.Added = ++AddCount;
			// 終了
			return;
		};
		// ノード毎の其のノードを頭（ドキュメントのアンカーと其れを先頭に持つラッパー）又は見出の中に持つドキュメント化対象の定義
		if(!Comments.TargetTables) Comments.TargetTables = TablesOf(Syntax, Work);
		const TargetTables &Tables = *Comments.TargetTables;
		// アンカーの後の独立した通常コメントの同期対象の判定
		// 定義の頭・見出，又は見出に続く本体の前へ置く物の，同じ定義の先行コメント・見出内コメントへの取込（整形仕様 7.4, 7.6）
		// 後置へ変えた語の後続を，其の構文の後に続く物として扱う為の探索
		// 空の本体と Go の最上位の空行の前の記録の除外
		const auto PlacedBefore = [&Syntax, &Tables](const NodeId Anchor) -> NodeId {
			// 空の本体の外の後続ノードの探索
			NodeId Up = Anchor, Next = Syntax.GetFollowingOf(Anchor);
			while(Next != NoNode && !Syntax.IsNamed(Next) && !Syntax.Is(Next, Role::CloserToken) && Syntax.GetParentOf(Up) != NoNode) {
				Next = Syntax.GetFollowingOf(Up = Syntax.GetParentOf(Up));
			}
			// 定義へ統合する位置の後続（開きのトークンの無い本体は本体の中の為に除く）か，統合しなければ `NoNode` の返戻
			return Next != NoNode && (
				Tables.Defs[Next] != NoNode || Tables.Defs[Anchor] != NoNode && Syntax.Is(Next, Role::DefBody) && Syntax.IsBracedBody(Next) &&
				Syntax.GetParentOf(Next) == Tables.Defs[Anchor]
			) ? Next : NoNode;
		};
		// 同期が所有する原文の記録
		CommentRecordList SrcRecords = std::move(Comments.Records);
		const std::span<CommentRecord> Records = SrcRecords;
		Syntax.Charge(Records.size() + Comments.GetDocumentationSites().size());
		std::unordered_map<NodeId, NodeId> InnerAnchors;
		std::unordered_set<NodeId> OuterAnchors;
		for(size_t Idx = 0; Idx < Records.size(); ++Idx) {
			CommentRecord &Record = Records[Idx];
			if(Rules.IsInnerDoc(Record.Notation)) InnerAnchors[Tables.Enclosing[Record.Anchor]] = Record.Anchor;
			if(
				Record.Kind != CommentKind::OwnLine || Record.Role != CommentRole::Presentation || Record.Placement.IsEmptyBody ||
				Record.Placement.IsRemovedCaseBody || Rules.IsInnerDoc(Record.Notation) ||
				Syntax.Is(Syntax.GetParentOf(Record.Anchor), Role::AdjacentDocSeq)
			) continue;
			if(Record.Placement.Side == CommentPlacement::After) {
				if(const NodeId Next = PlacedBefore(Record.Anchor); Next != NoNode) {
					Record.Anchor = Next;
					Record.Placement.Side = CommentPlacement::Before;
					Record.Placement.TokensBefore = 0;
				}
				continue;
			}
			// 本体の最初の子の前で，本体を開くトークンより前に在った記録の，本体の前への所属替（見出と本体の間の物，開きのトークンの無い本体は本体の中）
			if(
				const NodeId Body = Syntax.GetParentOf(Record.Anchor);
				Record.Placement.Side == CommentPlacement::Before && Syntax.Is(Body, Role::DefBody) && Syntax.IsBracedBody(Body) &&
				Syntax.GetNamedChild(Body, 0) == Record.Anchor && Record.Placement.TokensBefore >= Syntax.GetNode(Record.Anchor).Idx &&
				Tables.Defs[Syntax.GetParentOf(Body)] != NoNode
			) {
				Record.Placement.TokensBefore -= Syntax.GetNode(Record.Anchor).Idx;
				Record.Anchor = Body;
			}
		}
		// 全先行記録のアンカー毎の所有権の移譲
		for(CommentRecord &Record : SrcRecords) if(Record.Placement.Side == CommentPlacement::Before) {
			if(IsMergeable(Record) && !Rules.IsInnerDoc(Record.Notation)) OuterAnchors.insert(Record.Anchor);
			LeadingAt(Record.Anchor).Records.push_back(std::move(Record));
		}
		// 説明する側毎の同期対象を同じ所属表から確定する
		struct SyncSite {
			const DocumentationSite *Site; // 定義の記法
			NodeId Anchor; // 説明を置く境界
			bool IsInner; // 囲む本体の説明か
		};
		std::vector<SyncSite> Sites;
		Sites.reserve(Comments.GetDocumentationSites().size() << 1);
		for(const DocumentationSite &Site : Comments.GetDocumentationSites()) {
			const std::unordered_map<NodeId, NodeId>::const_iterator Found = InnerAnchors.find(Site.Anchor);
			const NodeId Inner = Found == InnerAnchors.end() ? NoNode : Found->second, Outer = Tables.Anchors[Site.Anchor];
			if(Inner == NoNode || Outer != NoNode && OuterAnchors.contains(Outer)) Sites.push_back({ &Site, Outer, false });
			if(Inner != NoNode) Sites.push_back({ &Site, Inner, true });
		}
		// 全ての説明する側を同じ同期へ通す
		for(const SyncSite &Location : Sites) {
			const DocumentationSite &Site = *Location.Site;
			const NodeId Node = Site.Anchor;
			const CommentOutputForm *const NewForm = Rules.GetAdoptedFormOf(CommentUse::Doc, Location.IsInner);
			const bool CanCreate = NewForm && VocabularyOf(Rules.Vocabulary, DocDescription::Summary).Policy != DocStylePolicy::SrcOwned;
			const std::string_view NewMarker = Location.IsInner ? std::string_view() : GetNewDocMarker(Rules);
			// ドキュメントを付けない定義と，シグネチャを信用出来ないエラーを含む定義・エラー領域の後の定義（保護したテキストの中へ独立行を作れず整形の度に増える）の読飛
			if(
				!Tables.IsDefs[Node] || Syntax.GetNode(Node).Facts.Has(ConstraintKind::ContainsError) ||
				Syntax.Is(Syntax.GetPrevSibling(Node), Role::Error)
			) continue;
			// 定義のアンカーと先行する記録
			NodeId Anchor = Location.Anchor;
			std::vector<CommentRecord> OldLeading = TakeLeading(Anchor);
			// アンカーを先頭に持つラッパーの先行コメントの探索
			for(
				NodeId Up = Anchor;
				!Location.IsInner && OldLeading.empty() && Syntax.GetParentOf(Up) != NoNode && !Syntax.GetNode(Up).Idx;
			) {
				Up = Syntax.GetParentOf(Up);
				OldLeading = TakeLeading(Up);
				if(!OldLeading.empty()) Anchor = Up;
			}
			// 独立コメントと既存ドキュメントを同期し，其の他の行末・文中のコメントは其の儘戻す
			const std::vector<CommentRecord>::iterator Split = std::stable_partition(
				OldLeading.begin(),
				OldLeading.end(),
				[&Rules, &Location](const CommentRecord &Comment) -> bool {
					// 同じ側の独立コメント又は同期する既存ドキュメントかの返戻
					return (Comment.Kind == CommentKind::OwnLine || IsMergeable(Comment)) && Rules.IsInnerDoc(Comment.Notation) == Location.IsInner;
				}
			);
			std::vector<CommentRecord> Others(std::make_move_iterator(Split), std::make_move_iterator(OldLeading.end()));
			OldLeading.erase(Split, OldLeading.end());
			// アンカー自身の先行記録の数（其の後は見出の記録のコピー）
			const size_t LeadingCount = OldLeading.size();
			// 見出（前置の属性と本体の手前迄の子）の中の先行の独立したドキュメントとドキュメントへ統合し得る独立コメントのコピー（整形仕様 7.4 は行幅・最終テキストでの判断を禁じる為，構文から決める）
			// 元の記録は同期を確定する迄其のアンカーに残し，ディレクティブコメント等の他の記録は元のアンカーの儘とする
			const auto OutputDocOf = [&Rules](const CommentRecord &Comment) -> std::optional<DocumentFormId> {
				// 出力記法でドキュメントと読む時のドキュメント形式の返戻
				return IsDoc(Comment) ? Rules.GetDocFormOf(CommentRepository::GetOutputNotationOf(Comment)) : std::nullopt;
			};
			std::optional<DocumentFormId> Container = NewForm ? Rules.GetDocFormOf(NewForm->Notation) : std::nullopt;
			for(const CommentRecord &Comment : OldLeading) if(const std::optional<DocumentFormId> Form = OutputDocOf(Comment)) {
				Container = Form;
			}
			// 既に取り込んだ見出の記録の照合
			const auto IsHeaderTaken = [&OutputDocOf, &Tables, &Rules, &Location, Container, Node](const CommentRecord &Comment) -> bool {
				// ドキュメント形式と既存項目の照合
				const std::optional<DocumentFormId> Form = OutputDocOf(Comment);
				// 見出からドキュメントの側へ移す記録かの返戻
				return !Location.IsInner && !Rules.IsInnerDoc(Comment.Notation) &&
				(Comment.Kind == CommentKind::OwnLine || Tables.Defs[Comment.Anchor] == Node) && (!Form || Form == Container) &&
				IsMergeable(Comment);
			};
			// 前置の属性（兄弟の装飾子等）と本体の手前迄の子と，波括弧で開く本体（本体の前に在る物だけを見る，開きのトークンの無い本体の前の物は本体の中）
			struct HeaderSelection {
				NodeId Anchor;
				std::vector<size_t> Indices;
			};
			std::vector<HeaderSelection> HeaderSelections;
			std::vector<NodeId> HeaderParts;
			NodeId Body = NoNode;
			for(NodeId Prefix = SignatureExtractor::GetLeadOf(Syntax, Node); Prefix != Node; Prefix = Syntax.GetNextNamedSibling(Prefix)) {
				HeaderParts.push_back(Prefix);
			}
			for(const NodeId Child : Syntax.GetChildrenOf(Node)) {
				if(IsHeaderEnd(Syntax, Node, Child)) {
					if(Syntax.Is(Child, Role::DefBody)) Body = Child;
					break;
				}
				HeaderParts.push_back(Child);
			}
			// ノードの先行の記録の内，見出からドキュメントの側へ移す物のコピー
			const auto TakeHeader = [&](const NodeId Child) -> void {
				// 見出の先行記録の探索
				const LeadingMap::iterator Found = LeadingOf.find(Child);
				// 記録の無い見出の終了
				if(Found == LeadingOf.end() || Found->second.Records.empty()) return;
				LeadingRecords &Leading = Found->second;
				HeaderSelection Selection { Child, {} };
				for(size_t Idx = 0; Idx < Leading.Records.size(); ++Idx) {
					if(!IsHeaderTaken(Leading.Records[Idx])) continue;
					OldLeading.push_back(Leading.Records[Idx]);
					Selection.Indices.push_back(Idx);
				}
				if(!Selection.Indices.empty()) {
					HeaderSelections.push_back(std::move(Selection));
					Leading.Added = ++AddCount;
				}
				// 終了
				return;
			};
			// 定義自身とアンカーとの間のラッパーの先行記録の収集
			if(Node != Anchor) {
				TakeHeader(Node);
				for(NodeId Part = Syntax.GetParentOf(Node); Part != Anchor && Part != NoNode && Tables.Defs[Part] == Node;) {
					TakeHeader(Part);
					Part = Syntax.GetParentOf(Part);
				}
			}
			for(const NodeId Part : HeaderParts) {
				Core::SyntaxModel::HasStoppedTraversal(
					Syntax,
					Part,
					[&TakeHeader](const NodeId Child) -> WalkStep {
						// 見出の説明の取込
						TakeHeader(Child);
						// 子孫の走査を続ける事の返戻
						return WalkStep::Descend;
					}
				);
			}
			if(Body != NoNode && Syntax.IsBracedBody(Body)) TakeHeader(Body);
			// 通常の説明のドキュメントを持つか（ディレクティブコメントとソース側に保持したドキュメントは改変しない）
			if(
				const bool HasPresentationDoc = std::any_of(
					OldLeading.begin(),
					OldLeading.end(),
					[](const CommentRecord &Comment) -> bool {
						// 通常の説明のドキュメントコメントかの返戻
						return Comment.Role == CommentRole::Presentation && IsDoc(Comment);
					}
				); !CanCreate && !HasPresentationDoc
			) {
				OldLeading.erase(OldLeading.begin() + static_cast<std::ptrdiff_t>(LeadingCount), OldLeading.end());
				OldLeading.insert(OldLeading.end(), std::make_move_iterator(Others.begin()), std::make_move_iterator(Others.end()));
				AddLeading(Anchor, std::move(OldLeading));
				// 既存のコメントの復帰と次の定義への移行
				continue;
			}
			// 型・値の並替後も原文の順を保つ見出の説明の概要への統合
			std::stable_sort(
				OldLeading.begin() + static_cast<std::ptrdiff_t>(LeadingCount),
				OldLeading.end(),
				[](const CommentRecord &Left, const CommentRecord &Right) -> bool {
					// 原文の順の比較の返戻
					return Left.OriginalOrder < Right.OriginalOrder;
				}
			);
			// 同じ記録の訪問で所属・統合可否・原文順を確定し，区間毎の処理でも共有
			std::vector<uint8_t> AllIsDocs(OldLeading.size()), CanMerge(OldLeading.size());
			size_t FullLastOrder = OldLeading.empty() ? std::numeric_limits<size_t>::max() : 0;
			std::optional<size_t> LastBoundary, LastDoc;
			for(size_t Idx = 0; Idx < OldLeading.size(); ++Idx) {
				const CommentRecord &Record = OldLeading[Idx];
				AllIsDocs[Idx] = Record.Role == CommentRole::Presentation && IsDoc(Record);
				if(AllIsDocs[Idx]) LastDoc = Idx;
				CanMerge[Idx] = IsMergeable(Record);
				FullLastOrder = std::max(FullLastOrder, Record.OriginalOrder);
				if(Idx < LeadingCount && Record.Kind == CommentKind::OwnLine && !CanMerge[Idx] && !Record.IsEmbeddedEdge) LastBoundary = Idx;
			}
			// 既存ドキュメントが在る時だけ，保持する記録より前の通常コメントも区間を分けて定義へ統合しない
			std::vector<size_t> RunEnds;
			for(size_t Idx = 1; LastDoc && LastBoundary && Idx <= *LastBoundary; ++Idx) {
				if(OldLeading[Idx].Kind == CommentKind::OwnLine && (!CanMerge[Idx] || !AllIsDocs[Idx]) && !OldLeading[Idx].IsEmbeddedEdge) {
					RunEnds.push_back(Idx);
				}
			}
			RunEnds.push_back(OldLeading.size());
			const Signature FunctionSig = SignatureExtractor::Extract(Syntax, Flow, Node);
			// 最後の統合可能な区間だけで骨格を補い，他の区間の整合で読んだ項目を共有
			// 定義へ直接続く独立した説明は，既存ドキュメントの無い最後の区間でも統合する
			const size_t FinalStart = RunEnds.size() > 1 ? RunEnds[RunEnds.size() - 2] : 0;
			bool HasFinalDescription = false;
			for(size_t Idx = FinalStart; Idx < OldLeading.size(); ++Idx) {
				const CommentRecord &Record = OldLeading[Idx];
				if(Record.Kind == CommentKind::OwnLine && CanMerge[Idx] && !AllIsDocs[Idx] && !Record.IsEmbeddedEdge) {
					HasFinalDescription = true;
				}
			}
			const size_t CompletionRun = LastDoc && !HasFinalDescription ?
			static_cast<size_t>(std::upper_bound(RunEnds.begin(), RunEnds.end(), *LastDoc) - RunEnds.begin()) :
			RunEnds.size() - 1;
			DocCoverage Coverage;
			const size_t TotalLeading = OldLeading.size();
			std::vector<CommentRecord> AllLeading = std::move(OldLeading), SyncedLeading;
			bool HasSynchronized = false;
			for(size_t Run = 0; Run < RunEnds.size(); ++Run) {
				const size_t Start = Run ? RunEnds[Run - 1] : 0, End = RunEnds[Run];
				const size_t OwnCount = LeadingCount > Start ? std::min(LeadingCount, End) - Start : 0;
				std::vector<CommentRecord> OldLeading;
				if(RunEnds.size() == 1) OldLeading = std::move(AllLeading);
				else {
					OldLeading.assign(
						std::make_move_iterator(AllLeading.begin() + static_cast<std::ptrdiff_t>(Start)),
						std::make_move_iterator(AllLeading.begin() + static_cast<std::ptrdiff_t>(End))
					);
				}
				const auto RestoreAsIs = [&]() -> void {
					// 見出のコピーを除き，区間内の原文の記録を復元
					OldLeading.erase(OldLeading.begin() + static_cast<std::ptrdiff_t>(OwnCount), OldLeading.end());
					if(RunEnds.size() == 1) SyncedLeading = std::move(OldLeading);
					else {
						SyncedLeading.insert(
							SyncedLeading.end(),
							std::make_move_iterator(OldLeading.begin()),
							std::make_move_iterator(OldLeading.end())
						);
					}
					// 終了
					return;
				};
				const std::span<const uint8_t> IsDocs = std::span<const uint8_t>(AllIsDocs).subspan(Start, End - Start);
				if(
					Run != CompletionRun && !std::ranges::any_of(
						IsDocs,
						[](const uint8_t HasDoc) -> bool {
							// 既存の同期対象の有無の返戻
							return HasDoc;
						}
					)
				) {
					RestoreAsIs();
					continue;
				}
				// 行形式のドキュメントの行か（処理系が読むディレクティブコメントはドキュメントへ取り込まない）
				const auto IsDocLineAt = [&OldLeading, IsDocs](const size_t Idx) -> bool {
					// ドキュメントの記号を持つ説明のコメントかの返戻
					return IsDocs[Idx] && !GetDocLineMarker(OldLeading[Idx]).empty();
				};
				// 新しいドキュメントの位置（対象の定義の直前，但し頭部へ置く物を除く後続の宣言へ作用する名前付のディレクティブコメント（`//go:noinline` 等）の前）
				size_t NewAt = OwnCount;
				while(
					NewAt && OldLeading[NewAt - 1].Role == CommentRole::Directive &&
					CommentRepository::IsNamedDirective(OldLeading[NewAt - 1].Notation) && OldLeading[NewAt - 1].Directive &&
					!CommentRepository::IsFileHeadDirective(OldLeading[NewAt - 1].Directive->Definition)
				) --NewAt;
				// アンカー自身の最後の囲ドキュメント，又は見出から移した最初の囲ドキュメントの選択（一般の囲みをドキュメントに使う形式と，処理系が読む専用ドキュメントの保持を含む）
				size_t DocIdx = OldLeading.size();
				for(size_t Idx = 0; Idx < OwnCount; ++Idx) {
					if(
						IsDocs[Idx] && CommentRepository::IsBlockNotation(CommentRepository::GetOutputNotationOf(OldLeading[Idx])) ||
						OldLeading[Idx].Role == CommentRole::Directive && IsDocBlock(OldLeading[Idx])
					) DocIdx = Idx;
				}
				// 本文が囲みを閉じる記号を含む為に行形式で保つドキュメントも，同期後の格納先とする（整形仕様 7.6, 7.7）
				for(size_t Idx = DocIdx < OwnCount ? DocIdx + 1 : 0; Idx < OwnCount; ++Idx) {
					if(CommentRepository::IsBlockLineDoc(OldLeading[Idx])) DocIdx = Idx;
				}
				for(size_t Idx = OwnCount; DocIdx == OldLeading.size() && Idx < OldLeading.size(); ++Idx) if(IsDocBlock(OldLeading[Idx])) {
					DocIdx = Idx;
				}
				// 独立ドキュメントを先に選び，其の格納先へ文中の記録を移さない
				for(
					size_t Idx = 0;
					Idx < OldLeading.size() && (DocIdx == OldLeading.size() || OldLeading[DocIdx].Kind != CommentKind::OwnLine);
					++Idx
				) {
					if(
						IsDocs[Idx] && OldLeading[Idx].Kind == CommentKind::OwnLine && (
							CommentRepository::IsBlockNotation(CommentRepository::GetOutputNotationOf(OldLeading[Idx])) ||
							CommentRepository::IsBlockLineDoc(OldLeading[Idx])
						)
					) DocIdx = Idx;
				}
				// 独立ドキュメントが無ければ，既存の文中ドキュメントを補完先とする
				for(size_t Idx = 0; DocIdx == OldLeading.size() && Idx < OldLeading.size(); ++Idx) {
					if(IsDocs[Idx] && OldLeading[Idx].Kind != CommentKind::OwnLine) DocIdx = Idx;
				}
				if(DocIdx < OldLeading.size() && OldLeading[DocIdx].Role == CommentRole::Directive) {
					RestoreAsIs();
					// ディレクティブコメントのドキュメントの原文保持と次の定義への移行
					continue;
				}
				const bool IsInlineDoc = DocIdx < OldLeading.size() && OldLeading[DocIdx].Kind != CommentKind::OwnLine;
				// 行形式のドキュメントの行の添字（アンカー自身の物，見出の行から移した物の順）
				std::vector<size_t> DocLines;
				for(size_t Idx = 0; Idx < OldLeading.size(); ++Idx) if(IsDocLineAt(Idx)) DocLines.push_back(Idx);
				// 既存の囲ドキュメント・既存の行形式のドキュメント・新しいドキュメントの書式の順に依る出力記法の選択
				const bool IsBlock = DocIdx < OldLeading.size() || DocLines.empty() && NewMarker.empty();
				// 同じ説明対象・ドキュメント形式の囲みの記録（見出内も含め，原文順へ揃える）
				std::vector<size_t> DocBlocks;
				if(IsBlock) {
					const std::optional<DocumentFormId> BlockForm = DocIdx < OldLeading.size() ? OutputDocOf(OldLeading[DocIdx]) : Container;
					// 既存ドキュメントの項目を，位置と種別に依らず同じシグネチャへ対応付ける
					for(size_t Idx = 0; Idx < OldLeading.size(); ++Idx) {
						if(
							IsDocs[Idx] && OutputDocOf(OldLeading[Idx]) == BlockForm &&
							OldLeading[Idx].Kind == (IsInlineDoc ? OldLeading[DocIdx].Kind : CommentKind::OwnLine) && (
								!IsInlineDoc || OldLeading[Idx].Anchor == OldLeading[DocIdx].Anchor &&
								OldLeading[Idx].Placement.TokensBefore == OldLeading[DocIdx].Placement.TokensBefore &&
								OldLeading[Idx].Format.ShouldSeparateLogicalLines == OldLeading[DocIdx].Format.ShouldSeparateLogicalLines
							)
						) DocBlocks.push_back(Idx);
					}
					std::stable_sort(
						DocBlocks.begin(),
						DocBlocks.end(),
						[&OldLeading](const size_t Left, const size_t Right) -> bool {
							return OldLeading[Left].OriginalOrder < OldLeading[Right].OriginalOrder;
						}
					);
				}
				// アンカー自身のドキュメントの最初の記録（無ければ新しいドキュメントの位置）
				const std::vector<size_t>::const_iterator OwnEnd = std::lower_bound(DocLines.begin(), DocLines.end(), OwnCount);
				const size_t FirstDoc =
				IsBlock && !DocBlocks.empty() ? DocBlocks.front() : IsBlock ? DocIdx : OwnEnd != DocLines.begin() ? DocLines.front() : NewAt;
				const size_t DocStart = std::min(FirstDoc, NewAt);
				// ディレクティブコメントを挟まず定義へ続く独立コメントの最初（整形仕様 7.4）
				size_t RunStart = NewAt;
				while(
					End == TotalLeading && !IsInlineDoc && RunStart && OldLeading[RunStart - 1].Kind == CommentKind::OwnLine &&
					CanMerge[Start + RunStart - 1]
				) --RunStart;
				// 其の中でドキュメントへ統合する通常コメント（ドキュメントの手前にディレクティブコメントが挟まれば統合しない）と，ドキュメントより前と後の物の論理行
				// 同期へ取り込まない空の縁の所属記録からの除去
				std::vector<bool> IsMerged(OldLeading.size());
				for(size_t Idx = 0; Idx < OldLeading.size(); ++Idx) IsMerged[Idx] = OldLeading[Idx].IsEmbeddedEdge;
				for(const size_t Idx : DocBlocks) IsMerged[Idx] = true;
				std::vector<std::string> Before, After;
				for(size_t Idx = RunStart; DocStart >= RunStart && Idx < NewAt; ++Idx) {
					if(IsDocs[Idx] || OldLeading[Idx].IsEmbeddedEdge) continue;
					IsMerged[Idx] = true;
					std::vector<std::string> &Target = Idx < DocStart ? Before : After;
					AppendLogicalLines(OldLeading[Idx], Target);
				}
				// 同期を確定した見出の記録の元のアンカーからの除去（コピーはドキュメントへ統合するかアンカーの先行コメントへ移す）
				if(End == TotalLeading) for(const HeaderSelection &Selection : HeaderSelections) {
					std::vector<CommentRecord> &Leading = LeadingAt(Selection.Anchor).Records;
					size_t Taken = 0, Kept = 0;
					for(size_t Idx = 0; Idx < Leading.size(); ++Idx) {
						if(Taken < Selection.Indices.size() && Selection.Indices[Taken] == Idx) ++Taken;
						else {
							if(Kept != Idx) Leading[Kept] = std::move(Leading[Idx]);
							++Kept;
						}
					}
					Leading.resize(Kept);
				}
				// シグネチャと同期後の記録
				Signature Completion = Run == CompletionRun ? FunctionSig : Signature(), NoCompletion;
				DocParamIdx ParamIdx;
				for(size_t Idx = 0; Idx < OldLeading.size(); ++Idx) {
					CommentRecord &Record = OldLeading[Idx];
					if(!IsDocs[Idx] || Record.Kind == CommentKind::OwnLine || IsMerged[Idx]) continue;
					// 同じ対象でも異なる境界のドキュメントは，本文だけを同期して配置を保つ
					std::vector<std::string> Lines;
					AppendLogicalLines(Record, Lines);
					const std::optional<std::vector<DocItem>> Items =
					ParseItems(Rules, Lines, FunctionSig, CodeLineStarts(Rules, Lines, Comments.GetStructure()), ParamIdx);
					CoverItems(Items, Coverage);
					const std::vector<std::string> NewLines = ReconcileLogicalLines(Rules, Lines, FunctionSig, Items, &NoCompletion);
					Record.Text = MakeDocBody(NewLines);
					Record.Format.HasSummaryPlaceholder = !NewLines.empty() && NewLines.front().empty();
					Record.StructureIndex.reset();
				}
				std::erase_if(
					Completion.Params,
					[&Coverage](const std::string &Name) -> bool {
						// 別の境界の既存項目を重複補完しない事の返戻
						return Coverage.Names.contains(Name);
					}
				);
				Completion.HasReturn = Completion.HasReturn && !Coverage.HasReturn;
				std::vector<CommentRecord> NewLeading;
				// ドキュメントの記法に応じた新しい先行記録の組立
				if(IsBlock) {
					std::vector<std::string> ExistingLines;
					// ドキュメントの説明を残す為，項目の整合前に論理行へ分解
					std::vector<size_t> GroupStarts;
					CommentGroupId LastGroup = NoCommentGroup;
					for(const size_t Idx : DocBlocks) {
						const CommentRecord &Record = OldLeading[Idx];
						if(
							(GroupStarts.empty() || GroupStarts.back() != ExistingLines.size()) && (
								CommentRepository::IsBlockNotation(Record.Notation) || Record.Format.InputGroup == NoCommentGroup ||
								Record.Format.InputGroup != LastGroup
							)
						) GroupStarts.push_back(ExistingLines.size());
						AppendLogicalLines(Record, ExistingLines);
						LastGroup = CommentRepository::IsBlockNotation(Record.Notation) ? NoCommentGroup : Record.Format.InputGroup;
					}
					// 見出の説明を直前の説明の後へ纏めて統合
					for(size_t Idx = OwnCount; Idx < OldLeading.size(); ++Idx) {
						if(IsMerged[Idx] || IsInlineDoc || OldLeading[Idx].Kind != CommentKind::OwnLine) continue;
						AppendLogicalLines(OldLeading[Idx], After);
						IsMerged[Idx] = true;
					}
					// 整合した論理行と描画するドキュメントの記録
					const std::vector<std::string> NewLines = SynchronizeLines(
						Rules,
						Comments.GetStructure(),
						FunctionSig,
						std::move(ExistingLines),
						Before,
						After,
						std::move(GroupStarts),
						&Completion,
						RunEnds.size() > 1 ? &Coverage : nullptr
					);
					// 格納先と，異なる境界の記録へ同じ本文の組立規則を適用
					std::string Text = MakeDocBody(NewLines);
					CommentRecord DocAttach { std::move(Text), { CommentPlacement::Before }, 0 };
					// 既存のドキュメントの入力の記法と正規化のグループの保持（出力記法は整形仕様 7.6 で選ぶ）
					if(DocIdx < OldLeading.size()) {
						const CommentRecord &Original = OldLeading[DocIdx];
						DocAttach.Notation = Original.Notation;
						DocAttach.Format.Group = Original.Format.Group;
						DocAttach.Format.ShouldSeparateLogicalLines = Original.Format.ShouldSeparateLogicalLines;
						if(Original.Kind != CommentKind::OwnLine) {
							DocAttach.Kind = Original.Kind;
							DocAttach.Anchor = Original.Anchor;
							DocAttach.Placement = Original.Placement;
							DocAttach.Format = Original.Format;
							DocAttach.OriginalOrder = Original.OriginalOrder;
						}
					} else DocAttach.Notation = NewForm->Notation;
					// 新しいドキュメントの記録への対象定義の記法の共有
					DocAttach.Format.HasSummaryPlaceholder = !NewLines.empty() && NewLines.front().empty();
					CommentRepository::AttachRules(*Site.Rules, DocAttach);
					// 元のドキュメント位置だけを新しい本文へ置換し，ドキュメントへ統合した物を除く（既存のドキュメントが無ければ新しいドキュメントの位置）
					const size_t At = DocIdx < OldLeading.size() && OldLeading[DocIdx].Kind != CommentKind::OwnLine ?
					DocIdx :
					DocBlocks.empty() ? NewAt : DocBlocks.front();
					for(size_t Idx = 0; Idx <= OldLeading.size(); ++Idx) {
						if(Idx == At) NewLeading.push_back(std::move(DocAttach));
						if(Idx < OldLeading.size() && Idx != DocIdx && !IsMerged[Idx]) NewLeading.push_back(std::move(OldLeading[Idx]));
					}
				} else {
					// ドキュメントの記号（既存のドキュメントの最初の行に揃え，無ければ言語の新しいドキュメントの記号）
					const std::string_view Marker = DocLines.empty() ? NewMarker : GetDocLineMarker(OldLeading[DocLines.front()]);
					// ドキュメントの行の本文と，其の本文毎の元の記録の位置（出現順に取り出す為に逆順に積む）
					std::vector<std::string_view> SrcTexts;
					SrcTexts.reserve(DocLines.size());
					std::vector<std::string> ExistingLines;
					ExistingLines.reserve(DocLines.size());
					std::vector<size_t> GroupStarts;
					CommentGroupId LastGroup = NoCommentGroup;
					// 原文の照合範囲・編集する論理行・ドキュメントの境界を同じ記録の訪問で確定
					for(const size_t Idx : DocLines) {
						const CommentRecord &Record = OldLeading[Idx];
						const std::string_view Text = GetLogicalText(Record);
						if(GroupStarts.empty() || Record.Format.InputGroup == NoCommentGroup || Record.Format.InputGroup != LastGroup) {
							GroupStarts.push_back(ExistingLines.size());
						}
						LastGroup = Record.Format.InputGroup;
						SrcTexts.push_back(Text);
						ExistingLines.emplace_back(Text);
					}
					std::unordered_map<std::string_view, std::vector<size_t>> Originals;
					for(size_t Pos = DocLines.size(); Pos--;) Originals[SrcTexts[Pos]].push_back(DocLines[Pos]);
					for(size_t Idx = OwnCount; Idx < OldLeading.size(); ++Idx) {
						if(OldLeading[Idx].IsEmbeddedEdge || IsDocLineAt(Idx) || OldLeading[Idx].Kind != CommentKind::OwnLine) continue;
						AppendLogicalLines(OldLeading[Idx], After);
						IsMerged[Idx] = true;
					}
					const std::vector<std::string> NewLines = SynchronizeLines(
						Rules,
						Comments.GetStructure(),
						FunctionSig,
						std::move(ExistingLines),
						Before,
						After,
						std::move(GroupStarts),
						&Completion,
						RunEnds.size() > 1 ? &Coverage : nullptr
					);
					std::vector<CommentRecord> DocRecords;
					DocRecords.reserve(NewLines.size());
					for(const std::string &Line : NewLines) {
						if(
							const std::unordered_map<std::string_view, std::vector<size_t>>::iterator Iter = Originals.find(Line);
							Iter != Originals.end() && !Iter->second.empty()
						) {
							const size_t Original = Iter->second.back();
							Iter->second.pop_back();
							// キーは逆順で最初に登録した最後の記録の本文を参照する為，其の移動より先に除く
							if(Iter->second.empty()) Originals.erase(Iter);
							// 元の記録の本文
							DocRecords.push_back(std::move(OldLeading[Original]));
							if(DocRecords.back().Text != Line) {
								DocRecords.back().Text = Line;
								DocRecords.back().StructureIndex.reset();
							}
							continue;
						}
						// 既存の論理行は元の記録（記号・続きの行のインデント）の儘，補った論理行と空行だけをドキュメントの記号の記法で書く
						CommentRecord Record { Line, { CommentPlacement::Before }, 0 };
						Record.Notation = CommentRepository::NotationOf(Rules, Marker, false);
						CommentRepository::AttachRules(*Site.Rules, Record);
						DocRecords.push_back(std::move(Record));
					}
					// ドキュメント全体のコード例を正規化・配置で判定する為の，既存の行・補った行・統合した行の単一つのグループ化
					for(size_t Idx = 0; Idx < DocRecords.size(); ++Idx) {
						DocRecords[Idx].Format.Group = NextGroup;
						DocRecords[Idx].Format.HasSummaryPlaceholder = !Idx && NewLines.front().empty();
					}
					++NextGroup;
					// ドキュメントの最初の行の位置へドキュメントを置き（無ければ新しいドキュメントの位置），ドキュメントへ統合しない他のコメントは元の順
					for(const size_t Idx : DocLines) IsMerged[Idx] = true;
					const size_t At = DocLines.empty() ? NewAt : DocLines.front();
					for(size_t Idx = 0; Idx <= OldLeading.size(); ++Idx) {
						if(Idx == At) {
							NewLeading.insert(NewLeading.end(), std::make_move_iterator(DocRecords.begin()), std::make_move_iterator(DocRecords.end()));
						}
						if(Idx < OldLeading.size() && !IsMerged[Idx]) NewLeading.push_back(std::move(OldLeading[Idx]));
					}
				}
				HasSynchronized = true;
				if(RunEnds.size() == 1) SyncedLeading = std::move(NewLeading);
				else {
					SyncedLeading.insert(
						SyncedLeading.end(),
						std::make_move_iterator(NewLeading.begin()),
						std::make_move_iterator(NewLeading.end())
					);
				}
			}
			// 保持した独立記録も同じ通し番号へ揃え，区間の順を配置へ渡す
			for(CommentRecord &Record : SyncedLeading) if(HasSynchronized && Record.Kind == CommentKind::OwnLine) {
				if(Record.OriginalOrder != FullLastOrder) Record.StructureIndex.reset();
				Record.OriginalOrder = FullLastOrder;
			}
			// 行末・文中の記録は，元のトークン境界と通し番号の儘戻す
			SyncedLeading.insert(SyncedLeading.end(), std::make_move_iterator(Others.begin()), std::make_move_iterator(Others.end()));
			AddLeading(Anchor, std::move(SyncedLeading));
		}
		// 触れたアンカーの先行の記録を最後に戻した順の並びへ移した差替（取り出して戻さなかった記録は除く）
		std::vector<CommentRecord> Kept;
		for(CommentRecord &Record : SrcRecords) if(Record.Placement.Side != CommentPlacement::Before) Kept.push_back(std::move(Record));
		std::vector<LeadingRecords *> Touched;
		Touched.reserve(LeadingOf.size());
		for(LeadingRecords *const Leading : Encountered) if(!Leading->Records.empty()) Touched.push_back(Leading);
		std::stable_sort(
			Touched.begin(),
			Touched.end(),
			[](const LeadingRecords *const Left, const LeadingRecords *const Right) -> bool {
				// 戻した順の比較の返戻（触れないアンカー同士は最初に見た原文順を保つ）
				return Left->Added < Right->Added;
			}
		);
		for(LeadingRecords *const Leading : Touched) {
			Kept.insert(Kept.end(), std::make_move_iterator(Leading->Records.begin()), std::make_move_iterator(Leading->Records.end()));
		}
		Comments.Update(std::move(Kept));
		// 終了
		return;
	}

	/**
	 * ドキュメントの位置が確定した後のグループの所属の確定関数（整形仕様 3.4, 7.6）
	 * 投影が記録した並びを，確定した配置でドキュメントとして置く記録に従い，配置の段と同じ定義のグループを纏める記法で分ける
	 * 制約：Code のアンカーが持つ Syntax ≠ nullptr 且つ読取中に生存する
	 * 計算量：`GetDocTargets` と `PlacedTargets` の費用の和 T，記録数 C，`MembershipOf` の費用 M に対し O(T + C + M)
	 * @param Code 変更不能なコードの出力
	 * @param Comments アンカーのノード毎のコメント
	 * @throws FormatLimitExceeded 読む記録に記法の定義が無い場合，又は構文の照会・走査が作業予算を超える場合
	 * @throws std::bad_alloc 必要なテキスト・記録・作業表のメモリを確保出来ない場合
	 * @throws std::length_error 構築する文字列・記録・作業表の要素数が格納先の上限を超える場合
	 * @throws std::out_of_range 読む構文の識別子がノード表に無い場合
	 */
	void DocCommentSynchronizer::ConfirmGroups(const LaidOutCode &Code, CommentRepository &Comments) {
		// 説明対象と配置対象は，此の所属確定段だけで値として記録
		const std::span<const CommentRecord> Records = Comments.GetRecords();
		Comments.DocTargets = GetDocTargets(*Code.GetAnchors().Syntax, Comments);
		Comments.TargetTables.reset();
		Comments.PlacementTargets = PlacedTargets(Code, Records, Comments.DocTargets);
		std::vector<uint8_t> IsDocs(Records.size());
		// 記法と所属に依る枠付の条件の保存（本文の行数は正規化後に確定する）
		for(size_t Idx = 0; Idx < Records.size(); ++Idx) {
			CommentRecord &Record = Comments.Records[Idx];
			IsDocs[Idx] = Comments.PlacementTargets[Idx] != NoNode;
			Record.Output->ShouldFrame =
			Record.Output->Transformation == CommentOutput::Change::Frame || IsDocs[Idx] && Record.Role == CommentRole::Presentation &&
			CommentRepository::GetRulesOf(Record).IsAvailable(CommentNotation::DoubleStarBlock) && !Record.Format.ShouldAttachFirstLine &&
			IsDocBlock(Record);
		}
		const std::vector<CommentGroupId> Groups = CommentRepository::MembershipOf(Records, IsDocs);
		bool HasChanged = false;
		for(size_t Idx = 0; Idx < Records.size(); ++Idx) {
			CommentRecord &Record = Comments.Records[Idx];
			HasChanged = HasChanged || Record.Format.Group != Groups[Idx] || Record.StructureIndex != Idx;
			Record.Format.Group = Groups[Idx];
		}
		// 確定した所属で変わったグループだけを判定し，未変更の本文の保護マスクを共有
		if(HasChanged) Comments.SetStructure(CommentNormalizer::Inspect(ReadOnlyCommentView(Records), Comments.GetStructure()));
		// 終了
		return;
	}
} // namespace Core
