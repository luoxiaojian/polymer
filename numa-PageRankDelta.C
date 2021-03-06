/*
 * This code is part of the project "NUMA-aware Graph-structured Analytics"
 *
 *
 * Copyright (C) 2014 Institute of Parallel And Distributed Systems (IPADS),
 * Shanghai Jiao Tong University All rights reserved
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 * For more about this software, visit:
 *
 *     http://ipads.se.sjtu.edu.cn/projects/polymer.html
 *
 */

#include "gettime.h"
#include "math.h"
#include "polymer.h"

#include <numa.h>
#include <pthread.h>
#include <sys/mman.h>
using namespace std;

#define PAGE_SIZE (4096)

int CORES_PER_NODE = 6;

bool needLog = false;

volatile int shouldStart = 0;

double *delta_global = NULL;
double *nghSum_global = NULL;

double *p_global = NULL;
intT vPerNode = 0;
int numOfNode = 0;

bool needResult = false;

pthread_barrier_t barr;
pthread_barrier_t global_barr;

vertices *Frontier;
vertices *All;

Default_Hash_F *hasher2;

volatile int global_counter = 0;
volatile int global_toggle = 0;

template <class vertex>
struct PR_F {
  vertex *V;
  double *Delta, *nghSum;
  PR_F(vertex *_V, double *_Delta, double *_nghSum)
      : V(_V), Delta(_Delta), nghSum(_nghSum) {}
  inline void *nextPrefetchAddr(intT index) { return NULL; }
  inline bool update(intT s, intT d) {
    nghSum[d] += Delta[s] / V[s].getOutDegree();
    return 1;
  }
  inline bool updateAtomic(intT s, intT d) {
    writeAdd(&nghSum[d], Delta[s] / V[s].getOutDegree());
    /*
      if (needLog && d == 1 && s == 657797) {
      cout << "Update from " << s << "\t" << std::scientific <<
      std::setprecision(9) << Delta[s]/V[s].getOutDegree() << " -- " <<
      nghSum[d] << "\n";
      }
    */
    return 1;
  }
  inline bool cond(intT d) { return 1; }
};

struct PR_Vertex_F_FirstRound {
  double damping, addedConstant, one_over_n, epsilon2;
  double *p, *Delta, *nghSum;
  PR_Vertex_F_FirstRound(double *_p, double *_Delta, double *_nghSum,
                         double _damping, double _one_over_n, double _epsilon2)
      : p(_p),
        damping(_damping),
        Delta(_Delta),
        nghSum(_nghSum),
        one_over_n(_one_over_n),
        addedConstant((1 - _damping) * _one_over_n),
        epsilon2(_epsilon2) {}
  inline bool operator()(intT i) {
    Delta[i] = damping * (p[i] + nghSum[i]) + addedConstant - p[i];
    p[i] += Delta[i];
    Delta[i] -= one_over_n;  // subtract off delta from initialization
    return (fabs(Delta[i]) > epsilon2 * p[i]);
  }
};

struct PR_Vertex_F {
  double damping, epsilon2;
  double *p, *Delta, *nghSum;
  PR_Vertex_F(double *_p, double *_Delta, double *_nghSum, double _damping,
              double _epsilon2)
      : p(_p),
        damping(_damping),
        Delta(_Delta),
        nghSum(_nghSum),
        epsilon2(_epsilon2) {}
  inline bool operator()(intT i) {
    Delta[i] = nghSum[i] * damping;
    p[i] += Delta[i];
    return (fabs(Delta[i]) > epsilon2 * p[i]);
  }
};

struct PR_Vertex_Reset {
  double *nghSum;
  PR_Vertex_Reset(double *_nghSum) : nghSum(_nghSum) {}
  inline bool operator()(intT i) {
    nghSum[i] = 0.0;
    return 1;
  }
};

struct PR_worker_arg {
  void *GA;
  int maxIter;
  int tid;
  int numOfNode;
  intT rangeLow;
  intT rangeHi;
  double damping;
  double epsilon;
  double epsilon2;
  double *delta;
  double *nghSum;
  double *p;
};

struct PR_subworker_arg {
  void *GA;
  int maxIter;
  int tid;
  int subTid;
  intT startPos;
  intT endPos;
  intT rangeLow;
  intT rangeHi;
  double **delta_ptr;
  double **nghSum_ptr;
  double **p_val_ptr;
  double damping;
  double epsilon2;
  pthread_barrier_t *node_barr;
  pthread_barrier_t *node_barr2;
  LocalFrontier *localFrontier;
  LocalFrontier *dummyFrontier;
  volatile int *barr_counter;
  volatile int *toggle;
};

