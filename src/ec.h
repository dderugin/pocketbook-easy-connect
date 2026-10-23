#ifndef EASY_CONNECT_H
#define EASY_CONNECT_H
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#define EC_PATH 4096
typedef enum { EC_KEYS, EC_PASSWORD, EC_BOTH } ec_auth;
typedef enum { EC_FTP_PASSWORD, EC_FTP_ANONYMOUS } ec_ftp_auth;
typedef struct {
    char root[EC_PATH], state[EC_PATH], runtime[EC_PATH];
    char bind_address[64], ftp_root[EC_PATH], ftp_user[64];
    unsigned ssh_port, ftp_port, pasv_min, pasv_max;
    ec_auth auth;
    ec_ftp_auth ftp_auth;
} ec_config;
typedef struct { pid_t ssh, ftp; int lock_fd; } ec_services;
int ec_error(const char *format, ...) __attribute__((format(printf, 1, 2)));
const char *ec_last_error(void);
int ec_path(char *out, size_t size, const char *base, const char *name);
int ec_config_load(ec_config *config, const char *root);
int ec_config_validate(const ec_config *config);
int ec_config_save(const ec_config *config);
int ec_set_password(const ec_config *config, const char *service, const char *password);
int ec_has_password(const ec_config *config, const char *service);
unsigned ec_key_count(const ec_config *config);
int ec_private_dir(const char *path);
int ec_copy_private(const char *source, const char *destination, int nonempty);
int ec_doctor(const ec_config *config, FILE *out);
int ec_services_open(ec_config *config, ec_services *services);
int ec_start(ec_config *config, ec_services *services, const char *name);
int ec_stop(ec_config *config, ec_services *services, const char *name);
void ec_stop_all(ec_config *config, ec_services *services);
void ec_services_close(ec_config *config, ec_services *services);
void ec_poll(ec_services *services);
int ec_fingerprint(const ec_config *config);
int ec_key_info(const ec_config *config, char *out, size_t size);
#endif
