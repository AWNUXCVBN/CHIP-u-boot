/*
 * Generic binary BCH encoding/decoding library
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright © 2011 Parrot S.A.
 *
 * Author: Ivan Djelic <ivan.djelic@parrot.com>
 *
 * Description:
 *
 * This library provides runtime configurable encoding/decoding of binary
 * Bose-Chaudhuri-Hocquenghem (BCH) codes.
 *
 * Call init_bch to get a pointer to a newly allocated bch_control structure for
 * the given m (Galois field order), t (error correction capability) and
 * (optional) primitive polynomial parameters.
 *
 * Call encode_bch to compute and store ecc parity bytes to a given buffer.
 * Call decode_bch to detect and locate errors in received data.
 *
 * On systems supporting hw BCH features, intermediate results may be provided
 * to decode_bch in order to skip certain steps. See decode_bch() documentation
 * for details.
 *
 * Option CONFIG_BCH_CONST_PARAMS can be used to force fixed values of
 * parameters m and t; thus allowing extra compiler optimizations and providing
 * better (up to 2x) encoding performance. Using this option makes sense when
 * (m,t) are fixed and known in advance, e.g. when using BCH error correction
 * on a particular NAND flash device.
 *
 * Algorithmic details:
 *
 * Encoding is performed by processing 32 input bits in parallel, using 4
 * remainder lookup tables.
 *
 * The final stage of decoding involves the following internal steps:
 * a. Syndrome computation
 * b. Error locator polynomial computation using Berlekamp-Massey algorithm
 * c. Error locator root finding (by far the most expensive step)
 *
 * In this implementation, step c is not performed using the usual Chien search.
 * Instead, an alternative approach described in [1] is used. It consists in
 * factoring the error locator polynomial using the Berlekamp Trace algorithm
 * (BTA) down to a certain degree (4), after which ad hoc low-degree polynomial
 * solving techniques [2] are used. The resulting algorithm, called BTZ, yields
 * much better performance than Chien search for usual (m,t) values (typically
 * m >= 13, t < 32, see [1]).
 *
 * [1] B. Biswas, V. Herbert. Efficient root finding of polynomials over fields
 * of characteristic 2, in: Western European Workshop on Research in Cryptology
 * - WEWoRC 2009, Graz, Austria, LNCS, Springer, July 2009, to appear.
 * [2] [Zin96] V.A. Zinoviev. On the solution of equations of degree 10 over
 * finite fields GF(2^q). In Rapport de recherche INRIA no 2829, 1996.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <asm/byteorder.h>
#include <endian.h>
#include <getopt.h>

#if defined(CONFIG_BCH_CONST_PARAMS)
#define GF_M(_p)               (CONFIG_BCH_CONST_M)
#define GF_T(_p)               (CONFIG_BCH_CONST_T)
#define GF_N(_p)               ((1 << (CONFIG_BCH_CONST_M))-1)
#else
#define GF_M(_p)               ((_p)->m)
#define GF_T(_p)               ((_p)->t)
#define GF_N(_p)               ((_p)->n)
#endif

#define DIV_ROUND_UP(n,d) (((n) + (d) - 1) / (d))

#define BCH_ECC_WORDS(_p)      DIV_ROUND_UP(GF_M(_p)*GF_T(_p), 32)
#define BCH_ECC_BYTES(_p)      DIV_ROUND_UP(GF_M(_p)*GF_T(_p), 8)

#ifndef dbg
#define dbg(_fmt, args...)     do {} while (0)
#endif

#undef cpu_to_be32
#define cpu_to_be32 htobe32
#define kfree free
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define BCH_PRIMITIVE_POLY	0x5803

struct image_info {
	int ecc_strength;
	int ecc_step_size;
	int page_size;
	int oob_size;
	int block_size;
	int usable_page_size;
	int repeat;
	int disable_scrambler;
	const char *source;
	const char *dest;
};

/**
 * struct bch_control - BCH control structure
 * @m:          Galois field order
 * @n:          maximum codeword size in bits (= 2^m-1)
 * @t:          error correction capability in bits
 * @ecc_bits:   ecc exact size in bits, i.e. generator polynomial degree (<=m*t)
 * @ecc_bytes:  ecc max size (m*t bits) in bytes
 * @a_pow_tab:  Galois field GF(2^m) exponentiation lookup table
 * @a_log_tab:  Galois field GF(2^m) log lookup table
 * @mod8_tab:   remainder generator polynomial lookup tables
 * @ecc_buf:    ecc parity words buffer
 * @ecc_buf2:   ecc parity words buffer
 * @xi_tab:     GF(2^m) base for solving degree 2 polynomial roots
 * @syn:        syndrome buffer
 * @cache:      log-based polynomial representation buffer
 * @elp:        error locator polynomial
 * @poly_2t:    temporary polynomials of degree 2t
 */
