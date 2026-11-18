/* Local-only 20E247 adapter. Default OFF. No direct sensor enable, route
 * mutation, dlopen, reset, private ivar write, or Apple client impersonation. */
#ifndef TG_NAMED_CLIENT_H
#define TG_NAMED_CLIENT_H
#include "TGClientModel.h"
#include <notify.h>
static _Atomic bool tgNamedSuppressReset;
static BOOL TGNamedResetSuppressed(void) { return atomic_load(&tgNamedSuppressReset); }

/* Runtime ABI validation, not selector presence alone. */
static BOOL TGNamedMethod(id o,const char *name,const char *types) {
    SEL s=sel_registerName(name);
    if(!o || ![o respondsToSelector:s])return NO;
    Method m=class_getInstanceMethod(object_getClass(o),s);
    unsigned n=(unsigned)strlen(types)-1;
    if(!m || method_getNumberOfArguments(m)!=n+2)return NO;
    char b[128]={0};method_getReturnType(m,b,sizeof(b));
    if(b[0]!=types[0] || b[1])return NO;
    for(unsigned i=0;i<n;i++) {
        memset(b,0,sizeof(b));method_getArgumentType(m,i+2,b,sizeof(b));
        if(types[i+1]=='@') { if(b[0]!='@')return NO; }
        else if(b[0]!=types[i+1] || b[1])return NO;
    }
    return YES;
}
static void TGNamedOne(id o,const char *s,id arg) {
    ((void(*)(id,SEL,id))objc_msgSend)(o,sel_registerName(s),arg);
}
@interface TGNamedRecovery : NSObject {
    id _observer,_routes,_manager;
    NSString *_client;
    NSUUID *_callUUID;
    struct TGClientModel _model;
    BOOL _routeRegistered,_sensorRegistered,_callRegistered,_cleanupQueued;
    BOOL _requestMayExist,_terminal;
    _Atomic bool _eventSeen;
    int _settingsToken;
    uint64_t _generation;
}
+ (instancetype)shared;
- (void)beginWithBoot:(uint64_t)tokenBoot created:(uint64_t)created;
- (void)invalidate:(const char *)reason;
- (void)detach;
- (void)tracePostRemoval;
- (void)finishCallSuppressionIfEmpty;
@end

