#!/bin/bash
# 期待出力との整形回帰試験
# 入力を既定幅で整形し，同名の期待結果とバイト単位で比較する
# 同名の .chars ファイルが在る場合は其の行幅で整形する（狭い行幅の行分割も不動点・コメント非依存まで検証する）
# 同名の .warn ファイルが在る場合は静的検査の警告の一覧（`行:桁: 本文` 形式）も併せて比較する
# .warn が無いケースでは警告を比較しない
# 同名の .skip ファイルが在る場合は其の理由で見送られる事を検証する
# 同名の .semantic-comment ファイルが在る場合は，処理系が読むコメントの為，コメント非依存検証から除外する
# 併せて整形結果を再度整形し，二巡目で変わらない事（不動点）も検証する
# .skip が無いのに見送られたら失敗とする（見送は .out が入力と同一に為る為，比較だけでは退行を検出出来ない）
# `.partial` の有無と部分整形経路の使用が一致する事を検証する
# 仕様変更で出力が変わった場合のみ UPDATE_GOLDEN=1 で .out を再生成し，レビューで差分を必ず確認する運用とする
# 使い方：
# tools/golden_test.sh # 全件チェック
# tools/golden_test.sh cpp javascript # 言語フィルタ
# UPDATE_GOLDEN=1 tools/golden_test.sh [lang...] # .out 再生成
# 環境変数：
# UPDATE_GOLDEN 1 を設定すると差分があった場合 .out を上書きする

set -u;
export LC_ALL=C;

ROOT="$(cd "$(dirname "$0")/.." && pwd)";
FMT="${ROOT}/build/shavefmt";
GOLDEN="${ROOT}/test_data/golden";
UPDATE="${UPDATE_GOLDEN:-0}";
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

TOTAL=0;
FAIL=0;
UPDATED=0;
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/golden.XXXXXX")";
trap 'rm -rf "${TMP_ROOT}"' EXIT;
TMP="${TMP_ROOT}/out";
WARN_TMP="${TMP_ROOT}/warn";
WARN_ACT="${TMP_ROOT}/warn_actual";
IDEM_TMP="${TMP_ROOT}/idempotent";
CI_TMP="${TMP_ROOT}/comment_independent";
CI2_TMP="${TMP_ROOT}/comment_independent_second";

