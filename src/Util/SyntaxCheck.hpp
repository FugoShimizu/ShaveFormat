#pragma once

#include "Lang.hpp"
#include "TsSource.hpp"
#include <cstdint>
#include <span>
#include <string_view>

// 入力と整形後で共通の構文破損判定を提供するクラス
class SyntaxCheck {
private: // 個別節点の内部判定

	static bool IsToleratedBreak(const TSSource &Src, const TSNode Node, const std::span<const TSNode> Ancestors); // 許容破損の判定関数
	static bool IsBrokenNode(const TSSource &Src, const TSNode Node, const std::span<const TSNode> Ancestors); // 構文破損ノードの判定関数

public: // 構文検査の公開入口

	static TSNode JsonTrailingCommaToken(const TSSource &Src, const TSNode Node); // JSONC の末尾コンマを表す破損からコンマを得る関数
	static uint32_t CountBrokenNodes(const TSSource &Src, const TSNode Node); // 壊れたノード（`ERROR` / `MISSING`）の計数関数
	static TSPoint FirstBrokenPoint(const TSSource &Src); // 最初の壊れたノードの位置の取得関数
	static uint32_t CountMalformedCDefinitions(const TSSource &Src); // C の文法に反する関数定義の計数関数
	static bool HasErrorDescendant(const TSSource &Src, const TSNode Node); // 壊れたノードの有無判定関数
	static bool ScanInputErrorStates(const TSSource &Src, const Lang Language, bool &HasInputError, std::string_view &SkipReason); // 入力エラー状態の収集関数

	SyntaxCheck() = delete; // コンストラクタ（禁止）
};
