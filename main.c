#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

int utf8_naive(const unsigned char *data, int len);

static struct ftab {
    const char *name;
    int (*func)(const unsigned char *data, int len);
} ftab[] = {
    {
        .name = "naive",
        .func = utf8_naive,
    },
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

static void test(unsigned char *data, int len, struct ftab *ftab, int bench)
{
    int ret;
    unsigned char save;

    ret = ftab->func(data, len);
    printf("%s(positive): %s\n", ftab->name, ret?"pass":"FAIL");

    /* Last byte can only between 00-BF */
    save = data[len-1];
    data[len-1] = 0xCC;
    ret = ftab->func(data, len);
    printf("%s(negative): %s\n", ftab->name, ret?"FAIL":"pass");
    data[len-1] = save;
}

int main(int argc, char *argv[])
{
    unsigned char *data;
    int bench = 0, len;

    if (argc > 1)
        bench = 1;

    data = load_test_file(&len);

    for (int i = 0; i < sizeof(ftab)/sizeof(ftab[0]); ++i)
        test(data, len, &ftab[i], bench);

    free(data);

    return 0;
}