@implementation TGNamedRecovery
+ (instancetype)shared {
    static TGNamedRecovery *s;static dispatch_once_t once;
    dispatch_once(&once,^{s=[TGNamedRecovery new];});return s;
}
- (instancetype)init { if((self=[super init]))_settingsToken=-1;return self; }
- (void)finishCallSuppressionIfEmpty {
    if(![NSThread isMainThread])return;
    @try {
        id calls=TGProbeObject(_observer,"calls");
        if(![calls isKindOfClass:[NSArray class]] || [calls count]>16)return;
        for(id call in calls) if(TGProbeBool(call,"hasEnded")!=1)return;
        atomic_store(&tgNamedSuppressReset,false);
        _callUUID=nil;
        if(_terminal && _callRegistered) {
            ((void(*)(id,SEL,id,dispatch_queue_t))objc_msgSend)(_observer,sel_registerName("setDelegate:queue:"),nil,dispatch_get_main_queue());
            _callRegistered=NO;_observer=nil;
        }
    } @catch(__unused NSException *e) { TGTrace("named_suppression_snapshot_error"); }
}
- (void)detach {
    /* Defer observer detachment until after the native callback turn. Exact
       route/permission implementations enumerate allObjects snapshots; this
       deferral is conservative, not needed for a live-table mutation issue. */
    if(![NSThread isMainThread] || _requestMayExist)return;
    @try {if(_routeRegistered){TGNamedOne(_routes,"removeDelegate:",self);_routeRegistered=NO;}}
    @catch(__unused NSException *e){TGTrace("named_route_detach_error");}
    @try {if(_sensorRegistered){TGNamedOne(_manager,"removeObserver:",self);_sensorRegistered=NO;}}
    @catch(__unused NSException *e){TGTrace("named_sensor_detach_error");}
    if(_settingsToken>=0){notify_cancel(_settingsToken);_settingsToken=-1;}
    [self finishCallSuppressionIfEmpty];
    /* Keep CX observer only while a recovered call may still be live. It
       cannot reacquire. This protects original reset suppression until empty. */
}
- (void)tracePostRemoval {
    @try {
        TGTrace("named_post_remove enabled=%d demand=%d permitted=%d",TGProbeBool(_manager,"isProximityDetectionEnabled"),TGProbeBool(_manager,"_clientsWantDetectionEnabled"),TGProbeBool(_manager,"isProximityDetectionPermitted"));
    } @catch(__unused NSException *e) { TGTrace("named_post_remove metadata_unknown=1"); }
}
- (void)invalidate:(const char *)reason {
    atomic_store(&_eventSeen,true);
    if(![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(),^{[self invalidate:"unexpected_callback_queue"];});return;
    }
    _terminal=YES;
    (void)TGClientInvalidate(&_model);
    TGTrace("named_terminal reason=%s request_may_exist=%d",reason,_requestMayExist);
#if TG_CLIENT_ACTUATOR_ENABLED
    if(_requestMayExist) {
        @try {
            ((void(*)(id,SEL,id,BOOL))objc_msgSend)(_manager,sel_registerName("client:wantsProximityDetectionEnabled:"),_client,NO);
            _requestMayExist=NO;
            TGTrace("named_own_request_removed");
            [self tracePostRemoval];
        } @catch(__unused NSException *e) {
            /* Preserve manager/client for one release retry, never pretend
               a failed removal succeeded, never remove another client's key. */
            TGTrace("named_remove_exception pending=1");
        }
    }
#endif
    if(!_cleanupQueued) {
        _cleanupQueued=YES;
        dispatch_async(dispatch_get_main_queue(),^{
#if TG_CLIENT_ACTUATOR_ENABLED
            if(self->_requestMayExist) {
                @try {
                    ((void(*)(id,SEL,id,BOOL))objc_msgSend)(self->_manager,sel_registerName("client:wantsProximityDetectionEnabled:"),self->_client,NO);
                    self->_requestMayExist=NO;TGTrace("named_remove_retry_success");
                    [self tracePostRemoval];
                } @catch(__unused NSException *e) { TGTrace("named_remove_retry_failed pending=1"); }
            }
#endif
            self->_cleanupQueued=NO; /* Later successful event retry must be able to detach. */
            [self detach];
        });
    }
}
- (void)callObserver:(id)observer callChanged:(id)call {
    (void)observer;
    /* ALWAYS release first, never retain/reacquire based on event identity. */
    [self invalidate:"call_event"];
    if([NSThread isMainThread]) {
        @try {
            id uuid=TGProbeObject(call,"UUID");
            int same=[uuid isKindOfClass:[NSUUID class]] && _callUUID ? [uuid isEqual:_callUUID] : -1;
            TGTrace("named_call_event same_call=%d connected=%d ended=%d held=%d",same,TGProbeBool(call,"hasConnected"),TGProbeBool(call,"hasEnded"),TGProbeBool(call,"isOnHold"));
        } @catch(__unused NSException *e) { TGTrace("named_call_event metadata_unknown=1"); }
        [self finishCallSuppressionIfEmpty];
    }
}
- (void)routesChangedForRouteController:(id)controller {
    (void)controller;[self invalidate:"route_event"];
}
- (void)proximitySensorManager:(id)manager proximityDetectionPermittedDidChange:(BOOL)permitted {
    (void)manager;(void)permitted;[self invalidate:"permission_event"];
}
- (void)beginWithBoot:(uint64_t)tokenBoot created:(uint64_t)created {
    if(![NSThread isMainThread] || _model.attempted || _terminal)return;
    _generation++;
    @try {
        Class cx=NSClassFromString(@"CXCallObserver");
        _observer=cx ? [[cx alloc] init] : nil;
        if(!TGNamedMethod(_observer,"setDelegate:queue:","v@@")) { [self invalidate:"call_delegate_abi"];return; }
        _callRegistered=YES;
        atomic_store(&tgNamedSuppressReset,true);
        ((void(*)(id,SEL,id,dispatch_queue_t))objc_msgSend)(_observer,sel_registerName("setDelegate:queue:"),self,dispatch_get_main_queue());
        id center=TGProbeObject(NSClassFromString(@"TUCallCenter"),"sharedInstance");
        _routes=TGProbeObject(center,"routeController");
        id queue=TGProbeObject(_routes,"queue");
        if(queue!=(id)dispatch_get_main_queue() ||
           !TGNamedMethod(_routes,"addDelegate:","v@") || !TGNamedMethod(_routes,"removeDelegate:","v@")) {
            [self invalidate:"route_main_queue_or_abi"];return;
        }
        _routeRegistered=YES;TGNamedOne(_routes,"addDelegate:",self);
        id sb=TGProbeObject(NSClassFromString(@"SpringBoard"),"sharedApplication");
        _manager=TGProbeObject(sb,"proximitySensorManager");
        if(!TGNamedMethod(_manager,"client:wantsProximityDetectionEnabled:","v@B") ||
           !TGNamedMethod(_manager,"addObserver:","v@") || !TGNamedMethod(_manager,"removeObserver:","v@")) {
            [self invalidate:"sensor_abi"];return;
        }
        _sensorRegistered=YES;TGNamedOne(_manager,"addObserver:",self);
        if(notify_register_dispatch("com.mohsen.touchguard.settings-changed",&_settingsToken,dispatch_get_main_queue(),^(int t){
            (void)t;[self invalidate:"settings_event"];
        })!=NOTIFY_STATUS_OK) { _settingsToken=-1;[self invalidate:"settings_observer"];return; }
        /* Delegates are installed BEFORE snapshot. No background snapshots,
           cross-queue sync, polling, or second acquisition are allowed. */
        struct TGClientFacts f={0};
        f.freshOwnReset=1;f.newHost=1;f.callDelegateReady=_callRegistered;f.routeDelegateReady=_routeRegistered;
        id calls=TGProbeObject(_observer,"calls");int live=0;id selectedCall=nil;
        if(![calls isKindOfClass:[NSArray class]] || [calls count]>16)f.callUnknown=1;
        else for(id call in calls) {
            int c=TGProbeBool(call,"hasConnected"),e=TGProbeBool(call,"hasEnded"),h=TGProbeBool(call,"isOnHold");
            if(c<0||e<0||h<0){f.callUnknown=1;continue;}
            if(!e){live++;selectedCall=call;if(c)f.connectedCount++;if(h)f.heldCount++;}
        }
        if(live!=1)f.callUnknown=1;
        id uuid=TGProbeObject(selectedCall,"UUID");
        if(![uuid isKindOfClass:[NSUUID class]])f.callUnknown=1;
        else _callUUID=[uuid copy];
        id picked=TGProbeObject(_routes,"pickedRoute");
        int receiver=TGProbeBool(picked,"isReceiver"),speaker=TGProbeBool(picked,"isSpeaker");
        f.routeKnown=(receiver>=0 && speaker>=0);f.receiver=receiver;f.speaker=speaker;
        f.managerReady=_manager!=nil;f.permitted=TGProbeBool(_manager,"isProximityDetectionPermitted");
        f.clientsWant=TGProbeBool(_manager,"_clientsWantDetectionEnabled");
        uint64_t boot=0,now=0;
        TGTrace("named_snapshot live=%d connected=%d held=%d unknown=%d route_known=%d receiver=%d speaker=%d permitted=%d demand=%d enabled=%d",
                live,f.connectedCount,f.heldCount,f.callUnknown,f.routeKnown,f.receiver,f.speaker,f.permitted,f.clientsWant,
                TGProbeBool(_manager,"isProximityDetectionEnabled"));
        if(_terminal || atomic_load(&_eventSeen) || !TGProbeClock(&boot,&now) || boot!=tokenBoot || now<created || now-created>TG_PROBE_TTL || !prefBool(@"guardEnabled",YES) || !prefBool(@"resetOnCallStart",YES) ||
           TGProbeBool(_manager,"isProximityDetectionEnabled")!=0 ||
           TGClientBegin(&_model,&f,now)!=TG_WOULD_ADD_OWN_CLIENT) { [self invalidate:"eligibility"];return; }
        /* Recheck only the same single call before committing the request.
           UUID exists in process memory only, never in trace/token/files. */
        id finalCalls=TGProbeObject(_observer,"calls");int finalLive=0;BOOL same=NO;
        if([finalCalls isKindOfClass:[NSArray class]] && [finalCalls count]<=16) {
            for(id call in finalCalls) {
                int ended=TGProbeBool(call,"hasEnded");
                if(ended<0){finalLive=-100;break;}
                if(!ended) {
                    finalLive++;
                    id current=TGProbeObject(call,"UUID");
                    same=[current isKindOfClass:[NSUUID class]] && [current isEqual:_callUUID] &&
                         TGProbeBool(call,"hasConnected")==1 && TGProbeBool(call,"isOnHold")==0;
                }
            }
        }
        if(finalLive!=1 || !same || _terminal || atomic_load(&_eventSeen)) { [self invalidate:"same_call_recheck"];return; }
        _client=[@"com.mohsen.touchguard.recovery." stringByAppendingString:[[NSUUID UUID] UUIDString]];
        uint64_t generation=_generation;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)TG_CLIENT_WINDOW_SECONDS*NSEC_PER_SEC),dispatch_get_main_queue(),^{
            if(generation==self->_generation)[self invalidate:"deadline"];
        });
        TGTrace("named_eligible lease_seconds=%d actuator_enabled=%d",TG_CLIENT_WINDOW_SECONDS,TG_CLIENT_ACTUATOR_ENABLED);
