#!/bin/bash
# Xperia 1 V (XQ-DQ72, 67.2.A.3.178) — temp root + NSG diag restore
# Usage: ./nsg-root-restore.sh
# Env:   ADB=/path/to/adb  BIN=/dir/with/built+ghostlock-binaries  GHOSTLOCK=/path/to/ghostlock
#        MOUNT_SU=1 to also expose su on /system (GHOSTLOCK_MOUNT_SU=1) — detector-visible
set -u

ADB="${ADB:-$(command -v adb || echo "$HOME/Downloads/platform-tools/adb")}"
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${BIN:-$(cd "$HERE/.." && pwd)/bin}"
GHOSTLOCK_BIN="${GHOSTLOCK:-$BIN/ghostlock}"
KERNEL_EXPECTED="5.15.189-android13-8-00016-g51bba4309aac-ab14546557"
NSG_PKG="com.qtrun.QuickTest"
GL="/data/local/tmp/gl"

G=$'\033[32m'; R=$'\033[31m'; Y=$'\033[33m'; B=$'\033[1m'; N=$'\033[0m'
ok()   { echo "${G}[+]${N} $*"; }
bad()  { echo "${R}[-]${N} $*"; }
warn() { echo "${Y}[!]${N} $*"; }
step() { echo "${B}== $* ==${N}"; }

shell() { "$ADB" shell "$@" 2>/dev/null; }
rootsh() { shell "$GL/glsu -c '$1'"; }

have_root() {
  shell "$GL/glsu -c 'id'" 2>/dev/null | grep -q "uid=0(root)"
}

wait_for() {
  local label="$1" marker="$2" log="$3" timeout_s="${4:-240}"
  local waited=0
  while [ "$waited" -lt "$timeout_s" ]; do
    if shell "grep -aq '$marker' '$log' 2>/dev/null"; then return 0; fi
    if [ "$(adb_state)" != "device" ]; then bad "device went away during $label (reboot/crash?)"; return 2; fi
    sleep 3; waited=$((waited + 3))
  done
  bad "timeout waiting for $label"
  return 1
}

adb_state() { "$ADB" get-state 2>/dev/null | tr -d '\r\n'; }

step "Preflight"
[ -x "$ADB" ] || { bad "adb not found at $ADB"; exit 1; }
[ "$(adb_state)" = "device" ] || { bad "no adb device (unlock, connect, retry)"; "$ADB" devices -l; exit 1; }

MODEL=$(shell getprop ro.product.model | tr -d '\r\n')
BUILD=$(shell getprop ro.build.version.incremental | tr -d '\r\n')
KVER=$(shell uname -r | tr -d '\r\n')
ok "device: $MODEL build $BUILD kernel $KVER"

WAITED=0
while [ "$(shell getprop sys.boot_completed | tr -d '\r\n')" != "1" ] && [ "$WAITED" -lt 180 ]; do
  sleep 4; WAITED=$((WAITED + 4))
done
[ "$WAITED" -lt 180 ] || { bad "device did not finish booting"; exit 1; }
while ! shell "pm path $NSG_PKG" 2>/dev/null | grep -q "base.apk"; do
  WAITED=$((WAITED + 4)); [ "$WAITED" -gt 180 ] && { bad "package manager not ready"; exit 1; }
  warn "waiting for package manager..."; sleep 4
done
ok "boot complete, package manager ready"
if [ "$KVER" != "$KERNEL_EXPECTED" ]; then
  bad "kernel mismatch: expected $KERNEL_EXPECTED"
  bad "refusing to run on an unverified kernel (wrong offsets can corrupt memory)"
  exit 1
fi

BATT=$(shell dumpsys battery | grep -m1 ' level' | tr -dc '0-9')
[ -n "${BATT:-}" ] && { [ "$BATT" -lt 25 ] && warn "battery at ${BATT}% — charge before racing the kernel" || ok "battery ${BATT}%"; }

