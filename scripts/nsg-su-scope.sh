#!/system/bin/sh
# nsg-su-scope: mount a su-carrying overlay into NSG's mount namespace only.
# Each NSG instance gets its OWN upperdir+workdir pair (overlayfs requires a
# dedicated workdir per live mount; sharing one across instances corrupts
# lookups and breaks app startup with "failed to attach"). Stage each
# instance BEFORE mounting and never modify it afterwards: cross-namespace
# changes to a live upperdir confuse overlayfs (ESTALE).
GL=/data/local/tmp/gl
OVL=/data/local/tmp/ovl
LOG=/data/local/tmp/.ghostlock_ksu.log
PKG=com.qtrun.QuickTest
mkdir -p "$OVL/instances" 2>/dev/null
grep -q " $OVL " /proc/mounts || mount -t tmpfs tmpfs "$OVL"
stage_instance() {
  mkdir -p "$1/bin" "$1/xbin" "$1/../work"
  cp "$GL/glsu" "$1/bin/su" 2>/dev/null
  cp "$GL/glsu" "$1/xbin/su" 2>/dev/null
  chmod 755 "$1/bin/su" "$1/xbin/su" 2>/dev/null
}
cleanup_dead() {
  for D in "$OVL/instances"/*; do
    [ -d "$D" ] || continue
    P=${D##*/}
    [ -d /proc/"$P" ] || { [ "$P" != "$LAST" ] && rm -rf "$D" 2>/dev/null; }
  done
}
LAST=""
while :; do
  cleanup_dead
  P=$(pidof $PKG 2>/dev/null | cut -d' ' -f1)
  if [ -n "$P" ] && [ "$P" != "$LAST" ]; then
    INST="$OVL/instances/$P"
    # never mount mid-attach: wait for the process to settle first
    sleep 6
    if [ "$P" != "$(pidof $PKG 2>/dev/null | cut -d' ' -f1)" ]; then
      continue
    fi
    if nsenter -t "$P" -m -- grep -q " /system overlay " /proc/mounts 2>/dev/null; then
      LAST=$P
      echo "[*] $PKG pid $P already scoped" >> $LOG
    else
      stage_instance "$INST/up"
      if nsenter -t "$P" -m -- mount -t overlay overlay -o lowerdir=/system,upperdir="$INST/up",workdir="$INST/work" /system 2>>$LOG; then
        LAST=$P
        echo "[*] su scoped into $PKG pid $P (instance $P)" >> $LOG
      else
        sleep 2
      fi
    fi
  fi
  sleep 1
done
