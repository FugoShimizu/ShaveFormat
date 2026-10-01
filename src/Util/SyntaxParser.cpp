#include "SyntaxParser.hpp"
#include "../Profile/GrammarProfile.hpp"
#include "ParseAdapter.hpp"
#include "ParsedTree.hpp"
#include "TextEdit.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * 解析器解放関数
 * @param Target 解放する解析器
 */
void SyntaxParser::Deleter::operator()(TSParser *const Target) const {
	// 所有する解析器の解放
	ts_parser_delete(Target);
	// 終了
	return;
}

/**
 * コンストラクタ（文法を設定しない解析器の構築）
 */
SyntaxParser::SyntaxParser() : Handle(ts_parser_new()) {}

/**
 * 文法の設定関数
 * @param Grammar 設定する文法
 * @return 実行時ライブラリと互換で設定出来れば true
 */
bool SyntaxParser::Use(const TSLanguage *const Grammar) {
	// 文法を設定出来たかの返戻
	return Handle && ts_parser_set_language(Handle.get(), Grammar);
}

/**
 * 解析器へ文面を渡す読出関数
 * @param Payload 読ませる文面
 * @param ByteIndex 読み始める位置
 * @param Position 読み始める行と桁（位置で決まる読出をしない為に使わない）
 * @param BytesRead 返した長さの格納先
 * @return 読み始めた位置からの残り全体（末尾では長さ０）
 */
static const char *ReadWholeSource(
	void *const Payload,
	const uint32_t ByteIndex,
	[[maybe_unused]] const TSPoint Position,
	uint32_t *const BytesRead
) {
	// 解析対象の文面
	const std::string_view &Text = *static_cast<const std::string_view *>(Payload);
	if(ByteIndex >= Text.size()) {
		// 末尾での読出量
		*BytesRead = 0;
		// 末尾に達し読む物が無い事の返戻
		return "";
	}
	// 返す残余の長さ
	*BytesRead = static_cast<uint32_t>(Text.size() - ByteIndex);
	// 残り全体の返戻
	return Text.data() + ByteIndex;
}

// 読取量を数える入力（挿入を重ねた本文を読み，解析器へ渡したバイト数を累計する）
struct CountedSource {
	std::string_view Text; // 挿入前の本文
	std::span<const uint32_t> Insertions; // 本文へ重ねた空白の挿入後の位置（昇順）
	size_t *Read; // 渡したバイト数の累計の格納先
	std::array<char, 64> Chunk; // 挿入を跨ぐ塊を組み立てる領域（UTF-8 の最長の字を跨いで読み直せる一度に渡す上限）
};

/**
 * 読取量を数える解析器の入力読出関数（整形仕様 9.3 の実読取量）
 * 読み始めた位置から上限迄を渡し，渡したバイト数を累計する（差分解析で再利用した区画は読まれない為に数えない）
 * 挿入を跨がない塊は本文の連続領域を其の儘渡し，跨ぐ塊だけを組み立てる
 * 計算量：挿入の数 K と渡すバイト数 B に対し O(log K + B)
 * @param Payload 読取量を数える入力
 * @param ByteIndex 挿入後の本文で読み始める位置
 * @param Position 読み始める行と列（位置だけで読む為に使わない）
 * @param BytesRead 返した長さの格納先
 * @return 読み始めた位置からの上限迄の本文（末尾では長さ０）
 */
static const char *ReadCountedChunk(
	void *const Payload,
	const uint32_t ByteIndex,
	[[maybe_unused]] const TSPoint Position,
	uint32_t *const BytesRead
) {
	// 読出量と挿入位置の対応付け
	CountedSource &Source = *static_cast<CountedSource *>(Payload);
	const size_t Size = Source.Text.size() + Source.Insertions.size();
	*BytesRead = ByteIndex >= Size ? 0 : static_cast<uint32_t>(std::min(Source.Chunk.size(), Size - ByteIndex));
	*Source.Read += *BytesRead;
	std::span<const uint32_t>::iterator Next = std::lower_bound(Source.Insertions.begin(), Source.Insertions.end(), ByteIndex);
	// 読み始める位置より前の挿入の数
	size_t Passed = static_cast<size_t>(Next - Source.Insertions.begin());
	if(Next == Source.Insertions.end() || *Next >= ByteIndex + *BytesRead) {
		// 挿入を跨がない塊の本文の連続領域の返戻
		return *BytesRead ? Source.Text.data() + ByteIndex - Passed : "";
	}
	// 挿入した空白を含む塊の組立
	for(size_t Offset = 0; Offset < *BytesRead; ++Offset) {
		if(Next != Source.Insertions.end() && *Next == ByteIndex + Offset) {
			Source.Chunk[Offset] = ' ';
			++Next;
			++Passed;
		} else Source.Chunk[Offset] = Source.Text[ByteIndex + Offset - Passed];
	}
	// 組み立てた塊の返戻
	return Source.Chunk.data();
}

/**
 * 解析の打切の判定関数
 * @param State 解析器が渡す進捗（期限を積んだ荷を持つ）
 * @return 期限を過ぎて居れば true（解析器は其処で解析を捨てる）
 */
static bool HasReachedParseDeadline(TSParseState *const State) {
	// 呼出側が設定した解析期限
	const std::chrono::steady_clock::time_point &Until =
	*static_cast<const std::chrono::steady_clock::time_point *>(State->payload);
	// 期限の超過の返戻
	return std::chrono::steady_clock::now() > Until;
}

