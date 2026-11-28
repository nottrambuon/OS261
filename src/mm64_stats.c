/*
 * mm64_stats.c - counters for the 5-level paging report.
 *
 * The assignment asks for statistics on
 *   (1) the number of memory accesses, and
 *   (2) the size of the multilevel paging storage.
 * Counters use GCC atomic builtins so several CPU threads can update them without a lock.
 */

#include "mm64.h"
#include <stdio.h>

static unsigned long long n_walks;                 // page-table walks
static unsigned long long n_entry_reads;           // table entries read
static unsigned long long n_pages_mapped;          // pages mapped to frames
static unsigned long long n_tables[MM64_LEVELS];   // tables allocated/level

static const char* level_name[MM64_LEVELS] = {
    "PGD",
    "P4D",
    "PUD",
    "PMD",
    "PT"
};

#define INC(x, n) __atomic_fetch_add(&(x), (unsigned long long)(n), __ATOMIC_RELAXED)
#define GET(x)    __atomic_load_n(&(x), __ATOMIC_RELAXED)

void mm64_stat_walk(void) {
    INC(n_walks, 1);
}

void mm64_stat_entry_read(void) {
    INC(n_entry_reads, 1);
}

void mm64_stat_table_alloc(int level) {
	if (level >= 0 && level < MM64_LEVELS)
		INC(n_tables[level], 1);
}

void mm64_stat_pages_mapped(unsigned long n) {
    INC(n_pages_mapped, n);
}

void mm64_stats_print(void) {
	unsigned long long walks = GET(n_walks);
	unsigned long long reads = GET(n_entry_reads);
	unsigned long long pages = GET(n_pages_mapped);
	unsigned long long total = 0;

	printf("\n===== MM64 5-level paging statistics =====\n");
	printf("Page-table walks      : %llu\n", walks);
	printf("Table entries read    : %llu", reads);
	if (walks > 0)
		printf("  (avg %.2f memory accesses/walk; 1-level paging = 1)", (double)reads / (double)walks);
	printf("\n");

	printf("Tables allocated      :");
	for (int lv = 0; lv < MM64_LEVELS; lv++) {
		unsigned long long t = GET(n_tables[lv]);
		printf(" %s=%llu", level_name[lv], t);
		total += t;
	}
	printf("  (total %llu)\n", total);

	printf("Page-table storage    : %llu bytes (%llu tables x %d B)\n", total * MM64_TABLE_BYTES, total, MM64_TABLE_BYTES);
	printf("Pages mapped          : %llu", pages);
	if (pages > 0)
		printf("  (%.1f B of tables per mapped page)", (double)(total * MM64_TABLE_BYTES) / (double)pages);
	printf("\n");

	// A flat 1-level table for 57-bit VA needs 2^45 entries x 8 B = 2^48 B
	printf("Flat 1-level table    : %llu TiB would be needed for the full 57-bit space\n", (1ULL << 48) >> 40);
	printf("==========================================\n");
}