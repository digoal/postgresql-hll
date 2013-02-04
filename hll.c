/* Copyright 2013 Aggregate Knowledge, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <postgres.h>	// Needs to be first.

#include <byteswap.h>
#include <funcapi.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "utils/array.h"
#include "utils/bytea.h"
#include "utils/memutils.h"
#include "catalog/pg_type.h"

#include "MurmurHash3.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

// ----------------------------------------------------------------
// Output Version Control
// ----------------------------------------------------------------

// Set the default output schema.
static uint8_t g_output_version = 1;

// ----------------------------------------------------------------
// Type Modifiers
// ----------------------------------------------------------------

// The type modifiers need to be packed in the lower 31 bits
// of an int32.  We currently use the lowest 15 bits.
//
#define LOG2M_BITS		5
#define REGWIDTH_BITS 	3
#define EXPTHRESH_BITS	6
#define SPARSEON_BITS	1
#define TYPMOD_BITS		15

#define MAX_BITVAL(nbits)	((1 << nbits) - 1)

// Defaults if type modifier values are not specified.
//
#define DEFAULT_LOG2M		11
#define DEFAULT_REGWIDTH	5
#define DEFAULT_EXPTHRESH	-1
#define DEFAULT_SPARSEON	1

static int32 g_default_log2m = DEFAULT_LOG2M;
static int32 g_default_regwidth = DEFAULT_REGWIDTH;
static int64 g_default_expthresh = DEFAULT_EXPTHRESH;
static int32 g_default_sparseon = DEFAULT_SPARSEON;

enum {
    MST_UNDEFINED	= 0x0,		// Invalid/undefined set.
    MST_EMPTY		= 0x1,		// Empty set.
    MST_EXPLICIT	= 0x2,		// List of explicit ids.
    MST_SPARSE		= 0x3,		// Sparse set of compression registers.
    MST_COMPRESSED	= 0x4,		// Array of compression registers.

    MST_UNINIT		= 0xffff,	// Internal uninitialized.
};

static int32 typmod_log2m(int32 typmod)
{
    return (typmod >> (TYPMOD_BITS - LOG2M_BITS))
        & MAX_BITVAL(LOG2M_BITS);
}

static int32 typmod_regwidth(int32 typmod)
{
    return (typmod >> (TYPMOD_BITS - LOG2M_BITS - REGWIDTH_BITS))
        & MAX_BITVAL(REGWIDTH_BITS);
}

static int32 typmod_expthresh(int32 typmod)
{
    return (typmod >> (TYPMOD_BITS - LOG2M_BITS -
                       REGWIDTH_BITS - EXPTHRESH_BITS))
        & MAX_BITVAL(EXPTHRESH_BITS);
}

static int32 typmod_sparseon(int32 typmod)
{
    return (typmod >> (TYPMOD_BITS - LOG2M_BITS -
                       REGWIDTH_BITS - EXPTHRESH_BITS - SPARSEON_BITS))
        & MAX_BITVAL(SPARSEON_BITS);
}

// The expthresh is represented in a encoded format in the
// type modifier to save metadata bits.  This routine is used
// when the expthresh comes from a typmod value or hll header.
//
static int64 decode_expthresh(int32 encoded_expthresh)
{
    // This routine presumes the encoded value is correct and
    // doesn't range check.
    //
    if (encoded_expthresh == 63)
        return -1LL;
    else if (encoded_expthresh == 0)
        return 0;
    else
        return 1LL << (encoded_expthresh - 1);
}

static int32 integer_log2(int64 val)
{
    // Take the log2 of the expthresh.
    int32 count = 0;
    int64 value = val;

    Assert(val >= 0);

    while (value)
    {
        ++count;
        value >>= 1;
    }
    return count - 1;
}

// This routine is used to encode an expthresh value to be stored
// in the typmod metadata or a hll header.
//
static int32 encode_expthresh(int64 expthresh)
{
    // This routine presumes the uncompressed value is correct and
    // doesn't range check.
    //
    if (expthresh == -1)
        return 63;
    else if (expthresh == 0)
        return 0;
    else
        return integer_log2(expthresh) + 1;
}

// If expthresh == -1 (auto select expthresh) determine
// the expthresh to use from nbits and nregs.
//
static size_t
expthresh_value(int64 expthresh, size_t nbits, size_t nregs)
{
    if (expthresh != -1)
    {
        return (size_t) expthresh;
    }
    else
    {
        // Auto is selected, choose the maximum number of explicit
        // registers that fits in the same space as the compressed
        // encoding.
        size_t cmpsz = ((nbits * nregs) + 7) / 8;
        return cmpsz / sizeof(uint64_t);
    }
}

// ----------------------------------------------------------------
// Maximum Sparse Control
// ----------------------------------------------------------------

// By default we set the sparse to full compressed threshold
// automatically to the point where the sparse representation would
// start to be larger.  This can be overridden with the
// hll_set_max_sparse directive ...
//
static int g_max_sparse = -1;


// ----------------------------------------------------------------
// Aggregating Data Structure
// ----------------------------------------------------------------

typedef struct
{
    size_t		mse_nelem;
    uint64_t	mse_elems[0];

} ms_explicit_t;

// Defines the *unpacked* register.
typedef uint8_t compreg_t;

typedef struct
{
    compreg_t	msc_regs[0];

} ms_compressed_t;

// Size of the compressed or explicit data.
#define MS_MAXDATA		(128 * 1024)

typedef struct
{
    size_t		ms_nbits;
    size_t		ms_nregs;
    size_t		ms_log2nregs;
    int64		ms_expthresh;
    bool		ms_sparseon;

	uint64_t	ms_type;	// size is only for alignment.

    union
    {
        // MST_EMPTY and MST_UNDEFINED don't need data.
        // MST_SPARSE is only used in the packed format.
        //
        ms_explicit_t	as_expl;	// MST_EXPLICIT
        ms_compressed_t	as_comp;	// MST_COMPRESSED
        uint8_t			as_size[MS_MAXDATA];	// sizes the union.

    }		ms_data;

} multiset_t;

typedef struct
{
    size_t			brc_nbits;	// Read size.
    uint32_t		brc_mask;	// Read mask.
    uint8_t const *	brc_curp;	// Current byte.
    size_t			brc_used;	// Used bits.

} bitstream_read_cursor_t;

static uint32_t
bitstream_unpack(bitstream_read_cursor_t * brcp)
{
    uint32_t retval;

    // Fetch the quadword containing our data.
    uint64_t qw = * (uint64_t const *) brcp->brc_curp;

    // Swap the bytes.
    qw = bswap_64(qw);

    // Shift the bits we want into place.
    qw >>= 64 - brcp->brc_nbits - brcp->brc_used;

    // Mask the bits we want.
    retval = (uint32_t) (qw & 0xffffffff) & brcp->brc_mask;

    // We've used some more bits now.
    brcp->brc_used += brcp->brc_nbits;

    // Normalize the cursor.
    while (brcp->brc_used >= 8)
    {
        brcp->brc_used -= 8;
        brcp->brc_curp += 1;
    }

    return retval;
}

static void
compressed_unpack(compreg_t * i_regp,
                  size_t i_width,
                  size_t i_nregs,
                  uint8_t const * i_bitp,
                  size_t i_size,
                  uint8_t i_vers)
{
    size_t bitsz;
    size_t padsz;

    bitstream_read_cursor_t brc;

    bitsz = i_width * i_nregs;

    // Fail fast if the compressed array isn't big enough.
    if (i_size * 8 < bitsz)
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("compressed hll argument not large enough")));

    padsz = i_size * 8 - bitsz;

    // Fail fast if the pad size doesn't make sense.
    if (padsz >= 8)
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("inconsistent padding in compressed hll argument")));

    brc.brc_nbits = i_width;
    brc.brc_mask = (1 << i_width) - 1;
    brc.brc_curp = i_bitp;
    brc.brc_used = 0;

    for (ssize_t ndx = 0; ndx < i_nregs; ++ndx)
    {
        uint32_t val = bitstream_unpack(&brc);
        i_regp[ndx] = val;
    }
}

static void
sparse_unpack(compreg_t * i_regp,
              size_t i_width,
              size_t i_log2nregs,
              size_t i_nfilled,
              uint8_t const * i_bitp,
              size_t i_size)
{
    size_t bitsz;
    size_t padsz;
    size_t chunksz;
    uint32_t regmask;

    bitstream_read_cursor_t brc;

    chunksz = i_log2nregs + i_width;
    bitsz = chunksz * i_nfilled;
    padsz = i_size * 8 - bitsz;

    // Fail fast if the pad size doesn't make sense.
    if (padsz >= 8)
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("inconsistent padding in sparse hll argument")));

    regmask = (1 << i_width) - 1;

    brc.brc_nbits = chunksz;
    brc.brc_mask = (1 << chunksz) - 1;
    brc.brc_curp = i_bitp;
    brc.brc_used = 0;

    for (ssize_t ii = 0; ii < i_nfilled; ++ii)
    {
        uint32_t buffer = bitstream_unpack(&brc);
        uint32_t val = buffer & regmask;
        uint32_t ndx = buffer >> i_width;
        i_regp[ndx] = val;
    }
}

typedef struct
{
    size_t			bwc_nbits;	// Write size.
    uint8_t *		bwc_curp;	// Current byte.
    size_t			bwc_used;	// Used bits.

} bitstream_write_cursor_t;

static void
bitstream_pack(bitstream_write_cursor_t * bwcp, uint32_t val)
{
    // Fetch the quadword where our data goes.
    uint64_t qw = * (uint64_t *) bwcp->bwc_curp;

    // Swap the bytes.
    qw = bswap_64(qw);

    // Shift our bits into place and combine.
    qw |= ((uint64_t) val << (64 - bwcp->bwc_nbits - bwcp->bwc_used));

    // Swap the bytes back again.
    qw = bswap_64(qw);

    // Write the word back out.
    * (uint64_t *) bwcp->bwc_curp = qw;

    // We've used some more bits now.
    bwcp->bwc_used += bwcp->bwc_nbits;

    // Normalize the cursor.
    while (bwcp->bwc_used >= 8)
    {
        bwcp->bwc_used -= 8;
        bwcp->bwc_curp += 1;
    }

}

static void
compressed_pack(compreg_t const * i_regp,
                size_t i_width,
                size_t i_nregs,
                uint8_t * o_bitp,
                size_t i_size,
                uint8_t i_vers)
{
    size_t bitsz;
    size_t padsz;

    bitstream_write_cursor_t bwc;

    // We need to zero the output array because we use
    // an bitwise-or-accumulator below.
    memset(o_bitp, '\0', i_size);

    bitsz = i_width * i_nregs;

    // Fail fast if the compressed array isn't big enough.
    if (i_size * 8 < bitsz)
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("compressed output buffer not large enough")));

    padsz = i_size * 8 - bitsz;

    // Fail fast if the pad size doesn't make sense.
    if (padsz >= 8)
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("inconsistent compressed output pad size")));

    bwc.bwc_nbits = i_width;
    bwc.bwc_curp = o_bitp;
    bwc.bwc_used = 0;

    for (ssize_t ndx = 0; ndx < i_nregs; ++ndx)
        bitstream_pack(&bwc, i_regp[ndx]);
}

static void
sparse_pack(compreg_t const * i_regp,
            size_t i_width,
            size_t i_nregs,
            size_t i_log2nregs,
            size_t i_nfilled,
            uint8_t * o_bitp,
            size_t i_size)
{
    size_t bitsz;
    size_t padsz;

    bitstream_write_cursor_t bwc;

    // We need to zero the output array because we use
    // an bitwise-or-accumulator below.
    memset(o_bitp, '\0', i_size);

    bitsz = i_nfilled * (i_log2nregs + i_width);

    // Fail fast if the compressed array isn't big enough.
    if (i_size * 8 < bitsz)
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("sparse output buffer not large enough")));

    padsz = i_size * 8 - bitsz;

    // Fail fast if the pad size doesn't make sense.
    if (padsz >= 8)
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("inconsistent sparse output pad size")));

    bwc.bwc_nbits = i_log2nregs + i_width;
    bwc.bwc_curp = o_bitp;
    bwc.bwc_used = 0;

    for (ssize_t ndx = 0; ndx < i_nregs; ++ndx)
    {
        if (i_regp[ndx] != 0)
        {
            uint32_t buffer = (ndx << i_width) | i_regp[ndx];
            bitstream_pack(&bwc, buffer);
        }
    }
}

static void
check_metadata(multiset_t const * i_omp, multiset_t const * i_imp)
{
    if (i_omp->ms_nbits != i_imp->ms_nbits)
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("register width does not match: "
                        "source uses %ld and dest uses %ld",
                        i_imp->ms_nbits, i_omp->ms_nbits)));
    }

    if (i_omp->ms_nregs != i_imp->ms_nregs)
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("register count does not match: "
                        "source uses %ld and dest uses %ld",
                        i_imp->ms_nregs, i_omp->ms_nregs)));
    }

    // Don't need to compare log2nregs because we compared nregs ...

    if (i_omp->ms_expthresh != i_imp->ms_expthresh)
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("explicit threshold does not match: "
                        "source uses %ld and dest uses %ld",
                        i_imp->ms_expthresh, i_omp->ms_expthresh)));
    }

    if (i_omp->ms_sparseon != i_imp->ms_sparseon)
    {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("sparse enable does not match: "
                        "source uses %d and dest uses %d",
                        i_imp->ms_sparseon, i_omp->ms_sparseon)));
    }
}

static void
copy_metadata(multiset_t * o_msp, multiset_t const * i_msp)
{
    o_msp->ms_nbits = i_msp->ms_nbits;
    o_msp->ms_nregs = i_msp->ms_nregs;
    o_msp->ms_log2nregs = i_msp->ms_log2nregs;
    o_msp->ms_expthresh = i_msp->ms_expthresh;
    o_msp->ms_sparseon = i_msp->ms_sparseon;
}

static void
compressed_add(multiset_t * o_msp, uint64_t elem)
{
    size_t nbits = o_msp->ms_nbits;
    size_t nregs = o_msp->ms_nregs;
    size_t log2nregs = o_msp->ms_log2nregs;

    ms_compressed_t * mscp = &o_msp->ms_data.as_comp;

    uint64_t mask = nregs - 1;

    size_t maxregval = (1 << nbits) - 1;

    size_t ndx = elem & mask;

    uint64_t ss_val = elem >> log2nregs;

    size_t p_w = ss_val == 0 ? 0 : __builtin_ctzll(ss_val) + 1;

    if (p_w > maxregval)
        p_w = maxregval;

    if (mscp->msc_regs[ndx] < p_w)
        mscp->msc_regs[ndx] = p_w;
}

static void
compressed_explicit_union(multiset_t * o_msp, multiset_t const * i_msp)
{
    ms_explicit_t const * msep = &i_msp->ms_data.as_expl;
    for (size_t ii = 0; ii < msep->mse_nelem; ++ii)
        compressed_add(o_msp, msep->mse_elems[ii]);
}

static void
explicit_to_compressed(multiset_t * msp)
{
    // Make a copy of the explicit multiset.
    multiset_t ms;
    memcpy(&ms, msp, sizeof(ms));

    // Clear the multiset.
    memset(msp, '\0', sizeof(*msp));

    // Restore the metadata.
    copy_metadata(msp, &ms);

    // Make it MST_COMPRESSED.
    msp->ms_type = MST_COMPRESSED;

    // Add all the elements back into the compressed multiset.
    compressed_explicit_union(msp, &ms);
}

static int
element_compare(void const * ptr1, void const * ptr2)
{
    // We used signed integer comparison to be compatible
    // with the java code.

    int64_t v1 = * (int64_t const *) ptr1;
    int64_t v2 = * (int64_t const *) ptr2;

    return (v1 < v2) ? -1 : (v1 > v2) ? 1 : 0;
}

static size_t numfilled(multiset_t const * i_msp)
{
    ms_compressed_t const * mscp = &i_msp->ms_data.as_comp;
    size_t nfilled = 0;
    size_t nregs = i_msp->ms_nregs;
    for (size_t ii = 0; ii < nregs; ++ii)
        if (mscp->msc_regs[ii] > 0)
            ++nfilled;

    return nfilled;
}

static char *
multiset_tostring(multiset_t const * i_msp)
{
    char expbuf[256];
    char * retstr;
    size_t len;
    size_t used;
    size_t nbits = i_msp->ms_nbits;
    size_t nregs = i_msp->ms_nregs;
    int64 expthresh = i_msp->ms_expthresh;
    size_t sparseon = i_msp->ms_sparseon;

    size_t expval = expthresh_value(expthresh, nbits, nregs);

    // If the expthresh is set to -1 (auto) augment the value
    // with the automatically determined value.
    //
    if (expthresh == -1)
        snprintf(expbuf, sizeof(expbuf), "%ld(%ld)", expthresh, expval);
    else
        snprintf(expbuf, sizeof(expbuf), "%ld", expthresh);

    // Allocate an initial return buffer.
    len = 1024;
    retstr = (char *) palloc(len);
    memset(retstr, '\0', len);

    // We haven't used any return buffer yet.
    used = 0;

    // Print in a type-dependent way.
    switch (i_msp->ms_type)
    {
    case MST_EMPTY:
        used += snprintf(retstr, len, "EMPTY, "
                         "nregs=%ld, nbits=%ld, expthresh=%s, sparseon=%ld",
                         nregs, nbits, expbuf, sparseon);
        break;
    case MST_EXPLICIT:
        {
            ms_explicit_t const * msep = &i_msp->ms_data.as_expl;
            size_t size = msep->mse_nelem;
            char linebuf[1024];
            ssize_t rv;

            used += snprintf(retstr, len, "EXPLICIT, %ld elements, "
                             "nregs=%ld, nbits=%ld, "
                             "expthresh=%s, sparseon=%ld:",
                             size, nregs, nbits, expbuf, sparseon);
            for (size_t ii = 0; ii < size; ++ii)
            {
                int64_t val = * (int64_t const *) & msep->mse_elems[ii];
                rv = snprintf(linebuf, sizeof(linebuf),
                              "\n%ld: %20" PRIi64 " ",
                              ii, val);
                // Do we need to reallocate the return buffer?
                if (rv + used > len - 1)
                {
                    len += 1024;
                    retstr = (char *) repalloc(retstr, len);
                }
                strncpy(&retstr[used], linebuf, len - used);
                used += rv;
            }
        }
        break;
    case MST_COMPRESSED:
        {
            ms_compressed_t const * mscp = &i_msp->ms_data.as_comp;

            char linebuf[1024];

            size_t rowsz = 32;
            size_t nrows = nregs / rowsz;
            size_t ndx = 0;

            used += snprintf(retstr, len,
                             "COMPRESSED, %ld filled "
                             "nregs=%ld, nbits=%ld, expthresh=%s, "
                             "sparseon=%ld:",
                             numfilled(i_msp),
                             nregs, nbits, expbuf, sparseon);

            for (size_t rr = 0; rr < nrows; ++rr)
            {
                size_t pos = 0;
                pos = snprintf(linebuf, sizeof(linebuf), "\n%4ld: ", ndx);
                for (size_t cc = 0; cc < rowsz; ++cc)
                {
                    pos += snprintf(&linebuf[pos], sizeof(linebuf) - pos,
                                    "%2d ", mscp->msc_regs[ndx]);
                    ++ndx;
                }

                // Do we need to reallocate the return buffer?
                if (pos + used > len - 1)
                {
                    len += 1024;
                    retstr = (char *) repalloc(retstr, len);
                }
                strncpy(&retstr[used], linebuf, len - used);
                used += pos;
            }
        }
        break;
    case MST_UNDEFINED:
        used += snprintf(retstr, len, "UNDEFINED "
                         "nregs=%ld, nbits=%ld, expthresh=%s, sparseon=%ld",
                         nregs, nbits, expbuf, sparseon);
        break;
    default:
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("unexpected multiset type value")));
        break;
    }

    return retstr;
}

static void
explicit_validate(multiset_t const * i_msp, ms_explicit_t const * i_msep)
{
    // Allow explicit multisets with no elements.
    if (i_msep->mse_nelem == 0)
        return;

    // Confirm that all elements are ascending with no duplicates.
    for (int ii = 0; ii < i_msep->mse_nelem - 1; ++ii)
    {
        if (element_compare(&i_msep->mse_elems[ii],
                            &i_msep->mse_elems[ii + 1]) != -1)
        {
            char * buf = multiset_tostring(i_msp);

            ereport(ERROR,
                    (errcode(ERRCODE_DATA_EXCEPTION),
                     errmsg("duplicate or descending explicit elements: %s",
                            buf)));

            pfree(buf);
        }
    }
}

static void
multiset_add(multiset_t * o_msp, uint64_t element)
{
    // WARNING!  This routine can change the type of the multiset!

    size_t expval = expthresh_value(o_msp->ms_expthresh,
                                    o_msp->ms_nbits,
                                    o_msp->ms_nregs);

    switch (o_msp->ms_type)
    {
    case MST_EMPTY:
        // Are we forcing compressed?
        if (expval == 0)
        {
            // Now we're explicit with no elements.
            o_msp->ms_type = MST_EXPLICIT;
            o_msp->ms_data.as_expl.mse_nelem = 0;

            // Convert it to compressed.
            explicit_to_compressed(o_msp);

            // Add the element in compressed format.
            compressed_add(o_msp, element);
        }
        else
        {
            // Now we're explicit with one element.
            o_msp->ms_type = MST_EXPLICIT;
            o_msp->ms_data.as_expl.mse_nelem = 1;
            o_msp->ms_data.as_expl.mse_elems[0] = element;
        }
        break;

    case MST_EXPLICIT:
        {
            ms_explicit_t * msep = &o_msp->ms_data.as_expl;

            // If the element is already in the set we're done.
            if (bsearch(&element,
                        msep->mse_elems,
                        msep->mse_nelem,
                        sizeof(uint64_t),
                        element_compare))
                return;

            // Is the explicit multiset full?
            if (msep->mse_nelem == expval)
            {
                // Convert it to compressed.
                explicit_to_compressed(o_msp);

                // Add the element in compressed format.
                compressed_add(o_msp, element);
            }
            else
            {
                // Add the element at the end.
                msep->mse_elems[msep->mse_nelem++] = element;

                // Resort the elements.
                qsort(msep->mse_elems,
                      msep->mse_nelem,
                      sizeof(uint64_t),
                      element_compare);
            }
        }
        break;

    case MST_COMPRESSED:
        compressed_add(o_msp, element);
        break;

    case MST_UNDEFINED:
        // Result is unchanged.
        break;

    default:
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("undefined multiset type value #1")));
        break;
    }
}

static void
explicit_union(multiset_t * o_msp, ms_explicit_t const * i_msep)
{
    // NOTE - This routine is optimized to add a batch of elements;
    // it doesn't resort until they are all added ...
    //
    // WARNING!  This routine can change the type of the target multiset!

    size_t expval = expthresh_value(o_msp->ms_expthresh,
                                    o_msp->ms_nbits,
                                    o_msp->ms_nregs);

    ms_explicit_t * msep = &o_msp->ms_data.as_expl;

    // Note the starting size of the target set.
    size_t orig_nelem = msep->mse_nelem;

    for (size_t ii = 0; ii < i_msep->mse_nelem; ++ii)
    {
        uint64_t element = i_msep->mse_elems[ii];

        switch (o_msp->ms_type)
        {
        case MST_EXPLICIT:
            if (bsearch(&element,
                        msep->mse_elems,
                        orig_nelem,
                        sizeof(uint64_t),
                        element_compare))
                continue;

            if (msep->mse_nelem < expval)
            {
                // Add the element at the end.
                msep->mse_elems[msep->mse_nelem++] = element;
            }
            else
            {
                // Convert it to compressed.
                explicit_to_compressed(o_msp);

                // Add the element in compressed format.
                compressed_add(o_msp, element);
            }
            break;

        case MST_COMPRESSED:
            compressed_add(o_msp, element);
            break;
        }
    }

    // If the target multiset is still explicit it needs to be
    // resorted.
    if (o_msp->ms_type == MST_EXPLICIT)
    {
        // Resort the elements.
        qsort(msep->mse_elems,
              msep->mse_nelem,
              sizeof(uint64_t),
              element_compare);
    }
}

static void unpack_header(multiset_t * o_msp,
                          uint8_t const * i_bitp,
                          uint8_t vers,
                          uint8_t type)
{
    o_msp->ms_nbits = (i_bitp[1] >> 5) + 1;
    o_msp->ms_log2nregs = i_bitp[1] & 0x1f;
    o_msp->ms_nregs = 1 <<  o_msp->ms_log2nregs;
    o_msp->ms_expthresh = decode_expthresh(i_bitp[2] & 0x3f);
    o_msp->ms_sparseon = (i_bitp[2] >> 6) & 0x1;
}

static uint8_t
multiset_unpack(multiset_t * o_msp,
                uint8_t const * i_bitp,
                size_t i_size,
                uint8_t * o_encoded_type)
{
    // First byte is the version and type header.
    uint8_t vers = (i_bitp[0] >> 4) & 0xf;
    uint8_t type = i_bitp[0] & 0xf;

    if (vers != 1)
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("unknown schema version %d", (int) vers)));

    if (o_encoded_type != NULL)
        *o_encoded_type = type;

    // Set the type. NOTE - MST_SPARSE are converted to MST_COMPRESSED.
    o_msp->ms_type = (type == MST_SPARSE) ? MST_COMPRESSED : type;

    switch (type)
    {
    case MST_EMPTY:
        if (vers == 1)
        {
            size_t hdrsz = 3;

            // Make sure the size is consistent.
            if (i_size != hdrsz)
            {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_EXCEPTION),
                         errmsg("inconsistently sized empty multiset")));
            }

            unpack_header(o_msp, i_bitp, vers, type);
        }
        else
        {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_EXCEPTION),
                     errmsg("unsupported empty version %d",
                            (int) vers)));
        }

        break;

    case MST_EXPLICIT:
        if (vers == 1)
        {
            ms_explicit_t * msep = &o_msp->ms_data.as_expl;
            size_t hdrsz = 3;
            size_t nelem = (i_size - hdrsz) / 8;
            size_t ndx = hdrsz;

            // Make sure the size is consistent.
            if (((i_size - hdrsz) % 8) != 0)
            {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_EXCEPTION),
                         errmsg("inconsistently sized explicit multiset")));
            }

            // Make sure the explicit array fits in memory.
            if ((i_size - hdrsz) > MS_MAXDATA)
            {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_EXCEPTION),
                         errmsg("explicit multiset too large")));
            }

            unpack_header(o_msp, i_bitp, vers, type);

            msep->mse_nelem = nelem;
            for (size_t ii = 0; ii < nelem; ++ii)
            {
                uint64_t val = 0;
                val |= ((uint64_t) i_bitp[ndx++] << 56);
                val |= ((uint64_t) i_bitp[ndx++] << 48);
                val |= ((uint64_t) i_bitp[ndx++] << 40);
                val |= ((uint64_t) i_bitp[ndx++] << 32);
                val |= ((uint64_t) i_bitp[ndx++] << 24);
                val |= ((uint64_t) i_bitp[ndx++] << 16);
                val |= ((uint64_t) i_bitp[ndx++] <<  8);
                val |= ((uint64_t) i_bitp[ndx++] <<  0);
                msep->mse_elems[ii] = val;
            }

            explicit_validate(o_msp, msep);
        }
        else
        {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_EXCEPTION),
                     errmsg("unsupported explicit version %d",
                            (int) vers)));
        }
        break;

    case MST_COMPRESSED:
        if (vers == 1)
        {
            size_t hdrsz = 3;

            // Decode the parameter byte.
            uint8_t param = i_bitp[1];
            size_t nbits = (param >> 5) + 1;
            size_t log2nregs = param & 0x1f;
            size_t nregs = 1 << log2nregs;

            // Make sure the size is consistent.
            size_t bitsz = nbits * nregs;
            size_t packedbytesz = (bitsz + 7) / 8;
            if ((i_size - hdrsz) != packedbytesz)
            {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_EXCEPTION),
                         errmsg("inconsistently sized "
                                "compressed multiset")));
            }

            // Make sure the compressed array fits in memory.
            if (nregs * sizeof(compreg_t) > MS_MAXDATA)
            {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_EXCEPTION),
                         errmsg("compressed multiset too large")));
            }

            unpack_header(o_msp, i_bitp, vers, type);

            // Fill the registers.
            compressed_unpack(o_msp->ms_data.as_comp.msc_regs,
                              nbits, nregs, &i_bitp[hdrsz], i_size - hdrsz,
                              vers);
        }
        else
        {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_EXCEPTION),
                     errmsg("unsupported compressed version %d",
                            (int) vers)));
        }
        break;

    case MST_UNDEFINED:
        if (vers == 1)
        {
            size_t hdrsz = 3;

            // Make sure the size is consistent.
            if (i_size != hdrsz)
            {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_EXCEPTION),
                         errmsg("undefined multiset value")));
            }

            unpack_header(o_msp, i_bitp, vers, type);
        }
        else
        {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_EXCEPTION),
                     errmsg("unsupported undefined version %d",
                            (int) vers)));
        }

        break;

    case MST_SPARSE:
        if (vers == 1)
        {
            size_t hdrsz = 3;

            ms_compressed_t * mscp;

            if (i_size < hdrsz)
            {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_EXCEPTION),
                         errmsg("sparse multiset too small")));
            }
            else
            {
                // Decode the parameter byte.
                uint8_t param = i_bitp[1];
                size_t nbits = (param >> 5) + 1;
                size_t log2nregs = param & 0x1f;
                size_t nregs = 1 << log2nregs;

                // Figure out how many encoded registers are in the
                // bitstream.  We depend on the log2nregs + nbits being
                // greater then the pad size so we aren't left with
                // ambiguity in the final pad byte.

                size_t bitsz = (i_size - hdrsz) * 8;
                size_t chunksz = log2nregs + nbits;
                size_t nfilled = bitsz / chunksz;

                // Make sure the compressed array fits in memory.
                if (nregs * sizeof(compreg_t) > MS_MAXDATA)
                {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_EXCEPTION),
                             errmsg("sparse multiset too large")));
                }

                unpack_header(o_msp, i_bitp, vers, type);

                mscp = &o_msp->ms_data.as_comp;

                // Pre-zero the registers since sparse only fills
                // in occasional ones.
                //
                for (int ii = 0; ii < nregs; ++ii)
                    mscp->msc_regs[ii] = 0;

                // Fill the registers.
                sparse_unpack(mscp->msc_regs,
                              nbits, log2nregs, nfilled,
                              &i_bitp[hdrsz], i_size - hdrsz);
            }
        }
        else
        {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_EXCEPTION),
                     errmsg("unsupported sparse version %d",
                            (int) vers)));
        }
        break;

    default:
        // This is always an error.
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("undefined multiset type")));
        break;
    }

    return vers;
}

static size_t
pack_header(uint8_t * o_bitp,
            uint8_t vers,
            uint8_t type,
            size_t nbits,
            size_t log2nregs,
            int64 expthresh,
            size_t sparseon)
{
    size_t ndx = 0;

    o_bitp[ndx++] = (vers << 4) | type;
    o_bitp[ndx++] = ((nbits - 1) << 5) | log2nregs;
    o_bitp[ndx++] = (sparseon << 6) | encode_expthresh(expthresh);

    return ndx;
}

static void
multiset_pack(multiset_t const * i_msp, uint8_t * o_bitp, size_t i_size)
{
    uint8_t vers = g_output_version;

    size_t nbits = i_msp->ms_nbits;
    size_t log2nregs = i_msp->ms_log2nregs;
    int64 expthresh = i_msp->ms_expthresh;
    size_t sparseon = i_msp->ms_sparseon;

    switch (i_msp->ms_type)
    {
    case MST_EMPTY:
        pack_header(o_bitp, vers, MST_EMPTY,
                    nbits, log2nregs, expthresh, sparseon);
        break;

    case MST_EXPLICIT:
        {
            ms_explicit_t const * msep = &i_msp->ms_data.as_expl;
            size_t size = msep->mse_nelem;

            size_t ndx = pack_header(o_bitp, vers, MST_EXPLICIT,
                                     nbits, log2nregs, expthresh, sparseon);

            for (size_t ii = 0; ii < size; ++ii)
            {
                uint64_t val = msep->mse_elems[ii];

                o_bitp[ndx++] = (val >> 56) & 0xff;
                o_bitp[ndx++] = (val >> 48) & 0xff;
                o_bitp[ndx++] = (val >> 40) & 0xff;
                o_bitp[ndx++] = (val >> 32) & 0xff;
                o_bitp[ndx++] = (val >> 24) & 0xff;
                o_bitp[ndx++] = (val >> 16) & 0xff;
                o_bitp[ndx++] = (val >>  8) & 0xff;
                o_bitp[ndx++] = (val >>  0) & 0xff;
            }
        }
        break;

    case MST_COMPRESSED:
        {
            ms_compressed_t const * mscp = &i_msp->ms_data.as_comp;
            size_t nregs = i_msp->ms_nregs;
            size_t nfilled = numfilled(i_msp);

            // Should we pack this as MST_SPARSE or MST_COMPRESSED?
            // IMPORTANT - matching code in multiset_packed_size!
            size_t sparsebitsz;
            size_t cmprssbitsz;
            sparsebitsz = nfilled * (log2nregs + nbits);
            cmprssbitsz = nregs * nbits;

            // If the vector does not have sparse enabled use
            // compressed.
            //
            // If the vector is smaller then the max sparse size use
            // compressed.
            //
            // If the max sparse size is auto (-1) use if smaller then
            // compressed.
            //
            if (sparseon &&
                ((g_max_sparse != -1 && nfilled <= g_max_sparse) ||
                 (g_max_sparse == -1 && sparsebitsz < cmprssbitsz)))
            {
                size_t ndx = pack_header(o_bitp, vers, MST_SPARSE,
                                         nbits, log2nregs, expthresh, sparseon);

                // Marshal the registers.
                sparse_pack(mscp->msc_regs,
                            nbits, nregs, log2nregs, nfilled,
                            &o_bitp[ndx], i_size - ndx);
            }
            else
            {
                size_t ndx = pack_header(o_bitp, vers, MST_COMPRESSED,
                                         nbits, log2nregs, expthresh, sparseon);

                // Marshal the registers.
                compressed_pack(mscp->msc_regs, nbits, nregs,
                                &o_bitp[ndx], i_size - ndx, vers);
            }
            break;
        }

    case MST_UNDEFINED:
        pack_header(o_bitp, vers, MST_UNDEFINED,
                    nbits, log2nregs, expthresh, sparseon);
        break;

    case MST_SPARSE:
        // We only marshal (pack) into sparse format; complain if
        // an in-memory multiset claims it is sparse.
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("invalid internal sparse format")));
        break;

    default:
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("undefined multiset type value #2")));
        break;
    }
}

static size_t
multiset_copy_size(multiset_t const * i_msp)
{
    size_t retval = 0;

    switch (i_msp->ms_type)
    {
    case MST_EMPTY:
        retval = __builtin_offsetof(multiset_t, ms_data);
        break;

    case MST_EXPLICIT:
        {
            ms_explicit_t const * msep = &i_msp->ms_data.as_expl;
            retval = __builtin_offsetof(multiset_t, ms_data.as_expl.mse_elems);
            retval += (msep->mse_nelem * sizeof(uint64_t));
        }
        break;

    case MST_COMPRESSED:
        {
            retval = __builtin_offsetof(multiset_t, ms_data.as_comp.msc_regs);
            retval += (i_msp->ms_nregs * sizeof(compreg_t));
        }
        break;

    case MST_UNDEFINED:
        retval = __builtin_offsetof(multiset_t, ms_data);
        break;

    default:
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("undefined multiset type value #3")));
        break;
    }

    return retval;
}

static size_t
multiset_packed_size(multiset_t const * i_msp)
{
    uint8_t vers = g_output_version;

    size_t retval = 0;

    switch (i_msp->ms_type)
    {
    case MST_EMPTY:
        switch (vers)
        {
        case 1:
            retval = 3;
            break;
        default:
            Assert(vers == 1);
        }
        break;

    case MST_EXPLICIT:
        switch (vers)
        {
        case 1:
            {
                ms_explicit_t const * msep = &i_msp->ms_data.as_expl;
                retval = 3 + (8 * msep->mse_nelem);
            }
            break;
        default:
            Assert(vers == 1);
        }
        break;

    case MST_COMPRESSED:
        if (vers == 1)
        {
            size_t hdrsz = 3;
            size_t nbits = i_msp->ms_nbits;
            size_t nregs = i_msp->ms_nregs;
            size_t nfilled = numfilled(i_msp);
            size_t log2nregs = i_msp->ms_log2nregs;
            size_t sparseon = i_msp->ms_sparseon;
            size_t sparsebitsz;
            size_t cmprssbitsz;

            // Should we pack this as MST_SPARSE or MST_COMPRESSED?
            // IMPORTANT - matching code in multiset_pack!
            //
            sparsebitsz = numfilled(i_msp) * (log2nregs + nbits);
            cmprssbitsz = nregs * nbits;

            // If the vector does not have sparse enabled use
            // compressed.
            //
            // If the vector is smaller then the max sparse size use
            // compressed.
            //
            // If the max sparse size is auto (-1) use if smaller then
            // compressed.
            //
            if (sparseon &&
                ((g_max_sparse != -1 && nfilled <= g_max_sparse) ||
                 (g_max_sparse == -1 && sparsebitsz < cmprssbitsz)))

            {
                // MST_SPARSE is more compact.
                retval = hdrsz + ((sparsebitsz + 7) / 8);
            }
            else
            {
                // MST_COMPRESSED is more compact.
                retval = hdrsz + ((cmprssbitsz + 7) / 8);
            }
        }
        else
        {
            Assert(vers == 1);
        }
        break;

    case MST_UNDEFINED:
        if (vers == 1)
        {
            size_t hdrsz = 3;
            retval = hdrsz;
        }
        else
        {
            Assert(vers == 1);
        }
        break;

    case MST_SPARSE:
        // We only marshal (pack) into sparse format; complain if
        // an in-memory multiset claims it is sparse.
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("invalid internal sparse format")));
        break;

    default:
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("undefined multiset type value #4")));
        break;
    }

    return retval;
}

PG_FUNCTION_INFO_V1(hll_in);
Datum		hll_in(PG_FUNCTION_ARGS);
Datum
hll_in(PG_FUNCTION_ARGS)
{
    Datum dd = DirectFunctionCall1(byteain, PG_GETARG_DATUM(0));

    int32 typmod = PG_GETARG_INT32(2);

    // Unpack to make sure the data is valid.
    bytea * bp = DatumGetByteaP(dd);
    size_t sz = VARSIZE(bp) - VARHDRSZ;
    multiset_t ms;
    multiset_unpack(&ms, (uint8_t *) VARDATA(bp), sz, NULL);

    // The typmod value will be valid for COPY and \COPY statements.
    // Check the metadata consistency in these cases.
    if (typmod != -1)
    {
        int32 log2m = typmod_log2m(typmod);
        int32 regwidth = typmod_regwidth(typmod);
        int64 expthresh = decode_expthresh(typmod_expthresh(typmod));
        int32 sparseon = typmod_sparseon(typmod);

        // Create a placeholder w/ declared metadata.
        multiset_t msx;
        msx.ms_nbits = regwidth;
        msx.ms_nregs = (1 << log2m);
        msx.ms_log2nregs = log2m;
        msx.ms_expthresh = expthresh;
        msx.ms_sparseon = sparseon;

        // Make sure the declared metadata matches the incoming.
        check_metadata(&msx, &ms);
    }

    return dd;
}

PG_FUNCTION_INFO_V1(hll_out);
Datum		hll_out(PG_FUNCTION_ARGS);
Datum
hll_out(PG_FUNCTION_ARGS)
{
    // NOTE - It's still worth interposing on these calls in case
    // we want to support alternate representations in the future.

    Datum dd = DirectFunctionCall1(byteaout, PG_GETARG_DATUM(0));
    return dd;
}

PG_FUNCTION_INFO_V1(hll);
Datum		hll(PG_FUNCTION_ARGS);
Datum
hll(PG_FUNCTION_ARGS)
{
    Datum dd = PG_GETARG_DATUM(0);
    bytea * bp = DatumGetByteaP(dd);
    size_t sz = VARSIZE(bp) - VARHDRSZ;
    int32 typmod = PG_GETARG_INT32(1);	// !! DIFFERENT THEN IN hll_in!
    bool isexplicit = PG_GETARG_BOOL(2); // explicit cast, not explicit vector
    int32 log2m = typmod_log2m(typmod);
    int32 regwidth = typmod_regwidth(typmod);
    int64 expthresh = decode_expthresh(typmod_expthresh(typmod));
    int32 sparseon = typmod_sparseon(typmod);

    multiset_t ms;
    multiset_t msx;

    // Unpack the bit data.
    multiset_unpack(&ms, (uint8_t *) VARDATA(bp), sz, NULL);

    // Make the compiler happpy.
    (void) isexplicit;

    // Create a placeholder w/ declared metadata.
    msx.ms_nbits = regwidth;
    msx.ms_nregs = (1 << log2m);
    msx.ms_log2nregs = log2m;
    msx.ms_expthresh = expthresh;
    msx.ms_sparseon = sparseon;

    // Make sure the declared metadata matches the incoming.
    check_metadata(&msx, &ms);

    // If we make it here we're good.
    return dd;
}

PG_FUNCTION_INFO_V1(hll_hashval_in);
Datum		hll_hashval_in(PG_FUNCTION_ARGS);
Datum
hll_hashval_in(PG_FUNCTION_ARGS)
{
    Datum dd = DirectFunctionCall1(int8in, PG_GETARG_DATUM(0));
    return dd;
}

PG_FUNCTION_INFO_V1(hll_hashval_out);
Datum		hll_hashval_out(PG_FUNCTION_ARGS);
Datum
hll_hashval_out(PG_FUNCTION_ARGS)
{
    Datum dd = DirectFunctionCall1(int8out, PG_GETARG_DATUM(0));
    return dd;
}

PG_FUNCTION_INFO_V1(hll_hashval_eq);
Datum		hll_hashval_eq(PG_FUNCTION_ARGS);
Datum
hll_hashval_eq(PG_FUNCTION_ARGS)
{
    int64 aa = PG_GETARG_INT64(0);
    int64 bb = PG_GETARG_INT64(1);
    PG_RETURN_BOOL(aa == bb);
}

PG_FUNCTION_INFO_V1(hll_hashval_ne);
Datum		hll_hashval_ne(PG_FUNCTION_ARGS);
Datum
hll_hashval_ne(PG_FUNCTION_ARGS)
{
    int64 aa = PG_GETARG_INT64(0);
    int64 bb = PG_GETARG_INT64(1);
    PG_RETURN_BOOL(aa != bb);
}

PG_FUNCTION_INFO_V1(hll_hashval);
Datum		hll_hashval(PG_FUNCTION_ARGS);
Datum
hll_hashval(PG_FUNCTION_ARGS)
{
    int64 aa = PG_GETARG_INT64(0);
    PG_RETURN_INT64(aa);
}

PG_FUNCTION_INFO_V1(hll_hashval_int4);
Datum		hll_hashval_int4(PG_FUNCTION_ARGS);
Datum
hll_hashval_int4(PG_FUNCTION_ARGS)
{
    int32 aa = PG_GETARG_INT32(0);
    int64 aaaa = aa;	// extend value to 64 bits.
    PG_RETURN_INT64(aaaa);
}

static void
check_modifiers(int32 log2m, int32 regwidth, int64 expthresh, int32 sparseon)
{
    // Range check each of the modifiers.
    if (log2m < 0 || log2m > MAX_BITVAL(LOG2M_BITS))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("log2m modifier must be between 0 and 31")));

    if (regwidth < 0 || regwidth > MAX_BITVAL(REGWIDTH_BITS))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("regwidth modifier must be between 0 and 7")));

    if (expthresh < -1 || expthresh > 4294967296LL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("expthresh modifier must be between -1 and 2^32")));

    if (expthresh > 0 && (1LL << integer_log2(expthresh)) != expthresh)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("expthresh modifier must be power of 2")));

    if (sparseon < 0 || sparseon > MAX_BITVAL(SPARSEON_BITS))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("sparseon modifier must be 0 or 1")));
}

// This routine is cloned from the arrayutils ArrayGetIntegerTypmods
// and converted to return 64 bit integers.
//
static int64 *
ArrayGetInteger64Typmods(ArrayType *arr, int *n)
{
	int64	   *result;
	Datum	   *elem_values;
	int			i;

	if (ARR_ELEMTYPE(arr) != CSTRINGOID)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_ELEMENT_ERROR),
				 errmsg("typmod array must be type cstring[]")));

	if (ARR_NDIM(arr) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("typmod array must be one-dimensional")));

	if (array_contains_nulls(arr))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("typmod array must not contain nulls")));

	/* hardwired knowledge about cstring's representation details here */
	deconstruct_array(arr, CSTRINGOID,
					  -2, false, 'c',
					  &elem_values, NULL, n);

	result = (int64 *) palloc(*n * sizeof(int64));

	for (i = 0; i < *n; i++)
    {
        char * endp = NULL;
        result[i] = strtoll(DatumGetCString(elem_values[i]), &endp, 0);
        if (*endp != '\0')
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("typmod array must contain integers")));
    }

	pfree(elem_values);

	return result;
}

