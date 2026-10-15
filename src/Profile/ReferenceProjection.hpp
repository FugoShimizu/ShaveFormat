#pragma once

#include "../Core/ControlFlow.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

class GrammarProfile;

namespace Core {
	// 入力の束縛を確認し，共通工程へ参照先と効果を渡す投影
	class ReferenceProjection : private ControlFlow {
	private:

		friend class SourceProjector;
		friend struct SyntaxNormalizerAccess;

		const GrammarProfile &Profile; // 標準対象を定める入力の定義
		SyntaxModel &Projected; // 投影結果の格納先
		bool HasMemberReassignment(const NodeId Reference, const std::string &Target) const; // 別名経由のメンバ再代入の確認関数
		void CollectTerminalMemberNames(const NodeId Root) const; // メンバの代入名の収集関数
		bool CanSkipMemberResolution(const NodeId Function) const; // 終端先の候補からメンバを除く判定関数

		std::shared_ptr<const ReferenceFacts> MakeMacroReference(
			const NodeId Head,
			const std::string_view OriginalName,
			const std::string_view ResolvedName,
			const bool IsUnknown = false
		) const; // マクロの役割と束縛を記録する関数

		std::shared_ptr<const ReferenceFacts> ResolveTokenMacro(NodeId Head) const; // トークン列のマクロ名の解決関数
		uint8_t GetCallEffects(const NodeId Node, bool &IsRegisteredMacro) const; // 入力の呼出効果の確定関数
		void CollectImportedPaths(const NodeId Import, const std::function<bool(NodeId)> &IsCustom, CallableNames &Names) const; // インポートの導入を投影する関数

	public:

		std::string ResolveCallName(
			NodeId Head,
			std::string_view Name,
			bool &HasContract,
			bool *IsUnknown = nullptr,
			const bool ShouldCheckMembers = true
		) const; // 供給元と標準契約を確認する解決関数

		std::string ResolveReference(const NodeId Node, const bool CanInvokeConstructor = false) const; // メンバの修飾の対象を解決する関数
		void RecordFacts(const NodeId Node, const bool InHandler) const; // 呼出効果と捕捉条件を確定する関数
		void RecordImports(); // インポートの供給元と戻値型を確定する関数

		ReferenceProjection(const GrammarProfile &Profile, SyntaxModel &Syntax); // コンストラクタ
	};
}
