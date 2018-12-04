#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

#include <sys/time.h>

static inline int ascii_std(const uint8_t *data, int len)
{
    return !std::any_of(data, data+len, [] (int8_t b) { return b < 0; });
}

static inline int ascii_u64(const uint8_t *data, int len)
{
    uint8_t orall = 0;

    if (len >= 16) {
        union {
            uint64_t u64;
            uint32_t u32[2];
            uint16_t u16[4];
            uint8_t u8[8];
        } orr;

        uint64_t or1 = 0, or2 = 0;
        const uint8_t *data2 = data+8;

        while (len >= 16) {
            or1 |= *(const uint64_t *)data;
            or2 |= *(const uint64_t *)data2;
            data += 16;
            data2 += 16;
            len -= 16;
        }

        orr.u64 = or1 | or2;
        orr.u32[0] |= orr.u32[1];
        orr.u16[0] |= orr.u16[1];
        orall = orr.u8[0] | orr.u8[1];
    }

    while (len--)
        orall |= *data++;

    return orall < 0x80;
}

#if defined(__x86_64__)
#include <x86intrin.h>

static inline int ascii_simd(const uint8_t *data, int len)
{
    if (len >= 32) {
        const uint8_t *data2 = data+16;

        __m128i or1 = _mm_set1_epi8(0), or2 = or1;

        while (len >= 32) {
            __m128i input1 = _mm_lddqu_si128((const __m128i *)data);
            __m128i input2 = _mm_lddqu_si128((const __m128i *)data2);

            or1 = _mm_or_si128(or1, input1);
            or2 = _mm_or_si128(or2, input2);

            data += 32;
            data2 += 32;
            len -= 32;
        }

        or1 = _mm_or_si128(or1, or2);
        if (_mm_movemask_epi8(_mm_cmplt_epi8(or1, _mm_set1_epi8(0))))
            return 0;
    }

    return ascii_u64(data, len);
}

#elif defined(__aarch64__)
#include <arm_neon.h>

static inline int ascii_simd(const uint8_t *data, int len)
{
    if (len >= 32) {
        const uint8_t *data2 = data+16;

        uint8x16_t or1 = vdupq_n_u8(0), or2 = or1;

        while (len >= 32) {
            const uint8x16_t input1 = vld1q_u8(data);
            const uint8x16_t input2 = vld1q_u8(data2);

            or1 = vorrq_u8(or1, input1);
            or2 = vorrq_u8(or2, input2);

            data += 32;
            data2 += 32;
            len -= 32;
        }

        or1 = vorrq_u8(or1, or2);
        if (vmaxvq_u8(or1) >= 0x80)
            return 0;
    }

    return ascii_u64(data, len);
}

#endif

struct ftab {
    const char *name;
    int (*func)(const uint8_t *data, int len);
};

static const std::vector<ftab> _f = {
    {
        .name = "std",
        .func = ascii_std,
    }, {
        .name = "u64",
        .func = ascii_u64,
    }, {
        .name = "simd",
        .func = ascii_simd,
    },
};

static void load_test_buf(uint8_t *data, int len)
{
    uint8_t v = 0;

    for (int i = 0; i < len; ++i) {
        data[i] = v++;
        v &= 0x7F;
    }
}

static void bench(const struct ftab &f, const uint8_t *data, int len)
{
    const int loops = 1024*1024*1024/len;
    int ret = 1;
    double time, size;
    struct timeval tv1, tv2;

    fprintf(stderr, "bench %s (%d bytes)... ", f.name, len);
    gettimeofday(&tv1, 0);
    for (int i = 0; i < loops; ++i)
        ret &= f.func(data, len);
    gettimeofday(&tv2, 0);
    printf("%s ", ret?"pass":"FAIL");

    time = tv2.tv_usec - tv1.tv_usec;
    time = time / 1000000 + tv2.tv_sec - tv1.tv_sec;
    size = ((double)len * loops) / (1024*1024);
    printf("%.2f MB/s\n", size / time);
}

static void test(const struct ftab &f, uint8_t *data, int len)
{
    int error = 0;

    fprintf(stderr, "test %s (%d bytes)... ", f.name, len);

    /* positive */
    error |= !f.func(data, len);

    /* negative */
    for (int i = 0; i < len; ++i) {
        data[i] += 0x80;
        error |= f.func(data, len);
        data[i] -= 0x80;
    }

    printf("%s\n", error ? "FAIL" : "pass");
}

/* ./ascii [test|bench] [alg] */
int main(int argc, const char *argv[])
{
    int do_test = 1, do_bench = 1;
    const char *alg = NULL;

    if (argc > 1) {
        do_bench &= !!strcmp(argv[1], "test");
        do_test &= !!strcmp(argv[1], "bench");
    }

    if (do_bench && argc > 2)
        alg = argv[2];

    const std::vector<int> size = { 9, 16+1, 32-1, 128+1, 1024+15,
                                    16*1024+1, 64*1024+15 };

    int max_size = *std::max_element(size.begin(), size.end());
    uint8_t *_data = new uint8_t[max_size+1];
    uint8_t *data = _data+1;    /* Unalign buffer address */

    load_test_buf(data, max_size);

    if (do_test) {
        printf("==================== Test ====================\n");
        for (int sz : size) {
            for (auto &f : _f) {
                test(f, data, sz);
            }
        }
    }

    if (do_bench) {
        printf("==================== Bench ====================\n");
        for (int sz : size) {
            for (auto &f : _f) {
                if (!alg || strcmp(alg, f.name) == 0)
                    bench(f, data, sz);
            }
            printf("-----------------------------------------------\n");
        }
    }

    delete _data;
    return 0;
}
