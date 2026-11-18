// MohsenTouchGuard 0.2.3 — FULL version, arm64-ONLY
// Target: iPhone SE 2020 (A13, arm64e), iOS 16.4
// Jailbreaks: Relaxin + Roothide, rootless (/var/jb)
// Author: Mr Mohsen / مستر محسن
//
// v0.2.3 = v0.2.0 feature set, rebuilt arm64-ONLY (no arm64e
// slice — the arm64e slice built with the clang 11 toolchain
// crashed SpringBoard on load, EXC_BREAKPOINT in libobjc
// readClass; the arm64-only log-only v0.2.2 was TESTED on the
// real device and passed cleanly, no hang, no new crash).
//
// Audit fixes (docs/V0.2-INCIDENT-AUDIT.md), applied here:
//  1. CTCallCenter setup is NO LONGER done in %ctor. %ctor only
//     logs and schedules the setup 5 s later on the main queue
//     (after SpringBoard has finished launching). No synchronous
//     telephony XPC during dyld load.
//  2. The currentCalls "already connected on load" branch is
//     REMOVED — it was the only load-time reset trigger. The
//     /tmp marker already prevents a second reset for the
//     same call; after a reset reloads SpringBoard, the call
//     is simply not re-detected until its state changes.
//
// Problem: touch stops working (screen lit, buttons work, happens
// even without jailbreak, worse during calls, screen was replaced
// before — likely a bad aftermarket digitizer / loose flex, and
// heat during calls makes it worse). RemoteCompanion's respring
// fixes it temporarily; a normal SpringBoard-only respring does not.
//
// RemoteCompanion's respring — VERIFIED from its public source
// (saihgupr/RemoteCompanion, Tweak.x, "respring" command): a
// 4-step fallback chain, NOT a userspace reboot / ldrestart
// (those are separate actions in the same tweak):
//   1. posix_spawn sbreload
//   2. SpringBoard _relaunchSpringBoardNow
//   3. FBSystemService exitAndRelaunch:YES
//   4. exit(0)
// Its v2.0.0-beta2 changelog also says the respring was improved
// by using killall backboardd. CORRECTION: v0.1.0's comments
// wrongly called RemoteCompanion's respring a userspace reboot.
// That was an unverified guess and is corrected here.
//
// What this v0.2.0 does:
//  - Injects into SpringBoard only, so it is background by nature.
//  - Watches for calls with CoreTelephony CTCallCenter (looked up
//    at runtime only — no direct link to a deprecated class).
//  - On call start (all options are switches, no sliders):
//      * dimOnCall (default OFF): if ON, saves brightness and dims
//        to a fixed 0.45. If OFF, brightness is never touched.
//      * lowPowerOnCall (default YES): tries Low Power Mode via
//        runtime lookup only.
//      * thermalMonitor (default YES): 30-second thermal monitor,
//        log-only; it dims to 0.30 on serious/critical heat ONLY
//        if dimOnCall is ON, otherwise it only logs.
//  - Pre-emptive touch reset: 3 seconds after a call connects,
//    if the resetOnCallStart pref is ON (default YES), it runs
//    the RemoteCompanion-style reset chain: (a) sbreload via
//    posix_spawn if the binary exists; (b) if that fails, kill
//    backboardd (SIGTERM on its pid, found via sysctl — the
//    equivalent of `killall backboardd`); (c) if that fails,
//    SpringBoard _relaunchSpringBoardNow; (d) if unavailable,
//    FBSystemService exitAndRelaunch:YES. NO exit(0) step and
//    NO ldrestart / userspace reboot anywhere. Each step has its
//    own switch (useSbreload, useBackboarddKill,
//    useRelaunchSpringBoard, useFBSystemService — all default
//    YES) so the user can isolate which step alone fixes the
//    touch; a step whose switch is OFF is skipped and logged.
//    CORRECTION (user-verified 2026-11-17): a respring during a
//    call does NOT drop the call — the call stays up normally.
//    A marker file in /tmp prevents a reset loop: the reset
//    reloads SpringBoard, so on reload the guard sees the call
//    is already connected and must NOT reset again for that call;
//    the marker is cleared when the call ends.
//  - On call end: restores brightness (only if dimmed) and the
//    saved Low Power Mode state, stops the timer.
//
// What it does NOT do:
//  - NEVER does an ldrestart / userspace reboot — not used at all.
//  - It cannot fix a loose / bad digitizer. The permanent fix is
//    reseating / replacing the screen. This only reduces heat and
//    resets the touch controller at call start, so the freeze
//    happens less often.

