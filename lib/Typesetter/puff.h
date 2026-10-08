/* puff.h
  Copyright (C) 2002-2013 Mark Adler, all rights reserved
  version 2.3, 21 Jan 2013

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the author be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Mark Adler    madler@alumni.caltech.edu
 */


/*
 * See puff.c for purpose and usage.
 *
 * Vendored from https://github.com/madler/zlib/tree/master/contrib/puff
 * for EPUB zip inflation (raw DEFLATE, no zlib/gzip wrapper). Chosen over
 * zlib's inflate for its tiny footprint (~4K code, <2K stack, no heap use).
 *
 * Altered from upstream: puff_stream() inflates from a refill callback so a
 * compressed member does not have to sit in a single heap allocation.
 */
#ifndef NIL
#  define NIL ((unsigned char *)0)      /* for no output option */
#endif

#ifdef __cplusplus
extern "C" {
#endif

int puff(unsigned char *dest,           /* pointer to destination pointer */
         unsigned long *destlen,        /* amount of output space */
         const unsigned char *source,   /* pointer to source data pointer */
         unsigned long *sourcelen);     /* amount of input available */

/* Read compressed bytes into buf (capacity cap). Return bytes written, 0 on
 * end of input, or a negative value on I/O error. */
typedef int (*puff_refill_fn)(unsigned char *buf, unsigned long cap, void *user);

/* Like puff(), but pulls compressed bytes through refill() into the caller-
 * provided inbuf instead of requiring the whole stream in memory. */
int puff_stream(unsigned char *dest, unsigned long *destlen,
                puff_refill_fn refill, void *user,
                unsigned char *inbuf, unsigned long inbufcap);

/* Write uncompressed bytes. Return 0 on success, non-zero on I/O error. */
typedef int (*puff_flush_fn)(const unsigned char *buf, unsigned long n, void *user);

/* Inflate to a 32 KB history window, flushing older bytes through flush().
 * windowcap must be 32768 (DEFLATE max match distance). *outcnt is set to
 * the full uncompressed size on success. */
int puff_stream_out(puff_refill_fn refill, void *in_user,
                    unsigned char *inbuf, unsigned long inbufcap,
                    unsigned char *window, unsigned long windowcap,
                    puff_flush_fn flush, void *out_user,
                    unsigned long *outcnt);

#ifdef __cplusplus
}
#endif
