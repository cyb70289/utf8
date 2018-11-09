#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>

int utf8_naive(const unsigned char *data, int len);
int utf8_lookup(const unsigned char *data, int len);
int utf8_boost(const unsigned char *data, int len);
int utf8_lemire(const unsigned char *data, int len);
int utf8_range(const unsigned char *data, int len);

static struct ftab {
    const char *name;
    int (*func)(const unsigned char *data, int len);
} ftab[] = {
    {
        .name = "naive",
        .func = utf8_naive,
    },
    {
        .name = "lookup",
        .func = utf8_lookup,
    },
    {
        .name = "lemire",
        .func = utf8_lemire,
    },
    {
        .name = "range",
        .func = utf8_range,
    },
#ifdef BOOST
    {
        .name = "boost",
        .func = utf8_boost,
    },
#endif
};

static unsigned char *load_test_file(int *len)
{
    unsigned char *data;
    int fd;
    struct stat stat;

    fd = open("./UTF-8-demo.txt", O_RDONLY);
    if (fd == -1) {
        printf("Failed to open UTF-8-demo.txt!\n");
        exit(1);
    }
    if (fstat(fd, &stat) == -1) {
        printf("Failed to get file size!\n");
        exit(1);
    }

    *len = stat.st_size;
    data = malloc(*len);
    if (read(fd, data, *len) != *len) {
        printf("Failed to read file!\n");
        exit(1);
    }

    close(fd);

    return data;
}

static void print_test(const unsigned char *data, int len)
{
    while (len--)
        printf("\\x%02X", *data++);

    printf("\n");
}

struct test {
    const unsigned char *data;
    int len;
};

static void prepare_test_buf(unsigned char *buf, const struct test *pos,
                             int pos_len, int pos_idx)
{
    /* Round concatenate correct tokens to 1024 bytes */
    int buf_idx = 0;
    while (buf_idx < 1024) {
        int buf_len = 1024 - buf_idx;

        if (buf_len >= pos[pos_idx].len) {
            memcpy(buf+buf_idx, pos[pos_idx].data, pos[pos_idx].len);
            buf_idx += pos[pos_idx].len;
        } else {
            memset(buf+buf_idx, 0, buf_len);
            buf_idx += buf_len;
        }

        if (++pos_idx == pos_len)
            pos_idx = 0;
    }
}

static int test_manual(const struct ftab *ftab)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-sign"
    /* positive tests */
    static const struct test pos[] = {
        {"", 0},
        {"\x00", 1},
        {"\x66", 1},
        {"\x7F", 1},
        {"\x00\x7F", 2},
        {"\x7F\x00", 2},
        {"\xC2\x80", 2},
        {"\xDF\xBF", 2},
        {"\xE0\xA0\x80", 3},
        {"\xE0\xA0\xBF", 3},
        {"\xED\x9F\x80", 3},
        {"\xEF\x80\xBF", 3},
        {"\xF0\x90\xBF\x80", 4},
        {"\xF2\x81\xBE\x99", 4},
        {"\xF4\x8F\x88\xAA", 4},
    };

    /* negative tests */
    static const struct test neg[] = {
        {"\x80", 1},
        {"\xBF", 1},
        {"\xC0\x80", 2},
        {"\xC1\x00", 2},
        {"\xC2\x7F", 2},
        {"\xDF\xC0", 2},
        {"\xE0\x9F\x80", 3},
        {"\xE0\xC2\x80", 3},
        {"\xED\xA0\x80", 3},
        {"\xED\x7F\x80", 3},
        {"\xEF\x80\x00", 3},
        {"\xF0\x8F\x80\x80", 4},
        {"\xF0\xEE\x80\x80", 4},
        {"\xF2\x90\x91\x7F", 4},
        {"\xF4\x90\x88\xAA", 4},
        {"\xF4\x00\xBF\xBF", 4},
    };
