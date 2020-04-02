/*************************************************************************
 * Copyright (c) 2016-2020, NVIDIA CORPORATION. All rights reserved.
 * Modifications Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All rights reserved.
 *
 * See LICENSE.txt for license information
 ************************************************************************/

#include "core.h"
#include "devcomm.h"
#include "comm.h"
#include "topo.h"

NCCL_PARAM(Nthreads, "NTHREADS", -2);
NCCL_PARAM(Ll128Nthreads, "LL128_NTHREADS", -2);

static int getNthreads(const char* name, int env, int min, int max, int def) {
  int nt = env;
  if (nt > 0) {
    if (nt % WARP_SIZE != 0) {
      WARN("Invalid %s %d (must be a multiple of %d)", name, nt, WARP_SIZE);
      nt = max;
    } else if (nt > max) {
      WARN("Invalid %s %d (maximum %d).", name, nt, max);
      nt = max;
    } else if (nt < min) {
      WARN("Invalid %s %d (minimum %d).", name, nt, min);
      nt = min;
     }
  } else {
    nt = def;
  }
  return nt;
}

ncclResult_t parseList(const char* str, const char* elems[], int nelems, int* list) {
  int def, set;
  if (str[0] == '^') {
    def = 1; set = 0; str++;
  } else {
    def = 0; set = 1;
  }
  for (int i=0; i<nelems; i++) list[i] = def;
  char* tokStr = strdup(str);
  char* tmpStr;
  char* token = strtok_r(tokStr, ",", &tmpStr);
  while (token) {
    for (int i=0; i<nelems; i++)
      if (strcasecmp(token, elems[i]) == 0) list[i] = set;
    token = strtok_r(NULL, ",", &tmpStr);
  }
  free(tokStr);
  return ncclSuccess;
}

static const char* ncclFuncStr[] = { "Broadcast", "Reduce", "AllGather", "ReduceScatter", "AllReduce" };
static const char* ncclAlgoStr[] = { "Tree", "Ring", "CollNet" };
static const char* ncclProtoStr[] = { "LL", "LL128", "Simple" };

// Latencies in us, Bandwidths in GB/s
// Tree { LL, LL128, Simple } , Ring { LL, LL128, Simple }
static const float baseLat  [NCCL_NUM_ALGORITHMS][NCCL_NUM_PROTOCOLS] = { { 37.9, 37.9, 40.4 }, { 20.5, 20.5, 27.9 }, { 37.9, 37.9, 40.4 } };

// NVLink, PCI, Network
#define NCCL_HW_NVLINK 0
#define NCCL_HW_PCI 1
#define NCCL_HW_NET 2
// Tree/Simple is the latency a 256kB chunk, which is ~ base lat + 256k/12GB/s (+ 256k/12GB/s for the network).
static const float hwLat [3][NCCL_NUM_ALGORITHMS][NCCL_NUM_PROTOCOLS] =
{ /* NVLINK */
  { /* Tree (LL/LL128/Simple)*/ { 1.2, 1.2, 3.8 }, /* Ring (LL/LL128/Simple)*/ { 2.3, 2.3, 2.7 }, /* CollNet (LL/LL128/Simple)*/ { 1.2, 1.2, 3.8 } },
  /* PCI */
  { /* Tree (LL/LL128/Simple)*/ { 2.2, 2.2, 5.7 }, /* Ring (LL/LL128/Simple)*/ { 1.3, 1.3, 1.9 }, /* CollNet (LL/LL128/Simple)*/ { 2.2, 2.2, 5.7 } },
  /* NET */
  { /* Tree (LL/LL128/Simple)*/ { 9.8, 9.8, 19.5 }, /* Ring (LL/LL128/Simple)*/ { 2.0, 2.0, 4.5 }, /* CollNet (LL/LL128/Simple)*/ { 9.8, 9.8, 19.5 } }
};

// LL128 max BW for the different collectives
static const double ll128MaxBw[NCCL_NUM_FUNCTIONS] = { 113.0, 72.0, 110.0, 91.0, 100.0 };