PG_FUNCTION_INFO_V1(hll_typmod_in);
Datum		hll_typmod_in(PG_FUNCTION_ARGS);
Datum
hll_typmod_in(PG_FUNCTION_ARGS)
{
	ArrayType *	ta = PG_GETARG_ARRAYTYPE_P(0);
	int64 * tl;
	int nmods;
    int32 typmod;
    int32 log2m;
    int32 regwidth;
    int64 expthresh;
    int32 sparseon;

	tl = ArrayGetInteger64Typmods(ta, &nmods);

    // Make sure the number of type modifiers is in a valid range.
    if (nmods > 4 || nmods < 0)
    {
        ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid number of type modifiers")));
		typmod = 0;				/* keep compiler quiet */
    }
    else
    {
        // Process the type modifiers, supply defaults if necessary.
        log2m =		(nmods >= 1) ? tl[0] : g_default_log2m;
        regwidth =	(nmods >= 2) ? tl[1] : g_default_regwidth;
        expthresh =	(nmods >= 3) ? tl[2] : g_default_expthresh;
        sparseon =	(nmods == 4) ? tl[3] : g_default_sparseon;

        check_modifiers(log2m, regwidth, expthresh, sparseon);

        // Construct the typmod value.
        typmod =
            (log2m << (TYPMOD_BITS - LOG2M_BITS)) |
            (regwidth << (TYPMOD_BITS - LOG2M_BITS - REGWIDTH_BITS)) |
            (encode_expthresh(expthresh) <<
                         (TYPMOD_BITS - LOG2M_BITS - REGWIDTH_BITS -
                           EXPTHRESH_BITS)) |
            (sparseon << (TYPMOD_BITS - LOG2M_BITS - REGWIDTH_BITS -
                          EXPTHRESH_BITS - SPARSEON_BITS));
    }

	PG_RETURN_INT32(typmod);
}

