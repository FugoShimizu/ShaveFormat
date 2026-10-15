# 実行時ライブラリの C にも，本体と同じ処理系別の防御を適用する
include("${CMAKE_CURRENT_LIST_DIR}/Hardening.cmake")
add_compile_options(${SHAVEFMT_C_HARDENING})

# 既定の防御水準を下げず，最適化する構成へ境界検査を足す
add_compile_definitions(${SHAVEFMT_C_HARDENING_DEFINITIONS})
