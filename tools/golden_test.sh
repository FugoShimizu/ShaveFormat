#!/bin/bash
# 期待出力との整形回帰テスト
# 入力を既定幅で整形し，同名の期待結果とバイト単位で比較する
# 同名の .chars ファイルが在る場合は其の行幅で整形する（狭い行幅の行分割も不動点迄検証する）
# 同名の .warn ファイルが在る場合は静的解析の警告の一覧（`行:桁: 本文` 形式）も併せて比較する
# .warn が無いケースでは警告を比較しない
# 同名の .skip ファイルが在る場合は其の理由で見送られる事を検証する
# 同名の .fail ファイルが在る場合は，其の理由で整形を差し戻し（終了コード 1），原文を其の儘返す事を検証する（整形仕様 8.3）
# 併せて整形結果を再度整形し，２巡目で変わらない事（不動点）も検証する
# .skip が無いのに見送られたら失敗とする（見送は .out が入力と同一に為る為，比較だけでは退行を検出出来ない）
# `.partial` の有無と部分整形経路の使用が一致する事を検証する
# 期待結果は仕様から人が作る
# 整形結果から期待結果を作る手段は用意しない
# 実行結果を期待結果にすると其の時点の実装のエラーが正解として固定され，此の検査が退行を検出出来なく為る
# 検査が落ちたら，実装と期待結果の何方が仕様に沿うかを条文と照らして１件ずつ吟味する
# 使方：
# tools/golden_test.sh # 全件チェック
# tools/golden_test.sh cpp javascript # 言語フィルタ
# SHAVEFMT_BIN=build_release/shavefmt tools/golden_test.sh # 検査するフォーマッターの指定（既定は build/shavefmt）
# SHAVEFMT_JOBS=4 tools/golden_test.sh # 同時に走らせる検査の数（既定は２，作業機の負荷の上限）
set -u;
export LC_ALL=C;
ROOT="$(cd "$(dirname "$0")/.." && pwd)";
FMT="${SHAVEFMT_BIN:-${ROOT}/build/shavefmt}";
JOBS="${SHAVEFMT_JOBS:-2}";
CASE_TIMEOUT="${SHAVEFMT_CASE_TIMEOUT:-30}";
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

# 段階の所要時間の表示（検査自体の時間を見て最適化する為）
timed() {
	local name="$1";
	shift;
	local start="${SECONDS}";
	"$@";
	local status=$?;
	echo "time ${name}: $((SECONDS - start))s" >&2;
	return "${status}";
}

# テストケース１件の検査（失敗の報告は標準エラーへ一纏めに出し，失敗なら `1` を返す）
check_case() {
	local input_file="$1";
	local lang_dir="${input_file%/*}";
	local lang="${lang_dir##*/}";
	local case_name="${lang}/$(basename "${input_file%.in}")";
	local TMP_DIR;
	if ! TMP_DIR="$(mktemp -d "${TMP_ROOT}/case.XXXXXX")"; then
		echo "FAIL ${case_name}: could not create the temporary directory" >&2;
		return 1;
	fi
	local ACTUAL_OUTPUT="${TMP_DIR}/out" WARNING_OUTPUT="${TMP_DIR}/warn" ACTUAL_WARNING="${TMP_DIR}/warn_actual" \
		IDEMPOTENT_OUTPUT="${TMP_DIR}/idempotent";
	local FAILED=0;
	# 並行するテストケースの報告が混ざらない様に，報告を溜めて最後に１度に出す
	if ! { run_case; } 2> "${TMP_DIR}/report"; then
		echo "FAIL ${case_name}: could not complete the case or record its diagnostics" >&2;
		FAILED=1;
	fi
	if ! cat "${TMP_DIR}/report" >&2; then FAILED=1; fi
	if ! rm -rf "${TMP_DIR}"; then FAILED=1; fi
	(( !FAILED ));
}

