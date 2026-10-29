/*
 * memqos - measure memory subsystem quality-of-service under neighbor load.
 *
 * A latency-bound pointer-chasing workload runs on every CPU of the "test"
 * cpuset.  Meanwhile, the "neighbor" cpuset runs one of:
 *
 *   same  - the same latency-bound pointer chase   (baseline)
 *   idle  - nothing                                 (best case)
 *   mlp   - many independent pointer chases per CPU (noisy neighbor, high
 *           memory-level parallelism, saturates the memory subsystem)
 *
 * All buffers are sized well past L3, so every load in the chase is a
 * DRAM access whose latency is fully exposed.  The report gives the
 * baseline latency, the boost the test set gets when its neighbor is idle,
 * and the penalty it suffers when its neighbor is noisy.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define LINE 64
#define HUGE_PAGE (2UL << 20)
#define CHUNK 256 /* chase steps between counter publications */

enum scenario { SC_SAME, SC_IDLE, SC_MLP, NR_SCENARIOS };
static const char *scenario_name[] = { "same", "idle", "mlp" };

enum role { ROLE_CHASE, ROLE_MLP };

struct worker {
	int cpu;
	char *buf;
	size_t lines;
	size_t huge; /* bytes of buf backed by huge pages */
	void **starts; /* mlp_chains evenly spaced positions along the cycle */
	enum role role;
	pthread_t thread;
	/* Written by the worker, read by main; on its own cache line. */
	_Alignas(LINE) _Atomic uint64_t loads;
	void *sink;
};

static size_t buf_size;
static unsigned mlp_chains = 16;
static double duration = 5.0;
static double warmup = 1.0;
static unsigned rounds = 3;
static _Atomic bool stop;

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void sleep_for(double sec)
{
	struct timespec ts = { .tv_sec = (time_t)sec,
			       .tv_nsec = (long)((sec - (time_t)sec) * 1e9) };
	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

static void pin(int cpu)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	int r = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
	if (r)
		die("cannot pin to cpu %d: %s", cpu, strerror(r));
}

static uint64_t xorshift64(uint64_t *s)
{
	uint64_t x = *s;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return *s = x;
}

/* Parse a cpu list such as "0-3,8,10-11" into an array of cpu numbers. */
static int parse_cpuset(const char *s, int **out)
{
	int cap = 16, n = 0;
	int *cpus = malloc(cap * sizeof(*cpus));
	const char *p = s;

	while (*p) {
		char *end;
		long a = strtol(p, &end, 10), b = a;
		if (end == p || a < 0)
			die("bad cpuset '%s'", s);
		p = end;
		if (*p == '-') {
			b = strtol(p + 1, &end, 10);
			if (end == p + 1 || b < a)
				die("bad cpuset '%s'", s);
			p = end;
		}
		for (long c = a; c <= b; c++) {
			if (n == cap)
				cpus = realloc(cpus, (cap *= 2) * sizeof(*cpus));
			cpus[n++] = (int)c;
		}
		if (*p == ',')
			p++;
		else if (*p)
			die("bad cpuset '%s'", s);
	}
	if (!n)
		die("empty cpuset '%s'", s);
	*out = cpus;
	return n;
}

static size_t l3_size(int cpu)
{
	char path[128], buf[64];
	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cache/index3/size", cpu);
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	size_t sz = 0;
	if (fgets(buf, sizeof(buf), f)) {
		char *end;
		sz = strtoull(buf, &end, 10);
		if (*end == 'K')
			sz <<= 10;
		else if (*end == 'M')
			sz <<= 20;
	}
	fclose(f);
	return sz;
}

static size_t parse_size(const char *s)
{
	char *end;
	size_t v = strtoull(s, &end, 10);
	switch (*end) {
	case 'g': case 'G': v <<= 10; /* fallthrough */
	case 'm': case 'M': v <<= 10; /* fallthrough */
	case 'k': case 'K': v <<= 10; break;
	case '\0': break;
	default: die("bad size '%s'", s);
	}
	return v;
}

/* AnonHugePages of the mapping starting at addr, from /proc/self/smaps. */
static size_t anon_huge_bytes(void *addr)
{
	FILE *f = fopen("/proc/self/smaps", "r");
	if (!f)
		return 0;
	char line[256];
	bool in_vma = false;
	size_t kb = 0;
	while (fgets(line, sizeof(line), f)) {
		unsigned long start, end;
		if (sscanf(line, "%lx-%lx ", &start, &end) == 2) {
			if (in_vma)
				break;
			in_vma = start == (uintptr_t)addr;
		} else if (in_vma && sscanf(line, "AnonHugePages: %zu kB", &kb) == 1) {
			break;
		}
	}
	fclose(f);
	return kb << 10;
}

