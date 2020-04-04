/*
 * test_slow_decompression.c
 *
 * Test how quickly libdeflate decompresses degenerate/malicious compressed data
 * streams that start new Huffman blocks extremely frequently.
 */

#include "test_util.h"

/*
 * Generate a DEFLATE stream containing all empty "static Huffman" blocks.
 *
 * libdeflate used to decompress this very slowly (~1000x slower than typical
 * data), but now it's much faster (only ~2x slower than typical data) because
 * now it skips rebuilding the decode tables for the static Huffman codes when
 * they're already loaded into the decompressor.
 */
static void
generate_empty_static_huffman_blocks(u8 *p, size_t len)
{
	struct output_bitstream os = { .next = p, .end = p + len };

	while (put_bits(&os, 0, 1) &&	/* BFINAL: 0 */
	       put_bits(&os, 1, 2) &&	/* BTYPE: STATIC_HUFFMAN */
	       put_bits(&os, 0, 7))	/* litlensym_256 (end-of-block) */
		;
}

static bool
generate_empty_dynamic_huffman_block(struct output_bitstream *os)
{
	int i;

	if (!put_bits(os, 0, 1))	/* BFINAL: 0 */
		return false;
	if (!put_bits(os, 2, 2))	/* BTYPE: DYNAMIC_HUFFMAN */
		return false;

	/*
	 * Write a minimal Huffman code, then the end-of-block symbol.
	 *
	 * Litlen code:
	 *	litlensym_256 (end-of-block)	freq=1 len=1 codeword=0
	 * Offset code:
	 *	offsetsym_0 (unused)		freq=0 len=1 codeword=0
	 *
	 * Litlen and offset codeword lengths:
	 *	[0..255] = 0	presym_{18,18}
	 *	[256]	 = 1	presym_1
	 *	[257]	 = 1	presym_1
	 *
	 * Precode:
	 *	presym_1	freq=2 len=1 codeword=0
	 *	presym_18	freq=2 len=1 codeword=1
	 */

	if (!put_bits(os, 0, 5))	/* num_litlen_syms: 0 + 257 */
		return false;
	if (!put_bits(os, 0, 5))	/* num_offset_syms: 0 + 1 */
		return false;
	if (!put_bits(os, 14, 4))	/* num_explicit_precode_lens: 14 + 4 */
		return false;
	/*
	 * Precode codeword lengths: order is
	 * [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15]
	 */
	for (i = 0; i < 2; i++) {	/* presym_{16,17}: len=0 */
		if (!put_bits(os, 0, 3))
			return false;
	}
	if (!put_bits(os, 1, 3))	/* presym_18: len=1 */
		return false;
	for (i = 0; i < 14; i++) {	/* presym_{0,...,14}: len=0 */
		if (!put_bits(os, 0, 3))
			return false;
	}
	if (!put_bits(os, 1, 3))	/* presym_1: len=1 */
		return false;

	/* Litlen and offset codeword lengths */
	for (i = 0; i < 2; i++) {
		if (!put_bits(os, 1, 1) || /* presym_18, 128 zeroes */
		    !put_bits(os, 117, 7))
			return false;
	}
	if (!put_bits(os, 0, 1))	/* presym_1 */
		return false;
	if (!put_bits(os, 0, 1))	/* presym_1 */
		return false;
	/* Done writing the Huffman codes */

	return put_bits(os, 0, 1);	/* litlensym_256 (end-of-block) */
}

/*
 * Generate a DEFLATE stream containing all empty "dynamic Huffman" blocks.
 *
 * This is the worst known case currently, being ~100x slower to decompress than
 * typical data.
 */
static void
generate_empty_dynamic_huffman_blocks(u8 *p, size_t len)
{
	struct output_bitstream os = { .next = p, .end = p + len };

	while (generate_empty_dynamic_huffman_block(&os))
		;
}

#define NUM_ITERATIONS	100

static u64
do_test_libdeflate(const char *input_type, const u8 *in, size_t in_nbytes,
		   u8 *out, size_t out_nbytes_avail)
{
	struct libdeflate_decompressor *d;
	enum libdeflate_result res;
	u64 t;
	int i;

	d = libdeflate_alloc_decompressor();
	ASSERT(d != NULL);

	t = timer_ticks();
	for (i = 0; i < NUM_ITERATIONS; i++) {
		res = libdeflate_deflate_decompress(d, in, in_nbytes, out,
						    out_nbytes_avail, NULL);
		ASSERT(res == LIBDEFLATE_BAD_DATA ||
		       res == LIBDEFLATE_INSUFFICIENT_SPACE);
	}
	t = timer_ticks() - t;

	printf("[%s, libdeflate]: %"PRIu64" KB/s\n", input_type,
	       timer_KB_per_s((u64)in_nbytes * NUM_ITERATIONS, t));

	libdeflate_free_decompressor(d);
	return t;
}

static u64
do_test_zlib(const char *input_type, const u8 *in, size_t in_nbytes,
	     u8 *out, size_t out_nbytes_avail)
{
	z_stream z;
	int res;
	u64 t;
	int i;

	memset(&z, 0, sizeof(z));
	res = inflateInit2(&z, -15);
	ASSERT(res == Z_OK);

	t = timer_ticks();
	for (i = 0; i < NUM_ITERATIONS; i++) {
		inflateReset(&z);
		z.next_in = (void *)in;
		z.avail_in = in_nbytes;
		z.next_out = out;
		z.avail_out = out_nbytes_avail;
		res = inflate(&z, Z_FINISH);
		ASSERT(res == Z_BUF_ERROR || res == Z_DATA_ERROR);
	}
	t = timer_ticks() - t;

	printf("[%s, zlib      ]: %"PRIu64" KB/s\n", input_type,
	       timer_KB_per_s((u64)in_nbytes * NUM_ITERATIONS, t));

	inflateEnd(&z);
	return t;
}