PG_FUNCTION_INFO_V1(hll_typmod_out);
Datum		hll_typmod_out(PG_FUNCTION_ARGS);
Datum
hll_typmod_out(PG_FUNCTION_ARGS)
{
    int32 typmod = (uint32) PG_GETARG_INT32(0);
    int32 log2m = typmod_log2m(typmod);
    int32 regwidth = typmod_regwidth(typmod);
    int64 expthresh = decode_expthresh(typmod_expthresh(typmod));
    int32 sparseon = typmod_sparseon(typmod);

    char buffer[1024];
    size_t len;
    char * typmodstr;

    memset(buffer, '\0', sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "(%d,%d,%ld,%d)",
             log2m, regwidth, expthresh, sparseon);

    len = strlen(buffer) + 1;
    typmodstr = (char *) palloc(len);
    strncpy(typmodstr, buffer, len);

	PG_RETURN_CSTRING(typmodstr);
}

static void
multiset_union(multiset_t * o_msap, multiset_t const * i_msbp)
{
    int typea = o_msap->ms_type;
    int typeb = i_msbp->ms_type;

    // If either multiset is MST_UNDEFINED result is MST_UNDEFINED.
    if (typea == MST_UNDEFINED || typeb == MST_UNDEFINED)
    {
        o_msap->ms_type = MST_UNDEFINED;
        return;
    }

    // If B is MST_EMPTY, we're done, A is unchanged.
    if (typeb == MST_EMPTY)
        return;

    // If A is MST_EMPTY, return B instead.
    if (typea == MST_EMPTY)
    {
        memcpy(o_msap, i_msbp, multiset_copy_size(i_msbp));
        return;
    }

    switch (typea)
    {
    case MST_EXPLICIT:
        {
            switch (typeb)
            {
            case MST_EXPLICIT:
                {
                    ms_explicit_t const * msebp =
                        (ms_explicit_t const *) &i_msbp->ms_data.as_expl;

                    // Note - we may not be explicit after this ...
                    explicit_union(o_msap, msebp);
                }
                break;

            case MST_COMPRESSED:
                {
                    // Make a copy of B since we can't modify it in place.
                    multiset_t mst;
                    memcpy(&mst, i_msbp, multiset_copy_size(i_msbp));
                    // Union into the copy.
                    compressed_explicit_union(&mst, o_msap);
                    // Copy the result over the A argument.
                    memcpy(o_msap, &mst, multiset_copy_size(&mst));
                }
                break;

            default:
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_EXCEPTION),
                         errmsg("undefined multiset type value #5")));
                break;
            }
        }
        break;

    case MST_COMPRESSED:
        {
            ms_compressed_t * mscap =
                (ms_compressed_t *) &o_msap->ms_data.as_comp;

            switch (typeb)
            {
            case MST_EXPLICIT:
                {
                    compressed_explicit_union(o_msap, i_msbp);
                }
                break;

            case MST_COMPRESSED:
                {
                    ms_compressed_t const * mscbp =
                        (ms_compressed_t const *) &i_msbp->ms_data.as_comp;

                    // The compressed vectors must be the same length.
                    if (o_msap->ms_nregs != i_msbp->ms_nregs)
                        ereport(ERROR,
                                (errcode(ERRCODE_DATA_EXCEPTION),
                                 errmsg("union of differently length "
                                        "compressed vectors not supported")));

                    for (unsigned ii = 0; ii < o_msap->ms_nregs; ++ii)
                    {
                        if (mscap->msc_regs[ii] < mscbp->msc_regs[ii])
                            mscap->msc_regs[ii] = mscbp->msc_regs[ii];
                    }
                }
                break;

            default:
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_EXCEPTION),
                         errmsg("undefined multiset type value #6")));
                break;
            }
        }
        break;

    default:
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("undefined multiset type value #7")));
        break;
    }
}