struct bch_control {
	unsigned int    m;
	unsigned int    n;
	unsigned int    t;
	unsigned int    ecc_bits;
	unsigned int    ecc_bytes;
/* private: */
	uint16_t       *a_pow_tab;
	uint16_t       *a_log_tab;
	uint32_t       *mod8_tab;
	uint32_t       *ecc_buf;
	uint32_t       *ecc_buf2;
	unsigned int   *xi_tab;
	unsigned int   *syn;
	int            *cache;
	struct gf_poly *elp;
	struct gf_poly *poly_2t[4];
};

static int fls(int x)
{
	int r = 32;

	if (!x)
		return 0;
	if (!(x & 0xffff0000u)) {
		x <<= 16;
		r -= 16;
	}
	if (!(x & 0xff000000u)) {
		x <<= 8;
		r -= 8;
	}
	if (!(x & 0xf0000000u)) {
		x <<= 4;
		r -= 4;
	}
	if (!(x & 0xc0000000u)) {
		x <<= 2;
		r -= 2;
	}
	if (!(x & 0x80000000u)) {
		x <<= 1;
		r -= 1;
	}
	return r;
}

/*
 * represent a polynomial over GF(2^m)
 */
struct gf_poly {
	unsigned int deg;    /* polynomial degree */
	unsigned int c[0];   /* polynomial terms */
};

/* given its degree, compute a polynomial size in bytes */
#define GF_POLY_SZ(_d) (sizeof(struct gf_poly)+((_d)+1)*sizeof(unsigned int))

/* polynomial of degree 1 */
struct gf_poly_deg1 {
	struct gf_poly poly;
	unsigned int   c[2];
};

/*
 * same as encode_bch(), but process input data one byte at a time
 */
static void encode_bch_unaligned(struct bch_control *bch,
				 const unsigned char *data, unsigned int len,
				 uint32_t *ecc)
{
	int i;
	const uint32_t *p;
	const int l = BCH_ECC_WORDS(bch)-1;

	while (len--) {
		p = bch->mod8_tab + (l+1)*(((ecc[0] >> 24)^(*data++)) & 0xff);

		for (i = 0; i < l; i++)
			ecc[i] = ((ecc[i] << 8)|(ecc[i+1] >> 24))^(*p++);

		ecc[l] = (ecc[l] << 8)^(*p);
	}
}

/*
 * convert ecc bytes to aligned, zero-padded 32-bit ecc words
 */
static void load_ecc8(struct bch_control *bch, uint32_t *dst,
		      const uint8_t *src)
{
	uint8_t pad[4] = {0, 0, 0, 0};
	unsigned int i, nwords = BCH_ECC_WORDS(bch)-1;

	for (i = 0; i < nwords; i++, src += 4)
		dst[i] = (src[0] << 24)|(src[1] << 16)|(src[2] << 8)|src[3];

	memcpy(pad, src, BCH_ECC_BYTES(bch)-4*nwords);
	dst[nwords] = (pad[0] << 24)|(pad[1] << 16)|(pad[2] << 8)|pad[3];
}

/*
 * convert 32-bit ecc words to ecc bytes
 */
static void store_ecc8(struct bch_control *bch, uint8_t *dst,
		       const uint32_t *src)
{
	uint8_t pad[4];
	unsigned int i, nwords = BCH_ECC_WORDS(bch)-1;

	for (i = 0; i < nwords; i++) {
		*dst++ = (src[i] >> 24);
		*dst++ = (src[i] >> 16) & 0xff;
		*dst++ = (src[i] >>  8) & 0xff;
		*dst++ = (src[i] >>  0) & 0xff;
	}
	pad[0] = (src[nwords] >> 24);
	pad[1] = (src[nwords] >> 16) & 0xff;
	pad[2] = (src[nwords] >>  8) & 0xff;
	pad[3] = (src[nwords] >>  0) & 0xff;
	memcpy(dst, pad, BCH_ECC_BYTES(bch)-4*nwords);
}

/**
 * encode_bch - calculate BCH ecc parity of data
 * @bch:   BCH control structure
 * @data:  data to encode
 * @len:   data length in bytes
 * @ecc:   ecc parity data, must be initialized by caller
 *
 * The @ecc parity array is used both as input and output parameter, in order to
 * allow incremental computations. It should be of the size indicated by member
 * @ecc_bytes of @bch, and should be initialized to 0 before the first call.
 *
 * The exact number of computed ecc parity bits is given by member @ecc_bits of
 * @bch; it may be less than m*t for large values of t.
 */
