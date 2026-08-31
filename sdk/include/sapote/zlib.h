/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_SDK_ZLIB_H
#define SAPOTE_SDK_ZLIB_H

#include <zlib.h>

/*
 * Prepare a zeroed Z_SOLO stream with Sapote's checked calloc/free adapter.
 * The caller still owns the matching deflateEnd/inflateEnd on every path.
 */
int sapote_zlib_stream_prepare(z_streamp stream);

#endif