static void check_thp(void)
{
	char buf[128] = "";
	FILE *f = fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
	if (!f)
		return;
	if (!fgets(buf, sizeof(buf), f))
		buf[0] = '\0';
	fclose(f);
	if (strstr(buf, "[never]"))
		fprintf(stderr, "warning: transparent huge pages are disabled; "
			"TLB misses will inflate latency.\n"
			"  enable with: echo madvise > "
			"/sys/kernel/mm/transparent_hugepage/enabled\n");
}

/*
 * Build a single random cycle through all cache lines of the buffer
 * (Sattolo's algorithm), so the chase visits every line before repeating
 * and defeats the hardware prefetchers.  Runs on the worker's own cpu so
 * that first touch places the memory on the local NUMA node.
 */
static void *build(void *arg)
{
	struct worker *w = arg;
	pin(w->cpu);

	size_t alloc = buf_size + HUGE_PAGE;
	char *raw = mmap(NULL, alloc, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (raw == MAP_FAILED)
		die("mmap %zu bytes: %s", alloc, strerror(errno));
	char *buf = (char *)(((uintptr_t)raw + HUGE_PAGE - 1) & ~(HUGE_PAGE - 1));
	/*
	 * Huge pages keep TLB misses from polluting the DRAM latency.  The
	 * hint must precede population (so no MAP_POPULATE), otherwise the
	 * range is faulted in with small pages.
	 */
	if (madvise(buf, buf_size, MADV_HUGEPAGE))
		fprintf(stderr, "warning: MADV_HUGEPAGE: %s\n", strerror(errno));
	if (madvise(buf, buf_size, MADV_POPULATE_WRITE)) {
		if (errno != EINVAL)
			die("MADV_POPULATE_WRITE %zu bytes: %s", buf_size,
			    strerror(errno));
		/* Kernel older than 5.14: populate by touching. */
		for (size_t off = 0; off < buf_size; off += 4096)
			buf[off] = 0;
	}
	w->huge = anon_huge_bytes(buf);

	size_t n = buf_size / LINE;
#define IDX(i) (*(uint64_t *)(buf + (i) * LINE))
	for (size_t i = 0; i < n; i++)
		IDX(i) = i;
	uint64_t seed = 0x9e3779b97f4a7c15ULL ^ ((uint64_t)w->cpu << 32 | 1);
	for (size_t i = n - 1; i > 0; i--) {
		size_t j = xorshift64(&seed) % i;
		uint64_t t = IDX(i);
		IDX(i) = IDX(j);
		IDX(j) = t;
	}
	for (size_t i = 0; i < n; i++)
		IDX(i) = (uint64_t)(uintptr_t)(buf + IDX(i) * LINE);
#undef IDX

	/* Find evenly spaced starting points along the cycle for mlp mode. */
	w->starts = malloc(mlp_chains * sizeof(*w->starts));
	void **p = (void **)buf;
	size_t spacing = n / mlp_chains;
	for (unsigned k = 0; k < mlp_chains; k++) {
		w->starts[k] = p;
		for (size_t s = 0; s < spacing; s++)
			p = *p;
	}

	w->buf = buf;
	w->lines = n;
	return NULL;
}

#define STEP1 p = *(void **)p;
#define STEP8 STEP1 STEP1 STEP1 STEP1 STEP1 STEP1 STEP1 STEP1
#define STEP64 STEP8 STEP8 STEP8 STEP8 STEP8 STEP8 STEP8 STEP8

/* Latency-bound: every load depends on the previous one. */
static void run_chase(struct worker *w)
{
	void *p = w->starts[0];
	uint64_t loads = 0;

	while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
		for (int i = 0; i < CHUNK / 64; i++) {
			STEP64
		}
		loads += CHUNK;
		atomic_store_explicit(&w->loads, loads, memory_order_relaxed);
	}
	w->sink = p;
}

/* Throughput-bound: mlp_chains independent chases interleaved. */
static void run_mlp(struct worker *w)
{
	unsigned k, nk = mlp_chains;
	void *p[nk];
	uint64_t loads = 0;

	memcpy(p, w->starts, sizeof(p));
	while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
		for (int i = 0; i < CHUNK / 16; i++)
			for (k = 0; k < nk; k++)
				p[k] = *(void **)p[k];
		loads += (uint64_t)(CHUNK / 16) * nk;
		atomic_store_explicit(&w->loads, loads, memory_order_relaxed);
	}
	for (k = 0; k < nk; k++)
		w->sink = (char *)w->sink + (uintptr_t)p[k];
}

