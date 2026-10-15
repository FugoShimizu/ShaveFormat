cmake_minimum_required(VERSION 3.20)
# macOS の標準ライブラリが提供するメモリリソースを使う為，下限を SDK の既定値へ委ねない
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")
project(ShaveFormat VERSION 0.2.0 LANGUAGES C CXX)

# 静的な C++ ランタイムでも，スレッドの依存を全環境で明示する
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

# glibc の下限は CI の構築環境で固定し，C++ と例外処理のランタイムは配布先に要求しない
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	add_link_options($<$<LINK_LANGUAGE:CXX>:-static-libstdc++> $<$<LINK_LANGUAGE:CXX>:-static-libgcc>)
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# C は C17 とし，bool は <stdbool.h> を用い，nullptr・constexpr・２進リテラルは用いない
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# 複数の構成を持つジェネレータ (Visual Studio) も Release の実行ファイルを build/ の直下に置き，検査のツールと CI の経路を環境で分けない
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
# 配布物は開発のツールの無い計算機で動く必要が有る為，MSVC は C / C++ の実行時を静的に結合する
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# MSVC は源を実行環境の符号ページで読み，狭い文字列の `\u2028` 等を其の符号ページへ変換する為，源と実行の文字集合を UTF-8 に揃える
if(MSVC)
	add_compile_options(/utf-8)
endif()

# 未使用引数・死枝等を機械検出する為，GCC / Clang では自プロジェクトの翻訳単位に警告を有効にする
# エラーにはせず（`-Werror` を付けず）環境差で構築が落ちない様にする
# MSVC は既定の警告水準を用いる
# vendor のパーサーは生成コードで手を入れない為，対象外とする（此処では変数だけ定め各ターゲットへ個別に付与する）
# `-Wparentheses` 系（`&&` を `||` で囲む等の冗長括弧を促すグループ）は演算子の優先順位から不要な括弧を要求する為，抑止する
# 抑止しないと本グループだけで１００件超が出て，未使用変数・符号比較等の実際のエラーを示す警告が埋もれる
# `-Wswitch`（列挙型の switch が扱わない列挙子）は，処理の無い `default` を書かない規約と衝突する為，抑止する
if(MSVC)
	set(SHAVEFMT_WARNINGS "")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	# 冗長括弧を促す下位グループだけを抑止する（`-Wno-parentheses` でグループ全体止めると `if(x = y)` の代入エラーと対応する if の無い else の検出迄失う）
	set(SHAVEFMT_WARNINGS
		-Wall -Wextra -Wno-logical-op-parentheses -Wno-bitwise-op-parentheses -Wno-shift-op-parentheses
		-Wno-overloaded-shift-op-parentheses -Wno-parentheses-equality -Wno-switch
	)
