#!/usr/bin/env python3
# Generates the guest of the code-footprint benchmark: NF distinct "system update" functions, each a
# straight run of f64 loads, multiply-adds, compares and stores on a shared state array, and an exported
# frame() that calls every one through a function-pointer table (call_indirect), the way a large gauge
# (TFDi's MD-11 compiles to a 22 MB DLL) touches a big body of different code every frame.
#
#   gen-big.py <NF> > big.c
import random, sys

nf = int(sys.argv[1])
rnd = random.Random(nf)
S = 8192
out = ['#define EXPORT(n) __attribute__((export_name(n)))', 'static double s[%d];' % S, 'typedef void (*fn)(void);']
for i in range(nf):
    body = []
    for _ in range(18):
        a, b, c, d, e = (rnd.randrange(S) for _ in range(5))
        k1, k2, k3 = rnd.uniform(-0.6, 0.6), rnd.uniform(-0.5, 0.5), rnd.uniform(0, 0.01)
        k4 = (1 - abs(k1)) * 0.9  # |k1| + |k4| < 1: values stay bounded however long it runs
        body.append('s[%d]=s[%d]*%.17g+s[%d]*%.17g;if(s[%d]>%.17g)s[%d]-=%.17g;' % (a, b, k1, c, k4, d, k2, e, k3))
    out.append('__attribute__((noinline)) static void f%d(void){%s}' % (i, ''.join(body)))
out.append('static fn T[%d]={%s};' % (nf, ','.join('f%d' % i for i in range(nf))))
out.append('EXPORT("init") void init(void){for(int i=0;i<%d;i++)s[i]=(i%%97)*0.01;}' % S)
out.append('EXPORT("frame") double frame(int n){for(int r=0;r<n;r++)for(int i=0;i<%d;i++)T[i]();return s[1];}' % nf)
print('\n'.join(out))
