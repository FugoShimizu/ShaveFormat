#!/usr/bin/env bash
# 文書が引用する名前が実装に在る事の検査
# 関数を別の層へ移したり工程を改名した時に，仕様書の名前空間が古い儘残る事を防ぐ
set -u;

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)";
TOTAL=0;
FAIL=0;

# 整形仕様 3.1 の責務の表が挙げる工程は，実装に同じ名前の型を持つ
# 表は責務の分担其の物の為，名前が無い工程は「実装がまだ其の責務を型として持って居ない」事を表す
while read -r NAME; do
	[ -n "${NAME}" ] || continue;
	: "$((++TOTAL))";
	if ! grep -rqE "^[[:space:]]*(class|struct) ${NAME}\b" "${ROOT}/src"; then
		echo "FAIL doc symbol: ${NAME} は仕様書 3.1 の工程だが実装に型が無い" >&2;
		: "$((++FAIL))";
	fi;
done < <(
	sed -n '/^## 3\.1 /,/^## 3\.2 /p' "${ROOT}/docs/formatter_spec.md" |
	grep -oE '^\| `[A-Za-z][A-Za-z0-9_]*` \|' | tr -d '|` ' | sort -u
);

# 整形仕様 3.3 のクラス図が挙げる型の内，未だ実装に無い物の一覧（移行の残りを毎回出す台帳）
# 図は再設計の到達点であり，名前が無い型は退行ではなく未着手の移行の為，此処では失敗にしない
PENDING="";
while read -r NAME; do
	[ -n "${NAME}" ] || continue;
	: "$((++TOTAL))";
	if ! grep -rqE "^[[:space:]]*(class|struct|enum class|using) ${NAME}\b" "${ROOT}/src"; then
		PENDING="${PENDING}${PENDING:+|}${NAME}";
	fi;
done < <(
	sed -n '/^```mermaid$/,/^```$/p' "${ROOT}/docs/formatter_spec.md" |
	grep -oE "^	class [A-Za-z][A-Za-z0-9_]*" | awk '{print $2}' | sort -u
);
[ -n "${PENDING}" ] && echo "doc symbols: 仕様書 3.3 の未実装の型（移行の残り）：${PENDING//|/, }" >&2;

# 文書の中の `Class::Name` の形の引用を取り出して照合する
# 名前は修飾付 (`ParseAdapter::PrepareCSource`) でも，欄の宣言 (`bool IsScss`) でも良い
while read -r NAME; do
	[ -n "${NAME}" ] || continue;
	: "$((++TOTAL))";
	# 未実装の型の成員は上の台帳が挙げる為，改名の検出からは除く
	if [[ "|${PENDING}|" == *"|${NAME%%::*}|"* ]]; then continue; fi
	# 修飾付の字面で見付からない場合は，型が在り且つ成員の名前が在る事で照合する（成員変数は修飾せずに宣言する）
	if
		! grep -rqE "\b${NAME}\b" "${ROOT}/src" && ! {
			grep -rqE "^[[:space:]]*(class|struct|enum class|using) ${NAME%%::*}\b" "${ROOT}/src" &&
			grep -rqE "\b${NAME##*::}\b" "${ROOT}/src";
		};
	then
		echo "FAIL doc symbol: ${NAME} is cited in the docs but not found in src/" >&2;
		: "$((++FAIL))";
	fi;
done < <(
	grep -rhoE "\`[A-Za-z][A-Za-z0-9_]*::[A-Za-z_]+\`" "${ROOT}/docs" "${ROOT}/README.md" 2>/dev/null |
	tr -d '`' | grep -v '^std::' | sort -u
);

# 改名で消えたクラス名が注記へ残る事の検査（修飾無の関数名は現存する為，上の照合では見付からない）
GONE='CommentAttach|DocSig|Edit|Layout|LayoutComment|LineSplit|Lint|ParseRepair|Postprocess|Preprocess|Structure|SyntaxCheck|TsSource';
while read -r LINE; do
	[ -n "${LINE}" ] || continue;
	: "$((++TOTAL))";
	echo "FAIL doc symbol: ${LINE} は改名で消えたクラス名を引用して居る" >&2;
	: "$((++FAIL))";
done < <(
	grep -rnoE "\b(${GONE})::[A-Za-z_]+" "${ROOT}/src" "${ROOT}/docs" "${ROOT}/README.md" 2>/dev/null | sort -u
);

echo "doc symbols: ${TOTAL} cited, ${FAIL} missing";

(( !FAIL ));
