# データベースにおける MMAP

最近 Twitter で以下の論文の存在を知ったので読んで学んでみる。
- ["Are You Sure You Want to Use MMAP in Your Database Management System?"](https://cs.brown.edu/people/acrotty/pubs/p13-crotty.pdf)

メモリ・ストレージ間のデータのやりとりは従来 file IO (read/write システムコール) ベースだったが、mmap ベースを使えればメモリ管理を OS に任せてデータベース側が楽できる、という流れがベースにあって、だけどそれはそんなに簡単じゃないよ、世の中そう甘くはないよ、というのが論文の趣旨になる。(この段落、本当??)

thp が例外なくオフられる問題はこの話の一部としてあり、個人的に何が満たされれば DBMS のようなアプリケーションがうまくカーネルの機能を使えるのか、というのは知っておきたい気がする。

mmap の利点は隠れた問題に対処するための複雑さに見合わないものという。

2.1 mmap の一般的な使用の流れを説明。パフォーマンスへの影響が大きいのは TLB shootdown 処理で、これはページ回収時に他の CPU コアの TLB にある当該ページへのエントリを削除する処理。

2.3
- MonetDB は列ごとに mmap ファイルとして保存する。
- SQLite は read/write の代わりに mmap を用いるというオプションがある。
- LMDB は mmap のみで利用可能で、それが高性能の原因だと言っている。

これら、mmap の利点を謳っている DB がある一方、mmap への以降に苦労している DB の話が続く。

- MongoDB は mmap を積極的に利用する DB だが、copying scheme が過剰に複雑化してしまった、書き出し時の圧縮ができない設計になってしまった、そのため 2015 に WiredTiger に置き換えられ、2019 にオプションとして mmap が再導入された形になる。
- InfluxDB で、DB サイズが大きくなったときに IO 遅延がスパイクする現象があったため、mmap の利用が制限されるようになったとか。
- SingleStore も性能上の問題で mmap は使用しないことにしたとか。shared write lock が原因だとか。
- RocksDB は性能の問題で mmap を使わず
- TileDB は SSD へのリードが遅いので使わず
- Scylla は eviction や IO scheduling の制御の粒度の問題で mmap 使わず
- mmap による IO が blocking な page fault である点を問題視して使わず
- RDF-3X は Windows と POSIX の非互換を理由として使わず

3.2

mmap の課題の指摘。

- Transactional Safety: 3 つのアプローチ。OS CoW, user CoW, Shadow Paging
- IO stall (async IO がない)
- Error Handling
- Performance issue: mmap の利点としてはシステムコールオーバーヘッドの回避と、メモリコピーが不要な点があるが、最近 SSD が高速化したことで顕在化した問題として OS のページ回収アルゴリズムは数スレッドを越えてスケールしないらしい。
  実験から分かった理由としては 3 つあって、page table の競合、ページ回収処理がシングルスレッドであること、TLB shootdown のコスト。
  前者二つは OS を改変することで克服できるようだが、TLB shootdown の問題はいかんともしがたいとか。

結論として mmap を使ってよいかもしれないケースとして、read-only workload でメモリに収まりきる状況のみ、としている。


# 個人的疑問

mmap は非効率だ、それは分かった。
個人的に気になるのは hugetlbfs を用いるデータベースである。
これらはバッキングストアがない (swap out もしない) ため、メモリ回収を前提としない用途で用いる前提である。
なので、データベースが hugetlbfs を用いる場合、どういう使い方をするのかというと、
メモリサイズを意識して従来どおりデータベース側でデータの書き出し、削除をコントロールしないといけないということになる。
おそらく通常のメモリの使い方とは異なる使い方をしているところに hugetlbfs を用いているという使い方にならざるを得ないのだろう。
