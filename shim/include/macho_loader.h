#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uintptr_t base_address;
    uintptr_t text_vmaddr;
    size_t    text_vmsize;
    uintptr_t data_vmaddr;
    size_t    data_vmsize;
    uintptr_t entry_point;
    size_t    num_rebases_applied;
    size_t    num_binds_resolved;
    size_t    num_mod_inits_run;
} MachOLoadInfo;

// Load and bootstrap ARM64 Mach-O binary from disk or memory buffer
int macho_loader_init(const char *macho_path, MachOLoadInfo *out_info);
int macho_loader_execute_mod_inits(const MachOLoadInfo *info);
int macho_loader_start_engine(const MachOLoadInfo *info, int argc, char **argv);

// Symbol hook registry
void *macho_resolve_symbol(const char *name);

#ifdef __cplusplus
}
#endif
