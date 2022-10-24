#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/mman.h>

#define PS	(1UL << 12)
#define HPS	(1UL << 21)
#define ADDR	(0x700000000000UL)
#define MADV_COLLAPSE	25		/* Synchronous hugepage collapse */

int main(int argc, char **argv)
{
	int ret;
	char buf[256];
	char *ptr;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s [read|write]\n", argv[0]);
		return 1;
	}

	ptr = mmap((void *)ADDR, HPS, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_SHARED, -1, 0);
	if (ptr == (void *)MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	if (!strcmp(argv[1], "read")) {
		for (int i = 0; i < 512; i++)
			ret = ptr[i * PS];
	} else if (!strcmp(argv[1], "write")) {
		memset(ptr, 0, HPS);
	} else {
		fprintf(stderr, "Usage: %s [read|write]\n", argv[0]);
		return 1;
	}
	sprintf(buf, "page-types -p %d -a 0x700000000+512 -rl", getpid());
	system(buf);
	ret = madvise(ptr, HPS, MADV_COLLAPSE);
	if (ret < 0) {
		perror("madvise");
		return 1;
	}
	system(buf);
	return 0;
}