# テストケースの検査の本体（check_case のローカル変数を読み書きし，検査を打ち切る所で返る）
run_case() {
	local output_file="${input_file%.in}.out";
	local chars_file="${input_file%.in}.chars";
	local WIDTH=();
	if [ -f "${chars_file}" ]; then WIDTH=(--chars "$(cat "${chars_file}")"); fi
	python3 "${ROOT}/tools/run_limited.py" --seconds "${CASE_TIMEOUT}" -- "${FMT}" --stdin --lang "${lang}" ${WIDTH[@]+"${WIDTH[@]}"} < "${input_file}" > "${ACTUAL_OUTPUT}" 2> "${WARNING_OUTPUT}";
	format_status=$?;
	if (( format_status == 124 )); then
		echo "FAIL ${case_name}: timed out" >&2;
		cat "${WARNING_OUTPUT}" >&2;
		FAILED=1;
		return;
	fi
	# 意味を保てない候補の差戻は，理由の通知と原文の返戻の双方を検証し，整形結果の検査は行わない
	fail_file="${input_file%.in}.fail";
	if [ -f "${fail_file}" ]; then
		if (( format_status != 1 )) ||
		! grep -qxF "Error: formatting would $(cat "${fail_file}"), left unchanged: <stdin>" "${WARNING_OUTPUT}"; then
			echo "FAIL ${case_name}: expected the formatting to be refused ($(cat "${fail_file}"))" >&2;
			cat "${WARNING_OUTPUT}" >&2;
			FAILED=1;
		elif ! cmp -s "${ACTUAL_OUTPUT}" "${input_file}"; then
			echo "FAIL ${case_name}: the refused formatting did not return the input unchanged" >&2;
			FAILED=1;
		fi
		return;
	fi
	if (( format_status )); then
		echo "FAIL ${case_name}: shavefmt exited non-zero" >&2;
		# 原因の手掛りが失われない様に，退避した標準エラーを其の儘出す
		cat "${WARNING_OUTPUT}" >&2;
		FAILED=1;
		return;
	fi
	if [ ! -f "${output_file}" ]; then
		echo "FAIL ${case_name}: missing .out (write the expected output from the specification)" >&2;
		FAILED=1;
		return;
	fi
	# 見送は .out が入力と同一に為る為，整形結果の比較だけでは退行を検出出来ない
	# 期待する見送理由を .skip へ置き，其の有無と内容の双方を検証する
	skip_file="${input_file%.in}.skip";
	skip_line="$(sed -nE "s|^\[skip\] <stdin>(:[0-9]+:[0-9]+)?: ||p" "${WARNING_OUTPUT}")";
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
	if grep -qE "formatting is partial|Embedded code was left as is:" "${WARNING_OUTPUT}"; then partial=1; else partial=0; fi
	if [ -f "${input_file%.in}.partial" ] && (( !partial )); then
		echo "FAIL ${case_name}: expected partial formatting of a protected region but the whole file was formatted" >&2;
		FAILED=1;
		return;
	elif [ ! -f "${input_file%.in}.partial" ] && [ "${partial}" = "1" ]; then
		echo "FAIL ${case_name}: unexpected partial formatting (the input contains a protected region)" >&2;
		FAILED=1;
		return;
	fi
	if ! cmp -s "${ACTUAL_OUTPUT}" "${output_file}"; then
		echo "FAIL ${case_name}:" >&2;
		diff -u "${output_file}" "${ACTUAL_OUTPUT}" | sed -n "1,40p" >&2;
		FAILED=1;
		return;
	fi
	# 初回が無変更なら同じ入力の再実行と為る為，不動点の追加検証は不要
	if ! cmp -s "${input_file}" "${ACTUAL_OUTPUT}"; then
		# ２巡目の不動点検証（整形結果を再度整形しても変わらない事）
		# 出力の比較だけでは，非不動点の１巡目の姿を期待値として固定して仕舞い，振動が検出出来ない
		# ２巡目の見送は原文を返して一致として通る為，失敗に数える
		python3 "${ROOT}/tools/run_limited.py" --seconds "${CASE_TIMEOUT}" -- "${FMT}" --stdin --lang "${lang}" --fail-on-skip ${WIDTH[@]+"${WIDTH[@]}"} < "${ACTUAL_OUTPUT}" > \
			"${IDEMPOTENT_OUTPUT}" 2> "${TMP_DIR}/idempotent_errors";
		local second_status=$?;
		if (( second_status == 124 )); then
			echo "FAIL ${case_name} (idempotence): timed out" >&2;
			cat "${TMP_DIR}/idempotent_errors" >&2;
			FAILED=1;
			return;
		elif (( second_status )); then
			echo "FAIL ${case_name} (idempotence): second pass exited non-zero or skipped" >&2;
			FAILED=1;
			return;
		fi
		if ! cmp -s "${ACTUAL_OUTPUT}" "${IDEMPOTENT_OUTPUT}"; then
			echo "FAIL ${case_name} (idempotence): second pass differs" >&2;
			diff -u "${ACTUAL_OUTPUT}" "${IDEMPOTENT_OUTPUT}" | sed -n "1,20p" >&2;
			FAILED=1;
			return;
		fi
	fi
	# 静的解析の警告の回帰検証（期待ファイルが在るケースのみ）
	warning_file="${input_file%.in}.warn";
	if [ -f "${warning_file}" ]; then
		sed -e "s|^\[warn\] <stdin>:||" -e "s|^\[warn\] ||" "${WARNING_OUTPUT}" > "${ACTUAL_WARNING}";
		if ! cmp -s "${ACTUAL_WARNING}" "${warning_file}"; then
			echo "FAIL ${case_name} (warnings):" >&2;
			diff -u "${warning_file}" "${ACTUAL_WARNING}" | sed -n "1,40p" >&2;
			FAILED=1;
		fi
	fi
	return;
}

if ! TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/golden.XXXXXX")"; then
	echo "golden: could not create the temporary root" >&2;
	exit 1;