double gamma_register_count_squared(int nregs);
double
gamma_register_count_squared(int nregs)
{
    if (nregs <= 8)
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("number of registers too small")));

    switch (nregs)
    {
    case 16:	return 0.673 * nregs * nregs;
    case 32:	return 0.697 * nregs * nregs;
    case 64:	return 0.709 * nregs * nregs;
    default:	return (0.7213 / (1.0 + 1.079 / nregs)) * nregs * nregs;
    }
}

static double
multiset_card(multiset_t const * i_msp)
{
    size_t nbits = i_msp->ms_nbits;
    size_t log2m = i_msp->ms_log2nregs;

    double retval = 0.0;

    uint64 max_register_value = (1ULL << nbits) - 1;
    uint64 pw_bits = (max_register_value - 1);
    uint64 total_bits = (pw_bits + log2m);
    uint64 two_to_l = (1ULL << total_bits);

    double large_estimator_cutoff = (double) two_to_l/30.0;

    switch (i_msp->ms_type)
    {
    case MST_EMPTY:
        retval = 0.0;
        break;

    case MST_EXPLICIT:
        {
            ms_explicit_t const * msep = &i_msp->ms_data.as_expl;
            return msep->mse_nelem;
        }
        break;

    case MST_COMPRESSED:
        {
            unsigned ii;
            double sum;
            int zero_count;
            uint64_t rval;
            double estimator;

            ms_compressed_t const * mscp = &i_msp->ms_data.as_comp;
            size_t nregs = i_msp->ms_nregs;

            sum = 0.0;
            zero_count = 0;

            for (ii = 0; ii < nregs; ++ii)
            {
                rval = mscp->msc_regs[ii];
                sum += 1.0 / (1L << rval);
                if (rval == 0)
                    ++zero_count;
            }

            estimator = gamma_register_count_squared(nregs) / sum;

            if ((zero_count != 0) && (estimator < (5.0 * nregs / 2.0)))
                retval = nregs * log((double) nregs / zero_count);
            else if (estimator <= large_estimator_cutoff)
                retval = estimator;
            else
                return (-1 * two_to_l) * log(1.0 - (estimator/two_to_l));
        }
        break;

    case MST_UNDEFINED:
        // Our caller will convert this to a NULL.
        retval = -1.0;
        break;

    default:
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("undefined multiset type value #8")));
        break;
    }

    return retval;
}

