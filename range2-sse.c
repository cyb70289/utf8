#ifdef __x86_64__

#include <stdio.h>
#include <stdint.h>
#include <x86intrin.h>

int utf8_naive(const unsigned char *data, int len);

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

/* Get number of followup bytes to take care per high nibble */
static inline __m128i get_followup_bytes(
        const __m128i input, const __m128i follow_table,
        __m128i *mask, const __m128i mask_table)
{
    /* Why no _mm_srli_epi8 ? */
    const __m128i high_nibbles =
        _mm_and_si128(_mm_srli_epi16(input, 4), _mm_set1_epi8(0x0F));

    *mask = _mm_shuffle_epi8(mask_table, high_nibbles);
    return _mm_shuffle_epi8(follow_table, high_nibbles);
}

static inline __m128i validate(const unsigned char *data, __m128i error,
       struct previous_input *prev, const __m128i tables[])
{
    __m128i range1;

    const __m128i input1 = _mm_lddqu_si128((const __m128i *)data);

    /* range is 8 if input=0xC0~0xFF, overlap will lead to 9, 10, 11 */
    __m128i follow_bytes1 =
        get_followup_bytes(input1, tables[0], &range1, tables[1]);

    /* 2nd byte */
    /* range = (follow_bytes, prev.follow_bytes) << 1 byte */
    range1 = _mm_or_si128(
            range1, _mm_alignr_epi8(follow_bytes1, prev->follow_bytes, 15));

    /* 3rd bytes */
    __m128i subp, sub1_3, sub1_4;
    /* saturate sub 1 */
    subp = _mm_subs_epu8(prev->follow_bytes, _mm_set1_epi8(1));
    sub1_3 = _mm_subs_epu8(follow_bytes1, _mm_set1_epi8(1));
    /* range1 |= (sub1, subp) << 2 bytes */
    range1 = _mm_or_si128(range1, _mm_alignr_epi8(sub1_3, subp, 14));

    /* 4th bytes */
    /* saturate sub 2 */
    subp = _mm_subs_epu8(prev->follow_bytes, _mm_set1_epi8(2));
    sub1_4 = _mm_subs_epu8(follow_bytes1, _mm_set1_epi8(2));
    /* range1 |= (sub1, subp) << 3 bytes */
    range1 = _mm_or_si128(range1, _mm_alignr_epi8(sub1_4, subp, 13));

    /*
     * Check special cases (not 80..BF)
     * +------------+---------------------+-------------------+
     * | First Byte | Special Second Byte | range table index |
     * +------------+---------------------+-------------------+
     * | E0         | A0..BF              | 4                 |
     * | ED         | 80..9F              | 5                 |
     * | F0         | 90..BF              | 6                 |
     * | F4         | 80..8F              | 7                 |
     * +------------+---------------------+-------------------+
     */
    __m128i shift1, pos, _range, tmp;

    /* shift1 = (input1, prev.input) << 1 byte */
    shift1 = _mm_alignr_epi8(input1, prev->input, 15);
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

    range1 = _mm_add_epi8(range1, _range);

    /* Check value range */
    __m128i minv = _mm_shuffle_epi8(tables[2], range1);
    __m128i maxv = _mm_shuffle_epi8(tables[3], range1);

    /* error |= ((input < min) | (input > max)) */
    error = _mm_or_si128(error, _mm_cmplt_epi8(input1, minv));
    error = _mm_or_si128(error, _mm_cmpgt_epi8(input1, maxv));

    /*===============================================================*/
    __m128i range2, sub2;

    const __m128i input2 = _mm_lddqu_si128((const __m128i *)(data+16));

    __m128i follow_bytes2 =
        get_followup_bytes(input2, tables[0], &range2, tables[1]);

    range2 = _mm_or_si128(
            range2, _mm_alignr_epi8(follow_bytes2, follow_bytes1, 15));

    sub2 = _mm_subs_epu8(follow_bytes2, _mm_set1_epi8(1));
    range2 = _mm_or_si128(range2, _mm_alignr_epi8(sub2, sub1_3, 14));

    sub2 = _mm_subs_epu8(follow_bytes2, _mm_set1_epi8(2));
    range2 = _mm_or_si128(range2, _mm_alignr_epi8(sub2, sub1_4, 13));

    shift1 = _mm_alignr_epi8(input2, input1, 15);
    pos = _mm_sub_epi8(shift1, _mm_set1_epi8(0xEF));
    tmp = _mm_subs_epu8(pos, _mm_set1_epi8(240));
    _range = _mm_shuffle_epi8(tables[4], tmp);
    tmp = _mm_adds_epu8(pos, _mm_set1_epi8(112));
    _range = _mm_add_epi8(_range, _mm_shuffle_epi8(tables[5], tmp));

    range2 = _mm_add_epi8(range2, _range);

    minv = _mm_shuffle_epi8(tables[2], range2);
    maxv = _mm_shuffle_epi8(tables[3], range2);

    error = _mm_or_si128(error, _mm_cmplt_epi8(input2, minv));
    error = _mm_or_si128(error, _mm_cmpgt_epi8(input2, maxv));
    /*===============================================================*/

    prev->input = input2;
    prev->follow_bytes = follow_bytes2;

    return error;
}

int utf8_range2(const unsigned char *data, int len)
{
    if (len >= 32) {
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

        while (len >= 32) {
            error = validate(data, error, &previous_input, tables);

            data += 32;
            len -= 32;
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
