#ifdef __aarch64__

#include <stdio.h>
#include <stdint.h>
#include <arm_neon.h>

int utf8_naive(const unsigned char *data, int len);

#if 0
static void print128(const char *s, const uint8x16_t v128)
{
    unsigned char v8[16];
    vst1q_u8(v8, v128);

    if (s)
        printf("%s:\t", s);
    for (int i = 0; i < 16; ++i)
        printf("%02x ", v8[i]);
    printf("\n");
}
#endif

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

/* E0: 2, ED: 3 */
static const uint8_t _e0_tbl[] = {
    2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0,
};

/* F0: 3; F4: 4 */
static const uint8_t _f0_tbl[] = {
    3, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static inline uint8x16_t validate(const unsigned char *data, uint8x16_t error,
        struct previous_input *prev, const uint8x16_t tables[])
{
    const uint8x16_t input = vld1q_u8(data);

    const uint8x16_t high_nibbles = vshrq_n_u8(input, 4);
    const uint8x16_t follow_bytes = vqtbl1q_u8(tables[0], high_nibbles);

    /* range is 8 if input=0xC0~0xFF, overlap will lead to 9, 10, 11 */
    uint8x16_t range = vqtbl1q_u8(tables[1], high_nibbles);

    /* 2nd byte */
    /* range |= (follow_bytes, prev.follow_bytes) << 1 byte */
    range = vorrq_u8(range, vextq_u8(prev->follow_bytes, follow_bytes, 15));

    /* 3rd bytes */
    uint8x16_t tmp, prev_follow_bytes;
    /* saturate sub 1 */
    tmp = vqsubq_u8(follow_bytes, vdupq_n_u8(1));
    prev_follow_bytes = vqsubq_u8(prev->follow_bytes, vdupq_n_u8(1));
    /* range |= (tmp, prev_follow_bytes) << 2 bytes */
    tmp = vextq_u8(prev_follow_bytes, tmp, 14);
    range = vorrq_u8(range, tmp);

    /* 4th bytes */
    /* saturate sub 2 */
    tmp = vqsubq_u8(follow_bytes, vdupq_n_u8(2));
    prev_follow_bytes = vqsubq_u8(prev->follow_bytes, vdupq_n_u8(2));
    /* range |= (tmp, prev_follow_bytes) << 3 bytes */
    tmp = vextq_u8(prev_follow_bytes, tmp, 13);
    range = vorrq_u8(range, tmp);

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
    uint8x16_t shift1, pos;
    /* shift1 = (input, prev.input) << 1 byte */
    shift1 = vextq_u8(prev->input, input, 15);
    /* E0: +2; ED: +3 */
    pos = vsubq_u8(shift1, vdupq_n_u8(0xE0));
    range = vaddq_u8(range, vqtbl1q_u8(tables[4], pos));
    /* F0: +3; F4: +4 */
    pos = vsubq_u8(shift1, vdupq_n_u8(0xF0));
    range = vaddq_u8(range, vqtbl1q_u8(tables[5], pos));

    /* Check value range */
    uint8x16_t minv = vqtbl1q_u8(tables[2], range);
    uint8x16_t maxv = vqtbl1q_u8(tables[3], range);

    /* errors |= ((input < min) | (input > max)) */
    error = vorrq_u8(error, vcltq_u8(input, minv));
    error = vorrq_u8(error, vcgtq_u8(input, maxv));

    prev->input = input;
    prev->follow_bytes = follow_bytes;

    return error;
}

int utf8_range(const unsigned char *data, int len)
{
    if (len >= 16) {
        struct previous_input previous_input;

        previous_input.input = vdupq_n_u8(0);
        previous_input.follow_bytes = vdupq_n_u8(0);

        /* Cached constant tables */
        uint8x16_t tables[6];

        tables[0] = vld1q_u8(_follow_tbl);
        tables[1] = vld1q_u8(_follow_mask_tbl);
        tables[2] = vld1q_u8(_range_min_tbl);
        tables[3] = vld1q_u8(_range_max_tbl);
        tables[4] = vld1q_u8(_e0_tbl);
        tables[5] = vld1q_u8(_f0_tbl);

        uint8x16_t error = vdupq_n_u8(0);

        while (len >= 16) {
            error = validate(data, error, &previous_input, tables);

            data += 16;
            len -= 16;
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

#ifdef DEBUG
int main(void)
{
    /* negative:
     *
     *  "\x00\x00\x00\x00\x00\xc2\x80\x00\x00\x00\xe1\x80\x80\x00\x00\xc2" \
     *  "\xc2\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00";
     *
     *  "\x00\x00\x00\x00\x00\xc2\xc2\x80\x00\x00\xe1\x80\x80\x00\x00\x00";
     */
    const unsigned char src[] =
        "\x00\x66\x7F\x00\x7F\x7F\x00\xC2\x80\xDF\xBF\xE0\xA0\x80\xE0\xA0\xBF";
    int ret = utf8_range(src, sizeof(src)-1);
    printf("%s\n", ret ? "ok": "bad");

    return 0;
}
#endif

#endif