static void *run(void *arg)
{
	struct worker *w = arg;
	pin(w->cpu);
	if (w->role == ROLE_CHASE)
		run_chase(w);
	else
		run_mlp(w);
	return NULL;
}

static void spawn(struct worker *w, pthread_attr_t *attr, void *(*fn)(void *))
{
	int r = pthread_create(&w->thread, attr, fn, w);
	if (r)
		die("pthread_create: %s", strerror(r));
}

struct result {
	double test_ns;		/* mean ns per load across test cpus */
	double test_min_ns;	/* fastest test cpu */
	double test_max_ns;	/* slowest test cpu */
	double neigh_gbps;	/* aggregate neighbor load bandwidth */
};

static struct result run_scenario(enum scenario sc,
				  struct worker *test, int ntest,
				  struct worker *neigh, int nneigh)
{
	struct worker *all[ntest + nneigh];
	int nall = 0;

	for (int i = 0; i < ntest; i++) {
		test[i].role = ROLE_CHASE;
		all[nall++] = &test[i];
	}
	if (sc != SC_IDLE) {
		for (int i = 0; i < nneigh; i++) {
			neigh[i].role = sc == SC_MLP ? ROLE_MLP : ROLE_CHASE;
			all[nall++] = &neigh[i];
		}
	}

	atomic_store(&stop, false);
	for (int i = 0; i < nall; i++) {
		atomic_store(&all[i]->loads, 0);
		spawn(all[i], NULL, run);
	}

	sleep_for(warmup);
	uint64_t before[nall];
	double t0 = now();
	for (int i = 0; i < nall; i++)
		before[i] = atomic_load(&all[i]->loads);
	sleep_for(duration);
	uint64_t after[nall];
	double t1 = now();
	for (int i = 0; i < nall; i++)
		after[i] = atomic_load(&all[i]->loads);

	atomic_store(&stop, true);
	for (int i = 0; i < nall; i++)
		pthread_join(all[i]->thread, NULL);

