/* Easy Connect password verification shared by the Dropbear/vsftpd adapters.
 * GPL-2.0-or-later. No fallback to the firmware account password. */
#ifndef EC_PASSWORD_H
#define EC_PASSWORD_H
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int ec_unhex(const char *text,unsigned char *out,size_t count) {
    size_t i;
    if (strlen(text)!=count*2) return 0;
    for (i=0;i<count;++i) {
        unsigned int value;
        char a=text[i*2],b=text[i*2+1];
        if (!((a>='0'&&a<='9')||(a>='a'&&a<='f')) ||
            !((b>='0'&&b<='9')||(b>='a'&&b<='f'))) return 0;
        if (sscanf(text+i*2,"%2x",&value)!=1) return 0;
        out[i]=(unsigned char)value;
    }
    return 1;
}
static int ec_password_matches(const char *password,size_t length) {
    const char *path=getenv("EASY_CONNECT_PASSWORD_FILE");
    char record[160],version[4],salt_hex[33],hash_hex[65],extra;
    unsigned char salt[16],expected[32],derived[32];
    struct stat st;
    int iterations,fd,result=0;
    ssize_t n;
    if (!path || *path!='/' || !length || length>1024) return 0;
    fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if (fd<0) return 0;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() ||
        (st.st_mode&0077) || st.st_size<=0 || st.st_size>=(off_t)sizeof record) {
        close(fd); return 0;
    }
    n=read(fd,record,sizeof record-1);
    close(fd);
    if (n!=st.st_size) return 0;
    record[n]=0;
    if (sscanf(record,"%3[^$]$%d$%32[^$]$%64s %c",version,&iterations,salt_hex,hash_hex,&extra)!=4 ||
        strcmp(version,"ec1") || iterations<210000 || iterations>1000000 ||
        !ec_unhex(salt_hex,salt,sizeof salt) || !ec_unhex(hash_hex,expected,sizeof expected)) return 0;
    if (PKCS5_PBKDF2_HMAC(password,(int)length,salt,sizeof salt,iterations,EVP_sha256(),sizeof derived,derived)==1)
        result=CRYPTO_memcmp(expected,derived,sizeof expected)==0;
    OPENSSL_cleanse(derived,sizeof derived);
    OPENSSL_cleanse(record,sizeof record);
    return result;
}
#endif
