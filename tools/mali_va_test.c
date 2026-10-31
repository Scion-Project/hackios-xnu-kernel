/*
 * The Galaxy S9 Mali's GPU addresses (drivers/gpu/hakios.mali_va.h, what drivers/gpu/hakios.mali.c gives programs'
 * buffers) checked on this machine:
 *
 *   cc -O1 -Wall -Wextra tools/mali_va_test.c -o mali_va_test && ./mali_va_test
 * (tools/mali-va-test.sh does that under the sanitizers.)
 *
 * What is checked, against what Mesa 25.0.7's panfrost and Linux panfrost take for granted:
 *   - every address is in Linux panfrost's range, 32 MB to 4 GB (panfrost_kmod.c's PANFROST_KMOD_VA_START and _END),
 *     64 KB aligned, and a buffer ends below 4 GB: nothing crosses a 4 GB line (pan_desc.c's assertion on workgroup
 *     storage, pan_cmdstream.c's 32-bit blend shader address on Bifrost);
 *   - no executable buffer of 16 MB or less crosses a 16 MB line (Linux panfrost_gem.c; Mesa's midgard_compile.c);
 *   - live buffers never overlap, and an empty 64 KB granule follows each;
 *   - addresses come back: a session of the window server's kind (thousands of buffers made and freed, Mesa's 128 MB tiler
 *     heap held throughout) runs for far more than 4 GB of allocations without running out, which the original's
 *     never-twice scheme could not;
 *   - an address freed is not given out again by the next allocation (the walk goes on from where it stopped);
 *   - running out is 0, not a wrapped or overlapping address; a double free and a free of what was never given change
 *     nothing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../drivers/gpu/hakios.mali_va.h"

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { if (failures++ < 40) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } } while (0)

#define LIVE_MAX 4096
static struct { uint64_t address, size; bool executable; } live[LIVE_MAX];
static unsigned live_count;

static uint64_t rng = 0x9e3779b97f4a7c15ull;

static uint64_t
next_random(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

static void
check_one(uint64_t address, uint64_t size, bool executable)
{
	CHECK(address >= MALI_VA_FIRST, "%#llx below 32 MB", (unsigned long long)address);
	CHECK(address % MALI_VA_GRANULE == 0, "%#llx not 64 KB aligned", (unsigned long long)address);
	CHECK(address + size <= MALI_VA_END, "%#llx + %#llx past 4 GB", (unsigned long long)address, (unsigned long long)size);
	CHECK(address >> 32 == (address + size - 1) >> 32, "%#llx + %#llx crosses a 4 GB line", (unsigned long long)address,
	    (unsigned long long)size);
	if (executable && size <= MALI_VA_EXEC_LINE) {
		CHECK(address / MALI_VA_EXEC_LINE == (address + size - 1) / MALI_VA_EXEC_LINE,
		    "code at %#llx + %#llx crosses a 16 MB line", (unsigned long long)address, (unsigned long long)size);
	}
}

/* no two live buffers overlap, and the 64 KB after each belongs to none */
static void
check_disjoint(void)
{
	for (unsigned i = 0; i < live_count; i++) {
		for (unsigned k = i + 1; k < live_count; k++) {
			uint64_t a0 = live[i].address, a1 = a0 + mali_va_granules(live[i].size) * MALI_VA_GRANULE;
			uint64_t b0 = live[k].address, b1 = b0 + mali_va_granules(live[k].size) * MALI_VA_GRANULE;

			CHECK(a1 <= b0 || b1 <= a0, "buffers at %#llx and %#llx overlap", (unsigned long long)a0,
			    (unsigned long long)b0);
		}
	}
}

static uint64_t
some_size(void)
{
	switch (next_random() % 8) {
	case 0: return 4096;                                            /* a small pool slab */
	case 1: return 16384;                                           /* a kernel page */
	case 2: return 65536;
	case 3: return 1440ull * 2960 * 4 + 16384;                      /* a full-screen IOSurface */
	case 4: return (next_random() % 64 + 1) * 16384;                /* up to 1 MB */
	case 5: return (next_random() % 16 + 1) << 20;                  /* up to 16 MB */
	case 6: return 0x60000;                                         /* a shader's TLS, as the G52 saw */
	default: return (next_random() % 4096 + 1) * 4096;              /* up to 16 MB, any page */
	}
}

static struct mali_va va;

