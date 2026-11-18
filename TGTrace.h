/* Local numeric diagnostics only. No network, call identifiers, or sensor APIs. */
#ifndef TG_TRACE_H
#define TG_TRACE_H
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#define TG_TRACE_PATH "/var/mobile/Library/Logs/MohsenTouchGuard-trace.log"
static pthread_mutex_t tgTraceLock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned tgAttempt;

static void TGTrace(const char *fmt, ...) __attribute__((format(printf,1,2)));
static void TGTrace(const char *fmt, ...) {
    int saved = errno;
    if (pthread_mutex_trylock(&tgTraceLock) != 0) { errno = saved; return; }
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    char line[512];
    int n = snprintf(line, sizeof(line), "%lld.%06ld sb=%d attempt=%u ",
                     (long long)tv.tv_sec, (long)tv.tv_usec, getpid(), atomic_load_explicit(&tgAttempt, memory_order_relaxed));
    if (n < 0 || (size_t)n >= sizeof(line)-2) goto done;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line+n, sizeof(line)-(size_t)n-1, fmt, ap);
    va_end(ap);
    size_t len = strnlen(line, sizeof(line)-2);
    line[len++] = '\n';
    /* Refuse symlinks, non-regular files, other owners and hard links. */
    mkdir("/var/mobile/Library/Logs", 0755);
    int fd = open(TG_TRACE_PATH, O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC, 0600);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st)==0 && S_ISREG(st.st_mode) && st.st_uid==getuid() && st.st_nlink==1 && flock(fd, LOCK_EX|LOCK_NB)==0) {
            if (st.st_size + (off_t)len <= 65536 || ftruncate(fd,0)==0) {
                /* One bounded write; tracing failure never changes reset decisions. */
                (void)write(fd,line,len);
            }
            (void)flock(fd,LOCK_UN);
        }
        close(fd);
    }
done:
    pthread_mutex_unlock(&tgTraceLock);
    errno = saved;
}

static __attribute__((unused)) pid_t TGObservedBackboardPID(void) {
    int saved=errno;
    int mib[4]={CTL_KERN,KERN_PROC,KERN_PROC_ALL,0};
    size_t len=0;
    pid_t result=-1;
    if (sysctl(mib,4,NULL,&len,NULL,0)==0 && len>0 && len<=4*1024*1024) {
        struct kinfo_proc *p=malloc(len);
        if (p) {
            if (sysctl(mib,4,p,&len,NULL,0)==0) {
                for (size_t i=0;i<len/sizeof(*p);++i) {
                    if (strcmp(p[i].kp_proc.p_comm,"backboardd")==0) { result=p[i].kp_proc.p_pid; break; }
                }
            }
            free(p);
        }
    }
    errno=saved;
    return result;
}
#endif