template <class vertex>
void *PageRankSubWorker(void *arg) {
  PR_subworker_arg *my_arg = (PR_subworker_arg *)arg;
  graph<vertex> &GA = *(graph<vertex> *)my_arg->GA;
  const intT n = GA.n;
  const double one_over_n = 1 / (double)n;
  int maxIter = my_arg->maxIter;
  int tid = my_arg->tid;
  int subTid = my_arg->subTid;
  pthread_barrier_t *local_barr = my_arg->node_barr;
  LocalFrontier *output = my_arg->localFrontier;
  LocalFrontier *dummy = my_arg->dummyFrontier;

  double *delta = *(my_arg->delta_ptr);
  double *nghSum = *(my_arg->nghSum_ptr);
  double *p = *(my_arg->p_val_ptr);
  double damping = my_arg->damping;
  double epsilon2 = my_arg->epsilon2;
  int currIter = 0;
  intT rangeLow = my_arg->rangeLow;
  intT rangeHi = my_arg->rangeHi;

  intT start = my_arg->startPos;
  intT end = my_arg->endPos;

  Custom_barrier globalCustom(&global_counter, &global_toggle,
                              Frontier->numOfNodes);
  Custom_barrier localCustom(my_arg->barr_counter, my_arg->toggle,
                             CORES_PER_NODE);

  Subworker_Partitioner subworker(CORES_PER_NODE);
  subworker.tid = tid;
  subworker.subTid = subTid;
  subworker.dense_start = start;
  subworker.dense_end = end;
  subworker.global_barr = &global_barr;
  subworker.local_barr = my_arg->node_barr2;
  subworker.local_custom = localCustom;
  subworker.subMaster_custom = globalCustom;

  pthread_barrier_wait(local_barr);
  intT threshold = 0;
  while (1) {
    if (maxIter > 0 && currIter >= maxIter) break;
    currIter++;
    if (subTid == 0) {
      Frontier->calculateNumOfNonZero(tid);
      //{parallel_for(long long i=output->startID;i<output->endID;i++)
      //output->setBit(i, false);}
    }

    pthread_barrier_wait(local_barr);

    edgeMap(GA, Frontier, PR_F<vertex>(GA.V, delta, nghSum), dummy, threshold,
            DENSE_FORWARD, false, true, subworker);

    pthread_barrier_wait(local_barr);
    pthread_barrier_wait(local_barr);

    if (currIter == 1) {
      vertexFilter(All,
                   PR_Vertex_F_FirstRound(p, delta, nghSum, damping, one_over_n,
                                          epsilon2),
                   tid, subTid, CORES_PER_NODE, output);
    } else {
      vertexFilter(All, PR_Vertex_F(p, delta, nghSum, damping, epsilon2), tid,
                   subTid, CORES_PER_NODE, output);
    }
    // printf("filter over: %d %d\n", tid, subTid);
    pthread_barrier_wait(local_barr);
    // compute L1-norm (use nghSum as temp array)

    // reset
    vertexMap(All, PR_Vertex_Reset(nghSum), tid, subTid, CORES_PER_NODE);
    output = Frontier->getFrontier(tid);
    pthread_barrier_wait(local_barr);
    // swap
    pthread_barrier_wait(local_barr);
  }
  return NULL;
}

pthread_barrier_t timerBarr;

