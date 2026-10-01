#pragma once

#include "ControlFlow.hpp"
#include "SyntaxModel.hpp"
#include <string>
#include <vector>

namespace Core {
	// 関数の署名（整形仕様 3.2，仮引数名列・戻値の有無・無名引数の有無）
	struct Signature {
		std::vector<std::string> Params;
		bool HasReturn = false;
		bool HasUnnamedParam = false; // C/C++ の無名引数（`const T &` 等）の有無
		bool ReturnsFromSignature = false; // 戻値の型の宣言から HasReturn が確定したか（偽は本体の返戻文からの推論か根拠無で，既存の戻値の説明を保つ）
		std::string Name; // 関数名（Go の godoc の書出し）
		std::vector<std::string> TypeParams; // 型仮引数名（KDoc 等が仮引数の札で説明する物）
	};

	// 関数の名前・仮引数・戻値の構文情報の抽出クラス（整形仕様 3.1, 7.4）
	class SignatureExtractor {
	public:

		static NodeId LeadOf(const SyntaxModel &Syntax, const NodeId Node); // 前置の属性を含む節点の頭の取得関数
		static NodeId DocAnchor(const SyntaxModel &Syntax, const NodeId FuncNode); // 文書の錨の取得関数
		static bool IsDocumentable(const SyntaxModel &Syntax, const NodeId Node); // 文書を付ける定義かの判定関数
		static Signature Extract(const SyntaxModel &Syntax, const NodeId FuncNode); // 署名の抽出関数

		SignatureExtractor() = delete; // コンストラクタ（禁止）
	};
} // namespace Core
