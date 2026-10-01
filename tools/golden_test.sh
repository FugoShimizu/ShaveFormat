#!/bin/bash
# 期待出力との整形回帰試験
# 入力を既定幅で整形し，同名の期待結果とバイト単位で比較する
# 同名の .chars ファイルが在る場合は其の行幅で整形する（狭い行幅の行分割も不動点迄検証する）
# 同名の .warn ファイルが在る場合は静的検査の警告の一覧（`行:桁: 本文` 形式）も併せて比較する
# .warn が無いケースでは警告を比較しない
# 同名の .skip ファイルが在る場合は其の理由で見送られる事を検証する
# 同名の .fail ファイルが在る場合は，其の理由で整形を差し戻し（終了コード 1），原文を其の儘返す事を検証する（整形仕様 8.3）
# 併せて整形結果を再度整形し，二巡目で変わらない事（不動点）も検証する
# .skip が無いのに見送られたら失敗とする（見送は .out が入力と同一に為る為，比較だけでは退行を検出出来ない）
# `.partial` の有無と部分整形経路の使用が一致する事を検証する
# 期待結果は仕様から人が作る
# 整形結果から期待結果を作る手段は用意しない
# 実行結果を期待結果にすると其の時点の実装の誤りが正解として固定され，此の検査が退行を検出出来なく為る
# 検査が落ちたら，実装と期待結果のどちらが仕様に沿うかを条文と照らして１件ずつ吟味する
# 使い方：
# tools/golden_test.sh # 全件チェック
# tools/golden_test.sh cpp javascript # 言語フィルタ
# SHAVEFMT_BIN=build_release/shavefmt tools/golden_test.sh # 検査する整形器の指定（既定は build/shavefmt）
# SHAVEFMT_JOBS=4 tools/golden_test.sh # 同時に走らせる検査の数（既定は２，作業機の負荷の上限）

set -u;
export LC_ALL=C;

ROOT="$(cd "$(dirname "$0")/.." && pwd)";
FMT="${SHAVEFMT_BIN:-${ROOT}/build/shavefmt}";
JOBS="${SHAVEFMT_JOBS:-2}";
GOLDEN="${ROOT}/test_data/golden";
FILTERS=("$@");

if [ ! -x "${FMT}" ]; then
	echo "missing formatter: ${FMT}" >&2;
	exit 1;
fi
if [ ! -d "${GOLDEN}" ]; then
	echo "missing fixtures dir: ${GOLDEN}" >&2;
	exit 1;
fi