for lang_dir in "${GOLDEN}"/*; do
	[ -d "${lang_dir}" ] || continue;
	lang="$(basename "${lang_dir}")";
	if ! filtered "${lang}"; then continue; fi
	for in_file in "${lang_dir}"/*.in; do
		[ -f "${in_file}" ] || continue;
		out_file="${in_file%.in}.out";
		case_name="${lang}/$(basename "${in_file%.in}")";
		: "$((++TOTAL))";
		chars_file="${in_file%.in}.chars";
		WIDTH=();
		if [ -f "${chars_file}" ]; then WIDTH=(--chars "$(cat "${chars_file}")"); fi
		COMMENT_ARGS=(--stdin --lang "${lang}" ${WIDTH[@]+"${WIDTH[@]}"});
		"${FMT}" --stdin --lang "${lang}" ${WIDTH[@]+"${WIDTH[@]}"} < "${in_file}" > "${TMP}" 2> "${WARN_TMP}";
		format_status=$?;
		if (( format_status )); then
			echo "FAIL ${case_name}: shavefmt exited non-zero" >&2;
			# 原因の手掛りが失われない様に，退避した標準エラーを其の儘出す
			cat "${WARN_TMP}" >&2;
			: "$((++FAIL))";
			continue;
		fi
		if [ "${UPDATE}" = "1" ]; then
			if [ ! -f "${out_file}" ] || ! cmp -s "${TMP}" "${out_file}"; then
				cp "${TMP}" "${out_file}";
				echo "UPDATED ${case_name}";
				: "$((++UPDATED))";
			fi
			# .warn が既に在るケースのみ警告一覧も追従させる（新規作成は手動）
			warn_file="${in_file%.in}.warn";
			if [ -f "${warn_file}" ]; then
				sed -e "s|^\[warn\] <stdin>:||" -e "s|^\[warn\] ||" "${WARN_TMP}" > "${WARN_ACT}";
				if ! cmp -s "${WARN_ACT}" "${warn_file}"; then
					cp "${WARN_ACT}" "${warn_file}";
					echo "UPDATED ${case_name} (warnings)";
				fi
			fi
		fi
		if [ ! -f "${out_file}" ]; then
			echo "FAIL ${case_name}: missing .out (run UPDATE_GOLDEN=1 to create)" >&2;
			: "$((++FAIL))";
			continue;
		fi
		# 見送は .out が入力と同一に為る為，整形結果の比較だけでは退行を検出出来ない
		# 期待する見送理由を .skip へ置き，其の有無と内容の双方を検証する
		skip_file="${in_file%.in}.skip";
		skip_line="$(sed -n "s|^\[skip\] <stdin>: ||p" "${WARN_TMP}")";
		if [ -f "${skip_file}" ]; then
			if [ -z "${skip_line}" ]; then
				echo "FAIL ${case_name}: expected skip but the file was formatted" >&2;
				: "$((++FAIL))";
				continue;
			fi
			if [ "${skip_line}" != "$(cat "${skip_file}")" ]; then
				echo "FAIL ${case_name}: skip reason changed" >&2;
				echo "  expected: $(cat "${skip_file}")" >&2;
				echo "  actual:   ${skip_line}" >&2;
				: "$((++FAIL))";
				continue;
			fi
		elif [ -n "${skip_line}" ]; then
			echo "FAIL ${case_name}: unexpected skip (${skip_line})" >&2;
			: "$((++FAIL))";
			continue;
		fi
		if grep -q "formatting is partial" "${WARN_TMP}"; then partial=1; else partial=0; fi
		if [ -f "${in_file%.in}.partial" ] && (( !partial )); then
			echo "FAIL ${case_name}: expected partial formatting of a syntax error but the whole file was formatted" >&2;
			: "$((++FAIL))";
			continue;
		elif [ ! -f "${in_file%.in}.partial" ] && [ "${partial}" = "1" ]; then
			echo "FAIL ${case_name}: unexpected partial formatting (the input is read as having syntax errors)" >&2;
			: "$((++FAIL))";
			continue;
		fi
		if ! cmp -s "${TMP}" "${out_file}"; then
			echo "FAIL ${case_name}:" >&2;
			diff -u "${out_file}" "${TMP}" | sed -n "1,40p" >&2;
			: "$((++FAIL))";
			continue;
		fi
		# 初回が無変更なら同じ入力の再実行と為る為，不動点の追加検証は不要
		if ! cmp -s "${in_file}" "${TMP}"; then
			# 二巡目の不動点検証（整形結果を再度整形しても変わらない事）
			# 出力の比較だけでは，非不動点の一巡目の姿を期待値として固定して仕舞い，振動が検出出来ない
			if ! "${FMT}" --stdin --lang "${lang}" ${WIDTH[@]+"${WIDTH[@]}"} < "${TMP}" > "${IDEM_TMP}" 2> /dev/null; then
				echo "FAIL ${case_name} (idempotence): second pass exited non-zero" >&2;
				: "$((++FAIL))";
				continue;
			fi
			if ! cmp -s "${TMP}" "${IDEM_TMP}"; then
				echo "FAIL ${case_name} (idempotence): second pass differs" >&2;
				diff -u "${TMP}" "${IDEM_TMP}" | sed -n "1,20p" >&2;
				: "$((++FAIL))";
				continue;
			fi
		fi
		# コメント非依存の検証（整形の絶対原則）
		if [ ! -f "${in_file%.in}.semantic-comment" ]; then
		# コメントを書き戻さない設定で整形した出力を，同じ設定で再度整形する
		# コメントの有無でコメント以外の整形結果が変わるなら，コメントが消えた二巡目で形が変わり不一致に為る
		if ! SHAVEFMT_DROP_COMMENTS=1 "${FMT}" "${COMMENT_ARGS[@]}" < "${in_file}" > "${CI_TMP}" 2> /dev/null; then
			echo "FAIL ${case_name} (comment independence): first pass exited non-zero" >&2;
			: "$((++FAIL))";
			continue;
		fi
		if ! SHAVEFMT_DROP_COMMENTS=1 "${FMT}" "${COMMENT_ARGS[@]}" < "${CI_TMP}" > "${CI2_TMP}" 2> /dev/null; then
			echo "FAIL ${case_name} (comment independence): second pass exited non-zero" >&2;
			: "$((++FAIL))";
			continue;
		fi
		if ! cmp -s "${CI_TMP}" "${CI2_TMP}"; then
			echo "FAIL ${case_name} (comment independence): output depends on comments" >&2;
			diff -u "${CI_TMP}" "${CI2_TMP}" | sed -n "1,20p" >&2;
			: "$((++FAIL))";
			continue;
		fi
		# コメント除去済の出力と通常出力が同一なら，直前の再実行で両経路を検証済
		if ! cmp -s "${TMP}" "${CI_TMP}"; then
			# 通常出力からコメントを除き，元入力からコメントを除いた整形と一致する事も検証する
			if ! SHAVEFMT_DROP_COMMENTS=1 "${FMT}" "${COMMENT_ARGS[@]}" < "${TMP}" > "${CI2_TMP}" 2> /dev/null; then
				echo "FAIL ${case_name} (comment independence): normal output reformat exited non-zero" >&2;
				: "$((++FAIL))";
				continue;
			fi
			if ! cmp -s "${CI_TMP}" "${CI2_TMP}"; then
				echo "FAIL ${case_name} (comment independence): normal output changes code" >&2;
				diff -u "${CI_TMP}" "${CI2_TMP}" | sed -n "1,20p" >&2;
				: "$((++FAIL))";
				continue;
			fi
		fi
		fi
		# 静的検査の警告の回帰検証（期待ファイルが在るケースのみ）
		warn_file="${in_file%.in}.warn";
		if [ -f "${warn_file}" ]; then
			sed -e "s|^\[warn\] <stdin>:||" -e "s|^\[warn\] ||" "${WARN_TMP}" > "${WARN_ACT}";
			if ! cmp -s "${WARN_ACT}" "${warn_file}"; then
				echo "FAIL ${case_name} (warnings):" >&2;
				diff -u "${warn_file}" "${WARN_ACT}" | sed -n "1,40p" >&2;
				: "$((++FAIL))";
			fi
		fi
	done
done

if (( !TOTAL )); then
	echo "golden: no test cases selected" >&2;
	exit 1;
fi

if [ "${UPDATE}" = "1" ]; then
	# 整形器が非零で終えた事例は期待値を作れて居らず，更新でも失敗として扱う（成功で終えると壊れた儘の期待値が残る）
	echo "golden: ${TOTAL} cases, ${UPDATED} updated, ${FAIL} failed";
	(( !FAIL ));
	exit $?;
fi
echo "golden: ${TOTAL} cases, ${FAIL} failed";

(( !FAIL ));
