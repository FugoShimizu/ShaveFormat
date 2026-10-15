#pragma once

#include <stdlib.h>

// 外部スキャナーの libc への直接呼出も，実行時ライブラリと同じ確保器へ渡す
extern void *(*ts_current_malloc)(size_t);
extern void *(*ts_current_calloc)(size_t, size_t);
extern void *(*ts_current_realloc)(void *, size_t);
extern void (*ts_current_free)(void *);

#define malloc ts_current_malloc
#define calloc ts_current_calloc
#define realloc ts_current_realloc
#define free ts_current_free