static void
session(void)
{
	uint64_t heap, total = 0;
	unsigned made = 0, refused = 0;

	memset(&va, 0, sizeof(va));
	live_count = 0;
	heap = mali_va_alloc(&va, 128ull << 20, false);                /* Mesa's tiler heap, made whole */
	CHECK(heap != 0, "no tiler heap");
	check_one(heap, 128ull << 20, false);
	for (unsigned round = 0; round < 200000; round++) {
		if (live_count > 0 && (live_count == LIVE_MAX || next_random() % 2 == 0)) {
			unsigned i = (unsigned)(next_random() % live_count);

			CHECK(mali_va_free(&va, live[i].address, live[i].size), "free of a live buffer at %#llx refused",
			    (unsigned long long)live[i].address);
			CHECK(!mali_va_free(&va, live[i].address, live[i].size), "double free at %#llx taken",
			    (unsigned long long)live[i].address);
			live[i] = live[--live_count];
			continue;
		}
		uint64_t size = some_size();
		bool executable = next_random() % 4 == 0;
		uint64_t address = mali_va_alloc(&va, size, executable);

		if (address == 0) {
			refused++;
			continue;
		}
		check_one(address, size, executable);
		live[live_count].address = address;
		live[live_count].size = size;
		live[live_count].executable = executable;
		live_count++;
		total += size;
		made++;
		if (round % 5000 == 0) {
			check_disjoint();
		}
	}
	check_disjoint();
	printf("mali_va_test: %u buffers made, %.1f GB in all, %u refused while %u were live (%llu MB of addresses)\n", made,
	    total / 1e9, refused, live_count, (unsigned long long)(va.taken * MALI_VA_GRANULE >> 20));
	CHECK(total > 8 * MALI_VA_END, "only %llu bytes in all: addresses do not come back", (unsigned long long)total);
	CHECK(refused == 0, "%u refused in a session that never holds more than ~4096 buffers", refused);
	while (live_count > 0) {
		live_count--;
		CHECK(mali_va_free(&va, live[live_count].address, live[live_count].size), "final free refused");
	}
	CHECK(mali_va_free(&va, heap, 128ull << 20), "the heap's free refused");
	CHECK(va.taken == 0, "%llu granules still taken with nothing live", (unsigned long long)va.taken);
}

static void
corners(void)
{
	uint64_t a, b;

	memset(&va, 0, sizeof(va));
	CHECK(mali_va_alloc(&va, 0, false) == 0, "a size of nothing was given addresses");
	a = mali_va_alloc(&va, 4096, false);
	CHECK(a == MALI_VA_FIRST, "the first buffer at %#llx, not 32 MB", (unsigned long long)a);
	b = mali_va_alloc(&va, 4096, false);
	CHECK(b == a + 2 * MALI_VA_GRANULE, "the second at %#llx: the empty granule after the first is missing",
	    (unsigned long long)b);
	CHECK(mali_va_free(&va, a, 4096), "free refused");
	b = mali_va_alloc(&va, 4096, false);
	CHECK(b != a, "an address just freed was given out again at once");
	CHECK(!mali_va_free(&va, 0x1000, 4096), "a free below 32 MB was taken");
	CHECK(!mali_va_free(&va, MALI_VA_FIRST + 0x800, 4096), "an unaligned free was taken");
	CHECK(!mali_va_free(&va, 0x80000000ull, 4096), "a free of what was never given was taken");

	/* code that would straddle 16 MB goes to the next line; data does not */
	memset(&va, 0, sizeof(va));
	va.next = (MALI_VA_EXEC_LINE * 3 - MALI_VA_GRANULE) / MALI_VA_GRANULE;
	a = mali_va_alloc(&va, 2 * MALI_VA_GRANULE, true);
	CHECK(a == MALI_VA_EXEC_LINE * 3, "code at %#llx, not at the 48 MB line", (unsigned long long)a);
	va.next = (MALI_VA_EXEC_LINE * 5 - MALI_VA_GRANULE) / MALI_VA_GRANULE;
	b = mali_va_alloc(&va, 2 * MALI_VA_GRANULE, false);
	CHECK(b == MALI_VA_EXEC_LINE * 5 - MALI_VA_GRANULE, "data at %#llx moved for nothing", (unsigned long long)b);

	/* running out: 0, and nothing past 4 GB */
	memset(&va, 0, sizeof(va));
	unsigned n = 0;
	while ((a = mali_va_alloc(&va, 256ull << 20, false)) != 0) {
		check_one(a, 256ull << 20, false);
		n++;
		CHECK(n < 64, "more 256 MB buffers than 4 GB holds");
		if (n >= 64) {
			break;
		}
	}
	CHECK(n == 15, "%u buffers of 256 MB (+64 KB) between 32 MB and 4 GB, 15 expected (4064 MB)", n);
	/* the walk wraps: with the top full, the bottom is found again once freed */
	CHECK(mali_va_free(&va, MALI_VA_FIRST, 256ull << 20), "free of the first refused");
	a = mali_va_alloc(&va, 256ull << 20, false);
	CHECK(a == MALI_VA_FIRST, "after a wrap, %#llx and not the freed bottom", (unsigned long long)a);
}

int
main(void)
{
	corners();
	session();
	printf("mali_va_test: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
