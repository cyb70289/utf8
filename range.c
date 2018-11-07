// Brand new utf8 validation algorithm

#include <stdio.h>
#include <stdint.h>
#include <x86intrin.h>

/*
 * http://www.unicode.org/versions/Unicode6.0.0/ch03.pdf - page 94
 *
 * Table 3-7. Well-Formed UTF-8 Byte Sequences
 *
 * +--------------------+------------+-------------+------------+-------------+
 * | Code Points        | First Byte | Second Byte | Third Byte | Fourth Byte |
 * +--------------------+------------+-------------+------------+-------------+
 * | U+0000..U+007F     | 00..7F     |             |            |             |
 * +--------------------+------------+-------------+------------+-------------+
 * | U+0080..U+07FF     | C2..DF     | 80..BF      |            |             |
 * +--------------------+------------+-------------+------------+-------------+
 * | U+0800..U+0FFF     | E0         | A0..BF      | 80..BF     |             |
 * +--------------------+------------+-------------+------------+-------------+
 * | U+1000..U+CFFF     | E1..EC     | 80..BF      | 80..BF     |             |
 * +--------------------+------------+-------------+------------+-------------+
 * | U+D000..U+D7FF     | ED         | 80..9F      | 80..BF     |             |
 * +--------------------+------------+-------------+------------+-------------+
 * | U+E000..U+FFFF     | EE..EF     | 80..BF      | 80..BF     |             |
 * +--------------------+------------+-------------+------------+-------------+
 * | U+10000..U+3FFFF   | F0         | 90..BF      | 80..BF     | 80..BF      |
 * +--------------------+------------+-------------+------------+-------------+
 * | U+40000..U+FFFFF   | F1..F3     | 80..BF      | 80..BF     | 80..BF      |
 * +--------------------+------------+-------------+------------+-------------+
 * | U+100000..U+10FFFF | F4         | 80..8F      | 80..BF     | 80..BF      |
 * +--------------------+------------+-------------+------------+-------------+
 */

int utf8_naive(const unsigned char *data, int len);

#if 0
static void print128(const char *s, const __m128i v128)
{
  const unsigned char *v8 = (const unsigned char *)&v128;
  if (s)
    printf("%s:\t", s);
  for (int i = 0; i < 16; i++)
    printf("%02x ", v8[i]);
  printf("\n");
}
#endif

/* Get number of followup bytes to take care per high nibble */
static inline __m128i get_followup_bytes(const __m128i input)
{
    /* Why no _mm_srli_epi8 ? */
    const __m128i high_nibbles =
        _mm_and_si128(_mm_srli_epi16(input, 4), _mm_set1_epi8(0x0F));

    const __m128i followup_table =
        _mm_setr_epi8(
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 00 ~ BF */
                1, 1,                                 /* C0 ~ DF */
                2,                                    /* E0 ~ EF */
                3);                                   /* F0 ~ FF */

    return _mm_shuffle_epi8(followup_table, high_nibbles);
}

static const int8_t _range_tbl[] = {
    /* 0,    1,    2,    3,    4,    5,    6,    7 */
    0x00, 0x80, 0xA0, 0x80, 0x90, 0x80, 0xC2, 0x80, /* min */
    0x7F, 0xBF, 0xBF, 0x9F, 0xBF, 0x8F, 0xF4, 0x7F, /* max */
};

static inline int get_max_epi8(const __m128i v, const __m128i zero)
{
    /* SSE doesn't support reduce max. Only check all zero. */
    if (_mm_movemask_epi8(_mm_cmpeq_epi8(v, zero)) == 0xFFFF)
        return 0;
    return 3;
}

