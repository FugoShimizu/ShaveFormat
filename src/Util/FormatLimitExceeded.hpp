#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

inline constexpr size_t MaxInputBytes = 0X1000000; // 入出力の大きさの上限（３２ビット位置を見込み，16 MiB に抑える，整形仕様 9.1）
inline constexpr uint32_t MaxNestingDepth = 0X10000; // 構文及び中間表現のネストの段数の上限（整形仕様 9.2）

// 見送・失敗の理由（表示文言は `Text` に集約する）
struct FailureReason {
	enum Kind : uint8_t {
		None,
		InputSizeExceeded,
		UnsupportedEncoding,
		UnsupportedEncodingDeclaration, // エンコーディング宣言の値が対応外
		HeaderDialect,
		MarkupDoc,
		ParseTimeExceeded,
		StrayBackslash,
		ParserInitFailed,
		ParserNestingExceeded,
		MarkupCommentsUnsupported,
		ParserUnreadable,
		InputSyntaxError,
		UnrecognizedSyntax,
		GluedCommentsLimit,
		NonAssocOperators,
		UnknownOperatorPrec,
		ErrorZoneLost,
		LeadingTextLost,
		BraceAbsorbed,
		SyntaxBroken,
		SemanticsChanged,
		DirectiveChanged,
		DocChanged,
		BlankLineChanged,
		EmptyBlock,
		StructureChanged,
		LimitExceeded,
		FormatterDefect,
		WorkingMemoryExceeded,
		ResourceFailure
	}; // 見送・失敗の理由の分類（列挙値を Texts の添字に使い，ResourceFailure を表の末尾と照合する為，順を保持）

	Kind Val = None; // 理由の種類
	std::string_view Detail; // 例外が持つ表示文言
	static FailureReason FromException(const std::exception &Error); // 例外の原因から失敗理由を決める関数
	std::string_view GetText() const; // 表示文言の取得関数
	std::string_view GetRemedy() const; // 原文を保持した後の対処の取得関数
	bool IsValidation() const; // 検証差戻の理由かの判定関数
	bool IsResourceLimit() const; // リソース上限の理由かの判定関数

	/**
	 * 理由の種類の比較関数
	 * 計算量：O(1)
	 * @param Code 比較する種類
	 * @return 種類が一致すれば true
	 */
	constexpr bool operator==(const Kind Code) const {
		// 理由の種類の一致の返戻
		return Val == Code;
	}

	/**
	 * 理由の構築関数
	 * 計算量：O(1)
	 * @param Code 理由の種類
	 * @param Text 例外が持つ表示文言
	 */
	constexpr FailureReason(const Kind Code, const std::string_view Text = std::string_view()) : Val(Code), Detail(Text) {}
	/**
	 * 理由無の構築関数
	 */
	constexpr FailureReason() = default;
};

// 整形を打ち切る例外（手間・時間・出力の上限の見送と，フォーマッターの不具合・保持契約の不成立の失敗を理由の種類で区別し，理由毎に型を分けない）
struct FormatLimitExceeded : std::exception {
	std::string_view Reason; // 利用者へ示す理由のテキスト
	FailureReason::Kind Kind; // 理由の種類（上限の超過は見送，其の他は失敗，整形仕様 8.3）

	explicit FormatLimitExceeded(const std::string_view Text, const FailureReason::Kind Code = FailureReason::LimitExceeded); // 打切の例外の構築関数
};
