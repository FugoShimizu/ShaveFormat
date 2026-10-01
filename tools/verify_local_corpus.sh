#!/bin/bash

set -euo pipefail;
export LC_ALL=C;

ROOT="$(cd "$(dirname "$0")/.." && pwd)";
FMT="${ROOT}/build/shavefmt";
DUMPAST="${ROOT}/build/dumpast";
MAX_CHARS="${MAX_CHARS:-128}";
KEEP_TMP="${KEEP_TMP:-0}";
REFERENCE_FMT="${REFERENCE_FMT:-}";
FILTERS=("$@");
VERIFIED_LANGUAGES=0;
SOURCE_INDEX=0;
EXAMPLE_INDEX=0;
# macOS 既定の bash 3.2 は `set -u` の下で空配列の展開を未定義変数として扱う為，要素が有る時だけ展開する
for filter in ${FILTERS[@]+"${FILTERS[@]}"}; do
	case "${filter}" in
		c|cpp|csharp|java|go|rust|kotlin|swift|php|javascript|typescript|tsx|ruby|python|json|html|css) ;;
		*) echo "unknown language: ${filter}" >&2; exit 1 ;;
	esac
done
# 整形器は記号リンクを経る経路を辿らない為（macOS の /tmp は /private/tmp へのリンク），実体の経路を使う
TMP_CREATED="$(mktemp -d "${TMPDIR:-/tmp}/shavefmt-corpus.XXXXXX")";

cleanup() {
	if [ "${KEEP_TMP}" != "1" ]; then
		rm -rf "${TMP_CREATED}";
	else
		echo "kept temp dir: ${TMP_CREATED}";
	fi
}
trap cleanup EXIT;
TMP_ROOT="$(cd "${TMP_CREATED}" && pwd -P)";

if [ ! -x "${FMT}" ]; then
	echo "missing formatter: ${FMT}" >&2;
	exit 1;
fi
if [ ! -x "${DUMPAST}" ]; then
	echo "missing dumpast: ${DUMPAST}" >&2;
	exit 1;
fi

