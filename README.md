# Build kernel

```sh
# Download kernel source code
wget https://www.kernel.org/pub/linux/kernel/v6.x/linux-6.8.1.tar.gz
tar -xf linux-6.8.1.tar.gz
cp -R linux-6.8.1 linux-6.8.1-ori

# Apply kernel patch
git apply 6.8.1.patch

# Compile kernel
nohup python3 -u build.py &>build.out &
```

# How to collect global memory write

When booting the kernel, add two extra command line arguments:

```
trace_event=mycov_rw:mycov_rw_write
trace_buf_size=32M
```

To get the write records:

```sh
cat /sys/kernel/debug/tracing/trace
```

Then you need to clear it (otherwise the next time you will get old records)

```sh
echo > /sys/kernel/debug/tracing/trace
```

# Corpus you can use

```
https://storage.googleapis.com/syzkaller/corpus/ci-upstream-kasan-gce-corpus.db
https://storage.googleapis.com/syzkaller/corpus/ci2-linux-6-6-kasan-corpus.db
```
