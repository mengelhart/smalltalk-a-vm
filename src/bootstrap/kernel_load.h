/* src/bootstrap/kernel_load.h
 * Load kernel .st files in dependency order.
 * Phase 1 — bootstrap scaffolding.
 *
 * Reads .st files from the given directory using sta_filein_load().
 * Files are loaded in a hardcoded dependency order.
 * On failure: reports the failing file and error, returns error code.
 */
#pragma once

/* Load all kernel .st files from kernel_dir in dependency order.
 * Returns 0 (STA_OK) on success, negative error code on failure.
 * On failure, sta_vm_last_error() contains the diagnostic. */
int sta_kernel_load_all(const char *kernel_dir);
