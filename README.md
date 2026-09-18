# glsu-kit

Userspace tooling for temporary-rooted Android phones — born on a Sony Xperia 1 V
(XQ-DQ72, Android 15, locked bootloader) rooted per-boot with
[GhostLock](https://github.com/YuKongA/ghostlock-app), to give
**Network Signal Guru (NSG)** full Qualcomm DIAG logging that normally only works
on Samsung devices.

```
 NSG engine ── open()/read() ── /dev/umts_router ──▶ /dev/pts/N (PTY)
                     (Samsung node emulation)            │
                                                    diagtty daemon
                                                    · HDLC (0x7e) framing
                                                    · strips {ver,len} header
                                                          │
                                                  AF_QIPCRTR socket
                                                  node 0 · port 28
                                                          │
                                                  DIAG service 4097
                                                     SM8550 modem
```

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
glsu daemon, mounts the su overlay (`/system/bin/su`, `/system/xbin/su` plus
`/data/local/*/su`), starts diagtty, creates `/dev/diag`, `/dev/umts_dm0`,
`/dev/umts_router` symlinks to the PTY, runs a DIAG VERNO round-trip self-test,
checks kernel SELinux health (aborts if any process is `unlabeled`), and
relaunches NSG.

**Unlock the phone once after a reboot before running it** — apps cannot launch
while the device is `RUNNING_LOCKED`.

Verify the bridge from the host:

```console
$ adb shell 'su -c "ls -l /proc/$(pidof bridge)/fd"' | grep pts
```

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
- **Banking apps / Play Integrity will detect a live session** (permissive
  SELinux, `su` in standard paths, overlay on `/system`). After a reboot none of
  that remains and the boot chain was never touched — verdicts behave as stock.
- **Root processes run in a kernel SELinux domain** and servicemanager refuses
  them binder lookups: `su -c 'pm install …'` fails with
  `Can't find service: package`. Use plain `adb shell pm install -r file.apk`.
- When rebooting from a live session by hand, use `su -c 'sync; reboot'` — a raw
  reboot skips init's orderly shutdown and can drop buffered logs.
- This kit is for your own device and passive diagnostics. The exploit itself
  belongs to the GhostLock authors; this repo ships only userspace.

## Documentation

- [docs/NSG-diag.md](docs/NSG-diag.md) — QRTR service discovery, DIAG framing,
  Samsung node emulation, DCI port.
- [docs/cold-boot-crash-analysis.md](docs/cold-boot-crash-analysis.md) — why a
  policy reload in post-exploit tooling crash-loops freshly-booted devices, and
  the fix.
- [docs/device-notes-XQ-DQ72.md](docs/device-notes-XQ-DQ72.md) — validation log
  and quirks for the reference device.

## License

Apache-2.0. NSG is a product of qtrun and is not affiliated with this project.
