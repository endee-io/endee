#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# generate_fstab.sh
#
# Prints fstab-compatible lines for currently-mounted real filesystems with
# UUIDs.  Uses findmnt (not lsblk) so that bind mounts, Docker propagation
# mounts, and other derivative mounts are correctly excluded.
#
# With --apply, appends only lines whose (UUID, mountpoint) pair is not already
# present in /etc/fstab — and never twice within the same run.
#
# Tested on Debian 12 (bookworm) and Debian 13 (trixie).
# Requires: bash >= 4.0, findmnt, findfs (util-linux).
# -----------------------------------------------------------------------------

set -uo pipefail

readonly FSTAB="/etc/fstab"
readonly PROGNAME="${0##*/}"
APPLY=0
DRY_RUN=0
VERBOSE=0
NON_INTERACTIVE=0
BACKUP=""

# ----------------------------- helpers ----------------------------------------

log_info()  { echo "[INFO]  $*"; }
log_warn()  { echo "[WARN]  $*" >&2; }
log_error() { echo "[ERROR] $*" >&2; }

usage() {
  cat <<EOF
Usage: $PROGNAME [OPTIONS]

Options:
  --apply             Append new entries to $FSTAB (requires root)
  --dry-run           Show what --apply would do without writing anything
  --non-interactive   When a UUID has multiple mounts, pick the shortest
                      path automatically instead of prompting
  --verbose           Print extra diagnostics
  -h, --help          Show this help

Without flags the script prints candidate fstab lines to stdout.
EOF
  exit 0
}

# ----------------------------- arg parsing ------------------------------------

while (( $# )); do
  case "$1" in
    --apply)   APPLY=1   ;;
    --dry-run) DRY_RUN=1 ;;
    --non-interactive) NON_INTERACTIVE=1 ;;
    --verbose) VERBOSE=1 ;;
    -h|--help) usage      ;;
    *) log_error "Unknown option: $1"; usage ;;
  esac
  shift
done

if (( APPLY && DRY_RUN )); then
  log_error "--apply and --dry-run are mutually exclusive"
  exit 1
fi

if (( APPLY )) && (( EUID != 0 )); then
  log_error "Need root/sudo for --apply"
  exit 1
fi

# ----------------------------- prereqs ----------------------------------------

for cmd in findmnt findfs; do
  if ! command -v "$cmd" &>/dev/null; then
    log_error "Required command not found: $cmd (install util-linux)"
    exit 1
  fi
done

if (( BASH_VERSINFO[0] < 4 )); then
  log_error "Bash >= 4.0 required (found ${BASH_VERSION})"
  exit 1
fi

# ----------------------------- state ------------------------------------------

declare -A HAVE=()       # (uuid|||mountpoint) pairs already in /etc/fstab
declare -A WANT_UUID=()  # key -> uuid
declare -A WANT_MNT=()   # key -> mountpoint
declare -A WANT_FS=()    # key -> fstype
WARNINGS=0
ADDED=0
SKIPPED=0

# -------- Load existing (UUID, mountpoint) pairs from /etc/fstab -------------

if [[ -r "$FSTAB" ]]; then
  while read -r f1 f2 f3 _rest; do
    [[ -z "${f1:-}" || "${f1:0:1}" == "#" ]] && continue
    [[ "${f1}" == UUID=* ]] || continue

    uuid="${f1#UUID=}"
    mnt="${f2:-}"
    fstype="${f3:-}"

    [[ "$fstype" == "swap" ]] && mnt="swap"
    [[ -n "$uuid" && -n "$mnt" ]] || continue

    HAVE["${uuid}|||${mnt}"]=1
  done < "$FSTAB"
else
  log_warn "$FSTAB is not readable; treating as empty"
fi

# -------- Collect desired entries using findmnt -------------------------------
#
# Why findmnt instead of lsblk?
#
# lsblk lists every mountpoint a block device appears at, including Docker
# bind-propagation mounts (e.g. /var/lib/docker/volumes/...).  It gives no
# way to tell which is the "real" filesystem mount vs a derivative.
#
# findmnt exposes FSROOT: the path *within* the filesystem that is mounted.
#   FSROOT="/"           -> direct/real mount of the whole filesystem
#   FSROOT="/some/sub"   -> bind mount of a subdirectory
#
# We only want FSROOT="/" entries — those are the ones that belong in fstab.
# Docker (and any other bind mount consumer) will work automatically once the
# real mount is in place.
# -----------------------------------------------------------------------------

is_virtual_fstype() {
  case "$1" in
    tmpfs|devtmpfs|sysfs|proc|cgroup|cgroup2|securityfs|\
    debugfs|tracefs|configfs|fusectl|hugetlbfs|mqueue|\
    pstore|bpf|autofs|overlay|squashfs|nsfs|devpts|ramfs)
      return 0 ;;
    *)
      [[ "$1" == fuse.* ]] && return 0
      return 1 ;;
  esac
}

SKIPPED_VIRTUAL=0
SKIPPED_BIND=0
SKIPPED_NO_UUID=0

