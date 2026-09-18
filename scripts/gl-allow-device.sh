#!/system/bin/sh
UIDS=/data/local/tmp/gl/glsu-uids
PKGDATA=/data/data

uid_of() { stat -c %u "$PKGDATA/$1" 2>/dev/null; }

has_uid() { grep -qE "^$1[[:space:]]" "$UIDS" 2>/dev/null; }

pkg_of_uid() {
  for d in "$PKGDATA"/*; do
    [ -d "$d" ] || continue
    [ "$(stat -c %u "$d" 2>/dev/null)" = "$1" ] && { basename "$d"; return 0; }
  done
  return 1
}

append_uid() {
  uid="$1"; pkg="$2"
  has_uid "$uid" && { echo "already allowed: $pkg (uid $uid)"; return 0; }
  echo "$uid $pkg" >>"$UIDS" || { echo "cannot write $UIDS"; return 1; }
  echo "allowed: $pkg (uid $uid)"
}

cmd="${1:-}"; shift 2>/dev/null
case "$cmd" in
add)
  pkg="${1:-}"
  [ -n "$pkg" ] || { echo "usage: gl-allow.sh add <package>"; exit 1; }
  uid=$(uid_of "$pkg")
  [ -n "$uid" ] || { echo "no such package (installed?): $pkg"; exit 1; }
  append_uid "$uid" "$pkg"
  ;;
add-recent)
  n="${1:-1}"
  case "$n" in ''|*[!0-9]*) n=1 ;; esac
  added=0
  for d in $(ls -t "$PKGDATA" 2>/dev/null); do
    [ "$added" -ge "$n" ] && break
    uid=$(stat -c %u "$PKGDATA/$d" 2>/dev/null) || continue
    case "$uid" in ''|*[!0-9]*) continue ;; esac
    [ "$uid" -lt 10000 ] && continue
    has_uid "$uid" && continue
    append_uid "$uid" "$d"
    added=$((added + 1))
  done
  [ "$added" -eq 0 ] && echo "no new packages to allow"
  ;;
list)
  [ -f "$UIDS" ] || { echo "(allowlist file missing: $UIDS)"; exit 1; }
  echo "uid    package (from allowlist)"
  while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    uid=${line%%[!0-9]*}
    [ -n "$uid" ] || continue
    note=$(echo "$line" | sed "s/^$uid[[:space:]]*//")
    live=$(pkg_of_uid "$uid" || true)
    [ -n "$live" ] && note="$live"
    echo "$uid    $note"
  done <"$UIDS"
  ;;
remove)
  pkg="${1:-}"
  [ -n "$pkg" ] || { echo "usage: gl-allow.sh remove <package>"; exit 1; }
  uid=$(uid_of "$pkg")
  [ -n "$uid" ] || uid=$(echo "$pkg" | grep -oE '^[0-9]+$' || true)
  [ -n "$uid" ] || { echo "no such package: $pkg"; exit 1; }
  sed -i "\|^$uid[[:space:]]|d" "$UIDS"
  echo "removed uid $uid ($pkg)"
  ;;
*)
  echo "usage: gl-allow.sh add <package> | add-recent [N] | list | remove <package>"
  echo "  allowlist: $UIDS (live; no daemon restart needed)"
  ;;
esac