static void encode_bch(struct bch_control *bch, const uint8_t *data,
		unsigned int len, uint8_t *ecc)
{
	const unsigned int l = BCH_ECC_WORDS(bch)-1;
	unsigned int i, mlen;
	unsigned long m;
	uint32_t w, r[l+1];
	const uint32_t * const tab0 = bch->mod8_tab;
	const uint32_t * const tab1 = tab0 + 256*(l+1);
	const uint32_t * const tab2 = tab1 + 256*(l+1);
	const uint32_t * const tab3 = tab2 + 256*(l+1);
	const uint32_t *pdata, *p0, *p1, *p2, *p3;

	if (ecc) {
		/* load ecc parity bytes into internal 32-bit buffer */
		load_ecc8(bch, bch->ecc_buf, ecc);
	} else {
		memset(bch->ecc_buf, 0, sizeof(r));
	}

	/* process first unaligned data bytes */
	m = ((unsigned long)data) & 3;
	if (m) {
		mlen = (len < (4-m)) ? len : 4-m;
		encode_bch_unaligned(bch, data, mlen, bch->ecc_buf);
		data += mlen;
		len  -= mlen;
	}

	/* process 32-bit aligned data words */
	pdata = (uint32_t *)data;
	mlen  = len/4;
	data += 4*mlen;
	len  -= 4*mlen;
	memcpy(r, bch->ecc_buf, sizeof(r));

	/*
	 * split each 32-bit word into 4 polynomials of weight 8 as follows:
	 *
	 * 31 ...24  23 ...16  15 ... 8  7 ... 0
	 * xxxxxxxx  yyyyyyyy  zzzzzzzz  tttttttt
	 *                               tttttttt  mod g = r0 (precomputed)
	 *                     zzzzzzzz  00000000  mod g = r1 (precomputed)
	 *           yyyyyyyy  00000000  00000000  mod g = r2 (precomputed)
	 * xxxxxxxx  00000000  00000000  00000000  mod g = r3 (precomputed)
	 * xxxxxxxx  yyyyyyyy  zzzzzzzz  tttttttt  mod g = r0^r1^r2^r3
	 */
	while (mlen--) {
		/* input data is read in big-endian format */
		w = r[0]^cpu_to_be32(*pdata++);
		p0 = tab0 + (l+1)*((w >>  0) & 0xff);
		p1 = tab1 + (l+1)*((w >>  8) & 0xff);
		p2 = tab2 + (l+1)*((w >> 16) & 0xff);
		p3 = tab3 + (l+1)*((w >> 24) & 0xff);

		for (i = 0; i < l; i++)
			r[i] = r[i+1]^p0[i]^p1[i]^p2[i]^p3[i];

		r[l] = p0[l]^p1[l]^p2[l]^p3[l];
	}
	memcpy(bch->ecc_buf, r, sizeof(r));

	/* process last unaligned bytes */
	if (len)
		encode_bch_unaligned(bch, data, len, bch->ecc_buf);

	/* store ecc parity bytes into original parity buffer */
	if (ecc)
		store_ecc8(bch, ecc, bch->ecc_buf);
}

static inline int modulo(struct bch_control *bch, unsigned int v)
{
	const unsigned int n = GF_N(bch);
	while (v >= n) {
		v -= n;
		v = (v & n) + (v >> GF_M(bch));
	}
	return v;
}

/*
 * shorter and faster modulo function, only works when v < 2N.
 */
static inline int mod_s(struct bch_control *bch, unsigned int v)
{
	const unsigned int n = GF_N(bch);
	return (v < n) ? v : v-n;
}

static inline int deg(unsigned int poly)
{
	/* polynomial degree is the most-significant bit index */
	return fls(poly)-1;
}

static inline int parity(unsigned int x)
{
	/*
	 * public domain code snippet, lifted from
	 * http://www-graphics.stanford.edu/~seander/bithacks.html
	 */
	x ^= x >> 1;
	x ^= x >> 2;
	x = (x & 0x11111111U) * 0x11111111U;
	return (x >> 28) & 1;
}

/* Galois field basic operations: multiply, divide, inverse, etc. */

static inline unsigned int gf_mul(struct bch_control *bch, unsigned int a,
				  unsigned int b)
{
	return (a && b) ? bch->a_pow_tab[mod_s(bch, bch->a_log_tab[a]+
					       bch->a_log_tab[b])] : 0;
}