/**
 * 期限付の構文解析関数
 * @param Old 増分解析の土台（全体解析では nullptr）
 * @param Text 解析する文面
 * @param OutIsExpired 期限の超過の格納先（超過した時だけ真を書く）
 * @param ReadBytes 解析器へ渡したバイト数の累計の格納先（数えない時は nullptr）
 * @param Insertions 文面へ重ねて読む空白の挿入後の位置（昇順，読取量を数える時だけ使う）
 * @return 解析した構文木（期限の超過と失敗では nullptr）
 */
TSTree *SyntaxParser::Parse(
	const TSTree *const Old,
	const std::string_view Text,
	bool &OutIsExpired,
	size_t *const ReadBytes,
	const std::span<const uint32_t> Insertions
) const {
	// 今回の解析期限
	const std::chrono::steady_clock::time_point Until = std::chrono::steady_clock::now() +
	std::chrono::microseconds(DeadlineBaseMicros + static_cast<int64_t>(Text.size() + Insertions.size()));
	// 全文読出の入力か読取量を数える入力
	CountedSource Counted { Text, Insertions, ReadBytes, {} };
	const TSInput Input = ReadBytes ?
	TSInput{ &Counted, ReadCountedChunk, TSInputEncodingUTF8, nullptr } :
	TSInput{ const_cast<std::string_view *>(&Text), ReadWholeSource, TSInputEncodingUTF8, nullptr };
	// 期限監視の指定
	const TSParseOptions Options { const_cast<std::chrono::steady_clock::time_point *>(&Until), HasReachedParseDeadline };
	// 期限付の解析結果
	TSTree *const Tree = ts_parser_parse_with_options(Handle.get(), Old, Input, Options);
	// 次の解析前に於ける打切後の解析器状態の初期化
	if(!Tree) {
		// 中断した解析状態の初期化
		ts_parser_reset(Handle.get());
		// 期限超過の通知
		if(std::chrono::steady_clock::now() > Until) OutIsExpired = true;
	}
	// 解析した構文木の返戻
	return Tree;
}

/**
 * 許容破損の判定関数（解析器の未対応に由来し，書掛ではない `ERROR` / `MISSING`）
 * @param Src 判定対象を含むソース
 * @param Node 判定対象のノード
 * @param Ancestors Node の祖先の並び（走査の道筋から渡す，末尾が親）
 * @return 解析器の追随遅れに由来するなら true
 */
bool SyntaxParser::IsToleratedBreak(const ParsedTree &Src, const TSNode Node, const std::span<const TSNode> Ancestors) {
	// CSS の空のカスタムプロパティ値を許容する事の返戻（解析器の補う整数値に依らず成立する）
	if(ParseAdapter::IsEmptyCssCustomValue(Src, Node)) return true;
	// 型を持たない節点は許容対象でない事の返戻
	if(ts_node_is_null(Node)) return false;
	// Sass の正当な `as *` 取込・先頭結合子に解析器が補う空選択子の `MISSING` を許容する事の返戻（解析器の追随遅れに由来する空のネスト選択子）
	if(Src.Is(Node, Core::Role::NestingSelector)) return true;
	// Sass の空の仮引数・実引数に対する誤った `MISSING` / `ERROR` を許容
	if(!Ancestors.empty()) {
		// 直近の祖先節点
		const TSNode Parent = Ancestors.back();
		// 解析器の既知の誤読（仮引数の祖父で調べる空の引数，取込の空の括弧，未対応の仮置きの名前）を許容する事の返戻
		if(
			const TSNode Host =
			Src.Is(Parent, Core::Role::Parameter) && !Src.Len(Parent) && Ancestors.size() > 2 ? Ancestors[Ancestors.size() - 3] : TSNode{};
			ts_node_is_missing(Node) && !ts_node_is_null(Host) && Src.Is(Host, Core::Role::ParameterRule) ||
			Src.Is(Node, Core::Role::Error) && Src.Is(Parent, Core::Role::IncludeStatementNode) && ts_node_child_count(Node) == 2 &&
			Src.Is(ts_node_child(Node, 0), Core::Role::OpeningParenToken) ||
			Src.Is(Node, Core::Role::Identifier) && Src.Is(Parent, Core::Role::PlaceholderNode)
		) return true;
	}
	// 構文誤りの外と SCSS の文法の外の返戻
	if(!Src.Is(Node, Core::Role::LaggingGrammarError)) return false;
	// Sass のモジュールの節の語 (`as` / `show` / `hide`) で始まる `ERROR` かの判定
	const auto IsModuleClause = [&Src](const TSNode Error) -> bool {
		const std::string_view Text = Src.View(Error);
		const std::vector<std::string_view> &Words = Src.GetReading().ModuleClauseWords;
		// 節の語の後が区切りかの返戻
		return std::any_of(
			Words.begin(),
			Words.end(),
			[&Text](const std::string_view Word) -> bool {
				// 語の境界で一致するかの返戻
				return Text.starts_with(Word) && (Text.size() == Word.size() || std::isspace(static_cast<unsigned char>(Text[Word.size()])));
			}
		);
	};
	// モジュールの規則の中の，節の `ERROR` と其の内側の `ERROR` だけ許容する事の返戻（空選択子と対で許し，真の破損は保持）
	for(size_t Idx = 0; Idx < Ancestors.size(); ++Idx) {
		if(!Src.Is(Ancestors[Idx], Core::Role::ModuleRule)) {
			// モジュールの規則は最上位の項目の為，走査の始まりの下で構文誤りでない祖先の内側には無い事の返戻
			if(Idx && !Src.Is(Ancestors[Idx], Core::Role::Error)) return false;
			continue;
		}
		// 節の `ERROR` の内側かの返戻
		return IsModuleClause(Node) || std::any_of(
			Ancestors.begin() + static_cast<std::ptrdiff_t>(Idx) + 1,
			Ancestors.end(),
			[&Src, &IsModuleClause](const TSNode Ancestor) -> bool {
				// 節の `ERROR` かの返戻
				return Src.Is(Ancestor, Core::Role::Error) && IsModuleClause(Ancestor);
			}
		);
	}
	// モジュールの規則の外の返戻
	return false;
}

