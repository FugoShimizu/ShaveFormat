#pragma once

#include <cstddef>
#include <memory>
#include <vector>

class GrammarProfile;
class LaidOutCode;

struct ValidationContext;

namespace Core {
	class SourceProjector;
	class CommentRepository;

	struct OutputExpectation;

	// 投影へ渡す子の所有経路（本文と検証期待の取得権限は投影だけが持つ）
	class OwnedInputRoutes {
	private:

		friend class SourceProjector;

		// 子のコード・配置・検証の別々の所有物
		struct Child {
			size_t Ordinal; // 親ドキュメントの埋込の順
			const GrammarProfile *Profile; // 子の入力で確定した文法
			std::shared_ptr<const LaidOutCode> Code; // 親の中核へ渡すコードだけの経路
			std::shared_ptr<OutputExpectation> Syntax; // 子の正規化後の期待構文
			std::shared_ptr<const CommentRepository> Comments; // 子の配置・検証だけへ渡す所有記録
			std::shared_ptr<ValidationContext> Ctx; // 子の入力を所有する検証データ
		};

		std::vector<Child> Children; // 完成テキストを持たない入力所有の並び

	public:

		void Add(
			const GrammarProfile &Profile,
			std::shared_ptr<const LaidOutCode> Code,
			std::shared_ptr<OutputExpectation> Syntax,
			std::shared_ptr<const CommentRepository> Comments,
			std::shared_ptr<ValidationContext> Ctx,
			const size_t Ordinal
		); // 子の所有経路の受渡関数
	};
} // namespace Core
