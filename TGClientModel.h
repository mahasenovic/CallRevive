/* Pure decision model. Side effects are exclusively in TGNamedClient.h. */
#ifndef TG_CLIENT_MODEL_H
#define TG_CLIENT_MODEL_H
#include <stdint.h>
#ifndef TG_CLIENT_ACTUATOR_ENABLED
#define TG_CLIENT_ACTUATOR_ENABLED 0
#endif
#if TG_CLIENT_ACTUATOR_ENABLED != 0 && TG_CLIENT_ACTUATOR_ENABLED != 1
#error actuator must be exactly 0 or 1
#endif
#define TG_CLIENT_WINDOW_SECONDS 300
struct TGClientFacts {
 int freshOwnReset, newHost, callDelegateReady, routeDelegateReady;
 int connectedCount, heldCount, callUnknown;
 int routeKnown, receiver, speaker;
 int managerReady, permitted, clientsWant;
};
struct TGClientModel { int attempted, done, wouldHold; uint64_t deadline; };
enum TGClientAction { TG_NONE, TG_WOULD_ADD_OWN_CLIENT, TG_WOULD_REMOVE_OWN_CLIENT };
static inline int TGClientEligible(const struct TGClientFacts *f) {
 return f->freshOwnReset==1 && f->newHost==1 && f->callDelegateReady==1 && f->routeDelegateReady==1 &&
 f->connectedCount==1 && f->heldCount==0 && f->callUnknown==0 && f->routeKnown==1 && f->receiver==1 &&
 f->speaker==0 && f->managerReady==1 && f->permitted==1 && f->clientsWant==0;
}
static inline enum TGClientAction TGClientBegin(struct TGClientModel *m,const struct TGClientFacts *f,uint64_t now) {
 if(m->attempted || m->done)return TG_NONE;
 m->attempted=1;
 if(!TGClientEligible(f) || now>UINT64_MAX-TG_CLIENT_WINDOW_SECONDS){m->done=1;return TG_NONE;}
 m->deadline=now+TG_CLIENT_WINDOW_SECONDS;m->wouldHold=1;return TG_WOULD_ADD_OWN_CLIENT;
}
/* ANY observed route or call event terminates the lease first, even if it seems
   harmless. Do not retain/reacquire while waiting for async state snapshots. */
static inline enum TGClientAction TGClientInvalidate(struct TGClientModel *m) {
 m->done=1;
 if(!m->wouldHold)return TG_NONE;
 m->wouldHold=0;return TG_WOULD_REMOVE_OWN_CLIENT;
}
static inline enum TGClientAction TGClientDeadline(struct TGClientModel *m,uint64_t now) {
 if(m->wouldHold && now>=m->deadline)return TGClientInvalidate(m);
 return TG_NONE;
}
#endif