/**
 * 構文破損ノードの判定関数（`ERROR` / `MISSING`，但し文字列リテラルの内側は除く）
 * @param Src 判定対象を含むソース
 * @param Node 判定対象のノード
 * @param Ancestors Node の祖先の並び（走査の道筋から渡す，末尾が親，根では空）
 * @return 構文の破損と見做すなら true
 */
bool SyntaxParser::IsBrokenNode(const ParsedTree &Src, const TSNode Node, const std::span<const TSNode> Ancestors) {
	// 欠落節点か
	const bool IsMissing = ts_node_is_missing(Node);
	// `ERROR` でも `MISSING` でもない節点と解析器の追随遅れを破損としない事の返戻
	if(!IsMissing && !Src.Is(Node, Core::Role::Error) || IsToleratedBreak(Src, Node, Ancestors)) return false;
	// 文字列直下の `ERROR` だけ許容し，`MISSING` 又は文字列外を破損とする事の返戻
	return IsMissing || Ancestors.empty() || !Src.Is(Ancestors.back(), Core::Role::StringLikeAll);
}

/**
 * JSONC の末尾コンマを表す破損からコンマを得る関数
 * @param Src 判定対象を含むソース
 * @param Node 判定対象の `ERROR` 又は `MISSING`
 * @return 配列又は物体の最後の要素に続くコンマの字句（該当しなければ空）
 */
TSNode SyntaxParser::JsonTrailingCommaToken(const ParsedTree &Src, const TSNode Node) {
	// 欠落として補われたコンマか
	const bool IsMissing = ts_node_is_missing(Node);
	if(!IsMissing && (!Src.Is(Node, Core::Role::Error) || !Src.GetRules().IsSpelling(Src.View(Node), Core::Word::ListSeparator))) {
		// JSONC の末尾コンマに対応して居ない解析器が立てる形でない事の返戻
		return TSNode{};
	}
	// 破損を含む親節点
	const TSNode Parent = ts_node_parent(Node);
	// 親を持たない破損は列の末尾ではない事の返戻
	if(ts_node_is_null(Parent)) return TSNode{};
	// JSON の配列と物体以外に現れたコンマは許容しない事の返戻
	if(!Src.Is(Parent, Core::Role::ArrayLiteral) && !Src.Is(Parent, Core::Role::ObjectValue)) return TSNode{};
	// 対象通過済と後続値有無の旗
	bool IsAfter = false, HasValueAfter = false;
	// 直前の値と其れに続くコンマ
	TSNode LastValue {}, Comma {};
	ForEachChild(
		Parent,
		[&](const TSNode Child) -> void {
			// 対象破損へ到達した場合
			if(ts_node_eq(Child, Node)) {
				// 対象破損通過済の記録
				IsAfter = true;
				// 終了
				return;
			}
			// 対象破損より後の子の場合
			if(IsAfter) {
				if(ts_node_is_named(Child) && !Src.Is(Child, Core::Role::Comment)) HasValueAfter = true;
				// 終了
				return;
			}
			if(const bool IsNamed = ts_node_is_named(Child); IsNamed && !Src.Is(Child, Core::Role::Comment)) {
				// 対象破損より前の最後の値
				LastValue = Child;
				// 新しい値に対応するコンマの初期化
				Comma = {};
			} else if(!IsNamed && !ts_node_is_null(LastValue) && Src.Is(Child, Core::Role::CommaToken)) Comma = Child;
		}
	);
	if(!IsMissing && !ts_node_is_null(LastValue) && ts_node_child_count(Node) == 1) Comma = ts_node_child(Node, 0);
	// 空の列の `[,]` / `{,}` は末尾コンマでなく構文誤りで，後続に値が在るコンマも区切りの誤りとして許容しない事の返戻
	return IsAfter && !ts_node_is_null(LastValue) && !HasValueAfter ? Comma : TSNode{};
}

/**
 * 壊れたノード (`ERROR` / `MISSING`) の計数関数
 * @param Src 走査対象を含むソース
 * @param Node 走査対象のルートノード
 * @return 壊れたノードの個数
 */
uint32_t SyntaxParser::CountBrokenNodes(const ParsedTree &Src, const TSNode Node) {
	// 破損節点の計数値
	uint32_t Count = 0;
	HasDescendantWithAncestorsOf(
		Node,
		[&Src, &Count](const TSNode Cur, const std::span<const TSNode> Ancestors) -> bool {
			// 破損節点の計数
			if(IsBrokenNode(Src, Cur, Ancestors)) ++Count;
			// 全ノードを数える迄走査を続ける事の返戻
			return false;
		}
	);
	// 壊れたノードの個数の返戻
	return Count;
}

