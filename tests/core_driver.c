/* Host-only driver for filesystem and credential behavior in the real C code. */
#include "ec.h"
#include "../adapters/password.h"

int main(int argc,char **argv) {
    ec_config config;
    memset(&config,0,sizeof config);
    if (argc<3) return 2;
    if (!strcmp(argv[1],"private-dir") && argc==3)
        return ec_private_dir(argv[2])!=0;
    if (!strcmp(argv[1],"copy") && argc==5)
        return ec_copy_private(argv[2],argv[3],atoi(argv[4]))!=0;
    if (!strcmp(argv[1],"config-save") && argc==3) {
        if (ec_config_load(&config,argv[2]) || ec_config_save(&config)) return 1;
        return ec_config_load(&config,argv[2]) || ec_doctor(&config,stdout);
    }
    if (!strcmp(argv[1],"key-info") && argc==4) {
        char out[128];
        size_t size=(size_t)strtoul(argv[3],NULL,10);
        if (size>sizeof out || ec_config_load(&config,argv[2])) return 2;
        if (ec_key_info(&config,out,size)) return 1;
        fputs(out,stdout);
        return 0;
    }
    if (!strcmp(argv[1],"error") && argc==3) {
        ec_error("operation %s failed: %d",argv[2],42);
        puts(ec_last_error());
        return 0;
    }
    if (strlen(argv[2])>=sizeof config.state) return 2;
    strcpy(config.state,argv[2]);
    if (!strcmp(argv[1],"key-count") && argc==3) {
        printf("%u\n",ec_key_count(&config));
        return 0;
    }
    if (!strcmp(argv[1],"has-password") && argc==4)
        return !ec_has_password(&config,argv[3]);
    if ((!strcmp(argv[1],"set-password") && argc==4) ||
        (!strcmp(argv[1],"verify") && argc==3)) {
        char password[2048];
        size_t n=fread(password,1,sizeof password-1,stdin);
        if (ferror(stdin) || !feof(stdin)) return 2;
        password[n]=0;
        if (!strcmp(argv[1],"set-password"))
            return ec_set_password(&config,argv[3],password)!=0;
        if (setenv("EASY_CONNECT_PASSWORD_FILE",argv[2],1)) return 2;
        return !ec_password_matches(password,n);
    }
    return 2;
}