template <class vertex>
void *PageRankThread(void *arg) {
  PR_worker_arg *my_arg = (PR_worker_arg *)arg;
  graph<vertex> &GA = *(graph<vertex> *)my_arg->GA;
  int maxIter = my_arg->maxIter;
  int tid = my_arg->tid;

  char nodeString[10];
  sprintf(nodeString, "%d", tid);
  struct bitmask *nodemask = numa_parse_nodestring(nodeString);
  numa_bind(nodemask);

  intT rangeLow = my_arg->rangeLow;
  intT rangeHi = my_arg->rangeHi;

  graph<vertex> localGraph = graphFilter(GA, rangeLow, rangeHi);

  while (shouldStart == 0)
    ;

  pthread_barrier_wait(&timerBarr);
  printf("over filtering: %d\n", tid);

  const intT n = GA.n;
  const double damping = my_arg->damping;
  const double epsilon = my_arg->epsilon;
  const double epsilon2 = my_arg->epsilon2;
  int numOfT = my_arg->numOfNode;

  intT blockSize = rangeHi - rangeLow;

  // printf("blockSizeof %d: %d low: %d high: %d\n", tid, blockSize, rangeLow,
  // rangeHi);

  double one_over_n = 1 / (double)n;

  double *p = p_global;
  double *delta = delta_global;
  double *nghSum = nghSum_global;
  bool *frontier = (bool *)numa_alloc_local(sizeof(bool) * blockSize);

  bool *all = (bool *)numa_alloc_local(sizeof(bool) * blockSize);

  /*
    if (tid == 0)
    startTime();
  */
  double mapTime = 0.0;
  struct timeval start, end;
  struct timezone tz = {0, 0};

  for (intT i = rangeLow; i < rangeHi; i++) p[i] = 0;
  for (intT i = rangeLow; i < rangeHi; i++) delta[i] = one_over_n;
  for (intT i = rangeLow; i < rangeHi; i++) nghSum[i] = 0;  // 0 if unchanged
  for (intT i = 0; i < blockSize; i++) frontier[i] = true;
  for (intT i = 0; i < blockSize; i++) all[i] = true;

  if (tid == 0) {
    Frontier = new vertices(numOfT);
    All = new vertices(numOfT);
  }

  LocalFrontier *current = new LocalFrontier(frontier, rangeLow, rangeHi);
  LocalFrontier *full = new LocalFrontier(all, rangeLow, rangeHi);

  // printf("register %d: %p\n", tid, frontier);

  pthread_barrier_wait(&barr);

  Frontier->registerFrontier(tid, current);
  All->registerFrontier(tid, full);

  pthread_barrier_wait(&barr);

  if (tid == 0) {
    Frontier->calculateOffsets();
    All->calculateOffsets();
  }

  bool *next = (bool *)numa_alloc_local(sizeof(bool) * blockSize);
  for (intT i = 0; i < blockSize; i++) next[i] = false;

  LocalFrontier *output = new LocalFrontier(next, rangeLow, rangeHi);

  // dummy frontier obj
  bool *dummyNext = (bool *)numa_alloc_local(sizeof(bool) * blockSize);
  for (intT i = 0; i < blockSize; i++) dummyNext[i] = false;
  LocalFrontier *dummy = new LocalFrontier(dummyNext, rangeLow, rangeHi);

  pthread_barrier_t localBarr;
  pthread_barrier_init(&localBarr, NULL, CORES_PER_NODE + 1);

  pthread_barrier_t localBarr2;
  pthread_barrier_init(&localBarr2, NULL, CORES_PER_NODE);

  intT sizeOfShards[CORES_PER_NODE];

  subPartitionByDegree(localGraph, CORES_PER_NODE, sizeOfShards, sizeof(double),
                       true, true);

  intT startPos = 0;

  pthread_t subTids[CORES_PER_NODE];

  volatile int local_custom_counter = 0;
  volatile int local_toggle = 0;

  for (int i = 0; i < CORES_PER_NODE; i++) {
    PR_subworker_arg *arg =
        (PR_subworker_arg *)malloc(sizeof(PR_subworker_arg));
    arg->GA = (void *)(&localGraph);
    arg->maxIter = maxIter;
    arg->tid = tid;
    arg->subTid = i;
    arg->rangeLow = rangeLow;
    arg->rangeHi = rangeHi;
    arg->delta_ptr = &delta;
    arg->nghSum_ptr = &nghSum;
    arg->p_val_ptr = &p;
    arg->damping = damping;
    arg->epsilon2 = epsilon2;
    arg->node_barr = &localBarr;
    arg->node_barr2 = &localBarr2;
    arg->localFrontier = output;
    arg->dummyFrontier = dummy;

    arg->barr_counter = &local_custom_counter;
    arg->toggle = &local_toggle;

    arg->startPos = startPos;
    arg->endPos = startPos + sizeOfShards[i];
    startPos = arg->endPos;
    pthread_create(&subTids[i], NULL, PageRankSubWorker<vertex>, (void *)arg);
  }

  pthread_barrier_wait(&localBarr);

  pthread_barrier_wait(&barr);
  intT round = 0;
  while (1) {
    if (maxIter > 0 && round >= maxIter) break;
    round++;

    pthread_barrier_wait(&localBarr);

    // edgeMap
    pthread_barrier_wait(&localBarr);
    pthread_barrier_wait(&barr);
    pthread_barrier_wait(&localBarr);
    needLog = true;
    // vertexFilter

    pthread_barrier_wait(&barr);
    pthread_barrier_wait(&localBarr);

    /*
    //compute L1-norm (use nghSum as temp array)
    {parallel_for(intT i=0;i<n;i++) {
    nghSum[i] = fabs(Delta[i]);
    }}
    double L1_norm = sequence::plusReduce(nghSum,n);
    //cout<<"L1 norm = "<<L1_norm<<endl;

    if(L1_norm < epsilon) break;
    */
    // vertexMap(reset);
    pthread_barrier_wait(&localBarr);

    switchFrontier(tid, Frontier, output);
    pthread_barrier_wait(&localBarr);
    pthread_barrier_wait(&barr);
  }
  return NULL;
}