else()
	# GCC は下位グループを持たずグループ全体の抑止しか出来ない
	set(SHAVEFMT_WARNINGS -Wall -Wextra -Wno-parentheses -Wno-switch)
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
# 効果：LTO の上に分岐予測精度向上で +5~15% 速度向上が期待出来る
set(PGO_MODE "off" CACHE STRING "PGO mode: off | generate | use")
set(PGO_DATA "${CMAKE_BINARY_DIR}/pgo-data" CACHE PATH "PGO profile data directory")
set_property(CACHE PGO_MODE PROPERTY STRINGS off generate use)
if(NOT PGO_MODE STREQUAL "off" AND NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	message(FATAL_ERROR "PGO_MODE is supported only with GCC or Clang")
endif()
if(PGO_MODE STREQUAL "generate")
	set(SHAVEFMT_PGO_COMPILE_OPTIONS -fprofile-generate=${PGO_DATA} -fprofile-update=atomic) # 並列の整形でも計数を原子的に更新し，実行記録の食違いを防ぐ
	set(SHAVEFMT_PGO_LINK_OPTIONS -fprofile-generate=${PGO_DATA})
	message(STATUS "PGO: profile-generate enabled (data dir: ${PGO_DATA})")
elseif(PGO_MODE STREQUAL "use")
	set(SHAVEFMT_PGO_COMPILE_OPTIONS -fprofile-use=${PGO_DATA})
	set(SHAVEFMT_PGO_LINK_OPTIONS -fprofile-use=${PGO_DATA})
	# 実行記録に含まれない関数等の警告だけはエラーへ格上げせず，コンパイラ固有の警告名を他のコンパイラへ渡さない
	if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		list(APPEND SHAVEFMT_PGO_COMPILE_OPTIONS -Wno-error=profile-instr-out-of-date -Wno-error=profile-instr-unprofiled)
	elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
		list(APPEND SHAVEFMT_PGO_COMPILE_OPTIONS -Wno-error=coverage-mismatch -Wno-error=missing-profile)
	endif()
	message(STATUS "PGO: profile-use enabled (data dir: ${PGO_DATA})")
endif()

# 他人の書いたソースを手書きの外部スキャナーへ直接渡す為，実行時の防御を有効にする
# 対応しない環境ではフラグ自体がエラーに為る為，使える物だけを選ぶ
include("${CMAKE_CURRENT_SOURCE_DIR}/tools/Hardening.cmake")

include(CheckCXXSourceCompiles)

# 標準ライブラリの添字参照の境界検査（ノード位置の算術と添字が多い為に費用対効果が高い）
# libc++ と libstdc++ でマーカーが違う為，実際に効く方だけを足す
check_cxx_source_compiles(
	"#include <version>\n#if !defined(_LIBCPP_VERSION)\n#error not libc++\n#endif\nint main() { return 0; }"
	SHAVEFMT_HAS_LIBCPP
)
check_cxx_source_compiles(
	"#include <version>\n#if !defined(__GLIBCXX__)\n#error not libstdc++\n#endif\nint main() { return 0; }"
	SHAVEFMT_HAS_LIBSTDCXX
)
if(SHAVEFMT_HAS_LIBCPP)
	set(CMAKE_REQUIRED_DEFINITIONS "-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST")
	check_cxx_source_compiles(
		"#include <vector>\nint main() { std::vector<int> V(1); return V[0]; }"
		SHAVEFMT_HAS_LIBCPP_HARDENING
	)
	set(CMAKE_REQUIRED_DEFINITIONS "")
	if(SHAVEFMT_HAS_LIBCPP_HARDENING)
		list(APPEND SHAVEFMT_HARDENING_DEFINITIONS "_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST")
	endif()
elseif(SHAVEFMT_HAS_LIBSTDCXX)
	list(APPEND SHAVEFMT_HARDENING_DEFINITIONS "_GLIBCXX_ASSERTIONS")
endif()

include(CTest)
set(SHAVEFMT_SOURCE_DIR "${CMAKE_SOURCE_DIR}")
include("${CMAKE_SOURCE_DIR}/tools/Grammars.cmake")
get_property(GRAMMAR_PARSERS GLOBAL PROPERTY SHAVEFMT_GRAMMAR_PARSERS)
string(JOIN "" GRAMMAR_PARSERS ${GRAMMAR_PARSERS})
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/grammar-parsers.tsv" CONTENT "${GRAMMAR_PARSERS}")

# 全文法を１つに纏めたインタフェースライブラリ（shavefmt / dumpast からのリンク用）
add_library(ts_parsers INTERFACE)
# 静的ライブラリの探索順に作用する為に文法ライブラリのリンク順を保つ
target_link_libraries(ts_parsers INTERFACE
	ts_c ts_cpp ts_javascript ts_typescript ts_tsx ts_java ts_c_sharp ts_json ts_go
	ts_rust ts_kotlin ts_swift ts_ruby ts_python ts_php ts_html ts_scss
)

# フォーマッターの本体（実行ファイルとテストが共有し，同じ翻訳単位を２度翻訳しない）
# 可変事実の保持を持つ翻訳単位だけは，テスト専用の基準側（整形仕様 11.1）の為に別の定義でも翻訳する
set(SHAVEFMT_FACT_SOURCES
	src/Core/ControlFlow.cpp
	src/Core/LineBreaker.cpp
	src/Core/SignatureExtractor.cpp
	src/Core/SourceProjector.cpp
	src/Core/SyntaxModel.cpp
	src/Core/SyntaxModelBinding.cpp
	src/Core/SyntaxNormalizer.cpp
	src/Core/SyntaxNormalizerParen.cpp
)
# 翻訳単位のリンク順が静的初期化順に作用し得る為に既存の列を保つ
add_library(shavefmt_common OBJECT
	src/Core/Canonicalizer.cpp
	src/Core/CanonicalizerEmitter.cpp
	src/Core/CanonicalizerFlat.cpp
	src/Core/CanonicalizerGeneric.cpp
	src/Core/CanonicalizerSpacing.cpp
	src/Core/CanonicalizerStructure.cpp
	src/Core/CanonicalizerStructureCore.cpp
	src/Core/CodeLayout.cpp
	src/Core/CommentPlacer.cpp
	src/Core/CommentRepository.cpp
	src/Core/CoreObservation.cpp
	src/Core/DocCommentSynchronizer.cpp
	src/Core/DocCommentReader.cpp
	src/Core/FormatIR.cpp
	src/Core/OutputPosition.cpp
	src/Core/SourceProjectorComments.cpp
	src/Core/SyntaxGeneration.cpp
	src/Core/SyntaxNormalizerBrace.cpp
	src/Core/SyntaxNormalizerDeclaration.cpp
	src/Lint/LintAnalyzer.cpp
	src/Lint/LintSyntax.cpp
	src/Profile/GrammarNames.cpp
	src/Profile/GrammarProfile.cpp
	src/Util/CommentNormalizer.cpp
	src/Util/FileIO.cpp
	src/Util/FormatLimitExceeded.cpp
	src/Util/InputNormalizer.cpp
	src/Util/Lang.cpp
	src/Util/LayoutTypes.cpp
	src/Util/OutputFinalizer.cpp
	src/Util/Parallel.cpp
	src/Util/ParseAdapter.cpp
	src/Util/ParseAdapterBindings.cpp
	src/Util/ParseAdapterRepair.cpp
	src/Util/ParsedTree.cpp
	src/Util/ParserAllocator.cpp
	src/Util/SyntaxValidator.cpp
	src/Util/TextEdit.cpp
	src/Util/TextMetrics.cpp
	src/Util/WorkingMemory.cpp
	src/Util/LineDiff.cpp
)
add_library(shavefmt_core OBJECT ${SHAVEFMT_FACT_SOURCES})
target_link_libraries(shavefmt_common PUBLIC Threads::Threads)
target_link_libraries(shavefmt_core PUBLIC Threads::Threads)
target_compile_options(shavefmt_common PRIVATE ${SHAVEFMT_PGO_COMPILE_OPTIONS})
target_compile_options(shavefmt_core PRIVATE ${SHAVEFMT_PGO_COMPILE_OPTIONS})
target_link_options(shavefmt_core INTERFACE ${SHAVEFMT_PGO_LINK_OPTIONS})
# 共通部分の物も，実行ファイル・既存の検証ツールへ１度だけ渡す
target_sources(shavefmt_core INTERFACE $<TARGET_OBJECTS:shavefmt_formatter> $<TARGET_OBJECTS:shavefmt_common>)
target_compile_options(shavefmt_core PUBLIC ${SHAVEFMT_WARNINGS} ${SHAVEFMT_HARDENING})
target_compile_definitions(shavefmt_core PUBLIC ${SHAVEFMT_HARDENING_DEFINITIONS})
add_executable(shavefmt src/Main.cpp)
target_compile_options(shavefmt PRIVATE ${SHAVEFMT_PGO_COMPILE_OPTIONS})
target_link_libraries(shavefmt PRIVATE shavefmt_core)
# 実行ファイルも位置独立にし，再配置表を読取専用へ固定して遅延結合を使わない（対応する環境だけ）
# `POSITION_INDEPENDENT_CODE` は `check_pie_supported()` を呼ばないと翻訳時の `-fPIE` だけに為り，結合段の `-pie` が付かない
include(CheckPIESupported)
check_pie_supported(LANGUAGES C CXX)
set_target_properties(shavefmt_common shavefmt_core shavefmt PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_link_options(shavefmt PRIVATE -Wl,-z,relro,-z,now -Wl,-z,noexecstack)
endif()
# バージョンはプロジェクト宣言を唯一の出所とし，定数を持つヘッダへ書き出す
configure_file(src/Version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/Version.hpp" @ONLY)

# tree-sitter の実行時ライブラリを検索（README の手順で源から構築した 0.27.0 を想定，パッケージ管理の物はバージョンが異なる）
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
		"tree-sitter library not found. Build the tree-sitter 0.27.0 runtime from source as described in README.md "
		"(package managers ship other versions) and set TREE_SITTER_ROOT to its install prefix."
	)
endif()
# 同梱の文法は ABI 15 を使う為，古い実行時ライブラリでは構築を開始しない
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES "${TREE_SITTER_INCLUDE_DIR}")
# 記号の有無迄見る為に実行時ライブラリも繋ぐ（宣言だけ在って実体が古い組合せを弾く）
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

target_compile_options(shavefmt_common PUBLIC ${SHAVEFMT_WARNINGS} ${SHAVEFMT_HARDENING})
target_compile_definitions(shavefmt_common PUBLIC ${SHAVEFMT_HARDENING_DEFINITIONS})
target_include_directories(shavefmt_common PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}" ${TREE_SITTER_INCLUDE_DIR})
# 性能の計測が計測付で翻訳し直す解析を別の目的ファイルの組にし，CMake のジェネレーターの目的ファイルの名付に依らず差し替える
add_library(shavefmt_parser OBJECT src/Util/SyntaxParser.cpp)
add_library(shavefmt_formatter OBJECT src/Formatter.cpp)
target_compile_options(shavefmt_formatter PRIVATE ${SHAVEFMT_PGO_COMPILE_OPTIONS})
target_compile_options(shavefmt_parser PRIVATE ${SHAVEFMT_PGO_COMPILE_OPTIONS})
target_compile_options(shavefmt_formatter PRIVATE $<TARGET_PROPERTY:shavefmt_common,INTERFACE_COMPILE_OPTIONS>)
target_compile_definitions(shavefmt_formatter PRIVATE $<TARGET_PROPERTY:shavefmt_common,INTERFACE_COMPILE_DEFINITIONS>)
target_include_directories(shavefmt_formatter PRIVATE $<TARGET_PROPERTY:shavefmt_common,INTERFACE_INCLUDE_DIRECTORIES>)
set_target_properties(shavefmt_formatter PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(shavefmt_parser PRIVATE $<TARGET_PROPERTY:shavefmt_common,INTERFACE_COMPILE_OPTIONS>)
target_compile_definitions(shavefmt_parser PRIVATE $<TARGET_PROPERTY:shavefmt_common,INTERFACE_COMPILE_DEFINITIONS>)
target_include_directories(shavefmt_parser PRIVATE $<TARGET_PROPERTY:shavefmt_common,INTERFACE_INCLUDE_DIRECTORIES>)
set_target_properties(shavefmt_parser PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_sources(shavefmt_core INTERFACE $<TARGET_OBJECTS:shavefmt_parser>)
target_include_directories(shavefmt_core PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}" ${TREE_SITTER_INCLUDE_DIR})
# 静的な文法ライブラリの未解決記号を解決する為に tree-sitter 本体を最後に置く
target_link_libraries(shavefmt_core PUBLIC ts_parsers ${TREE_SITTER_LIBRARY})
if(WIN32)
	target_link_libraries(shavefmt_core PUBLIC advapi32)
	target_compile_definitions(shavefmt_core PUBLIC NOMINMAX _WIN32_WINNT=0x0602)
	target_compile_definitions(shavefmt_common PUBLIC NOMINMAX _WIN32_WINNT=0x0602)
endif()
if(MSVC)
	# 制御フローガードは翻訳とリンクの双方へ指定して初めて表が入る
	# Windows 8 の API の下限と PE の宣言を揃える
	target_link_options(shavefmt PRIVATE /GUARD:CF /SUBSYSTEM:CONSOLE,6.02 /OSVERSION:6.2)
endif()

# 翻訳単位のリンク順が静的初期化順に作用し得る為に既存の列を保つ
set(DUMPAST_TARGETS dumpast)
if(BUILD_TESTING)
	list(APPEND DUMPAST_TARGETS dumpast_original)
	get_target_property(ORIGINAL_PARSERS ts_parsers INTERFACE_LINK_LIBRARIES)
	foreach(GRAMMAR IN LISTS EXTENDED_GRAMMARS)
		string(REPLACE "|" ";" GRAMMAR "${GRAMMAR}")
		list(GET GRAMMAR 2 TARGET_SUFFIX)
		list(TRANSFORM ORIGINAL_PARSERS REPLACE "^ts_${TARGET_SUFFIX}$" "ts_${TARGET_SUFFIX}_original")
	endforeach()
	add_library(ts_parsers_original INTERFACE)
	target_link_libraries(ts_parsers_original INTERFACE ${ORIGINAL_PARSERS})
endif()
foreach(DUMPAST_TARGET IN LISTS DUMPAST_TARGETS)
	add_executable(${DUMPAST_TARGET} tools/DumpAst.cpp src/Util/Lang.cpp src/Util/ParserAllocator.cpp
		src/Util/FormatLimitExceeded.cpp
	)
	target_compile_options(${DUMPAST_TARGET} PRIVATE ${SHAVEFMT_WARNINGS} ${SHAVEFMT_HARDENING})
	target_compile_definitions(${DUMPAST_TARGET} PRIVATE ${SHAVEFMT_HARDENING_DEFINITIONS})
	if(WIN32)
		target_compile_definitions(${DUMPAST_TARGET} PRIVATE NOMINMAX)
	endif()

	target_include_directories(${DUMPAST_TARGET} PRIVATE src ${TREE_SITTER_INCLUDE_DIR})
	if(DUMPAST_TARGET STREQUAL "dumpast_original")
		target_link_libraries(${DUMPAST_TARGET} PRIVATE ts_parsers_original ${TREE_SITTER_LIBRARY} Threads::Threads)
	else()
		target_link_libraries(${DUMPAST_TARGET} PRIVATE ts_parsers ${TREE_SITTER_LIBRARY} Threads::Threads)
	endif()
endforeach()

if(BUILD_TESTING)
	# 可変事実と幅要約の再利用を無効にしたテスト専用の基準側（整形仕様 11.1，製品には切替の入口を設けない）
	add_library(shavefmt_reference_core OBJECT ${SHAVEFMT_FACT_SOURCES})
	# 基準側とフック付の検査は，製品の実行記録を別の制御フローへ流用しない
	target_link_libraries(shavefmt_reference_core PUBLIC Threads::Threads)
	target_link_options(shavefmt_reference_core INTERFACE ${SHAVEFMT_PGO_LINK_OPTIONS})
	target_sources(shavefmt_reference_core INTERFACE $<TARGET_OBJECTS:shavefmt_formatter> $<TARGET_OBJECTS:shavefmt_common>
		$<TARGET_OBJECTS:shavefmt_parser>
	)
	target_compile_options(shavefmt_reference_core PUBLIC ${SHAVEFMT_WARNINGS} ${SHAVEFMT_HARDENING})
	target_compile_definitions(shavefmt_reference_core PUBLIC ${SHAVEFMT_HARDENING_DEFINITIONS} ShaveFormatRefFacts)
	target_include_directories(shavefmt_reference_core PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}" ${TREE_SITTER_INCLUDE_DIR})
	target_link_libraries(shavefmt_reference_core PUBLIC ts_parsers ${TREE_SITTER_LIBRARY})
	add_executable(shavefmt_reference src/Main.cpp)
	target_link_libraries(shavefmt_reference PRIVATE shavefmt_reference_core)
	set_target_properties(shavefmt_reference_core shavefmt_reference PROPERTIES POSITION_INDEPENDENT_CODE ON)
	if(WIN32)
		target_link_libraries(shavefmt_reference_core PUBLIC advapi32)
		target_compile_definitions(shavefmt_reference_core PUBLIC NOMINMAX)
	endif()
	if(MSVC)
		target_link_options(shavefmt_reference PRIVATE /GUARD:CF)
	elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		target_link_options(shavefmt_reference PRIVATE -Wl,-z,relro,-z,now -Wl,-z,noexecstack)
	endif()
	# 読込後の外部保存を決定論的に検査する（整形の所要時間へ依存させない）
	add_executable(verify_fileio tools/VerifyFileIO.cpp src/Util/FileIO.cpp src/Util/ParserAllocator.cpp
		src/Util/FormatLimitExceeded.cpp
	)
	target_include_directories(verify_fileio PRIVATE ${TREE_SITTER_INCLUDE_DIR})
	target_link_libraries(verify_fileio PRIVATE ${TREE_SITTER_LIBRARY} Threads::Threads)
	target_compile_options(verify_fileio PRIVATE ${SHAVEFMT_WARNINGS} ${SHAVEFMT_HARDENING})
	target_compile_definitions(verify_fileio PRIVATE ${SHAVEFMT_HARDENING_DEFINITIONS})
	target_compile_definitions(verify_fileio PRIVATE ShaveFormatFileIoTestHooks)
	target_include_directories(verify_fileio PRIVATE src)
	if(WIN32)
		target_link_libraries(verify_fileio PRIVATE advapi32)
		target_compile_definitions(verify_fileio PRIVATE NOMINMAX _WIN32_WINNT=0x0602)
	endif()
	add_test(NAME fileio COMMAND verify_fileio)
	# 確保失敗はテスト専用の翻訳単位へ注入し，製品の入口へ環境変数を追加しない
	add_executable(verify_parser_allocator tools/VerifyParserAllocator.cpp src/Util/ParserAllocator.cpp src/Util/FileIO.cpp
		src/Util/FormatLimitExceeded.cpp src/Util/Lang.cpp
	)
	target_compile_options(verify_parser_allocator PRIVATE ${SHAVEFMT_WARNINGS} ${SHAVEFMT_HARDENING})
	target_compile_definitions(verify_parser_allocator PRIVATE ${SHAVEFMT_HARDENING_DEFINITIONS}
		ShaveFormatParserAllocationTestHooks ShaveFormatFileIoTestHooks
	)
	target_include_directories(verify_parser_allocator PRIVATE src ${TREE_SITTER_INCLUDE_DIR})
	target_link_libraries(verify_parser_allocator PRIVATE ts_parsers ${TREE_SITTER_LIBRARY} Threads::Threads)
	if(WIN32)
		target_compile_definitions(verify_parser_allocator PRIVATE NOMINMAX _WIN32_WINNT=0x0602)
		target_link_libraries(verify_parser_allocator PRIVATE advapi32)
	endif()
	find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
	add_test(NAME grammar_extensions COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/verify_grammar_extensions.py"
		--original $<TARGET_FILE:dumpast_original> --extended $<TARGET_FILE:dumpast>
		--source-dir "${CMAKE_SOURCE_DIR}" --work-dir "${CMAKE_CURRENT_BINARY_DIR}")

	add_test(NAME parser_allocator COMMAND
		"${CMAKE_COMMAND}" -E env --unset=PYTHONOPTIMIZE
		"${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/verify_parser_allocator.py"
		--binary $<TARGET_FILE:verify_parser_allocator>
	)
	if(NOT WIN32)
		# 信号テストの待機だけを別の入口へ置き，配布する入口へ環境変数を持ち込まない
		add_executable(verify_interruption src/Main.cpp)
		target_compile_definitions(verify_interruption PRIVATE ShaveFormatInterruptionTestHooks)
		target_link_libraries(verify_interruption PRIVATE shavefmt_core)
		add_test(NAME interruption COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/verify_interruption.py"
			--binary $<TARGET_FILE:shavefmt> --write-binary $<TARGET_FILE:verify_interruption>)
	else()
		add_test(NAME interruption COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/verify_interruption.py" --binary
			$<TARGET_FILE:shavefmt>
		)
	endif()
	set_tests_properties(interruption PROPERTIES TIMEOUT 90)
	add_test(NAME limited_process COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/run_limited.py" --self-check)
	set_tests_properties(limited_process PROPERTIES TIMEOUT 10)
	# 通常コメントへ変換したドキュメントの本文・順序・移動先の照合を，欠落・複製・順序違反・移動先の不一致の形で検査する（整形仕様 8.1, 11.2）
	add_executable(verify_document_conversion tools/VerifyDocumentConversion.cpp)
	target_link_libraries(verify_document_conversion PRIVATE shavefmt_core)
	add_test(NAME document_conversion COMMAND verify_document_conversion)
	# 基準手順と再利用手順の構文・出力・完全な選択状態及び実作業量を照合する
	add_executable(verify_line_breaking_reuse tools/VerifyLineBreakingReuse.cpp)
	target_link_libraries(verify_line_breaking_reuse PRIVATE shavefmt_core)
	add_test(NAME line_breaking_reuse COMMAND verify_line_breaking_reuse)
	add_test(NAME performance_sources COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/build_10k.py" --self-test)
	add_test(NAME performance_baseline COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/measure_10k.py" --self-test)
	# 工程とパーサーの観測は計数ツールだけへ結合し，通常の製品と他の検査へ持ち込まない
	add_executable(verify_performance tools/VerifyPerformance.cpp src/Formatter.cpp src/Util/SyntaxParser.cpp)
	# 通常の工程と解析の組を結合せず，計測付で翻訳し直した工程・解析と中核・共通部分の目的ファイルで組む
	target_sources(verify_performance PRIVATE $<TARGET_OBJECTS:shavefmt_core> $<TARGET_OBJECTS:shavefmt_common>)
	target_link_libraries(verify_performance PRIVATE ts_parsers ${TREE_SITTER_LIBRARY} Threads::Threads)
	target_link_options(verify_performance PRIVATE ${SHAVEFMT_PGO_LINK_OPTIONS})
	target_compile_options(verify_performance PRIVATE $<TARGET_PROPERTY:shavefmt_common,INTERFACE_COMPILE_OPTIONS>)
	target_compile_definitions(verify_performance PRIVATE $<TARGET_PROPERTY:shavefmt_common,INTERFACE_COMPILE_DEFINITIONS>)
	target_include_directories(verify_performance PRIVATE tools $<TARGET_PROPERTY:shavefmt_common,INTERFACE_INCLUDE_DIRECTORIES>)
	target_compile_definitions(verify_performance PRIVATE SHAVEFMT_PERFORMANCE_PROBE)
	if(WIN32)
		target_link_libraries(verify_performance PRIVATE advapi32)
	endif()
	# 投影・束縛・制御の流れ，規則適用及びエラー領域喪失時の分類を手記述の期待で検査する
	add_executable(verify_syntax_model tools/VerifySyntaxModel.cpp)
	target_link_libraries(verify_syntax_model PRIVATE shavefmt_core)
	add_test(NAME syntax_model COMMAND verify_syntax_model)
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

# 拡張機能の再パッケージ + 再インストール（vsce は package-lock.json でバージョンを固定した開発依存を使い，VS Code は PATH の `code` か macOS の既定の場所の物を使う）
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
