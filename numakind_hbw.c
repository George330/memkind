/*
 * Copyright (2014) Intel Corporation All Rights Reserved.
 *
 * This software is supplied under the terms of a license
 * agreement or nondisclosure agreement with Intel Corp.
 * and may not be copied or disclosed except in accordance
 * with the terms of that agreement.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <sys/mman.h>
#include <pthread.h>
#include <numa.h>
#include <numaif.h>
#include <sys/types.h>
#include <jemalloc/jemalloc.h>
#define _GNU_SOURCE
#include <utmpx.h>

#include "numakind_hbw.h"

struct numanode_bandwidth_t {
    int numanode;
    int bandwidth;
};

struct bandwidth_nodes_t {
    int bandwidth;
    int num_numanodes;
    int *numanodes;
};

struct numakind_hbw_closest_numanode_t {
    int init_err;
    int num_cpu;
    int *closest_numanode;
};

static struct numakind_hbw_closest_numanode_t numakind_hbw_closest_numanode_g;
static pthread_once_t numakind_hbw_closest_numanode_once_g = PTHREAD_ONCE_INIT;

static void numakind_hbw_closest_numanode_init(void);

static int parse_node_bandwidth(int num_bandwidth, int *bandwidth,
                                const char *bandwidth_path);

static int create_bandwidth_nodes(int num_bandwidth, const int *bandwidth,
                                  int *num_unique, struct bandwidth_nodes_t **bandwidth_nodes);

static int set_closest_numanode(int num_unique,
                                const struct bandwidth_nodes_t *bandwidth_nodes,
                                int target_bandwidth, int num_cpunode, int *closest_numanode);

static int numanode_bandwidth_compare(const void *a, const void *b);

int numakind_hbw_is_available(struct numakind *kind)
{
    int err;
    err = kind->ops->get_mbind_nodemask(kind, NULL, 0);
    return (!err);
}

int numakind_hbw_hugetlb_get_mmap_flags(struct numakind *kind, int *flags)
{
    *flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB;
    return 0;
}

int numakind_hbw_preferred_get_mbind_mode(struct numakind *kind, int *mode)
{
    *mode = MPOL_PREFERRED;
    return 0;
}

int numakind_hbw_get_mbind_nodemask(struct numakind *kind,
                                    unsigned long *nodemask, unsigned long maxnode)
{
    int cpu;
    struct bitmask nodemask_bm = {maxnode, nodemask};
    struct numakind_hbw_closest_numanode_t *g =
                &numakind_hbw_closest_numanode_g;
    pthread_once(&numakind_hbw_closest_numanode_once_g,
                 numakind_hbw_closest_numanode_init);

    if (!g->init_err && nodemask) {
        numa_bitmask_clearall(&nodemask_bm);
        cpu = sched_getcpu();
        if (cpu < g->num_cpu) {
            numa_bitmask_setbit(&nodemask_bm, g->closest_numanode[cpu]);
        }
        else {
            return NUMAKIND_ERROR_GETCPU;
        }
    }
    return g->init_err;
}

static void numakind_hbw_closest_numanode_init(void)
{
    struct numakind_hbw_closest_numanode_t *g =
                &numakind_hbw_closest_numanode_g;
    int *bandwidth = NULL;
    int num_unique = 0;
    int high_bandwidth = 0;
    int node;
    struct bandwidth_nodes_t *bandwidth_nodes = NULL;
    char *hbw_nodes_env;
    struct bitmask *hbw_nodes_bm;

    g->num_cpu = numa_num_configured_cpus();
    g->closest_numanode = (int *)je_malloc(sizeof(int) * g->num_cpu);
    bandwidth = (int *)je_malloc(sizeof(int) * NUMA_NUM_NODES);
    if (!(g->closest_numanode && bandwidth)) {
        g->init_err = NUMAKIND_ERROR_MALLOC;
    }
    if (!g->init_err) {
        hbw_nodes_env = getenv("NUMAKIND_HBW_NODES");
        if (hbw_nodes_env) {
            hbw_nodes_bm = numa_parse_nodestring(hbw_nodes_env);
            if (!hbw_nodes_bm) {
                g->init_err = NUMAKIND_ERROR_ENVIRON;
            }
            else {
                for (node = 0; node < NUMA_NUM_NODES; ++node) {
                    if (numa_bitmask_isbitset(hbw_nodes_bm, node)) {
                        bandwidth[node] = 2;
                    }
                    else {
                        bandwidth[node] = 1;
                    }
                }
                numa_bitmask_free(hbw_nodes_bm);
            }
        }
        else {
            g->init_err = parse_node_bandwidth(NUMA_NUM_NODES, bandwidth,
                                               NUMAKIND_BANDWIDTH_PATH);
        }
    }
    if (!g->init_err) {
        g->init_err = create_bandwidth_nodes(NUMA_NUM_NODES, bandwidth,
                                             &num_unique, &bandwidth_nodes);
    }
    if (!g->init_err) {
        if (num_unique == 1) {
            g->init_err = NUMAKIND_ERROR_UNAVAILABLE;
        }
    }
    if (!g->init_err) {
        high_bandwidth = bandwidth_nodes[num_unique-1].bandwidth;
        g->init_err = set_closest_numanode(num_unique, bandwidth_nodes,
                                           high_bandwidth, g->num_cpu,
                                           g->closest_numanode);
    }
    if (bandwidth_nodes) {
        je_free(bandwidth_nodes);
    }
    if (bandwidth) {
        je_free(bandwidth);
    }
    if (g->init_err) {
        if (g->closest_numanode) {
            je_free(g->closest_numanode);
            g->closest_numanode = NULL;
        }
    }
}

static int parse_node_bandwidth(int num_bandwidth, int *bandwidth,
                                const char *bandwidth_path)
{
    FILE *fid;
    size_t nread;
    int err = 0;
    fid = fopen(bandwidth_path, "r");
    if (!fid) {
        err = NUMAKIND_ERROR_PMTT;
    }
    if (!err) {
        nread = fread(bandwidth, sizeof(int), num_bandwidth, fid);
        if (nread != num_bandwidth) {
            err = NUMAKIND_ERROR_PMTT;
        }
    }
    return err;
}

static int create_bandwidth_nodes(int num_bandwidth, const int *bandwidth,
                                  int *num_unique, struct bandwidth_nodes_t **bandwidth_nodes)
{
    /***************************************************************************
    *   num_bandwidth (IN):                                                    *
    *       number of numa nodes and length of bandwidth vector.               *
    *   bandwidth (IN):                                                        *
    *       A vector of length num_bandwidth that gives bandwidth for          *
    *       each numa node, zero if numa node has unknown bandwidth.           *
    *   num_unique (OUT):                                                      *
    *       number of unique non-zero bandwidth values in bandwidth            *
    *       vector.                                                            *
    *   bandwidth_nodes (OUT):                                                 *
    *       A list of length num_unique sorted by bandwidth value where        *
    *       each element gives a list of the numa nodes that have the          *
    *       given bandwidth.                                                   *
    *   RETURNS zero on success, error code on failure                         *
    ***************************************************************************/
    int err = 0;
    int i, j, k, l, last_bandwidth;
    struct numanode_bandwidth_t *numanode_bandwidth = NULL;
    *bandwidth_nodes = NULL;
    /* allocate space for sorting array */
    numanode_bandwidth = je_malloc(sizeof(struct numanode_bandwidth_t) *
                                   num_bandwidth);
    if (!numanode_bandwidth) {
        err = NUMAKIND_ERROR_MALLOC;
    }
    if (!err) {
        /* set sorting array */
        j = 0;
        for (i = 0; i < num_bandwidth; ++i) {
            if (bandwidth[i] != 0) {
                numanode_bandwidth[j].numanode = i;
                numanode_bandwidth[j].bandwidth = bandwidth[i];
                ++j;
            }
        }
        /* ignore zero bandwidths */
        num_bandwidth = j;
        if (num_bandwidth == 0) {
            err = NUMAKIND_ERROR_PMTT;
        }
    }
    if (!err) {
        qsort(numanode_bandwidth, num_bandwidth,
              sizeof(struct numanode_bandwidth_t), numanode_bandwidth_compare);
        /* calculate the number of unique bandwidths */
        *num_unique = 1;
        last_bandwidth = numanode_bandwidth[0].bandwidth;
        for (i = 1; i < num_bandwidth; ++i) {
            if (numanode_bandwidth[i].bandwidth != last_bandwidth) {
                last_bandwidth = numanode_bandwidth[i].bandwidth;
                ++*num_unique;
            }
        }
        /* allocate output array */
        *bandwidth_nodes = (struct bandwidth_nodes_t*)je_malloc(
                               sizeof(struct bandwidth_nodes_t) **num_unique +
                               sizeof(int) * num_bandwidth);
        if (!*bandwidth_nodes) {
            err = NUMAKIND_ERROR_MALLOC;
        }
    }
    if (!err) {
        /* populate output */
        (*bandwidth_nodes)[0].numanodes = (int*)(*bandwidth_nodes + *num_unique);
        last_bandwidth = numanode_bandwidth[0].bandwidth;
        k = 0;
        l = 0;
        for (i = 0; i < num_bandwidth; ++i, ++l) {
            (*bandwidth_nodes)[0].numanodes[i] = numanode_bandwidth[i].numanode;
            if (numanode_bandwidth[i].bandwidth != last_bandwidth) {
                (*bandwidth_nodes)[k].num_numanodes = l;
                (*bandwidth_nodes)[k].bandwidth = last_bandwidth;
                l = 0;
                ++k;
                (*bandwidth_nodes)[k].numanodes = (*bandwidth_nodes)[0].numanodes + i;
                last_bandwidth = numanode_bandwidth[i].bandwidth;
            }
        }
        (*bandwidth_nodes)[k].num_numanodes = l;
        (*bandwidth_nodes)[k].bandwidth = last_bandwidth;
    }
    if (numanode_bandwidth) {
        je_free(numanode_bandwidth);
    }
    if (err) {
        if (*bandwidth_nodes) {
            je_free(*bandwidth_nodes);
        }
    }
    return err;
}

