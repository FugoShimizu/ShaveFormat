#pragma once

#include "NodeId.hpp"
#include "SyntaxRole.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Core {
	struct CallableIntroduction; // 呼出の供給元
	struct CallablePositions; // 供給元の可視位置

	using CallableNames = std::unordered_map<uint32_t, std::vector<CallableIntroduction>>; // 名前毎の供給元
	using CallableMembers = std::unordered_map<std::string, CallablePositions>; // 修飾毎の供給元の位置
	using CallableImports = std::unordered_map<NodeId, CallableMembers>; // インポート範囲毎の供給元の位置

	// 呼出の先頭名の導入と，修飾名の供給元が同名のモジュールである事実
	struct CallableIntroduction {
		NodeId Name = NoNode; // 導入する名前のトークン
		std::string Member {}; // メンバへの代入なら先頭名の後の修飾（通常の導入は空）
		std::string Target {}; // インポート元の完全名（通常の宣言は空）
		NodeId AliasSrc = NoNode; // 単純な別名の初期値又は代入元
		NodeId Function = NoNode; // 戻値の型・非復帰の契約を持つ宣言
		bool IsModuleImport = false; // 先頭名と同じモジュールを直接取り込む導入か
		bool IsTerminatingImport = false; // 既知の終端呼出を供給するインポートか
		bool IsScopeWide = false; // 記述の位置に依らず範囲全体で有効な導入か
		NodeId SrcRoot = NoNode; // 既知のインポート先の先頭名の参照
		std::optional<Word> ImportedResultType = std::nullopt; // 既知の供給元から同名で取り込む戻値型の語
		bool IsCustomDefinition = false; // 関数の本文を持つ独自定義か
		bool IsUncertain = false; // 供給値を一意に追えない束縛変更か
		bool IsValueImport = false; // メンバの値を取得するインポートか
		bool IsSequential = false; // 範囲全体でも記述位置以降に限る導入か
	};

	// 宣言順の位置と独自定義の累積数
	struct CallablePosition {
		size_t Decl;
		size_t CustomCount;
	};

	// 同じ供給元とメンバの宣言の可視性
	struct CallablePositions {
		mutable size_t OrderedEnd = SIZE_MAX; // 最後の照会の使用位置が属する宣言間の区間（未照会は最大値）
		mutable size_t WideEnd = SIZE_MAX; // 範囲全体の導入での同じ区間
		mutable size_t SequentialEnd = SIZE_MAX; // 記述位置以降の導入での同じ区間
		std::vector<CallablePosition> Ordered;
		std::vector<CallablePosition> Wide;
		std::vector<CallablePosition> Sequential;
	};

	struct CallableResolution {
		std::string Name;
		NodeId Reference = NoNode; // 関数の取得位置（モジュールの使用位置を継承する場合は無）
		bool HasContract = false;
		bool IsUnknown = false;
		bool ShouldCheckMembers = false;
	}; // 導入時に確定した供給値の解決結果

	// 呼出の解決と遮蔽の照会が共有する名前と位置
	struct CallableIndex {
		using MemberPositions = std::vector<std::pair<uint32_t, size_t>>; // 修飾への代入を持つ名前の番号と宣言列の位置

		mutable std::unordered_map<uint64_t, NodeId> GuardPaths; // 独立した制御の境界迄の共有する所属
		mutable std::unordered_map<NodeId, NodeId> AliasSources; // 連鎖代入の右辺の終端
		mutable std::unordered_map<NodeId, std::unordered_map<std::string, CallableResolution>> Resolutions; // 供給元と修飾毎の解決
		mutable std::unordered_map<std::string, bool> EntireMemberReassignments; // 根の範囲全体から読む標準対象毎の再代入の要約
		mutable std::unordered_map<uint32_t, CallableImports> Positions; // 照会された名前だけのインポート範囲と修飾毎の宣言位置
		CallableNames Names;
		std::unordered_map<uint32_t, std::unordered_set<size_t>> MemberSizes; // 明示的に代入した修飾部分の長さ
		std::unordered_map<std::string, MemberPositions> MemberAssignments; // 修飾毎の導入名の番号と宣言列の位置
	};
}
