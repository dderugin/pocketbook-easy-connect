#include "ec.h"
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t stopping=0;
static void stop(int signal_number) { (void)signal_number; stopping=1; }
int main(int argc,char **argv) {
    umask(0077);
    const char *root="/mnt/ext1/system/share/easy-connect";
    int at=1;
    if (argc>2 && !strcmp(argv[1],"--root")) { root=argv[2]; at=3; }
    if (argc<=at) {
        fprintf(stderr,"Usage: easy-connect [--root DIRECTORY] doctor|fingerprint|serve ssh|ftp|both\n");
        return 2;
    }
    ec_config config;
    if (ec_config_load(&config,root)) return 1;
    const char *command=argv[at++];
    if (!strcmp(command,"doctor") && argc==at) return ec_doctor(&config,stdout)!=0;
    if (!strcmp(command,"fingerprint") && argc==at) return ec_fingerprint(&config)!=0;
    if (strcmp(command,"serve") || argc!=at+1 ||
        (strcmp(argv[at],"ssh") && strcmp(argv[at],"ftp") && strcmp(argv[at],"both"))) return 2;
    struct sigaction action;
    memset(&action,0,sizeof action); action.sa_handler=stop;
    sigaction(SIGINT,&action,NULL); sigaction(SIGTERM,&action,NULL); sigaction(SIGHUP,&action,NULL);
    ec_services services;
    if (ec_services_open(&config,&services)) return 1;
    int rc=0;
    if (strcmp(argv[at],"ftp")) rc=ec_start(&config,&services,"ssh");
    if (!rc && strcmp(argv[at],"ssh")) rc=ec_start(&config,&services,"ftp");
    while (!rc && !stopping) {
        ec_poll(&services);
        if ((!strcmp(argv[at],"ssh") && !services.ssh) ||
            (!strcmp(argv[at],"ftp") && !services.ftp) ||
            (!strcmp(argv[at],"both") && (!services.ssh || !services.ftp))) { rc=-1; break; }
        struct timespec delay={0,100000000}; nanosleep(&delay,NULL);
    }
    ec_services_close(&config,&services);
    return rc!=0;
}