#import <UIKit/UIKit.h>
#import <Foundation/Foundation.h>
#import <signal.h>
#import <string.h>
#import <spawn.h>
#import <sys/sysctl.h>
#import <objc/runtime.h>
#import <objc/message.h>

#include "TGTrace.h"
#define TG_PROBE_TOKEN_PATH "/var/mobile/Library/Logs/MohsenTouchGuard-named.pending"
#include "TGPostlaunchProbe.h"

extern char **environ;

static NSString * const kPrefsPath = @"/var/mobile/Library/Preferences/com.mohsen.touchguard.plist";
static NSString * const kPrefsChangedNotification = @"com.mohsen.touchguard.settings-changed";
static NSString * const kResetMarkerPath = @"/tmp/MohsenTouchGuardResetDone";

static const CGFloat kCallDimBrightness = 0.45;   // fixed, dimOnCall ON
static const CGFloat kThermalDimBrightness = 0.30; // fixed, thermal high + dimOnCall ON

static BOOL prefBool(NSString *key, BOOL def) {
    NSDictionary *prefs = [NSDictionary dictionaryWithContentsOfFile:kPrefsPath];
    NSNumber *v = prefs[key];
    return v ? [v boolValue] : def;
}

// Local adapter is OFF unless explicitly rebuilt with approval.
#define TG_CLIENT_ACTUATOR_ENABLED 1
#include "TGNamedClient.h"

// ---------- Guard ----------
@interface MohsenTouchGuard : NSObject
+ (instancetype)shared;
- (void)start;
- (void)onCallConnected;
- (void)onCallDisconnected;
- (void)thermalTick:(NSTimer *)timer;
- (void)schedulePreemptiveReset;
- (void)doPreemptiveReset;
@end

@implementation MohsenTouchGuard {
    BOOL _inCall;
    CGFloat _savedBrightness;
    BOOL _savedBrightnessValid;
    BOOL _savedLowPowerMode;
    BOOL _savedLowPowerModeValid;
    NSTimer *_thermalTimer;
    id _callCenter;
    BOOL _callDetectionStarted;
}

+ (instancetype)shared {
    static MohsenTouchGuard *s;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ s = [MohsenTouchGuard new]; });
    return s;
}

// Low Power Mode, runtime-only. _CDBatterySaver is private, so never
// link it directly: look it up by name, and skip silently if it is
// not there or does not respond. Returns YES if it was applied.
- (BOOL)trySetLowPowerMode:(BOOL)enabled saveState:(BOOL)save {
    Class cls = NSClassFromString(@"_CDBatterySaver");
    if (!cls) {
        NSLog(@"[MohsenTouchGuard] low power mode skipped: _CDBatterySaver not found");
        return NO;
    }
    id saver = nil;
    if ([cls respondsToSelector:@selector(sharedInstance)]) {
        saver = ((id (*)(id, SEL))objc_msgSend)(cls, @selector(sharedInstance));
    }
    if (!saver && [cls respondsToSelector:@selector(sharedBatterySaver)]) {
        saver = ((id (*)(id, SEL))objc_msgSend)(cls, @selector(sharedBatterySaver));
    }
    if (!saver) {
        saver = [[cls alloc] init];
    }
    if (!saver) {
        NSLog(@"[MohsenTouchGuard] low power mode skipped: no _CDBatterySaver instance");
        return NO;
    }
    SEL setter = NSSelectorFromString(@"setLowPowerModeEnabled:");
    if (![saver respondsToSelector:setter]) {
        NSLog(@"[MohsenTouchGuard] low power mode skipped: setLowPowerModeEnabled: not available");
        return NO;
    }
    if (save) {
        SEL getter = NSSelectorFromString(@"lowPowerModeEnabled");
        if ([saver respondsToSelector:getter]) {
            _savedLowPowerMode = ((BOOL (*)(id, SEL))objc_msgSend)(saver, getter);
            _savedLowPowerModeValid = YES;
        }
    }
    ((void (*)(id, SEL, BOOL))objc_msgSend)(saver, setter, enabled);
    NSLog(@"[MohsenTouchGuard] low power mode set to %@", enabled ? @"ON" : @"OFF");
    return YES;
}

