#include "ec.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

static void pause_ms(unsigned ms) {
    struct timespec t={(time_t)(ms/1000),(long)(ms%1000)*1000000};
    while (nanosleep(&t,&t) && errno==EINTR) {}
}
static int child_status(pid_t pid) {
    int status;
    if (waitpid(pid,&status,0)!=pid || !WIFEXITED(status) || WEXITSTATUS(status))
        return ec_error("child command failed; see service log");
    return 0;
}
static int run(char *const argv[]) {
    pid_t pid=fork();
    if (pid<0) return ec_error("fork: %s",strerror(errno));
    if (!pid) { execv(argv[0],argv); perror(argv[0]); _exit(127); }
    return child_status(pid);
}
int ec_services_open(ec_config *c, ec_services *s) {
    memset(s,0,sizeof *s); s->lock_fd=-1;
    char lockdir[EC_PATH], path[EC_PATH];
    snprintf(lockdir,sizeof lockdir,"/tmp/easy-connect-%lu",(unsigned long)geteuid());
    if (ec_private_dir(lockdir) || ec_path(path,sizeof path,lockdir,"lock")) return -1;
    s->lock_fd=open(path,O_RDWR|O_CREAT|O_NOFOLLOW,0600);
    if (s->lock_fd<0) return ec_error("open lock: %s",strerror(errno));
    fcntl(s->lock_fd,F_SETFD,FD_CLOEXEC);
    if (flock(s->lock_fd,LOCK_EX|LOCK_NB)) {
        close(s->lock_fd); s->lock_fd=-1;
        return ec_error("another Easy Connect session is running");
    }
    if (ec_path(c->runtime,sizeof c->runtime,lockdir,"session.XXXXXX") || !mkdtemp(c->runtime)) {
        close(s->lock_fd); s->lock_fd=-1;
        return ec_error("cannot create private session directory");
    }
#ifdef __linux__
    /* Adopt daemon session grandchildren so closing the app closes sessions. */
    if (prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0)) {
        ec_services_close(c,s);
        return ec_error("kernel lacks child subreaper support");
    }
