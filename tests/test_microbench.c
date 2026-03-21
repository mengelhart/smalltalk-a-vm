/* tests/test_microbench.c
 * Dispatch speed microbenchmarks via sta_eval.
 * NOT part of the default ctest suite (label "soak").
 *
 * Usage:  ./build/tests/test_microbench
 */
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include <sta/vm.h>

static double elapsed_sec(struct timespec *start, struct timespec *end) {
    return (double)(end->tv_sec - start->tv_sec)
         + (double)(end->tv_nsec - start->tv_nsec) / 1e9;
}

static void bench(STA_VM *vm, const char *label, const char *expr,
                  long sends_per_run) {
    struct timespec t0, t1;

    /* Warm up. */
    STA_Handle *h = sta_eval(vm, expr);
    assert(h);
    sta_handle_release(vm, h);

    /* Timed run. */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    h = sta_eval(vm, expr);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    assert(h);

    const char *result = sta_inspect_cstring(vm, h);
    double secs = elapsed_sec(&t0, &t1);
    double sends_sec = (double)sends_per_run / secs;

    printf("%-25s %8.3f ms  %12.0f sends/sec  result=%s\n",
           label, secs * 1000.0, sends_sec, result ? result : "?");

    sta_handle_release(vm, h);
}

int main(void) {
    STA_VMConfig cfg = {0};
    STA_VM *vm = sta_vm_create(&cfg);
    assert(vm);

    printf("=== MICROBENCH ===\n\n");

    /* 1. SmallInt arithmetic: 1M iterations of c := c + 1.
     * Each iteration: timesRepeat: send + block value + c + 1 send + c := store
     * Conservative count: 3 sends per iteration (timesRepeat:, +, value). */
    bench(vm, "SmallInt arithmetic",
          "| c | c := 0. 1000000 timesRepeat: [c := c + 1]. c",
          3000000L);

    /* 2. Array at:put:: 1M iterations.
     * Each iteration: timesRepeat: send + block value + at:put: send
     * Conservative count: 3 sends per iteration. */
    bench(vm, "Array at:put:",
          "| a | a := Array new: 1. 1000000 timesRepeat: [a at: 1 put: 42]. a",
          3000000L);

    printf("\nDone.\n");

    sta_vm_destroy(vm);
    return 0;
}