ncclResult_t ncclTopoSetThresholds(struct ncclComm* comm, int minCompCap, int maxCompCap, struct ncclTopoGraph* treeGraph, struct ncclTopoGraph* ringGraph, struct ncclTopoGraph* collNetGraph) {
  int simpleDefaultThreads = (ringGraph->speedIntra*ringGraph->nChannels <= PCI_WIDTH) ? 256 : NCCL_MAX_NTHREADS;
  comm->maxThreads[NCCL_ALGO_RING][NCCL_PROTO_SIMPLE] =
    getNthreads("NCCL_NTHREADS", ncclParamNthreads(), 4*WARP_SIZE, NCCL_MAX_NTHREADS, simpleDefaultThreads);
  comm->maxThreads[NCCL_ALGO_TREE][NCCL_PROTO_SIMPLE] = comm->maxThreads[NCCL_ALGO_COLLNET][NCCL_PROTO_SIMPLE] =
    getNthreads("NCCL_NTHREADS", ncclParamNthreads(), 4*WARP_SIZE, NCCL_MAX_NTHREADS, NCCL_MAX_NTHREADS);
  comm->maxThreads[NCCL_ALGO_RING][NCCL_PROTO_LL] = comm->maxThreads[NCCL_ALGO_TREE][NCCL_PROTO_LL] = comm->maxThreads[NCCL_ALGO_COLLNET][NCCL_PROTO_LL] =
    getNthreads("NCCL_NTHREADS", ncclParamNthreads(), 4*WARP_SIZE, NCCL_MAX_NTHREADS, NCCL_MAX_NTHREADS);
  comm->maxThreads[NCCL_ALGO_RING][NCCL_PROTO_LL128] = comm->maxThreads[NCCL_ALGO_TREE][NCCL_PROTO_LL128] = comm->maxThreads[NCCL_ALGO_COLLNET][NCCL_PROTO_LL128] =
    getNthreads("NCCL_LL128_NTHREADS", ncclParamLl128Nthreads(), NCCL_LL128_MAX_NTHREADS/4, NCCL_LL128_MAX_NTHREADS, NCCL_LL128_MAX_NTHREADS);

  if (comm->nRanks <= 1) return ncclSuccess;

  struct ncclTopoGraph* graphs[NCCL_NUM_ALGORITHMS] = { treeGraph, ringGraph, collNetGraph };
  int intraHw[NCCL_NUM_ALGORITHMS], hw[NCCL_NUM_ALGORITHMS];
  for (int a=0; a<NCCL_NUM_ALGORITHMS; a++) intraHw[a] = graphs[a]->typeIntra == LINK_NVL ? NCCL_HW_NVLINK : NCCL_HW_PCI;
  for (int a=0; a<NCCL_NUM_ALGORITHMS; a++) hw[a] = comm->nNodes == 1 ? intraHw[a] : NCCL_HW_NET;

  for (int coll=0; coll<NCCL_NUM_FUNCTIONS; coll++) {
    int nsteps = coll == ncclCollAllReduce ? 2*(comm->nRanks-1) :
      coll == ncclCollReduceScatter || coll == ncclCollAllGather ? comm->nRanks-1 :
      comm->nRanks;

    for (int a=0; a<NCCL_NUM_ALGORITHMS; a++) {
      if (coll != ncclCollAllReduce && a != NCCL_ALGO_RING) continue;

      for (int p=0; p<NCCL_NUM_PROTOCOLS; p++) {
        float speed = comm->nNodes <= 2 || a == NCCL_ALGO_COLLNET ? graphs[a]->speedIntra : graphs[a]->speedInter;
        float busBw = graphs[a]->nChannels * speed;

        // Various model refinements
        if (a == NCCL_ALGO_RING && p == NCCL_PROTO_LL)    busBw *= 1.0/5.0;
        if (a == NCCL_ALGO_RING && p == NCCL_PROTO_LL128) busBw = std::min(busBw*120.0/128.0, ll128MaxBw[coll]);
        if (a == NCCL_ALGO_TREE) busBw = std::min(busBw*.27, comm->nNodes > 1 ? 70.0 : 90.0);
        if (a == NCCL_ALGO_TREE && p == NCCL_PROTO_LL) busBw *= 1.0/2.3;
        if (a == NCCL_ALGO_TREE && p == NCCL_PROTO_LL128) busBw *= 7.0/9.0;
        if (a == NCCL_ALGO_COLLNET) busBw *= .9;
        if (a == NCCL_ALGO_COLLNET && p == NCCL_PROTO_LL) busBw *= 1.0/6.0; // Take into account that GDR read is disabled on both sides
        if (a == NCCL_ALGO_COLLNET && p == NCCL_PROTO_LL128) busBw = 0;  // CollNet does not support LL128

        // Convert bus BW to algorithm BW
        float ratio = (a != NCCL_ALGO_RING) ? .5 : (1.0 * comm->nRanks) / nsteps;
        comm->bandwidths[coll][a][p] = busBw * ratio;

        comm->latencies[coll][a][p] = baseLat[a][p];
        if (a == NCCL_ALGO_RING) {
          float lat = hwLat[hw[a]][a][p];
          if ((coll == ncclCollReduce || coll == ncclCollBroadcast)) {
            if (ringGraph->sameChannels) {
              comm->latencies[coll][a][p] += lat;
            } else {
              if (p == NCCL_PROTO_SIMPLE) lat = hwLat[hw[a]][NCCL_ALGO_TREE][p]; // Add some chunk latency, waiting for proper chunk modeling
              comm->latencies[coll][a][p] += nsteps*lat;
            }
          } else {
            comm->latencies[coll][a][p] += nsteps*lat;
          }
        } else if (a == NCCL_ALGO_TREE) {
          float intraLat = hwLat[intraHw[a]][a][p];
          float interLat = hwLat[NCCL_HW_NET][a][p];
          comm->latencies[coll][a][p] +=
            2 * ((comm->nRanks/comm->nNodes-1) * intraLat + log2i(comm->nNodes) * interLat);
        } else {
          float intraLat = hwLat[intraHw[a]][a][p];
          float interLat = hwLat[NCCL_HW_NET][a][p];
          comm->latencies[coll][a][p] +=
            2 * (comm->nRanks/comm->nNodes-1) * intraLat + interLat;
        }
      }
    }
  }

  // Protocols/Algorithms enable/disable, and user overrides.
  // All are enabled except ll128 which is enabled by default only in certain cases.
  int protoEnable[NCCL_NUM_PROTOCOLS] = { 1, 2, 1 };
  int algoEnable[NCCL_NUM_ALGORITHMS] = { 1, 1, 1 };

  const char *protoStr = getenv("NCCL_PROTO");
  if (protoStr) NCCLCHECK(parseList(protoStr, ncclProtoStr, NCCL_NUM_PROTOCOLS, protoEnable));
  const char *algoStr = getenv("NCCL_ALGO");
  if (algoStr) NCCLCHECK(parseList(algoStr, ncclAlgoStr, NCCL_NUM_ALGORITHMS, algoEnable));

  for (int c=0; c<NCCL_NUM_FUNCTIONS; c++) for (int a=0; a<NCCL_NUM_ALGORITHMS; a++) for (int p=0; p<NCCL_NUM_PROTOCOLS; p++) {
    int pEnable = protoEnable[p];
    if (pEnable == 2 && p == NCCL_PROTO_LL128) {
      // Enable LL128 by default only on Volta+NVLink. Other cases are not tested and may cause silent data corruption.
      pEnable = (graphs[a]->typeInter <= LINK_PCI) && graphs[a]->typeIntra == LINK_NVL && minCompCap == 70 && maxCompCap == 70 ? 1 : 0;
    }
    if (pEnable == 0 || algoEnable[a] == 0) comm->bandwidths[c][a][p] = 0;
  }

  if (comm->rank == 0) {
    char line[1024];
    sprintf(line, "Latency/AlgBw |");
    for (int a=0; a<NCCL_NUM_ALGORITHMS; a++) {
      for (int p=0; p<NCCL_NUM_PROTOCOLS; p++) {
        sprintf(line+strlen(line), " %7s/%6s |", ncclAlgoStr[a], ncclProtoStr[p]);
      }
    }
    INFO(NCCL_TUNING, "%s", line);
    sprintf(line, " Max NThreads |");
    for (int a=0; a<NCCL_NUM_ALGORITHMS; a++) {
      for (int p=0; p<NCCL_NUM_PROTOCOLS; p++) {
        sprintf(line+strlen(line), " %14d |", comm->maxThreads[a][p]);
      }
    }
    INFO(NCCL_TUNING, "%s", line);
    for (int c=0; c<NCCL_NUM_FUNCTIONS; c++) {
      sprintf(line, "%13s |", ncclFuncStr[c]);
      for (int a=0; a<NCCL_NUM_ALGORITHMS; a++) {
        for (int p=0; p<NCCL_NUM_PROTOCOLS; p++) {
          sprintf(line+strlen(line), "%8.1f/%6.1f |", comm->latencies[c][a][p], comm->bandwidths[c][a][p]);
        }
      }
      INFO(NCCL_TUNING, "%s", line);
    }
  }

  // Set per-thread amount of work before we increase nThreads and nChannels
  for (int a=0; a<NCCL_NUM_ALGORITHMS; a++) {
    comm->threadThresholds[a][NCCL_PROTO_LL] = NCCL_LL_THREAD_THRESHOLD;
    comm->threadThresholds[a][NCCL_PROTO_LL128] = NCCL_LL128_THREAD_THRESHOLD;
    comm->threadThresholds[a][NCCL_PROTO_SIMPLE] = NCCL_SIMPLE_THREAD_THRESHOLD;
  }
  comm->threadThresholds[NCCL_ALGO_RING][NCCL_PROTO_LL] *= comm->nRanks;

  // Override defaults with user env
  char* str = getenv("NCCL_THREAD_THRESHOLDS");
  if (str) {
    ssize_t t[NCCL_NUM_ALGORITHMS][NCCL_NUM_PROTOCOLS] = {{ -2, -2, -2 }, { -2, -2, -2}};
    sscanf(str, "%ld %ld %ld %ld %ld %ld", t[0], t[0]+1, t[0]+2, t[1], t[1]+1, t[1]+2);
    for (int a=0; a<NCCL_NUM_ALGORITHMS; a++) {
      for (int p=0; p<NCCL_NUM_PROTOCOLS; p++) {
        if (t[a][p] >= 0) comm->threadThresholds[a][p] = t[a][p];
      }
    }
  }

  INFO(NCCL_INIT, "threadThresholds %ld/%ld/%ld | %ld/%ld/%ld | %ld/%ld/%ld",
      comm->threadThresholds[NCCL_ALGO_TREE][NCCL_PROTO_LL],
      comm->threadThresholds[NCCL_ALGO_TREE][NCCL_PROTO_LL128],
      comm->threadThresholds[NCCL_ALGO_TREE][NCCL_PROTO_SIMPLE],
      comm->threadThresholds[NCCL_ALGO_RING][NCCL_PROTO_LL],
      comm->threadThresholds[NCCL_ALGO_RING][NCCL_PROTO_LL128],
      comm->threadThresholds[NCCL_ALGO_RING][NCCL_PROTO_SIMPLE],
      comm->threadThresholds[NCCL_ALGO_COLLNET][NCCL_PROTO_LL],
      comm->threadThresholds[NCCL_ALGO_COLLNET][NCCL_PROTO_LL128],
      comm->threadThresholds[NCCL_ALGO_COLLNET][NCCL_PROTO_SIMPLE]);
  return ncclSuccess;
}