#endif
    return 0;
}
static int stage(ec_config *c, const char *name, int required) {
    char source[EC_PATH], target[EC_PATH];
    if (ec_path(source,sizeof source,c->state,name) || ec_path(target,sizeof target,c->runtime,name)) return -1;
    return ec_copy_private(source,target,required);
}
static int hostkey(ec_config *c) {
    char key[EC_PATH], tool[EC_PATH];
    if (ec_path(key,sizeof key,c->state,"host_ed25519") ||
        ec_path(tool,sizeof tool,c->root,"bin/dropbearkey")) return -1;
    struct stat st;
    if (lstat(c->state,&st)) {
        if (errno!=ENOENT || mkdir(c->state,0700)) return ec_error("create state directory: %s",strerror(errno));
    } else if (!S_ISDIR(st.st_mode)) return ec_error("state directory must not be a symlink");
    if (lstat(key,&st)) {
        if (errno!=ENOENT) return ec_error("stat host key: %s",strerror(errno));
        char temp[EC_PATH];
        if (ec_path(temp,sizeof temp,c->runtime,"new_host_ed25519")) return -1;
        char *argv[]={tool,"-t","ed25519","-f",temp,NULL};
        if (run(argv)) return -1;
        /* O_EXCL prevents overwriting a key installed concurrently over USB. */
        int fd=open(key,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
        if (fd<0) { unlink(temp); return ec_error("create host key: %s",strerror(errno)); }
        close(fd);
        if (ec_copy_private(temp,key,1)) { unlink(temp); unlink(key); return -1; }
        unlink(temp);
    }
    return stage(c,"host_ed25519",1);
}
static int available(ec_config *c,unsigned port) {
    int fd=socket(AF_INET,SOCK_STREAM,0),rc;
    if (fd<0) return ec_error("socket: %s",strerror(errno));
    struct sockaddr_in addr;
    memset(&addr,0,sizeof addr);
    addr.sin_family=AF_INET; addr.sin_port=htons((unsigned short)port);
    inet_pton(AF_INET,c->bind_address,&addr.sin_addr);
    rc=bind(fd,(struct sockaddr *)&addr,sizeof addr);
    close(fd);
    return rc ? ec_error("cannot bind %s:%u: %s",c->bind_address,port,strerror(errno)) : 0;
}
static int listening(ec_config *c,unsigned port) {
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if (fd<0) return 0;
    fcntl(fd,F_SETFL,O_NONBLOCK);
    struct sockaddr_in addr;
    memset(&addr,0,sizeof addr);
    addr.sin_family=AF_INET; addr.sin_port=htons((unsigned short)port);
    inet_pton(AF_INET,!strcmp(c->bind_address,"0.0.0.0")?"127.0.0.1":c->bind_address,&addr.sin_addr);
    int rc=connect(fd,(struct sockaddr *)&addr,sizeof addr);
    if (rc<0 && errno==EINPROGRESS) {
        fd_set writes;
        struct timeval timeout={0,10000};
        FD_ZERO(&writes); FD_SET(fd,&writes);
        if (select(fd+1,NULL,&writes,NULL,&timeout)>0) {
            int err=0; socklen_t size=sizeof err;
            if (!getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&size) && !err) rc=0;
        }
    }
    close(fd); return rc==0;
}
static volatile sig_atomic_t service_stopping=0;
static void request_service_stop(int sig) { (void)sig; service_stopping=1; }
static void signal_direct_children(int sig) {
#ifdef __linux__
    char path[128];
    snprintf(path,sizeof path,"/proc/self/task/%ld/children",(long)getpid());
    FILE *f=fopen(path,"r");
    if (f) {
        long pid;
        while (fscanf(f,"%ld",&pid)==1) if (pid>1) kill((pid_t)pid,sig);
        fclose(f);
    }
#else
    (void)sig;
#endif
}
/* One supervisor per daemon owns its session descendants on Linux. */
static void supervise(char *const argv[],pid_t expected_parent) {
    struct sigaction action;
    memset(&action,0,sizeof action); action.sa_handler=request_service_stop;
    service_stopping=0;
    sigaction(SIGTERM,&action,NULL); sigaction(SIGINT,&action,NULL); sigaction(SIGHUP,&action,NULL);
#ifdef __linux__
    if (prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0) ||
        prctl(PR_SET_PDEATHSIG,SIGTERM) || getppid()!=expected_parent) _exit(126);
#else
    (void)expected_parent;