static inline unsigned int gf_sqr(struct bch_control *bch, unsigned int a)
{
	return a ? bch->a_pow_tab[mod_s(bch, 2*bch->a_log_tab[a])] : 0;
}

static inline unsigned int gf_div(struct bch_control *bch, unsigned int a,
				  unsigned int b)
{
	return a ? bch->a_pow_tab[mod_s(bch, bch->a_log_tab[a]+
					GF_N(bch)-bch->a_log_tab[b])] : 0;
}

static inline unsigned int gf_inv(struct bch_control *bch, unsigned int a)
{
	return bch->a_pow_tab[GF_N(bch)-bch->a_log_tab[a]];
}

static inline unsigned int a_pow(struct bch_control *bch, int i)
{
	return bch->a_pow_tab[modulo(bch, i)];
}

static inline int a_log(struct bch_control *bch, unsigned int x)
{
	return bch->a_log_tab[x];
}

static inline int a_ilog(struct bch_control *bch, unsigned int x)
{
	return mod_s(bch, GF_N(bch)-bch->a_log_tab[x]);
}

/*
 * generate Galois field lookup tables
 */
static int build_gf_tables(struct bch_control *bch, unsigned int poly)
{
	unsigned int i, x = 1;
	const unsigned int k = 1 << deg(poly);

	/* primitive polynomial must be of degree m */
	if (k != (1u << GF_M(bch)))
		return -1;

	for (i = 0; i < GF_N(bch); i++) {
		bch->a_pow_tab[i] = x;
		bch->a_log_tab[x] = i;
		if (i && (x == 1))
			/* polynomial is not primitive (a^i=1 with 0<i<2^m-1) */
			return -1;
		x <<= 1;
		if (x & k)
			x ^= poly;
	}
	bch->a_pow_tab[GF_N(bch)] = 1;
	bch->a_log_tab[0] = 0;

	return 0;
}

/*
 * compute generator polynomial remainder tables for fast encoding
 */
static void build_mod8_tables(struct bch_control *bch, const uint32_t *g)
{
	int i, j, b, d;
	uint32_t data, hi, lo, *tab;
	const int l = BCH_ECC_WORDS(bch);
	const int plen = DIV_ROUND_UP(bch->ecc_bits+1, 32);
	const int ecclen = DIV_ROUND_UP(bch->ecc_bits, 32);

	memset(bch->mod8_tab, 0, 4*256*l*sizeof(*bch->mod8_tab));

	for (i = 0; i < 256; i++) {
		/* p(X)=i is a small polynomial of weight <= 8 */
		for (b = 0; b < 4; b++) {
			/* we want to compute (p(X).X^(8*b+deg(g))) mod g(X) */
			tab = bch->mod8_tab + (b*256+i)*l;
			data = i << (8*b);
			while (data) {
				d = deg(data);
				/* subtract X^d.g(X) from p(X).X^(8*b+deg(g)) */
				data ^= g[0] >> (31-d);
				for (j = 0; j < ecclen; j++) {
					hi = (d < 31) ? g[j] << (d+1) : 0;
					lo = (j+1 < plen) ?
						g[j+1] >> (31-d) : 0;
					tab[j] ^= hi|lo;
				}
			}
		}
	}
}

/*
 * build a base for factoring degree 2 polynomials
 */
static int build_deg2_base(struct bch_control *bch)
{
	const int m = GF_M(bch);
	int i, j, r;
	unsigned int sum, x, y, remaining, ak = 0, xi[m];

	/* find k s.t. Tr(a^k) = 1 and 0 <= k < m */
	for (i = 0; i < m; i++) {
		for (j = 0, sum = 0; j < m; j++)
			sum ^= a_pow(bch, i*(1 << j));

		if (sum) {
			ak = bch->a_pow_tab[i];
			break;
		}
	}
	/* find xi, i=0..m-1 such that xi^2+xi = a^i+Tr(a^i).a^k */
	remaining = m;
	memset(xi, 0, sizeof(xi));

	for (x = 0; (x <= GF_N(bch)) && remaining; x++) {
		y = gf_sqr(bch, x)^x;
		for (i = 0; i < 2; i++) {
			r = a_log(bch, y);
			if (y && (r < m) && !xi[r]) {
				bch->xi_tab[r] = x;
				xi[r] = 1;
				remaining--;
				dbg("x%d = %x\n", r, x);
				break;
			}
			y ^= ak;
		}
	}
	/* should not happen but check anyway */
	return remaining ? -1 : 0;
}