// Cardinality of a multiset.
//
PG_FUNCTION_INFO_V1(hll_cardinality);
Datum		hll_cardinality(PG_FUNCTION_ARGS);
Datum
hll_cardinality(PG_FUNCTION_ARGS)
{
    double retval = 0.0;

    bytea * ab;
    size_t asz;
    multiset_t ms;

    ab = PG_GETARG_BYTEA_P(0);
    asz = VARSIZE(ab) - VARHDRSZ;

    multiset_unpack(&ms, (uint8_t *) VARDATA(ab), asz, NULL);

    retval = multiset_card(&ms);

    if (retval == -1.0)
        PG_RETURN_NULL();
    else
        PG_RETURN_FLOAT8(retval);
}

// Union of a pair of multiset.
//
PG_FUNCTION_INFO_V1(hll_union);
Datum		hll_union(PG_FUNCTION_ARGS);
Datum
hll_union(PG_FUNCTION_ARGS)
{
    bytea * ab;
    size_t asz;
    bytea * bb;
    size_t bsz;

    bytea * cb;
    size_t csz;

    multiset_t	msa;
    multiset_t	msb;

    ab = PG_GETARG_BYTEA_P(0);
    asz = VARSIZE(ab) - VARHDRSZ;

    bb = PG_GETARG_BYTEA_P(1);
    bsz = VARSIZE(bb) - VARHDRSZ;

    multiset_unpack(&msa, (uint8_t *) VARDATA(ab), asz, NULL);
    multiset_unpack(&msb, (uint8_t *) VARDATA(bb), bsz, NULL);

    check_metadata(&msa, &msb);

    multiset_union(&msa, &msb);

    csz = multiset_packed_size(&msa);
    cb = (bytea *) palloc(VARHDRSZ + csz);
    SET_VARSIZE(cb, VARHDRSZ + csz);

    multiset_pack(&msa, (uint8_t *) VARDATA(cb), csz);

    PG_RETURN_BYTEA_P(cb);
}

// Add an integer hash to a multiset.
//
PG_FUNCTION_INFO_V1(hll_add);
Datum		hll_add(PG_FUNCTION_ARGS);
Datum
hll_add(PG_FUNCTION_ARGS)
{
    bytea * ab;
    size_t asz;
    int64 val;

    bytea * cb;
    size_t csz;

    multiset_t	msa;

    ab = PG_GETARG_BYTEA_P(0);
    asz = VARSIZE(ab) - VARHDRSZ;

    val = PG_GETARG_INT64(1);

    multiset_unpack(&msa, (uint8_t *) VARDATA(ab), asz, NULL);

    multiset_add(&msa, val);

    csz = multiset_packed_size(&msa);
    cb = (bytea *) palloc(VARHDRSZ + csz);
    SET_VARSIZE(cb, VARHDRSZ + csz);

    multiset_pack(&msa, (uint8_t *) VARDATA(cb), csz);

    PG_RETURN_BYTEA_P(cb);
}

