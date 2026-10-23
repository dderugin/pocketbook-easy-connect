#include "ec.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <pwd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

/* Netlink-based getifaddrs can be denied by reader firmware. Linux's older
 * interface ioctl still lets the launching account discover its IPv4 address. */
bool ec_network_address(char *out, size_t size) {
    struct ifaddrs *interfaces = NULL;
    if (!getifaddrs(&interfaces)) {
        for (struct ifaddrs *i = interfaces; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET ||
                !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) {
                continue;
            }
            const struct sockaddr_in *a = (const struct sockaddr_in *)i->ifa_addr;
            if (a->sin_addr.s_addr != INADDR_ANY &&
                inet_ntop(AF_INET, &a->sin_addr, out, (socklen_t)size)) {
                freeifaddrs(interfaces);
                return true;
            }
        }
        freeifaddrs(interfaces);
    }
#ifdef __linux__
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        struct ifreq requests[64];
        struct ifconf list = {.ifc_len = sizeof requests, .ifc_req = requests};
        if (!ioctl(fd, SIOCGIFCONF, &list)) {
            for (size_t i = 0; i < (size_t)list.ifc_len / sizeof requests[0]; ++i) {
                struct ifreq flags = requests[i];
                if (ioctl(fd, SIOCGIFFLAGS, &flags) || !(flags.ifr_flags & IFF_UP) ||
                    (flags.ifr_flags & IFF_LOOPBACK)) {
                    continue;
                }
                const struct sockaddr_in *a = (const struct sockaddr_in *)&requests[i].ifr_addr;
                if (a->sin_family == AF_INET && a->sin_addr.s_addr != INADDR_ANY &&
                    inet_ntop(AF_INET, &a->sin_addr, out, (socklen_t)size)) {
                    close(fd);
                    return true;
                }
            }
        }
        close(fd);
    }
#endif
    snprintf(out, size, "not connected");
    return false;
}

int ec_doctor(const ec_config *config, FILE *out) {
    struct utsname system_info;
    struct passwd *account = getpwuid(geteuid());
    struct statvfs storage_info;
    if (!uname(&system_info)) {
        fprintf(out, "OS: %s %s %s\n", system_info.sysname, system_info.release,
                system_info.machine);
    }
    fprintf(out, "UID: %lu GID: %lu\nAccount: %s\nShell: %s\n", (unsigned long)geteuid(),
            (unsigned long)getegid(), account ? account->pw_name : "unknown",
            account ? (*account->pw_shell ? account->pw_shell : "/bin/sh (empty account shell)")
                    : "unknown");
    fprintf(out, "Application: %s\nState: %s\n", config->root, config->state);
    if (!statvfs(config->root, &storage_info)) {
        fprintf(out, "Storage: %lu MiB free; flags 0x%lx\n",
                (unsigned long)((double)storage_info.f_bavail * storage_info.f_frsize / 1048576.0),
                (unsigned long)storage_info.f_flag);
    }
    int pty_fd = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    int pty_error = errno;
    fprintf(out, "PTY master: %s\n", pty_fd < 0 ? "unavailable" : "available");
    if (pty_fd < 0) {
        fprintf(out, "PTY /dev/ptmx: %s (errno %d)\nSSH without PTY: use ssh -T\n",
                strerror(pty_error), pty_error);
    }
    if (pty_fd >= 0) {
        close(pty_fd);
    }
    struct ifaddrs *interfaces = NULL;
    if (!getifaddrs(&interfaces)) {
        for (struct ifaddrs *i = interfaces; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP)) {
                continue;
            }
            char address[INET_ADDRSTRLEN];
            const struct sockaddr_in *a = (const struct sockaddr_in *)i->ifa_addr;
            if (inet_ntop(AF_INET, &a->sin_addr, address, sizeof address)) {
                fprintf(out, "Network: %s %s%s\n", i->ifa_name, address,
                        (i->ifa_flags & IFF_LOOPBACK) ? " (loopback)" : "");
            }
        }
        freeifaddrs(interfaces);
    }
    char network_address[64];
    if (ec_network_address(network_address, sizeof network_address)) {
        fprintf(out, "Connection address: %s\n", network_address);
    }
    char path[EC_PATH];
    const char *executables[] = {"bin/dropbear", "bin/dropbearkey", "bin/vsftpd", NULL};
    for (int i = 0; executables[i]; ++i) {
        if (ec_path(path, sizeof path, config->root, executables[i])) {
            return -1;
        }
        fprintf(out, "%s: %s\n", executables[i], access(path, X_OK) ? "missing" : "available");
    }
    fprintf(out, "SSH: %s:%u auth=%s\nFTP: %s:%u passive=%u-%u\n", config->bind_address,
            config->ssh_port,
            config->auth == EC_KEYS       ? "key"
            : config->auth == EC_PASSWORD ? "password"
                                          : "both",
            config->bind_address, config->ftp_port, config->pasv_min, config->pasv_max);
    fprintf(out, "SSH public keys: %u entries\nSSH password: %s\nFTP password: %s\n",
            ec_key_count(config), ec_has_password(config, "ssh") ? "configured" : "not set",
            ec_has_password(config, "ftp") ? "configured" : "not set");
    fprintf(out, "FTP user: %s\nFTP mode: %s\n", config->ftp_user,
            geteuid() == 0 ? "privileged; non-root target account and chroot required"
                           : "current user; starting directory only, no chroot confinement");
    fprintf(out, "FTP access: %s\n",
            config->ftp_auth == EC_FTP_PASSWORD ? "password" : "anonymous (no password)");
#ifdef __linux__
    FILE *mounts = fopen("/proc/mounts", "r");
    if (mounts) {
        char line[8192];
        while (fgets(line, sizeof line, mounts)) {
            if (strstr(line, " /mnt/ext1 ") || strstr(line, " /tmp ")) {
                fprintf(out, "Mount: %s", line);
            }
        }
        fclose(mounts);
    }
#endif
    return 0;
}