#pragma GCC diagnostic push

    /* Test single token */
    for (int i = 0; i < sizeof(pos)/sizeof(pos[0]); ++i) {
        if (ftab->func(pos[i].data, pos[i].len) == 0) {
            printf("FAILED positive test: ");
            print_test(pos[i].data, pos[i].len);
            return 0;
        }
    }
    for (int i = 0; i < sizeof(neg)/sizeof(neg[0]); ++i) {
        if (ftab->func(neg[i].data, neg[i].len) != 0) {
            printf("FAILED negitive test: ");
            print_test(neg[i].data, neg[i].len);
            return 0;
        }
    }

    /* Test shifted buffer to cover 1k length */
    const int max_size = 1024+32;
    uint64_t buf64[max_size/8 + 2];
    /* Offset 8 bytes by 1 byte */
    unsigned char *buf = ((unsigned char *)buf64) + 1;
    int buf_len;

    for (int i = 0; i < sizeof(pos)/sizeof(pos[0]); ++i) {
        /* Positive test: shift 16 bytes, validate each shift */
        prepare_test_buf(buf, pos, sizeof(pos)/sizeof(pos[0]), i);
        buf_len = 1024;
        for (int j = 0; j < 16; ++j) {
            if (ftab->func(buf, buf_len) == 0) {
                printf("FAILED positive test: ");
                print_test(buf, buf_len);
                return 0;
            }
            for (int k = buf_len; k >= 1; --k)
                buf[k] = buf[k-1];
            buf[0] = '\x55';
            ++buf_len;
        }

        /* Negative test: trunk last non ascii */
        while (buf_len >= 1 && buf[buf_len-1] <= 0x7F)
            --buf_len;
        if (buf_len && ftab->func(buf, buf_len-1) != 0) {
            printf("FAILED negitive test: ");
            print_test(buf, buf_len);
            return 0;
        }
    }

    /* Negative test */
    for (int i = 0; i < sizeof(neg)/sizeof(neg[0]); ++i) {
        /* Append one error token, shift 16 bytes, validate each shift */
        int pos_idx = i % (sizeof(pos)/sizeof(pos[0]));
        prepare_test_buf(buf, pos, sizeof(pos)/sizeof(pos[0]), pos_idx);
        memcpy(buf+1024, neg[i].data, neg[i].len);
        buf_len = 1024 + neg[i].len;
        for (int j = 0; j < 16; ++j) {
            if (ftab->func(buf, buf_len) != 0) {
                printf("FAILED negative test: ");
                print_test(buf, buf_len);
                return 0;
            }
            for (int k = buf_len; k >= 1; --k)
                buf[k] = buf[k-1];
            buf[0] = '\x66';
            ++buf_len;
        }
    }

    return 1;
}

static void test(const unsigned char *data, int len, const struct ftab *ftab)
{
    printf("%s\n", ftab->name);
    printf("standard test: %s\n", ftab->func(data, len) ? "pass" : "FAIL");

    printf("manual test: %s\n", test_manual(ftab) ? "pass" : "FAIL");
}

static void bench(const unsigned char *data, int len, const struct ftab *ftab)
{
    const int loops = 1024*1024*1024/len;
    int ret = 1;
    double time, size;
    struct timeval tv1, tv2;

    fprintf(stderr, "bench %s... ", ftab->name);
    gettimeofday(&tv1, 0);
    for (int i = 0; i < loops; ++i)
        ret &= ftab->func(data, len);
    gettimeofday(&tv2, 0);
    printf("%s\n", ret?"pass":"FAIL");

    time = tv2.tv_usec - tv1.tv_usec;
    time = time / 1000000 + tv2.tv_sec - tv1.tv_sec;
    size = ((double)len * loops) / (1024*1024);
    printf("time: %.4f s\n", time);
    printf("data: %.0f MB\n", size);
    printf("BW: %.2f MB/s\n", size / time);
}

static void usage(const char *bin)
{
    printf("Usage:\n");
    printf("%s test  [alg] ==> test all or one algorithm\n", bin);
    printf("%s bench [alg] ==> benchmark all or one algorithm\n", bin);
    printf("[alg] = ");
    for (int i = 0; i < sizeof(ftab)/sizeof(ftab[0]); ++i)
        printf("%s ", ftab[i].name);
    printf("\n");
}

int main(int argc, char *argv[])
{
    int len;
    unsigned char *data;
    const char *alg = NULL;
    void (*tb)(const unsigned char *data, int len, const struct ftab *ftab);

    tb = NULL;
    if (argc >= 2) {
        if (strcmp(argv[1], "test") == 0)
            tb = test;
        else if (strcmp(argv[1], "bench") == 0)
            tb = bench;
        if (argc >= 3)
            alg = argv[2];
    }

    if (tb == NULL) {
        usage(argv[0]);
        return 1;
    }

    /* Load UTF8 test buffer */
    data = load_test_file(&len);

    if (tb == bench)
        printf("==================== Bench UTF8 ====================\n");
    for (int i = 0; i < sizeof(ftab)/sizeof(ftab[0]); ++i) {
        if (alg && strcmp(alg, ftab[i].name) != 0)
            continue;
        tb((const unsigned char *)data, len, &ftab[i]);
        printf("\n");
    }

    if (tb == bench) {
        printf("==================== Bench ASCII ====================\n");
        /* Change test buffer to ascii */
        for (int i = 0; i < len; i++)
            data[i] &= 0x7F;

        for (int i = 0; i < sizeof(ftab)/sizeof(ftab[0]); ++i) {
            if (alg && strcmp(alg, ftab[i].name) != 0)
                continue;
            tb((const unsigned char *)data, len, &ftab[i]);
            printf("\n");
        }
    }

    free(data);

    return 0;
}
