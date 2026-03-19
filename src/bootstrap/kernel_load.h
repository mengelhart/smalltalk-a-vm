/* src/bootstrap/kernel_load.h
 * Load kernel .st files in dependency order.
 * Phase 1 — bootstrap scaffolding.
 */
#pragma once

/* Forward declaration */
struct STA_VM;

/* Load all kernel .st files from kernel_dir in dependency order.
 * Returns 0 (STA_OK) on success, negative error code on failure. */
int sta_kernel_load_all(struct STA_VM *vm, const char *kernel_dir);
