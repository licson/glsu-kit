# Device notes — Sony Xperia 1 V (XQ-DQ72)

The reference device for this kit.

## Firmware / kernel

| Item | Value |
| --- | --- |
| Model | XQ-DQ72 (Xperia 1 V) |
| OS | Android 15, build 67.2.A.3.178 |
| Kernel (`uname -r`) | `5.15.189-android13-8-00016-g51bba4309aac-ab14546557` |
| SoC | Snapdragon 8 Gen 2 (SM8550) |
| DIAG (QRTR) | service 4097, node 0, cmd port 28, DCI port 31 |
| NSG | 4.8.8 (`com.qtrun.QuickTest`, uid 10474) |

`reference/offsets.json` is the validated GhostLock profile for this exact
kernel (origin: NickJi2019's fork). All 15 `task_struct` offsets were
cross-checked against the GKI BTF for `ab14546557` and the extractor's
disassembly preflight; the exploit succeeded from a fresh enforcing boot on the
first cold attempt after the fixes below.

## Verified boot after sessions

Measured while the stack was live, i.e. after exploit + overlay + diag bridge:

```console
$ adb shell getprop ro.boot.verifiedbootstate   # green
$ adb shell getprop ro.boot.flash.locked        # 1
$ adb shell getprop ro.boot.vbmeta.device_state # locked
```

The exploit never flashes anything, so these hold after reboots too.

## Quirks encountered

- **`RUNNING_LOCKED`**: after a reboot, apps cannot launch until the phone has
  been unlocked once. The restore script's NSG relaunch fails silently without
  this; unlock first.
- **glsu daemon races**: never start a second daemon. Kill all instances, wait
  until `pidof glsu` is empty, then start exactly one. The restore script
  embeds this pattern; NSG shows "root access found, but access denied" if you
  get it wrong.
- **Binder refusal from kernel-domain root**: `su -c 'pm install …'` fails with
  `cmd: Can't find service: package` — servicemanager refuses binder lookups
  from the exploit's root context even under permissive SELinux. Install with
  plain `adb shell pm install -r` instead.
- **Dirty reboots lose evidence**: `su -c reboot` skips init's shutdown; the
  last <60s of buffered logs can vanish. Use `su -c 'sync; reboot'` when
  capturing evidence.
- **Play Protect vs streamed installs**: `adb install` of unsigned/modded APKs
  gets aborted by Play Protect on-device; `adb shell pm install -r` (shell uid,
  local file) does not.
