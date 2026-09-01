#include "Edit.hpp"
#include "../Util/DocSig.hpp"
#include "../Util/NodeKind.hpp"
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * 関数定義のドキュメントコメント (Doxygen/JSDoc/KDoc/YARD) の生成と補正関数
 * @param Src 整形対象のソース（付随情報は有効前提）
 * @param Language 対象言語
 */
void EditPass::ApplyDocComment(TSSource &Src, const Lang Language) {
	// ドキュメント不在ならタグ骨格を付与し，有れば不足 @param / @return を追加し余分を削除する（説明文は保持）
	if(!DocSig::IsTargetLanguage(Language) || !Src.IsParsed()) return;
	// 文書形式と返戻タグの取得
	const DocSig::Style Style = DocSig::StyleOf(Language);
	const std::string_view ReturnTag = DocSig::ReturnTag(Language);
	// 構文木全体の関数走査
	WalkAst(
		Src.GetRoot(),
		// 文書を持てる関数定義の処理
		[&](const TSNode Node) -> void {
			// 無名字句の型名取得と集合検索前の除外
			if(!ts_node_is_named(Node)) return;
			const std::string_view TypeView(ts_node_type(Node));
			// 引数抽出を信用出来ない解析失敗の部分木への文書付与の除外
			if(!NodeKind::FunctionLikeDefinition.Contains(TypeView) || ts_node_has_error(Node)) return;
			// 実装・コールバック用途の JS/TS オブジェクト省略形メソッドへの文書付与の除外
			if(TypeView == "method_definition") {
				// 物体リテラル内のメソッドは文書付与からの除外
				if(const TSNode Parent = ts_node_parent(Node); !ts_node_is_null(Parent) && std::string_view(ts_node_type(Parent)) == "object") {
					// 終了
					return;
				}
			}
			// 本体が必ず在る構築子・メソッド以外の，本体型に依る抽象宣言・原型の除外
			if(
				Language != Lang::Ruby && !NodeKind::KotlinBraceBodyHost.Contains(TypeView) && !HasChildOf(
					Node,
					[](const TSNode Child) -> bool {
						// 実装本体を持つ事の返戻
						return NodeKind::FunctionBodyContainer.Contains(Child);
					}
				)
				// 条件成立時の返戻
			) return;
			TSNode Anchor = DocSig::DocAnchor(Node, Language);
			const std::string &Source = Src;
			const uint32_t AnchorStart = ts_node_start_byte(Anchor), LineBegin = TextEdit::LineStartOf(Source, AnchorStart);
			bool StartsLine = true;
			// 行頭と錨の間にコードが無い事の確認
			for(uint32_t Pos = LineBegin; Pos < AnchorStart; ++Pos) if(Source[Pos] != ' ' && Source[Pos] != '\t') {
				// 行途中のコードを検出
				StartsLine = false;
				break;
			}
			// 行の途中の定義への文書付与の抑止
			if(!StartsLine) return;
			std::vector<CommentAttach> OldLeading = Src.TakeLeading(Anchor);
			const auto BlankBefore = [&Source, AnchorStart](const uint32_t From) -> bool {
				// 錨より後の祖先は錨を先頭に持たない事の返戻
				if(From > AnchorStart) return false;
				// 包装と錨の間の空白範囲の検査
				for(uint32_t Pos = From; Pos < AnchorStart; ++Pos) {
					// 空白文字の順次確認
					// 空白以外の文字が有る事の返戻
					if(const char Char = Source[Pos]; Char != ' ' && Char != '\t' && Char != '\n' && Char != '\r') return false;
				}
				// 間が空白だけである事の返戻
				return true;
			};
			if(OldLeading.empty()) {
				// 同じ先頭を共有する包装の先行コメントの探索
				for(
					TSNode Ancestor = ts_node_parent(Anchor);
					!ts_node_is_null(Ancestor) && BlankBefore(ts_node_start_byte(Ancestor));
					Ancestor = ts_node_parent(Ancestor)
				) {
					// コメントを持つ最初の祖先への文書配置
					OldLeading = Src.TakeLeading(Ancestor);
					if(!OldLeading.empty()) {
						// 文書を持つ祖先へ錨を移動
						Anchor = Ancestor;
						break;
					}
				}
			}
			// 道具が位置と字面を読む Ruby の型注釈・文書化制御を持つコメントの保持
			if(
				Style != DocSig::Style::Block && std::any_of(
					OldLeading.begin(),
					OldLeading.end(),
					[](const CommentAttach &Comment) -> bool {
						// 密着が要件の印で始まるコメントかの返戻
						return Comment.Text.starts_with("#:") || Comment.Text.starts_with("#--") || Comment.Text.starts_with("#++");
					}
				)
			) {
				// 位置を読む指令を元の錨への復元
				Src.AddLeading(Anchor, std::move(OldLeading));
				// 既存のコメントを戻して終了
				return;
			}
			std::vector<std::string> ExistingLines;
			size_t DocIdx = OldLeading.size();
			// 囲み文書の既存本文を回収
			if(Style == DocSig::Style::Block) {
				// 最後の文書化コメントブロックを特定
				for(size_t Idx = 0; Idx < OldLeading.size(); ++Idx) if(DocSig::IsDocBlock(OldLeading[Idx].Text)) DocIdx = Idx;
				// 文書の説明を残す為，タグ整合前に論理行へ分解
				if(DocIdx < OldLeading.size()) ExistingLines = DocSig::BlockLogicalLines(OldLeading[DocIdx].Text);
			}
			bool HasRdocMarker = false;
			std::vector<CommentAttach> Unprefixed;
			// 行文書の本文と指令を分離
			if(Style != DocSig::Style::Block) for(CommentAttach &Comment : OldLeading) {
				const std::string_view Text(Comment.Text);
				if(!Text.starts_with('#') || ExistingLines.empty() && !HasRdocMarker && DocSig::IsMagicComment(Text)) {
					// 文書以外のコメントの原形保存
					Unprefixed.push_back(std::move(Comment));
					continue;
				}
				// RDoc の開始印は本文と分けて復元
				if(Text.size() > 1 && Text.find_first_not_of('#') == std::string_view::npos) {
					// 開始印を再配置対象として記録
					HasRdocMarker = true;
					continue;
				}
				// 行頭記号を除いた本文の収集
				ExistingLines.push_back(DocSig::StripHashLine(Text));
			}
			const std::vector<std::string> NewLines =
			DocSig::ReconcileLogicalLines(ExistingLines, DocSig::Extract(Src, Node, Language), ReturnTag, Language);
			std::vector<CommentAttach> NewLeading;
			// 囲み文書の生成
			if(Style == DocSig::Style::Block) {
				size_t Reserve = (NewLines.size() << 2) + 8;
				// 各論理行の本文長を加算
				for(const std::string &Line : NewLines) Reserve += Line.size();
				std::string Text = "/**\n";
				Text.reserve(Reserve);
				// 論理行への文書化記号と改行の付与
				for(const std::string &Line : NewLines) {
					Text += Line.empty() ? " *" : " * ";
					Text += Line;
					Text += '\n';
				}
				// 囲み文書の終端の確定
				Text += " */";
				CommentAttach DocAttach { std::move(Text), true, 0 };
				bool IsDocPlaced = false;
				for(size_t Idx = 0; Idx < OldLeading.size(); ++Idx) {
					if(Idx == DocIdx) {
						// 元の文書位置だけを新しい本文へ置換
						NewLeading.push_back(std::move(DocAttach));
						// 既存文書位置への配置完了
						IsDocPlaced = true;
					} else NewLeading.push_back(std::move(OldLeading[Idx]));
				}
				if(!IsDocPlaced) {
					size_t InsertAt = NewLeading.size();
					while(InsertAt && DocSig::IsNextLineDirective(NewLeading[InsertAt - 1].Text)) --InsertAt;
					NewLeading.insert(NewLeading.begin() + static_cast<std::ptrdiff_t>(InsertAt), std::move(DocAttach));
				}
				// 行文書の復元
			} else {
				// 指令と文書開始印を本文より先に復元
				NewLeading = std::move(Unprefixed);
				// RDoc 開始印の復元
				if(HasRdocMarker) NewLeading.push_back({ "##", true, 0 });
				// 空行も行コメントとして文書の内部への復元
				for(const std::string &Line : NewLines) NewLeading.push_back({ Line.empty() ? "#" : "# " + Line, true, 0 });
			}
			// 同一アンカーの先行コメント内順序を Seq 昇順で確定
			for(uint32_t SeqIdx = 0; SeqIdx < NewLeading.size(); ++SeqIdx) NewLeading[SeqIdx].Seq = SeqIdx;
			// 文書と先行コメント列の所有権を錨への復元
			Src.AddLeading(Anchor, std::move(NewLeading));
		}
	);
	// 終了
	return;
}
