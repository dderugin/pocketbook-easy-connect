/* Two-tab InkView interface. All coordinates use a 720 x 960 logical canvas. */
#include "ec.h"
#include <inkview.h>
#include <openssl/crypto.h>
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static ec_config config;
static ec_services services;
static ifont *body_font,*small_font,*title_font;
static int opened=0,tab=0,editing=0,password_tab=0;
static char notices[2][256]={{0},{0}};
static char fingerprint[128]="Created when SSH starts.";
static char last_address[64]={0};
static char keyboard[EC_PATH],pending_password[1025];
enum { EDIT_NONE,EDIT_SSH_PORT,EDIT_FTP_PORT,EDIT_PASV_MIN,EDIT_PASV_MAX,
       EDIT_FTP_ROOT,EDIT_PASSWORD,EDIT_PASSWORD_CONFIRM };
static void draw(void);
static void keyboard_result(char *text);
static int px(int x) { return x*ScreenWidth()/720; }
static int py(int y) { return y*ScreenHeight()/960; }
static const char *service_name(void) { return tab?"ftp":"ssh"; }
static int running(void) { return tab?services.ftp!=0:services.ssh!=0; }
static void note(const char *message) { snprintf(notices[tab],sizeof notices[tab],"%s",message); }
static void text(int x,int y,int w,int h,const char *value,ifont *font,int color) {
    SetFont(font,color);
    DrawTextRect(px(x),py(y),px(w),py(h),value,ALIGN_LEFT);
}
static void button(int x,int y,int w,int h,const char *label,int selected,int enabled) {
    if (selected) FillArea(px(x),py(y),px(w),py(h),BLACK);
    else DrawRect(px(x),py(y),px(w),py(h),enabled?BLACK:LGRAY);
    text(x+12,y+12,w-24,h-16,label,body_font,selected?WHITE:enabled?BLACK:DGRAY);
}
static void address(char *out,size_t size) {
    struct ifaddrs *interfaces=NULL;
    snprintf(out,size,"not connected");
    if (getifaddrs(&interfaces)) return;
    for (struct ifaddrs *i=interfaces;i;i=i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family!=AF_INET ||
            !(i->ifa_flags&IFF_UP) || (i->ifa_flags&IFF_LOOPBACK)) continue;
        struct sockaddr_in *a=(struct sockaddr_in *)i->ifa_addr;
        if (inet_ntop(AF_INET,&a->sin_addr,out,(socklen_t)size)) break;
    }
    freeifaddrs(interfaces);
}
static void update_fingerprint(void) {
    char info[1024],path[EC_PATH];
    if (ec_path(path,sizeof path,config.state,"host_ed25519") || access(path,R_OK)) return;
    if (!ec_key_info(&config,info,sizeof info)) {
        char *hash=strstr(info,"SHA256:");
        if (hash) { hash[strcspn(hash,"\r\n")]=0; snprintf(fingerprint,sizeof fingerprint,"%.127s",hash); }
    }
}
static void draw(void) {
    if (editing || !body_font) return;
    ClearScreen();
    text(24,20,672,45,"Easy Connect",title_font,BLACK);
    char line[EC_PATH+128],ip[64];
    address(ip,sizeof ip);
    snprintf(last_address,sizeof last_address,"%s",ip);
    snprintf(line,sizeof line,"Device address: %s",ip);
    text(24,68,672,32,line,small_font,BLACK);
    button(24,110,336,62,"SSH",tab==0,1);
    button(360,110,336,62,"FTP",tab==1,1);
    text(24,198,360,40,tab?"FTP daemon":"SSH daemon",title_font,BLACK);
    text(472,204,224,34,running()?"RUNNING":"STOPPED",body_font,BLACK);
    struct passwd *pw=getpwuid(geteuid());
    const char *user=tab?(config.ftp_auth==EC_FTP_ANONYMOUS?"anonymous":config.ftp_user):pw?pw->pw_name:"unknown";
    if (tab) snprintf(line,sizeof line,"ftp://%s@%s:%u/",user,ip,config.ftp_port);
    else snprintf(line,sizeof line,"ssh -p %u %s@%s",config.ssh_port,user,ip);
    text(24,248,672,48,line,body_font,BLACK);
    button(24,312,324,62,"Start",0,!running());
    button(372,312,324,62,"Stop",0,running());
    text(24,400,672,32,"Access type",body_font,BLACK);
    if (!tab) {
        button(24,440,216,52,"Public key",config.auth==EC_KEYS,!running());
        button(252,440,216,52,"Password",config.auth==EC_PASSWORD,!running());
        button(480,440,216,52,"Key or password",config.auth==EC_BOTH,!running());
        snprintf(line,sizeof line,"SSH port: %u    Edit",config.ssh_port);
        button(24,514,672,54,line,0,!running());
        if (config.auth!=EC_KEYS) {
            text(24,598,430,36,ec_has_password(&config,"ssh")?"Password: configured":"Password: not set",body_font,BLACK);
            button(480,584,216,54,"Set password",0,!running());
        } else text(24,586,672,52,"Connect with your private SSH key.",body_font,BLACK);
        if (config.auth!=EC_PASSWORD) {
            snprintf(line,sizeof line,"Public keys: %u entries",ec_key_count(&config));
            text(24,656,672,32,line,body_font,BLACK);
            snprintf(line,sizeof line,"Copy .pub keys to:\n%s/authorized_keys",config.state);
            text(24,698,672,72,line,small_font,BLACK);
        } else text(24,656,672,88,"Use the device account shown above and your Easy Connect SSH password.",body_font,BLACK);
        snprintf(line,sizeof line,"Device fingerprint\n%s",fingerprint);
        text(24,770,672,64,line,small_font,BLACK);
    } else {
        button(24,440,324,52,"Password",config.ftp_auth==EC_FTP_PASSWORD,!running());
        button(372,440,324,52,"No password",config.ftp_auth==EC_FTP_ANONYMOUS,!running());
        snprintf(line,sizeof line,"Port: %u",config.ftp_port);
        button(24,514,216,54,line,0,!running());
        snprintf(line,sizeof line,"Passive: %u",config.pasv_min);
        button(252,514,216,54,line,0,!running());
        snprintf(line,sizeof line,"to %u",config.pasv_max);
        button(480,514,216,54,line,0,!running());
        snprintf(line,sizeof line,"Starting folder: %s",config.ftp_root);
        button(24,590,672,70,line,0,!running());
        if (config.ftp_auth==EC_FTP_PASSWORD) {
            text(24,702,430,36,ec_has_password(&config,"ftp")?"Password: configured":"Password: not set",body_font,BLACK);
            button(480,688,216,54,"Set password",0,!running());
            text(24,764,672,58,"Login uses the account shown above. FTP transfers are unencrypted.",small_font,BLACK);
        } else text(24,690,672,70,"Login as anonymous; no password needed. Anyone on your network can access these files.",body_font,BLACK);
        if (geteuid()!=0) text(24,828,672,32,"Access follows this user's file permissions; no folder jail.",small_font,BLACK);
    }
    text(24,850,672,40,*notices[tab]?notices[tab]:"Stop this daemon before changing its settings.",small_font,BLACK);
    button(24,900,450,48,"Save diagnostics",0,1);
    button(498,900,198,48,"Exit",0,1);
    FullUpdate();
}
static int save_candidate(ec_config *candidate) {
    if (ec_config_save(candidate)) { note(ec_last_error()); return -1; }
    config=*candidate;
    note("Settings saved.");
    return 0;
}
static void edit(int field) {
    if (running()) { note("Stop this daemon before changing its settings."); draw(); return; }
    editing=field;
    const char *title="";
    int flags=KBD_NUMERIC;
    switch (field) {
    case EDIT_SSH_PORT: title="SSH port"; snprintf(keyboard,sizeof keyboard,"%u",config.ssh_port); break;
    case EDIT_FTP_PORT: title="FTP port"; snprintf(keyboard,sizeof keyboard,"%u",config.ftp_port); break;
    case EDIT_PASV_MIN: title="First passive port"; snprintf(keyboard,sizeof keyboard,"%u",config.pasv_min); break;
    case EDIT_PASV_MAX: title="Last passive port"; snprintf(keyboard,sizeof keyboard,"%u",config.pasv_max); break;
    case EDIT_FTP_ROOT: title="FTP starting folder"; snprintf(keyboard,sizeof keyboard,"%s",config.ftp_root); flags=0; break;
    case EDIT_PASSWORD: title="New password (12+ characters)"; *keyboard=0; flags=KBD_PASSWORD; password_tab=tab; break;
    case EDIT_PASSWORD_CONFIRM: title="Confirm new password"; *keyboard=0; flags=KBD_PASSWORD; break;
    }
    OpenKeyboard(title,keyboard,(field==EDIT_PASSWORD || field==EDIT_PASSWORD_CONFIRM)?1025:(int)sizeof keyboard,flags,keyboard_result);
}
static void keyboard_result(char *value) {
    int field=editing;
    editing=0;
    if (!value) {
        OPENSSL_cleanse(keyboard,sizeof keyboard); OPENSSL_cleanse(pending_password,sizeof pending_password);
        note("Edit cancelled."); draw(); return;
    }
    if (field==EDIT_PASSWORD) {
        size_t length=strlen(value);
        size_t characters=0;
        for (const unsigned char *p=(const unsigned char *)value; *p; ++p)
            if ((*p&0xc0)!=0x80) ++characters;
        if (characters<12 || length>1024 || strpbrk(value,"\r\n")) {
            note("Use at least 12 characters, without line breaks.");
            OPENSSL_cleanse(keyboard,sizeof keyboard); draw(); return;
        }
        memcpy(pending_password,value,length+1);
        OPENSSL_cleanse(keyboard,sizeof keyboard);
        edit(EDIT_PASSWORD_CONFIRM);
        return;
    }
    if (field==EDIT_PASSWORD_CONFIRM) {
        if (strcmp(value,pending_password)) note("Passwords did not match; password unchanged.");
        else if (ec_set_password(&config,password_tab?"ftp":"ssh",pending_password)) note(ec_last_error());
        else note("Password saved.");
        OPENSSL_cleanse(keyboard,sizeof keyboard); OPENSSL_cleanse(pending_password,sizeof pending_password);
        draw(); return;
    }
    ec_config candidate=config;
    if (field==EDIT_FTP_ROOT) {
        if (strlen(value)>=sizeof candidate.ftp_root) { note("Folder path is too long."); draw(); return; }
        strcpy(candidate.ftp_root,value);
    } else {
        char *end;
        errno=0;
        unsigned long port=strtoul(value,&end,10);
        if (!*value || strspn(value,"0123456789")!=strlen(value) || *end || errno || port<1024 || port>65535) {
            note("Port must be a number between 1024 and 65535."); draw(); return;
        }
        switch (field) {
        case EDIT_SSH_PORT: candidate.ssh_port=(unsigned)port; break;
        case EDIT_FTP_PORT: candidate.ftp_port=(unsigned)port; break;
        case EDIT_PASV_MIN: candidate.pasv_min=(unsigned)port; break;
        case EDIT_PASV_MAX: candidate.pasv_max=(unsigned)port; break;
        }
    }
    save_candidate(&candidate);
    OPENSSL_cleanse(keyboard,sizeof keyboard);
    draw();
}
static void tap(int x,int y) {
    if (editing) return;
    ec_poll(&services);
    if (y>=110 && y<172 && x>=24 && x<696) { tab=x<360?0:1; draw(); return; }
    if (x<24 || x>=696) return;
    if (y>=312 && y<374) {
        if (x<348 && !running()) {
            if (ec_start(&config,&services,service_name())) note(ec_last_error());
            else { note("Daemon started."); if (!tab) update_fingerprint(); }
        } else if (x>=372 && running()) {
            if (ec_stop(&config,&services,service_name())) note(ec_last_error()); else note("Daemon stopped.");
        }
    } else if (y>=440 && y<492) {
        if (running()) note("Stop this daemon before changing its settings.");
        else {
            ec_config candidate=config;
            if (!tab) {
                if (x<240) candidate.auth=EC_KEYS;
                else if (x>=252 && x<468) candidate.auth=EC_PASSWORD;
                else if (x>=480) candidate.auth=EC_BOTH;
                else return;
            } else {
                if (x<348) candidate.ftp_auth=EC_FTP_PASSWORD;
                else if (x>=372) candidate.ftp_auth=EC_FTP_ANONYMOUS;
                else return;
            }
            save_candidate(&candidate);
        }
    } else if (y>=514 && y<568) {
        if (!tab) edit(EDIT_SSH_PORT);
        else if (x<240) edit(EDIT_FTP_PORT);
        else if (x>=252 && x<468) edit(EDIT_PASV_MIN);
        else if (x>=480) edit(EDIT_PASV_MAX);
    } else if (!tab && config.auth!=EC_KEYS && y>=584 && y<638 && x>=480) edit(EDIT_PASSWORD);
    else if (tab && y>=590 && y<660) edit(EDIT_FTP_ROOT);
    else if (tab && config.ftp_auth==EC_FTP_PASSWORD && y>=688 && y<742 && x>=480) edit(EDIT_PASSWORD);
    else if (y>=900 && y<948) {
        if (x>=498) { CloseApp(); return; }
        if (x<474) {
            char path[EC_PATH];
            if (!ec_path(path,sizeof path,config.root,"diagnostics.txt")) {
                FILE *out=fopen(path,"w");
                if (out) { ec_doctor(&config,out); fclose(out); note("Saved diagnostics.txt in the app folder."); }
                else note("Could not save diagnostics.");
            }
        }
    }
    draw();
}
static void refresh(void) {
    pid_t ssh=services.ssh,ftp=services.ftp;
    ec_poll(&services);
    if (ssh && !services.ssh) snprintf(notices[0],sizeof notices[0],"SSH exited. Check its log for details.");
    if (ftp && !services.ftp) snprintf(notices[1],sizeof notices[1],"FTP exited. Check its log for details.");
    char ip[64];
    address(ip,sizeof ip);
    if (ssh!=services.ssh || ftp!=services.ftp || strcmp(ip,last_address)) draw();
    SetHardTimer("easy-connect-status",refresh,1000);
}
static void background(void) {
    ClearTimer(refresh);
    ec_stop_all(&config,&services);
    snprintf(notices[0],sizeof notices[0],"Connections stopped when the app left the foreground.");
    snprintf(notices[1],sizeof notices[1],"Connections stopped when the app left the foreground.");
}
static int handler(int type,int p1,int p2) {
    switch (type) {
    case EVT_INIT: {
        int size=20*ScreenWidth()/720;
        if (size>20*ScreenHeight()/960) size=20*ScreenHeight()/960;
        body_font=OpenFont(DEFAULTFONT,size,0);
        small_font=OpenFont(DEFAULTFONT,size*4/5,0);
        title_font=OpenFont(DEFAULTFONT,size*3/2,1);
        if (!body_font || !small_font || !title_font) { CloseApp(); return 0; }
        update_fingerprint(); break;
    }
    case EVT_SHOW: draw(); SetHardTimer("easy-connect-status",refresh,1000); break;
    case EVT_POINTERUP: tap(p1*720/ScreenWidth(),p2*960/ScreenHeight()); break;
    case EVT_KEYPRESS: if (p1==KEY_BACK) CloseApp(); break;
    case EVT_HIDE: if (!editing) background(); break;
    case EVT_BACKGROUND: background(); break;
    case EVT_EXIT:
        ClearTimer(refresh);
        if (opened) { ec_services_close(&config,&services); opened=0; }
        if (body_font) { CloseFont(body_font); body_font=NULL; }
        if (small_font) { CloseFont(small_font); small_font=NULL; }
        if (title_font) { CloseFont(title_font); title_font=NULL; }
        OPENSSL_cleanse(keyboard,sizeof keyboard); OPENSSL_cleanse(pending_password,sizeof pending_password);
        break;
    }
    return 0;
}
int main(void) {
    umask(0077);
    const char *root="/mnt/ext1/system/share/easy-connect";
#ifdef EC_UI_TEST
    if (getenv("EC_UI_ROOT")) root=getenv("EC_UI_ROOT");
#endif
    if (ec_config_load(&config,root) || ec_services_open(&config,&services)) return 1;
    opened=1;
    InkViewMain(handler);
    if (opened) ec_services_close(&config,&services);
    return 0;
}
