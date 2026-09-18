# Cold-boot crash: policy reload corrupts SELinux state

A post-mortem of a framework crash-loop caused by inherited post-exploit
tooling, and why the kit's restore script refuses to reload SELinux policy.

## Symptom

First exploit run on a **freshly booted** device: apps die at launch, the
framework restarts in a loop, and dozens of processes carry the SELinux context
`unlabeled`. Warm re-runs of the identical stack had always been fine — which is
exactly why this class of bug ships in otherwise-working tooling.

## Evidence

- `ps -AZ | grep -c unlabeled` → **171** processes
- Zygote tombstone: `selinux_android_setcontext` failed/aborted while forking
  `com.android.phone`
- system_server tombstone: `RuntimeException` failing to set the system property
  `cache_key.display_info`
- Exploit-side log (`.ghostlock_ksu.log`): `policyload before=0 after=2`,
  immediately followed by `ksud missing; cannot late-load`

## Root cause

The root script (inherited from GhostLock-era tooling) prepares a KernelSU
late-load that never happens on this device:

1. dump `/sys/fs/selinux/policy`,
2. OR permission bits 30/31 into byte 23,
3. `load_policy` the modified blob back.

Reloading a policy forces the kernel to rebuild SID/context mappings. Doing that
**mid-boot, while zygote is forking hundreds of processes**, corrupts SID
conversion: children get contexts that cannot resolve, `setcontext` aborts
inside zygote, and system_server dies on a property write. On a warm system the
fork storm has long passed, so the same reload is harmless — an
intermittent-looking failure with a deterministic trigger.

The purpose of the reload was to enable `ksud` late-loading — and `ksud` was not
even installed; the script discovered that only *after* damaging policy state.

## Fix

This kit's deployment:

- repairs `/sys/fs/selinux/checkreqprot` (which the exploit's memory writes can
  disturb) — that part is safe and needed,
- **exits before the policy section entirely**; SELinux simply stays permissive
  until reboot,
- verifies kernel health after deployment: the restore script counts `unlabeled`
  processes and aborts loudly if any exist.

The general upstream fix we proposed to GhostLock: resolve `ksud` **first**, and
only reload policy when a usable KernelSU daemon actually exists.

## Lesson

A one-shot exploit's post-exploit script must be tested from the coldest boot
state it claims to support. Warm-run success proves nothing about cold-boot
safety — the difference is not the code, it's what the rest of the system is
doing concurrently.
