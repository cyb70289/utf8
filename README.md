# Fast UTF-8 validation with range based algorithm

This is a brand new algorithm to leverage SIMD for fast UTF-8 string validation. Both **NEON**(armv8a) and **SSE4** versions are implemented.

Four UTF-8 validation methods are compared on both x86 and Arm platforms. Benchmark result shows range base algorithm is the best solution on Arm, and achieves same performance as [Lemire's approach](https://lemire.me/blog/2018/05/16/validating-utf-8-strings-using-as-little-as-0-7-cycles-per-byte/) on x86.

* Range based algorithm
  * range-neon.c: NEON version
  * range-sse.c: SSE4 version
  * range2-neon.c, range2-sse.c: Process two blocks in one iteration
* [Lemire's SIMD implementation](https://github.com/lemire/fastvalidate-utf-8)
  * lemire-sse.c: SSE4 version
  * lemire-neon.c: NEON porting
* naive.c: Naive UTF-8 validation byte by byte
* lookup.c: [Lookup-table method](http://bjoern.hoehrmann.de/utf-8/decoder/dfa/)

## About the code

* Run "make" to build. Built and tested with gcc-7.3.
* Run "./utf8" to see all command line options.
* Benchmark
  * Run "./utf8 bench" to bechmark all algorithms with [default test file](https://raw.githubusercontent.com/cyb70289/utf8/master/UTF-8-demo.txt).
  * Run "./utf8 bench size NUM" to benchmark specified string size.
* Run "./utf8 test" to test all algorithms with positive and negative test cases.
* To benchmark or test specific algorithm, run something like "./utf8 bench range".

## Benchmark result (MB/s)

### Method
1. Generate UTF-8 test buffer per [test file](https://raw.githubusercontent.com/cyb70289/utf8/master/UTF-8-demo.txt) or buffer size.
1. Call validation sub-routines in a loop until 1G bytes are checked.
1. Calculate speed(MB/s) of validating UTF-8 strings.

### Arm(armv8a)
test case | naive | lookup | lemire | range | range2
--------- | ----- | ------ | ------ | ----- | ------
[UTF-demo.txt](https://raw.githubusercontent.com/cyb70289/utf8/master/UTF-8-demo.txt) | 562.25 | 412.84 | 1198.50 | 1411.72 | **1579.85**
32 bytes | 651.55 | 441.70 | 891.38 | 1003.95 | **1043.58**
33 bytes | 660.00 | 446.78 | 588.77 | 1009.31 | **1048.12**
129 bytes | 771.89 | 402.55 | 938.07 | 1283.77 | **1401.76**
1K bytes | 811.92 | 411.58 | 1188.96 | 1398.15 | **1560.23**
8K bytes | 812.25  | 412.74 | 1198.90 | 1412.18 | **1580.65**
64K bytes | 817.35 | 412.24 | 1200.20 | 1415.11 | **1583.86**
1M bytes | 815.70  | 411.93 | 1200.93 | 1415.65 | **1585.40**

### x86(E5-2650)
test case | naive | lookup | lemire | range | range2
--------- | ----- | ------ | ------ | ----- | ------
[UTF-demo.txt](https://raw.githubusercontent.com/cyb70289/utf8/master/UTF-8-demo.txt) | 753.70 | 310.41 | 3954.74 | 3945.60 | **3986.13**
32 bytes | 1135.76 | 364.07 | **2890.52** | 2351.81 | 2173.02
33 bytes | 1161.85 | 376.29 | 1352.95 | **2239.55** | 2041.43
129 bytes | 1161.22 | 322.47 | 2742.49 | **3315.33** | 3249.35
1K bytes | 1310.95 | 310.72 | 3755.88 | 3781.23 | **3874.17**
8K bytes | 1348.32 | 307.93 | 3860.71 | 3922.81 | **3968.93**
64K bytes | 1301.34 | 308.39 | 3935.15 | 3973.50 | **3983.44**
1M bytes | 1279.78 | 309.06 | 3923.51 | 3953.00 | **3960.49**

## Range algorithm analysis

Basically, range algorithm reads 16 bytes and figures out value range for each byte efficiently, then validate them at once.

![Range based UTF-8 validation algorithm](https://raw.githubusercontent.com/cyb70289/utf8/master/range.png)
