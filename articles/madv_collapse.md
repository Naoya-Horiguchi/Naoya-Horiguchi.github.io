# MADV_COLLAPSE

`MADV_COLLAPSE` はカーネル v6.1 向けにマージされた機能で、
ユーザ空間から特定のアドレス範囲を THP (Transparent HugePage) に変換するよう指示する機能です。

Anonymous ページが THP に変換されることを collapse 処理と呼びます。
通常 THP の collapse 処理は khugepaged というカーネルスレッドがバックグラウンドで実行するのに任せることになりますが、
アプリケーションが THP から得られる利得を最大化するために意図的に THP collapse を駆動したいことがあり、
本機能はそれを実現するためのものです。

# 背景知識

... (省略)

# 簡単な例

`MADV_COLLAPSE` は以下のようなプログラムでデモできます。

```
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
	printf("%p\n", ptr);
	if (!strcmp(argv[1], "read")) {
		for (int i = 0; i < 512; i++)
			ret = ptr[i * PS];
	} else if (!strcmp(argv[1], "write")) {
		memset(ptr, 0, HPS);
	} else {
		fprintf(stderr, "Usage: %s [read|write]\n", argv[0]);
		return 1;
	}
	sprintf(buf, "page-types -p %d -a 0x700000000+512 -r", getpid());
	system(buf);
	ret = madvise(ptr, HPS, MADV_COLLAPSE);
	if (ret < 0) {
		perror("madvise");
		return 1;
	}
	system(buf);
	return 0;
}
```

このプログラムは外部プログラム [`page-types`](https://github.com/torvalds/linux/blob/master/tools/vm/page-types.c) を使用しています。
このプログラムはプロセスのアドレス空間にマップされたページの物理アドレスとページフラグを表示するものです。

このプログラムを実行すると、以下のような出力が得られ、`madvise(MADV_COLLAPSE)` により anonymous ページが THP に変換されていることが分かります。

```
$ ./a.out write
             flags	page-count       MB  symbolic-flags			long-symbolic-flags
0xa000010000004838	       512        2  ___UDl_____M__b____________________f_____F_1	uptodate,dirty,lru,mmap,swapbacked,softdirty,file,mmap_exclusive
             total	       512        2

// ここで madvise(MADV_COLLAPSE) が実行される

             flags	page-count       MB  symbolic-flags			long-symbolic-flags
0x8000010000410800	       511        1  ___________M____T_____t____________f_______1	mmap,compound_tail,thp,softdirty,mmap_exclusive
0x800001000040c838	         1        0  ___UDl_____M__bH______t____________f_______1	uptodate,dirty,lru,mmap,swapbacked,compound_head,thp,softdirty,mmap_exclusive
             total	       512        2
```

これにより THP の動作をアプリケーションからコントロールできるようになり、THP 利用時の選択肢が増えたということになります。

以上です。
