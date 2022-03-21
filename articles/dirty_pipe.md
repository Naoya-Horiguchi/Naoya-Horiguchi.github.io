# Dirty Pipe 脆弱性

本記事では、3/7 に公開され、既に多くのニュースサイトなどでも話題になっている脆弱性である [Dirty Pipe](https://dirtypipe.cm4all.com/) について説明していきます。
この脆弱性は、カーネル内の pipe のバッファの扱いの不備により、
書き込み権限がないファイルにデータを書き込めてしまう脆弱性です。

{:toc}

# ディストリビューション対応

RHEL や Ubuntu など主要なディストリビューションの脆弱性対応状況に
ついては以下の公式情報を参照のこと。

- https://access.redhat.com/ja/security/vulnerabilities/6802131
- https://ubuntu.com/security/CVE-2022-0847

upstream の変更履歴上は 5.8 でエンバグして 5.17 で修正がマージされているので、
基本的にはその間のカーネルが影響を受けることになる。
しかし、関連する潜在的な問題が 4.9 で入っている関係で、
RHEL8 (4.18 系) や Ubuntu 20.04 LTS (5.4 系)
では「根本的な不具合は存在するが、既存の攻撃手段は成立しない」といった
書き方になっている。

# pipe の基本

- pipe はプロセス間通信に利用されるデータチャネル。
- bash などのシェル環境で `|` を用いて標準入出力をつなぐときも使われている。
- mkfifo でファイル (名前付きパイプ) として作成することも可能。
- システムコール pipe() を用いるとファイルディスクリプタの対が得られる。

# splice

- システムコール `splice()` は fd から fd に「継ぎ足す」

```
       ssize_t splice(int fd_in, off64_t *off_in, int fd_out,
                      off64_t *off_out, size_t len, unsigned int flags
```

- `fd_in` で指定される「ファイル」の `off_in` バイト目から `len` バイト分だけ、
  `fd_out` で指定される「ファイル」の `off_out` バイト目に流し込む。
- `read()`/`write()` システムコールを用いるとユーザ空間のバッファを経由しないといけないので効率が悪い。
- `splice()` を使うとカーネル内で参照を付け替えることでコピーを回避できることがある。
- `splice()` に渡す fd はファイルだけでなくソケットや pipe を渡すこともできる。
- 似たようなシステムコールとしてネットワーク送信に対象を絞った `sendfile()` もある。
- これらのシステムコールは、カーネル内で pipe と共通のコードを通っている。

# 実運用システムがどのように Dirty Pipe を踏むか

- 発見者の観測した現象: ログサーバがネットワーク越しにデータを集め、
  splice() と pipe を使って月次のデータの圧縮ファイルを作成するワークロードで、
  圧縮ファイルが破壊されて見える (CRC チェックに失敗)。

# 攻撃

攻撃が成功する条件

- 対象ファイルに対するリード権限がある。
- 対象書き込み位置がページサイズアラインでない。
- ライトが page boundary をまたがない。またいでしまうと anonymous buffer になってしまう (大きなデータは書けない)

https://dirtypipe.cm4all.com/#exploiting をやってみる。

```
[build3:~/tmp/220316_dirty_pipe]$ uname -r
v5.16
[build3:~/tmp/220316_dirty_pipe]$ whoami
root
[build3:~/tmp/220316_dirty_pipe]$ echo "confidential data" > confidential
[build3:~/tmp/220316_dirty_pipe]$ ls -l
...
-rw-r--r--. 1 root root        18 Mar 16 22:25 confidential
[build3:~/tmp/220316_dirty_pipe]$ su hori
[hori@build3 220316_dirty_pipe]$ cat confidential
confidential data
[hori@build3 220316_dirty_pipe]$ echo 'change' > confidential
bash: confidential: Permission denied
[hori@build3 220316_dirty_pipe]$ gcc -o exploit exploit.c
[hori@build3 220316_dirty_pipe]$ ll
total 91816
-rw-r--r--. 1 root root        18 Mar 16 22:25 confidential
-rwxrwxr-x. 1 hori hori     24520 Mar 16 22:26 exploit
-rw-rw-r--. 1 hori hori      4384 Mar 16 21:33 exploit.c
...
[hori@build3 220316_dirty_pipe]$ ./exploit confidential 13 'aaaa'
It worked!
[hori@build3 220316_dirty_pipe]$ cat confidential
confidential aaaa
```

興味深い点としては、
この状態、メモリ上のデータが不正に更新されたのに、
dirty フラグが立っていないのでそのままだとライトバックされない。
drop_caches すると元に戻る。

```
[build3:~/tmp/220316_dirty_pipe]$ cat confidential
confidential aaaa
[build3:~/tmp/220316_dirty_pipe]$ echo 3 > /proc/sys/vm/drop_caches
[build3:~/tmp/220316_dirty_pipe]$ cat confidential
confidential data
```

同じことを修正済みカーネルでやってみると、攻撃は成立していないことがわかる。

```
[build4:~/tmp/220316_dirty_pipe]$ uname -r
v5.17-rc7
[build4:~/tmp/220316_dirty_pipe]$ echo "confidential data" > confidential
[build4:~/tmp/220316_dirty_pipe]$ ll
...
-rw-rw-r--. 1 hori hori     4384 Mar 16 21:33 exploit.c
-rw-r--r--. 1 root root       18 Mar 16 22:02 confidential
[build4:~/tmp/220316_dirty_pipe]$ sudo su hori
[hori@build4 220316_dirty_pipe]$ cat confidential
confidential data
[hori@build4 220316_dirty_pipe]$ echo 'attack' >  confidential
bash: confidential: Permission denied
[hori@build4 220316_dirty_pipe]$ gcc -o exploit exploit.c
[hori@build4 220316_dirty_pipe]$ ./exploit confidential 13 'aaaa'
It worked!
[hori@build4 220316_dirty_pipe]$ cat confidential
confidential data
```

# ソースコード

攻撃用のコードを実行したときの処理の流れを、ユーザ空間・カーネル内の
両面について追跡していく。
再現プログラムは以下をやっている。

- pipe() システムコールを呼んで pipe の初期化。
- 一回 pipe にデータを書き込んで・読み出すことで、pipe_buffer のフラグ PIPE_BUF_FLAG_CAN_MERGE をセットする。
- splice で**攻撃したいファイルの攻撃したい箇所の直前 1 バイトを書く」**
  こうすることで攻撃対象のページキャッシュを pipe_buffer から参照させる。
- 攻撃によって書き込みたいデータを pipe 経由で write する。

# カーネルソースの関連箇所

## pipe の初期化 (do_pipe2, alloc_pipe_info)

pipe() システムコールから以下のように pipe_buffer 構造体配列 (デフォルト 16 ページ分) 割り当てている。

```
__x64_sys_pipe
  do_pipe2
    __do_pipe_flags
      create_pipe_files
        get_pipe_inode
          new_inode_pseudo
          alloc_pipe_info  // この先で struct pipe_buffer の割当て
            zalloc(sizeof(struct pipe_inode_info), ...)
            account_pipe_buffers
            pipe->bufs = kvcalloc(pipe_bufs, sizeof(struct pipe_buffer), ...)
              // ->bufs でバッファ領域を管理
        alloc_file_pseudo
        alloc_file_clone
          alloc_file
      // fd の割当て (ユーザ空間に返す)
    fd_install
```

## pipe への書き込み

pipe に対して `write()` を実行すると `pipe_write()` を通して
`pipe->bufs` の先のデータが更新される。

```
__kernel_write
  file->f_op->write_iter() // pipe の場合はこれが pipe_write()
    pipe_write
      // buffer が空でない場合、追記データをマージさせようとする。
      for ...
        if (!pipe_fill)
          pipe->tmp_page = alloc_page()
          // このあたりの処理がおそらく重要
          /* Insert it into the buffer array */
          buf = &pipe->bufs[head & mask];
          buf->page = page;
          buf->ops = &anon_pipe_buf_ops;  // anonymous pipe を使用
          buf->offset = 0;
          buf->len = 0;
          if (is_packetized(filp))
                  buf->flags = PIPE_BUF_FLAG_PACKET;
          else
                  buf->flags = PIPE_BUF_FLAG_CAN_MERGE;  // 再現コードではこちらを通る
          ...
          copy_page_from_iter // データ転送箇所
```

ここでセットされるフラグ PIPE_BUF_FLAG_CAN_MERGE は
https://github.com/torvalds/linux/commit/f6dd975583bd8ce088400648fd9819e4691c8958
で追加されている。
これがバグの原因となるコミット。

```
commit f6dd975583bd8ce088400648fd9819e4691c8958
Author: Christoph Hellwig <hch@lst.de>
Date:   Wed May 20 17:58:12 2020 +0200 // v5.8-rc1

    pipe: merge anon_pipe_buf*_ops
```

## packetized mode

余談になるが、is_packetized() の真偽によって動作を変えている点について。
file 構造体の O_DIRECT フラグがセットされていれば true となるが、
これは `pipe2()` (`pipe()` の引数ありバージョン) で明示的にしていしないとそうならないので、デフォルトでは false と考えて良い。
packetized mode と呼んでいるようだ。
この関数自体が追加されたのは以下のコミットで、結構古い。

```
commit 9883035ae7edef3ec62ad215611cb8e17d6a1a5d
Author: Linus Torvalds <torvalds@linux-foundation.org>
Date:   Sun Apr 29 13:12:42 2012 -0700

    pipes: add a "packetized pipe" mode for writing
```

write 側が細かい (バッファサイズに満たないサイズの) データを書いていても、
read 側はまとめて一回で読み出せるので少し効率が良くなる。

## pipe からの読み出し

特に注意するようなことはやっていない。

```
__kernel_read
  file->f_op->read_iter() // = pipe_read
    pipe_read
      for
        if (!pipe_empty())
          pipe_buf_confirm
          copy_page_to_iter
```

## splice のソースコード

攻撃プログラムの splice の箇所は以下。

```
  /* splice one byte from before the specified offset into the
     pipe; this will add a reference to the page cache, but
     since copy_page_to_iter_pipe() does not initialize the
     "flags", PIPE_BUF_FLAG_CAN_MERGE is still set */
  --offset;
  ssize_t nbytes = splice(fd, &offset, p[1], NULL, 1, 0);
```

`fd_in` が攻撃対象ファイルの fd、`fd_out` がパイプの fd なので、
splice() は以下のように file_to_pipe のパスを通る。

```
__x64_sys_splice
  __do_splice
    do_splice
      splice_file_to_pipe
        do_splice_to
          in->f_op->splice_read (== generic_file_splice_read)
            generic_file_splice_read
              iov_iter_pipe // iterator のタイプが ITER_PIPE として初期化。
              call_read_iter
                generic_file_read_iter
                  filemap_read
                    copy_folio_to_iter
                      copy_page_to_iter  // lib/iov_iter.c
                        __copy_page_to_iter
                          copy_page_to_iter_pipe // iter type が ITER_PIPE なので
                            buf = &pipe->bufs[i_head & p_mask];
                            ...
                            buf->ops = &page_cache_pipe_buf_ops;
                            buf->flags = 0;   // v5.17-rc1 で修正パッチで追加される行
                            get_page(page);
                            buf->page = page;
                            buf->offset = offset;
                            buf->len = bytes;
```


上記から、`buf->page` が切り替わるのは良いのだが、`buf->flags` の PIPE_BUF_FLAG_CAN_MERGE がセットされたままだったために、後続の pipe への書き込みにおいてバッファが使い回されてしまった。

# [修正パッチ](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=9d2231c5d74e13b2a0546fee6737ee4446017903)

```
commit 9d2231c5d74e13b2a0546fee6737ee4446017903
Author: Max Kellermann <max.kellermann@ionos.com>
Date:   Mon Feb 21 11:03:13 2022 +0100

    lib/iov_iter: initialize "flags" in new pipe_buffer

    The functions copy_page_to_iter_pipe() and push_pipe() can both
    allocate a new pipe_buffer, but the "flags" member initializer is
    missing.

    Fixes: 241699cd72a8 ("new iov_iter flavour: pipe-backed")
    To: Alexander Viro <viro@zeniv.linux.org.uk>
    To: linux-fsdevel@vger.kernel.org
    To: linux-kernel@vger.kernel.org
    Cc: stable@vger.kernel.org
    Signed-off-by: Max Kellermann <max.kellermann@ionos.com>
    Signed-off-by: Al Viro <viro@zeniv.linux.org.uk>

diff --git a/lib/iov_iter.c b/lib/iov_iter.c
index b0e0acdf96c1..6dd5330f7a99 100644
--- a/lib/iov_iter.c
+++ b/lib/iov_iter.c
@@ -414,6 +414,7 @@ static size_t copy_page_to_iter_pipe(struct page *page, size_t offset, size_t by
                return 0;

        buf->ops = &page_cache_pipe_buf_ops;
+       buf->flags = 0;
        get_page(page);
        buf->page = page;
        buf->offset = offset;
@@ -577,6 +578,7 @@ static size_t push_pipe(struct iov_iter *i, size_t size,
                        break;

                buf->ops = &default_pipe_buf_ops;
+               buf->flags = 0;
                buf->page = page;
                buf->offset = 0;
                buf->len = min_t(ssize_t, left, PAGE_SIZE);
```

## PIPE_BUF_FLAG_CAN_MERGE の意味

このフラグが脆弱性の説明の動作とどう関係しているか?
このフラグを参照しているのは pipe_write の前半の以下の箇所。

```
        was_empty = pipe_empty(head, pipe->tail);
        chars = total_len & (PAGE_SIZE-1);    // 今回書き込むデータ
                                                 (のページ境界まで) のサイズ
        if (chars && !was_empty) {
                unsigned int mask = pipe->ring_size - 1;    // 0xfff
                struct pipe_buffer *buf = &pipe->bufs[(head - 1) & mask];
                int offset = buf->offset + buf->len;   // 未処理データの末尾位置

                if ((buf->flags & PIPE_BUF_FLAG_CAN_MERGE) &&
                    offset + chars <= PAGE_SIZE) {
                        // フラグセットされていて、
                        // かつ、書き込みデータがバッファページの残り位置に収まる場合
                        ret = pipe_buf_confirm(pipe, buf);
                        if (ret)
                                goto out;

                        ret = copy_page_from_iter(buf->page, offset, chars, from);
                        if (unlikely(ret < chars)) {
                                ret = -EFAULT;
                                goto out;
                        }

                        buf->len += ret;
                        if (!iov_iter_count(from))
                                goto out;
                }
        }
```

小さいデータの (同じページに収まるような) 書き込みが複数回発生したとき、
このフラグが立っていた場合、上記コードを通って `pipe->bufs` の先のページ (攻撃対象ファイルのページキャッシュ) を更新してしまう (`copy_page_from_iter`)。
フラグがセットされていない場合は、上記ブロックはスキップされ、続くロジックで適切に扱われる。
