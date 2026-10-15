/**
 * 編集する文法の型・名前・メンバの位置を，変更前の形と照合する
 * 計算量：照合する定義の大きさ N に対して O(N)
 * @param {unknown} actual 元の文法又は其の一部
 * @param {unknown} expected 編集が前提とする形
 * @param {string} location 不一致を報告する文法内のパス
 * @throws {Error} 型・名前・値又はメンバ数が前提と異なる場合
 */
export function assertGrammarShape(actual, expected, location) {
	// 文法の構造と値の照合
	if(expected !== null && typeof expected === "object") {
		if(actual === null || typeof actual !== "object" || Array.isArray(actual) !== Array.isArray(expected)) {
			throw new Error(`Grammar shape mismatch at ${location}: expected an object of the same kind`);
		}
		if(Array.isArray(expected) && actual.length != expected.length) {
			throw new Error(`Grammar shape mismatch at ${location}: expected ${expected.length} members, got ${actual.length}`);
		}
		for(const [key, value] of Object.entries(expected)) assertGrammarShape(actual[key], value, `${location}.${key}`);
	} else if(actual !== expected) throw new Error(`Grammar shape mismatch at ${location}: expected ${expected}, got ${actual}`);
	// 終了
	return;
}
