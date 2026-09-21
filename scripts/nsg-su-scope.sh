#!/system/bin/sh
# nsg-su-scope v5: scope EVERY process of the app's uid, not just the main
# process. NSG runs its root-exec helper in a namespace copied at the
# helper's fork time; only namespaces created AFTER our mounts inherit them,
# so each new uid-10474 process gets freeze->mount->resume on first sight.
GL=/data/local/tmp/gl
OVL=/data/local/tmp/ovl
LOG=/data/local/tmp/.ghostlock_ksu.log
UID_APP=10474
mkdir -p /data/local/xbin /data/local/bin /data/local/su/bin 2>/dev/null
mkdir -p "$OVL/instances" 2>/dev/null
grep -q " $OVL " /proc/mounts || mount -t tmpfs tmpfs "$OVL"
for D in xbin bin su; do mkdir -p "$GL/sustage/$D"; cp "$GL/suwrap" "$GL/sustage/$D/su" 2>/dev/null; chmod 755 "$GL/sustage/$D/su" 2>/dev/null; done
SEEN=/data/local/tmp/gl/.scope-seen
: > $SEEN
scope_one() {
  P=$1
  kill -STOP "$P" 2>/dev/null
  if [ ! -d /proc/$P ]; then kill -CONT "$P" 2>/dev/null; return; fi
  if ! nsenter -t "$P" -m -- grep -q " /system overlay " /proc/mounts 2>/dev/null; then
    INST="$OVL/instances/$P"
    mkdir -p "$INST/up/bin" "$INST/up/xbin" "$INST/work"
    cp "$GL/suwrap" "$INST/up/bin/su" 2>/dev/null
    cp "$GL/suwrap" "$INST/up/xbin/su" 2>/dev/null
    chmod 755 "$INST/up/bin/su" "$INST/up/xbin/su" 2>/dev/null
    nsenter -t "$P" -m -- mount -t overlay overlay -o lowerdir=/system,upperdir="$INST/up",workdir="$INST/work" /system 2>>$LOG
    echo "[*] scoped pid $P (overlay only, no binds)" >> $LOG
  fi
  kill -CONT "$P" 2>/dev/null
}
while :; do
  for P in $(ps -A -o UID,PID 2>/dev/null | awk -v u=$UID_APP '$1==u {print $2}'); do
    grep -qx "$P" $SEEN && continue
    echo "$P" >> $SEEN
    scope_one $P
  done
  # periodically prune dead pids so a recycled pid gets re-scoped
  N=$(( ${N:-0} + 1 ))
  if [ $((N % 40)) -eq 0 ]; then
    cp $SEEN $SEEN.b
    : > $SEEN
    while read -r Q; do [ -d /proc/$Q ] && echo "$Q" >> $SEEN; done < $SEEN.b
  fi
  sleep 0.15
done