# 見出の後の `:language(方言)` の見本は其の方言の文法だけの物で，同じ集を読む他方の方言（TS と TSX）の検査からは除く
extract_corpus_file() {
	local corpus_file="$1";
	local ext="$2";
	local out_dir="$3";
	local prefix="$4";
	local error_list="$5";
	local other_dialect="$6";
	awk -v ext="${ext}" -v out_dir="${out_dir}" -v prefix="${prefix}" -v error_list="${error_list}" -v other="${other_dialect}" '
function flush_sample() {
	if(foreign) {
		source = "";
		collecting = 0;
		name = "";
		return;
	}
	if(!collecting || source == "") return;
	++count;
	name = sprintf("%s_%04d.%s", prefix, count, ext);
	path = sprintf("%s/%s", out_dir, name);
	printf "%s", source > path;
	close(path);
	source = "";
	collecting = 0;
}
# 期待木に ERROR / MISSING を持つ見本は構文誤りの入力の為，見送を失敗に数えない一覧へ載せる
function flush_error() {
	if(errored && name != "") print name >> error_list;
	errored = 0;
}
BEGIN {
	state = "seek_eq";
	count = 0;
	source = "";
	collecting = 0;
}
/^={3,}$/ {
	if(state == "seek_eq") {
		state = "read_title";
		foreign = 0;
		next;
	}
	if(state == "after_title") {
		state = "seek_source";
		next;
	}
	if(state == "skip_ast") {
		flush_error();
		state = "read_title";
		foreign = 0;
		next;
	}
}
{
	if(state == "read_title") {
		state = "after_title";
		next;
	}
	if(state == "after_title") {
		if(other != "" && $0 == ":language(" other ")") foreign = 1;
		next;
	}
	if(state == "seek_source") {
		if($0 == "") next;
		if($0 ~ /^-{3,}$/) next;
		source = $0 "\n";
		collecting = 1;
		state = "read_source";
		next;
	}
	if(state == "read_source") {
		if($0 ~ /^-{3,}$/) {
			flush_sample();
			state = "skip_ast";
			next;
		}
		source = source $0 "\n";
		next;
	}
	if(state == "skip_ast" && $0 ~ /\((ERROR|MISSING)/) errored = 1;
}
END {
	flush_sample();
	flush_error();
}' "${corpus_file}";
}

# 見本の整形（構文誤りの見本は見送を許し，構文守衛と異常終了だけを失敗とする）
format_samples() {
	local formatter="$1";
	local sample_dir="$2";
	local error_list="$3";
	local error_dir="${sample_dir}.error";
	mkdir -p "${error_dir}";
	while IFS= read -r name; do
		[ -f "${sample_dir}/${name}" ] && mv "${sample_dir}/${name}" "${error_dir}/${name}";
	done < "${error_list}"
	"${formatter}" -w --fail-on-skip --chars "${MAX_CHARS}" "${sample_dir}" >/dev/null;
	if [ -n "$(ls -A "${error_dir}")" ]; then
		"${formatter}" -w --chars "${MAX_CHARS}" "${error_dir}" >/dev/null;
		mv "${error_dir}"/* "${sample_dir}/";
	fi
	rmdir "${error_dir}";
}

collect_examples() {
	local ext="$1";
	local out_dir="$2";
	local error_list="$3";
	shift 3;
	for example_dir in "$@"; do
		[ -d "${example_dir}" ] || continue;
		while IFS= read -r path; do
			[ -f "${path}" ] || continue;
			((++EXAMPLE_INDEX));
			local name="example_${EXAMPLE_INDEX}.${ext}";
			cp "${path}" "${out_dir}/${name}";
			# 期待木を持たない見本は，解析器が ERROR / MISSING を立てる物を構文誤りの入力として一覧へ載せる
			# `grep -q` は一致で読むのを止めて dumpast を SIGPIPE で終わらせ，pipefail の下で偽に為る為，全行を読ませる
			if "${DUMPAST}" --types-only "${out_dir}/${name}" | grep -xE "ERROR|MISSING" > /dev/null; then
				echo "${name}" >> "${error_list}";
			fi
		done < <(find "${example_dir}" -type f \( -name "*.${ext}" \) | sort)
	done
}

verify_language() {
	local label="$1";
	local ext="$2";
	shift 2;
	if (( ${#FILTERS[@]} )); then
		local matched=0;
		local filter;
		for filter in "${FILTERS[@]}"; do
			if [ "${filter}" = "${label}" ]; then
				matched=1;
				break;
			fi
		done
		if (( !matched )); then
			return 0;
		fi
	fi
	local work_dir="${TMP_ROOT}/${label}";
	local corpus_dir="${work_dir}/corpus";
	local current_dir="${work_dir}/current";
	local reference_dir="${work_dir}/reference";
	local error_list="${work_dir}/error_samples.txt";
	local corpus_inputs=("$@");
	local other_dialect="";
	case "${label}" in
		typescript) other_dialect="tsx" ;;
		tsx) other_dialect="typescript" ;;
	esac

	mkdir -p "${corpus_dir}";
	: > "${error_list}";

	for corpus_path in "${corpus_inputs[@]}"; do
		if [ -f "${corpus_path}" ]; then
			((++SOURCE_INDEX))
			extract_corpus_file "${corpus_path}" "${ext}" "${corpus_dir}" "source_${SOURCE_INDEX}" "${error_list}" "${other_dialect}";
		elif [ -d "${corpus_path}" ]; then
			while IFS= read -r corpus_file; do
				((++SOURCE_INDEX))
				extract_corpus_file "${corpus_file}" "${ext}" "${corpus_dir}" "source_${SOURCE_INDEX}" "${error_list}" "${other_dialect}";
			done < <(find "${corpus_path}" -type f -name "*.txt" | sort)
			collect_examples "${ext}" "${corpus_dir}" "${error_list}" "${corpus_path}";
		fi
	done

	local sample_count;
	sample_count="$(find "${corpus_dir}" -type f | wc -l | tr -d " ")";
	if (( !sample_count )); then
		echo "FAIL ${label}: no samples" >&2;
		return 1;
	fi

	echo "verify ${label}: ${sample_count} samples";
	if [ -n "${REFERENCE_FMT}" ]; then
		cp -R "${corpus_dir}" "${current_dir}";
		cp -R "${corpus_dir}" "${reference_dir}";
		# 整形の差戻・見送・書込失敗を，出力一致と混同しない
		format_samples "${REFERENCE_FMT}" "${reference_dir}" "${error_list}";
		format_samples "${FMT}" "${current_dir}" "${error_list}";
		if ! diff -ru "${reference_dir}" "${current_dir}" >/dev/null; then
			echo "FAIL ${label}: output differs from reference formatter" >&2;
			diff -ru "${reference_dir}" "${current_dir}" | sed -n "1,200p";
			exit 1;
		fi
		echo "output ${label}: matches reference";
	else
		cp -R "${corpus_dir}" "${current_dir}";
		format_samples "${FMT}" "${current_dir}" "${error_list}";
		echo "output ${label}: reference skipped";
	fi
	: "$((++VERIFIED_LANGUAGES))";
}

verify_language "c" "c" "${ROOT}/vendor/tree-sitter-c/test/corpus" "${ROOT}/vendor/tree-sitter-c/examples";

verify_language "cpp" "cpp" "${ROOT}/vendor/tree-sitter-cpp/test/corpus" "${ROOT}/vendor/tree-sitter-cpp/examples";

paths=(
	"${ROOT}/vendor/tree-sitter-c-sharp/test/corpus"
	"${ROOT}/vendor/tree-sitter-c-sharp/test/highlight"
	"${ROOT}/vendor/tree-sitter-c-sharp/test/queries"
);
verify_language "csharp" "cs" "${paths[@]}";

verify_language "java" "java" "${ROOT}/vendor/tree-sitter-java/test/corpus";

verify_language "go" "go" "${ROOT}/vendor/tree-sitter-go/test/corpus" "${ROOT}/vendor/tree-sitter-go/examples";

verify_language "rust" "rs" "${ROOT}/vendor/tree-sitter-rust/test/corpus" "${ROOT}/vendor/tree-sitter-rust/examples";

verify_language "kotlin" "kt" "${ROOT}/vendor/tree-sitter-kotlin/test/corpus" "${ROOT}/vendor/tree-sitter-kotlin/examples";

paths=("${ROOT}/vendor/tree-sitter-swift/test/corpus" "${ROOT}/vendor/tree-sitter-swift/test/highlight");
verify_language "swift" "swift" "${paths[@]}";

verify_language "php" "php" "${ROOT}/vendor/tree-sitter-php/test/corpus";

paths=("${ROOT}/vendor/tree-sitter-javascript/test/corpus" "${ROOT}/vendor/tree-sitter-javascript/examples");
verify_language "javascript" "js" "${paths[@]}";

# `.ts` は JSX 無の文法，`.tsx` は JSX 付の文法で読む（見本は拡張子で振り分けて集める）
paths=(
	"${ROOT}/vendor/tree-sitter-typescript/test/corpus"
	"${ROOT}/vendor/tree-sitter-typescript/examples"
	"${ROOT}/vscode-extension/src"
);
verify_language "typescript" "ts" "${paths[@]}";

verify_language "tsx" "tsx" "${ROOT}/vendor/tree-sitter-typescript/test/corpus";

verify_language "ruby" "rb" "${ROOT}/vendor/tree-sitter-ruby/test/corpus";

verify_language "python" "py" "${ROOT}/vendor/tree-sitter-python/test/corpus" "${ROOT}/vendor/tree-sitter-python/examples";

verify_language "json" "json" "${ROOT}/vendor/tree-sitter-json/test/corpus";

verify_language "html" "html" "${ROOT}/vendor/tree-sitter-html/test/corpus" "${ROOT}/vendor/tree-sitter-html/examples";

paths=(
	"${ROOT}/vendor/tree-sitter-scss/test/corpus"
	"${ROOT}/vendor/tree-sitter-scss/examples"
	"${ROOT}/vendor/tree-sitter-scss/test/highlight"
);
verify_language "css" "css" "${paths[@]}";

(( VERIFIED_LANGUAGES )) || { echo "FAIL: no languages verified" >&2; exit 1; }
echo "verify_local_corpus: complete (${VERIFIED_LANGUAGES} languages)";