- (void)start {
    // Audit fix: this is called from a deferred dispatch_after
    // in %ctor, NOT during dyld load. Guarded so it runs once.
    if (_callDetectionStarted) return;
    _callDetectionStarted = YES;
    // Call detection via CTCallCenter, runtime-only (the class is
    // deprecated, so no direct reference — that also avoids a
    // -Wdeprecated-declarations build error under -Werror).
    // NOTE: CoreTelephony is NOT linked in the Makefile; the
    // class is looked up at runtime only (audit discrepancy
    // in v0.2.0 fixed — it used to be directly linked despite
    // this comment saying runtime-only).
    Class ctClass = NSClassFromString(@"CTCallCenter");
    if (!ctClass) {
        NSLog(@"[MohsenTouchGuard] call detection unavailable: CTCallCenter not found");
        return;
    }
    _callCenter = [[ctClass alloc] init];
    if (!_callCenter) {
        NSLog(@"[MohsenTouchGuard] call detection unavailable: CTCallCenter init failed");
        return;
    }
    __weak typeof(self) weakSelf = self;
    void (^handler)(id) = ^(id call) {
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf) return;
        NSString *state = nil;
        @try {
            state = [call valueForKey:@"callState"];
        } @catch (__unused NSException *e) {
            state = nil;
        }
        if (![state isKindOfClass:[NSString class]]) return;
        NSLog(@"[MohsenTouchGuard] call state: %@", state);
        if ([state isEqualToString:@"CTCallStateConnected"]) {
            [strongSelf onCallConnected];
        } else if ([state isEqualToString:@"CTCallStateDisconnected"]) {
            [strongSelf onCallDisconnected];
        }
    };
    SEL setter = NSSelectorFromString(@"setCallEventHandler:");
    if ([_callCenter respondsToSelector:setter]) {
        ((void (*)(id, SEL, id))objc_msgSend)(_callCenter, setter, [handler copy]);
        NSLog(@"[MohsenTouchGuard] call detection started");
    } else {
        NSLog(@"[MohsenTouchGuard] call detection unavailable: setCallEventHandler: not available");
        _callCenter = nil;
        return;
    }
    // Audit fix: the old currentCalls "already connected on
    // load" branch was REMOVED here. It was the only code path
    // that could fire a reset without a real call event (a
    // stale/phantom connected call at load would have run the
    // reset chain during the install respring's own backboardd
    // restart). The /tmp reset marker already prevents a
    // second reset for the same call.
}

// Pre-emptive touch-controller reset, 3 s after the call connects
// so the call is established. Runs the RemoteCompanion-style chain
// (see header comment): sbreload -> kill backboardd ->
// _relaunchSpringBoardNow -> FBSystemService exitAndRelaunch:YES.
// NO exit(0) step, NO ldrestart / userspace reboot. The marker
// file stops the SpringBoard reload from resetting again for the
// same call.
- (void)schedulePreemptiveReset {
    if (!prefBool(@"resetOnCallStart", YES)) {
        NSLog(@"[MohsenTouchGuard] pre-emptive reset disabled in preferences");
        return;
    }
    if ([[NSFileManager defaultManager] fileExistsAtPath:kResetMarkerPath]) {
        NSLog(@"[MohsenTouchGuard] pre-emptive reset already done for this call");
        return;
    }
    __weak typeof(self) weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3.0 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf) return;
        [strongSelf doPreemptiveReset];
    });
}