// Add a multiset to an integer hash.
//
PG_FUNCTION_INFO_V1(hll_add_rev);
Datum		hll_add_rev(PG_FUNCTION_ARGS);
Datum
hll_add_rev(PG_FUNCTION_ARGS)
{
    bytea * ab;
    size_t asz;
    int64 val;

    bytea * cb;
    size_t csz;

    multiset_t	msa;

    val = PG_GETARG_INT64(0);

    ab = PG_GETARG_BYTEA_P(1);
    asz = VARSIZE(ab) - VARHDRSZ;

    multiset_unpack(&msa, (uint8_t *) VARDATA(ab), asz, NULL);

    multiset_add(&msa, val);

    csz = multiset_packed_size(&msa);
    cb = (bytea *) palloc(VARHDRSZ + csz);
    SET_VARSIZE(cb, VARHDRSZ + csz);

    multiset_pack(&msa, (uint8_t *) VARDATA(cb), csz);

    PG_RETURN_BYTEA_P(cb);
}

// Pretty-print a multiset
//
PG_FUNCTION_INFO_V1(hll_print);
Datum		hll_print(PG_FUNCTION_ARGS);
Datum
hll_print(PG_FUNCTION_ARGS)
{
    bytea * ab;
    size_t asz;
    char * retstr;
    multiset_t	msa;

    ab = PG_GETARG_BYTEA_P(0);
    asz = VARSIZE(ab) - VARHDRSZ;

    // Unpack the multiset.
    multiset_unpack(&msa, (uint8_t *) VARDATA(ab), asz, NULL);

    retstr = multiset_tostring(&msa);

	PG_RETURN_CSTRING(retstr);
}

// Create an empty multiset with parameters.
//
PG_FUNCTION_INFO_V1(hll_empty4);
Datum		hll_empty4(PG_FUNCTION_ARGS);
Datum
hll_empty4(PG_FUNCTION_ARGS)
{
    bytea * cb;
    size_t csz;

    int32 log2m = PG_GETARG_INT32(0);
    int32 regwidth = PG_GETARG_INT32(1);
    int64 expthresh = PG_GETARG_INT64(2);
    int32 sparseon = PG_GETARG_INT32(3);

    multiset_t	ms;

    check_modifiers(log2m, regwidth, expthresh, sparseon);

    memset(&ms, '\0', sizeof(ms));

    ms.ms_type = MST_EMPTY;
    ms.ms_nbits = regwidth;
    ms.ms_nregs = 1 << log2m;
    ms.ms_log2nregs = log2m;
    ms.ms_expthresh = expthresh;
    ms.ms_sparseon = sparseon;

    csz = multiset_packed_size(&ms);
    cb = (bytea *) palloc(VARHDRSZ + csz);
    SET_VARSIZE(cb, VARHDRSZ + csz);

    multiset_pack(&ms, (uint8_t *) VARDATA(cb), csz);

    PG_RETURN_BYTEA_P(cb);
}

// Create an empty multiset with parameters.
//
PG_FUNCTION_INFO_V1(hll_empty3);
Datum		hll_empty3(PG_FUNCTION_ARGS);
Datum
hll_empty3(PG_FUNCTION_ARGS)
{
    PG_RETURN_DATUM(DirectFunctionCall4(hll_empty4,
                                        PG_GETARG_DATUM(0),
                                        PG_GETARG_DATUM(1),
                                        PG_GETARG_DATUM(2),
                                        Int32GetDatum(g_default_sparseon)));
}

// Create an empty multiset with parameters.
//
PG_FUNCTION_INFO_V1(hll_empty2);
Datum		hll_empty2(PG_FUNCTION_ARGS);
Datum
hll_empty2(PG_FUNCTION_ARGS)
{
    PG_RETURN_DATUM(DirectFunctionCall4(hll_empty4,
                                        PG_GETARG_DATUM(0),
                                        PG_GETARG_DATUM(1),
                                        Int64GetDatum(g_default_expthresh),
                                        Int32GetDatum(g_default_sparseon)));
}

// Create an empty multiset with parameters.
//
PG_FUNCTION_INFO_V1(hll_empty1);
Datum		hll_empty1(PG_FUNCTION_ARGS);
Datum
hll_empty1(PG_FUNCTION_ARGS)
{
    PG_RETURN_DATUM(DirectFunctionCall4(hll_empty4,
                                        PG_GETARG_DATUM(0),
                                        Int32GetDatum(g_default_regwidth),
                                        Int64GetDatum(g_default_expthresh),
                                        Int32GetDatum(g_default_sparseon)));
}

// Create an empty multiset with parameters.
//
PG_FUNCTION_INFO_V1(hll_empty0);
Datum		hll_empty0(PG_FUNCTION_ARGS);
Datum
hll_empty0(PG_FUNCTION_ARGS)
{
    PG_RETURN_DATUM(DirectFunctionCall4(hll_empty4,
                                        Int32GetDatum(g_default_log2m),
                                        Int32GetDatum(g_default_regwidth),
                                        Int64GetDatum(g_default_expthresh),
                                        Int32GetDatum(g_default_sparseon)));
}

// Returns the schema version of an hll.
//
PG_FUNCTION_INFO_V1(hll_schema_version);
Datum		hll_schema_version(PG_FUNCTION_ARGS);
Datum
hll_schema_version(PG_FUNCTION_ARGS)
{
    bytea * ab;
    size_t asz;
    multiset_t	msa;
    uint8_t vers;

    ab = PG_GETARG_BYTEA_P(0);
    asz = VARSIZE(ab) - VARHDRSZ;

    // Unpack the multiset.
    vers = multiset_unpack(&msa, (uint8_t *) VARDATA(ab), asz, NULL);

	PG_RETURN_INT32(vers);
}

// Returns the type of an hll.
//
PG_FUNCTION_INFO_V1(hll_type);
Datum		hll_type(PG_FUNCTION_ARGS);
Datum
hll_type(PG_FUNCTION_ARGS)
{
    bytea * ab;
    size_t asz;
    multiset_t	msa;
    uint8_t type;
    uint8_t vers;

    ab = PG_GETARG_BYTEA_P(0);
    asz = VARSIZE(ab) - VARHDRSZ;

    // Unpack the multiset.
    vers = multiset_unpack(&msa, (uint8_t *) VARDATA(ab), asz, &type);

	PG_RETURN_INT32(type);
}

// Returns the log2m of an hll.
//
PG_FUNCTION_INFO_V1(hll_log2m);
Datum		hll_log2m(PG_FUNCTION_ARGS);
Datum
hll_log2m(PG_FUNCTION_ARGS)
{
    bytea * ab;
    size_t asz;
    multiset_t	msa;
    uint8_t vers;

    ab = PG_GETARG_BYTEA_P(0);
    asz = VARSIZE(ab) - VARHDRSZ;

    // Unpack the multiset.
    vers = multiset_unpack(&msa, (uint8_t *) VARDATA(ab), asz, NULL);

	PG_RETURN_INT32(msa.ms_log2nregs);
}

// Returns the regwidth of an hll.
//
PG_FUNCTION_INFO_V1(hll_regwidth);
Datum		hll_regwidth(PG_FUNCTION_ARGS);
Datum
hll_regwidth(PG_FUNCTION_ARGS)
{
    bytea * ab;
    size_t asz;
    multiset_t	msa;
    uint8_t vers;

    ab = PG_GETARG_BYTEA_P(0);
    asz = VARSIZE(ab) - VARHDRSZ;

    // Unpack the multiset.
    vers = multiset_unpack(&msa, (uint8_t *) VARDATA(ab), asz, NULL);

	PG_RETURN_INT32(msa.ms_nbits);
}

// Returns the expthresh of an hll.
//
PG_FUNCTION_INFO_V1(hll_expthresh);
Datum		hll_expthresh(PG_FUNCTION_ARGS);
Datum
hll_expthresh(PG_FUNCTION_ARGS)
{
    bytea * ab;
    size_t asz;
    multiset_t	msa;
    uint8_t vers;

    size_t nbits;
    size_t nregs;
    int64 expthresh;

    int64 effective;

	Datum		result;

    ab = PG_GETARG_BYTEA_P(0);
    asz = VARSIZE(ab) - VARHDRSZ;

    // Unpack the multiset.
    vers = multiset_unpack(&msa, (uint8_t *) VARDATA(ab), asz, NULL);

    nbits = msa.ms_nbits;
    nregs = msa.ms_nregs;
    expthresh = msa.ms_expthresh;

    effective = expthresh_value(expthresh, nbits, nregs);

    // Build the result tuple.
	{
		TupleDesc tupleDesc;
		int j;
		char * values[2];
		HeapTuple tuple;

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupleDesc) !=
            TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		j = 0;

		values[j] = palloc(32);
		snprintf(values[j++], 32, INT64_FORMAT, expthresh);
		values[j] = palloc(32);
		snprintf(values[j++], 32, INT64_FORMAT, effective);

		tuple = BuildTupleFromCStrings(TupleDescGetAttInMetadata(tupleDesc),
									   values);

		result = HeapTupleGetDatum(tuple);
	}

	PG_RETURN_DATUM(result);
}

// Returns the sparseon of an hll.
//
PG_FUNCTION_INFO_V1(hll_sparseon);
Datum		hll_sparseon(PG_FUNCTION_ARGS);
Datum
hll_sparseon(PG_FUNCTION_ARGS)
{
    bytea * ab;
    size_t asz;
    multiset_t	msa;
    uint8_t vers;

    ab = PG_GETARG_BYTEA_P(0);
    asz = VARSIZE(ab) - VARHDRSZ;

    // Unpack the multiset.
    vers = multiset_unpack(&msa, (uint8_t *) VARDATA(ab), asz, NULL);

	PG_RETURN_INT32(msa.ms_sparseon);
}

// Set the output version.
//
PG_FUNCTION_INFO_V1(hll_set_output_version);
Datum		hll_set_output_version(PG_FUNCTION_ARGS);
Datum
hll_set_output_version(PG_FUNCTION_ARGS)
{
    int32 old_vers = g_output_version;
    int32 vers = PG_GETARG_INT32(0);

    if (vers != 1)
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("output version must be 1")));

    g_output_version = vers;

    PG_RETURN_INT32(old_vers);
}

// Set sparse to full compressed threshold to fixed value.
//
PG_FUNCTION_INFO_V1(hll_set_max_sparse);
Datum		hll_set_max_sparse(PG_FUNCTION_ARGS);
Datum
hll_set_max_sparse(PG_FUNCTION_ARGS)
{
    int32 old_maxsparse = g_max_sparse;
    int32 maxsparse = PG_GETARG_INT32(0);

    if (maxsparse < -1)
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("sparse threshold must be in range [-1,MAXINT]")));

    g_max_sparse = maxsparse;

    PG_RETURN_INT32(old_maxsparse);
}

