#ifdef __x86_64__

#include <stdio.h>
#include <stdint.h>
#include <x86intrin.h>

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

struct previous_input {
    __m128i input;
    __m128i follow_bytes;
};

static const int8_t _follow_tbl[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 00 ~ BF */
    1, 1,                               /* C0 ~ DF */
    2,                                  /* E0 ~ EF */
    3,                                  /* F0 ~ FF */
};

static const int8_t _follow_mask_tbl[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 00 ~ BF */
    8, 8,                               /* C0 ~ DF */
    8,                                  /* E0 ~ EF */
    8,                                  /* F0 ~ FF */
};

static const int8_t _range_min_tbl[] = {
    /* 0,    1,    2,    3,    4,    5,    6,    7,    8 */
    0x00, 0x80, 0x80, 0x80, 0xA0, 0x80, 0x90, 0x80, 0xC2,
    /* Must be invalid */
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
};

static const int8_t _range_max_tbl[] = {
    /* 0,    1,    2,    3,    4,    5,    6,    7,    8 */
    0x7F, 0xBF, 0xBF, 0xBF, 0xBF, 0x9F, 0xBF, 0x8F, 0xF4,
    /* Must be invalid */
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
};

/* E0: 2, ED: 3 */
static const uint8_t _df_ee_tbl[] = {
    0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0,
};

/* F0: 3; F4: 4 */
static const uint8_t _ef_fe_tbl[] = {
    0, 3, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static inline __m128i validate(const unsigned char *data, __m128i error,
       struct previous_input *prev, const __m128i tables[])
{
    const __m128i input = _mm_lddqu_si128((const __m128i *)data);

    /* Why no _mm_srli_epi8 ? */
    const __m128i high_nibbles =
        _mm_and_si128(_mm_srli_epi16(input, 4), _mm_set1_epi8(0x0F));

    __m128i follow_bytes = _mm_shuffle_epi8(tables[0], high_nibbles);

    /* range is 8 if input=0xC0~0xFF, overlap will lead to 9, 10, 11 */
    __m128i range = _mm_shuffle_epi8(tables[1], high_nibbles);

    /* 2nd byte */
    /* range |= (follow_bytes, prev.follow_bytes) << 1 byte */
    range = _mm_or_si128(
            range, _mm_alignr_epi8(follow_bytes, prev->follow_bytes, 15));

    /* 3rd bytes */
    __m128i tmp, prev_follow_bytes;
    /* saturate sub 1 */
    tmp = _mm_subs_epu8(follow_bytes, _mm_set1_epi8(1));
    prev_follow_bytes = _mm_subs_epu8(prev->follow_bytes, _mm_set1_epi8(1));
    /* range |= (tmp, prev_follow_bytes) << 2 bytes */
    tmp = _mm_alignr_epi8(tmp, prev_follow_bytes, 14);
    range = _mm_or_si128(range, tmp);

    /* 4th bytes */
    /* saturate sub 2 */
    tmp = _mm_subs_epu8(follow_bytes, _mm_set1_epi8(2));
    prev_follow_bytes = _mm_subs_epu8(prev->follow_bytes, _mm_set1_epi8(2));
    /* range |= (tmp, prev_follow_bytes) << 3 bytes */
    tmp = _mm_alignr_epi8(tmp, prev_follow_bytes, 13);
    range = _mm_or_si128(range, tmp);

    /*
     * Check special cases (not 80..BF)
     * +------------+---------------------+-------------------+
     * | First Byte | Special Second Byte | range table index |
     * +------------+---------------------+-------------------+
     * | E0         | A0..BF              | 4=2+2             |
     * | ED         | 80..9F              | 5=2+3             |
     * | F0         | 90..BF              | 6=3+3             |
     * | F4         | 80..8F              | 7=3+4             |
     * +------------+---------------------+-------------------+
     */
    __m128i shift1, pos, _range;
    /* shift1 = (input, prev.input) << 1 byte */
    shift1 = _mm_alignr_epi8(input, prev->input, 15);
    /*
     * shift1:  | EF  F0 ... FE | FF  00  ... ... ... ...  DE | DF  E0 ... EE |
     * pos:     | 0   1      15 | 16  17                   239| 240 241    255|
     * pos-240: | 0   0      0  | 0   0                    0  | 0   1      15 |
     * pos+112: | 112 113    127|           >= 128            |     >= 128    |
     */
    pos = _mm_sub_epi8(shift1, _mm_set1_epi8(0xEF));
    /* E0: +2; ED: +3 */
    /* 0~15 -> 112~127(bit[3:0]=0~15), others -> above 127(bit[7]=1) */
    tmp = _mm_subs_epu8(pos, _mm_set1_epi8(240));
    _range = _mm_shuffle_epi8(tables[4], tmp);
    /* F0: +3; F4: +4 */
    /* 240~255 -> 0~15, others -> 0 */
    tmp = _mm_adds_epu8(pos, _mm_set1_epi8(112));
    _range = _mm_add_epi8(_range, _mm_shuffle_epi8(tables[5], tmp));

    range = _mm_add_epi8(range, _range);

    /* Check value range */
    __m128i minv = _mm_shuffle_epi8(tables[2], range);
    __m128i maxv = _mm_shuffle_epi8(tables[3], range);

    /* error |= ((input < min) | (input > max)) */
    error = _mm_or_si128(error, _mm_cmplt_epi8(input, minv));
    error = _mm_or_si128(error, _mm_cmpgt_epi8(input, maxv));

    prev->input = input;
    prev->follow_bytes = follow_bytes;

    return error;
}

int utf8_range(const unsigned char *data, int len)
{
    if (len >= 16) {
        struct previous_input previous_input;

        previous_input.input = _mm_set1_epi8(0);
        previous_input.follow_bytes = _mm_set1_epi8(0);

        /* Cached constant tables */
        __m128i tables[6];

        tables[0] = _mm_lddqu_si128((const __m128i *)_follow_tbl);
        tables[1] = _mm_lddqu_si128((const __m128i *)_follow_mask_tbl);
        tables[2] = _mm_lddqu_si128((const __m128i *)_range_min_tbl);
        tables[3] = _mm_lddqu_si128((const __m128i *)_range_max_tbl);
        tables[4] = _mm_lddqu_si128((const __m128i *)_df_ee_tbl);
        tables[5] = _mm_lddqu_si128((const __m128i *)_ef_fe_tbl);

        __m128i error = _mm_set1_epi8(0);

        while (len >= 16) {
            error = validate(data, error, &previous_input, tables);

            data += 16;
            len -= 16;
        }

        /* Delay error check till loop ends */
        /* Reduce error vector, error_reduced = 0xFFFF if error == 0 */
        int error_reduced =
            _mm_movemask_epi8(_mm_cmpeq_epi8(error, _mm_set1_epi8(0)));
        if (error_reduced != 0xFFFF)
            return 0;

        /* Find previous token (not 80~BF) */
        int32_t token4 = _mm_extract_epi32(previous_input.input, 3);

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

#endif
