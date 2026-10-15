#! /usr/bin/env python3
# 深い木の走査の作業列
from __future__ import annotations

from typing import Any, Generator, TypeAlias

Work: TypeAlias = Generator["Work", Any, Any]

def evaluate(work: Work) -> Any:
	"""子の作業を明示した列で評価し，結果を待つ親へ渡す
	子の要求の順序と，各作業内の評価の順序を保持する
	計算量：作業数 N に対して O(N)，追加メモリは深さ D に対して O(D)
	引数：
		work: 根の作業
	戻値：
		根の作業の結果
	送出例外：
		Exception: 作業から伝わる例外を其の儘送出する場合
	"""
	# 子の要求と，完了した結果の受渡
	pending: list[Work] = [work]
	result: Any = None
	while pending:
		try:
			child: Work = pending[-1].send(result)
		except StopIteration as e:
			pending.pop()
			result = e.value
		else:
			pending.append(child)
			result = None
	# 根の結果の返戻
	return result