struct PR_Hash_F {
  int shardNum;
  intT vertPerShard;
  intT n;
  PR_Hash_F(intT _n, int _shardNum)
      : n(_n), shardNum(_shardNum), vertPerShard(_n / _shardNum) {}

  inline intT hashFunc(intT index) {
    if (index >= shardNum * vertPerShard) {
      return index;
    }
    intT idxOfShard = index % shardNum;
    intT idxInShard = index / shardNum;
    return (idxOfShard * vertPerShard + idxInShard);
  }

  inline intT hashBackFunc(intT index) {
    if (index >= shardNum * vertPerShard) {
      return index;
    }
    intT idxOfShard = index / vertPerShard;
    intT idxInShard = index % vertPerShard;
    return (idxOfShard + idxInShard * shardNum);
  }
};

template <class vertex>
void PageRankDelta(graph<vertex> &GA, int maxIter = -1) {
  const intT n = GA.n;
  const double damping = 0.85;
  const double epsilon = 0.0000001;
  const double epsilon2 = 0.01;
  numOfNode = numa_num_configured_nodes();
  vPerNode = GA.n / numOfNode;
  CORES_PER_NODE = numa_num_configured_cpus() / numOfNode;
  pthread_barrier_init(&barr, NULL, numOfNode);
  pthread_barrier_init(&timerBarr, NULL, numOfNode + 1);
  pthread_barrier_init(&global_barr, NULL, CORES_PER_NODE * numOfNode);
  intT sizeArr[numOfNode];
  PR_Hash_F hasher(GA.n, numOfNode);
  hasher2 = new Default_Hash_F(GA.n, numOfNode);
  graphHasher(GA, hasher);
  partitionByDegree(GA, numOfNode, sizeArr, sizeof(double));
  /*
    for (int i = 0; i < numOfNode; i++) {
    cout << sizeArr[i] << "\n";
    }
    return;
  */
  delta_global = (double *)mapDataArray(numOfNode, sizeArr, sizeof(double));
  nghSum_global = (double *)mapDataArray(numOfNode, sizeArr, sizeof(double));
  p_global = (double *)mapDataArray(numOfNode, sizeArr, sizeof(double));

  printf("start create %d threads\n", numOfNode);
  pthread_t tids[numOfNode];
  intT prev = 0;
  for (int i = 0; i < numOfNode; i++) {
    PR_worker_arg *arg = (PR_worker_arg *)malloc(sizeof(PR_worker_arg));
    arg->GA = (void *)(&GA);
    arg->maxIter = maxIter;
    arg->tid = i;
    arg->numOfNode = numOfNode;
    arg->rangeLow = prev;
    arg->rangeHi = prev + sizeArr[i];
    arg->damping = damping;
    arg->epsilon = epsilon;
    arg->epsilon2 = epsilon2;
    prev = prev + sizeArr[i];
    pthread_create(&tids[i], NULL, PageRankThread<vertex>, (void *)arg);
  }
  shouldStart = 1;
  pthread_barrier_wait(&timerBarr);
  // nextTime("Graph Partition");
  startTime();
  printf("all created\n");
  for (int i = 0; i < numOfNode; i++) {
    pthread_join(tids[i], NULL);
  }
  nextTime("PageRankDelta");
  if (needResult) {
    for (intT i = 0; i < GA.n; i++) {
      cout << i << "\t" << std::scientific << std::setprecision(9)
           << p_global[hasher.hashFunc(i)] << "\n";
    }
  }
}

int parallel_main(int argc, char *argv[]) {
  char *iFile;
  bool binary = false;
  bool symmetric = false;
  int maxIter = -1;
  if (argc > 1) iFile = argv[1];
  if (argc > 2) maxIter = atoi(argv[2]);
  if (argc > 3)
    if ((string)argv[3] == (string) "-result") needResult = true;
  if (argc > 4)
    if ((string)argv[4] == (string) "-s") symmetric = true;
  if (argc > 5)
    if ((string)argv[5] == (string) "-b") binary = true;
  numa_set_interleave_mask(numa_all_nodes_ptr);
  if (symmetric) {
    graph<symmetricVertex> G =
        readGraphSkipRing<symmetricVertex>(iFile, symmetric, binary);
    PageRankDelta(G);
    G.del();
  } else {
    graph<asymmetricVertex> G =
        readGraphSkipRing<asymmetricVertex>(iFile, symmetric, binary);
    printf("read over\n");
    PageRankDelta(G, maxIter);
    G.del();
  }
}
