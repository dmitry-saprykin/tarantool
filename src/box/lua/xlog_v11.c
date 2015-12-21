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
#include "xlog_v11.h"

#include <dirent.h>
#include <fcntl.h>
#include <ctype.h>

#include <say.h>
#include <crc32.h>
#include <fiber.h>
#include <box/error.h>
#include <box/errcode.h>

#include <small/region.h>


const log_magic_t row_marker_v11 = 0xba0babed;
const log_magic_t eof_marker_v11 = 0x10adab1e;
const char v11[] = "0.11\n";

static int
cmp_i64(const void *_a, const void *_b)
{
	const int64_t *a = (const int64_t *) _a, *b = (const int64_t *) _b;
	if (*a == *b)
		return 0;
	return (*a > *b) ? 1 : -1;
}

/* {{{ struct xdir V11 */

void
xdir_v11_create_(struct xdir *dir, const char *dirname,
		 enum xdir_type type)
{
	memset((struct xdir_v11 *)dir, 0, sizeof(struct xdir_v11));
	/* Default mode. */
	dir->mode = 0660;
	snprintf(dir->dirname, PATH_MAX, "%s", dirname);
	if (type == SNAP) {
/*		strcpy(dir->open_wflags, "wxd"); */
/*		dir->filetype = "SNAP\n"; */
		dir->filename_ext = ".snap";
	} else {
/*		strcpy(dir->open_wflags, "wx"); */
/*		dir->filetype = "XLOG\n"; */
		dir->filename_ext = ".xlog";
	}
	dir->type = type;
}

void
xdir_v11_destroy(struct xdir *dir)
{
	struct xdir_v11 *derived = (struct xdir_v11 *)dir;
	derived->sig_count = 0;
	free(derived->sig);
	derived->sig = NULL;
}

void
xdir_v11_create(struct xdir *dir, const char *dirname,
		enum xdir_type type, const struct tt_uuid *server_uuid)
{
	(void )server_uuid;
	return xdir_v11_create_(dir, dirname, type);
}

int
xdir_v11_scan(struct xdir *dir)
{
	DIR *dh = opendir(dir->dirname);        /* log dir */
	int64_t *signatures = NULL;             /* log file names */
	size_t s_count = 0, s_capacity = 0;

	if (dh == NULL) {
		box_error_set(__FILE__, __LINE__, ER_UNKNOWN, "error reading "
			      "directory '%s'", dir->dirname);
		return -1;
	}

	struct dirent *dent;
	/*
	  A note regarding thread safety, readdir vs. readdir_r:

	  POSIX explicitly makes the following guarantee: "The
	  pointer returned by readdir() points to data which may
	  be overwritten by another call to readdir() on the same
	  directory stream. This data is not overwritten by another
	  call to readdir() on a different directory stream.

	  In practice, you don't have a problem with readdir(3)
	  because Android's bionic, Linux's glibc, and OS X and iOS'
	  libc all allocate per-DIR* buffers, and return pointers
	  into those; in Android's case, that buffer is currently about
	  8KiB. If future file systems mean that this becomes an actual
	  limitation, we can fix the C library and all your applications
	  will keep working.

	  See also
	  http://elliotth.blogspot.co.uk/2012/10/how-not-to-use-readdirr3.html
	*/
	while ((dent = readdir(dh)) != NULL) {
		char *ext = strchr(dent->d_name, '.');
		if (ext == NULL)
			continue;
		/*
		 * Compare the rest of the filename with
		 * dir->filename_ext.
		 */
		if (strcmp(ext, dir->filename_ext) != 0)
			continue;

		char *dot;
		long long signature = strtoll(dent->d_name, &dot, 10);
		if (ext != dot ||
		    signature == LLONG_MAX || signature == LLONG_MIN) {
			say_warn("can't parse `%s', skipping", dent->d_name);
			continue;
		}

		if (s_count == s_capacity) {
			s_capacity = s_capacity > 0 ? 2 * s_capacity : 16;
			size_t size = sizeof(*signatures) * s_capacity;
			signatures = (int64_t *) realloc(signatures, size);
			if (signatures == NULL) {
				box_error_set(__FILE__, __LINE__, ER_UNKNOWN,
					      "can't allocate memory with "
					      "realloc for signatures array");
				closedir(dh);
				free(signatures);
				return -1;
			}
		}
		signatures[s_count++] = signature;
	}
	/** Sort the list of files */
	qsort(signatures, s_count, sizeof(*signatures), cmp_i64);
	struct xdir_v11 *derived = (struct xdir_v11 *)dir;
	if (derived->sig) free(derived);
	derived->sig_count = s_count;
	derived->sig = signatures;
	return 0;
}