// Change the default type modifier, empty and add aggregate defaults.
//
PG_FUNCTION_INFO_V1(hll_set_defaults);
Datum		hll_set_defaults(PG_FUNCTION_ARGS);
Datum
hll_set_defaults(PG_FUNCTION_ARGS)
{
    int32 old_log2m = g_default_log2m;
    int32 old_regwidth = g_default_regwidth;
    int64 old_expthresh = g_default_expthresh;
    int32 old_sparseon = g_default_sparseon;

    int32 log2m = PG_GETARG_INT32(0);
    int32 regwidth = PG_GETARG_INT32(1);
    int64 expthresh = PG_GETARG_INT64(2);
    int32 sparseon = PG_GETARG_INT32(3);

	Datum		result;

    check_modifiers(log2m, regwidth, expthresh, sparseon);

    g_default_log2m = log2m;
    g_default_regwidth = regwidth;
    g_default_expthresh = expthresh;
    g_default_sparseon = sparseon;

    // Build the result tuple.
	{
		TupleDesc tupleDesc;
		int j;
		char * values[4];
		HeapTuple tuple;

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupleDesc) !=
            TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		j = 0;
		values[j] = palloc(32);
		snprintf(values[j++], 32, "%d", old_log2m);
		values[j] = palloc(32);
		snprintf(values[j++], 32, "%d", old_regwidth);
		values[j] = palloc(32);
		snprintf(values[j++], 32, INT64_FORMAT, old_expthresh);
		values[j] = palloc(32);
		snprintf(values[j++], 32, "%d", old_sparseon);

		tuple = BuildTupleFromCStrings(TupleDescGetAttInMetadata(tupleDesc),
									   values);

		result = HeapTupleGetDatum(tuple);
	}

	PG_RETURN_DATUM(result);
}

// Hash a 1 byte fixed-size object.
//
PG_FUNCTION_INFO_V1(hll_hash_1byte);
Datum		hll_hash_1byte(PG_FUNCTION_ARGS);
Datum
hll_hash_1byte(PG_FUNCTION_ARGS)
{
    char key = PG_GETARG_CHAR(0);
    int32 seed = PG_GETARG_INT32(1);
    uint64 out[2];

    if (seed < 0)
        ereport(WARNING,
                (errcode(ERRCODE_WARNING),
                 errmsg("negative seed values not compatible")));

    MurmurHash3_x64_128(&key, sizeof(key), seed, out);

    PG_RETURN_INT64(out[0]);
}


// Hash a 2 byte fixed-size object.
//
PG_FUNCTION_INFO_V1(hll_hash_2byte);
Datum		hll_hash_2byte(PG_FUNCTION_ARGS);
Datum
hll_hash_2byte(PG_FUNCTION_ARGS)
{
    int16 key = PG_GETARG_INT16(0);
    int32 seed = PG_GETARG_INT32(1);
    uint64 out[2];

    if (seed < 0)
        ereport(WARNING,
                (errcode(ERRCODE_WARNING),
                 errmsg("negative seed values not compatible")));

    MurmurHash3_x64_128(&key, sizeof(key), seed, out);

    PG_RETURN_INT64(out[0]);
}

// Hash a 4 byte fixed-size object.
//
PG_FUNCTION_INFO_V1(hll_hash_4byte);
Datum		hll_hash_4byte(PG_FUNCTION_ARGS);
Datum
hll_hash_4byte(PG_FUNCTION_ARGS)
{
    int32 key = PG_GETARG_INT32(0);
    int32 seed = PG_GETARG_INT32(1);
    uint64 out[2];

    if (seed < 0)
        ereport(WARNING,
                (errcode(ERRCODE_WARNING),
                 errmsg("negative seed values not compatible")));

    MurmurHash3_x64_128(&key, sizeof(key), seed, out);

    PG_RETURN_INT64(out[0]);
}

// Hash an 8 byte fixed-size object.
//
PG_FUNCTION_INFO_V1(hll_hash_8byte);
Datum		hll_hash_8byte(PG_FUNCTION_ARGS);
Datum
hll_hash_8byte(PG_FUNCTION_ARGS)
{
    int64 key = PG_GETARG_INT64(0);
    int32 seed = PG_GETARG_INT32(1);
    uint64 out[2];

    if (seed < 0)
        ereport(WARNING,
                (errcode(ERRCODE_WARNING),
                 errmsg("negative seed values not compatible")));

    MurmurHash3_x64_128(&key, sizeof(key), seed, out);

    PG_RETURN_INT64(out[0]);
}

// Hash a varlena object.
//
PG_FUNCTION_INFO_V1(hll_hash_varlena);
Datum		hll_hash_varlena(PG_FUNCTION_ARGS);
Datum
hll_hash_varlena(PG_FUNCTION_ARGS)
{
    struct varlena * vlap = PG_GETARG_VARLENA_PP(0);

    void * keyp = VARDATA_ANY(vlap);
    int len = VARSIZE_ANY_EXHDR(vlap);

    int32 seed = PG_GETARG_INT32(1);

    uint64 out[2];

    if (seed < 0)
        ereport(WARNING,
                (errcode(ERRCODE_WARNING),
                 errmsg("negative seed values not compatible")));

    MurmurHash3_x64_128(keyp, len, seed, out);

	/* Avoid leaking memory for toasted inputs */
	PG_FREE_IF_COPY(vlap, 0);

    PG_RETURN_INT64(out[0]);
}

PG_FUNCTION_INFO_V1(hll_eq);
Datum		hll_eq(PG_FUNCTION_ARGS);
Datum
hll_eq(PG_FUNCTION_ARGS)
{
    bytea * ab;
    size_t asz;
    bytea * bb;
    size_t bsz;
    bool retval;

    ab = PG_GETARG_BYTEA_P(0);
    asz = VARSIZE(ab) - VARHDRSZ;

    bb = PG_GETARG_BYTEA_P(1);
    bsz = VARSIZE(bb) - VARHDRSZ;

    if (asz != bsz)
    {
        retval = false;
    }
    else
    {
        void const * ap = VARDATA(ab);
        void const * bp = VARDATA(bb);
        int rv = memcmp(ap, bp, asz);
        retval = rv == 0;
    }

	PG_FREE_IF_COPY(ab, 0);
	PG_FREE_IF_COPY(bb, 1);
	PG_RETURN_BOOL(retval);
}

PG_FUNCTION_INFO_V1(hll_ne);
Datum		hll_ne(PG_FUNCTION_ARGS);
Datum
hll_ne(PG_FUNCTION_ARGS)
{
    bytea * ab;
    size_t asz;
    bytea * bb;
    size_t bsz;
    bool retval;

    ab = PG_GETARG_BYTEA_P(0);
    asz = VARSIZE(ab) - VARHDRSZ;

    bb = PG_GETARG_BYTEA_P(1);
    bsz = VARSIZE(bb) - VARHDRSZ;

    if (asz != bsz)
    {
        retval = true;
    }
    else
    {
        void const * ap = VARDATA(ab);
        void const * bp = VARDATA(bb);
        int rv = memcmp(ap, bp, asz);
        retval = rv != 0;
    }

	PG_FREE_IF_COPY(ab, 0);
	PG_FREE_IF_COPY(bb, 1);
	PG_RETURN_BOOL(retval);
}

// This function creates a multiset_t in a temporary context.
//
multiset_t *	setup_multiset(MemoryContext rcontext);
multiset_t *
setup_multiset(MemoryContext rcontext)
{
    MemoryContext tmpcontext;
    MemoryContext oldcontext;
    multiset_t * msp;

    tmpcontext = AllocSetContextCreate(rcontext,
                                       "multiset",
                                       ALLOCSET_DEFAULT_MINSIZE,
                                       ALLOCSET_DEFAULT_INITSIZE,
                                       ALLOCSET_DEFAULT_MAXSIZE);

    oldcontext = MemoryContextSwitchTo(tmpcontext);

    msp = (multiset_t *) palloc(sizeof(multiset_t));

    msp->ms_type = MST_UNINIT;

    MemoryContextSwitchTo(oldcontext);

    return msp;
}

// Union aggregate transition function, first arg unpacked, second packed.
//
// NOTE - This function is not declared STRICT, it is initialized with
// a NULL ...
//
PG_FUNCTION_INFO_V1(hll_union_trans);
Datum		hll_union_trans(PG_FUNCTION_ARGS);
Datum
hll_union_trans(PG_FUNCTION_ARGS)
{
    MemoryContext aggctx;

    bytea * bb;
    size_t bsz;

    multiset_t * msap;

    multiset_t msb;

    // We must be called as a transition routine or we fail.
    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("hll_union_trans outside transition context")));

    // Is the first argument a NULL?
    if (PG_ARGISNULL(0))
    {
        msap = setup_multiset(aggctx);
    }
    else
    {
        msap = (multiset_t *) PG_GETARG_POINTER(0);
    }

    // Is the second argument non-null?
    if (!PG_ARGISNULL(1))
    {
        // This is the packed "argument" vector.
        bb = PG_GETARG_BYTEA_P(1);
        bsz = VARSIZE(bb) - VARHDRSZ;

        multiset_unpack(&msb, (uint8_t *) VARDATA(bb), bsz, NULL);

        // Was the first argument uninitialized?
        if (msap->ms_type == MST_UNINIT)
        {
            // Yes, clone the metadata from the second arg.
            copy_metadata(msap, &msb);
            msap->ms_type = MST_EMPTY;
        }
        else
        {
            // Nope, make sure the metadata is compatible.
            check_metadata(msap, &msb);
        }

        multiset_union(msap, &msb);
    }

    PG_RETURN_POINTER(msap);
}

// Add aggregate transition function.
//
// NOTE - This function is not declared STRICT, it is initialized with
// a NULL ...
//
PG_FUNCTION_INFO_V1(hll_add_trans4);
Datum		hll_add_trans4(PG_FUNCTION_ARGS);
Datum
hll_add_trans4(PG_FUNCTION_ARGS)
{
    MemoryContext aggctx;

    multiset_t * msap;

    // We must be called as a transition routine or we fail.
    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("hll_add_trans4 outside transition context")));

    // Is the first argument a NULL?
    if (PG_ARGISNULL(0))
    {
        msap = setup_multiset(aggctx);
    }
    else
    {
        msap = (multiset_t *) PG_GETARG_POINTER(0);
    }

    // Is the second argument non-null?
    if (!PG_ARGISNULL(1))
    {
        int64 val = PG_GETARG_INT64(1);

        // Was the first argument uninitialized?
        if (msap->ms_type == MST_UNINIT)
        {
            int32 log2m = PG_GETARG_INT32(2);
            int32 regwidth = PG_GETARG_INT32(3);
            int64 expthresh = PG_GETARG_INT64(4);
            int32 sparseon = PG_GETARG_INT32(5);

            multiset_t	ms;

            check_modifiers(log2m, regwidth, expthresh, sparseon);

            memset(msap, '\0', sizeof(ms));

            msap->ms_type = MST_EMPTY;
            msap->ms_nbits = regwidth;
            msap->ms_nregs = 1 << log2m;
            msap->ms_log2nregs = log2m;
            msap->ms_expthresh = expthresh;
            msap->ms_sparseon = sparseon;
        }

        multiset_add(msap, val);
    }

    PG_RETURN_POINTER(msap);
}