static void *bch_alloc(size_t size, int *err)
{
	void *ptr;

	ptr = malloc(size);
	if (ptr == NULL)
		*err = 1;
	return ptr;
}

/*
 * compute generator polynomial for given (m,t) parameters.
 */
static uint32_t *compute_generator_polynomial(struct bch_control *bch)
{
	const unsigned int m = GF_M(bch);
	const unsigned int t = GF_T(bch);
	int n, err = 0;
	unsigned int i, j, nbits, r, word, *roots;
	struct gf_poly *g;
	uint32_t *genpoly;

	g = bch_alloc(GF_POLY_SZ(m*t), &err);
	roots = bch_alloc((bch->n+1)*sizeof(*roots), &err);
	genpoly = bch_alloc(DIV_ROUND_UP(m*t+1, 32)*sizeof(*genpoly), &err);

	if (err) {
		kfree(genpoly);
		genpoly = NULL;
		goto finish;
	}

	/* enumerate all roots of g(X) */
	memset(roots , 0, (bch->n+1)*sizeof(*roots));
	for (i = 0; i < t; i++) {
		for (j = 0, r = 2*i+1; j < m; j++) {
			roots[r] = 1;
			r = mod_s(bch, 2*r);
		}
	}
	/* build generator polynomial g(X) */
	g->deg = 0;
	g->c[0] = 1;
	for (i = 0; i < GF_N(bch); i++) {
		if (roots[i]) {
			/* multiply g(X) by (X+root) */
			r = bch->a_pow_tab[i];
			g->c[g->deg+1] = 1;
			for (j = g->deg; j > 0; j--)
				g->c[j] = gf_mul(bch, g->c[j], r)^g->c[j-1];

			g->c[0] = gf_mul(bch, g->c[0], r);
			g->deg++;
		}
	}
	/* store left-justified binary representation of g(X) */
	n = g->deg+1;
	i = 0;

	while (n > 0) {
		nbits = (n > 32) ? 32 : n;
		for (j = 0, word = 0; j < nbits; j++) {
			if (g->c[n-1-j])
				word |= 1u << (31-j);
		}
		genpoly[i++] = word;
		n -= nbits;
	}
	bch->ecc_bits = g->deg;

finish:
	kfree(g);
	kfree(roots);

	return genpoly;
}

/**
 *  free_bch - free the BCH control structure
 *  @bch:    BCH control structure to release
 */
static void free_bch(struct bch_control *bch)
{
	unsigned int i;

	if (bch) {
		kfree(bch->a_pow_tab);
		kfree(bch->a_log_tab);
		kfree(bch->mod8_tab);
		kfree(bch->ecc_buf);
		kfree(bch->ecc_buf2);
		kfree(bch->xi_tab);
		kfree(bch->syn);
		kfree(bch->cache);
		kfree(bch->elp);

		for (i = 0; i < ARRAY_SIZE(bch->poly_2t); i++)
			kfree(bch->poly_2t[i]);

		kfree(bch);
	}
}

/**
 * init_bch - initialize a BCH encoder/decoder
 * @m:          Galois field order, should be in the range 5-15
 * @t:          maximum error correction capability, in bits
 * @prim_poly:  user-provided primitive polynomial (or 0 to use default)
 *
 * Returns:
 *  a newly allocated BCH control structure if successful, NULL otherwise
 *
 * This initialization can take some time, as lookup tables are built for fast
 * encoding/decoding; make sure not to call this function from a time critical
 * path. Usually, init_bch() should be called on module/driver init and
 * free_bch() should be called to release memory on exit.
 *
 * You may provide your own primitive polynomial of degree @m in argument
 * @prim_poly, or let init_bch() use its default polynomial.
 *
 * Once init_bch() has successfully returned a pointer to a newly allocated
 * BCH control structure, ecc length in bytes is given by member @ecc_bytes of
 * the structure.
 */
static struct bch_control *init_bch(int m, int t, unsigned int prim_poly)
{
	int err = 0;
	unsigned int i, words;
	uint32_t *genpoly;
	struct bch_control *bch = NULL;

	const int min_m = 5;
	const int max_m = 15;

	/* default primitive polynomials */
	static const unsigned int prim_poly_tab[] = {
		0x25, 0x43, 0x83, 0x11d, 0x211, 0x409, 0x805, 0x1053, 0x201b,
		0x402b, 0x8003,
	};

#if defined(CONFIG_BCH_CONST_PARAMS)
	if ((m != (CONFIG_BCH_CONST_M)) || (t != (CONFIG_BCH_CONST_T))) {
		printk(KERN_ERR "bch encoder/decoder was configured to support "
		       "parameters m=%d, t=%d only!\n",
		       CONFIG_BCH_CONST_M, CONFIG_BCH_CONST_T);
		goto fail;
	}
#endif
	if ((m < min_m) || (m > max_m))
		/*
		 * values of m greater than 15 are not currently supported;
		 * supporting m > 15 would require changing table base type
		 * (uint16_t) and a small patch in matrix transposition
		 */
		goto fail;