step "Push payload"
shell "mkdir -p $GL" >/dev/null
[ -f "$GHOSTLOCK_BIN" ] || { bad "missing $GHOSTLOCK_BIN (build it from the ghostlock fork, or set GHOSTLOCK=/path/to/ghostlock)"; exit 1; }
[ -f "$BIN/glsu" ]    || { bad "missing $BIN/glsu (run: make)"; exit 1; }
[ -f "$BIN/diagtty" ] || { bad "missing $BIN/diagtty (run: make)"; exit 1; }
[ -f "$HERE/gl-allow-device.sh" ] || { bad "missing $HERE/gl-allow-device.sh"; exit 1; }
"$ADB" push "$GHOSTLOCK_BIN" /data/local/tmp/ghostlock >/dev/null
"$ADB" push "$BIN/glsu" "$GL/glsu" >/dev/null
"$ADB" push "$BIN/diagtty" "$GL/diagtty" >/dev/null
"$ADB" push "$HERE/gl-allow-device.sh" "$GL/gl-allow.sh" >/dev/null
shell "chmod 755 /data/local/tmp/ghostlock $GL/glsu $GL/diagtty $GL/gl-allow.sh"
REQ="$(mktemp)"; printf '\x7e\x37\x44\xb5\x7e' > "$REQ"
"$ADB" push "$REQ" "$GL/req.bin" >/dev/null; rm -f "$REQ"
ok "binaries pushed (ghostlock, glsu, diagtty)"

if have_root; then
  ok "root already available this boot — skipping exploit"
else
  step "Running GhostLock (temp root)"
  ENFORCE=$(shell getenforce | tr -d '\r\n')
  ok "SELinux: $ENFORCE"
  shell "rm -f $GL/ghostlock-run.log"
  shell "nohup /data/local/tmp/ghostlock > $GL/ghostlock-run.log 2>&1 &"
  if wait_for "exploit" "child is root" "$GL/ghostlock-run.log" 240; then
    ok "root obtained"
    wait_for "exploit tail" "temporary root ready" "$GL/ghostlock-run.log" 60 || warn "exploit log did not reach 'temporary root ready'"
  else
    bad "exploit failed — last log lines:"
    shell "tail -15 $GL/ghostlock-run.log" | sed 's/^/    /'
    exit 1
  fi
fi

step "Root services (verify/repair)"
rootsh "for p in \$(pidof glsu 2>/dev/null); do kill -9 \$p 2>/dev/null; done; i=0; while pidof glsu >/dev/null 2>&1; do i=\$((i+1)); [ \$i -gt 10 ] && break; sleep 0.3; done; [ -f $GL/glsu-uids ] || stat -c '%u $NSG_PKG' /data/data/$NSG_PKG > $GL/glsu-uids 2>/dev/null; setsid $GL/glsu daemon < /dev/null > $GL/glsu-daemon.log 2>&1 & sleep 0.7; echo started" >/dev/null
if shell "$GL/glsu -c 'id'" 2>/dev/null | grep -q "uid=0(root)"; then
  ok "glsu su daemon working (gl/glsu -c id -> uid 0)"
else
  bad "glsu daemon not answering"; exit 1
fi

if [ "${MOUNT_SU:-0}" = "1" ]; then
  step "Mounting su on /system (detector-VISIBLE, opt-in)"
  shell "GHOSTLOCK_MOUNT_SU=1 /data/local/tmp/ghostlock 2>/dev/null >/dev/null &" >/dev/null 2>&1
  warn "MOUNT_SU=1 requested — su is on /system/{bin,xbin}; banking apps may refuse"
else
  step "Hide-root verification"
  HITS=0
  for p in /system/bin/su /system/xbin/su /data/local/bin/su /data/local/xbin/su /data/local/su/bin/su; do
    shell "[ -e $p ] && echo hit" | grep -q hit && { bad "detector hit: $p exists"; HITS=1; }
  done
  MNT=$(shell "grep -cE ' /system type overlay | /data/local/tmp/ovl ' /proc/mounts" | tr -d '\r\n')
  [ "${MNT:-0}" -ge 1 ] && { bad "overlay mounts visible in /proc/mounts"; HITS=1; }
  if [ "$HITS" = "0" ]; then
    ok "no su paths, no overlay mounts — session is hidden (root via $GL/glsu)"
  else
    warn "run the exploit's unroot script or reboot to reset, then re-run with a fresh hidden session"
  fi
fi

