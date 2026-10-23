#include "ec.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <pwd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

int ec_doctor(const ec_config *c, FILE *out) {
    struct utsname u;
    struct passwd *pw = getpwuid(geteuid());
    struct statvfs fs;
    if (!uname(&u)) fprintf(out,"OS: %s %s %s\n",u.sysname,u.release,u.machine);
    fprintf(out,"UID: %lu GID: %lu\nAccount: %s\nShell: %s\n",
        (unsigned long)geteuid(),(unsigned long)getegid(),pw?pw->pw_name:"unknown",pw?pw->pw_shell:"unknown");
    fprintf(out,"Application: %s\nState: %s\n",c->root,c->state);
    if (!statvfs(c->root,&fs)) fprintf(out,"Storage: %lu MiB free; flags 0x%lx\n",
        (unsigned long)((double)fs.f_bavail*fs.f_frsize/1048576.0),(unsigned long)fs.f_flag);
    int pty = open("/dev/ptmx",O_RDWR|O_NOCTTY);
    fprintf(out,"PTY master: %s\n",pty<0?"unavailable":"available");
    if (pty>=0) close(pty);
    struct ifaddrs *interfaces=NULL;
    if (!getifaddrs(&interfaces)) {
        for (struct ifaddrs *i=interfaces;i;i=i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family!=AF_INET || !(i->ifa_flags & IFF_UP)) continue;
            char address[INET_ADDRSTRLEN];
            const struct sockaddr_in *a=(const struct sockaddr_in *)i->ifa_addr;
            if (inet_ntop(AF_INET,&a->sin_addr,address,sizeof address))
                fprintf(out,"Network: %s %s%s\n",i->ifa_name,address,(i->ifa_flags&IFF_LOOPBACK)?" (loopback)":"");
        }
        freeifaddrs(interfaces);
    }
    char path[EC_PATH];
    const char *bins[]={"bin/dropbear","bin/dropbearkey","bin/vsftpd",NULL};
    for (int i=0;bins[i];++i) {
        if (ec_path(path,sizeof path,c->root,bins[i])) return -1;
        fprintf(out,"%s: %s\n",bins[i],access(path,X_OK)?"missing":"available");
    }
    fprintf(out,"SSH: %s:%u auth=%s\nFTP: %s:%u passive=%u-%u\n",
        c->bind_address,c->ssh_port,c->auth==EC_KEYS?"key":c->auth==EC_PASSWORD?"password":"both",
        c->bind_address,c->ftp_port,c->pasv_min,c->pasv_max);
    fprintf(out,"FTP user: %s\nFTP mode: %s\n",c->ftp_user,
        geteuid()==0?"privileged; non-root target account and chroot required":
        "current user; starting directory only, no chroot confinement");
    fprintf(out,"FTP access: %s\n",c->ftp_auth==EC_FTP_PASSWORD?"password":"anonymous (no password)");
#ifdef __linux__
    FILE *mounts=fopen("/proc/mounts","r");
    if (mounts) {
        char line[8192];
        while (fgets(line,sizeof line,mounts))
            if (strstr(line," /mnt/ext1 ") || strstr(line," /tmp ")) fprintf(out,"Mount: %s",line);
        fclose(mounts);
    }
#endif
    return 0;
}
