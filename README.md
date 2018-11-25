# Fast UTF-8 validation with range based algorithm

This is a brand new algorithm to leverage SIMD for fast UTF-8 string validation. Both NEON(armv8a) and SSE4 versions are implemented.

Four UTF-8 validation methods are implemented and compared on both x86 and Arm platforms. Benchmark result shows range base algorithm is the best solution on Arm, and achieves same performance as [Lemire's approach](https://lemire.me/blog/2018/05/16/validating-utf-8-strings-using-as-little-as-0-7-cycles-per-byte/) on x86.

* Range based algorithm
  * range-neon.c: NEON version
  * range-sse.c: SSE4 version
  * range2-neon.c, range2-sse.c: Process two blocks in one iteration
* [Lemire's SIMD implementation](https://github.com/lemire/fastvalidate-utf-8)
  * lemire-sse.c: SSE4 version
  * lemire-neon.c: NEON porting
* naive.c: Naive UTF-8 validation byte by byte
* lookup.c: [Lookup-table method](http://bjoern.hoehrmann.de/utf-8/decoder/dfa/)

## Benchmark result

#### Method
1. Generate UTF-8 test buffer per [test file](https://raw.githubusercontent.com/cyb70289/utf8/master/UTF-8-demo.txt) or buffer size.
1. Call validation sub-routines in a loop until 1G bytes are checked.
1. Calculate speed(MB/s) of validating UTF-8 strings.

#### Arm(armv8a)
test case | naive | lemire | range
--------- | ----- | ------ | -----
[UTF-demo.txt](https://raw.githubusercontent.com/cyb70289/utf8/master/UTF-8-demo.txt) | 454.18 | 1195.50 | 1411.72
33 bytes | 600.90 | 588.77 | 1009.31
129 bytes | 689.69 | 938.07 | 1283.77
1K bytes | 721.50 | 1188.96 | 1398.15
8K bytes | 724.93 | 1198.90 | 1412.18
64K bytes | 728.18 | 1200.20 | 1415.11
1M bytes | 714.76 | 1200.93 | 1415.65

#### x86(E5-2650)
test case | naive | lemire | range
--------- | ----- | ------ | -----
[UTF-demo.txt](https://raw.githubusercontent.com/cyb70289/utf8/master/UTF-8-demo.txt) | 504.24 | 3954.74 | 3945.60
33 bytes | 906.04 | 1352.95 | 2239.55
129 bytes | 993.20 | 2742.49 | 3315.33
1K bytes | 1070.17 | 3755.88 | 3781.23
8K bytes | 1081.64 | 3860.71 | 3922.81
64K bytes | 1094.28 | 3935.15 | 3973.50
1M bytes | 1068.84 | 3923.51 | 3953.00

## Range algorithm analysis

Basically, range algorithm reads 16 bytes and figures out value range for each byte efficiently, then validate them at once.

![Range based UTF-8 validation algorithm](https://raw.githubusercontent.com/cyb70289/utf8/master/range.png)