	double elapsed = t1 - t0;
	struct result r = { .test_min_ns = 1e30 };
	for (int i = 0; i < ntest; i++) {
		double ns = elapsed * 1e9 / (double)(after[i] - before[i]);
		r.test_ns += ns / ntest;
		if (ns < r.test_min_ns)
			r.test_min_ns = ns;
		if (ns > r.test_max_ns)
			r.test_max_ns = ns;
	}
	for (int i = ntest; i < nall; i++)
		r.neigh_gbps += (after[i] - before[i]) * (double)LINE / elapsed / 1e9;
	return r;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static double median(double *v, unsigned n)
{
	qsort(v, n, sizeof(*v), cmp_double);
	return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s -t TEST_CPUS -n NEIGHBOR_CPUS [options]\n"
		"\n"
		"  -t, --test CPUS       cpus running the latency-bound pointer chase\n"
		"  -n, --neighbor CPUS   cpus running the neighbor workload\n"
		"  -s, --size SIZE       buffer per cpu (default: 4x L3, min 64M)\n"
		"  -c, --chains N        independent chains per cpu in mlp mode (default 16)\n"
		"  -d, --duration SEC    measurement time per scenario (default 5)\n"
		"  -w, --warmup SEC      warmup before each measurement (default 1)\n"
		"  -r, --rounds N        rounds of all scenarios; median reported (default 3)\n"
		"\n"
		"CPUS is a list such as 0-7,16-23.\n",
		prog);
	exit(2);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "test", required_argument, NULL, 't' },
		{ "neighbor", required_argument, NULL, 'n' },
		{ "size", required_argument, NULL, 's' },
		{ "chains", required_argument, NULL, 'c' },
		{ "duration", required_argument, NULL, 'd' },
		{ "warmup", required_argument, NULL, 'w' },
		{ "rounds", required_argument, NULL, 'r' },
		{ "help", no_argument, NULL, 'h' },
		{}
	};
	int *test_cpus = NULL, *neigh_cpus = NULL, ntest = 0, nneigh = 0;
	int opt;

	while ((opt = getopt_long(argc, argv, "t:n:s:c:d:w:r:h", opts, NULL)) != -1) {
		switch (opt) {
		case 't': ntest = parse_cpuset(optarg, &test_cpus); break;
		case 'n': nneigh = parse_cpuset(optarg, &neigh_cpus); break;
		case 's': buf_size = parse_size(optarg); break;
		case 'c': mlp_chains = (unsigned)atoi(optarg); break;
		case 'd': duration = atof(optarg); break;
		case 'w': warmup = atof(optarg); break;
		case 'r': rounds = (unsigned)atoi(optarg); break;
		default: usage(argv[0]);
		}
	}
	if (!ntest || !nneigh || optind != argc)
		usage(argv[0]);
	if (!mlp_chains || !rounds || duration <= 0 || warmup < 0)
		die("chains, rounds and duration must be positive");
	for (int i = 0; i < ntest; i++)
		for (int j = 0; j < nneigh; j++)
			if (test_cpus[i] == neigh_cpus[j])
				die("cpu %d is in both cpusets", test_cpus[i]);

	if (!buf_size) {
		size_t l3 = 0;
		for (int i = 0; i < ntest; i++)
			if (l3_size(test_cpus[i]) > l3)
				l3 = l3_size(test_cpus[i]);
		for (int i = 0; i < nneigh; i++)
			if (l3_size(neigh_cpus[i]) > l3)
				l3 = l3_size(neigh_cpus[i]);
		buf_size = 4 * l3;
		if (buf_size < (64UL << 20))
			buf_size = 64UL << 20;
	}
	buf_size = (buf_size + HUGE_PAGE - 1) & ~(HUGE_PAGE - 1);
	if (buf_size / LINE < 2 * mlp_chains)
		die("buffer too small");

	struct worker *test = aligned_alloc(LINE, ntest * sizeof(*test));
	struct worker *neigh = aligned_alloc(LINE, nneigh * sizeof(*neigh));
	memset(test, 0, ntest * sizeof(*test));
	memset(neigh, 0, nneigh * sizeof(*neigh));
	for (int i = 0; i < ntest; i++)
		test[i].cpu = test_cpus[i];
	for (int i = 0; i < nneigh; i++)
		neigh[i].cpu = neigh_cpus[i];

	printf("test cpus: %d, neighbor cpus: %d, buffer per cpu: %zu MiB "
	       "(total %zu MiB), mlp chains: %u\n",
	       ntest, nneigh, buf_size >> 20,
	       (buf_size >> 20) * (ntest + nneigh), mlp_chains);
	check_thp();
	printf("building pointer chains...\n");
	fflush(stdout);
	for (int i = 0; i < ntest; i++)
		spawn(&test[i], NULL, build);
	for (int i = 0; i < nneigh; i++)
		spawn(&neigh[i], NULL, build);
	for (int i = 0; i < ntest; i++)
		pthread_join(test[i].thread, NULL);
	for (int i = 0; i < nneigh; i++)
		pthread_join(neigh[i].thread, NULL);

	size_t huge = 0;
	for (int i = 0; i < ntest; i++)
		huge += test[i].huge;
	for (int i = 0; i < nneigh; i++)
		huge += neigh[i].huge;
	double huge_pct = 100.0 * huge / ((double)buf_size * (ntest + nneigh));
	printf("huge page coverage: %.1f%%\n", huge_pct);
	if (huge_pct < 90)
		fprintf(stderr, "warning: low huge page coverage; TLB misses "
			"will inflate latency (memory fragmented? try "
			"echo 1 > /proc/sys/vm/compact_memory)\n");

	double ns[NR_SCENARIOS][rounds], gbps[NR_SCENARIOS][rounds];

	printf("\n%-6s %-5s %10s %10s %10s %14s\n", "round", "neigh",
	       "ns/load", "min", "max", "neigh GB/s");
	for (unsigned r = 0; r < rounds; r++) {
		/* Interleave scenarios so slow drift affects all of them alike. */
		for (int sc = 0; sc < NR_SCENARIOS; sc++) {
			struct result res = run_scenario(sc, test, ntest,
							 neigh, nneigh);
			ns[sc][r] = res.test_ns;
			gbps[sc][r] = res.neigh_gbps;
			printf("%-6u %-5s %10.1f %10.1f %10.1f %14.2f\n", r + 1,
			       scenario_name[sc], res.test_ns, res.test_min_ns,
			       res.test_max_ns, res.neigh_gbps);
			fflush(stdout);
		}
	}

	double base = median(ns[SC_SAME], rounds);
	double idle = median(ns[SC_IDLE], rounds);
	double noisy = median(ns[SC_MLP], rounds);

	printf("\nmedian of %u rounds, test-set latency per dependent load:\n", rounds);
	printf("  base level (neighbor runs same chase): %8.1f ns   (neighbor %.2f GB/s)\n",
	       base, median(gbps[SC_SAME], rounds));
	printf("  idle neighbor:                         %8.1f ns   boost   %+6.1f%%\n",
	       idle, (base / idle - 1) * 100);
	printf("  noisy neighbor (mlp):                  %8.1f ns   penalty %+6.1f%%   (neighbor %.2f GB/s)\n",
	       noisy, (noisy / base - 1) * 100, median(gbps[SC_MLP], rounds));
	printf("\nboost = base/idle - 1 (speedup); penalty = noisy/base - 1 (slowdown)\n");
	return 0;
}
