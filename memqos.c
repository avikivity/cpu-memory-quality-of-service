/*
 * memqos - measure memory subsystem quality-of-service under neighbor load.
 *
 * A latency-bound pointer-chasing workload runs on every CPU of the "test"
 * cpuset, while the "neighbor" cpuset runs another workload.  Two tests are
 * run, each targeting one level of the memory hierarchy:
 *
 *   dram - the chase buffers are sized well past L3, so every load is a
 *          DRAM access whose latency is fully exposed.
 *   l3   - the chase buffers are sized past L2 but small enough that the
 *          test cpus sharing an L3 fit in it, so with an idle neighbor
 *          loads are L3 hits (or L2 hits, for the part L2 can hold).
 *
 * For each test the neighbor runs one of:
 *
 *   same  - the same chase as the test cpus           (baseline)
 *   idle  - nothing                                   (best case)
 *   noisy - for dram: many independent DRAM chases per cpu (high
 *                     memory-level parallelism, saturates the memory
 *                     subsystem)
 *           for l3:   streaming stores over buffers that together fill
 *                     each L3 the neighbors share (keeps L3 full of dirty
 *                     lines, evicting the test cpus' working set)
 *
 * The report gives, per test, the baseline latency, the boost the test set
 * gets when its neighbor is idle, and the penalty it suffers when its
 * neighbor is noisy.
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

enum role { ROLE_CHASE, ROLE_MLP, ROLE_FILL };

enum test { T_DRAM, T_L3, NR_TESTS };

static const struct {
	const char *name;
	const char *noisy_name;
	enum role noisy;
} tests[NR_TESTS] = {
	[T_DRAM] = { "dram", "mlp", ROLE_MLP },
	[T_L3] = { "l3", "l3 flood", ROLE_FILL },
};

enum scenario { SC_SAME, SC_IDLE, SC_NOISY, NR_SCENARIOS };
static const char *scenario_name[] = { "same", "idle", "noisy" };

struct worker {
	int cpu;
	void **starts;   /* mlp_chains evenly spaced positions in the dram cycle */
	char *l3buf;     /* l3 chase cycle */
	size_t l3_size;
	char *fill;      /* l3 flood buffer; neighbors only */
	size_t fill_size;
	size_t mapped;   /* bytes allocated */
	size_t huge;     /* bytes of those backed by huge pages */
	enum role role;
	void *head;      /* where ROLE_CHASE starts */
	pthread_t thread;
	/* Written by the worker, read by main; on its own cache line. */
	_Alignas(LINE) _Atomic uint64_t loads;
	void *sink;
};

static size_t buf_size;
static size_t l3_override;
static size_t fill_override;
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

static bool read_cache_attr(int cpu, int index, const char *attr,
			    char *buf, size_t len)
{
	char path[128];
	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cache/index%d/%s", cpu, index, attr);
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	bool ok = fgets(buf, len, f);
	fclose(f);
	buf[strcspn(buf, "\n")] = '\0';
	return ok;
}

/* Size of a cache level of cpu (index 2 = L2, 3 = L3), or 0 if unknown. */
static size_t cache_size(int cpu, int index)
{
	char buf[64];
	if (!read_cache_attr(cpu, index, "size", buf, sizeof(buf)))
		return 0;
	char *end;
	size_t sz = strtoull(buf, &end, 10);
	if (*end == 'K')
		sz <<= 10;
	else if (*end == 'M')
		sz <<= 20;
	return sz;
}

