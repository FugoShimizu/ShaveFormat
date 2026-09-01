cmake_minimum_required(VERSION 3.20)
project(ShaveFormat VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# C は C17 とし，bool は <stdbool.h> を用い，nullptr・constexpr・２進リテラルは用いない
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# 複数の構成を持つ生成器 (Visual Studio) も Release の実行ファイルを build/ の直下に置き，検査の道具と CI の経路を環境で分けない
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
# 配布物は開発の道具の無い計算機で動く必要が有る為，MSVC は C / C++ の実行時を静的に結合する
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 未使用引数・死枝等を機械検出する為，GCC / Clang では自プロジェクトの翻訳単位に警告を有効にする
# 誤りにはせず（`-Werror` を付けず）環境差で構築が落ちない様にする
# MSVC は既定の警告水準を用いる
# vendor の構文解析器は生成コードで手を入れない為，対象外とする（此処では変数だけ定め各ターゲットへ個別に付与する）
# `-Wparentheses` 系（`&&` を `||` で囲む等の冗長括弧を促す群）は演算子の優先順位から不要な括弧を要求する為，抑止する
# 抑止しないと本群だけで百件超が出て，未使用変数・符号比較等の実際の誤りを示す警告が埋もれる
if(MSVC)
	set(SHAVEFMT_WARNINGS "")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	# 冗長括弧を促す下位群だけを抑止する（`-Wno-parentheses` で群全体止めると `if(x = y)` の代入誤りと対応する if の無い else の検出迄失う）
	set(SHAVEFMT_WARNINGS
		-Wall -Wextra -Wno-logical-op-parentheses -Wno-bitwise-op-parentheses -Wno-shift-op-parentheses
		-Wno-overloaded-shift-op-parentheses -Wno-parentheses-equality
	)
else()
	# GCC は下位群を持たず群全体の抑止しか出来ない
	set(SHAVEFMT_WARNINGS -Wall -Wextra -Wno-parentheses)
endif()
if(MSVC)
	set(SHAVEFMT_C_WARNINGS "")
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
	set(SHAVEFMT_C_WARNINGS -Wall -Wextra -Wno-logical-op-parentheses -Wno-bitwise-op-parentheses -Wno-shift-op-parentheses)
else()
	set(SHAVEFMT_C_WARNINGS -Wall -Wextra -Wno-parentheses)
endif()

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Release ビルドでリンク時最適化を有効化
# 対応コンパイラ + リンカ環境でバイナリサイズ削減 (5-15%) と関数間最適化に依る速度向上を得る
include(CheckIPOSupported)
check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)
if(IPO_SUPPORTED AND (CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo"))
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
	message(STATUS "LTO/IPO enabled for ${CMAKE_BUILD_TYPE} build")
elseif(NOT IPO_SUPPORTED)
	message(STATUS "LTO/IPO not supported: ${IPO_ERROR}")
endif()

# ホスト CPU 専用最適化（ホスト専用実行ファイル）
# 配布バイナリでは OFF（既定値）の儘使う事
# AVX2/NEON 等のネイティブ命令で tree-sitter 構文解析表アクセスや SIMD ループが加速する
option(NATIVE_ARCH "Build with -march=native + -funroll-loops (host-specific binary)" OFF)
if(NATIVE_ARCH)
	if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
		add_compile_options(-march=native -funroll-loops)
		add_link_options(-march=native)
		message(STATUS "Native arch optimization enabled (host-specific binary, do not redistribute)")
	else()
		message(FATAL_ERROR "NATIVE_ARCH is supported only with GCC or Clang")
	endif()
endif()

# 実行記録に基付く最適化 (PGO) の対応 (Clang / GCC)
# ２段階ビルド：
# cmake -B build_pgo -DPGO_MODE=generate && cmake --build build_pgo
# ./build_pgo/shavefmt src/ # 代表的な処理の実行（実行記録の生成，書き戻さない試行でも整形の全工程が走る）
# xcrun llvm-profdata merge -output=build_pgo/pgo-data/default.profdata build_pgo/pgo-data/*.profraw # Clang の実行記録の統合（Linux は llvm-profdata，GCC は不要）
# cmake -B build -DPGO_MODE=use -DPGO_DATA=${PWD}/build_pgo/pgo-data && cmake --build build
# 効果：LTO の上に分岐予測精度向上で +5〜15% 速度向上が期待出来る
set(PGO_MODE "off" CACHE STRING "PGO mode: off | generate | use")
set(PGO_DATA "${CMAKE_BINARY_DIR}/pgo-data" CACHE PATH "PGO profile data directory")
set_property(CACHE PGO_MODE PROPERTY STRINGS off generate use)
if(NOT PGO_MODE STREQUAL "off" AND NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	message(FATAL_ERROR "PGO_MODE is supported only with GCC or Clang")
endif()
if(PGO_MODE STREQUAL "generate")
	add_compile_options(-fprofile-generate=${PGO_DATA})
	add_link_options(-fprofile-generate=${PGO_DATA})
	message(STATUS "PGO: profile-generate enabled (data dir: ${PGO_DATA})")
elseif(PGO_MODE STREQUAL "use")
	add_compile_options(-fprofile-use=${PGO_DATA})
	add_link_options(-fprofile-use=${PGO_DATA})
	# 実行記録に含まれない関数等の警告だけは誤りへ格上げせず，構築器固有の警告名を他の構築器へ渡さない
	if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		add_compile_options(-Wno-error=profile-instr-out-of-date -Wno-error=profile-instr-unprofiled)
	elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
		add_compile_options(-Wno-error=coverage-mismatch -Wno-error=missing-profile)
	endif()
	message(STATUS "PGO: profile-use enabled (data dir: ${PGO_DATA})")
endif()

# 他人の書いたソースを手書きの外部走査器へ直接渡す為，実行時の防御を有効にする
# 対応しない環境では旗自体が誤りに為る為，使える物だけを選ぶ
include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)
set(SHAVEFMT_C_HARDENING "")
set(SHAVEFMT_HARDENING "")
if(MSVC)
	set(SHAVEFMT_C_HARDENING /guard:cf)
	set(SHAVEFMT_HARDENING /guard:cf) # /GS は既定で有効
else()
	# 受け付けるが働かない旗（Apple の clang の `-fstack-clash-protection`）は警告を誤りに格上げして落とす
	# 警告の名前は clang 固有で，GCC は未知の名前を誤りとし全ての旗を非対応と判定する為，clang でのみ格上げする
	# 呼出側の値を壊さない様に控えてから足し，検査の後に戻す
	set(SHAVEFMT_SAVED_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
	set(SHAVEFMT_C_REQUIRED_FLAGS "${SHAVEFMT_SAVED_REQUIRED_FLAGS}")
	set(SHAVEFMT_CXX_REQUIRED_FLAGS "${SHAVEFMT_SAVED_REQUIRED_FLAGS}")
	if(CMAKE_C_COMPILER_ID MATCHES "Clang")
		string(APPEND SHAVEFMT_C_REQUIRED_FLAGS " -Werror=unused-command-line-argument")
	endif()
	if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		string(APPEND SHAVEFMT_CXX_REQUIRED_FLAGS " -Werror=unused-command-line-argument")
	endif()
	# `-fcf-protection` は x86 専用，`-mbranch-protection` は arm64 専用の為，双方を候補に置いて対応する側だけを採る
	foreach(HARDENING_FLAG -fstack-protector-strong -fstack-clash-protection -fcf-protection=full -mbranch-protection=standard)
		string(MAKE_C_IDENTIFIER "SHAVEFMT_C_HAS${HARDENING_FLAG}" C_HARDENING_VAR)
		string(MAKE_C_IDENTIFIER "SHAVEFMT_CXX_HAS${HARDENING_FLAG}" HARDENING_VAR)
		set(CMAKE_REQUIRED_FLAGS "${SHAVEFMT_C_REQUIRED_FLAGS}")
		check_c_compiler_flag("${HARDENING_FLAG}" ${C_HARDENING_VAR})
		set(CMAKE_REQUIRED_FLAGS "${SHAVEFMT_CXX_REQUIRED_FLAGS}")
		check_cxx_compiler_flag("${HARDENING_FLAG}" ${HARDENING_VAR})
		if(${C_HARDENING_VAR})
			list(APPEND SHAVEFMT_C_HARDENING ${HARDENING_FLAG})
		endif()
		if(${HARDENING_VAR})
			list(APPEND SHAVEFMT_HARDENING ${HARDENING_FLAG})
		endif()
	endforeach()
	set(CMAKE_REQUIRED_FLAGS "${SHAVEFMT_SAVED_REQUIRED_FLAGS}")
endif()

# 標準ライブラリの境界検査は最適化と組で働く為，最適化を掛ける構成にだけ足す
# 処理系が既に水準を決めて居る場合（新しい GCC の既定は 3）は，重ねると再定義の警告が出た上に水準が下がる為に触らない
include(CheckCXXSourceCompiles)
set(SHAVEFMT_SAVED_FORTIFY_FLAGS "${CMAKE_REQUIRED_FLAGS}")
set(CMAKE_REQUIRED_FLAGS "-O2")
check_cxx_source_compiles("#ifndef _FORTIFY_SOURCE\n#error none\n#endif\nint main() { return 0; }" SHAVEFMT_HAS_DEFAULT_FORTIFY)
set(CMAKE_REQUIRED_FLAGS "${SHAVEFMT_SAVED_FORTIFY_FLAGS}")
if(SHAVEFMT_HAS_DEFAULT_FORTIFY)
	set(SHAVEFMT_HARDENING_DEFINITIONS "")
else()
	set(SHAVEFMT_HARDENING_DEFINITIONS "$<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=2>")
endif()

# tree-sitter 言語解析器（文法毎に独立した静的ライブラリ）
# 各文法が独自の `tree_sitter/parser.h`（ABI バージョン依存）を持つ為，取込経路を文法単位に閉じる事でヘッダ衝突を回避する
# 第３引数は１つの文法の集まりから複数の文法を作る時の対象名（省略時は文法名：tree-sitter-typescript の typescript / tsx）
function(add_ts_grammar GRAMMAR_NAME SRC_SUBDIR)
	set(SRC_DIR "${CMAKE_SOURCE_DIR}/vendor/tree-sitter-${GRAMMAR_NAME}/${SRC_SUBDIR}")
	file(GLOB GRAMMAR_SOURCES "${SRC_DIR}/parser.c" "${SRC_DIR}/scanner.c" "${SRC_DIR}/scanner.cc" "${SRC_DIR}/schema.generated.cc")
	if(GRAMMAR_NAME STREQUAL "scss")
		find_program(TREE_SITTER_CLI NAMES tree-sitter tree-sitter.cmd REQUIRED)
		find_program(NODE_EXECUTABLE NAMES node node.exe REQUIRED)
		set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/scss")
		add_custom_command(
			OUTPUT "${GENERATED_DIR}/parser.c" "${GENERATED_DIR}/tree_sitter/parser.h"
			BYPRODUCTS "${GENERATED_DIR}/grammar.json" "${GENERATED_DIR}/node-types.json"
				"${GENERATED_DIR}/tree_sitter/alloc.h" "${GENERATED_DIR}/tree_sitter/array.h"
			COMMAND "${NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/prepareScss.mjs"
				"${SRC_DIR}/grammar.json" "${GENERATED_DIR}"
			COMMAND "${TREE_SITTER_CLI}" generate --abi 14 "${GENERATED_DIR}/grammar.json" -o "${GENERATED_DIR}"
			DEPENDS "${SRC_DIR}/grammar.json" "${CMAKE_SOURCE_DIR}/tools/prepareScss.mjs"
				"${TREE_SITTER_CLI}" "${NODE_EXECUTABLE}"
			VERBATIM
		)
		list(REMOVE_ITEM GRAMMAR_SOURCES "${SRC_DIR}/parser.c")
		list(APPEND GRAMMAR_SOURCES "${GENERATED_DIR}/parser.c")
	endif()
	if(ARGC GREATER 2)
		set(TARGET_SUFFIX "${ARGV2}")
	else()
		string(REPLACE "-" "_" TARGET_SUFFIX "${GRAMMAR_NAME}")
	endif()
	set(TARGET_NAME "ts_${TARGET_SUFFIX}")
	add_library(${TARGET_NAME} STATIC ${GRAMMAR_SOURCES})
	if(GRAMMAR_NAME STREQUAL "scss")
		target_include_directories(${TARGET_NAME} PRIVATE "${GENERATED_DIR}")
	endif()
	target_include_directories(${TARGET_NAME} PRIVATE ${SRC_DIR})
	# 生成した構文解析器と共に人が書いた外部走査器を含む為，走査器も防御の対象にする
	foreach(HARDENING_FLAG IN LISTS SHAVEFMT_C_HARDENING)
		target_compile_options(${TARGET_NAME} PRIVATE "$<$<COMPILE_LANGUAGE:C>:${HARDENING_FLAG}>")
	endforeach()
	foreach(HARDENING_FLAG IN LISTS SHAVEFMT_HARDENING)
		target_compile_options(${TARGET_NAME} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${HARDENING_FLAG}>")
	endforeach()
	# 境界検査が最も要るのは手書きの走査器を含む此処の為，実行ファイルと同じ定義を渡す
	target_compile_definitions(${TARGET_NAME} PRIVATE ${SHAVEFMT_HARDENING_DEFINITIONS})
	# 生成物の parser.c は対象外の儘とし，人が書いた走査器にだけ警告を掛ける（未使用引数は上流の書き方で直せない為に除く）
	if(NOT MSVC)
		set_source_files_properties("${SRC_DIR}/scanner.c"
			TARGET_DIRECTORY ${TARGET_NAME}
			PROPERTIES COMPILE_OPTIONS "${SHAVEFMT_C_WARNINGS};-Wno-unused-parameter")
		set_source_files_properties("${SRC_DIR}/scanner.cc"
			TARGET_DIRECTORY ${TARGET_NAME}
			PROPERTIES COMPILE_OPTIONS "${SHAVEFMT_WARNINGS};-Wno-unused-parameter")
	endif()
	# 静的ライブラリでも動的リンクされる shavefmt 経由で extern される為 PIC 必須
	set_target_properties(${TARGET_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

add_ts_grammar(c src)
add_ts_grammar(cpp src)
add_ts_grammar(javascript src)
# TypeScript は JSX の有無で文法が分かれる（`.ts` の型表明 `<T>x` は `.tsx` の文法では JSX と読まれる）
add_ts_grammar(typescript typescript/src)
add_ts_grammar(typescript tsx/src tsx)
add_ts_grammar(java src)
add_ts_grammar(c-sharp src)
add_ts_grammar(json src)
add_ts_grammar(go src)
add_ts_grammar(rust src)
add_ts_grammar(kotlin src)
add_ts_grammar(swift src)
add_ts_grammar(ruby src)
add_ts_grammar(python src)
# tree-sitter-php は PHP 単体 (php_only) と HTML 混在 (php) の２文法を持ち，後者が `<?php ... ?>` を含む実ファイルを扱える
add_ts_grammar(php php/src)
add_ts_grammar(html src)
# CSS / SCSS は tree-sitter-scss で統一する（SCSS は CSS の上位集合で，純 CSS も解析出来る）
add_ts_grammar(scss src)

# 全文法を１つに纏めたインタフェースライブラリ（shavefmt / dumpast からのリンク用）
add_library(ts_parsers INTERFACE)
target_link_libraries(ts_parsers INTERFACE
	ts_c ts_cpp ts_javascript ts_typescript ts_tsx ts_java ts_c_sharp ts_json ts_go
	ts_rust ts_kotlin ts_swift ts_ruby ts_python ts_php ts_html ts_scss
)

add_executable(shavefmt
	src/Main.cpp
	src/Formatter.cpp
	src/Pass/Edit.cpp
	src/Pass/EditBraces.cpp
	src/Pass/EditDeclaration.cpp
	src/Pass/EditDocComment.cpp
	src/Pass/EditExpression.cpp
	src/Pass/EditRuby.cpp
	src/Pass/EditSyntax.cpp
	src/Pass/Layout.cpp
	src/Pass/LineSplit.cpp
	src/Pass/Lint.cpp
	src/Pass/Structure.cpp
	src/Util/BracketPairs.cpp
	src/Util/DeclEdit.cpp
	src/Util/DocSig.cpp
	src/Util/FileIO.cpp
	src/Util/Lang.cpp
	src/Util/NodeKind.cpp
	src/Util/Parallel.cpp
	src/Util/Postprocess.cpp
	src/Util/Preprocess.cpp
	src/Util/SyntaxCheck.cpp
	src/Util/TextEdit.cpp
	src/Util/TsSource.cpp
)
target_compile_options(shavefmt PRIVATE ${SHAVEFMT_WARNINGS} ${SHAVEFMT_HARDENING})
target_compile_definitions(shavefmt PRIVATE ${SHAVEFMT_HARDENING_DEFINITIONS})
# 実行ファイルも位置独立にし，再配置表を読取専用へ固定して遅延結合を使わない（対応する環境だけ）
# `POSITION_INDEPENDENT_CODE` は `check_pie_supported()` を呼ばないと翻訳時の `-fPIE` だけに為り，結合段の `-pie` が付かない
include(CheckPIESupported)
check_pie_supported(LANGUAGES C CXX)
set_target_properties(shavefmt PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_link_options(shavefmt PRIVATE -Wl,-z,relro,-z,now -Wl,-z,noexecstack)
endif()
# 版はプロジェクト宣言を唯一の出所とし，定数を持つヘッダへ書き出す
configure_file(src/Version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/Version.hpp" @ONLY)

# tree-sitter の実行時ライブラリを検索（README の手順で源から構築した 0.26.8 を想定，パッケージ管理の物は版が異なる）
# 環境変数 TREE_SITTER_ROOT 又は CMake 変数 TREE_SITTER_ROOT で明示指定可能
find_path(TREE_SITTER_INCLUDE_DIR tree_sitter/api.h
	HINTS ${TREE_SITTER_ROOT}/include $ENV{TREE_SITTER_ROOT}/include
	PATHS /usr/local/include /opt/homebrew/include /opt/local/include /usr/include
)
# 配布物は tree-sitter が入って居ない計算機で動く必要が有る為，静的な書庫を優先して探す
# 動的ライブラリを掴むと構築機の導入先（Homebrew 等）への絶対パスが埋まり，利用者の計算機で起動出来ない
set(SHAVEFMT_ORIG_LIB_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
if(WIN32)
	set(CMAKE_FIND_LIBRARY_SUFFIXES .lib ${SHAVEFMT_ORIG_LIB_SUFFIXES})
else()
	set(CMAKE_FIND_LIBRARY_SUFFIXES .a ${SHAVEFMT_ORIG_LIB_SUFFIXES})
endif()
find_library(TREE_SITTER_LIBRARY NAMES tree-sitter libtree-sitter
	HINTS ${TREE_SITTER_ROOT}/lib $ENV{TREE_SITTER_ROOT}/lib
	PATHS /usr/local/lib /opt/homebrew/lib /opt/local/lib /usr/lib
)
set(CMAKE_FIND_LIBRARY_SUFFIXES ${SHAVEFMT_ORIG_LIB_SUFFIXES})
if(NOT TREE_SITTER_INCLUDE_DIR OR NOT TREE_SITTER_LIBRARY)
	message(FATAL_ERROR
		"tree-sitter library not found. Build the tree-sitter 0.26.8 runtime from source as described in README.md "
		"(package managers ship other versions) and set TREE_SITTER_ROOT to its install prefix."
	)
endif()
# 同梱の文法は ABI 15 を使う為，古い実行時ライブラリでは構築を開始しない
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES "${TREE_SITTER_INCLUDE_DIR}")
# 記号の有無まで見る為に実行時ライブラリも繋ぐ（宣言だけ在って実体が古い組合せを弾く）
set(CMAKE_REQUIRED_LIBRARIES "${TREE_SITTER_LIBRARY}")
# 解析の打切に使う `ts_parser_parse_with_options` は 0.25 以降の実行時ライブラリだけが持つ為，宣言の有無も同時に見る
check_c_source_compiles("#include <tree_sitter/api.h>
#if TREE_SITTER_LANGUAGE_VERSION < 15
#error Incompatible tree-sitter runtime
#endif
static TSTree *(*ParseWithOptions)(TSParser *, const TSTree *, TSInput, TSParseOptions) = ts_parser_parse_with_options;
int main(void) { (void)ParseWithOptions; return 0; }" SHAVEFMT_TREE_SITTER_COMPATIBLE)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT SHAVEFMT_TREE_SITTER_COMPATIBLE)
	message(FATAL_ERROR
		"tree-sitter must support language ABI 15 and ts_parser_parse_with_options. "
		"Set TREE_SITTER_ROOT to a compatible installation."
	)
endif()
message(STATUS "tree-sitter include: ${TREE_SITTER_INCLUDE_DIR}")
message(STATUS "tree-sitter library: ${TREE_SITTER_LIBRARY}")

target_include_directories(shavefmt PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}" ${TREE_SITTER_INCLUDE_DIR})
target_link_libraries(shavefmt PRIVATE ts_parsers ${TREE_SITTER_LIBRARY})
if(WIN32)
	target_link_libraries(shavefmt PRIVATE advapi32)
	target_compile_definitions(shavefmt PRIVATE NOMINMAX)
endif()
# 主スレッドのスタック領域を 64 MiB に拡張（深いネスト・巨大な二項式連鎖に依る領域不足を防止）
# macOS は ld の `-stack_size` で主スレッドのスタック領域を起動時に固定指定可
# Windows MSVC はリンカの `/STACK:0x4000000` で同等
# Linux は Main.cpp 内 setrlimit で主スレッドのスタック領域を拡張
if(APPLE)
	target_link_options(shavefmt PRIVATE LINKER:-stack_size,0x4000000)
elseif(MSVC)
	# 制御フローガードは翻訳とリンクの双方へ指定して初めて表が入る
	target_link_options(shavefmt PRIVATE /STACK:67108864 /GUARD:CF)
endif()

add_executable(dumpast tools/DumpAst.cpp src/Util/Lang.cpp)
target_compile_options(dumpast PRIVATE ${SHAVEFMT_WARNINGS} ${SHAVEFMT_HARDENING})
target_compile_definitions(dumpast PRIVATE ${SHAVEFMT_HARDENING_DEFINITIONS})

target_include_directories(dumpast PRIVATE src ${TREE_SITTER_INCLUDE_DIR})
target_link_libraries(dumpast PRIVATE ts_parsers ${TREE_SITTER_LIBRARY})

# 読込後の外部保存を決定論的に検査する（整形の所要時間へ依存させない）
include(CTest)
if(BUILD_TESTING)
	add_executable(verify_fileio tools/VerifyFileIO.cpp src/Util/FileIO.cpp)
	target_compile_options(verify_fileio PRIVATE ${SHAVEFMT_WARNINGS} ${SHAVEFMT_HARDENING})
	target_compile_definitions(verify_fileio PRIVATE ${SHAVEFMT_HARDENING_DEFINITIONS})
	target_compile_definitions(verify_fileio PRIVATE ShaveFormatFileIoTestHooks)
	target_include_directories(verify_fileio PRIVATE src)
	if(WIN32)
		target_link_libraries(verify_fileio PRIVATE advapi32)
		target_compile_definitions(verify_fileio PRIVATE NOMINMAX)
	endif()
	add_test(NAME fileio COMMAND verify_fileio)
endif()

# OS/arch を Node.js 形式に正規化（process.platform / process.arch と一致）
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	set(NODE_PLATFORM "darwin")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	set(NODE_PLATFORM "linux")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	set(NODE_PLATFORM "win32")
else()
	set(NODE_PLATFORM "${CMAKE_SYSTEM_NAME}")
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
	set(NODE_ARCH "x64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
	set(NODE_ARCH "arm64")
else()
	set(NODE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(WIN32)
	set(BIN_EXT ".exe")
else()
	set(BIN_EXT "")
endif()
set(VSCODE_BIN_NAME "shavefmt-${NODE_PLATFORM}-${NODE_ARCH}${BIN_EXT}")

# ビルド毎に VSCode 拡張機能の bin/ を OS/arch 別ファイル名で同期
add_custom_command(TARGET shavefmt POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		$<TARGET_FILE:shavefmt>
		${CMAKE_SOURCE_DIR}/vscode-extension/bin/${VSCODE_BIN_NAME}
	COMMENT "Copying shavefmt to vscode-extension/bin/${VSCODE_BIN_NAME}"
)

# 拡張機能の再パッケージ + 再インストール（vsce は package-lock.json で版を固定した開発依存を使い，VS Code は PATH の `code` か macOS の既定の場所の物を使う）
# 使い方：cmake --build build --target extension-reinstall
find_program(VSCODE_CLI NAMES code code.cmd PATHS "/Applications/Visual Studio Code.app/Contents/Resources/app/bin")
# 命令が見付からない時は理由を示して止める（見付からない値の儘実行すると分かり難く失敗する）
if(VSCODE_CLI)
	set(VSCODE_INSTALL_COMMAND ${VSCODE_CLI} --install-extension shavefmt-${PROJECT_VERSION}.vsix --force)
else()
	set(VSCODE_INSTALL_COMMAND
		${CMAKE_COMMAND} -E echo "VS Code command line (code) not found; add it to PATH to install the extension"
		COMMAND ${CMAKE_COMMAND} -E false
	)
endif()
add_custom_target(extension-reinstall
	DEPENDS shavefmt
	COMMAND npm run build
	COMMAND npx --no-install vsce package
	COMMAND ${VSCODE_INSTALL_COMMAND}
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/vscode-extension
	COMMENT "Repackaging and reinstalling VSCode extension"
)