/**
 * C の文法に反する関数定義の計数関数
 * tree-sitter-c は C++ の `class A { ... };` を型 `class`・宣言子 `A` の関数定義と読み，破損を立てない
 * C の関数定義の宣言子は関数の宣言子に限る為，其れ以外の宣言子を持つ関数定義を C としての破損に数える
 * @param Src 走査対象のソース（C として解析済）
 * @return 宣言子が関数の宣言子でない関数定義の個数
 */
uint32_t SyntaxParser::CountMalformedCDefinitions(const ParsedTree &Src) {
	// 不正な関数定義の計数値
	uint32_t Count = 0;
	WalkAst(
		Src.GetRoot(),
		[&Src, &Count](const TSNode Node) -> void {
			// C の関数定義でない節点の場合の終了
			if(!Src.Is(Node, Core::Role::FunctionDefinition)) return;
			// 包装した宣言子の解包
			TSNode Declarator = Src.FieldChild(Node, Core::ChildRole::Declarator);
			while(!ts_node_is_null(Declarator) && Src.Is(Declarator, Core::Role::NestingDeclarator)) {
				const TSNode Inner = Src.FieldChild(Declarator, Core::ChildRole::Declarator);
				Declarator = ts_node_is_null(Inner) ? ts_node_named_child(Declarator, 0) : Inner;
			}
			if(ts_node_is_null(Declarator) || !Src.Is(Declarator, Core::Role::FunctionDeclarator)) ++Count;
		}
	);
	// 関数の宣言子を持たない関数定義の個数の返戻
	return Count;
}

/**
 * 行継続の直後のコメントの有無の判定関数
 * 解析器は行継続の後のコメントを誤りを立てずに受理するが，処理系は継続する行へコメントを置けない（Python 等）
 * 整形すると継続の逆斜線を失い別の構文誤りへ書き換わる為，入力の誤りとして扱う（整形仕様 4.6, 8.2）
 * @param Src 走査対象のソース
 * @return 行継続の直後にコメントが在れば true
 */
bool SyntaxParser::HasContinuationComment(const ParsedTree &Src) {
	// 行継続の直後のコメントの有無
	bool HasComment = false;
	WalkAst(
		Src.GetRoot(),
		[&Src, &HasComment](const TSNode Node) -> void {
			// 行継続でない節点を読飛ばす場合の終了
			if(!Src.Is(Node, Core::Role::LineContinuation)) return;
			// 直後の兄弟
			const TSNode Next = ts_node_next_sibling(Node);
			// 直後がコメントである事の記録
			HasComment = HasComment || !ts_node_is_null(Next) && Src.Is(Next, Core::Role::Comment);
		}
	);
	// 行継続の直後のコメントの有無の返戻
	return HasComment;
}

/**
 * 矢印関数を被演算子に置いた誤りの有無の判定関数
 * 矢印関数は括弧で包まない限り参照・添字・呼出の被演算子に為れず，行の終わりを挟まない `() => {}[0]` は言語処理系が拒む（JavaScript 等）
 * 解析器は誤りを立てずに受理する為，入力の誤りとして扱う（整形仕様 4.6, 8.2）
 * @param Src 走査対象のソース
 * @return 行の終わりを挟まない矢印関数の被演算子が在れば true
 */
bool SyntaxParser::HasArrowOperandMisuse(const ParsedTree &Src) {
	// 誤りの有無
	bool HasMisuse = false;
	WalkAst(
		Src.GetRoot(),
		[&Src, &HasMisuse](const TSNode Node) -> void {
			// 被演算子を先頭子に取らない節点を読飛ばす場合の終了
			if(!Src.Is(Node, Core::Role::OperandHeadedAccess)) return;
			const TSNode Object = ts_node_child(Node, 0);
			// 矢印関数でない被演算子を読飛ばす場合の終了
			if(ts_node_is_null(Object) || !Src.Is(Object, Core::Role::ArrowFunction)) return;
			const TSNode Next = ts_node_child(Node, 1);
			// 次の字句を持たない節点を読飛ばす場合の終了
			if(ts_node_is_null(Next)) return;
			const uint32_t From = Src.End(Object), To = Src.Start(Next);
			// 行の終わりを挟まない事の記録（自動セミコロン挿入が効かない）
			HasMisuse = HasMisuse || std::string_view(Src.data() + From, To - From).find('\n') == std::string_view::npos;
		}
	);
	// 誤りの有無の返戻
	return HasMisuse;
}

/**
 * 境界を確定出来ない誤り区画の有無の判定関数（整形仕様 4.6）
 * 誤りの回復が文書を組み直す文法では全ての区画を，其の他の文法でも文書の中に閉じない括弧が在れば区画を信頼しない
 * 字下げ又は語で塊を閉じる文法では，誤りを含む最も大きい１行の構文が其の行の全体を占め，区画が語の字句を含まない時だけ塊の境界を確定出来る
 * 其の他の文法は波括弧等の記号で塊を閉じる為，区画毎の境界を保護の照合（整形仕様 8.3）に委ねる
 * 計算量：節点数 N と行の長さ L に対し O(N L)
 * @param Src 解析済の原稿
 * @return 境界を確定出来ない誤り区画が在れば true
 */