// Step (a): try sbreload via posix_spawn, as RemoteCompanion does.
// Returns YES if sbreload was spawned (rc == 0), NO otherwise.
static BOOL MohsenTrySbreload(void) {
    NSArray<NSString *> *candidates = @[
        @"/var/jb/usr/bin/sbreload",
        @"/usr/bin/sbreload",
    ];
    for (NSString *path in candidates) {
        if (![[NSFileManager defaultManager] fileExistsAtPath:path]) continue;
        pid_t pid = 0;
        const char *args[] = { [path UTF8String], NULL };
        int res = posix_spawn(&pid, [path UTF8String], NULL, NULL,
                              (char * const *)args, environ);
        if (res == 0) {
            NSLog(@"[MohsenTouchGuard] reset: sbreload spawned pid %d", (int)pid);
            return YES;
        }
        NSLog(@"[MohsenTouchGuard] reset: sbreload %@ spawn failed rc=%d", path, res);
    }
    NSLog(@"[MohsenTouchGuard] reset: sbreload unavailable, trying kill backboardd");
    return NO;
}

// Step (b): find backboardd's pid via sysctl and SIGTERM it — the
// classic respring trigger (launchd respawns backboardd, SpringBoard
// reloads). No system() / shell: system() is unavailable in the
// iOS SDK, and a direct kill() is the precise equivalent of
// `killall backboardd`. Returns the kill() return code, or -1 if
// the pid was not found.
static int MohsenKillBackboardd(void) {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
    size_t len = 0;
    if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0 || len == 0) return -1;
    struct kinfo_proc *procs = malloc(len);
    if (!procs) return -1;
    int rc = -1;
    if (sysctl(mib, 4, procs, &len, NULL, 0) == 0) {
        size_t count = len / sizeof(struct kinfo_proc);
        for (size_t i = 0; i < count; i++) {
            if (strcmp(procs[i].kp_proc.p_comm, "backboardd") == 0) {
                pid_t pid = procs[i].kp_proc.p_pid;
                TGTrace("kill_before target_bb=%d", (int)pid);
                rc = kill(pid, SIGTERM);
                int killError = (rc == 0) ? 0 : errno;
                TGTrace("kill_return target_bb=%d rc=%d errno=%d", (int)pid, rc, killError);
                errno = killError;
                NSLog(@"[MohsenTouchGuard] reset: kill backboardd pid %d -> %d", (int)pid, rc);
                break;
            }
        }
    }
    free(procs);
    return rc;
}

// Step (c): SpringBoard _relaunchSpringBoardNow, runtime lookup only.
static BOOL MohsenTryRelaunchSpringBoardNow(void) {
    Class sbClass = NSClassFromString(@"SpringBoard");
    if (!sbClass || ![sbClass respondsToSelector:@selector(sharedApplication)]) return NO;
    id sb = ((id (*)(id, SEL))objc_msgSend)(sbClass, @selector(sharedApplication));
    if (!sb) return NO;
    SEL relaunchSel = NSSelectorFromString(@"_relaunchSpringBoardNow");
    if (![sb respondsToSelector:relaunchSel]) return NO;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
    [sb performSelector:relaunchSel];
#pragma clang diagnostic pop
    NSLog(@"[MohsenTouchGuard] reset: relaunchSpringBoardNow");
    return YES;
}

// Step (d): FBSystemService exitAndRelaunch:YES, runtime lookup only.
static BOOL MohsenTryFBSystemService(void) {
    Class fbClass = NSClassFromString(@"FBSystemService");
    if (!fbClass || ![fbClass respondsToSelector:@selector(sharedInstance)]) return NO;
    id fb = ((id (*)(id, SEL))objc_msgSend)(fbClass, @selector(sharedInstance));
    if (!fb) return NO;
    SEL sel = NSSelectorFromString(@"exitAndRelaunch:");
    if (![fb respondsToSelector:sel]) return NO;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
    [fb performSelector:sel withObject:@YES];
#pragma clang diagnostic pop
    NSLog(@"[MohsenTouchGuard] reset: FBSystemService exitAndRelaunch");
    return YES;
}

