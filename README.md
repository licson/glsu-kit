# glsu-kit

Userspace tooling for temporary-rooted Android phones — born on a Sony Xperia 1 V
(XQ-DQ72, Android 15, locked bootloader) rooted per-boot with
[GhostLock](https://github.com/YuKongA/ghostlock-app), to give
**Network Signal Guru (NSG)** full Qualcomm DIAG logging that normally only works
on Samsung devices.

![Temp-root DIAG bridge architecture: NSG engine opening the emulated Samsung node, backed by a PTY whose master side is the diagtty daemon relaying to the QRTR diag service on the modem](https://s3.licson.net/licson/blog/temp-root-nsg-diag-bridge/architecture.svg)

Everything the kit creates lives in RAM or `/data/local/tmp` and is gone after a
reboot: no patched boot image, no modified partitions, verified boot stays green.
See [docs/cold-boot-crash-analysis.md](docs/cold-boot-crash-analysis.md) for a
safety bug this kit fixes, and the security notes below before granting apps root.

## Components

| File | What it is |
| --- | --- |
| `src/glsu.c` | `su` shim speaking the record protocol NSG's libsu engine expects; abstract socket `@glsu`; per-app allowlist |
| `src/diagtty.c` | PTY ↔ QRTR DIAG bridge (`/data/local/tmp/diag0`), command port 28, optional DCI port 31 |
| `src/qrtr_probe.c` | Dumps the QRTR service table (`NEW_LOOKUP`) — find the diag service on your SoC |
| `scripts/nsg-root-restore.sh` | One-command restore after a reboot: exploit → glsu → su overlay → diagtty → symlinks → self-test → NSG relaunch |
| `scripts/gl-allow` | Host wrapper for the on-device allowlist manager |
| `scripts/gl-allow-device.sh` | On-device allowlist manager (`add` / `add-recent` / `list` / `remove`) |
| `reference/offsets.json` | Validated GhostLock offsets profile for the Xperia 1 V kernel (for cross-checking) |

## Requirements

- A device/kernel with an existing GhostLock profile. The kit was verified on
  **XQ-DQ72, build 67.2.A.3.178, kernel `5.15.189-android13-8-00016-g51bba4309aac-ab14546557`**
  (profile from [NickJi2019's fork](https://github.com/NickJi2019/ghostlock-app)).
  GhostLock matches `uname -r` **exactly**; check yours before anything:

  ```console
  $ adb shell uname -r
  ```

- Android NDK (clang for aarch64), `adb`, NSG 4.8.8 (`com.qtrun.QuickTest`).
- The `ghostlock` binary built from the fork (its own Makefile).

## Build

```console
$ ANDROID_NDK_HOME=/path/to/android-ndk make
$ ls bin/
glsu  diagtty  qrtr_probe
```

## Restore the stack after a reboot

```console
$ cp /path/to/ghostlock bin/ghostlock        # or: export GHOSTLOCK=/path/to/ghostlock
$ scripts/nsg-root-restore.sh
```

The script is idempotent and, in order: preflights (kernel string, battery,
package manager), runs the exploit only if root is absent, repairs/restarts the
glsu daemon, verifies the session is **hidden** (no `su` in any standard path,
no overlay mounts), starts diagtty, creates `/dev/diag`, `/dev/umts_dm0`,
`/dev/umts_router` symlinks to the PTY, runs a DIAG VERNO round-trip self-test,
checks kernel SELinux health (aborts if any process is `unlabeled`), and
relaunches NSG.

**Unlock the phone once after a reboot before running it** — apps cannot launch
while the device is `RUNNING_LOCKED`.

Verify the bridge from the host:

```console
$ adb shell '/data/local/tmp/gl/glsu -c "ls -l /proc/$(pidof bridge)/fd"' | grep pts
```

## Hiding root from detector apps

Sessions are **hidden by default**: no `/system/bin/su` or `/system/xbin/su`
(no `/system` overlay is mounted at all), no `/data/local/{bin,xbin,su/bin}/su`,
and nothing suspicious in `/proc/mounts`. Root is reached through the glsu
client directly — `adb shell '/data/local/tmp/gl/glsu -c "<cmd>"` — which no
detector knows to look for, and an allowlisted app keeps using the abstract
socket as before.

Verified on-device (live session): ZA Bank, CMHK MyLink and HA Go (RootBeer)
all pass; HA Go's "Jailbreak detected" dialog is gone. ZA Bank and Hang Seng
additionally refuse while **USB debugging** is on — that is a stock Android
signal, not something this kit creates.

**NSG keeps its root**: the `nsg-su-scope.sh` watcher (started by the exploit's
root script) mounts the su overlay *inside NSG's own mount namespace* within a
couple of seconds of the app launching — NSG's PATH search finds `su`, the
allowlisted glsu client connects as before, while every other app (and every
other `/proc/mounts`) still sees a completely stock `/system`.

Known residuals while a session is live (accepted): SELinux is permissive and
the daemon processes run as uid 0 — TMX-class detectors (Hang Seng) can still
flag the device. Use such apps unrooted; after a reboot (or the one-command
unroot script) nothing remains and all detectors behave as on a stock phone.

- Unroot without rebooting:
  `adb shell '/data/local/tmp/gl/glsu -c "sh /data/local/tmp/.ghostlock_unroot.sh"'`
  (kills the services, lazily unmounts leftovers, removes every on-disk
  artifact, restores enforcing SELinux — NSG's root bridge dies with it).
- Opt back into a PATH-visible `su` (detector-visible, e.g. for interactive
  tinkering): run the exploit with `GHOSTLOCK_MOUNT_SU=1`, or
  `MOUNT_SU=1 scripts/nsg-root-restore.sh`.

## Granting root to other apps

The glsu daemon admits uid 0, adb's uid 2000, and every uid listed in
`/data/local/tmp/gl/glsu-uids` (format: `<uid> <package>`, `#` comments; the
file is re-read on every connection — changes are instant):

```console
$ scripts/gl-allow add com.qtrun.QuickTest   # by package name
$ scripts/gl-allow add-recent 1              # newest installed app (random package names)
$ scripts/gl-allow list
$ scripts/gl-allow remove <package>
```

The allowlist survives reboots (it lives in `/data`); root itself does not.

## Safety and security notes

- **Allowlisted apps get full, silent uid 0** while the session is live, amplified
  by permissive SELinux. Treat `glsu-uids` like sudoers; revoke with
  `gl-allow remove`. Reboot revokes everything.
- **Detector apps**: file/path detectors (RootBeer, Chinese packers) pass during
  a hidden session; SELinux-permissive and process scans (TMX) can still flag it.
  After a reboot none of the session remains and the boot chain was never
  touched — verdicts behave as stock.
- **Root processes run in a kernel SELinux domain** and servicemanager refuses
  them binder lookups: `su -c 'pm install …'` fails with
  `Can't find service: package`. Use plain `adb shell pm install -r file.apk`.
- When rebooting from a live session by hand, use `su -c 'sync; reboot'` — a raw
  reboot skips init's orderly shutdown and can drop buffered logs.
- This kit is for your own device and passive diagnostics. The exploit itself
  belongs to the GhostLock authors; this repo ships only userspace.

## Documentation

![Causal chain of the cold-boot SELinux corruption: a policy reload races the boot-time fork storm, processes spawn unlabeled, zygote aborts, the framework crash-loops — fixed by skipping the reload when ksud is absent](https://s3.licson.net/licson/blog/temp-root-nsg-diag-bridge/cold-boot-chain.svg)

- [docs/NSG-diag.md](docs/NSG-diag.md) — QRTR service discovery, DIAG framing,
  Samsung node emulation, DCI port.
- [docs/cold-boot-crash-analysis.md](docs/cold-boot-crash-analysis.md) — why a
  policy reload in post-exploit tooling crash-loops freshly-booted devices, and
  the fix.
- [docs/device-notes-XQ-DQ72.md](docs/device-notes-XQ-DQ72.md) — validation log
  and quirks for the reference device.

## License

Apache-2.0. NSG is a product of qtrun and is not affiliated with this project.