bool SyntaxParser::HasUntrustedErrorZone(const ParsedTree &Src) {
	const ReadingRules &Rules = Src.GetReading();
	// 誤りの回復が文書を組み直す文法の返戻
	if(Rules.RecoveryRestructuresDocument) return true;
	// 文書の中で閉じない括弧の有無（括弧の対を失った誤り区画の外側の境界は確定出来ない）
	int Depth = 0;
	WalkChildrenCursor(
		Src.GetRoot(),
		[&](const TSNode Token) -> bool {
			if(
				Src.Is(Token, Core::Role::OpeningBraceToken) || Src.Is(Token, Core::Role::OpeningBracketToken) ||
				Src.Is(Token, Core::Role::OpeningParenToken)
			) ++Depth;
			else if(
				Src.Is(Token, Core::Role::ClosingBraceToken) || Src.Is(Token, Core::Role::ClosingParenToken) ||
				Src.Is(Token, Core::Role::ClosingSquareToken)
			) --Depth;
			// 子を辿る事の返戻
			return true;
		}
	);
	// 閉じない括弧の在る文書の返戻
	if(Depth > 0) return true;
	// 記号で塊を閉じる文法の返戻
	if(!Rules.HasIndentBlocks && !Rules.HasKeywordClosedBlocks) return false;
	const std::string &Text = Src;
	// 境界を確定出来ない区画の有無の返戻
	return HasDescendantOf(
		Src.GetRoot(),
		[&](const TSNode Node) -> bool {
			// 誤り区画でない節点の返戻
			if(!Src.Is(Node, Core::Role::Error)) return false;
			// 誤りを含む最も大きい１行の構文
			TSNode Line = Node;
			for(TSNode Up = ts_node_parent(Line); !ts_node_is_null(Up) && Src.View(Up).find('\n') == std::string_view::npos;) {
				Line = Up;
				Up = ts_node_parent(Up);
			}
			const uint32_t Lead = TextEdit::SkipSpLeft(Text, Src.Start(Line), Rules);
			uint32_t Tail = TextEdit::SkipSpRight(Text, Src.End(Line), Rules);
			// 行末のコメントは行の終わりへ含める
			if(
				const TSNode Trailing = ts_node_descendant_for_byte_range(Src.GetRoot(), Tail, Tail);
				Tail < Text.size() && Src.Is(Trailing, Core::Role::Comment) && Src.Start(Trailing) == Tail
			) Tail = TextEdit::SkipSpRight(Text, Src.End(Trailing), Rules);
			const bool IsWholeLine = Src.View(Line).find('\n') == std::string_view::npos && (!Lead || Text[Lead - 1] == '\n') &&
			(Tail >= Text.size() || Text[Tail] == '\n' || Text[Tail] == '\r');
			// 行の全体を占めない区画と，塊を開閉し得る語の字句を含む区画かの返戻
			return !IsWholeLine || HasDescendantOf(
				Node,
				[&Src](const TSNode Leaf) -> bool {
					// 名前付節点又は子を持つ節点の除外の返戻
					if(ts_node_is_named(Leaf) || ts_node_child_count(Leaf)) return false;
					const std::string_view Word = Src.View(Leaf);
					// 空でない語の字句かの返戻
					return !Word.empty() && std::isalpha(static_cast<unsigned char>(Word.front()));
				}
			);
		}
	);
}

/**
 * 本体を失ったインデントの塊の有無の判定関数
 * tree-sitter-python はインデントの無い本体（`def f():` の次の行のインデント無の文）を空の塊と読み，誤りを立てない（Python は拒む）
 * @param Src 解析済のソース（インデントで塊を成す言語）
 * @return 幅０の塊 (`block`) が有れば true
 */
bool SyntaxParser::HasEmptyBlock(const ParsedTree &Src) {
	// 幅０の塊の有無の返戻
	return Src.IsParsed() && HasDescendantOf(
		Src.GetRoot(),
		[&Src](const TSNode Node) -> bool {
			// 本体の無い塊かの返戻
			return !Src.Len(Node) && Src.Is(Node, Core::Role::LexicalBlock);
		}
	);
}

/**
 * 解析結果の受理の判定関数（整形仕様 3.4 の Parse，4.6）
 * 解析器が読めない原稿，処理系が拒む構文，本文・インデントを保てない `ERROR`・`MISSING` を受理せず，其の他の誤り区画は部分整形へ回す
 * @param Profile 入力で確定した文法
 * @param Input 適応を済ませた読取専用の入力
 * @return 適応時の解析結果の写しと受理の判定
 */
