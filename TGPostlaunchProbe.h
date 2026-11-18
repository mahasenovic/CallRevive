/* Shared checked clock/getter helpers only. No probe or actuator here. */
#ifndef TG_POSTLAUNCH_PROBE_H
#define TG_POSTLAUNCH_PROBE_H
#include "TGProbeToken.h"
#include <time.h>
#include <sys/sysctl.h>

static BOOL TGProbeClock(uint64_t *boot,uint64_t *now) {
    struct timeval b={0};size_t len=sizeof(b);struct timespec t={0};
    int mib[2]={CTL_KERN,KERN_BOOTTIME};
    if(sysctl(mib,2,&b,&len,NULL,0)!=0 || len!=sizeof(b) || b.tv_sec<=0 || clock_gettime(CLOCK_MONOTONIC,&t)!=0 || t.tv_sec<0)return NO;
    *boot=(uint64_t)b.tv_sec;*now=(uint64_t)t.tv_sec;return YES;
}
/* Only zero-argument object or BOOL getters with exact runtime ABI. */
static BOOL TGProbeGetter(id obj,SEL sel,char kind) {
    if(!obj || ![obj respondsToSelector:sel])return NO;
    Method m=class_getInstanceMethod(object_getClass(obj),sel);
    if(!m || method_getNumberOfArguments(m)!=2)return NO;
    char ret[16]={0};method_getReturnType(m,ret,sizeof(ret));
    return kind=='@' ? (ret[0]=='@' && ret[1]==0) : ((ret[0]=='B'||ret[0]=='c') && ret[1]==0);
}
static id TGProbeObject(id obj,const char *name) {
    SEL s=sel_registerName(name);
    return TGProbeGetter(obj,s,'@') ? ((id(*)(id,SEL))objc_msgSend)(obj,s) : nil;
}
static int TGProbeBool(id obj,const char *name) {
    SEL s=sel_registerName(name);
    return TGProbeGetter(obj,s,'B') ? (int)((BOOL(*)(id,SEL))objc_msgSend)(obj,s) : -1;
}
#endif