/* Number of cpus in the list that share cpu's L3 (including cpu itself). */
static int l3_sharers(int cpu, const int *cpus, int ncpus)
{
	char buf[1024];
	if (!read_cache_attr(cpu, 3, "shared_cpu_list", buf, sizeof(buf)))
		return 1;
	int *shared, nshared = parse_cpuset(buf, &shared), n = 0;
	for (int i = 0; i < ncpus; i++)
		for (int j = 0; j < nshared; j++)
			if (cpus[i] == shared[j])
				n++;
	free(shared);
	return n ? n : 1;
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
 * Allocate a huge-page-aligned, populated buffer.  Huge pages keep TLB
 * misses from polluting the latency.  The hint must precede population
 * (so no MAP_POPULATE), otherwise the range is faulted in with small
 * pages.
 */
static char *alloc_huge(struct worker *w, size_t size)
{
	size = (size + HUGE_PAGE - 1) & ~(HUGE_PAGE - 1);
	size_t alloc = size + HUGE_PAGE;
	char *raw = mmap(NULL, alloc, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (raw == MAP_FAILED)
		die("mmap %zu bytes: %s", alloc, strerror(errno));
	char *buf = (char *)(((uintptr_t)raw + HUGE_PAGE - 1) & ~(HUGE_PAGE - 1));
	if (madvise(buf, size, MADV_HUGEPAGE))
		fprintf(stderr, "warning: MADV_HUGEPAGE: %s\n", strerror(errno));
	if (madvise(buf, size, MADV_POPULATE_WRITE)) {
		if (errno != EINVAL)
			die("MADV_POPULATE_WRITE %zu bytes: %s", size,
			    strerror(errno));
		/* Kernel older than 5.14: populate by touching. */
		for (size_t off = 0; off < size; off += 4096)
			buf[off] = 0;
	}
	w->mapped += size;
	w->huge += anon_huge_bytes(buf);
	return buf;
}

/*
 * Link the first n cache lines of buf into a single random cycle
 * (Sattolo's algorithm), so the chase visits every line before repeating
 * and defeats the hardware prefetchers.
 */
static void make_cycle(char *buf, size_t n, uint64_t seed)
{
#define IDX(i) (*(uint64_t *)(buf + (i) * LINE))
	for (size_t i = 0; i < n; i++)
		IDX(i) = i;
	for (size_t i = n - 1; i > 0; i--) {
		size_t j = xorshift64(&seed) % i;
		uint64_t t = IDX(i);
		IDX(i) = IDX(j);
		IDX(j) = t;
	}
	for (size_t i = 0; i < n; i++)
		IDX(i) = (uint64_t)(uintptr_t)(buf + IDX(i) * LINE);
#undef IDX
}

/*
 * Set up a worker's buffers.  Runs on the worker's own cpu so that first
 * touch places the memory on the local NUMA node.
 */
static void *build(void *arg)
{
	struct worker *w = arg;
	pin(w->cpu);

	uint64_t seed = 0x9e3779b97f4a7c15ULL ^ ((uint64_t)w->cpu << 32 | 1);
	char *buf = alloc_huge(w, buf_size);
	size_t n = buf_size / LINE;
	make_cycle(buf, n, seed);

	/* Find evenly spaced starting points along the cycle for mlp mode. */
	w->starts = malloc(mlp_chains * sizeof(*w->starts));
	void **p = (void **)buf;
	size_t spacing = n / mlp_chains;
	for (unsigned k = 0; k < mlp_chains; k++) {
		w->starts[k] = p;
		for (size_t s = 0; s < spacing; s++)
			p = *p;
	}

	w->l3buf = alloc_huge(w, w->l3_size);
	make_cycle(w->l3buf, w->l3_size / LINE, ~seed);

	if (w->fill_size)
		w->fill = alloc_huge(w, w->fill_size);
	return NULL;
}

#define STEP1 p = *(void **)p;
#define STEP8 STEP1 STEP1 STEP1 STEP1 STEP1 STEP1 STEP1 STEP1
#define STEP64 STEP8 STEP8 STEP8 STEP8 STEP8 STEP8 STEP8 STEP8

/* Latency-bound: every load depends on the previous one. */
static void run_chase(struct worker *w)
{
	void *p = w->head;
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

/* Throughput-bound: mlp_chains independent DRAM chases interleaved. */
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

/*
 * Cache-bound: one store per line, streaming over a buffer sized so that
 * all neighbors sharing an L3 together fill it.  Lines cycle through L2
 * into L3 continuously, and they are dirty, so evicting them costs a
 * writeback.
 */
static void run_fill(struct worker *w)
{
	char *b = w->fill;
	size_t size = w->fill_size, off = 0;
	uint64_t lines = 0;

	while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
		for (int i = 0; i < CHUNK; i++) {
			*(volatile uint64_t *)(b + off) = lines + i;
			off += LINE;
			if (off == size)
				off = 0;
		}
		lines += CHUNK;
		atomic_store_explicit(&w->loads, lines, memory_order_relaxed);
	}
}

static void *run(void *arg)
{
	struct worker *w = arg;
	pin(w->cpu);
	switch (w->role) {
	case ROLE_CHASE: run_chase(w); break;
	case ROLE_MLP: run_mlp(w); break;
	case ROLE_FILL: run_fill(w); break;
	}
	return NULL;
}

static void spawn(struct worker *w, void *(*fn)(void *))
{
	int r = pthread_create(&w->thread, NULL, fn, w);
	if (r)
		die("pthread_create: %s", strerror(r));
}

struct result {
	double test_ns;		/* mean ns per load across test cpus */
	double test_min_ns;	/* fastest test cpu */
	double test_max_ns;	/* slowest test cpu */
	double neigh_gbps;	/* aggregate neighbor bandwidth (lines touched) */
};

static void set_chase(struct worker *w, enum test t)
{
	w->role = ROLE_CHASE;
	w->head = t == T_L3 ? (void *)w->l3buf : w->starts[0];
}

static struct result run_scenario(enum test t, enum scenario sc,
				  struct worker *test, int ntest,
				  struct worker *neigh, int nneigh)
{
	struct worker *all[ntest + nneigh];
	int nall = 0;

	for (int i = 0; i < ntest; i++) {
		set_chase(&test[i], t);
		all[nall++] = &test[i];
	}
	if (sc != SC_IDLE) {
		for (int i = 0; i < nneigh; i++) {
			if (sc == SC_SAME)
				set_chase(&neigh[i], t);
			else
				neigh[i].role = tests[t].noisy;
			all[nall++] = &neigh[i];
		}
	}

	atomic_store(&stop, false);
	for (int i = 0; i < nall; i++) {
		atomic_store(&all[i]->loads, 0);
		spawn(all[i], run);
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
		"  -s, --size SIZE       dram chase buffer per cpu (default: 4x L3, min 64M)\n"
		"  -l, --l3-size SIZE    l3 chase buffer per cpu (default: L2 + 1/2 of\n"
		"                        L3 / test cpus sharing that L3)\n"
		"  -f, --fill SIZE       l3 flood buffer per neighbor cpu (default: L2 +\n"
		"                        3/4 of L3 / neighbor cpus sharing that L3)\n"
		"  -c, --chains N        independent chains per cpu in mlp mode (default 16)\n"
		"  -d, --duration SEC    measurement time per scenario (default 5)\n"
		"  -w, --warmup SEC      warmup before each measurement (default 1)\n"
		"  -r, --rounds N        rounds of all scenarios; median reported (default 3)\n"
		"\n"
		"CPUS is a list such as 0-7,16-23.\n",
		prog);
	exit(2);
}

static size_t line_align(size_t sz)
{
	sz &= ~(size_t)(LINE - 1);
	return sz < 2 * LINE ? 2 * LINE : sz;
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "test", required_argument, NULL, 't' },
		{ "neighbor", required_argument, NULL, 'n' },
		{ "size", required_argument, NULL, 's' },
		{ "l3-size", required_argument, NULL, 'l' },
		{ "fill", required_argument, NULL, 'f' },
		{ "chains", required_argument, NULL, 'c' },
		{ "duration", required_argument, NULL, 'd' },
		{ "warmup", required_argument, NULL, 'w' },
		{ "rounds", required_argument, NULL, 'r' },
		{ "help", no_argument, NULL, 'h' },
		{}
	};
	int *test_cpus = NULL, *neigh_cpus = NULL, ntest = 0, nneigh = 0;
	int opt;

	while ((opt = getopt_long(argc, argv, "t:n:s:l:f:c:d:w:r:h", opts, NULL)) != -1) {
		switch (opt) {
		case 't': ntest = parse_cpuset(optarg, &test_cpus); break;
		case 'n': nneigh = parse_cpuset(optarg, &neigh_cpus); break;
		case 's': buf_size = parse_size(optarg); break;
		case 'l': l3_override = parse_size(optarg); break;
		case 'f': fill_override = parse_size(optarg); break;
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

	int nall = ntest + nneigh;
	int all_cpus[nall];
	memcpy(all_cpus, test_cpus, ntest * sizeof(int));
	memcpy(all_cpus + ntest, neigh_cpus, nneigh * sizeof(int));

	if (!buf_size) {
		size_t l3 = 0;
		for (int i = 0; i < nall; i++)
			if (cache_size(all_cpus[i], 3) > l3)
				l3 = cache_size(all_cpus[i], 3);
		buf_size = 4 * l3;
		if (buf_size < (64UL << 20))
			buf_size = 64UL << 20;
	}
	buf_size = (buf_size + HUGE_PAGE - 1) & ~(HUGE_PAGE - 1);
	if (buf_size / LINE < 2 * mlp_chains)
		die("buffer too small");

	struct worker *workers = aligned_alloc(LINE, nall * sizeof(*workers));
	memset(workers, 0, nall * sizeof(*workers));
	struct worker *test = workers, *neigh = workers + ntest;

	/*
	 * A core only moves lines into L3 by evicting them from L2, so every
	 * L3-targeting footprint is L2 plus a share of L3.
	 *
	 * The l3 chase gets half of the test cpus' share of L3, and the flood
	 * 3/4 of the neighbors' share: past either, set conflicts and
	 * replacement start spilling to DRAM.  It is sized by the test cpus alone, and the
	 * neighbors' "same" chase uses the same size: on parts where the
	 * L2s together dwarf L3 not everyone can be L3-resident, and the
	 * baseline should show that.
	 */
	size_t l3_chase = SIZE_MAX, l2_max = 0;
	for (int i = 0; i < ntest; i++) {
		int cpu = test_cpus[i];
		size_t l3 = cache_size(cpu, 3), l2 = cache_size(cpu, 2);
		if (!l3 && !l3_override)
			die("cannot determine L3 size of cpu %d; use --l3-size", cpu);
		size_t sz = l2 + l3 / l3_sharers(cpu, test_cpus, ntest) / 2;
		if (sz < l3_chase)
			l3_chase = sz;
		if (l2 > l2_max)
			l2_max = l2;
	}
	l3_chase = line_align(l3_override ? l3_override : l3_chase);
	if (l3_chase < 2 * l2_max)
		fprintf(stderr, "warning: l3 chase buffer (%zu KiB) is less than "
			"twice L2 (%zu KiB); over half its loads may hit L2. Use "
			"fewer test cpus per L3.\n", l3_chase >> 10, l2_max >> 10);

	size_t fill_min = SIZE_MAX, fill_max = 0;
	for (int i = 0; i < nall; i++) {
		struct worker *w = &workers[i];
		int cpu = all_cpus[i];
		w->cpu = cpu;
		w->l3_size = l3_chase;
		if (i < ntest)
			continue;

		size_t l3 = cache_size(cpu, 3), l2 = cache_size(cpu, 2);
		if (!l3 && !fill_override)
			die("cannot determine L3 size of cpu %d; use --fill", cpu);
		w->fill_size = line_align(fill_override ? fill_override
					  : l2 + l3 / l3_sharers(cpu, neigh_cpus, nneigh) * 3 / 4);
		if (w->fill_size < fill_min)
			fill_min = w->fill_size;
		if (w->fill_size > fill_max)
			fill_max = w->fill_size;
	}

	printf("test cpus: %d, neighbor cpus: %d, mlp chains: %u\n",
	       ntest, nneigh, mlp_chains);
	printf("per cpu: dram chase %zu MiB, l3 chase %zu KiB (L2 %zu KiB), "
	       "l3 flood %zu-%zu KiB (neighbors)\n",
	       buf_size >> 20, l3_chase >> 10, l2_max >> 10,
	       fill_min >> 10, fill_max >> 10);
	check_thp();
	printf("building pointer chains...\n");
	fflush(stdout);
	for (int i = 0; i < nall; i++)
		spawn(&workers[i], build);
	for (int i = 0; i < nall; i++)
		pthread_join(workers[i].thread, NULL);

	size_t huge = 0, mapped = 0;
	for (int i = 0; i < nall; i++) {
		huge += workers[i].huge;
		mapped += workers[i].mapped;
	}
	double huge_pct = 100.0 * huge / mapped;
	printf("memory: %zu MiB, huge page coverage: %.1f%%\n",
	       mapped >> 20, huge_pct);
	if (huge_pct < 90)
		fprintf(stderr, "warning: low huge page coverage; TLB misses "
			"will inflate latency (memory fragmented? try "
			"echo 1 > /proc/sys/vm/compact_memory)\n");

	double ns[NR_TESTS][NR_SCENARIOS][rounds];
	double gbps[NR_TESTS][NR_SCENARIOS][rounds];

	printf("\n%-6s %-5s %-9s %10s %10s %10s %14s\n", "round", "test", "neigh",
	       "ns/load", "min", "max", "neigh GB/s");
	for (unsigned r = 0; r < rounds; r++) {
		/* Interleave scenarios so slow drift affects all of them alike. */
		for (int t = 0; t < NR_TESTS; t++) {
			for (int sc = 0; sc < NR_SCENARIOS; sc++) {
				struct result res = run_scenario(t, sc, test, ntest,
								 neigh, nneigh);
				ns[t][sc][r] = res.test_ns;
				gbps[t][sc][r] = res.neigh_gbps;
				printf("%-6u %-5s %-9s %10.1f %10.1f %10.1f %14.2f\n",
				       r + 1, tests[t].name,
				       sc == SC_NOISY ? tests[t].noisy_name
						      : scenario_name[sc],
				       res.test_ns, res.test_min_ns,
				       res.test_max_ns, res.neigh_gbps);
				fflush(stdout);
			}
		}
	}

	printf("\nmedian of %u rounds, test-set latency per dependent load:\n", rounds);
	for (int t = 0; t < NR_TESTS; t++) {
		double base = median(ns[t][SC_SAME], rounds);
		double idle = median(ns[t][SC_IDLE], rounds);
		double noisy = median(ns[t][SC_NOISY], rounds);
		char label[64];

		printf("\n  %s test:\n", tests[t].name);
		printf("    base level (neighbor runs same chase): %8.1f ns   (neighbor %.2f GB/s)\n",
		       base, median(gbps[t][SC_SAME], rounds));
		printf("    idle neighbor:                         %8.1f ns   boost   %+6.1f%%\n",
		       idle, (base / idle - 1) * 100);
		snprintf(label, sizeof(label), "noisy neighbor (%s):", tests[t].noisy_name);
		printf("    %-38s %8.1f ns   penalty %+6.1f%%   (neighbor %.2f GB/s)\n",
		       label, noisy, (noisy / base - 1) * 100,
		       median(gbps[t][SC_NOISY], rounds));
	}
	printf("\nboost = base/idle - 1 (speedup); penalty = noisy/base - 1 (slowdown)\n");
	printf("neighbor GB/s counts cache lines touched, not DRAM traffic\n");
	return 0;
}
