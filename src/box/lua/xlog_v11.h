#ifndef TARANTOOL_XLOG_V11_H_INCLUDED
#define TARANTOOL_XLOG_V11_H_INCLUDED
/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "box/xlog.h"

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

typedef uint32_t log_magic_t;

struct xlog_meta {
	enum xdir_type type;
	char version[32];
};

struct xdir_v11 {
	struct xdir base;
	int64_t *sig;
	size_t   sig_count;
};

struct header_v11 {
	uint32_t header_crc32c;
	int64_t lsn;
	double tm;
	uint32_t len;
	uint32_t data_crc32c;
} __attribute__((packed));

struct row_v11 {
	log_magic_t marker;
	struct header_v11 header;
	uint16_t tag;
	uint64_t cookie;
	uint8_t data[];
} __attribute__((packed));

int
xlog_v11_close(struct xlog **lptr);

struct xlog *
xlog_v11_open_stream(struct xdir *dir, FILE *file, const char *filename);

struct xlog *
xlog_v11_ropen(struct xdir *dir, int64_t signature);

void
xlog_cursor_v11_open(struct xlog_cursor *i, struct xlog *l);

void
xlog_cursor_v11_close(struct xlog_cursor *i);

const char *
xlog_cursor_v11_next(struct xlog_cursor *i, uint32_t *rowlen);

#endif /* TARANTOOL_XLOG_V11_H_INCLUDED */
