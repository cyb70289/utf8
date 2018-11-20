#ifdef __aarch64__

#include <stdio.h>
#include <stdint.h>
#include <arm_neon.h>

int utf8_naive(const unsigned char *data, int len);

struct previous_input {
    uint8x16_t input;
    uint8x16_t follow_bytes;
};

static const uint8_t _follow_tbl[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 00 ~ BF */
    1, 1,                               /* C0 ~ DF */
    2,                                  /* E0 ~ EF */
    3,                                  /* F0 ~ FF */
};

static const uint8_t _follow_mask_tbl[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 00 ~ BF */
    8, 8,                               /* C0 ~ DF */
    8,                                  /* E0 ~ EF */
    8,                                  /* F0 ~ FF */
};

static const uint8_t _range_min_tbl[] = {
    /* 0,    1,    2,    3,    4,    5,    6,    7,    8 */
    0x00, 0x80, 0x80, 0x80, 0xA0, 0x80, 0x90, 0x80, 0xC2,
    /* Must be invalid */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

static const uint8_t _range_max_tbl[] = {
    /* 0,    1,    2,    3,    4,    5,    6,    7,    8 */
    0x7F, 0xBF, 0xBF, 0xBF, 0xBF, 0x9F, 0xBF, 0x8F, 0xF4,
    /* Must be invalid */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* Get number of followup bytes to take care per high nibble */
static inline uint8x16_t get_followup_bytes(
        const uint8x16_t input, const uint8x16_t follow_table,
        uint8x16_t *mask, const uint8x16_t mask_table)
{
    const uint8x16_t high_nibbles = vshrq_n_u8(input, 4);

    *mask = vqtbl1q_u8(mask_table, high_nibbles);
    return vqtbl1q_u8(follow_table, high_nibbles);
}

static inline uint8x16_t validate(const unsigned char *data, uint8x16_t error,
        struct previous_input *prev, const uint8x16_t tables[])
{
    uint8x16_t range1, range2;

    const uint8x16_t input1 = vld1q_u8(data);
    const uint8x16_t input2 = vld1q_u8(data+16);

    /* range is 8 if input=0xC0~0xFF, overlap will lead to 9, 10, 11 */
    const uint8x16_t follow_bytes1 =
        get_followup_bytes(input1, tables[0], &range1, tables[1]);
    const uint8x16_t follow_bytes2 =
        get_followup_bytes(input2, tables[0], &range2, tables[1]);

    /* 2nd byte */
    /* range |= (follow_bytes, prev.follow_bytes) << 1 byte */
    range1 = vorrq_u8(range1, vextq_u8(prev->follow_bytes, follow_bytes1, 15));
    range2 = vorrq_u8(range2, vextq_u8(follow_bytes1, follow_bytes2, 15));

    /* 3rd bytes */
    uint8x16_t subp, sub1, sub2;
    /* saturate sub 1 */
    subp = vqsubq_u8(prev->follow_bytes, vdupq_n_u8(1));
    sub1 = vqsubq_u8(follow_bytes1, vdupq_n_u8(1));
    sub2 = vqsubq_u8(follow_bytes2, vdupq_n_u8(1));
    /* range1 |= (sub1, subp) << 2 bytes */
    range1 = vorrq_u8(range1, vextq_u8(subp, sub1, 14));
    /* range2 |= (sub2, sub1) << 2 bytes */
    range2 = vorrq_u8(range2, vextq_u8(sub1, sub2, 14));

    /* 4th bytes */
    /* saturate sub 2 */
    subp = vqsubq_u8(prev->follow_bytes, vdupq_n_u8(2));
    sub1 = vqsubq_u8(follow_bytes1, vdupq_n_u8(2));
    sub2 = vqsubq_u8(follow_bytes2, vdupq_n_u8(2));
    /* range1 |= (sub1, subp) << 3 bytes */
    range1 = vorrq_u8(range1, vextq_u8(subp, sub1, 13));
    /* range2 |= (sub2, sub1) << 3 bytes */
    range2 = vorrq_u8(range2, vextq_u8(sub1, sub2, 13));

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
    uint8x16_t shift1, pos;

    /* shift1 = (input1, prev.input) << 1 byte */
    shift1 = vextq_u8(prev->input, input1, 15);
    pos = vceqq_u8(shift1, vdupq_n_u8(0xE0));
    range1 = vaddq_u8(range1, vandq_u8(pos, vdupq_n_u8(2)));  /* 2+2 */
    pos = vceqq_u8(shift1, vdupq_n_u8(0xED));
    range1 = vaddq_u8(range1, vandq_u8(pos, vdupq_n_u8(3)));  /* 2+3 */
    pos = vceqq_u8(shift1, vdupq_n_u8(0xF0));
    range1 = vaddq_u8(range1, vandq_u8(pos, vdupq_n_u8(3)));  /* 3+3 */
    pos = vceqq_u8(shift1, vdupq_n_u8(0xF4));
    range1 = vaddq_u8(range1, vandq_u8(pos, vdupq_n_u8(4)));  /* 3+4 */

    /* shift1 = (input2, input1) << 1 byte */
    shift1 = vextq_u8(input1, input2, 15);
    pos = vceqq_u8(shift1, vdupq_n_u8(0xE0));
    range2 = vaddq_u8(range2, vandq_u8(pos, vdupq_n_u8(2)));  /* 2+2 */
    pos = vceqq_u8(shift1, vdupq_n_u8(0xED));
    range2 = vaddq_u8(range2, vandq_u8(pos, vdupq_n_u8(3)));  /* 2+3 */
    pos = vceqq_u8(shift1, vdupq_n_u8(0xF0));
    range2 = vaddq_u8(range2, vandq_u8(pos, vdupq_n_u8(3)));  /* 3+3 */
    pos = vceqq_u8(shift1, vdupq_n_u8(0xF4));
    range2 = vaddq_u8(range2, vandq_u8(pos, vdupq_n_u8(4)));  /* 3+4 */

    /* Check value range */
    uint8x16_t minv1 = vqtbl1q_u8(tables[2], range1);
    uint8x16_t maxv1 = vqtbl1q_u8(tables[3], range1);
    uint8x16_t minv2 = vqtbl1q_u8(tables[2], range2);
    uint8x16_t maxv2 = vqtbl1q_u8(tables[3], range2);

    /* errors |= ((input < min) | (input > max)) */
    error = vorrq_u8(error, vcltq_u8(input1, minv1));
    error = vorrq_u8(error, vcgtq_u8(input1, maxv1));
    error = vorrq_u8(error, vcltq_u8(input2, minv2));
    error = vorrq_u8(error, vcgtq_u8(input2, maxv2));

    prev->input = input2;
    prev->follow_bytes = follow_bytes2;

    return error;
}

int utf8_range2(const unsigned char *data, int len)
{
    if (len >= 32) {
        struct previous_input previous_input;

        previous_input.input = vdupq_n_u8(0);
        previous_input.follow_bytes = vdupq_n_u8(0);

        /* Cached constant tables */
        uint8x16_t tables[4];

        tables[0] = vld1q_u8(_follow_tbl);
        tables[1] = vld1q_u8(_follow_mask_tbl);
        tables[2] = vld1q_u8(_range_min_tbl);
        tables[3] = vld1q_u8(_range_max_tbl);

        uint8x16_t error = vdupq_n_u8(0);

        while (len >= 32) {
            error = validate(data, error, &previous_input, tables);

            data += 32;
            len -= 32;
        }

        /* Delay error check till loop ends */
        if (vmaxvq_u8(error))
            return 0;

        /* Find previous token (not 80~BF) */
        uint32_t token4;
        vst1q_lane_u32(&token4, vreinterpretq_u32_u8(previous_input.input), 3);

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
