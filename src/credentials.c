#include "ec.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *password_file(const char *service) {
    return !strcmp(service,"ssh")?"password.hash":!strcmp(service,"ftp")?"ftp-password.hash":NULL;
}
int ec_has_password(const ec_config *c,const char *service) {
    char path[EC_PATH];
    const char *name=password_file(service);
    struct stat st;
    return name && !ec_path(path,sizeof path,c->state,name) &&
        !lstat(path,&st) && S_ISREG(st.st_mode) && st.st_size>0;
}
int ec_set_password(const ec_config *c,const char *service,const char *password) {
    const char *name=password_file(service);
    size_t length=strlen(password);
    if (!name) return ec_error("unknown password service");
    if (length<12 || length>1024 || strpbrk(password,"\r\n"))
        return ec_error("password must contain 12-1024 bytes without newlines");
    struct stat st;
    if (lstat(c->state,&st)) {
        if (errno!=ENOENT || mkdir(c->state,0700)) return ec_error("cannot create credential directory");
    } else if (!S_ISDIR(st.st_mode)) return ec_error("credential directory must not be a symlink");
    unsigned char salt[16],derived[32];
    if (RAND_bytes(salt,sizeof salt)!=1 ||
        PKCS5_PBKDF2_HMAC(password,(int)length,salt,sizeof salt,210000,EVP_sha256(),sizeof derived,derived)!=1)
        return ec_error("could not generate password verifier");
    char temporary[EC_PATH],target[EC_PATH];
    if (ec_path(temporary,sizeof temporary,c->state,".password.XXXXXX") ||
        ec_path(target,sizeof target,c->state,name)) { OPENSSL_cleanse(derived,sizeof derived); return -1; }
    int fd=mkstemp(temporary);
    if (fd<0) { OPENSSL_cleanse(derived,sizeof derived); return ec_error("cannot create password file"); }
    FILE *out=fdopen(fd,"w");
    if (!out) { close(fd); unlink(temporary); OPENSSL_cleanse(derived,sizeof derived); return ec_error("cannot open password file"); }
    fprintf(out,"ec1$210000$");
    for (unsigned i=0;i<sizeof salt;++i) fprintf(out,"%02x",salt[i]);
    fputc('$',out);
    for (unsigned i=0;i<sizeof derived;++i) fprintf(out,"%02x",derived[i]);
    fputc('\n',out);
    OPENSSL_cleanse(derived,sizeof derived);
    int failed=ferror(out);
    if (fflush(out) || fsync(fd)) failed=1;
    if (fclose(out)) failed=1;
    if (failed || rename(temporary,target)) {
        unlink(temporary); return ec_error("could not save password; previous password retained");
    }
    return 0;
}
unsigned ec_key_count(const ec_config *c) {
    char path[EC_PATH],line[8192];
    if (ec_path(path,sizeof path,c->state,"authorized_keys")) return 0;
    int fd=open(path,O_RDONLY|O_NONBLOCK|O_NOFOLLOW);
    struct stat st;
    if (fd<0) return 0;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size>1048576) { close(fd); return 0; }
    FILE *in=fdopen(fd,"r");
    if (!in) { close(fd); return 0; }
    unsigned count=0;
    while (fgets(line,sizeof line,in)) {
        char *p=line;
        while (*p==' ' || *p=='\t') ++p;
        if (*p && *p!='#' && *p!='\r' && *p!='\n') ++count;
        if (!strchr(line,'\n')) while (fgets(line,sizeof line,in) && !strchr(line,'\n')) {}
    }
    fclose(in); return count;
}
