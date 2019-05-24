// This code is part of the project "Ligra: A Lightweight Graph Processing
// Framework for Shared Memory", presented at Principles and Practice of
// Parallel Programming, 2013.
// Copyright (c) 2013 Julian Shun and Guy Blelloch
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights (to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#if defined(CILK)
#include <cilk/cilk.h>
#define parallel_main main
#define parallel_for cilk_for
#define parallel_for_1 _Pragma("cilk_grainsize = 1") cilk_for
#define parallel_for_256 _Pragma("cilk_grainsize = 256") cilk_for

// intel cilk+
#elif defined(CILKP)
#include <cilk/cilk.h>
#define parallel_for cilk_for
#define parallel_main main
#define parallel_for_1 _Pragma("cilk grainsize = 1") cilk_for
#define parallel_for_256 _Pragma("cilk grainsize = 256") cilk_for

// openmp
#elif defined(OPENMP)
#include <omp.h>
#define cilk_spawn
#define cilk_sync
#define parallel_main main
#define parallel_for _Pragma("omp parallel for") for
#define parallel_for_1 _Pragma("omp parallel for schedule (static,1)") for
#define parallel_for_256 _Pragma("omp parallel for schedule (static,256)") for

// c++
#else
#define cilk_spawn
#define cilk_sync
#define parallel_main main
#define parallel_for for
#define parallel_for_1 for
#define parallel_for_256 for
#define cilk_for for

#endif

#include <limits.h>

#if defined(LONG)
typedef long long intT;
typedef unsigned long long uintT;
#define INT_T_MAX LLONG_MAX
#define PRIintT "lld"
#define PRIuintT "llu"
#else
typedef int intT;
typedef unsigned int uintT;
#define INT_T_MAX INT_MAX
#define PRIintT "d"
#define PRIuintT "u"
#endif

// edges store 32-bit quantities unless EDGELONG is defined
#if defined(EDGELONG)
typedef long long intE;
typedef unsigned long long uintE;
#define PRIintE "lld"
#define PRIuintE "llu"
#else
typedef int intE;
typedef unsigned int uintE;
#define PRIintE "d"
#define PRIuintE "u"
#endif
