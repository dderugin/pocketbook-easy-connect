#include "ec.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char last_error[512];
const char *ec_last_error(void) { return last_error; }
int ec_error(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vsnprintf(last_error,sizeof last_error,format,ap);
    fprintf(stderr, "easy-connect: %s\n",last_error);
    va_end(ap);
    return -1;
}
int ec_path(char *out, size_t size, const char *base, const char *name) {
    int n = snprintf(out, size, "%s/%s", base, name);
    return n < 0 || (size_t)n >= size ? ec_error("path too long") : 0;
}
/* Runtime directories must have real Unix permissions; do not relax on FAT. */
int ec_private_dir(const char *path) {
    struct stat st;
    if (mkdir(path, 0700) && errno != EEXIST)
        return ec_error("mkdir %s: %s", path, strerror(errno));
    if (lstat(path, &st) || !S_ISDIR(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0077))
        return ec_error("%s must be an owned, private directory", path);
    return 0;
}
int ec_copy_private(const char *source, const char *destination, int nonempty) {
    char buffer[4096];
    struct stat st;
    ssize_t n;
    size_t total = 0;
    int src = open(source, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (src < 0) return ec_error("open %s: %s", source, strerror(errno));
    if (fstat(src, &st) || !S_ISREG(st.st_mode) || st.st_size > 1048576) {
        close(src);
        return ec_error("%s must be a regular file of at most 1 MiB", source);
    }
    int dst = open(destination, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (dst < 0) { close(src); return ec_error("open %s: %s", destination, strerror(errno)); }
    int result = 0;
    while ((n = read(src, buffer, sizeof buffer)) > 0) {
        total += (size_t)n;
        if (total > 1048576) { result = -1; break; }
        ssize_t offset = 0;
        while (offset < n) {
            ssize_t written = write(dst, buffer + offset, (size_t)(n - offset));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { result = -1; break; }
            offset += written;
        }
        if (result) break;
    }
    if (n < 0 || (nonempty && !total) || fsync(dst)) result = -1;
    close(src);
    close(dst);
    if (result) { unlink(destination); return ec_error("copy failed: %s", source); }
    return 0;
}
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) ++s;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n-1])) s[--n] = 0;
    return s;
}
static int port(const char *s, unsigned *out) {
    char *end;
    for (const char *p=s; *p; ++p)
        if (!isdigit((unsigned char)*p)) return ec_error("invalid port: %s",s);
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || !*s || *end || n < 1024 || n > 65535)
        return ec_error("invalid unprivileged port: %s", s);
    *out = (unsigned)n;
    return 0;
}
int ec_config_load(ec_config *c, const char *root) {
    memset(c, 0, sizeof *c);
    if (!realpath(root, c->root)) return ec_error("root %s: %s", root, strerror(errno));
    if (ec_path(c->state, sizeof c->state, c->root, "state")) return -1;
    strcpy(c->bind_address, "0.0.0.0");
    strcpy(c->ftp_root, "/mnt/ext1");
    struct passwd *current_user = getpwuid(geteuid());
    if (!current_user || strlen(current_user->pw_name)>=sizeof c->ftp_user)
        return ec_error("cannot determine the launching account");
    strcpy(c->ftp_user,current_user->pw_name);
    c->ssh_port = 2222; c->ftp_port = 2121;
    c->pasv_min = 50000; c->pasv_max = 50009;
    c->auth = EC_KEYS;
    char path[EC_PATH], line[EC_PATH + 100];
    if (ec_path(path, sizeof path, c->root, "easy-connect.conf")) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 0 : ec_error("read config: %s", strerror(errno));
    unsigned lineno = 0, seen = 0;
    int rc = 0;
    while (fgets(line, sizeof line, f)) {
        ++lineno;
        if (!strchr(line, '\n') && !feof(f)) { rc = -1; break; }
        char *key = trim(line);
        if (!*key || *key == '#') continue;
        char *value = strchr(key, '=');
        if (!value) { rc = -1; break; }
        *value++ = 0; key = trim(key); value = trim(value);
        unsigned flag = 0;
        if (!strcmp(key, "ssh_port")) { flag=1; rc=port(value, &c->ssh_port); }
        else if (!strcmp(key, "ftp_port")) { flag=2; rc=port(value, &c->ftp_port); }
        else if (!strcmp(key, "pasv_min")) { flag=4; rc=port(value, &c->pasv_min); }
        else if (!strcmp(key, "pasv_max")) { flag=8; rc=port(value, &c->pasv_max); }
        else if (!strcmp(key, "auth")) {
            flag=16;
            if (!strcmp(value,"key")) c->auth=EC_KEYS;
            else if (!strcmp(value,"password")) c->auth=EC_PASSWORD;
            else if (!strcmp(value,"both")) c->auth=EC_BOTH;
            else rc=-1;
        } else if (!strcmp(key,"ftp_auth")) {
            flag=512;
            if (!strcmp(value,"password")) c->ftp_auth=EC_FTP_PASSWORD;
            else if (!strcmp(value,"anonymous")) c->ftp_auth=EC_FTP_ANONYMOUS;
            else rc=-1;
        } else if (!strcmp(key, "bind_address")) {
            struct in_addr address;
            flag=32;
            if (strlen(value) >= sizeof c->bind_address || inet_pton(AF_INET,value,&address)!=1) rc=-1;
            else strcpy(c->bind_address,value);
        } else if (!strcmp(key,"ftp_root") || !strcmp(key,"state_dir")) {
            flag=!strcmp(key,"ftp_root") ? 64 : 128;
            if (*value!='/' || strlen(value)>=EC_PATH || strchr(value,'\r')) rc=-1;
            else strcpy(flag==64 ? c->ftp_root : c->state,value);
        } else if (!strcmp(key,"ftp_user")) {
            flag=256;
            if (!*value || strlen(value)>=sizeof c->ftp_user) rc=-1;
            else {
                for (const char *p=value; *p; ++p)
                    if (!isalnum((unsigned char)*p) && *p!='_' && *p!='-' && *p!='.') rc=-1;
                if (!rc && strcmp(value,"current")) strcpy(c->ftp_user,value);
            }
        } else rc=-1;
        if (rc || (seen & flag)) { rc=-1; break; }
        seen |= flag;
    }
    if (ferror(f)) rc=-1;
    fclose(f);
    if (rc) return ec_error("invalid or duplicate config setting on line %u",lineno);
    return ec_config_validate(c);
}
int ec_config_validate(const ec_config *c) {
    const unsigned ports[]={c->ssh_port,c->ftp_port,c->pasv_min,c->pasv_max};
    for (unsigned i=0;i<4;++i)
        if (ports[i]<1024 || ports[i]>65535) return ec_error("ports must be between 1024 and 65535");
    struct in_addr address;
    if (inet_pton(AF_INET,c->bind_address,&address)!=1) return ec_error("invalid bind address");
    if (c->auth<EC_KEYS || c->auth>EC_BOTH ||
        c->ftp_auth<EC_FTP_PASSWORD || c->ftp_auth>EC_FTP_ANONYMOUS)
        return ec_error("invalid access type");
    if (*c->state!='/' || *c->ftp_root!='/' ||
        strpbrk(c->state,"\r\n") || strpbrk(c->ftp_root,"\r\n"))
        return ec_error("directories must be absolute paths without newlines");
    if (!*c->ftp_user || strpbrk(c->ftp_user,"\r\n=")) return ec_error("invalid FTP account");
    if (c->ssh_port==c->ftp_port || c->pasv_min>c->pasv_max ||
        (c->ssh_port>=c->pasv_min && c->ssh_port<=c->pasv_max) ||
        (c->ftp_port>=c->pasv_min && c->ftp_port<=c->pasv_max))
        return ec_error("service and passive ports must not overlap");
    return 0;
}
int ec_config_save(const ec_config *c) {
    char temporary[EC_PATH],target[EC_PATH];
    if (ec_config_validate(c) ||
        ec_path(temporary,sizeof temporary,c->root,".easy-connect.conf.XXXXXX") ||
        ec_path(target,sizeof target,c->root,"easy-connect.conf")) return -1;
    int fd=mkstemp(temporary);
    if (fd<0) return ec_error("cannot create settings file: %s",strerror(errno));
    FILE *out=fdopen(fd,"w");
    if (!out) { close(fd); unlink(temporary); return ec_error("cannot open settings file"); }
    int failed=fprintf(out,
        "# Saved by Easy Connect\nauth=%s\nftp_auth=%s\nbind_address=%s\n"
        "ssh_port=%u\nftp_port=%u\npasv_min=%u\npasv_max=%u\n"
        "ftp_user=%s\nftp_root=%s\nstate_dir=%s\n",
        c->auth==EC_KEYS?"key":c->auth==EC_PASSWORD?"password":"both",
        c->ftp_auth==EC_FTP_PASSWORD?"password":"anonymous",c->bind_address,
        c->ssh_port,c->ftp_port,c->pasv_min,c->pasv_max,c->ftp_user,c->ftp_root,c->state)<0;
    if (fflush(out) || fsync(fd)) failed=1;
    if (fclose(out)) failed=1;
    if (failed || rename(temporary,target)) {
        unlink(temporary); return ec_error("could not save settings; previous settings retained");
    }
    return 0;
}
