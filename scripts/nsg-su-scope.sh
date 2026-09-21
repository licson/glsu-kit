#!/system/bin/sh
# nsg-su-scope: on each new NSG main pid, mount the su overlay into that
# process's mount namespace only. Other apps (and their /proc/mounts)
# never see su or the overlay. Runs as root for the whole session.
GL=/data/local/tmp/gl
OVL=/data/local/tmp/ovl
LOG=/data/local/tmp/.ghostlock_ksu.log
PKG=com.qtrun.QuickTest
mkdir -p $OVL 2>/dev/null
grep -q " $OVL " /proc/mounts || mount -t tmpfs tmpfs $OVL
mkdir -p $OVL/up/bin $OVL/work
cp $GL/glsu $OVL/up/bin/su 2>/dev/null
chmod 755 $OVL/up/bin/su
LAST=""
while :; do
  P=$(pidof $PKG 2>/dev/null | cut -d' ' -f1)
  if [ -n "$P" ] && [ "$P" != "$LAST" ]; then
    if nsenter -t $P -m -- grep -q " /system overlay " /proc/mounts 2>/dev/null; then
      LAST=$P
      echo "[*] $PKG pid $P already scoped" >> $LOG
    elif nsenter -t $P -m -- mount -t overlay overlay -o lowerdir=/system,upperdir=$OVL/up,workdir=$OVL/work /system 2>>$LOG; then
      LAST=$P
      echo "[*] su scoped into $PKG pid $P" >> $LOG
    else
      sleep 2
    fi
  fi
  sleep 1
done