fi

trap 'rm -rf "${TMP_ROOT}"' EXIT;
export FMT TMP_ROOT ROOT CASE_TIMEOUT;
export -f check_case run_case;
CASES="${TMP_ROOT}/cases";

if ! : > "${CASES}"; then
	echo "golden: could not create the case list" >&2;
	exit 1;
fi

TOTAL=0;

for lang_dir in "${GOLDEN}"/*; do
	[ -d "${lang_dir}" ] || continue;
	if ! filtered "$(basename "${lang_dir}")"; then continue; fi
	for input_file in "${lang_dir}"/*.in; do
		[ -f "${input_file}" ] || continue;
		if ! printf '%s\0' "${input_file}" >> "${CASES}"; then
			echo "golden: could not record the case ${input_file}" >&2;
			exit 1;
		fi
		: "$((++TOTAL))";
	done
done

# 失敗したテストケースの数（各テストケースは失敗の時だけマーカーを１行出す）
if ! timed cases xargs -0 -n 1 -P "${JOBS}" bash -c 'check_case "$1" || echo failed' _ < "${CASES}" > "${TMP_ROOT}/failed"; then
	echo "golden: the case runner failed" >&2;
	exit 1;
fi

if ! FAIL="$(wc -l < "${TMP_ROOT}/failed")"; then
	echo "golden: could not count the failed cases" >&2;
	exit 1;
fi

# macOS の wc が前に付ける空白を数値として読み直して除く
FAIL="$((FAIL))";

if (( !TOTAL )); then
	echo "golden: no test cases selected" >&2;
	exit 1;
fi

# ドキュメントが引用する名前の実在（関数を別の層へ移した時に仕様書の名前空間が古い儘残るのを防ぐ）
if ! timed doc_symbols bash "$(dirname "${BASH_SOURCE[0]}")/verify_doc_symbols.sh"; then : "$((++FAIL))"; fi
# 使う標準記号に対するインクルードの不足（手元の推移的なインクルードに頼ると他環境で壊れる）
if ! timed includes python3 "$(dirname "${BASH_SOURCE[0]}")/verify_includes.py"; then : "$((++FAIL))"; fi
# 期待結果其の物が条文に適うか（フォーマッターを動かさず `.out` だけを読み，実行結果を期待結果にする過ちを防ぐ）
if ! timed golden_spec python3 "$(dirname "${BASH_SOURCE[0]}")/verify_golden_spec.py"; then : "$((++FAIL))"; fi
# 経路の分離・コメント非依存・空行の不変性（整形仕様 1.2, 11.1）は，中核の出力の観測をテストケース毎とディレクティブを含む記法の並びで比べて確かめる
if ! timed comment_separation python3 "$(dirname "${BASH_SOURCE[0]}")/verify_comment_separation.py" "${FMT}"; then
	: "$((++FAIL))";
fi
# テストケースに無い形の冪等性（整形仕様 1.3）は，人が書いたテストケースだけでは覆えない為に入力を組み立てて確かめる
if ! timed generated_idempotence python3 "$(dirname "${BASH_SOURCE[0]}")/verify_generated_idempotence.py" "${FMT}"; then
	: "$((++FAIL))";
fi
# 行幅に依らない冪等性（整形仕様 1.3）とコメントの深さ（整形仕様 7.6），行末へ空白を足した入力の終了コードの契約（整形仕様 10.4）をテストケースの変種で確かめる
if ! timed case_variants python3 "$(dirname "${BASH_SOURCE[0]}")/verify_case_variants.py" "${FMT}"; then : "$((++FAIL))"; fi

# 可変事実と幅要約の再利用をしないテスト専用の基準側との照合（時間は合否と分離する）
REFERENCE="$(dirname "${FMT}")/shavefmt_reference";

if [[ "${FMT}" == *.exe ]]; then REFERENCE="${REFERENCE}.exe"; fi

REFERENCE_COMMAND=(
	python3 "$(dirname "${BASH_SOURCE[0]}")/verify_reference_comparison.py"
	"${FMT}" "${REFERENCE}" "${TMP_ROOT}/reference-failures"
);

if ! timed reference_comparison "${REFERENCE_COMMAND[@]}"; then
	if [ -f "${TMP_ROOT}/reference-failures" ]; then
		REFERENCE_FAIL="$(cat "${TMP_ROOT}/reference-failures")";
		FAIL="$((FAIL + REFERENCE_FAIL))";
	else
		: "$((++FAIL))";
	fi
fi

# 工程の間の遮断（整形仕様 3）は出力に現れない為，依存関係の側から機械的に確かめる
if ! timed design_invariants python3 "$(dirname "${BASH_SOURCE[0]}")/verify_design_invariants.py"; then : "$((++FAIL))"; fi

echo "golden: ${TOTAL} cases, ${FAIL} failed";
(( !FAIL ));