static int set_closest_numanode(int num_unique,
                                const struct bandwidth_nodes_t *bandwidth_nodes,
                                int target_bandwidth, int num_cpunode, int *closest_numanode)
{
    /***************************************************************************
    *   num_unique (IN):                                                       *
    *       Length of bandwidth_nodes vector.                                  *
    *   bandwidth_nodes (IN):                                                  *
    *       Output vector from create_bandwitdth_nodes().                      *
    *   target_bandwidth (IN):                                                 *
    *       The bandwidth to select for comparison.                            *
    *   num_cpunode (IN):                                                      *
    *       Number of cpu's and length of closest_numanode.                    *
    *   closest_numanode (OUT):                                                *
    *       Vector that maps cpu index to closest numa node of the specified   *
    *       bandwidth.                                                         *
    *   RETURNS zero on success, error code on failure                         *
    ***************************************************************************/

    int err = 0;
    int min_distance, distance, i, j;
    struct bandwidth_nodes_t match;
    match.bandwidth = -1;
    for (i = 0; i < num_cpunode; ++i) {
        closest_numanode[i] = -1;
    }
    for (i = 0; i < num_unique; ++i) {
        if (bandwidth_nodes[i].bandwidth == target_bandwidth) {
            match = bandwidth_nodes[i];
            break;
        }
    }
    if (match.bandwidth == -1) {
        err = NUMAKIND_ERROR_PMTT;
    }
    else {
        for (i = 0; i < num_cpunode; ++i) {
            min_distance = INT_MAX;
            for (j = 0; j < match.num_numanodes; ++j) {
                distance = numa_distance(numa_node_of_cpu(i),
                                         match.numanodes[j]);
                if (distance < min_distance) {
                    min_distance = distance;
                    closest_numanode[i] = match.numanodes[j];
                }
                else if (distance == min_distance) {
                    err = NUMAKIND_ERROR_TIEDISTANCE;
                }
            }
        }
    }
    return err;
}

static int numanode_bandwidth_compare(const void *a, const void *b)
{
    /***************************************************************************
    *  qsort comparison function for numa_node_bandwidth structures.  Sorts in *
    *  order of bandwidth and then numanode.                                   *
    ***************************************************************************/
    int result;
    struct numanode_bandwidth_t *aa = (struct numanode_bandwidth_t *)(a);
    struct numanode_bandwidth_t *bb = (struct numanode_bandwidth_t *)(b);
    result = (aa->bandwidth > bb->bandwidth) - (aa->bandwidth < bb->bandwidth);
    if (result == 0) {
        result = (aa->numanode > bb->numanode) - (aa->numanode < bb->numanode);
    }
    return result;
}

