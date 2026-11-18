# CallRevive 1.0
Developed by Mohsen Mahmoud  
© 2026 Mohsen Mahmoud

I developed CallRevive while looking for a workaround for an issue on my iPhone where the screen would become unresponsive to touch during calls. It grew out of testing a way to recover touch response and restore proximity behavior after the system interface restarted.

An experimental tweak for jailbroken iPhones that restarts system services after a call connects, then attempts to restore proximity response for the same call on the internal earpiece.

The proximity request is scheduled for removal after five minutes, or earlier when a call, audio route, permission or settings change is reported. It does not restart after switching to speaker and back. System stalls or removal failures can delay cleanup.

Tested on an iPhone SE (2020) running iOS 16.4 with Relaxin/RootHide. A 1-minute 18-second call worked, and request removal at call end was verified. Other devices, longer calls and repeated reliability have not been verified. This is not a guaranteed or permanent touch-freeze fix. Unexpected screen blanking, crashes or call interruption are possible. Test on a non-critical call and keep a recovery method available.

Version 1.0 adds the new name, matching icons, developer credit and GitHub button. Existing settings and the tested client2 core remain unchanged; diagnostic logs still identify the core as client2.