// Trees are not perfectly sticking to the model for medium sizes. Applying a static correction
// factor is not ideal but works quite well. Powers of two, 64 B to 1 GB.
static float treeCorrectionFactor[NCCL_NUM_PROTOCOLS][22] = {
  {  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  .84,  .49,  .42,  .60,  .75,  .87,  .94,  .94,  .99,  1.0,  1.0 ,  1.0 ,  1.0 ,  1.0 ,  1.0 },
  {  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  .84,  .49,  .42,  .60,  .75,  .87,  .94,  .94,  .99,  1.0,  1.0 ,  1.0 ,  1.0 ,  1.0 ,  1.0 },
  {  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  .41,  .27,  .25,  .39,  .46,  .72,  .76,  .87,  .92,  .97,  1.0,  1.0 ,  1.0 ,  1.0 ,  1.0 ,  1.0 }
};

static float ringCorrectionFactor[NCCL_NUM_PROTOCOLS][22] = {
  {  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  .25,  .41,  .55,  .56,  .78,  .94,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0 ,  1.0 ,  1.0 ,  1.0 ,  1.0 },
  {  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  .25,  .41,  .55,  .56,  .78,  .94,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0 ,  1.0 ,  1.0 ,  1.0 ,  1.0 },
  {  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  .04,  .08,  .09,  .09,  .11,  .13,  .25,  .40,  .59,  .76,  .86,  1.0 ,  1.0 ,  1.0 ,  1.0 ,  1.0 }
};

ncclResult_t ncclTopoGetAlgoTime(struct ncclInfo* info, int algorithm, int protocol, float* time) {
  float bw = info->comm->bandwidths[info->coll][algorithm][protocol];
  if (bw == 0) {
    *time = -1.0; return ncclSuccess;
  }
  int logSize = log2i(info->nBytes>>6);
  if (algorithm == NCCL_ALGO_TREE && logSize < 22) bw *= treeCorrectionFactor[protocol][logSize];
  else if (algorithm == NCCL_ALGO_RING && logSize < 22) bw *= ringCorrectionFactor[protocol][logSize];
  *time = info->comm->latencies[info->coll][algorithm][protocol] + (info->nBytes) / (1000 * bw);
  return ncclSuccess;
}
