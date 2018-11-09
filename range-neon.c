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

static const uint8_t _followup_tbl[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 00 ~ BF */
    1, 1,                               /* C0 ~ DF */
    2,                                  /* E0 ~ EF */
    3,                                  /* F0 ~ FF */
};

static const uint8_t _range_tbl[] = {
    /* 0,    1,    2,    3,    4,    5,    6,    7 */
    0x00, 0x80, 0xA0, 0x80, 0x90, 0x80, 0xC2, 0x80, /* min */
    0x7F, 0xBF, 0xBF, 0x9F, 0xBF, 0x8F, 0xF4, 0x7F, /* max */
};

/* Get number of followup bytes to take care per high nibble */
static inline uint8x16_t get_followup_bytes(const uint8x16_t input)
{
    const uint8x16_t high_nibbles = vshrq_n_u8(input, 4);
    const uint8x16_t followup_table = vld1q_u8(_followup_tbl);

    return vqtbl1q_u8(followup_table, high_nibbles);
}

static inline uint8x16_t validate(const unsigned char *data, uint8_t *error,
                                  uint8x16_t range0)
{
    const uint8x16_t input = vld1q_u8(data);

    const uint8x16_t zero = vdupq_n_u8(0);
    const uint8x16_t one = vdupq_n_u8(1);

    uint8x16_t fi0 = get_followup_bytes(input);
    uint8x16_t fi1 = vdupq_n_u8(0);
    uint8x16_t mask0 = vcgtq_u8(fi0, zero);    /* fi0 > 0 ? 0xFF: 0 */
    uint8x16_t mask1;
    uint8x16_t errors = vandq_u8(mask0, range0);
    uint8x16_t range1 = vdupq_n_u8(0);

    /* range0 |= mask & 6 */
    range0 = vorrq_u8(range0, vandq_u8(mask0, vdupq_n_u8(6)));

    const int max_followup_bytes = vmaxvq_u8(fi0);

    if (max_followup_bytes) {
        /* Add range 80~BF to followup bytes */
        for (int i = 0; i < max_followup_bytes; ++i) {
            /* (fi1, fi0) <<= 8 */
            fi1 = vextq_u8(fi0, fi1, 15);
            fi0 = vextq_u8(fi1, fi0, 15);   /* high bit of fi1 must be 0 */

            /* mask = (fi > 0 ? 0xFF : 0) */
            mask1 = vcgtq_u8(fi1, zero);
            mask0 = vcgtq_u8(fi0, zero);

            /* overlap: errors |= (mask0 & range0) */
            errors = vorrq_u8(errors, vandq_u8(mask0, range0));

            /* range += (mask & 1) */
            range1 = vaddq_u8(range1, vandq_u8(mask1, one));
            range0 = vaddq_u8(range0, vandq_u8(mask0, one));

            /* fi = (fi >= 1 ? fi-1 : 0) */
            fi1 = vqsubq_u8(fi1, one);
            fi0 = vqsubq_u8(fi0, one);
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
        mask1 = vceqq_u8(input, vdupq_n_u8(0xE0));
        mask0 = vandq_u8(mask1, one);
        /* mask0 += (input == 0xED) & 2 */
        mask1 = vceqq_u8(input, vdupq_n_u8(0xED));
        mask0 = vaddq_u8(mask0, vandq_u8(mask1, vdupq_n_u8(2)));
        /* mask0 += (input == 0xF0) & 3 */
        mask1 = vceqq_u8(input, vdupq_n_u8(0xF0));
        mask0 = vaddq_u8(mask0, vandq_u8(mask1, vdupq_n_u8(3)));
        /* mask0 += (input == 0xF4) & 4 */
        mask1 = vceqq_u8(input, vdupq_n_u8(0xF4));
        mask0 = vaddq_u8(mask0, vandq_u8(mask1, vdupq_n_u8(4)));

        /* (mask1, mask0) = (0, mask0) << 8 */
        mask1 = vdupq_n_u8(0);
        mask1 = vextq_u8(mask0, mask1, 15);
        mask0 = vextq_u8(mask1, mask0, 15); /* high bit of mask1 must be 0 */

        /* range += mask */
        range1 = vaddq_u8(range1, mask1);
        range0 = vaddq_u8(range0, mask0);
    }

    /* mask0 = min, mask1 = max */
    fi0 = vld1q_u8(_range_tbl);
    mask0 = vqtbl1q_u8(fi0, range0);
    mask1 = vqtbl1q_u8(fi0, vaddq_u8(range0, vdupq_n_u8(8)));

    /* errors |= ((input < min) | (input > max)) */
    errors = vorrq_u8(errors, vcltq_u8(input, mask0));
    errors = vorrq_u8(errors, vcgtq_u8(input, mask1));

    /* Reduce errors vector */
    *error |= vmaxvq_u8(errors);

    return range1;
}

int utf8_range(const unsigned char *data, int len)
{
    if (len >= 16) {
        uint8_t error = 0;
        uint32_t range3;
        uint8x16_t range = vdupq_n_u8(0);

        while (len >= 16) {
            range = validate(data, &error, range);

            data += 16;
            len -= 16;
        }

        /* Delay error check till loop ends */
        if (error)
            return 0;

        /* At most three followup bytes need to take care */
        vst1q_lane_u32(&range3, vreinterpretq_u32_u8(range), 0);

        while (range3) {
            uint8_t idx = range3 & 7;

            if (len == 0)
                return 0;
            if (*data < _range_tbl[idx] || *data > _range_tbl[idx+8])
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

#endif