	/* sanity checks */
	if ((t < 1) || (m*t >= ((1 << m)-1)))
		/* invalid t value */
		goto fail;

	/* select a primitive polynomial for generating GF(2^m) */
	if (prim_poly == 0)
		prim_poly = prim_poly_tab[m-min_m];

	bch = malloc(sizeof(*bch));
	if (bch == NULL)
		goto fail;

	memset(bch, 0, sizeof(*bch));

	bch->m = m;
	bch->t = t;
	bch->n = (1 << m)-1;
	words  = DIV_ROUND_UP(m*t, 32);
	bch->ecc_bytes = DIV_ROUND_UP(m*t, 8);
	bch->a_pow_tab = bch_alloc((1+bch->n)*sizeof(*bch->a_pow_tab), &err);
	bch->a_log_tab = bch_alloc((1+bch->n)*sizeof(*bch->a_log_tab), &err);
	bch->mod8_tab  = bch_alloc(words*1024*sizeof(*bch->mod8_tab), &err);
	bch->ecc_buf   = bch_alloc(words*sizeof(*bch->ecc_buf), &err);
	bch->ecc_buf2  = bch_alloc(words*sizeof(*bch->ecc_buf2), &err);
	bch->xi_tab    = bch_alloc(m*sizeof(*bch->xi_tab), &err);
	bch->syn       = bch_alloc(2*t*sizeof(*bch->syn), &err);
	bch->cache     = bch_alloc(2*t*sizeof(*bch->cache), &err);
	bch->elp       = bch_alloc((t+1)*sizeof(struct gf_poly_deg1), &err);

	for (i = 0; i < ARRAY_SIZE(bch->poly_2t); i++)
		bch->poly_2t[i] = bch_alloc(GF_POLY_SZ(2*t), &err);

	if (err)
		goto fail;

	err = build_gf_tables(bch, prim_poly);
	if (err)
		goto fail;

	/* use generator polynomial for computing encoding tables */
	genpoly = compute_generator_polynomial(bch);
	if (genpoly == NULL)
		goto fail;

	build_mod8_tables(bch, genpoly);
	kfree(genpoly);

	err = build_deg2_base(bch);
	if (err)
		goto fail;

	return bch;

fail:
	free_bch(bch);
	return NULL;
}

static void swap_bits(uint8_t *buf, int len)
{
	int i, j;

	for (j = 0; j < len; j++) {
		uint8_t byte = buf[j];

		buf[j] = 0;
		for (i = 0; i < 8; i++) {
			if (byte & (1 << i))
				buf[j] |= (1 << (7 - i));
		}
	}
}

static uint16_t lfsr_step(uint16_t state, int count)
{
	state &= 0x7fff;
	while (count--)
		state = ((state >> 1) |
			 ((((state >> 0) ^ (state >> 1)) & 1) << 14)) & 0x7fff;

	return state;
}

static uint16_t default_scrambler_seeds[] = {
	0x2b75, 0x0bd0, 0x5ca3, 0x62d1, 0x1c93, 0x07e9, 0x2162, 0x3a72,
	0x0d67, 0x67f9, 0x1be7, 0x077d, 0x032f, 0x0dac, 0x2716, 0x2436,
	0x7922, 0x1510, 0x3860, 0x5287, 0x480f, 0x4252, 0x1789, 0x5a2d,
	0x2a49, 0x5e10, 0x437f, 0x4b4e, 0x2f45, 0x216e, 0x5cb7, 0x7130,
	0x2a3f, 0x60e4, 0x4dc9, 0x0ef0, 0x0f52, 0x1bb9, 0x6211, 0x7a56,
	0x226d, 0x4ea7, 0x6f36, 0x3692, 0x38bf, 0x0c62, 0x05eb, 0x4c55,
	0x60f4, 0x728c, 0x3b6f, 0x2037, 0x7f69, 0x0936, 0x651a, 0x4ceb,
	0x6218, 0x79f3, 0x383f, 0x18d9, 0x4f05, 0x5c82, 0x2912, 0x6f17,
	0x6856, 0x5938, 0x1007, 0x61ab, 0x3e7f, 0x57c2, 0x542f, 0x4f62,
	0x7454, 0x2eac, 0x7739, 0x42d4, 0x2f90, 0x435a, 0x2e52, 0x2064,
	0x637c, 0x66ad, 0x2c90, 0x0bad, 0x759c, 0x0029, 0x0986, 0x7126,
	0x1ca7, 0x1605, 0x386a, 0x27f5, 0x1380, 0x6d75, 0x24c3, 0x0f8e,
	0x2b7a, 0x1418, 0x1fd1, 0x7dc1, 0x2d8e, 0x43af, 0x2267, 0x7da3,
	0x4e3d, 0x1338, 0x50db, 0x454d, 0x764d, 0x40a3, 0x42e6, 0x262b,
	0x2d2e, 0x1aea, 0x2e17, 0x173d, 0x3a6e, 0x71bf, 0x25f9, 0x0a5d,
	0x7c57, 0x0fbe, 0x46ce, 0x4939, 0x6b17, 0x37bb, 0x3e91, 0x76db,
};