# --- Pass 1: collect ALL candidate mountpoints per UUID ----------------------
# A single partition (one UUID) can appear at multiple mountpoints when Docker
# or other tools create bind mounts.  We collect every FSROOT="/" mount, then
# in pass 2 pick the best (shortest path) mountpoint per UUID for fstab.

declare -A UUID_CANDIDATES=()  # uuid -> newline-separated list of mountpoints
declare -A UUID_FSTYPE=()      # uuid -> fstype (same for all mounts of a UUID)

# findmnt --real skips pseudo-filesystems.  --pairs gives KEY="VALUE" output.
while IFS= read -r line; do
  [[ -z "$line" ]] && continue

  # Parse KEY="VALUE" pairs
  target="" source="" fstype="" uuid="" fsroot=""
  while [[ "$line" =~ ([A-Z_-]+)=\"([^\"]*)\" ]]; do
    case "${BASH_REMATCH[1]}" in
      TARGET)  target="${BASH_REMATCH[2]}"  ;;
      SOURCE)  source="${BASH_REMATCH[2]}"  ;;
      FSTYPE)  fstype="${BASH_REMATCH[2]}"  ;;
      UUID)    uuid="${BASH_REMATCH[2]}"    ;;
      FSROOT)  fsroot="${BASH_REMATCH[2]}"  ;;
    esac
    line="${line#*\"${BASH_REMATCH[2]}\"}"
  done

  # Must have a UUID to be useful in fstab
  if [[ -z "$uuid" ]]; then
    (( SKIPPED_NO_UUID++ ))
    (( VERBOSE )) && log_info "Skipping (no UUID): ${source:-?} on ${target:-?}"
    continue
  fi

  [[ -n "$fstype" ]] || continue

  # Skip virtual/pseudo filesystems (belt-and-suspenders with --real)
  if is_virtual_fstype "$fstype"; then
    (( SKIPPED_VIRTUAL++ ))
    (( VERBOSE )) && log_info "Skipping virtual: $fstype on ${target:-<none>}"
    continue
  fi

  # Only keep direct mounts where FSROOT="/".
  if [[ "$fsroot" != "/" ]]; then
    (( SKIPPED_BIND++ ))
    (( VERBOSE )) && log_info "Skipping bind/sub-mount: ${target:-<none>} (fsroot=$fsroot)"
    continue
  fi

  mountpoint="${target:-}"
  [[ "$fstype" == "swap" ]] && mountpoint="swap"
  [[ -n "$mountpoint" ]] || continue

  # Normalize: strip trailing slash except for root
  if [[ "$mountpoint" != "/" ]]; then
    mountpoint="${mountpoint%/}"
  fi

  UUID_FSTYPE["$uuid"]="$fstype"

  # Append to candidate list (newline-separated)
  if [[ -v UUID_CANDIDATES["$uuid"] ]]; then
    UUID_CANDIDATES["$uuid"]+=$'\n'"$mountpoint"
  else
    UUID_CANDIDATES["$uuid"]="$mountpoint"
  fi

done < <(findmnt --real --pairs -o TARGET,SOURCE,FSTYPE,UUID,FSROOT -n 2>/dev/null)

# --- Pass 2: for each UUID, pick ONE mountpoint for fstab --------------------
#
# If a UUID has multiple FSROOT="/" mounts (e.g. real mount + Docker bind),
# we ask the user to choose.  With --non-interactive, fall back to the
# shortest-path heuristic (penalising /var/lib/docker and /snap paths).

