# x11-kde-rcf
This repository (X11 KDE Race Condition Fix) serves to resolve a long-standing KDE issue for X11 users regarding a race condition between sleep entry and lockscreen launching, causing the desktop to show for a moment before the lockscreen displays. This is a potential security risk as it can leak desktop contents before authentication, when the user would expect a lockscreen as their device wakes up from sleep.

## Installation Instructions
When the package is ready for release, installation instructions will be here.

## Who is this for?
The description of this repository makes it seem like this issue is restricted to X11 users on KDE Plasma, but that is simply where I have seen this issue the most. Refer to root causing for my reasoning, but I believe that the failure actually lies in a missing lock contract between `logind` and the desktop environment. X11 makes it extremely obvious and I use KDE, but I'd be interested to see if others can benefit from this solution.

Strictly speaking, if on wake up from sleep your laptop/PC is showing you your screen before you put the laptop to sleep for a moment or a flash, and then showing the lock screen, this package is for you. I haven't tested with wayland, but there are similar bug reports for wayland that this package _should_ work for. Thats because this code is desktop environment agnostic (in terms of implementation) as it only touches `systemd-logind`.

## Why this package, and not an upstream fix?
I provide more information in the Past PRs section below, but it's my opinion that this bug/fix does not belong _specifically_ to KDE, X11, or systemd. The failure lies in a missing lock contract between the desktop environment and the init system, which normally would not be an issue unless there are external factors - _hint: there are always external factors!_ - such as nvme speed, GPU start up behaviour, etc. This is not an issue with all users due to the many possible factors that differing hardware introduces. The implemented fix, in my opinion, _should_ be in user space as this is not an issue _all users_ will encounter - only a subset.

### Why logind? Why a daemon?
The fix to be implemented only touches logind as its the only part of the system that has oversight over the entire process. It is a lightweight systemd daemon written in C for high portability between distributions and low binary size - remember, we are trying to reduce the bloat where we can! I use a daemon as a script in the system-sleep directory runs too late - another race condition. So, a daemon listening for the earliest sign of session sleep/suspension is the move.

### Implementation specifics
The daemon waits for a DBus message for `PrepareForSleep` via the `org.freedesktop.login1.Manager` interface, intercepts it, and runs `loginctl lock-session` via a system call before a short sleep to ensure the lock is applied before returning and allowing the sleep process to continue.

# Root Cause
Earlier in this document it was stated that a race condition is the culprit for this issue - I based this assessment on multiple sources. First, I looked at my own journalctl logs and repeatedly reproduced the issue to analyze the problem.

## Logs
```bash
nicholascabrera@localhost:~>
```

## Bugzilla Reports
There is a bugzilla report on KDE for this [here](https://bugs.kde.org/show_bug.cgi?id=485085). 

## Past PRs
Several PRs have been made for `KScreenLocker` that would have fixed the issue - but it's my opinion and also I think the opinion of the KScreenLocker team that KScreenLocker is not where this code should be. This race condition is not the responsibility of KDE, X11, OR systemd. All packages are acting exactly as they should be - with a collaboration failure between them all (notoriously difficult to collaborate between such large organizations). They can be found here:
- [PR 290](https://invent.kde.org/plasma/kscreenlocker/-/merge_requests/290)
- [PR 289](https://invent.kde.org/plasma/kscreenlocker/-/merge_requests/289)

# Future Enhancements
I plan to make some future enhancements to the script, such as using a native DBus call to logind rather than a system call, locking specific sessions, handling multiple signals, user configuration for delay tuning, and (non)verbose logging.
