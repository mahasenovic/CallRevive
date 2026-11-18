#include "TGClientModel.h"
#include <assert.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
static const size_t offsets[]={
 offsetof(struct TGClientFacts,freshOwnReset),offsetof(struct TGClientFacts,newHost),
 offsetof(struct TGClientFacts,callDelegateReady),offsetof(struct TGClientFacts,routeDelegateReady),
 offsetof(struct TGClientFacts,connectedCount),offsetof(struct TGClientFacts,heldCount),
 offsetof(struct TGClientFacts,callUnknown),offsetof(struct TGClientFacts,routeKnown),
 offsetof(struct TGClientFacts,receiver),offsetof(struct TGClientFacts,speaker),
 offsetof(struct TGClientFacts,managerReady),offsetof(struct TGClientFacts,permitted),
 offsetof(struct TGClientFacts,clientsWant)};
int main(void) {
 const struct TGClientFacts yes={1,1,1,1,1,0,0,1,1,0,1,1,0};
 for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
  for(int k=-1;k<=2;k++) {
   struct TGClientFacts f=yes;int old=0;
   memcpy(&old,(const char *)&f+offsets[i],sizeof(old));
   if(k==old)continue;
   memcpy((char *)&f+offsets[i],&k,sizeof(k));
   struct TGClientModel m={0};
   assert(TGClientBegin(&m,&f,100)==TG_NONE);
   assert(m.done&&!m.wouldHold);
   assert(TGClientBegin(&m,&yes,101)==TG_NONE);
  }
 }
 for(int eventTime=0;eventTime<=410;eventTime++) {
  struct TGClientModel m={0};
  if(eventTime<100) {
   assert(TGClientInvalidate(&m)==TG_NONE);
   assert(TGClientBegin(&m,&yes,100)==TG_NONE);
  } else {
   assert(TGClientBegin(&m,&yes,100)==TG_WOULD_ADD_OWN_CLIENT);
   assert(m.deadline==100+TG_CLIENT_WINDOW_SECONDS);
   int removals=0;
   for(int t=100;t<=410;t++) {
    if(t==eventTime)removals+=TGClientInvalidate(&m)==TG_WOULD_REMOVE_OWN_CLIENT;
    removals+=TGClientDeadline(&m,(uint64_t)t)==TG_WOULD_REMOVE_OWN_CLIENT;
    assert(TGClientBegin(&m,&yes,(uint64_t)t)==TG_NONE);
   }
   assert(removals==1 && !m.wouldHold);
   assert(TGClientInvalidate(&m)==TG_NONE);
  }
 }
 for(uint64_t i=0;i<TG_CLIENT_WINDOW_SECONDS;i++) {
  struct TGClientModel m={0};assert(TGClientBegin(&m,&yes,UINT64_MAX-i)==TG_NONE);
 }
 assert(TG_CLIENT_ACTUATOR_ENABLED==0);
 puts("PASS shared model:13 gates x unknown/bad values;411 event times incl before acquisition, same-tick expiry/event; at most one add/remove; no reacquire; overflow; defaultOFF");
}