static inline __m128i validate(const unsigned char *data, short *error,
                               __m128i range0)
{
    const __m128i input = _mm_lddqu_si128((const __m128i *)data);

    const __m128i zero = _mm_setzero_si128();
    const __m128i one = _mm_set1_epi8(1);

    __m128i fi0 = get_followup_bytes(input);
    __m128i fi1 = _mm_setzero_si128();
    __m128i mask0 = _mm_cmpgt_epi8(fi0, zero);    /* fi0 > 0 ? 0xFF: 0 */
    __m128i mask1;
    __m128i errors = _mm_and_si128(mask0, range0);
    __m128i range1 = _mm_setzero_si128();

    /* range0 |= mask & 6 */
    range0 = _mm_or_si128(range0, _mm_and_si128(mask0, _mm_set1_epi8(6)));

    const int max_followup_bytes = get_max_epi8(fi0, zero);

    if (max_followup_bytes) {
        /* Add range 80~BF to followup bytes */
        for (int i = 0; i < max_followup_bytes; ++i) {
            /* (fi1, fi0) <<= 8 */
            fi1 = _mm_alignr_epi8(fi1, fi0, 15);
            fi0 = _mm_slli_si128(fi0, 1);

            /* mask = (fi > 0 ? 0xFF : 0) */
            mask1 = _mm_cmpgt_epi8(fi1, zero);
            mask0 = _mm_cmpgt_epi8(fi0, zero);

            /* overlap: errors |= (mask0 & range0) */
            errors = _mm_or_si128(errors, _mm_and_si128(mask0, range0));

            /* range += (mask & 1) */
            range1 = _mm_add_epi8(range1, _mm_and_si128(mask1, one));
            range0 = _mm_add_epi8(range0, _mm_and_si128(mask0, one));

            /* fi = (fi >= 1 ? fi-1 : 0) */
            fi1 = _mm_subs_epu8(fi1, one);
            fi0 = _mm_subs_epu8(fi0, one);
        }

        /*
         * Deal with special cases (not 80..BF)
         * +------------+---------------------+-------------------+
         * | First Byte | Special Second Byte | range table index |
         * +------------+---------------------+-------------------+
         * | E0         | A0..BF              | 2                 |
         * | ED         | 80..9F              | 3                 |
         * | F0         | 90..BF              | 4                 |
         * | F4         | 80..8F              | 5                 |
         * +------------+---------------------+-------------------+
         */

        /* mask0 = (input == 0xE0) & 1 */
        mask1 = _mm_cmpeq_epi8(input, _mm_set1_epi8(0xE0));
        mask0 = _mm_and_si128(mask1, one);
        /* mask0 += (input == 0xED) & 2 */
        mask1 = _mm_cmpeq_epi8(input, _mm_set1_epi8(0xED));
        mask0 = _mm_add_epi8(mask0, _mm_and_si128(mask1, _mm_set1_epi8(2)));
        /* mask0 += (input == 0xF0) & 3 */
        mask1 = _mm_cmpeq_epi8(input, _mm_set1_epi8(0xF0));
        mask0 = _mm_add_epi8(mask0, _mm_and_si128(mask1, _mm_set1_epi8(3)));
        /* mask0 += (input == 0xF4) & 4 */
        mask1 = _mm_cmpeq_epi8(input, _mm_set1_epi8(0xF4));
        mask0 = _mm_add_epi8(mask0, _mm_and_si128(mask1, _mm_set1_epi8(4)));

        /* (mask1, mask0) = (0, mask0) << 8 */
        mask1 = _mm_setzero_si128();
        mask1 = _mm_alignr_epi8(mask1, mask0, 15);
        mask0 = _mm_slli_si128(mask0, 1);

        /* range += mask */
        range1 = _mm_add_epi8(range1, mask1);
        range0 = _mm_add_epi8(range0, mask0);
    }

    /* mask0 = min, mask1 = max */
    fi0 = _mm_lddqu_si128((const __m128i *)_range_tbl);
    mask0 = _mm_shuffle_epi8(fi0, range0);
    mask1 = _mm_shuffle_epi8(fi0, _mm_add_epi8(range0, _mm_set1_epi8(8)));

    /* errors |= ((input < min) | (input > max)) */
    errors = _mm_or_si128(errors, _mm_cmplt_epi8(input, mask0));
    errors = _mm_or_si128(errors, _mm_cmpgt_epi8(input, mask1));

    /* Reduce errors vector, _mm_movemask_epi8 returns 0xFFFF if errors == 0 */
    *error |= ~_mm_movemask_epi8(_mm_cmpeq_epi8(errors, zero));

    return range1;
}

int utf8_range(const unsigned char *data, int len)
{
    if (len >= 16) {
        short error = 0;
        __m128i range = _mm_setzero_si128();

        while (len >= 16) {
            range = validate(data, &error, range);

            data += 16;
            len -= 16;
        }

        /* Delay error check till loop ends */
        if (error)
            return 0;

        /* At most three followup bytes need to take care */
        uint32_t range3 = _mm_extract_epi32(range, 0);

        while (range3) {
            uint8_t idx = range3 & 7;
            int8_t input = (int8_t)(*data);

            if (len == 0)
                return 0;
            if (input < _range_tbl[idx] || input > _range_tbl[idx+8])
                return 0;

            --len;
            ++data;
            range3 >>= 8;
        }
    }

    /* Check remaining bytes with naive method */
    return utf8_naive(data, len);
}

#ifdef DEBUG
int main(void)
{
    const unsigned char src[] =
        "\x00\x00\x00\x00\xc2\x80\x00\x00\x00\xe0\xa0\x80\x00\x00\xf4\x80" \
        "\x80\x80\x00\x00\x00\xc2\x80\x00\x00\x00\xe1\x80\x80\x00\x00\xf1" \
        "\x80\x80\x80\x00\x00";

    int ret = utf8_range(src, sizeof(src)-1);
    printf("%s\n", ret ? "ok": "bad");

    return 0;
}
#endif
