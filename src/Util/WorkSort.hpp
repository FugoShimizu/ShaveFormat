#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace WorkSort {
	/**
	 * 隣接した要素を前から１度ずつ比較する整列済の判定関数
	 * 計算量：要素数 N に対し O(N)
	 * @param Values 要素列
	 * @param IsBefore 昇順の比較と其の実作業の計上
	 * @return 逆順の隣接要素が無ければ true
	 * @throws std::exception 比較関数から伝わるエラーの場合
	 */
	template<class T, class Less> bool IsSorted(const std::span<const T> Values, const Less &IsBefore) {
		for(size_t Idx = 1; Idx < Values.size(); ++Idx) if(IsBefore(Values[Idx], Values[Idx - 1])) return false;
		// 整列済である事の返戻
		return true;
	}

	/**
	 * 比較する位置を列の中央に固定した境界の探索関数
	 * 比較と其の実作業の計上は受け取った述語に任せる
	 * 計算量：要素数 N に対し O(log(N + 1))
	 * @param Values 整列済の要素列
	 * @param Value 探索する境界
	 * @param IsBefore 境界より前かの比較と其の実作業の計上
	 * @return 境界以上の最初の要素の位置（無ければ要素数）
	 * @throws std::exception 比較関数から伝わるエラーの場合
	 */
	template<class T, class Key, class Less> size_t LowerBound(
		const std::span<const T> Values,
		const Key &Value,
		const Less &IsBefore
	) {
		size_t Begin = 0, Count = Values.size();
		while(Count) {
			const size_t Half = Count >> 1, Middle = Begin + Half;
			if(IsBefore(Values[Middle], Value)) {
				Begin = Middle + 1;
				Count -= Half + 1;
			} else Count = Half;
		}
		// 境界以上の最初の位置の返戻
		return Begin;
	}

	/**
	 * 比較と移動の順が処理系に依存しない安定整列関数
	 * 比較の計上は述語，要素の生成と移動の計上は受け取った関数に任せる
	 * 計算量：要素数 N に対し O(N log(N + 1))，作業メモリ O(N)
	 * @param Values 整列する要素列
	 * @param IsBefore 昇順の比較と其の実作業の計上
	 * @param Charge 要素の生成と移動の作業単位の計上
	 * @throws std::bad_alloc 作業列を確保出来ない場合
	 * @throws std::length_error 作業列の要素数上限を超える場合
	 * @throws std::exception 要素の操作又は計上関数から伝わるエラーの場合
	 */
	template<class T, class Less, class ChargeWork> void Stable(
		const std::span<T> Values,
		const Less &IsBefore,
		const ChargeWork &Charge
	) {
		// 空又は単一要素の列は移動しない
		const size_t Count = Values.size();
		// 空又は単一要素の列での終了
		if(Count < 2) return;
		Charge(Count);
		std::vector<T> Buffer(Count);
		bool FromBuffer = false;
		// 隣接する２列を，等しい要素では左を先にして合成する
		for(size_t Width = 1; Width < Count;) {
			const std::span<T> Source = FromBuffer ? std::span<T>(Buffer) : Values;
			const std::span<T> Target = FromBuffer ? Values : std::span<T>(Buffer);
			for(size_t Begin = 0; Begin < Count;) {
				const size_t Middle = Begin + std::min(Width, Count - Begin), End = Middle + std::min(Width, Count - Middle);
				size_t Left = Begin, Right = Middle, Output = Begin;
				while(Left < Middle && Right < End) {
					Charge(1);
					Target[Output++] = std::move(IsBefore(Source[Right], Source[Left]) ? Source[Right++] : Source[Left++]);
				}
				while(Left < Middle) {
					Charge(1);
					Target[Output++] = std::move(Source[Left++]);
				}
				while(Right < End) {
					Charge(1);
					Target[Output++] = std::move(Source[Right++]);
				}
				Begin = End;
			}
			FromBuffer ^= true;
			// 最後の合成で桁上りを生じさせない
			if(Width > (Count >> 1)) break;
			Width <<= 1;
		}
		if(FromBuffer) for(size_t Idx = 0; Idx < Count; ++Idx) {
			Charge(1);
			Values[Idx] = std::move(Buffer[Idx]);
		}
		// 終了
		return;
	}
} // namespace WorkSort