// Experimental candidate removes all inherited raw proximity forcing.
- (void)doPreemptiveReset {
    TGTrace("reset_enter cached_inCall=%d", _inCall);
    if (!_inCall) return;
    if (TGNamedResetSuppressed()) { TGTrace("reset_suppressed recovered_call=1"); return; }
    if (!prefBool(@"resetOnCallStart", YES)) return;
    if ([[NSFileManager defaultManager] fileExistsAtPath:kResetMarkerPath]) return;
    atomic_fetch_add_explicit(&tgAttempt, 1, memory_order_relaxed);
    TGTrace("reset_attempt");
    TGPrepareNamedRecovery();
    // Create the marker BEFORE resetting, so the SpringBoard reload
    // caused by the reset does not trigger another reset.
    [[NSData data] writeToFile:kResetMarkerPath atomically:YES];
    NSLog(@"[MohsenTouchGuard] call started, pre-emptive touch reset (RemoteCompanion chain)");
    // Each step has its own switch (all default YES) so the user
    // can isolate exactly which step fixes the touch: turn OFF
    // all but one, trigger a reset, and see whether touch returns.
    // v0.2.17: backboardd kill is tried FIRST, before sbreload.
    // Named-client candidate has no raw proximity toggles. Reset order unchanged.
    if (prefBool(@"useBackboarddKill", YES)) {
        if (MohsenKillBackboardd() == 0) {
            return;
        }
        NSLog(@"[MohsenTouchGuard] reset: backboardd kill failed, trying sbreload");
    } else {
        NSLog(@"[MohsenTouchGuard] reset: backboardd kill skipped (switch off)");
    }
    if (prefBool(@"useSbreload", YES)) {
        TGTrace("sbreload_attempt");
        if (MohsenTrySbreload()) { TGTrace("sbreload_spawn_accepted"); return; }
    } else {
        NSLog(@"[MohsenTouchGuard] reset: sbreload skipped (switch off)");
    }
    if (prefBool(@"useRelaunchSpringBoard", YES)) {
        if (MohsenTryRelaunchSpringBoardNow()) return;
        NSLog(@"[MohsenTouchGuard] reset: relaunchSpringBoardNow unavailable, trying FBSystemService");
    } else {
        NSLog(@"[MohsenTouchGuard] reset: relaunchSpringBoardNow skipped (switch off)");
    }
    if (prefBool(@"useFBSystemService", YES)) {
        if (MohsenTryFBSystemService()) return;
    } else {
        NSLog(@"[MohsenTouchGuard] reset: FBSystemService skipped (switch off)");
    }
    TGCancelNamedRecovery(); // No reset accepted: revoke only our pending recovery token.
    NSLog(@"[MohsenTouchGuard] reset: all reset steps failed or skipped, no reset performed");
}

- (void)onCallConnected {
    TGTrace("call_connected_event");
    if (!prefBool(@"guardEnabled", YES)) {
        NSLog(@"[MohsenTouchGuard] call started, guard disabled in preferences");
        return;
    }
    if (_inCall) return;
    _inCall = YES;
    NSLog(@"[MohsenTouchGuard] call started");
    // Dimming is fully gated by the dimOnCall switch (default OFF):
    // with default prefs the brightness is never touched.
    if (prefBool(@"dimOnCall", NO)) {
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_savedBrightness = [UIScreen mainScreen].brightness;
            self->_savedBrightnessValid = YES;
            [UIScreen mainScreen].brightness = kCallDimBrightness;
            NSLog(@"[MohsenTouchGuard] brightness dimmed to %.2f for call", (double)kCallDimBrightness);
        });
    } else {
        NSLog(@"[MohsenTouchGuard] dimming off (dimOnCall switch OFF), brightness untouched");
    }
    // Low Power Mode only if its switch is ON (default YES).
    if (prefBool(@"lowPowerOnCall", YES)) {
        [self trySetLowPowerMode:YES saveState:YES];
    } else {
        NSLog(@"[MohsenTouchGuard] low power mode off (lowPowerOnCall switch OFF)");
    }
    [self schedulePreemptiveReset];
    // Thermal monitor only if its switch is ON (default YES).
    if (prefBool(@"thermalMonitor", YES)) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self->_thermalTimer invalidate];
            self->_thermalTimer = [NSTimer scheduledTimerWithTimeInterval:30.0
                                                                   target:self
                                                                 selector:@selector(thermalTick:)
                                                                 userInfo:nil
                                                                  repeats:YES];
        });
    } else {
        NSLog(@"[MohsenTouchGuard] thermal monitor off (thermalMonitor switch OFF)");
    }
}