/* }}} */

/* {{{ struct xlog V11 */

int
xlog_v11_close(struct xlog **lptr)
{
	struct xlog *l = *lptr;
	int r;

	r = fclose(l->f);
	if (r == -1) {
		box_error_set(__FILE__, __LINE__, ER_UNKNOWN, "%s: failed to "
			      "close file", l->filename);
	}
	free(l);
	*lptr = NULL;
	return r;
}

static int
xlog_v11_read_meta(struct xlog *l, struct xlog_meta *meta)
{
	memset(meta, 0, sizeof(struct xlog_meta));
	char filetype[32], buf[256];
	FILE *stream = l->f;

	if (fgets(filetype, sizeof(filetype), stream) == NULL ||
	    fgets(meta->version, sizeof(meta->version), stream) == NULL) {
		box_error_set(__FILE__, __LINE__, ER_INVALID_XLOG, "%s: failed "
			      "to read log file header", l->filename);
		return -1;
	}

	if (strcmp(meta->version, v11) != 0) {
		box_error_set(__FILE__, __LINE__, ER_INVALID_XLOG, "%s: "
			      "unknown version (%.*s)", l->filename,
			      strlen(meta->version) - 1, meta->version);
		return -1;
	}

	meta->type = 0;

	if (strcmp("XLOG\n", filetype) == 0 && l->dir->type == XLOG) {
		meta->type = XLOG;
	} else if (strcmp("SNAP\n", filetype) == 0 && l->dir->type == SNAP) {
		meta->type = SNAP;
	} else {
		box_error_set(__FILE__, __LINE__, ER_INVALID_XLOG, "%s: unknown"
			      " filetype (%.*s)", l->filename,
			      strlen(filetype) - 1, filetype);
		return -1;
	}
	for (;;) {
		if (fgets(buf, sizeof(buf), stream) == NULL) {
			box_error_set(__FILE__, __LINE__, ER_INVALID_XLOG,
				      "failed to read log file header");
			return -1;
		}
		if (strcmp(buf, "\n") == 0 || strcmp(buf, "\r\n") == 0)
			break;
	}
	return 0;
}

struct xlog *
xlog_v11_open_stream(struct xdir *dir, FILE *file, const char *filename)
{
	/*
	 * Check fopen() result the caller first thing, to
	 * preserve the errno.
	 */
	if (file == NULL) {
		box_error_set(__FILE__, __LINE__, ER_UNKNOWN, "%s: failed to "
			      "open file", filename);
		return NULL;
	}

	struct xlog *l = (struct xlog *) calloc(1, sizeof(*l));

	if (l == NULL) {
		box_error_set(__FILE__, __LINE__, ER_UNKNOWN, "can't allocate "
			      "memory with calloc for 'struct xlog'");
		fclose(file);
		return NULL;
	}

	l->f = file;
	snprintf(l->filename, PATH_MAX, "%s", filename);
	l->dir = dir;

	struct xlog_meta meta;

	/* Read xlog/snap metadata and verify it */
	if (xlog_v11_read_meta(l, &meta) == -1) {
		fclose(file);
		free(l);
		return NULL;
	}

	return l;
}

struct xlog *
xlog_v11_ropen(struct xdir *dir, int64_t signature)
{
	const char *filename = format_filename(dir, signature, NONE);
	FILE *f = fopen(filename, "r");
	return xlog_v11_open_stream(dir, f, filename);
}
/* }}} */

/* {{{ struct xlog_cursor V11 */

static const char ROW_EOF[] = "";