/*
 * Test case from https://github.com/ebiggers/libdeflate/issues/33
 * with the gzip header and footer removed to leave just the DEFLATE stream
 */
static const u8 orig_repro[3962] =
	"\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x6a\x6a\x6a\x6a\x6a\x6a\x6a\x6a\x6a\x6a\x6a\x6a"
	"\x6a\x6a\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48\x00\x20"
	"\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x00\xea\x04\x48\x00\x20\x80\x28"
	"\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11"
	"\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x00\xea\x04\x48"
	"\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48\x00\x20\x80"
	"\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x00\xea\x04\x48\x00\x20\x80\x28\x00"
	"\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\xea"
	"\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\xea"
	"\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48"
	"\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x00\xea\x04\x48\x00\x20"
	"\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48\x00\x20\x80\x28\x00"
	"\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11"
	"\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x63"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92"
	"\x63\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x00"
	"\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48"
	"\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x00\xea\x04\x48\x00\x20"
	"\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48\x00\x20\x80\x28\x00"
	"\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x92\x63\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\xea"
	"\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x00\xea\x04\x48"
	"\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11"
	"\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x92\x63\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48\x00\x20\x80\x28\x00"
	"\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x92\x63\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11"
	"\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63"
	"\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x00\xea"
	"\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x92\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x6a\x6a\x6a\x6a\x6a\x6a\x6a\x6a\x6a\x6a\x6a\x6a\x6a"
	"\x6a\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48\x00\x20\x80"
	"\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x00\xea\x04\x48\x00\x20\x80\x28\x00"
	"\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48\x00\x20\x80\x28\x00"
	"\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x92\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x6a\x6a\x6a\x6a\x6a\x6a\x6a\x6a\x6a\x6a\x6a"
	"\x6a\x6a\x6a\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48\x00"
	"\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x00\xea\x04\x48\x00\x20\x80"
	"\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00"
	"\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x00\xea\x04"
	"\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48\x00\x20"
	"\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x00\xea\x04\x48\x00\x20\x80\x28"
	"\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00"
	"\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04"
	"\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x00\xea\x04\x48\x00"
	"\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48\x00\x20\x80\x28"
	"\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00"
	"\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x63\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x92\x63\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04"
	"\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x00\xea\x04\x48\x00"
	"\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48\x00\x20\x80\x28"
	"\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x92\x63\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00"
	"\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x00\xea\x04"
	"\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00"
	"\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x92\x63\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48\x00\x20\x80\x28"
	"\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x92\x63\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00"
	"\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92"
	"\x63\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x00"
	"\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x63\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48\x00"
	"\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x00\xea\x04\x48\x00\x20\x80"
	"\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00"
	"\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92"
	"\x63\x00\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00"
	"\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x00\xea\x04"
	"\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48\x00\x20"
	"\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x00\xea\x04\x48\x00\x20\x80\x28"
	"\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1a\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00"
	"\xea\x04\x48\x00\x20\x80\x28\x00\x00\x11\x00\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b"
	"\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x1b\x92\x63\x00\x04\xea\x48\x00\x20"
	"\x80\x28\x00\x00\x11\x1b\x1b\x1b\x1b\x92\x63\x00\xea\x04\x48\x00"
	"\x20\x80\x28\x00\x00\x11\x00\x00\x01\x04\x00\x3f\x00\x00\x00\x00"
	"\x28\xf7\xff\x00\xff\xff\xff\xff\x00\x00";

int
tmain(int argc, tchar *argv[])
{
	u8 in[4096];
	u8 out[10000];
	u64 t, tz;

	program_invocation_name = get_filename(argv[0]);

	begin_performance_test();

	/* static huffman case */
	generate_empty_static_huffman_blocks(in, sizeof(in));
	t = do_test_libdeflate("static huffman", in, sizeof(in),
			       out, sizeof(out));
	tz = do_test_zlib("static huffman", in, sizeof(in), out, sizeof(out));
	/*
	 * libdeflate is faster than zlib in this case, e.g.
	 *	[static huffman, libdeflate]: 215861 KB/s
	 *	[static huffman, zlib      ]: 73651 KB/s
	 */
	putchar('\n');
	ASSERT(t < tz);

	/* dynamic huffman case */
	generate_empty_dynamic_huffman_blocks(in, sizeof(in));
	t = do_test_libdeflate("dynamic huffman", in, sizeof(in),
			       out, sizeof(out));
	tz = do_test_zlib("dynamic huffman", in, sizeof(in), out, sizeof(out));
	/*
	 * libdeflate is slower than zlib in this case, though not super bad.
	 *	[dynamic huffman, libdeflate]: 6277 KB/s
	 *	[dynamic huffman, zlib      ]: 10419 KB/s
	 * FIXME: make it faster.
	 */
	putchar('\n');
	ASSERT(t < 4 * tz);

	/* original reproducer */
	t = do_test_libdeflate("original repro", orig_repro, sizeof(orig_repro),
			       out, sizeof(out));
	tz = do_test_zlib("original repro", orig_repro, sizeof(orig_repro),
			  out, sizeof(out));
	ASSERT(t < tz);

	return 0;
}
