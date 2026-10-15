#pragma once

#include "ControlFlow.hpp"
#include "NodeId.hpp"
#include "SyntaxModel.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Core {
	// 名前の無い宣言形式の仮引数の位置とシグネチャ内の順
	struct UnnamedParam {
		NodeId Node = NoNode; // 名前の無い仮引数全体のノード
		uint32_t Ordinal = 0; // 名前付の仮引数も含めた１始まりの番号
	};

	// シグネチャの仮引数の位置・分解構造・束縛の対応
	struct ParamShape {
		NodeId Node = NoNode; // 分解又は型を含む仮引数全体
		uint32_t Ordinal = 0; // 実際のシグネチャ順の１始まりの番号
		std::vector<NodeId> Bindings {}; // 原文順の導入名（型・キー・外部名は含めない）
		bool IsDestructure = false; // 単一名で表せない分解の仮引数か
		bool HasRest = false; // 残余の指定を含むか
		bool IsUncertain = false; // 束縛の構造を確定出来ないか
		size_t NameBegin = 0; // Params 内で此の仮引数の導入名が始まる位置
	};

	// 関数のシグネチャ（整形仕様 3.2，仮引数名列・戻値の有無・無名引数の有無）
	struct Signature {
		ReturnShape Return { ReturnKind::Void, ReturnEvidence::Unknown }; // 同じ構文から１度求めた戻値の区分と根拠
		std::vector<std::string> Params;
		std::vector<ParamShape> ParamShapes {}; // ドキュメントと照合する仮引数の構造
		std::vector<UnnamedParam> UnnamedParams; // 名前の無い実在の仮引数（`void` と `...` を除く）
		bool HasReturn = false;
		bool HasUnnamedParam = false; // 無名又は単一の名前で列挙出来ない仮引数の有無
		std::string Name; // 関数名（Go の godoc の書出）
		std::vector<std::string> TypeParams; // テンプレート・ジェネリックの仮引数名（C++ の非型仮引数も含む）
	};

	// 関数の名前・仮引数・戻値の構文情報の抽出クラス（整形仕様 3.1, 7.4）
	class SignatureExtractor {
	private:

		static NodeId GetDocAnchor(const SyntaxModel &Syntax, const NodeId FunctionNode); // ドキュメントのアンカーの取得関数
		static bool IsDocumentable(const SyntaxModel &Syntax, const NodeId Node, const NodeId Anchor); // ドキュメントを付ける定義かの判定関数

	public:

		// 名前付の定義とシグネチャの抽出操作
		static NodeId GetLeadOf(const SyntaxModel &Syntax, const NodeId Node); // 前置の属性を含むノードの頭の取得関数
		static DocumentationFact GetDocumentationFact(const SyntaxModel &Syntax, const NodeId Node); // 同じ構文の定義の判定とアンカーを共有する取得関数
		static Signature Extract(const SyntaxModel &Syntax, const ControlFlow &Flow, const NodeId FunctionNode); // シグネチャの抽出関数

		SignatureExtractor() = delete; // コンストラクタ（禁止）
	};
} // namespace Core
