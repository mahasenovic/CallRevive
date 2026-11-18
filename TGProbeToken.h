/* Own-reset token for a read-only postlaunch probe. No call identifiers. */
#ifndef TG_PROBE_TOKEN_H
#define TG_PROBE_TOKEN_H
#include <stdint.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#ifndef TG_PROBE_TOKEN_PATH
#define TG_PROBE_TOKEN_PATH "/var/mobile/Library/Logs/MohsenTouchGuard-probe.pending"
#endif
#define TG_PROBE_TTL 30ULL
struct TGProbeToken { uint64_t magic, version, boot, created; int64_t origin; };
static int TGTokenOpen(void) {
    int fd=open(TG_PROBE_TOKEN_PATH,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0)return -1;
    struct stat s;
    if(fstat(fd,&s)!=0 || !S_ISREG(s.st_mode) || s.st_uid!=getuid() || s.st_nlink!=1 || (s.st_mode&0777)!=0600 || flock(fd,LOCK_EX|LOCK_NB)!=0) {close(fd);return -1;}
    return fd;
}
/* Remove only the inode we safely opened. A failed check is fail-closed. */
static int TGTokenUnlink(int fd) {
    struct stat a,b;
    if(fstat(fd,&a)!=0 || lstat(TG_PROBE_TOKEN_PATH,&b)!=0 || a.st_dev!=b.st_dev || a.st_ino!=b.st_ino)return 0;
    return unlink(TG_PROBE_TOKEN_PATH)==0;
}
static int TGTokenWrite(uint64_t boot,uint64_t now,pid_t origin) {
    int saved=errno,ok=0;
    int old=TGTokenOpen();
    if(old>=0){int removed=TGTokenUnlink(old);close(old);if(!removed)goto done;}
    else {struct stat exists;if(lstat(TG_PROBE_TOKEN_PATH,&exists)==0 || errno!=ENOENT)goto done;}
    char temp[sizeof(TG_PROBE_TOKEN_PATH)+40];
    int len=snprintf(temp,sizeof(temp),"%s.writing.%lld",TG_PROBE_TOKEN_PATH,(long long)origin);
    if(len<=0 || (size_t)len>=sizeof(temp))goto done;
    int fd=open(temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
    if(fd<0)goto done;
    struct TGProbeToken t={0x544750524f424531ULL,1,boot,now,origin};
    /* Publish a complete token atomically; no synchronous fsync delay. */
    int complete=write(fd,&t,sizeof(t))==(ssize_t)sizeof(t);
    close(fd);
    if(complete)ok=rename(temp,TG_PROBE_TOKEN_PATH)==0;
    if(!ok)(void)unlink(temp);
done: errno=saved;return ok;
}
/* Consume BEFORE returning success, including invalid/expired tokens. */
static int TGTokenConsume(uint64_t boot,uint64_t now,pid_t self,struct TGProbeToken *result) {
    int saved=errno,ok=0;int fd=TGTokenOpen();
    if(fd<0)goto done;
    struct TGProbeToken t;struct stat st;
    ssize_t n=read(fd,&t,sizeof(t));
    int exact=fstat(fd,&st)==0 && st.st_size==(off_t)sizeof(t);
    int removed=TGTokenUnlink(fd);close(fd);
    if(!removed || !exact || n!=(ssize_t)sizeof(t))goto done;
    if(t.magic!=0x544750524f424531ULL || t.version!=1 || t.boot!=boot || t.origin<=0 || t.origin==self || now<t.created || now-t.created>TG_PROBE_TTL)goto done;
    if(result)*result=t;
    ok=1;
done: errno=saved;return ok;
}
#endif
