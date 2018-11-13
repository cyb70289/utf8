// Brand new utf8 validation algorithm

#ifdef __aarch64__

#include <stdio.h>
#include <stdint.h>
#include <arm_neon.h>

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
    int        follow_max;
};

static const uint8_t _follow_tbl[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 00 ~ BF */
    1, 1,                               /* C0 ~ DF */
    2,                                  /* E0 ~ EF */
    3,                                  /* F0 ~ FF */
};

static const uint8_t _range_tbl[] = {
    /* 0,    1,    2,    3,    4,    5,    6,    7 */
    0x00, 0x80, 0x80, 0x80, 0xC2, 0xFF, 0xFF, 0xFF, /* min */
    0x7F, 0xBF, 0xBF, 0xBF, 0xF4, 0x00, 0x00, 0x00, /* max */
};

/* Get number of followup bytes to take care per high nibble */
static inline uint8x16_t get_followup_bytes(const uint8x16_t input,
        const uint8x16_t follow_table)
{
    const uint8x16_t high_nibbles = vshrq_n_u8(input, 4);

    return vqtbl1q_u8(follow_table, high_nibbles);
}

static inline uint8x16_t validate(const unsigned char *data, uint8x16_t error,
        struct previous_input *prev, const uint8x16_t follow_table,
        const uint8x16_t range_table)
{
    const uint8x16_t input = vld1q_u8(data);

    const uint8x16_t follow_bytes = get_followup_bytes(input, follow_table);
    const uint8x16_t follow_mask = vcgtq_u8(follow_bytes, vdupq_n_u8(0));
    uint8x16_t prev_follow_bytes, range, tmp;

    const int follow_max = vmaxvq_u8(follow_bytes);

    if (follow_max == 0 && prev->follow_max == 0) {
        /* All ascii */
        prev->input = input;
        return vorrq_u8(error, vcgtq_u8(input, vdupq_n_u8(0x7F)));
    }

    /* 2nd byte */
    /* range = (follow_bytes, prev.follow_bytes) << 1 byte */
    range = vextq_u8(prev->follow_bytes, follow_bytes, 15);

    /* 3rd bytes */
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

#if 1
    /* If token overlaps, range will be 5,6,7 of bad range */
    range = vaddq_u8(range, vandq_u8(follow_mask, vdupq_n_u8(4)));
#else
    /* Check overlap */
    error = vorrq_u8(error, vandq_u8(range, follow_mask));
    /* range = 4 if follow_mask else 0 */
    range = vorrq_u8(range, vandq_u8(follow_mask, vdupq_n_u8(4)));
#endif

    /* Check value range */
    uint8x16_t minv = vqtbl1q_u8(range_table, range);
    uint8x16_t maxv = vqtbl1q_u8(range_table, vaddq_u8(range, vdupq_n_u8(8)));

    /* errors |= ((input < min) | (input > max)) */
    error = vorrq_u8(error, vcltq_u8(input, minv));
    error = vorrq_u8(error, vcgtq_u8(input, maxv));

#if 0
    if (vmaxvq_u8(error)) {
        print128("pinput", prev->input);
        print128("pbytes", prev->follow_bytes);
        print128("input", input);
        print128("range", range);
        exit(1);
    }
#endif

    /*
     * Check special cases (not 80..BF)
     * +------------+---------------------+-------------------+
     * | First Byte | Special Second Byte | range table index |
     * +------------+---------------------+-------------------+
     * | E0         | A0..BF              | 2                 |
     * | ED         | 80..9F              | 3                 |
     * | F0         | 90..BF              | 4                 |
     * | F4         | 80..8F              | 5                 |
     * +------------+---------------------+-------------------+
     */
    /* tmp = (input, prev.input) << 1 byte */
    tmp = vextq_u8(prev->input, input, 15);
    /* tmp == 0xE0 && input < 0xA0 */
    error = vorrq_u8(error,
                     vandq_u8(vceqq_u8(tmp, vdupq_n_u8(0xE0)),
                              vcltq_u8(input, vdupq_n_u8(0xA0))));
    /* tmp == 0xED && input > 0x9F */
    error = vorrq_u8(error,
                     vandq_u8(vceqq_u8(tmp, vdupq_n_u8(0xED)),
                              vcgtq_u8(input, vdupq_n_u8(0x9F))));
    /* tmp == 0xF0 && input < 0x90 */
    error = vorrq_u8(error,
                     vandq_u8(vceqq_u8(tmp, vdupq_n_u8(0xF0)),
                              vcltq_u8(input, vdupq_n_u8(0x90))));
    /* tmp == 0xF4 && input > 0x8F */
    error = vorrq_u8(error,
                     vandq_u8(vceqq_u8(tmp, vdupq_n_u8(0xF4)),
                              vcgtq_u8(input, vdupq_n_u8(0x8F))));

    prev->input = input;
    prev->follow_bytes = follow_bytes;
    prev->follow_max = follow_max;

    return error;
}

int utf8_range(const unsigned char *data, int len)
{
    if (len >= 16) {
        struct previous_input previous_input;

        previous_input.input = vdupq_n_u8(0);
        previous_input.follow_bytes = vdupq_n_u8(0);
        previous_input.follow_max = 0;

        const uint8x16_t follow_table = vld1q_u8(_follow_tbl);
        const uint8x16_t range_table = vld1q_u8(_range_tbl);

        uint8x16_t error = vdupq_n_u8(0);

        while (len >= 16) {
            error = validate(data, error, &previous_input,
                    follow_table, range_table);

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