ParsedTree SyntaxParser::Parse(const GrammarProfile &Profile, const ParseInput &Input) {
	// 適応段階で止めた入力の受理区分の引継
	ParsedTree Src(*Input.Src);
	if(Input.Decision != ParseDecision::Continue) {
		const ParseDecision Decision = Input.Decision == ParseDecision::Skip ? ParseDecision::Skip : ParseDecision::Fail;
		Src.SetParseDecision(Decision, Input.Failure, false);
		// 適応が既に止めた結果の返戻
		return Src;
	}
	const ReadingRules &Rules = Profile.GetReading();
	// 解析器が読めない字面と処理系が拒む構文の検査
	if(
		ts_node_has_error(Src.GetRoot()) && std::memchr(Src.CodeText().data(), '\0', Src.CodeText().size()) ||
		Rules.AcceptsArrowOperand && HasArrowOperandMisuse(Src) || Rules.AcceptsContinuationComment && HasContinuationComment(Src)
	) {
		Src.SetParseDecision(ParseDecision::Skip, FailureReason::ParserUnreadable, false);
		// 解析器が NUL で読めない原稿と，解析器は受理するが処理系が拒む原稿の返戻
		return Src;
	}
	// 入力破損の種類別の旗
	bool HasInputMissing = false, HasJsxError = false, HasOpenString = false, HasInputError = false;
	// 未閉じ文字列を同じ行の引用符対と区別し，索引を再利用して残りの本文・インデントを保護
	std::vector<uint32_t> LineEnds;
	std::unordered_map<std::string_view, std::vector<uint32_t>> QuotePositions;
	const auto IsUnclosed = [&Src, &LineEnds, &QuotePositions](const uint32_t Start, const std::string_view Quote) -> bool {
		// 指定位置の引用符の閉鎖判定
		const std::string &Text = Src;
		if(LineEnds.empty()) {
			// 原文中の行末位置の収集
			for(size_t Found = Text.find('\n'); Found != std::string::npos; Found = Text.find('\n', Found + 1)) LineEnds.push_back(Found);
			// 原文終端の行末登録
			LineEnds.push_back(static_cast<uint32_t>(Text.size()));
		}
		const auto [Iter, IsFresh] = QuotePositions.try_emplace(Quote);
		std::vector<uint32_t> &Positions = Iter->second;
		// 当該引用符の出現位置収集
		if(IsFresh) for(size_t Found = Text.find(Quote); Found != std::string::npos; Found = Text.find(Quote, Found + Quote.size())) {
			Positions.push_back(Found);
		}
		// 対象位置の属する行末
		const std::vector<uint32_t>::const_iterator LineEnd = std::lower_bound(LineEnds.begin(), LineEnds.end(), Start);
		// 開き候補の位置
		const std::vector<uint32_t>::const_iterator Here = std::lower_bound(Positions.begin(), Positions.end(), Start);
		// 開き候補の後続引用符
		const std::vector<uint32_t>::const_iterator Close = std::lower_bound(Here, Positions.cend(), Start + Quote.size());
		// 行の手前の引用符が偶数個（開きの引用符）で，行末迄に閉じの引用符が無いかの返戻
		return !(Here - std::lower_bound(Positions.cbegin(), Here, LineEnd == LineEnds.begin() ? 0 : *(LineEnd - 1) + 1) & 1) &&
		(Close == Positions.end() || *Close >= *LineEnd);
	};
	// 接頭辞後の未閉じ判定（生・三重・逐語文字列は残り全体で閉じを探索）
	// 行を跨げる文字列の引用符の長さ（其の引用符で始まらなければ０）
	const auto MultilineQuoteLength = [&Rules](const std::string_view Text) -> size_t {
		// 一致した引用符の長さの返戻
		for(const std::string_view Quote : Rules.MultilineQuotes) if(Text.starts_with(Quote)) return Quote.size();
		// 行を跨げる引用符で始まらない事の返戻
		return 0;
	};
	const auto OpensUnclosed = [&Src, &IsUnclosed, &MultilineQuoteLength, &Rules](const uint32_t Start) -> bool {
		// 文字列接頭辞に対応する閉じ引用符の探索
		const std::string &Whole = Src;
		const std::string_view Text = std::string_view(Whole).substr(Start, 0X20);
		size_t Prefix = 0;
		for(size_t Length = std::min<size_t>(Text.size(), 3); Length; --Length) {
			if(std::ranges::find(Rules.StringPrefixes, Text.substr(0, Length)) != Rules.StringPrefixes.end()) {
				// 一致した接頭辞長の記録
				Prefix = Length;
				break;
			}
		}
		const std::string_view Rest = Text.substr(Prefix), Head = Text.substr(0, Prefix);
		// 接頭辞が文字列の形の一覧に在るか
		const auto IsHeadIn = [&Head](const std::vector<std::string_view> &Heads) -> bool {
			// 一覧の接頭辞かの返戻
			return std::ranges::find(Heads, Head) != Heads.end();
		};
		// 接頭辞の後に字句が無ければ文字列の開きではない事の返戻
		if(Rest.empty()) return false;
		std::string Close;
		size_t OpenLength = 1;
		if(Rest.front() == '#' && (!Prefix || IsHeadIn(Rules.HashQuotePrefixes))) {
			const size_t Hashes = Rest.find_first_not_of('#');
			// `#` の並びの後が引用符でなければ文字列の開きではない事の返戻
			if(Hashes == std::string_view::npos || Rest[Hashes] != '"') return false;
			const size_t Quotes = std::max<size_t>(MultilineQuoteLength(Rest.substr(Hashes)), 1);
			Close.assign(Quotes, '"').append(Hashes, '#');
			OpenLength = Hashes + Quotes;
		// 引用符無の返戻
		} else if(Rest.front() != '"' && Rest.front() != '\'' && Rest.front() != '`') return false;
		else if(IsHeadIn(Rules.RawStringPrefixes) && Rest.front() == '"') {
			const size_t Paren = Rest.find('(');
			// 区切りの `(` が無ければ生文字列の開きではない事の返戻
			if(Paren == std::string_view::npos) return false;
			Close.assign(1, ')').append(Rest.substr(1, Paren - 1)).push_back('"');
			OpenLength = Paren + 1;
		} else if(MultilineQuoteLength(Rest)) {
			OpenLength = std::min(Rest.find_first_not_of(Rest.front()), Rest.size());
			Close.assign(OpenLength, Rest.front());
		} else if(IsHeadIn(Rules.VerbatimStringPrefixes)) Close = "\"";
		// 通常の引用符が閉じて居ないかの返戻
		else return IsUnclosed(Start + static_cast<uint32_t>(Prefix), Rest.substr(0, 1));
		// 行を跨げる文字列の閉じの区切りがファイルの残りに無いかの返戻
		return Whole.find(Close, Start + Prefix + OpenLength) == std::string::npos;
	};
	// JSX は最外容器を１度だけ調べ，`ERROR` 毎の祖先探索の二乗化を回避
	uint32_t JsxCheckedEnd = 0;
	// 既存 `ERROR` は部分整形で許容し新規分だけ破壊判定（O(1) の旗で健全なら全木走査を省略）
	if(Src.IsParsed() && ts_node_has_error(Src.GetRoot())) {
		HasDescendantWithAncestorsOf(
			Src.GetRoot(),
			[&](const TSNode Cur, const std::span<const TSNode> Ancestors) -> bool {
				// 型を持たない節点の内側は調べない事の返戻
				if(ts_node_is_null(Cur)) return false;
				if(Src.Start(Cur) >= JsxCheckedEnd && ts_node_has_error(Cur) && Src.Is(Cur, Core::Role::RenderedChildren)) {
					// 検査済 JSX 容器範囲の更新
					JsxCheckedEnd = Src.End(Cur);
					HasJsxError = HasDescendantWithAncestorsOf(
						Cur,
						[&Src](const TSNode Inner, const std::span<const TSNode> InnerAncestors) -> bool {
							// 破損した `ERROR` かの返戻
							return Src.Is(Inner, Core::Role::Error) && IsBrokenNode(Src, Inner, InnerAncestors);
						}
					);
				}
				// 破損の判定は `IsBrokenNode` に集約する（文字列リテラル直下の `ERROR` は中身の字面で構文は壊れて居ない）
				const bool IsTreeBroken =
				IsBrokenNode(Src, Cur, Ancestors) && !(Rules.ToleratesTrailingCommaError && !ts_node_is_null(JsonTrailingCommaToken(Src, Cur)));
				if(IsTreeBroken && Src.Is(Cur, Core::Role::Error)) {
					HasOpenString = OpensUnclosed(Src.Start(Cur));
					if(!HasOpenString) {
						ForEachChild(
							Cur,
							[&](const TSNode Child) -> bool {
								// C++ の `R"` 等の接頭辞付開始字句を含む引用符候補
								if(Src.Is(Child, Core::Role::QuoteToken)) HasOpenString = OpensUnclosed(Src.Start(Child));
								// 閉じない文字列を見付ける迄次の子へ進む事の返戻
								return !HasOpenString;
							}
						);
					}
				}
				// 未対応構文でも出る `ERROR` と区別し，閉じ括弧等の不足を表す `MISSING` を記録
				if(ts_node_is_missing(Cur) && IsTreeBroken) HasInputMissing = true;
				if(IsTreeBroken) HasInputError = true;
				// 整形しない事が確定したら早期に打切の返戻（HasDescendantOf は true 返戻で打切となる）
				return HasInputMissing && HasInputError || HasOpenString || HasJsxError;
			}
		);
	}
	// 宣言の値の後の行コメントの開きを解析器が読めず字句に分けて呑み，其の行の終わりを誤り区画の外の字句の間に置いた CSS / SCSS は，行の終わりの空白を整えると後続の波括弧がコメントへ入り，境界を確定出来ない為の見送（整形仕様 4.6, 10.4）
	const auto IsProtectedAt = [&Src](const uint32_t Position) -> bool {
		// 誤り区画の中の位置の返戻
		for(
			TSNode Up = ts_node_descendant_for_byte_range(Src.GetRoot(), Position, Position + 1);
			!ts_node_is_null(Up);
			Up = ts_node_parent(Up)
		) if(Src.Is(Up, Core::Role::Error)) return true;
		// 誤り区画の外の位置の返戻
		return false;
	};
	if(
		Rules.AbsorbsRulesIntoValueComments && HasInputError && HasDescendantOf(
			Src.GetRoot(),
			[&Src, &IsProtectedAt](const TSNode Node) -> bool {
				TSNode Previous {};
				// 隣り合う２つの `/` の字句を子に持ち，其の行の終わりが誤り区画の外に在る誤り区画かの返戻
				return Src.Is(Node, Core::Role::Error) && HasChildOf(
					Node,
					[&Src, &Previous, &IsProtectedAt](const TSNode Child) -> bool {
						const bool IsJoined =
						!ts_node_is_null(Previous) && Src.Is(Previous, Core::Role::SlashToken) && Src.Is(Child, Core::Role::SlashToken) &&
						Src.End(Previous) == Src.Start(Child);
						Previous = Child;
						const size_t LineEnd = IsJoined ? Src.CodeText().find('\n', Src.End(Child)) : std::string_view::npos;
						// 行コメントの開きの字句の並びで，行の終わりを整え得るかの返戻
						return LineEnd != std::string_view::npos && !IsProtectedAt(static_cast<uint32_t>(LineEnd));
					}
				);
			}
		)
	) {
		Src.SetParseDecision(ParseDecision::Skip, FailureReason::BraceAbsorbed, HasInputError);
		// 境界を確定出来ない入力の見送の返戻
		return Src;
	}
	// 本文・インデントを保てない破損だけ見送り，解析器未対応の正当な構文は許容
	if(
		HasInputMissing || HasJsxError || HasOpenString || Rules.HasIndentBlocks && HasEmptyBlock(Src) || HasInputError && (
			HasUntrustedErrorZone(Src) || Rules.MisreadsCustomProperties && (
				Src.Is(Src.GetRoot(), Core::Role::Error) || HasDescendantOf(
					Src.GetRoot(),
					[&Src, &Rules](const TSNode Node) -> bool {
						// CSS の `ERROR` 節点の関数呼出判定
						if(Src.Is(Node, Core::Role::Error)) {
							// CSS 関数名の照合字面
							const std::string_view Url = Rules.UrlFunctionHead;
							// 関数名を値と誤読した url の直前３字からの探索
							const uint32_t From = Src.Start(Node) - std::min<uint32_t>(Src.Start(Node), 3);
							// url 誤読の探索範囲
							const std::string_view Text(Src.data() + From, Src.End(Node) - From);
							// 大小を問わず url 内のコメント誤読を検出した結果の返戻（コメント無なら逐語保持可能）
							return std::search(
								Text.begin(),
								Text.end(),
								Url.begin(),
								Url.end(),
								[](const char Actual, const char Expected) -> bool {
									// 英字の大小を揃えた字の一致の返戻
									return std::tolower(static_cast<unsigned char>(Actual)) == Expected;
								}
							) != Text.end() && HasDescendantOf(
								Node,
								[&Src](const TSNode Inner) -> bool {
									// コメントかの返戻
									return Src.Is(Inner, Core::Role::Comment);
								}
							);
						}
						// 要素の足りない規則は誤読の形でない事の返戻
						if(!Src.Is(Node, Core::Role::SelectorRule) || ts_node_named_child_count(Node) < 3) return false;
						const std::string_view Selector = Src.View(ts_node_named_child(Node, 0));
						const TSNode Colon = ts_node_named_child(Node, 1);
						// `--x` だけの選択子・`:` の `ERROR`・値の波括弧が並ぶ規則（カスタムプロパティの誤読）かの返戻
						return Selector.starts_with(Rules.CustomPropertyHead) && Selector.find_first_of(" \t\n") == std::string_view::npos &&
						Selector.find(Rules.PropertySeparator) == std::string_view::npos && Src.Is(Colon, Core::Role::Error) &&
						Src.View(Colon) == Rules.PropertySeparator;
					}
				)
			) || Rules.MisreadsEscapedRegexEnd && HasDescendantOf(
				Src.GetRoot(),
				[&Src](const TSNode Node) -> bool {
					// 正規表現でない事の返戻
					if(!Src.Is(Node, Core::Role::RegexLiteralNode)) return false;
					const std::string_view Text = Src.View(Node);
					// `/` で終わらない（空の範囲を含む）正規表現でない事の返戻
					if(Text.size() < 2 || Text.back() != '/') return false;
					// 正規表現末尾の逃避記号数
					size_t Escapes = 0;
					for(size_t At = Text.size() - 1; At && Text[At - 1] == '\\'; --At) ++Escapes;
					// 終わりの `/` が逃がされた（前に奇数個の `\` が続く）かの返戻
					return Escapes & 1;
				}
			) || Rules.DropsTokensOnRecovery && HasDescendantOf(
				Src.GetRoot(),
				[&Src](const TSNode Node) -> bool {
					// Kotlin が回復時に落とした字句を子の間隙から検出し，else 脱落等による文の誤結合を防止
					uint32_t PreviousEnd = std::numeric_limits<uint32_t>::max();
					// 子の間に落とした字句が在るかの返戻
					return HasChildOf(
						Node,
						[&](const TSNode Child) -> bool {
							// 現在の子の開始と直前の終端
							const uint32_t Start = Src.Start(Child), Previous = std::exchange(PreviousEnd, Src.End(Child));
							// 前の子との間に落とした字句が在るかの返戻
							return Previous < Start &&
							std::string_view(Src.data() + Previous, Start - Previous).find_first_not_of(" \t\r\n;") != std::string_view::npos;
						}
					);
				}
			) || Rules.AbsorbsVerbatimOnError && (
				Src.CodeText().find(Rules.VerbatimStart) != std::string_view::npos || HasChildOf(
					Src.GetRoot(),
					[&Src](const TSNode Child) -> bool {
						// 地の文の子かの返戻
						return Src.Is(Child, Core::Role::InlineText);
					}
				)
			)
		)
	) {
		Src.SetParseDecision(ParseDecision::Skip, FailureReason::ParserUnreadable, HasInputError);
		// 構造が信用出来ない為，整形未実施の返戻
		return Src;
	}
	Src.SetParseDecision(ParseDecision::Continue, FailureReason::None, HasInputError);
	// 整形続行を呼出側に通知の返戻
	return Src;
}