#endif
    pid_t parent=getpid(),daemon=fork();
    if (daemon<0) _exit(126);
    if (!daemon) {
        action.sa_handler=SIG_DFL;
        sigaction(SIGTERM,&action,NULL); sigaction(SIGINT,&action,NULL); sigaction(SIGHUP,&action,NULL);
#ifdef __linux__
        if (prctl(PR_SET_PDEATHSIG,SIGTERM) || getppid()!=parent) _exit(126);
#else
        (void)parent;
#endif
        execv(argv[0],argv); perror(argv[0]); _exit(127);
    }
    int status=0,code=0,shutdown_ticks=0;
    while (1) {
        pid_t child;
        while ((child=waitpid(-1,&status,WNOHANG))>0) {
            if (child==daemon) {
                daemon=0;
                code=WIFEXITED(status)?WEXITSTATUS(status):1;
                service_stopping=1;
            }
        }
        if (child<0 && errno==ECHILD) _exit(code);
        if (service_stopping) {
            int sig=shutdown_ticks++<25?SIGTERM:SIGKILL;
            if (daemon>0) kill(daemon,sig);
            signal_direct_children(sig);
        }
        pause_ms(20);
    }
}
static int launch(ec_config *c,pid_t *slot,char *const argv[],const char *name,unsigned port) {
    char log[EC_PATH], filename[64], password[EC_PATH];
    snprintf(filename,sizeof filename,"%s.log",name);
    if (ec_path(log,sizeof log,c->runtime,filename) ||
        ec_path(password,sizeof password,c->runtime,!strcmp(name,"ftp")?"ftp-password.hash":"password.hash")) return -1;
    if (access(argv[0],X_OK)) return ec_error("missing executable %s",argv[0]);
    int fd=open(log,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    if (fd<0) return ec_error("create log: %s",strerror(errno));
    pid_t expected_parent=getpid();
    pid_t pid=fork();
    if (!pid) {
        setpgid(0,0);
        int input=open("/dev/null",O_RDONLY);
        if (input<0 || dup2(input,STDIN_FILENO)<0 || dup2(fd,STDOUT_FILENO)<0 || dup2(fd,STDERR_FILENO)<0) _exit(126);
        if (input>2) close(input);
        if (fd>2) close(fd);
        setenv("EASY_CONNECT_PASSWORD_FILE",password,1);
        supervise(argv,expected_parent);
    }
    close(fd);
    if (pid<0) return ec_error("fork: %s",strerror(errno));
    setpgid(pid,pid);
    *slot=pid;
    for (int i=0;i<100;++i) {
        int status;
        if (waitpid(pid,&status,WNOHANG)==pid) {
            *slot=0;
            return ec_error("%s exited during startup; inspect %s",name,log);
        }
        if (listening(c,port)) { printf("%s listening on %s:%u; log %s\n",name,c->bind_address,port,log); fflush(stdout); return 0; }
        pause_ms(20);
    }
    /* Keep ownership of the child until stop_all can reap it. */
    return ec_error("%s did not open its port; inspect %s",name,log);
}
static int start_ssh(ec_config *c,ec_services *s) {
    char daemon[EC_PATH], key[EC_PATH], keys[EC_PATH], address[96], pidfile[EC_PATH];
    if (s->ssh) return ec_error("SSH already running");
    if (available(c,c->ssh_port) || hostkey(c)) return -1;
    if (ec_path(daemon,sizeof daemon,c->root,"bin/dropbear") ||
        ec_path(key,sizeof key,c->runtime,"host_ed25519") ||
        ec_path(keys,sizeof keys,c->runtime,"authorized_keys") ||
        ec_path(pidfile,sizeof pidfile,c->runtime,"dropbear.pid")) return -1;
    if (c->auth!=EC_PASSWORD) { if (stage(c,"authorized_keys",1)) return -1; }
    else {
        int fd=open(keys,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
        if (fd<0) return ec_error("cannot create empty authorized_keys");
        close(fd);
    }
    if (c->auth!=EC_KEYS && stage(c,"password.hash",1)) return -1;
    snprintf(address,sizeof address,"%s:%u",c->bind_address,c->ssh_port);
    char *argv[]={daemon,"-F","-E","-p",address,"-r",key,"-D",c->runtime,
        "-P",pidfile,"-T","3","-I","900","-j","-k",NULL,NULL};
    if (c->auth==EC_KEYS) argv[17]="-s";
    return launch(c,&s->ssh,argv,"ssh",c->ssh_port);
}
static int start_ftp(ec_config *c,ec_services *s) {
    if (s->ftp) return ec_error("FTP already running");
    int rootless=geteuid()!=0;
    struct passwd *pw=getpwnam(c->ftp_user);
    if (!pw || pw->pw_uid==0) return ec_error("FTP requires an existing non-root account: %s",c->ftp_user);
    if (rootless && pw->pw_uid!=geteuid())
        return ec_error("FTP must use the launching account; set ftp_user=current");
    char daemon[EC_PATH], config[EC_PATH], userlist[EC_PATH], empty[EC_PATH];
    if (available(c,c->ftp_port) ||
        ec_path(daemon,sizeof daemon,c->root,"bin/vsftpd") ||
        ec_path(config,sizeof config,c->runtime,"vsftpd.conf") ||
        ec_path(userlist,sizeof userlist,c->runtime,"ftp-users") ||
        ec_path(empty,sizeof empty,c->runtime,"empty")) return -1;
    int anonymous=c->ftp_auth==EC_FTP_ANONYMOUS;
    if (!anonymous && stage(c,"ftp-password.hash",1)) return -1;
    struct stat st;
    if (lstat(c->ftp_root,&st) || !S_ISDIR(st.st_mode)) return ec_error("FTP root must be an existing directory, not a symlink");
    if (ec_private_dir(empty)) return -1;
    FILE *f=fopen(userlist,"w");
    if (!f) return ec_error("cannot write FTP user list");
    fprintf(f,"%s\n",anonymous?"anonymous\nftp":c->ftp_user);
    if (fclose(f)) return ec_error("cannot save FTP user list");
    f=fopen(config,"w");
    if (!f) return ec_error("cannot write FTP config");
    fprintf(f,"listen=YES\nbackground=NO\nlisten_address=%s\nlisten_port=%u\n"
        "anonymous_enable=%s\nlocal_enable=%s\nwrite_enable=YES\nno_anon_password=YES\n"
        "anon_upload_enable=YES\nanon_mkdir_write_enable=YES\nanon_other_write_enable=YES\n"
        "anon_world_readable_only=NO\nanon_umask=022\n"
        "chroot_local_user=%s\nallow_writeable_chroot=YES\nlocal_root=%s\n"
        "run_as_launching_user=%s\none_process_model=%s\nftp_username=%s\nanon_root=%s\n"
        "nopriv_user=%s\nsecure_chroot_dir=%s\nuserlist_enable=YES\n"
        "userlist_deny=NO\nuserlist_file=%s\ncheck_shell=NO\n"
        "pasv_enable=YES\nport_enable=NO\npasv_min_port=%u\npasv_max_port=%u\n"
        "max_clients=3\nmax_per_ip=2\nidle_session_timeout=300\n"
        "local_umask=022\nsession_support=NO\nxferlog_enable=NO\nseccomp_sandbox=%s\n",
        c->bind_address,c->ftp_port,anonymous?"YES":"NO",anonymous?"NO":"YES",
        rootless?"NO":"YES",c->ftp_root,
        rootless?"YES":"NO",rootless?"YES":"NO",c->ftp_user,c->ftp_root,
        c->ftp_user,empty,userlist,c->pasv_min,c->pasv_max,rootless?"NO":"YES");
    if (fclose(f)) return ec_error("cannot save FTP config");
    char *argv[]={daemon,config,NULL};
    return launch(c,&s->ftp,argv,"ftp",c->ftp_port);
}
int ec_start(ec_config *c,ec_services *s,const char *name) {
    if (!strcmp(name,"ssh")) return start_ssh(c,s);
    if (!strcmp(name,"ftp")) return start_ftp(c,s);
    return ec_error("unknown service: %s",name);
}
void ec_poll(ec_services *s) {
    int status;
    if (s->ssh && waitpid(s->ssh,&status,WNOHANG)==s->ssh) { fprintf(stderr,"SSH listener exited\n"); s->ssh=0; }
    if (s->ftp && waitpid(s->ftp,&status,WNOHANG)==s->ftp) { fprintf(stderr,"FTP listener exited\n"); s->ftp=0; }
}
int ec_stop(ec_config *c,ec_services *s,const char *name) {
    (void)c;
    pid_t *slot=!strcmp(name,"ssh")?&s->ssh:!strcmp(name,"ftp")?&s->ftp:NULL;
    if (!slot) return ec_error("unknown service: %s",name);
    if (!*slot) return 0;
    int status;
    pid_t already_reaped=waitpid(*slot,&status,WNOHANG);
    if (already_reaped==*slot || (already_reaped<0 && errno==ECHILD)) { *slot=0; return 0; }
    /* Signal only this supervisor's process group; the other daemon continues. */
    kill(-*slot,SIGTERM);
    for (int i=0;i<150;++i) {
        pid_t reaped=waitpid(*slot,&status,WNOHANG);
        if (reaped==*slot || (reaped<0 && errno==ECHILD)) { *slot=0; return 0; }
        pause_ms(20);
    }
    return ec_error("%s is still stopping; try again",name);
}
static void signal_children(ec_services *s,int sig) {
    if (s->ssh>0) kill(-s->ssh,sig);
    if (s->ftp>0) kill(-s->ftp,sig);
#ifdef __linux__
    char path[128];
    snprintf(path,sizeof path,"/proc/self/task/%ld/children",(long)getpid());
    FILE *f=fopen(path,"r");
    if (f) {
        long child;
        while (fscanf(f,"%ld",&child)==1)
            if (child>1) kill((pid_t)child,sig);
        fclose(f);
    }
#endif
}
void ec_stop_all(ec_config *c,ec_services *s) {
    (void)c;
    /* On Linux, orphaned session descendants are adopted and terminated too. */
    for (int i=0;i<100;++i) {
        signal_children(s,i<25?SIGTERM:SIGKILL);
        int status;
        pid_t reaped;
        while ((reaped=waitpid(-1,&status,WNOHANG))>0) {
            if (reaped==s->ssh) s->ssh=0;
            if (reaped==s->ftp) s->ftp=0;
        }
        if (reaped<0 && errno==ECHILD) break;
        pause_ms(20);
    }
    s->ssh=s->ftp=0;
}
void ec_services_close(ec_config *c,ec_services *s) {
    ec_stop_all(c,s);
    /* Logs deliberately survive until reboot for diagnostics; secrets do not. */
    const char *secrets[]={"authorized_keys","password.hash","ftp-password.hash","host_ed25519",NULL};
    char path[EC_PATH];
    if (*c->runtime) for (int i=0;secrets[i];++i)
        if (!ec_path(path,sizeof path,c->runtime,secrets[i])) unlink(path);
    if (s->lock_fd>=0) { close(s->lock_fd); s->lock_fd=-1; }
}
int ec_fingerprint(const ec_config *c) {
    char tool[EC_PATH],key[EC_PATH];
    if (ec_path(tool,sizeof tool,c->root,"bin/dropbearkey") || ec_path(key,sizeof key,c->state,"host_ed25519")) return -1;
    char *argv[]={tool,"-y","-f",key,NULL};
    return run(argv);
}
int ec_key_info(const ec_config *c,char *out,size_t size) {
    char tool[EC_PATH],key[EC_PATH];
    int channel[2];
    if (!size) return -1;
    *out=0;
    if (ec_path(tool,sizeof tool,c->root,"bin/dropbearkey") ||
        ec_path(key,sizeof key,c->state,"host_ed25519") || pipe(channel)) return -1;
    pid_t pid=fork();
    if (!pid) {
        close(channel[0]);
        if (dup2(channel[1],STDOUT_FILENO)<0) _exit(126);
        close(channel[1]);
        execl(tool,tool,"-y","-f",key,(char *)NULL);
        _exit(127);
    }
    close(channel[1]);
    if (pid<0) { close(channel[0]); return -1; }
    char buffer[1024];
    size_t used=0;
    ssize_t n;
    /* Drain fully even if the caller requests only a short output. */
    while ((n=read(channel[0],buffer,sizeof buffer))>0) {
        size_t take=(size_t)n;
        if (take>size-1-used) take=size-1-used;
        memcpy(out+used,buffer,take); used+=take;
    }
    out[used]=0;
    close(channel[0]);
    return child_status(pid);
}
