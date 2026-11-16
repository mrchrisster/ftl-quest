#pragma once

#include <cstdint>
#include <cstddef>
#include "objc_shim.h"

#ifdef __cplusplus
extern "C" {
#endif

// NSSearchPath constants
enum {
    NSDocumentDirectory = 9,
    NSCachesDirectory = 13,
    NSApplicationSupportDirectory = 14
};

enum {
    NSUserDomainMask = 1
};

// Foundation replacement APIs
id nsstring_create_with_utf8(const char *utf8_str);
const char *nsstring_get_utf8(id ns_str);

id nsbundle_main_bundle(void);
id nsbundle_path_for_resource(id bundle, id name, id ext);
id nsbundle_resource_path(id bundle);

id nsuserdefaults_standard(void);
int nsuserdefaults_get_int(id defaults, const char *key);
void nsuserdefaults_set_int(id defaults, const char *key, int value);
const char *nsuserdefaults_get_string(id defaults, const char *key);
void nsuserdefaults_set_string(id defaults, const char *key, const char *val);

void *NSSearchPathForDirectoriesInDomains(unsigned long directory, unsigned long domainMask, signed char expandTilde);

size_t nsarray_count(id arr);
id nsarray_object_at_index(id arr, size_t idx);

#ifdef __cplusplus
}
#endif