rootsh "kill \$(pidof diagtty) 2>/dev/null; sleep 0.3; setsid $GL/diagtty /data/local/tmp/diag0 < /dev/null >> $GL/diagtty.log 2>&1 & sleep 0.7; ln -sf /data/local/tmp/diag0 /dev/umts_dm0; ln -sf /data/local/tmp/diag0 /dev/umts_router; ln -sf /data/local/tmp/diag0 /dev/diag" >/dev/null
sleep 1
if shell "$GL/glsu -c 'pidof diagtty'" | grep -q .; then
  ok "diagtty running: $(shell "$GL/glsu -c 'readlink /data/local/tmp/diag0'" | tr -d '\r\n')"
else
  bad "diagtty failed to start"; shell "$GL/glsu -c 'tail -3 $GL/diagtty.log'"; exit 1
fi
for l in /dev/umts_dm0 /dev/umts_router /dev/diag; do
  shell "$GL/glsu -c '[ -c $l ] && echo y'" | grep -q y && ok "$l -> diag0" || bad "$l missing"
done

step "Diag self-test (VERNO round-trip via QRTR)"
rootsh "PTS=\$(readlink /data/local/tmp/diag0); rm -f $GL/diagtest.out; (cat \$PTS > $GL/diagtest.out &) ; sleep 0.3; dd if=$GL/req.bin of=\$PTS bs=5 count=1 2>/dev/null; sleep 2; pkill -f \"cat \$PTS\" 2>/dev/null; true" >/dev/null
DIAGOK=$(shell "$GL/glsu -c 'od -An -tx1 $GL/diagtest.out 2>/dev/null'" | tr -d ' \r\n')
case "$DIAGOK" in
  7e01*) ok "modem answered: modem diag live" ;;
  "")    bad "no diag response — check diagtty.log / qrtr"; exit 1 ;;
  *)     warn "unexpected diag reply: $DIAGOK" ;;
esac

step "Kernel health check"
UNLABELED=$(shell "$GL/glsu -c 'ps -AZ 2>/dev/null | grep -c unlabeled'" | tr -d '\r\n')
if [ "${UNLABELED:-0}" -gt 0 ] 2>/dev/null; then
  bad "SELinux corrupted: ${UNLABELED} unlabeled processes"
  bad "kernel state is bad — stop using the phone and reboot it now"
  bad "recovery: adb reboot (hold power + volume-up if adb fails)"
  exit 1
fi
ok "no unlabeled processes — kernel SELinux state healthy"

step "Restart NSG"
shell "am force-stop $NSG_PKG" >/dev/null 2>&1
sleep 1
NPID=""
for attempt in 1 2 3; do
  shell "monkey -p $NSG_PKG -c android.intent.category.LAUNCHER 1" >/dev/null 2>&1
  sleep 12
  NPID=$(shell pidof $NSG_PKG | tr -d '\r\n')
  [ -n "$NPID" ] && break
  warn "NSG launch attempt $attempt failed — retrying"
done
if [ -n "$NPID" ]; then
  ok "NSG running (pid $NPID)"
  BRIDGE_FD=$(shell "$GL/glsu -c 'ls -l /proc/\$(pidof bridge)/fd 2>/dev/null | grep -c pts'" | tr -d '\r\n')
  if [ "${BRIDGE_FD:-0}" -ge 1 ]; then
    ok "NSG bridge holds the diag pty — full modem info active"
  else
    warn "bridge has no diag fd yet — open a measurement view in NSG and recheck"
  fi
else
  warn "NSG did not restart (not installed?) — start it manually"
fi

echo
step "Ready"
ok "temp root + QRTR diag bridge active (hidden mode)"
echo "    one-shot:     adb shell \"$GL/glsu -c '<command>'\""
echo "    root shell:   adb shell  ->  cd /data/local/tmp/gl  ->  cat < out &  ->  cat > in"
echo "    app root:     ./gl-allow add <package>   (or: add-recent 1 | list | remove)"
echo "    unroot now:   adb shell \"$GL/glsu -c 'sh /data/local/tmp/.ghostlock_unroot.sh'\" (no reboot)"
echo "    cleanup:      reboot (restores enforcing SELinux, removes all session state)"
echo "    residuals:    SELinux stays permissive + uid-0 daemon procs are visible in-session;"
echo "                  TMX-class detectors (e.g. Hang Seng) may still refuse — use them unrooted"