static uint16_t brom_scrambler_seeds[] = { 0x4a80 };

static void scramble(const struct image_info *info,
		     int page, uint8_t *data, int datalen)
{
	int seedidx = page % ARRAY_SIZE(default_scrambler_seeds);
	uint16_t state;
	int i;

	/* First pass: scramble data with the BROM seed */
	state = lfsr_step(brom_scrambler_seeds[0], 15);
	for (i = 0; i < datalen; i++) {
		data[i] ^= state;
		state = lfsr_step(state, 8);
	}

	if (info->disable_scrambler)
		return;

	/*
	 * Second pass: scramble data with the default seed to
	 * to revert the default randomization used by Linux.
	 */
	state = lfsr_step(default_scrambler_seeds[seedidx], 15);
	for (i = 0; i < datalen; i++) {
		data[i] ^= state;
		state = lfsr_step(state, 8);
	}
}

static int create_image(const struct image_info *info)
{
	struct bch_control *bch;
	uint8_t *data, *ecc, *buffer;
	int eccbytes = DIV_ROUND_UP(info->ecc_strength * 14, 8);
	int data_steps = info->usable_page_size / info->ecc_step_size;
	int usable_page_size = data_steps *
			       (info->ecc_step_size + 4 + eccbytes);
	int page = 0, page_offset = 0;
	FILE *src, *dst, *rnd;
	int repeat = info->repeat;
	int pages_per_block = info->block_size / info->page_size;
	size_t ret;

	if (!pages_per_block)
		pages_per_block = 1;

	bch = init_bch(14, info->ecc_strength, BCH_PRIMITIVE_POLY);
	if (!bch) {
		printf("Failed to init the BCH engine\n");
		return -1;
	}

	buffer = malloc(info->page_size + info->oob_size);
	if (!buffer) {
		printf("Failed to allocate the NAND page buffer\n");
		return -1;
	}

	memset(buffer, 0xff, info->page_size + info->oob_size);

	src = fopen(info->source, "r");
	if (!src) {
		printf("Failed to open source file (%s)\n", info->source);
		return -1;
	}

	dst = fopen(info->dest, "w");
	if (!dst) {
		printf("Failed to open dest file (%s)\n", info->dest);
		return -1;
	}

	rnd = fopen("/dev/urandom", "r");
	if (!rnd) {
		printf("Failed to open /dev/urandom\n");
		return -1;
	}

	if (usable_page_size < info->page_size + info->oob_size) {
		ret = fread(buffer + usable_page_size, 1,
			    info->page_size + info->oob_size -
			    usable_page_size,
			    rnd);
		if (ret != info->page_size + info->oob_size - usable_page_size)
			return -1;

		memset(buffer + info->page_size, 0xff, 2);
	}

	while (1) {
		size_t cnt, srcpadsize;

		cnt = info->ecc_step_size;

		data = buffer + page_offset;
		ecc = buffer + page_offset + info->ecc_step_size + 4;
		cnt = fread(data, 1, info->ecc_step_size, src);
		if (!cnt && !feof(src)) {
			printf("Failed to read data from the source\n");
			return -1;
		}

		/*
		 * If page_offset != 0 continue (fill the rest of the page with
		 * random data) to make sure the entire page has valid ecc data
		 */
		if (page_offset == 0 && cnt == 0) {
			if (!repeat--)
				break;

			while (page % 0x40) {
				ret = fread(buffer, 1,
					    info->page_size + info->oob_size,
					    rnd);
				if (ret != info->page_size + info->oob_size)
					return -1;

				memset(buffer + info->page_size, 0xff, 2);
				fwrite(buffer,
				       info->page_size + info->oob_size,
				       1, dst);
				page++;
			}

			page_offset = 0;
			fseek(src, 0, SEEK_SET);
			continue;
		}

		srcpadsize = info->ecc_step_size - cnt;
		if (srcpadsize) {
			ret = fread(data + cnt, 1, srcpadsize, rnd);
			if (ret != srcpadsize)
				return -1;
		}

		memset(ecc, 0, eccbytes);
		swap_bits(data, info->ecc_step_size + 4);
		encode_bch(bch, data, info->ecc_step_size + 4, ecc);
		swap_bits(data, info->ecc_step_size + 4);
		swap_bits(ecc, eccbytes);
		scramble(info, page, data, info->ecc_step_size + 4 + eccbytes);

		page_offset += info->ecc_step_size + 4 + eccbytes;
		if (page_offset >= usable_page_size) {
			fwrite(buffer, info->page_size + info->oob_size, 1, dst);
			memset(buffer, 0xff, info->page_size + info->oob_size);
			page++;
			page_offset = 0;
			if (usable_page_size < info->page_size + info->oob_size) {
				ret = fread(buffer + usable_page_size, 1,
					    info->page_size + info->oob_size -
					    usable_page_size,
					    rnd);
				if (ret != info->page_size + info->oob_size -
					   usable_page_size)
					return -1;
				memset(buffer + info->page_size, 0xff, 2);
			}
		}
	}

	while (page % pages_per_block) {
		ret = fread(buffer, 1,
			    info->page_size + info->oob_size, rnd);
		if (ret != info->page_size + info->oob_size)
			return -1;

		memset(buffer + info->page_size, 0xff, 2);
		fwrite(buffer,
		       info->page_size + info->oob_size,
		       1, dst);
		page++;
	}

	return 0;
}

