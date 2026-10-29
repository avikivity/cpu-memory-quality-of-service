# memqos

Measures memory subsystem quality-of-service: how much a latency-bound
workload on one set of CPUs is helped or hurt by what runs on a
neighboring set of CPUs.

## Build

    make

Requires Linux, a C11 compiler and pthreads.

## Usage

    ./memqos -t TEST_CPUS -n NEIGHBOR_CPUS [options]

CPU lists use the usual `0-7,16-23` syntax; the two sets must not overlap.

| option | meaning | default |
|---|---|---|
| `-t, --test CPUS` | cpus running the latency-bound pointer chase | required |
| `-n, --neighbor CPUS` | cpus running the neighbor workload | required |
| `-s, --size SIZE` | dram chase buffer per cpu | 4× L3, min 64M |
| `-l, --l3-size SIZE` | l3 chase buffer per cpu | ¾ × L3 / cpus sharing it |
| `-f, --fill SIZE` | l3 flood buffer per neighbor cpu | L3 / neighbor cpus sharing it |
| `-c, --chains N` | independent chains per cpu in mlp mode | 16 |
| `-d, --duration SEC` | measurement time per scenario | 5 |
| `-w, --warmup SEC` | warmup before each measurement | 1 |
| `-r, --rounds N` | rounds of all scenarios; median reported | 3 |

Sizes accept `K`, `M` and `G` suffixes.

## What it measures

Every test cpu runs a pointer chase: a single random cycle through a
buffer, one cache line per step, each load depending on the previous one.
The time per load is therefore the access latency, with prefetchers
defeated. Two tests target different levels of the hierarchy:

- **dram** – the buffer is well past L3, so every load goes to DRAM.
- **l3** – the buffer is past L2 but small enough that all chasers sharing
  an L3 fit in it, so every load is an L3 hit.

For each test the neighbor cpus run one of:

| scenario | dram test | l3 test |
|---|---|---|
| same | the same DRAM chase | the same L3 chase |
| idle | nothing | nothing |
| noisy | **mlp**: 16 independent DRAM chases per cpu, saturating memory bandwidth | **l3 flood**: streaming stores over buffers that together fill each L3 the neighbors share, evicting the test's working set with dirty lines |

For each test the report gives:

- **base level** – latency with the neighbor running the same workload
- **boost** – `base/idle − 1`, the speedup when the neighbor is idle
- **penalty** – `noisy/base − 1`, the slowdown when the neighbor is noisy

Scenarios are interleaved across rounds so slow drift (thermals, other
system activity) affects all of them alike, and the median round is
reported. The per-round table also shows the spread across test cpus and
the neighbor's throughput. Neighbor GB/s counts cache lines touched, not
DRAM traffic.

## Example

AMD Ryzen 9 5950X (two 32 MiB L3s: cpus 0-7,16-23 and 8-15,24-31), test
cpus sharing an L3 with four of the neighbors, short run:

    $ ./memqos -t 0-3 -n 4-15 -d 1 -w 0.3 -r 1
    ...
      dram test:
        base level (neighbor runs same chase):    114.9 ns   (neighbor 6.56 GB/s)
        idle neighbor:                            102.9 ns   boost    +11.7%
        noisy neighbor (mlp):                     162.3 ns   penalty  +41.2%   (neighbor 26.93 GB/s)

      l3 test:
        base level (neighbor runs same chase):     13.3 ns   (neighbor 65.31 GB/s)
        idle neighbor:                              9.6 ns   boost    +38.7%
        noisy neighbor (l3 flood):                151.9 ns   penalty +1043.0%   (neighbor 731.13 GB/s)

Without cache partitioning, the L3 flood evicts the test's working set
entirely and its L3 chase runs at DRAM latency.

## Notes

- **Huge pages.** Buffers are allocated with `MADV_HUGEPAGE` and then
  populated with `MADV_POPULATE_WRITE`, so TLB misses don't inflate the
  latency. Transparent huge pages must be set to `madvise` or `always`
  (`/sys/kernel/mm/transparent_hugepage/enabled`). The tool reports huge
  page coverage and warns if it is low; compacting memory
  (`echo 1 > /proc/sys/vm/compact_memory`) usually helps.
- **NUMA.** Each buffer is allocated and touched by a thread pinned to the
  cpu that uses it, so memory is local to that cpu's node.
- **Memory use** is roughly the dram buffer size times the total number of
  cpus. On large machines with big L3s, set `-s` explicitly.
- **Idle boost includes frequency effects.** When neighbor cores sleep, the
  test cores may get more turbo headroom; that is part of what an idle
  neighbor provides but is not purely a memory effect.
- **L3 vs L2.** On parts where a core's share of L3 is close to its L2
  size, the l3 chase can't be both L3-resident and past L2; the tool warns
  when the buffer is less than twice L2.
- **Cache allocation.** On CPUs with L3 partitioning (Intel RDT / AMD
  PQoS, via resctrl), running the neighbors in a restricted class of
  service shows how much the partitioning recovers.