# Auto-pick: shortest path, with Docker/snap penalised.
auto_pick_mountpoint() {
  local candidates="$1"
  local best="" best_len=999999

  while IFS= read -r candidate; do
    [[ -z "$candidate" ]] && continue
    local len=${#candidate}
    local effective_len=$len
    case "$candidate" in
      /var/lib/docker/*|/snap/*) effective_len=$(( len + 10000 )) ;;
    esac
    if (( effective_len < best_len )); then
      best="$candidate"
      best_len=$effective_len
    fi
  done <<< "$candidates"

  printf '%s' "$best"
}

# Interactive prompt: show numbered list, read choice.
ask_user_mountpoint() {
  local uuid="$1" candidates="$2"
  local -a opts=()

  while IFS= read -r c; do
    [[ -n "$c" ]] && opts+=("$c")
  done <<< "$candidates"

  echo "" >&2
  log_warn "UUID=$uuid is mounted at ${#opts[@]} locations:"
  local i
  for i in "${!opts[@]}"; do
    echo "  $((i+1))  ${opts[$i]}" >&2
  done
  echo "  s  Skip this UUID entirely" >&2
  echo "" >&2

  while true; do
    read -rp "Which mountpoint should go in fstab? [1-${#opts[@]}/s]: " choice </dev/tty
    if [[ "$choice" == "s" || "$choice" == "S" ]]; then
      printf ''   # empty = skip
      return
    fi
    if [[ "$choice" =~ ^[0-9]+$ ]] && (( choice >= 1 && choice <= ${#opts[@]} )); then
      printf '%s' "${opts[$((choice-1))]}"
      return
    fi
    echo "  Invalid choice. Enter 1-${#opts[@]} or 's' to skip." >&2
  done
}

for uuid in "${!UUID_CANDIDATES[@]}"; do
  candidates="${UUID_CANDIDATES[$uuid]}"
  fstype="${UUID_FSTYPE[$uuid]}"

  # Count candidates
  local_count=0
  while IFS= read -r _; do (( local_count++ )); done <<< "$candidates"

  if (( local_count == 1 )); then
    # Only one mount — use it directly
    mountpoint="$candidates"
  elif (( NON_INTERACTIVE )); then
    # Multiple mounts, non-interactive: auto-pick with heuristic
    mountpoint="$(auto_pick_mountpoint "$candidates")"
    log_warn "UUID=$uuid has $local_count mounts; auto-picked: $mountpoint"
  else
    # Multiple mounts, interactive: ask the user
    mountpoint="$(ask_user_mountpoint "$uuid" "$candidates")"
    if [[ -z "$mountpoint" ]]; then
      log_info "Skipping UUID=$uuid (user choice)"
      continue
    fi
  fi

  [[ -n "$mountpoint" ]] || continue

  key="${uuid}|||${mountpoint}"
  WANT_UUID["$key"]="$uuid"
  WANT_MNT["$key"]="$mountpoint"
  WANT_FS["$key"]="$fstype"
done

if (( ${#WANT_UUID[@]} == 0 )); then
  log_warn "No eligible filesystems found — nothing to do"
  exit 0
fi

(( VERBOSE )) && {
  log_info "Eligible filesystems: ${#WANT_UUID[@]}"
  log_info "Skipped (no UUID): $SKIPPED_NO_UUID"
  log_info "Skipped (virtual):  $SKIPPED_VIRTUAL"
  log_info "Skipped (bind/sub): $SKIPPED_BIND"
}

# -------- Pick sensible dump/pass values -------------------------------------

get_dump_pass() {
  local mnt="$1" fstype="$2"
  case "$mnt" in
    swap) echo "0 0"; return ;;
    /)    echo "0 1"; return ;;
  esac
  # btrfs doesn't use traditional fsck
  [[ "$fstype" == "btrfs" ]] && { echo "0 0"; return; }
  # everything else: fsck after root
  echo "0 2"
}

# -------- Backup fstab before writing ----------------------------------------

backup_fstab() {
  BACKUP="${FSTAB}.bak.$(date +%Y%m%d%H%M%S)"
  if ! cp "$FSTAB" "$BACKUP"; then
    log_error "Failed to back up $FSTAB to $BACKUP — aborting"
    exit 1
  fi
  log_info "Backup saved: $BACKUP"
}

# -------- Emit and optionally apply ------------------------------------------

mapfile -t ORDERED_KEYS < <(printf '%s\n' "${!WANT_UUID[@]}" | sort)

if (( APPLY && !DRY_RUN )); then
  backup_fstab
fi

for key in "${ORDERED_KEYS[@]}"; do
  uuid="${WANT_UUID[$key]}"
  mountpoint="${WANT_MNT[$key]}"
  fstype="${WANT_FS[$key]}"
  dump_pass="$(get_dump_pass "$mountpoint" "$fstype")"

  # Validate the UUID still resolves (guard against race / hot-unplug)
  if ! findfs "UUID=$uuid" &>/dev/null; then
    log_warn "UUID=$uuid no longer resolves to a device — skipping"
    (( WARNINGS++ ))
    continue
  fi

  entry="UUID=${uuid}  ${mountpoint}  ${fstype}  defaults  ${dump_pass}"

  if [[ -n "${HAVE[$key]:-}" ]]; then
    (( VERBOSE )) && log_info "Already in fstab: $entry"
    (( SKIPPED++ ))
    continue
  fi

  echo "$entry"

  if (( DRY_RUN )); then
    log_info "[dry-run] Would append above line"
    continue
  fi

  if (( APPLY )); then
    if ! echo "$entry" >> "$FSTAB"; then
      log_error "Failed to write to $FSTAB — stopping"
      log_error "Restore from backup: $BACKUP"
      exit 1
    fi
    HAVE["$key"]=1
    (( ADDED++ ))
    log_info "Added to $FSTAB"
  fi
done

# -------- Summary -------------------------------------------------------------

echo ""
if (( APPLY )); then
  log_info "Done. Added=$ADDED  Skipped=$SKIPPED  Warnings=$WARNINGS"
  (( WARNINGS > 0 )) && log_warn "Review warnings above. Backup at: $BACKUP"
  log_info "Verify with:  cat $FSTAB"
  log_info "Test with:    sudo mount -a --fake"
  log_info "Then reboot or: sudo mount -a"
elif (( DRY_RUN )); then
  log_info "Dry run complete. No changes written."
else
  log_info "Preview only. Use --apply to write or --dry-run to simulate."
fi

(( WARNINGS > 0 )) && exit 2
exit 0