static void display_help(int status)
{
	fprintf(status == EXIT_SUCCESS ? stdout : stderr,
"Usage: spl-image-builder [OPTIONS] source-image output-image\n"
"Creates an image fitting the BROM requirements (randomizer and ECC config).\n"
"\n"
"-h          --help                  Display this help and exit\n"
"-s strength --ecc-strength=size     ECC strength\n"
"-c size     --ecc-step-size=size    ECC step size\n"
"-p size     --page-size=size        Page size\n"
"-o size     --oob-size=size         OOB size\n"
"-b size     --block-size=size       Block size\n"
"-u size     --usable-page-size=size Usable page size\n"
"-r num      --repeat=num            Number of redundant SPL images\n"
"-d          --disable-scrambler     Disable the second scrambling pass\n"
"\n");
	exit(status);
}

int main(int argc, char **argv)
{
	struct image_info info;

	memset(&info, 0, sizeof(info));
	/*
	 * Process user arguments
	 */
	for (;;) {
		int option_index = 0;
		static const struct option long_options[] = {
			{"help", no_argument, 0, 0},
			{"ecc-strength", required_argument, 0, 's'},
			{"ecc-step-size", required_argument, 0, 'c'},
			{"page-size", required_argument, 0, 'p'},
			{"oob-size", required_argument, 0, 'o'},
			{"block-size", required_argument, 0, 'b'},
			{"usable-page-size", required_argument, 0, 'u'},
			{"repeat", required_argument, 0, 'r'},
			{"disable-scrambler", no_argument, 0, 'd'},
			{0, 0, 0, 0},
		};

		int c = getopt_long(argc, argv, "s:c:p:o:b:u:r:d",
				long_options, &option_index);
		if (c == EOF)
			break;

		switch (c) {
		case 'h':
			display_help(0);
			break;
		case 's':
			info.ecc_strength = strtol(optarg, NULL, 0);
			break;
		case 'c':
			info.ecc_step_size = strtol(optarg, NULL, 0);
			break;
		case 'p':
			info.page_size = strtol(optarg, NULL, 0);
			break;
		case 'o':
			info.oob_size = strtol(optarg, NULL, 0);
			break;
		case 'b':
			info.block_size = strtol(optarg, NULL, 0);
			break;
		case 'u':
			info.usable_page_size = strtol(optarg, NULL, 0);
			break;
		case 'r':
			info.repeat = strtol(optarg, NULL, 0);
			if (info.repeat > 7)
				info.repeat = 7;
			break;
		case 'd':
			info.disable_scrambler = 1;
			break;
		case '?':
			display_help(-1);
			break;
		}
	}

	if ((argc - optind) != 2)
		display_help(-1);

	info.source = argv[optind];
	info.dest = argv[optind + 1];

	return create_image(&info);
}
