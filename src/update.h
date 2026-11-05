#ifndef EC_UPDATE_H
#define EC_UPDATE_H
#include "ec.h"
#include <stdint.h>
#define EC_UPDATE_ORIGIN "https://pocketbook.nimbushub.eu/"
#define EC_UPDATE_CHANNEL "ota/easy-connect-pocketbook-armv7-softfp.txt"
#define EC_UPDATE_PACKAGE "easy-connect-pocketbook-armv7-softfp.zip"
enum { EC_UPDATE_LIMIT = 32 * 1024 * 1024, EC_UPDATE_MANIFEST_LIMIT = 16384, EC_UPDATE_FILES = 7 };
typedef struct {
    char version[24], checksum[65], text[EC_UPDATE_MANIFEST_LIMIT + 1];
} ec_update_release;
/* A separate lock serializes transfer/staging/install without stopping services. */
int ec_update_lock(void);
bool ec_update_newer(const char *, const char *);
int ec_update_parse(const char *, const char *, const char *, ec_update_release *);
int ec_update_paths(const ec_config *, char *, const char *);
int ec_update_remove(const char *);
int ec_update_copy(const char *, const char *);
int ec_update_read(const char *, char *, size_t);
int ec_update_write(const char *, const void *, size_t);
int ec_update_hash(const char *, char[65]);
/* Validates every entry; extracts only when output is non-NULL. */
int ec_update_archive(const char *, const ec_update_release *, const char *, const char *,
                      const char *);
int ec_update_stage(const ec_config *, const char *, const ec_update_release *, const char *,
                    const char *);
int ec_update_pending(const ec_config *, const char *, const char *, ec_update_release *);
int ec_update_request(const ec_config *, const char *, const char *);
int ec_update_install(ec_config *);
int ec_update_rollback(ec_config *);
extern const char *const ec_update_members[EC_UPDATE_FILES];
extern const char *const ec_update_targets[EC_UPDATE_FILES];

typedef struct ec_update_transfer ec_update_transfer;
ec_update_transfer *ec_update_fetch(const char *, const char *, size_t);
int ec_update_poll(ec_update_transfer *); /* 0 pending, 1 success, -1 failure */
const char *ec_update_body(const ec_update_transfer *);
unsigned long long ec_update_received(const ec_update_transfer *);
unsigned long long ec_update_total(const ec_update_transfer *);
void ec_update_free(ec_update_transfer *); /* Cancels incomplete transfers. */
#endif