#if TG_CLIENT_ACTUATOR_ENABLED
        if(atomic_load(&_eventSeen)) { [self invalidate:"event_before_add"];return; }
        _requestMayExist=YES; /* Set before mutation, covers exceptions/reentrancy. */
        ((void(*)(id,SEL,id,BOOL))objc_msgSend)(_manager,sel_registerName("client:wantsProximityDetectionEnabled:"),_client,YES);
        int enabled=TGProbeBool(_manager,"isProximityDetectionEnabled");
        TGTrace("named_own_request_added enabled=%d",enabled);
        if(enabled!=1 || TGProbeBool(_manager,"isProximityDetectionPermitted")!=1)
            [self invalidate:"post_add_state"];
#else
        TGTrace("named_dry_run no_request_sent");
#endif
    } @catch(__unused NSException *e) { [self invalidate:"setup_exception"]; }
}
@end

static void TGPrepareNamedRecovery(void) {
    uint64_t boot=0,now=0;
    int ok=TGProbeClock(&boot,&now) && TGTokenWrite(boot,now,getpid());
    TGTrace("named_token prepared=%d actuator_enabled=%d",ok,TG_CLIENT_ACTUATOR_ENABLED);
}
static void TGCancelNamedRecovery(void) {
    int saved=errno,fd=TGTokenOpen();
    if(fd>=0) {
        struct TGProbeToken t;
        if(read(fd,&t,sizeof(t))==(ssize_t)sizeof(t) && t.origin==getpid())(void)TGTokenUnlink(fd);
        close(fd);
    }
    errno=saved;
}
static void TGRunPostlaunchNamedRecovery(void) {
    uint64_t boot=0,now=0;struct TGProbeToken token;
    if(!TGProbeClock(&boot,&now) || !TGTokenConsume(boot,now,getpid(),&token))return;
    TGTrace("named_token claimed age=%llu actuator_enabled=%d",(unsigned long long)(now-token.created),TG_CLIENT_ACTUATOR_ENABLED);
    [[TGNamedRecovery shared] beginWithBoot:token.boot created:token.created];
}
#endif