// Add aggregate transition function.
//
// NOTE - This function is not declared STRICT, it is initialized with
// a NULL ...
//
PG_FUNCTION_INFO_V1(hll_add_trans3);
Datum		hll_add_trans3(PG_FUNCTION_ARGS);
Datum
hll_add_trans3(PG_FUNCTION_ARGS)
{
    MemoryContext aggctx;

    multiset_t * msap;

    // We must be called as a transition routine or we fail.
    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("hll_add_trans3 outside transition context")));

    // Is the first argument a NULL?
    if (PG_ARGISNULL(0))
    {
        msap = setup_multiset(aggctx);
    }
    else
    {
        msap = (multiset_t *) PG_GETARG_POINTER(0);
    }

    // Is the second argument non-null?
    if (!PG_ARGISNULL(1))
    {
        int64 val = PG_GETARG_INT64(1);

        // Was the first argument uninitialized?
        if (msap->ms_type == MST_UNINIT)
        {
            int32 log2m = PG_GETARG_INT32(2);
            int32 regwidth = PG_GETARG_INT32(3);
            int64 expthresh = PG_GETARG_INT64(4);
            int32 sparseon = g_default_sparseon;

            multiset_t	ms;

            check_modifiers(log2m, regwidth, expthresh, sparseon);

            memset(msap, '\0', sizeof(ms));

            msap->ms_type = MST_EMPTY;
            msap->ms_nbits = regwidth;
            msap->ms_nregs = 1 << log2m;
            msap->ms_log2nregs = log2m;
            msap->ms_expthresh = expthresh;
            msap->ms_sparseon = sparseon;
        }

        multiset_add(msap, val);
    }

    PG_RETURN_POINTER(msap);
}

// Add aggregate transition function.
//
// NOTE - This function is not declared STRICT, it is initialized with
// a NULL ...
//
PG_FUNCTION_INFO_V1(hll_add_trans2);
Datum		hll_add_trans2(PG_FUNCTION_ARGS);
Datum
hll_add_trans2(PG_FUNCTION_ARGS)
{
    MemoryContext aggctx;

    multiset_t * msap;

    // We must be called as a transition routine or we fail.
    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("hll_add_trans2 outside transition context")));

    // Is the first argument a NULL?
    if (PG_ARGISNULL(0))
    {
        msap = setup_multiset(aggctx);
    }
    else
    {
        msap = (multiset_t *) PG_GETARG_POINTER(0);
    }

    // Is the second argument non-null?
    if (!PG_ARGISNULL(1))
    {
        int64 val = PG_GETARG_INT64(1);

        // Was the first argument uninitialized?
        if (msap->ms_type == MST_UNINIT)
        {
            int32 log2m = PG_GETARG_INT32(2);
            int32 regwidth = PG_GETARG_INT32(3);
            int64 expthresh = g_default_expthresh;
            int32 sparseon = g_default_sparseon;

            multiset_t	ms;

            check_modifiers(log2m, regwidth, expthresh, sparseon);

            memset(msap, '\0', sizeof(ms));

            msap->ms_type = MST_EMPTY;
            msap->ms_nbits = regwidth;
            msap->ms_nregs = 1 << log2m;
            msap->ms_log2nregs = log2m;
            msap->ms_expthresh = expthresh;
            msap->ms_sparseon = sparseon;
        }

        multiset_add(msap, val);
    }

    PG_RETURN_POINTER(msap);
}

// Add aggregate transition function.
//
// NOTE - This function is not declared STRICT, it is initialized with
// a NULL ...
//
PG_FUNCTION_INFO_V1(hll_add_trans1);
Datum		hll_add_trans1(PG_FUNCTION_ARGS);
Datum
hll_add_trans1(PG_FUNCTION_ARGS)
{
    MemoryContext aggctx;

    multiset_t * msap;

    // We must be called as a transition routine or we fail.
    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("hll_add_trans1 outside transition context")));

    // Is the first argument a NULL?
    if (PG_ARGISNULL(0))
    {
        msap = setup_multiset(aggctx);
    }
    else
    {
        msap = (multiset_t *) PG_GETARG_POINTER(0);
    }

    // Is the second argument non-null?
    if (!PG_ARGISNULL(1))
    {
        int64 val = PG_GETARG_INT64(1);

        // Was the first argument uninitialized?
        if (msap->ms_type == MST_UNINIT)
        {
            int32 log2m = PG_GETARG_INT32(2);
            int32 regwidth = g_default_regwidth;
            int64 expthresh = g_default_expthresh;
            int32 sparseon = g_default_sparseon;

            multiset_t	ms;

            check_modifiers(log2m, regwidth, expthresh, sparseon);

            memset(msap, '\0', sizeof(ms));

            msap->ms_type = MST_EMPTY;
            msap->ms_nbits = regwidth;
            msap->ms_nregs = 1 << log2m;
            msap->ms_log2nregs = log2m;
            msap->ms_expthresh = expthresh;
            msap->ms_sparseon = sparseon;
        }

        multiset_add(msap, val);
    }

    PG_RETURN_POINTER(msap);
}

// Add aggregate transition function.
//
// NOTE - This function is not declared STRICT, it is initialized with
// a NULL ...
//
PG_FUNCTION_INFO_V1(hll_add_trans0);
Datum		hll_add_trans0(PG_FUNCTION_ARGS);
Datum
hll_add_trans0(PG_FUNCTION_ARGS)
{
    MemoryContext aggctx;

    multiset_t * msap;

    // We must be called as a transition routine or we fail.
    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("hll_add_trans0 outside transition context")));

    // Is the first argument a NULL?
    if (PG_ARGISNULL(0))
    {
        msap = setup_multiset(aggctx);
    }
    else
    {
        msap = (multiset_t *) PG_GETARG_POINTER(0);
    }

    // Is the second argument non-null?
    if (!PG_ARGISNULL(1))
    {
        int64 val = PG_GETARG_INT64(1);

        // Was the first argument uninitialized?
        if (msap->ms_type == MST_UNINIT)
        {
            int32 log2m = g_default_log2m;
            int32 regwidth = g_default_regwidth;
            int64 expthresh = g_default_expthresh;
            int32 sparseon = g_default_sparseon;

            multiset_t	ms;

            check_modifiers(log2m, regwidth, expthresh, sparseon);

            memset(msap, '\0', sizeof(ms));

            msap->ms_type = MST_EMPTY;
            msap->ms_nbits = regwidth;
            msap->ms_nregs = 1 << log2m;
            msap->ms_log2nregs = log2m;
            msap->ms_expthresh = expthresh;
            msap->ms_sparseon = sparseon;
        }

        multiset_add(msap, val);
    }

    PG_RETURN_POINTER(msap);
}

// Final function, converts multiset_t into packed format.
//
PG_FUNCTION_INFO_V1(hll_pack);
Datum		hll_pack(PG_FUNCTION_ARGS);
Datum
hll_pack(PG_FUNCTION_ARGS)
{
    MemoryContext aggctx;

    bytea * cb;
    size_t csz;

    multiset_t * msap;

    // We must be called as a transition routine or we fail.
    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("hll_pack outside aggregate context")));

    // Is the first argument a NULL?
    if (PG_ARGISNULL(0))
    {
        PG_RETURN_NULL();
    }
    else
    {
        msap = (multiset_t *) PG_GETARG_POINTER(0);

        // Was the aggregation uninitialized?
        if (msap->ms_type == MST_UNINIT)
        {
            PG_RETURN_NULL();
        }
        else
        {
            csz = multiset_packed_size(msap);
            cb = (bytea *) palloc(VARHDRSZ + csz);
            SET_VARSIZE(cb, VARHDRSZ + csz);

            multiset_pack(msap, (uint8_t *) VARDATA(cb), csz);

            // We don't need to pfree the msap memory because it is zone
            // allocated inside postgres.
            //
            // Furthermore, sometimes final functions are called multiple
            // times so deallocating it the first time leads to badness.

            PG_RETURN_BYTEA_P(cb);
        }
    }
}

// Final function, computes cardinality of unpacked bytea.
//
PG_FUNCTION_INFO_V1(hll_card_unpacked);
Datum		hll_card_unpacked(PG_FUNCTION_ARGS);
Datum
hll_card_unpacked(PG_FUNCTION_ARGS)
{
    MemoryContext aggctx;

    multiset_t * msap;

    double retval;

    // We must be called as a transition routine or we fail.
    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("hll_card_unpacked outside aggregate context")));

    // Is the first argument a NULL?
    if (PG_ARGISNULL(0))
    {
        retval = -1.0;
    }
    else
    {
        msap = (multiset_t *) PG_GETARG_POINTER(0);

        if (msap->ms_type == MST_UNINIT)
        {
            retval = -1.0;
        }
        else
        {
            retval = multiset_card(msap);
        }

        // We don't need to pfree the msap memory because it is zone
        // allocated inside postgres.
        //
        // Furthermore, sometimes final functions are called multiple
        // times so deallocating it the first time leads to badness.
    }

    if (retval == -1.0)
        PG_RETURN_NULL();
    else
        PG_RETURN_FLOAT8(retval);
}

// Final function, computes floor of cardinality of multiset_t.
//
PG_FUNCTION_INFO_V1(hll_floor_card_unpacked);
Datum		hll_floor_card_unpacked(PG_FUNCTION_ARGS);
Datum
hll_floor_card_unpacked(PG_FUNCTION_ARGS)
{
    MemoryContext aggctx;

    multiset_t * msap;

    double retval;
    int64_t floorval;

    // We must be called as a transition routine or we fail.
    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("hll_floor_card_unpacked "
                        "outside aggregate context")));

    // Is the first argument a NULL?
    if (PG_ARGISNULL(0))
    {
        retval = -1.0;
    }
    else
    {
        msap = (multiset_t *) PG_GETARG_POINTER(0);

        if (msap->ms_type == MST_UNINIT)
        {
            retval = -1.0;
        }
        else
        {
            retval = multiset_card(msap);
        }

        // NOTE - The following comment from array_agg_final suggests we
        // should not free the memory here:
        //
        // "Make the result.  We cannot release the ArrayBuildState
        // because sometimes aggregate final functions are
        // re-executed.  Rather, it is nodeAgg.c's responsibility to reset the
        // aggcontext when it's safe to do so."
    }

    if (retval == -1.0)
    {
        PG_RETURN_NULL();
    }
    else
    {
        // Take the floor of the value.
        floorval = (int64) floor(retval);
        PG_RETURN_INT64(floorval);
    }
}

// Final function, computes ceil of cardinality of multiset_t.
//
PG_FUNCTION_INFO_V1(hll_ceil_card_unpacked);
Datum		hll_ceil_card_unpacked(PG_FUNCTION_ARGS);
Datum
hll_ceil_card_unpacked(PG_FUNCTION_ARGS)
{
    MemoryContext aggctx;

    multiset_t * msap;

    double retval;
    int64_t ceilval;

    // We must be called as a transition routine or we fail.
    if (!AggCheckCallContext(fcinfo, &aggctx))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("hll_ceil_card_unpacked outside aggregate context")));

    // Is the first argument a NULL?
    if (PG_ARGISNULL(0))
    {
        retval = -1.0;
    }
    else
    {
        msap = (multiset_t *) PG_GETARG_POINTER(0);

        if (msap->ms_type == MST_UNINIT)
        {
            retval = -1.0;
        }
        else
        {
            retval = multiset_card(msap);
        }

        // We don't need to pfree the msap memory because it is zone
        // allocated inside postgres.
        //
        // Furthermore, sometimes final functions are called multiple
        // times so deallocating it the first time leads to badness.
    }

    if (retval == -1.0)
    {
        PG_RETURN_NULL();
    }
    else
    {
        // Take the ceil of the value.
        ceilval = (int64) ceil(retval);
        PG_RETURN_INT64(ceilval);
    }
}
