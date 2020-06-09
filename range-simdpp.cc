#ifdef __x86_64__
#define SIMDPP_ARCH_X86_SSE4_1
#endif

#ifdef __aarch64__
#define SIMDPP_ARCH_ARM_NEON
#define SIMDPP_ARCH_ARM_NEON_FLT_SP
#endif

extern "C" int utf8_naive(const unsigned char *data, int len);

#include "simdpp/simd.h"

static const uint8_t _first_len_tbl[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 3,
};

static const uint8_t _first_range_tbl[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 8, 8, 8,
};

static const uint8_t _range_min_tbl[] = {
    0x00, 0x80, 0x80, 0x80, 0xA0, 0x80, 0x90, 0x80,
    0xC2, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
static const uint8_t _range_max_tbl[] = {
    0x7F, 0xBF, 0xBF, 0xBF, 0xBF, 0x9F, 0xBF, 0x8F,
    0xF4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t _range_adjust_tbl1[] = {
    2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0,
};
static const uint8_t _range_adjust_tbl2[] = {
    3, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

extern "C" int utf8_range(const unsigned char *data, int len)
{
    if (len >= 16) {
        uint8_t zero = 0, one = 1, two = 2, e0 = 0xE0;

        uint8<16> error = simdpp::load_splat(&zero);
        uint8<16> prev_input = error;
        uint8<16> prev_first_len = error;

        const uint8<16> first_len_tbl = simdpp::load_u(_first_len_tbl);
        const uint8<16> first_range_tbl = simdpp::load_u(_first_range_tbl);
        const uint8<16> range_min_tbl = simdpp::load_u(_range_min_tbl);
        const uint8<16> range_max_tbl = simdpp::load_u(_range_max_tbl);
        const uint8<16> range_adjust_tbl1 = simdpp::load_u(_range_adjust_tbl1);
        const uint8<16> range_adjust_tbl2 = simdpp::load_u(_range_adjust_tbl2);

        const uint8<16> const_1 = simdpp::load_splat(&one);
        const uint8<16> const_2 = simdpp::load_splat(&two);
        const uint8<16> const_e0 = simdpp::load_splat(&e0);

        while (len >= 16) {
            const uint8<16> input = simdpp::load_u(data);

            const uint8<16> high_nibbles = input >> 4;

            /* first_len = first_len_tbl[high_nibbles] */
            const uint8<16> first_len =
                simdpp::permute_bytes16(first_len_tbl, high_nibbles);

            /* range = first_range_tbl[high_nibbles] */
            uint8<16> range =
                simdpp::permute_bytes16(first_range_tbl, high_nibbles);

            /* range |= (first_len high 15 bytes | prev_first_len low 1 byte) */
            range |= simdpp::align16<15>(prev_first_len, first_len);

            uint8<16> tmp1, tmp2;
            /* tmp1 = first_len >= 1 ? (first_len-1) : 0 */
            tmp1 = simdpp::sub_sat(first_len, const_1);
            tmp2 = simdpp::sub_sat(prev_first_len, const_1);
            /* range |= (tmp1 high 14 bytes + tmp2 low 2 bytes) */
            range |= simdpp::align16<14>(tmp2, tmp1);

            /* tmp1 = first_len >= 2 ? (first_len-2) : 0 */
            tmp1 = simdpp::sub_sat(first_len, const_2);
            tmp2 = simdpp::sub_sat(prev_first_len, const_2);
            /* range |= (tmp1 high 13 bytes | tmp2 low 3 bytes) */
            range |= simdpp::align16<13>(tmp2, tmp1);

            uint8<16> pos = simdpp::align16<15>(prev_input, input) - const_e0;
            //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
            // XXX: no way to combine neon and sse
            // neon `tbl` can index mutiple tables and returns 0 when index is
            // out of bound, no similar behavior from sse `pshufb`
            range += vqtbl2q_u8(range_adjust_tbl, pos);
            //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

            /* Load min and max values per calculated range index */
            uint8<16> minv = simdpp::permute_bytes16(range_min_tbl, range);
            uint8<16> maxv = simdpp::permute_bytes16(range_max_tbl, range);

            /* Check value range */
            error |= input < minv;
            error |= input > maxv;

            prev_input = input;
            prev_first_len = first_len;

            data += 16;
            len -= 16;
        }

        /* Delay error check till loop ends */
        if (simdpp::reduce_max(error))
            return -1;

        /* Find previous token (not 80~BF) */
        uint32_t token4;
        simdpp::extract<3>(&token4, (uint32<4>)prev_input);

        const int8_t *token = (const int8_t *)&token4;
        int lookahead = 0;
        if (token[3] > (int8_t)0xBF)
            lookahead = 1;
        else if (token[2] > (int8_t)0xBF)
            lookahead = 2;
        else if (token[1] > (int8_t)0xBF)
            lookahead = 3;

        data -= lookahead;
        len += lookahead;
    }

    /* Check remaining bytes with naive method */
    return utf8_naive(data, len);
}
