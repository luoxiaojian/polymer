#include <sys/stat.h>
#include <sys/types.h>

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "gettime.h"
#include "ligra.h"
#include "math.h"

using namespace std;

#define PAGESIZE (4096)

bool needOutDegree = false;

char *fileName;

int partitionNum = 1;

template <class vertex>
void convertToBin(graph<vertex> GA, int numOfShards) {
  const intT n = GA.n;

  if (numOfShards == 1) {
    long long totalSize = 0;
    for (intT i = 0; i < n; i++) {
      totalSize += 4 * sizeof(intT);  // const -1, vertID, outDeg, inDeg
      totalSize += GA.V[i].getOutDegree() * sizeof(intE);
      totalSize += GA.V[i].getInDegree() * sizeof(intE);
    }
    totalSize += sizeof(intT) + sizeof(long long) * 2;

    void *buf = (void *)malloc(totalSize);

    printf("Total size is: %lld %p\n", totalSize, buf);

    char *ptr = (char *)buf;

    *(long long *)ptr = totalSize;
    ptr += sizeof(long long);

    *(intT *)ptr = n;
    ptr += sizeof(intT);

    *(long long *)ptr = GA.m;
    ptr += sizeof(long long);

    printf("write n & m: %" PRIintT " %" PRIintT "\n", n, GA.m);

    for (intT i = 0; i < n; i++) {
      *(intT *)ptr = -1;
      ptr += sizeof(intT);

      *(intT *)ptr = i;
      ptr += sizeof(intT);

      *(intT *)ptr = GA.V[i].getOutDegree();
      ptr += sizeof(intT);

      *(intT *)ptr = GA.V[i].getInDegree();
      ptr += sizeof(intT);

      for (intT j = 0; j < GA.V[i].getOutDegree(); j++) {
        *(intE *)ptr = GA.V[i].getOutNeighbor(j);
        ptr += sizeof(intE);
      }

      for (intT j = 0; j < GA.V[i].getInDegree(); j++) {
        *(intE *)ptr = GA.V[i].getInNeighbor(j);
        ptr += sizeof(intE);
      }
    }

    long long sizeCheck = (long long)ptr - (long long)buf;

    printf("size check: %lld %lld\n", sizeCheck, totalSize);

    int fd = open(fileName, O_RDWR | O_CREAT, S_IWRITE | S_IREAD);
    long long written = 0;
    while (written < totalSize) {
      long long sizeWritten =
          write(fd, (void *)((char *)buf + written), totalSize - written);
      if (sizeWritten < 0) {
        printf("oops\n");
      }
      written += sizeWritten;
      printf("wrote: %lld, accum: %lld\n", sizeWritten, written);
    }
  }
}

int parallel_main(int argc, char *argv[]) {
  char *iFile;
  needOutDegree = false;
  if (argc > 1) iFile = argv[1];
  if (argc > 2) fileName = argv[2];
  if (argc > 3) partitionNum = atoi(argv[3]);

  bool symmetric = false;
  bool binary = false;

  if (symmetric) {
    graph<symmetricVertex> G =
        readGraph<symmetricVertex>(iFile, symmetric, binary);
    convertToBin(G, partitionNum);
    G.del();
  } else {
    graph<asymmetricVertex> G =
        readGraph<asymmetricVertex>(iFile, symmetric, binary);
    convertToBin(G, partitionNum);
    G.del();
  }
}