- (void)thermalTick:(NSTimer *)timer {
    (void)timer;
    if (!_inCall) return;
    NSProcessInfo *pi = [NSProcessInfo processInfo];
    NSInteger state = -1;
    if ([pi respondsToSelector:@selector(thermalState)]) {
        state = (NSInteger)pi.thermalState;
    }
    NSLog(@"[MohsenTouchGuard] thermal state: %ld", (long)state);
    // NSProcessInfoThermalState: 0 nominal, 1 fair, 2 serious, 3 critical.
    // Log-only unless dimOnCall is ON: the monitor must never dim the
    // screen on its own.
    if (state >= 2) {
        if (!prefBool(@"dimOnCall", NO)) {
            NSLog(@"[MohsenTouchGuard] thermal high, dimming skipped (dimOnCall OFF)");
            return;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if ([UIScreen mainScreen].brightness > kThermalDimBrightness) {
                [UIScreen mainScreen].brightness = kThermalDimBrightness;
                NSLog(@"[MohsenTouchGuard] thermal high, dimming to 0.30");
            }
        });
    }
}

- (void)onCallDisconnected {
    TGTrace("call_disconnected");
    // Clear the per-call reset marker so the next call can reset again.
    [[NSFileManager defaultManager] removeItemAtPath:kResetMarkerPath error:nil];
    if (!_inCall) {
        NSLog(@"[MohsenTouchGuard] call ended");
        return;
    }
    _inCall = NO;
    dispatch_async(dispatch_get_main_queue(), ^{
        [self->_thermalTimer invalidate];
        self->_thermalTimer = nil;
        if (self->_savedBrightnessValid) {
            [UIScreen mainScreen].brightness = self->_savedBrightness;
            self->_savedBrightnessValid = NO;
            NSLog(@"[MohsenTouchGuard] call ended, brightness restored");
        }
    });
    if (_savedLowPowerModeValid) {
        [self trySetLowPowerMode:_savedLowPowerMode saveState:NO];
        _savedLowPowerModeValid = NO;
    }
    NSLog(@"[MohsenTouchGuard] call ended");
}

@end

// ---------- Lifecycle ----------
// Audit fix: %ctor does NO telephony work at all. It only logs,
// registers the settings-changed observer, and schedules the
// CTCallCenter setup 5 s later on the main queue — after
// SpringBoard has finished launching. No synchronous telephony
// XPC during dyld load, and no reset can ever fire at load time.
%ctor {
    TGTrace("launch version=0.2.18+client2 actuator_enabled=1");
    NSLog(@"[MohsenTouchGuard] loaded v0.2.3, arm64-only, rootless, iOS 16.4, SpringBoard only");
    [[NSNotificationCenter defaultCenter] addObserverForName:kPrefsChangedNotification
        object:nil queue:[NSOperationQueue mainQueue]
        usingBlock:^(__unused NSNotification *n) {
            NSLog(@"[MohsenTouchGuard] settings changed");
        }];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(5.0 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        TGRunPostlaunchNamedRecovery();
        [[MohsenTouchGuard shared] start];
    });
}