const char *
row_reader_v11(FILE *f, uint32_t *rowlen)
{
	struct header_v11 m;

	uint32_t header_crc, data_crc;

	if (fread(&m, sizeof(m), 1, f) != 1)
		return ROW_EOF;

	/* header crc32c calculated on <lsn, tm, len, data_crc32c> */
	header_crc = crc32_calc(0, (const char *) &m +
				offsetof(struct header_v11, lsn),
				sizeof(m) - offsetof(struct header_v11, lsn));

#if 0
	if (m.header_crc32c != header_crc) {
		say_error("header crc32c mismatch");
		return NULL;
	}
#endif

	char *row = (char *) region_alloc(&fiber()->gc, sizeof(m) + m.len);
	if (row == NULL) {
		box_error_set(__FILE__, __LINE__, ER_UNKNOWN, "can't allocate "
			      "memory with region_alloc for tuple");
		return NULL;
	}
	memcpy(row, &m, sizeof(m));

	if (fread(row + sizeof(m), m.len, 1, f) != 1)
		return ROW_EOF;

	data_crc = crc32_calc(0, (const char *) row + sizeof(m), m.len);

#if 0
	if (m.data_crc32c != data_crc) {
		say_error("data crc32c mismatch");
		return NULL;
	}
#endif

	(void )header_crc;
	(void )data_crc;
	*rowlen = m.len + sizeof(m);
	return row;
}

void
xlog_cursor_v11_open(struct xlog_cursor *i, struct xlog *l)
{
	i->log         = l;
	i->row_count   = 0;
	i->good_offset = ftello(l->f);
	i->eof_read    = false;
}

void
xlog_cursor_v11_close(struct xlog_cursor *i)
{
	struct xlog *l = i->log;
	l->rows += i->row_count;
	/*
	 * Since we don't close xlog
	 * we must rewind xlog to last known
	 * good position if there was an error.
	 * Seek back to last known good offset.
	 */
	fseeko(l->f, i->good_offset, SEEK_SET);
	region_free(&fiber()->gc);
}

const char *
xlog_cursor_v11_next(struct xlog_cursor *i, uint32_t *rowlen)
{
	struct xlog *l = i->log;
	const char *row;
	log_magic_t magic;
	off_t marker_offset = 0;

	assert(i->eof_read == false);

	/*
	 * Don't let gc pool grow too much. Yet to
	 * it before reading the next row, to make
	 * sure it's not freed along here.
	 */
	region_free_after(&fiber()->gc, 128 * 1024);

restart:
	if (marker_offset > 0)
		fseeko(l->f, marker_offset + 1, SEEK_SET);

	if (fread(&magic, sizeof(magic), 1, l->f) != 1)
		goto eof;

	while (magic != row_marker_v11) {
		int c = fgetc(l->f);
		if (c == EOF) {
			say_debug("eof while looking for magic");
			goto eof;
		}
		magic = magic >> 8 |
			((log_magic_t) c & 0xff) << (sizeof(magic)*8 - 8);
	}
	marker_offset = ftello(l->f) - sizeof(row_marker_v11);
	if (i->good_offset != marker_offset)
		say_warn("skipped %jd bytes after 0x%08jx offset",
			(intmax_t)(marker_offset - i->good_offset),
			(uintmax_t)i->good_offset);
	say_debug("magic found at 0x%08jx", (uintmax_t)marker_offset);

	row = row_reader_v11(l->f, rowlen);
	if (row == ROW_EOF)
		goto eof;

	if (row == NULL) {
		say_warn("failed to read row");
		goto restart;
	}

	i->good_offset = ftello(l->f);
	i->row_count++;

	return row;
eof:
	/*
	 * The only two cases of fully read file:
	 * 1. sizeof(eof_marker_v11) > 0 and it is the last record in file
	 * 2. sizeof(eof_marker_v11) == 0 and there is no unread data in file
	 */
	if (ftello(l->f) == i->good_offset + sizeof(eof_marker_v11)) {
		fseeko(l->f, i->good_offset, SEEK_SET);
		if (fread(&magic, sizeof(magic), 1, l->f) != 1) {
			say_error("can't read eof marker");
		} else if (magic == eof_marker_v11) {
			i->good_offset = ftello(l->f);
			i->eof_read = true;
		} else if (magic != row_marker_v11) {
			say_error("eof marker is corrupt: %lu",
				  (unsigned long) magic);
		} else {
			/*
			 * Row marker at the end of a file: a sign
			 * of a corrupt log file in case of
			 * recovery, but OK in case we're in local
			 * hot standby or replication relay mode
			 * (i.e. data is being written to the
			 * file. Don't pollute the log, the
			 * condition is taken care of up the
			 * stack.
			 */
		}
	}
	/* No more rows. */
	return NULL;
}

/* }}} */