# 存在しない言語を指定して０件の検査を成功扱いしない
if (( ${#FILTERS[@]} )); then
	for filter in "${FILTERS[@]}"; do
		case "${filter}" in
			""|.|..|*/*)
				echo "invalid language filter: ${filter}" >&2;
				exit 1;
				;;
		esac
		if [ ! -d "${GOLDEN}/${filter}" ]; then
			echo "unknown language filter: ${filter}" >&2;
			exit 1;
		fi
	done
fi

filtered() {
	local lang="$1";
	if (( ! ${#FILTERS[@]} )); then return 0; fi
	local f;
	for f in "${FILTERS[@]}"; do
		if [ "${f}" = "${lang}" ]; then return 0; fi
	done
	return 1;
}

# 検体１件の検査（失敗の報告は標準エラーへ一纏めに出し，失敗なら `1` を返す）
check_case() {
	local in_file="$1";
	local lang_dir="${in_file%/*}";
	local lang="${lang_dir##*/}";
	local case_name="${lang}/$(basename "${in_file%.in}")";
	local TMP_DIR;
	TMP_DIR="$(mktemp -d "${TMP_ROOT}/case.XXXXXX")";
	local TMP="${TMP_DIR}/out" WARN_TMP="${TMP_DIR}/warn" WARN_ACT="${TMP_DIR}/warn_actual" IDEM_TMP="${TMP_DIR}/idempotent";
	local FAILED=0;
	# 並行する検体の報告が混ざらない様に，報告を溜めて最後に一度に出す
	{
		run_case;
	} 2> "${TMP_DIR}/report";
	cat "${TMP_DIR}/report" >&2;
	rm -rf "${TMP_DIR}";
	(( !FAILED ));
}
# 検体の検査の本体（check_case の局所変数を読み書きし，検査を打ち切る所で返る）
run_case() {
	local out_file="${in_file%.in}.out";
	local chars_file="${in_file%.in}.chars";
	local WIDTH=();
	if [ -f "${chars_file}" ]; then WIDTH=(--chars "$(cat "${chars_file}")"); fi
	"${FMT}" --stdin --lang "${lang}" ${WIDTH[@]+"${WIDTH[@]}"} < "${in_file}" > "${TMP}" 2> "${WARN_TMP}";
	format_status=$?;
	# 意味を保てない候補の差戻は，理由の通知と原文の返戻の双方を検証し，整形結果の検査は行わない
	fail_file="${in_file%.in}.fail";
	if [ -f "${fail_file}" ]; then
		if (( format_status != 1 )) || ! grep -qxF "Error: formatting would $(cat "${fail_file}"), left unchanged: <stdin>" "${WARN_TMP}"; then
			echo "FAIL ${case_name}: expected the formatting to be refused ($(cat "${fail_file}"))" >&2;
			cat "${WARN_TMP}" >&2;
			FAILED=1;
		elif ! cmp -s "${TMP}" "${in_file}"; then
			echo "FAIL ${case_name}: the refused formatting did not return the input unchanged" >&2;
			FAILED=1;
		fi
		return;
	fi
	if (( format_status )); then
		echo "FAIL ${case_name}: shavefmt exited non-zero" >&2;
		# 原因の手掛りが失われない様に，退避した標準エラーを其の儘出す
		cat "${WARN_TMP}" >&2;
		FAILED=1;
		return;
	fi
	if [ ! -f "${out_file}" ]; then
		echo "FAIL ${case_name}: missing .out (write the expected output from the specification)" >&2;
		FAILED=1;
		return;
	fi
	# 見送は .out が入力と同一に為る為，整形結果の比較だけでは退行を検出出来ない
	# 期待する見送理由を .skip へ置き，其の有無と内容の双方を検証する
	skip_file="${in_file%.in}.skip";
	skip_line="$(sed -n "s|^\[skip\] <stdin>: ||p" "${WARN_TMP}")";
	if [ -f "${skip_file}" ]; then
		if [ -z "${skip_line}" ]; then
			echo "FAIL ${case_name}: expected skip but the file was formatted" >&2;
			FAILED=1;
			return;
		fi
		if [ "${skip_line}" != "$(cat "${skip_file}")" ]; then
			echo "FAIL ${case_name}: skip reason changed" >&2;
			echo "  expected: $(cat "${skip_file}")" >&2;
			echo "  actual:   ${skip_line}" >&2;
			FAILED=1;
			return;
		fi
	elif [ -n "${skip_line}" ]; then
		echo "FAIL ${case_name}: unexpected skip (${skip_line})" >&2;
		FAILED=1;
		return;
	fi
	if grep -q "formatting is partial" "${WARN_TMP}"; then partial=1; else partial=0; fi
	if [ -f "${in_file%.in}.partial" ] && (( !partial )); then
		echo "FAIL ${case_name}: expected partial formatting of a syntax error but the whole file was formatted" >&2;
		FAILED=1;
		return;
	elif [ ! -f "${in_file%.in}.partial" ] && [ "${partial}" = "1" ]; then
		echo "FAIL ${case_name}: unexpected partial formatting (the input is read as having syntax errors)" >&2;
		FAILED=1;
		return;
	fi
	if ! cmp -s "${TMP}" "${out_file}"; then
		echo "FAIL ${case_name}:" >&2;
		diff -u "${out_file}" "${TMP}" | sed -n "1,40p" >&2;
		FAILED=1;
		return;
	fi
	# 初回が無変更なら同じ入力の再実行と為る為，不動点の追加検証は不要
	if ! cmp -s "${in_file}" "${TMP}"; then
		# 二巡目の不動点検証（整形結果を再度整形しても変わらない事）
		# 出力の比較だけでは，非不動点の一巡目の姿を期待値として固定して仕舞い，振動が検出出来ない
		# 二巡目の見送は原文を返して一致として通る為，失敗に数える
		if ! "${FMT}" --stdin --lang "${lang}" --fail-on-skip ${WIDTH[@]+"${WIDTH[@]}"} < "${TMP}" > "${IDEM_TMP}" 2> /dev/null; then
			echo "FAIL ${case_name} (idempotence): second pass exited non-zero or skipped" >&2;
			FAILED=1;
			return;
		fi
		if ! cmp -s "${TMP}" "${IDEM_TMP}"; then
			echo "FAIL ${case_name} (idempotence): second pass differs" >&2;
			diff -u "${TMP}" "${IDEM_TMP}" | sed -n "1,20p" >&2;
			FAILED=1;
			return;
		fi
	fi
	# 静的検査の警告の回帰検証（期待ファイルが在るケースのみ）
	warn_file="${in_file%.in}.warn";
	if [ -f "${warn_file}" ]; then
		sed -e "s|^\[warn\] <stdin>:||" -e "s|^\[warn\] ||" "${WARN_TMP}" > "${WARN_ACT}";
		if ! cmp -s "${WARN_ACT}" "${warn_file}"; then
			echo "FAIL ${case_name} (warnings):" >&2;
			diff -u "${warn_file}" "${WARN_ACT}" | sed -n "1,40p" >&2;
			FAILED=1;
		fi
	fi
	return;
}

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/golden.XXXXXX")";
trap 'rm -rf "${TMP_ROOT}"' EXIT;
export FMT TMP_ROOT;
export -f check_case run_case;
CASES="${TMP_ROOT}/cases";
: > "${CASES}";
for lang_dir in "${GOLDEN}"/*; do
	[ -d "${lang_dir}" ] || continue;
	if ! filtered "$(basename "${lang_dir}")"; then continue; fi
	for in_file in "${lang_dir}"/*.in; do
		[ -f "${in_file}" ] && printf '%s\0' "${in_file}" >> "${CASES}";
	done
done
TOTAL="$(tr -cd '\0' < "${CASES}" | wc -c | tr -d ' ')";
# 失敗した検体の数（各検体は失敗の時だけ印を１行出す）
FAIL="$(xargs -0 -n 1 -P "${JOBS}" bash -c 'check_case "$1" || echo failed' _ < "${CASES}" | wc -l | tr -d ' ')";

if (( !TOTAL )); then
	echo "golden: no test cases selected" >&2;
	exit 1;
fi

# 文書が引用する名前の実在（関数を別の層へ移した時に仕様書の名前空間が古い儘残るのを防ぐ）
if ! bash "$(dirname "${BASH_SOURCE[0]}")/verify_doc_symbols.sh"; then
	: "$((++FAIL))";
fi
# 使う標準記号に対する取込の不足（手元の推移的な取込に頼ると他環境で壊れる）
if ! python3 "$(dirname "${BASH_SOURCE[0]}")/verify_includes.py"; then
	: "$((++FAIL))";
fi
# 期待結果其の物が条文に適うか（整形器を動かさず `.out` だけを読み，実行結果を期待結果にする過ちを防ぐ）
if ! python3 "$(dirname "${BASH_SOURCE[0]}")/verify_golden_spec.py"; then
	: "$((++FAIL))";
fi
# 経路の分離・コメント非依存・空行の不変性（整形仕様 1.2, 11.1）は，中核の出力の観測を検体毎と指示子を含む記法の並びで比べて確かめる
if ! python3 "$(dirname "${BASH_SOURCE[0]}")/verify_comment_separation.py" "${FMT}"; then
	: "$((++FAIL))";
fi
# 検体に無い形の冪等性（整形仕様 1.3）は，人が書いた検体だけでは覆えない為に入力を組み立てて確かめる
if ! python3 "$(dirname "${BASH_SOURCE[0]}")/verify_generated_idempotence.py" "${FMT}"; then
	: "$((++FAIL))";
fi
# 行幅に依らない冪等性（整形仕様 1.3）とコメントの深さ（整形仕様 7.6），行末へ空白を足した入力の終了コードの契約（整形仕様 10.4）を検体の変種で確かめる
if ! python3 "$(dirname "${BASH_SOURCE[0]}")/verify_case_variants.py" "${FMT}"; then
	: "$((++FAIL))";
fi
# 工程の間の遮断（整形仕様 3）は出力に現れない為，依存関係の側から機械的に確かめる
if ! python3 "$(dirname "${BASH_SOURCE[0]}")/verify_design_invariants.py"; then
	: "$((++FAIL))";
fi
echo "golden: ${TOTAL} cases, ${FAIL} failed";

(( !FAIL ));
