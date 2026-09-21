#!/usr/bin/env bash
#
# FPM integration test matrix.
#
# Builds FPM, creates a real local FPM repository from test packages,
# serves it over HTTP and runs the full pipeline: mirror select -> update ->
# validation -> search -> fuzzy search -> download -> checksum -> install ->
# manifest -> local DB -> CleanMyDisk, plus failure/fallback scenarios.
#
# Each scenario gets its own cwd (local fpm_db/fpm_cache) and its own HOME with
# mirrors at "$HOME/fpm/etc/mirrors.list", so the run is independent of any
# system /etc/fpm/mirrors.list.
#
# Root-required steps use `sudo -n`. If sudo needs a password, export
# FPM_SUDO_PASS=<password> for the script to authenticate via `sudo -S`.
# The password is supplied at runtime from the environment and never written
# to any file in the repository.
#
# Usage:  tests/run_tests.sh [path-to-fpm-binary]
set -u

PROJECT_DIR=$(cd "$(dirname "$0")/.." && pwd)
FPM=${1:-"$PROJECT_DIR/build/fpm"}

GOOD_PORT=18777
JSON_PORT=18778
GARBAGE_PORT=18779
BADSHA_PORT=18780
ERR500_PORT=18781

RESULTS="$PROJECT_DIR/tests/.results.tsv"
SCRATCH="$PROJECT_DIR/tests/scratch"
: "${KEEP_SCRATCH:=0}"

PASS=0
FAIL=0
BLOCKED=0

start_server() { # start_server <port> <root>
    local port=$1 root=$2
    ( cd "$root" && exec python3 -m http.server "$port" --bind 127.0.0.1 >/dev/null 2>&1 ) &
    SERVER_PIDS+=($!)
    local i=0
    while :; do
        if curl -fsS -o /dev/null --max-time 2 "http://127.0.0.1:$port/" >/dev/null 2>&1; then
            return 0
        fi
        i=$((i + 1))
        [ "$i" -gt 50 ] && { echo "SERVER FAILED TO START on port $port"; return 1; }
        sleep 0.1
    done
}

stop_servers() {
    for p in "${SERVER_PIDS[@]:-}"; do kill "$p" >/dev/null 2>&1; done
    wait 2>/dev/null
}

record() { # record <test> <status> <detail>
    printf '%-28s | %-7s | %s\n' "$1" "$2" "$3" >>"$RESULTS"
    case "$2" in
        PASS) PASS=$((PASS + 1)) ;;
        FAIL) FAIL=$((FAIL + 1)) ;;
        BLOCKED) BLOCKED=$((BLOCKED + 1)) ;;
    esac
}

expect_contains() { # expect_contains <out> <needle>
    case "$1" in
        *"$2"*) return 0 ;;
        *) return 1 ;;
    esac
}

# Run a command as root. If FPM_SUDO_PASS is set the password is fed through
# `sudo -S` (first stdin line); sudo passes the remainder to the command, so
# `(pass\n; app-input\n) | sudox ...` still lets the app read its own stdin.
sudox() {
    if [ -n "${FPM_SUDO_PASS:-}" ]; then
        printf '%s\n' "$FPM_SUDO_PASS" | sudo -S -p '' "$@"
    else
        sudo -n "$@"
    fi
}

rootid() { # rootid <detail-flag>
    if [ -n "${FPM_SUDO_PASS:-}" ]; then
        printf '%s\n' "$FPM_SUDO_PASS" | sudo -S -p '' true >/dev/null 2>&1 && echo 1 || echo 0
    else
        sudo -n true 2>/dev/null && echo 1 || echo 0
    fi
}

# Scenario dir = cwd (local fpm_db/fpm_cache) + own HOME with mirrors.list
mkscenario() { # mkscenario <dir> [urls...]
    local dir=$1; shift
    rm -rf "$dir"
    mkdir -p "$dir/home/fpm/etc"
    set_mirrors "$dir" "$@"
}

set_mirrors() { # set_mirrors <dir> [urls...]  -- rewrite mirrors.list in place
    local dir=$1; shift
    : > "$dir/home/fpm/etc/mirrors.list"
    for u in "$@"; do
        case "$u" in
            *"|"*) echo "$u" >> "$dir/home/fpm/etc/mirrors.list" ;;
            *)     echo "$u | Local | http | 0" >> "$dir/home/fpm/etc/mirrors.list" ;;
        esac
    done
}

fpmrun() { # fpmrun <scenario-dir> <args...>
    local dir=$1; shift
    ( cd "$dir" && HOME="$dir/home" "$FPM" "$@" )
}

fpmrun_root() { # fpmrun_root <scenario-dir> <stdin> <args...>
    local dir=$1 input=$2; shift 2
    if [ "$RDRY" != "1" ]; then return 9; fi
    # Refresh the sudo timestamp (if password-driven) then use plain `sudo -n`
    # so stdin stays undistorted for the fpm process (sudo -S would swallow it).
    if [ -n "${FPM_SUDO_PASS:-}" ]; then
        printf '%s\n' "$FPM_SUDO_PASS" | sudo -S -p '' -v >/dev/null 2>&1
    fi
    if [ "$input" = "-" ]; then
        sudo -n env HOME="$dir/home" sh -c "cd '$dir' && '$FPM' $*"
    else
        printf '%s\n' "$input" | sudo -n env HOME="$dir/home" sh -c "cd '$dir' && '$FPM' $*"
    fi
}

cleanup() {
    stop_servers
    if [ "$KEEP_SCRATCH" = "1" ]; then
        echo "scratch kept: $SCRATCH"
    else
        rm -rf "$SCRATCH"
    fi
    rm -rf "$PROJECT_DIR/tests/.results.tsv" /tmp/fpm_testjunk /tmp/fpm_live
}

trap cleanup EXIT

SERVER_PIDS=()
mkdir -p "$SCRATCH"
: > "$RESULTS"

RDRY=$(rootid)
if [ "$RDRY" = "1" ]; then
    ROOT_CMT="password from FPM_SUDO_PASS" ; [ -z "${FPM_SUDO_PASS:-}" ] && ROOT_CMT="passwordless sudo"
else
    ROOT_CMT="no sudo available"
fi

# =========================================================
echo "== 0. Environment =="
# =========================================================

if [ ! -x "$FPM" ]; then
    echo "Building FPM..."
    if ( cd "$PROJECT_DIR" && g++ -std=c++17 -Iinclude \
         src/*.cpp -o "$FPM" ); then
        record "build" PASS "g++ (no cmake on host)"
    else
        record "build" FAIL "compilation error"
    fi
else
    record "build" PASS "binary already built"
fi

# =========================================================
echo "== 1. Build and package the test repository =="
# =========================================================

W="$SCRATCH/w"
REPO="$SCRATCH/repo"
mkscenario "$W" "http://127.0.0.1:$GOOD_PORT"
mkdir -p "$REPO"
cp -r "$PROJECT_DIR/tests/recipes" "$SCRATCH/recipes"

BUILD_OK=1
for r in test-lib tree fpm-hello; do
    if fpmrun "$W" -b "$SCRATCH/recipes/$r" >/dev/null 2>&1; then
        mv "$W/$r-1.0.0.x86_64.fpm" "$REPO/" 2>/dev/null || BUILD_OK=0
    else
        BUILD_OK=0
    fi
done

if [ "$BUILD_OK" = "1" ] && [ -n "$(ls "$REPO"/*.fpm 2>/dev/null)" ]; then
    record "build test packages (fpm -b)" PASS "$(ls "$REPO" | tr '\n' ' ')"
else
    record "build test packages (fpm -b)" FAIL "recipe build failure"
    exit 1
fi

if fpmrun "$W" -repo "$REPO" --base "http://127.0.0.1:$GOOD_PORT" >/dev/null 2>&1; then
    [ -f "$REPO/packages.db" ] && [ -f "$REPO/packages.json" ] \
        && record "repository backend (fpm -repo)" PASS "packages.db + packages.json generated" \
        || record "repository backend (fpm -repo)" FAIL "index files missing"
else
    record "repository backend (fpm -repo)" FAIL "command failed"
    exit 1
fi

start_server "$GOOD_PORT" "$REPO" || { record "local mirror server" FAIL "http.server"; exit 1; }
cp -r "$REPO" "$SCRATCH/json-only" && rm -f "$SCRATCH/json-only/packages.db"
start_server "$JSON_PORT" "$SCRATCH/json-only" || record "json mirror server" FAIL
mkdir -p "$SCRATCH/garbage" && printf '# garbage\nnot an fpm index\ntotally|broken\n' > "$SCRATCH/garbage/packages.db"
start_server "$GARBAGE_PORT" "$SCRATCH/garbage" || record "garbage mirror server" FAIL
cp -r "$REPO" "$SCRATCH/badsha" && python3 - "$SCRATCH/badsha/packages.db" <<'PYEOF'
import sys
path = sys.argv[1]
lines = open(path).read().splitlines()
out = []
for ln in lines:
    f = ln.split('|')
    if len(f) >= 7 and f[0].strip() == 'tree':
        f[6] = 'a' * 64
        ln = '|'.join(f)
    out.append(ln)
open(path, 'w').write('\n'.join(out) + '\n')
PYEOF
start_server "$BADSHA_PORT" "$SCRATCH/badsha" || record "badsha mirror server" FAIL
( python3 "$PROJECT_DIR/tests/tools/server_500.py" "$ERR500_PORT" >/dev/null 2>&1 ) &
SERVER_PIDS+=($!)
sleep 0.3
record "local HTTP test mirrors" PASS "$GOOD_PORT/$JSON_PORT/$GARBAGE_PORT/$BADSHA_PORT/$ERR500_PORT"
record "sudo availability" "$([ "$RDRY" = "1" ] && echo PASS || echo BLOCKED)" "$ROOT_CMT"

# =========================================================
echo "== 2. CLI basics =="
# =========================================================

out=$(fpmrun "$W" --help 2>&1); rc=$?
[ $rc -eq 0 ] && expect_contains "$out" "-repo" \
    && record "fpm --help" PASS "" || record "fpm --help" FAIL "rc=$rc"
out=$(fpmrun "$W" -vr 2>&1); rc=$?
[ $rc -eq 0 ] && expect_contains "$out" "0.2.0" \
    && record "fpm -vr" PASS "" || record "fpm -vr" FAIL "rc=$rc"
fpmrun "$W" -x-unknown >/dev/null 2>&1; rc=$?
record "unknown option rejected" "$([ $rc -ne 0 ] && echo PASS || echo FAIL)" "rc=$rc"

# =========================================================
echo "== 3. Repository update (fpm -upd / fpm update) =="
# =========================================================

out=$(fpmrun "$W" -upd 2>&1); rc=$?
if [ $rc -eq 0 ] && expect_contains "$out" "Repository index updated: 3 package(s)"; then
    record "fpm -upd (fresh index)" PASS "3 packages"
else
    record "fpm -upd (fresh index)" FAIL "rc=$rc :: $(echo "$out" | tail -4)"
fi

out=$(fpmrun "$W" update 2>&1); rc=$?
[ $rc -eq 0 ] && record "fpm update (alias)" PASS "" \
    || record "fpm update (alias)" FAIL "rc=$rc"

out=$(fpmrun "$W" -info tree 2>&1); rc=$?
if [ $rc -eq 0 ] && expect_contains "$out" "test-lib"; then
    record "package metadata (fpm -info)" PASS "depends shown"
else
    record "package metadata (fpm -info)" FAIL "rc=$rc"
fi

# JSON-only index consumption
J="$SCRATCH/jsonw"
mkscenario "$J" "http://127.0.0.1:$JSON_PORT"
out=$(fpmrun "$J" -upd 2>&1); rc=$?
[ $rc -eq 0 ] && expect_contains "$out" "3 package(s)" \
    && record "index via packages.json" PASS "" \
    || record "index via packages.json" FAIL "rc=$rc"

# Corrupted index is rejected; the hardcoded reserved mirror then heals it.
G="$SCRATCH/garbw"
mkscenario "$G" "http://127.0.0.1:$GOOD_PORT"
out=$(fpmrun "$G" -upd 2>&1); rc=$?
set_mirrors "$G" "http://127.0.0.1:$GARBAGE_PORT"
out=$(fpmrun "$G" -upd 2>&1); rc=$?
if expect_contains "$out" "invalid index content" \
   && [ $rc -eq 0 ] && expect_contains "$out" "Repository index updated"; then
    record "corrupt index rejected + auto recovery" PASS ""
else
    record "corrupt index rejected + auto recovery" FAIL "rc=$rc :: $(echo "$out" | tail -5)"
fi

# Mirror fallback: connection refused first, working second
N="$SCRATCH/netw"
mkscenario "$N" "http://127.0.0.1:1" "http://127.0.0.1:$GOOD_PORT"
out=$(fpmrun "$N" -upd 2>&1); rc=$?
if [ $rc -eq 0 ] && expect_contains "$out" "connection failed" \
   && expect_contains "$out" "Repository index updated: 3 package(s)"; then
    record "mirror fallback (connection refused)" PASS ""
else
    record "mirror fallback (connection refused)" FAIL "rc=$rc :: $(echo "$out" | tail -4)"
fi

# Mirror fallback: HTTP 404 first, working second
F="$SCRATCH/404w"
mkscenario "$F" "http://127.0.0.1:$GOOD_PORT/nonexistent" "http://127.0.0.1:$GOOD_PORT"
out=$(fpmrun "$F" -upd 2>&1); rc=$?
if [ $rc -eq 0 ] && expect_contains "$out" "HTTP 404" \
   && expect_contains "$out" "Repository index updated"; then
    record "mirror fallback (HTTP 404)" PASS ""
else
    record "mirror fallback (HTTP 404)" FAIL "rc=$rc :: $(echo "$out" | tail -4)"
fi

# Mirror fallback: HTTP 500 first, working second
V="$SCRATCH/500w"
mkscenario "$V" "http://127.0.0.1:$ERR500_PORT" "http://127.0.0.1:$GOOD_PORT"
out=$(fpmrun "$V" -upd 2>&1); rc=$?
if [ $rc -eq 0 ] && expect_contains "$out" "HTTP 500" \
   && expect_contains "$out" "Repository index updated"; then
    record "mirror fallback (HTTP 500)" PASS ""
else
    record "mirror fallback (HTTP 500)" FAIL "rc=$rc :: $(echo "$out" | tail -4)"
fi

# Total network failure: the hardcoded reserved mirror auto-heals -upd.
X="$SCRATCH/netfail"
mkscenario "$X" "http://127.0.0.1:1"
out=$(fpmrun "$X" -upd 2>&1); rc=$?
index_count=$(echo "$out" | sed -n 's/.*Repository index updated: \([0-9][0-9]*\) package(s).*/\1/p' | head -1)
if [ $rc -eq 0 ] \
   && expect_contains "$out" "[WARN] Active mirror failed or unreachable! Falling back to https://geo.mirror.pkgbuild.com/" \
   && [ -n "$index_count" ] && [ "$index_count" -ge 10000 ]; then
    record "network failure (auto fallback)" PASS "$index_count packages"
else
    record "network failure (auto fallback)" FAIL "rc=$rc :: $(echo "$out" | tail -5)"
fi

# =========================================================
echo "== 4. Search / fuzzy search =="
# =========================================================

out=$(printf '0\n' | fpmrun "$W" -s tree 2>&1); rc=$?
if [ $rc -eq 0 ] && expect_contains "$out" "Directory tree viewer" \
   && expect_contains "$out" "results 1-1 of 1"; then
    record "search exact (fpm -s tree)" PASS ""
else
    record "search exact (fpm -s tree)" FAIL "rc=$rc :: $(echo "$out" | tail -3)"
fi

out=$(fpmrun "$W" -s tre 2>&1); rc=$?
[ $rc -eq 0 ] && expect_contains "$out" "tree" \
    && record "search prefix (tre)" PASS "" || record "search prefix (tre)" FAIL "rc=$rc"
out=$(fpmrun "$W" -s ree 2>&1); rc=$?
[ $rc -eq 0 ] && expect_contains "$out" "tree" \
    && record "search substring (ree)" PASS "" || record "search substring (ree)" FAIL "rc=$rc"
out=$(fpmrun "$W" -s TREE 2>&1); rc=$?
[ $rc -eq 0 ] && expect_contains "$out" "tree" \
    && record "search case-insensitive (TREE)" PASS "" || record "search case-insensitive (TREE)" FAIL "rc=$rc"
out=$(fpmrun "$W" -fs tred 2>&1); rc=$?
[ $rc -eq 0 ] && expect_contains "$out" "tree" \
    && record "fuzzy search (-fs tred)" PASS "" || record "fuzzy search (-fs tred)" FAIL "rc=$rc"
out=$(fpmrun "$W" -fs zzzzzz 2>&1); rc=$?
if [ $rc -eq 0 ] && expect_contains "$out" "No packages found"; then
    record "fuzzy search no result (zzzzzz)" PASS ""
else
    record "fuzzy search no result (zzzzzz)" FAIL "rc=$rc :: $(echo "$out" | head -3)"
fi
out=$(fpmrun "$W" -s zzznothing 2>&1); rc=$?
[ $rc -eq 0 ] && expect_contains "$out" "No packages found" \
    && record "search no result" PASS "" || record "search no result" FAIL "rc=$rc"
fpmrun "$W" -s "" >/dev/null 2>&1; rc=$?
record "empty query rejected" "$([ $rc -ne 0 ] && echo PASS || echo FAIL)" "rc=$rc"

out=$(fpmrun "$W" -s Directory 2>&1); rc=$?
if [ $rc -eq 0 ] && expect_contains "$out" "No packages found" \
   && ! expect_contains "$out" "tree"; then
    record "search name-only (-s ignores description)" PASS ""
else
    record "search name-only (-s ignores description)" FAIL "rc=$rc :: $(echo "$out" | head -2)"
fi
out=$(fpmrun "$W" -fs Directory 2>&1); rc=$?
[ $rc -eq 0 ] && expect_contains "$out" "tree" \
    && record "deep search (-fs matches description)" PASS "" \
    || record "deep search (-fs matches description)" FAIL "rc=$rc :: $(echo "$out" | head -2)"
out=$(fpmrun "$W" -fs tess 2>&1); rc=$?
[ $rc -eq 0 ] && expect_contains "$out" "test-lib" \
    && expect_contains "$out" "results 1-1 of 1" \
    && record "fuzzy prefix (-fs tess -> test-lib)" PASS "" \
    || record "fuzzy prefix (-fs tess -> test-lib)" FAIL "rc=$rc :: $(echo "$out" | head -2)"

# =========================================================
echo "== 5. Mirror selector / mirror management =="
# =========================================================

out=$(printf '1\n' | fpmrun "$W" -ms 2>&1); rc=$?
if [ $rc -eq 0 ] && expect_contains "$out" "INDEX OK" \
   && expect_contains "$out" "Preferred mirror set to: http://127.0.0.1:$GOOD_PORT"; then
    record "mirror selector (fpm -ms)" PASS ""
else
    record "mirror selector (fpm -ms)" FAIL "rc=$rc :: $(echo "$out" | tail -5)"
fi

# -ms reads the candidates.list pool, and re-running -upd must still work after
# the active mirrors.list was rewritten to the single selected mirror.
out=$(fpmrun "$W" -upd 2>&1); rc=$?
[ $rc -eq 0 ] && expect_contains "$out" "3 package(s)" \
    && record "update works after -ms rewrite" PASS "" \
    || record "update works after -ms rewrite" FAIL "rc=$rc"

# -ms falls back to mirrors.list when candidates.list is absent (fresh system)
MC="$SCRATCH/mscand"
mkscenario "$MC" "http://127.0.0.1:1"
cp "$MC/home/fpm/etc/mirrors.list" "$MC/home/fpm/etc/candidates.list"
printf '%s\n' "# active (broken)" \
             "http://127.0.0.1:1 | Local | http | 0" > "$MC/home/fpm/etc/mirrors.list"
printf '%s\n' "http://127.0.0.1:$GOOD_PORT | Local | http | 0" >> "$MC/home/fpm/etc/candidates.list"
out=$(printf '1\n' | fpmrun "$MC" -ms 2>&1); rc=$?
if [ $rc -eq 0 ] && expect_contains "$out" "INDEX OK" \
   && grep -q "$GOOD_PORT" "$MC/home/fpm/etc/mirrors.list"; then
    record "mirror selector from candidates.list (-ms)" PASS ""
else
    record "mirror selector from candidates.list (-ms)" FAIL "rc=$rc :: $(echo "$out" | tail -5)"
fi

# -am: add a mirror; goes to candidates.list by default, mirrors.list with --active
AM="$SCRATCH/amm"
mkscenario "$AM"
: > "$AM/home/fpm/etc/mirrors.list"
out=$(fpmrun "$AM" -am "http://127.0.0.1:$GOOD_PORT" 2>&1); rc=$?
if [ $rc -eq 0 ] && expect_contains "$out" "Mirror added" \
   && grep -q "127.0.0.1:$GOOD_PORT" "$AM/home/fpm/etc/candidates.list" \
   && ! grep -q "127.0.0.1:$GOOD_PORT" "$AM/home/fpm/etc/mirrors.list"; then
    record "add mirror to candidates.list (fpm -am)" PASS ""
else
    record "add mirror to candidates.list (fpm -am)" FAIL "rc=$rc :: $(echo "$out" | tail -4)"
fi
out=$(fpmrun "$AM" -am --active "http://127.0.0.1:$GOOD_PORT" 2>&1); rc=$?
if [ $rc -eq 0 ] && expect_contains "$out" "Mirror added" \
   && grep -q "127.0.0.1:$GOOD_PORT" "$AM/home/fpm/etc/mirrors.list"; then
    record "add mirror as active (fpm -am --active)" PASS ""
else
    record "add mirror as active (fpm -am --active)" FAIL "rc=$rc :: $(echo "$out" | tail -4)"
fi

# Unreachable mirror is rejected by -am
out=$(fpmrun "$AM" -am "http://127.0.0.1:1" 2>&1); rc=$?
if [ $rc -ne 0 ] && expect_contains "$out" "unreachable"; then
    record "add mirror rejects dead URL (fpm -am)" PASS ""
else
    record "add mirror rejects dead URL (fpm -am)" FAIL "rc=$rc"
fi

# Mirror selector on a fully dead pool (active list only) still fails
set_mirrors "$X" "http://127.0.0.1:1"
out=$(fpmrun "$X" -ms 2>&1); rc=$?
[ $rc -ne 0 ] && record "mirror selector (all dead)" PASS "" \
    || record "mirror selector (all dead)" FAIL "should fail on dead mirror"

# =========================================================
echo "== 6. Install (root) =="
# =========================================================

out=$(fpmrun_root "$W" "-" -upd 2>&1); rc=$?
if [ "$RDRY" = "1" ] && [ $rc -eq 0 ] && expect_contains "$out" "Repository index updated: 3 package(s)"; then
    record "sudo fpm -upd" PASS ""
else
    record "sudo fpm -upd" "$([ "$RDRY" = "1" ] && echo FAIL || echo BLOCKED)" "rc=$rc"
fi

out=$(fpmrun "$W" -i tree 2>&1); rc=$?
if [ $rc -ne 0 ] && expect_contains "$out" "requires root"; then
    record "perm denied without root" PASS ""
else
    record "perm denied without root" "$([ "$RDRY" = "1" ] && echo FAIL || echo BLOCKED)" "rc=$rc :: $(echo "$out" | head -2)"
fi

out=$(fpmrun_root "$W" "y" -i tree 2>&1); rc=$?
if [ "$RDRY" = "1" ] && [ $rc -eq 0 ]; then
    DB_OK=$(sudox test -f /var/lib/fpm/local/tree/meta && echo yes || echo no)
    TESTLIB_OK=$(sudox test -f /var/lib/fpm/local/test-lib/meta && echo yes || echo no)
    BIN_OK=$(sudox test -x /usr/bin/fpm-tree && echo yes || echo no)
    SYSTEM_LEFT=$(sudox test -f /usr/bin/tree && echo "real /usr/bin/tree present" || echo "no real tree touched")
    if [ "$DB_OK" = "yes" ] && [ "$TESTLIB_OK" = "yes" ] && [ "$BIN_OK" = "yes" ]; then
        record "sudo fpm -i tree (dep resolution)" PASS "tree+test-lib installed; ${SYSTEM_LEFT}"
    else
        record "sudo fpm -i tree (dep resolution)" FAIL "db=$DB_OK lib=$TESTLIB_OK bin=$BIN_OK"
    fi
else
    record "sudo fpm -i tree (dep resolution)" "$([ "$RDRY" = "1" ] && echo FAIL || echo BLOCKED)" "rc=$rc"
fi

out=$(fpmrun_root "$W" "-" -l 2>&1); rc=$?
if [ "$RDRY" = "1" ] && [ $rc -eq 0 ] && expect_contains "$out" "tree" && expect_contains "$out" "test-lib"; then
    record "local DB (fpm -l)" PASS ""
else
    record "local DB (fpm -l)" "$([ "$RDRY" = "1" ] && echo FAIL || echo BLOCKED)" "rc=$rc"
fi

out=$(fpmrun_root "$W" "-" -qs tree 2>&1); rc=$?
if [ "$RDRY" = "1" ] && [ $rc -eq 0 ] && expect_contains "$out" "tree" \
   && ! expect_contains "$out" "test-lib"; then
    record "installed search (fpm -qs)" PASS ""
else
    record "installed search (fpm -qs)" "$([ "$RDRY" = "1" ] && echo FAIL || echo BLOCKED)" "rc=$rc"
fi

out=$(fpmrun_root "$W" "-" -v tree 2>&1); rc=$?
if [ "$RDRY" = "1" ] && [ $rc -eq 0 ] && expect_contains "$out" "verified: OK"; then
    record "package verification (fpm -v)" PASS ""
else
    record "package verification (fpm -v)" "$([ "$RDRY" = "1" ] && echo FAIL || echo BLOCKED)" "rc=$rc"
fi

out=$(fpmrun_root "$W" "-" -info tree 2>&1); rc=$?
if [ "$RDRY" = "1" ] && [ $rc -eq 0 ] && expect_contains "$out" "fpm-test"; then
    record "installed package info" PASS ""
else
    record "installed package info" "$([ "$RDRY" = "1" ] && echo FAIL || echo BLOCKED)" "rc=$rc"
fi

out=$(fpmrun_root "$W" "y" -pkg "$REPO/fpm-hello-1.0.0.x86_64.fpm" 2>&1); rc=$?
HELLO_OK=$(sudox test -x /usr/bin/fpm-hello && echo yes || echo no)
if [ "$RDRY" = "1" ] && [ $rc -eq 0 ] && [ "$HELLO_OK" = "yes" ]; then
    record "local .fpm install (fpm -pkg)" PASS ""
else
    record "local .fpm install (fpm -pkg)" "$([ "$RDRY" = "1" ] && echo FAIL || echo BLOCKED)" "rc=$rc"
fi

# bad-checksum repository: index downloads but package hash does not match
B="$SCRATCH/badw"
mkscenario "$B" "http://127.0.0.1:$BADSHA_PORT"
# clean slate so only the bad repo can be the source of the install
if [ "$RDRY" = "1" ]; then
    sudox sh -c 'rm -rf /var/lib/fpm/local/tree /var/lib/fpm/local/test-lib \
                 /usr/bin/fpm-tree /usr/bin/fpm-hello /usr/share/fpm-test'
fi
out=$(fpmrun_root "$B" "-" -upd 2>&1); rc=$?
if [ "$RDRY" = "1" ] && [ $rc -eq 0 ] && expect_contains "$out" "3 package(s)"; then
    record "badsha index loads (bad checksum)" PASS ""
else
    record "badsha index loads (bad checksum)" "$([ "$RDRY" = "1" ] && echo FAIL || echo BLOCKED)" "rc=$rc"
fi
out=$(fpmrun_root "$B" "y" -i tree 2>&1); rc=$?
ALREADY=$(sudox test -d /var/lib/fpm/local/tree && echo installed || echo not-installed)
if [ "$RDRY" = "1" ] && [ $rc -ne 0 ] \
   && expect_contains "$out" "SHA256 mismatch" && [ "$ALREADY" = "not-installed" ]; then
    record "checksum mismatch blocks install" PASS ""
else
    record "checksum mismatch blocks install" "$([ "$RDRY" = "1" ] && echo FAIL || echo BLOCKED)" "rc=$rc :: $ALREADY"
fi

if [ "$RDRY" = "1" ]; then
    out=$(sudox env HOME="$W/home" sh -c "cd '$W' && '$FPM' -upd" 2>&1)
    expect_contains "$out" "Repository index updated" \
        && record "restore root index" PASS "" \
        || record "restore root index" FAIL ""
fi

# =========================================================
echo "== 7. CleanMyDisk (fpm -cmd) =="
# =========================================================

mkdir -p /tmp/fpm_testjunk && dd if=/dev/zero of=/tmp/fpm_testjunk/junk.bin bs=1024 count=16 2>/dev/null

out=$(fpmrun "$W" -cmd 2>&1); rc=$?
if [ $rc -eq 0 ] && expect_contains "$out" "Cache cleaned:" \
   && expect_contains "$out" "Temporary cleaned:" && expect_contains "$out" "Total freed:"; then
    record "clean cache (non-root fpm -cmd)" PASS ""
else
    record "clean cache (non-root fpm -cmd)" FAIL "rc=$rc :: $(echo "$out" | tail -6)"
fi

if [ "$RDRY" = "1" ]; then
    PKG_LEFT=$(sudox sh -c 'ls /var/cache/fpm/packages 2>/dev/null | wc -l')
    out=$(fpmrun_root "$W" "-" -cmd 2>&1); rc=$?
    if [ $rc -eq 0 ] && expect_contains "$out" "Cache cleaned:" \
       && expect_contains "$out" "Temporary cleaned:" \
       && [ ! -e /tmp/fpm_testjunk ]; then
        record "CleanMyDisk (root fpm -cmd)" PASS "pkg cache files left: ${PKG_LEFT}"
    else
        record "CleanMyDisk (root fpm -cmd)" FAIL "rc=$rc; junk=$([ ! -e /tmp/fpm_testjunk ] && echo removed || echo remains)"
    fi
else
    record "CleanMyDisk (root fpm -cmd)" BLOCKED "no sudo"
fi

if [ -d "$W/fpm_cache/packages" ] && [ -z "$(ls -A "$W/fpm_cache/packages" 2>/dev/null)" ]; then
    record "cache dir preserved (contents empty)" PASS ""
else
    record "cache dir preserved (contents empty)" FAIL
fi

# =========================================================
echo "== 8. Remove installed test packages (root) =="
# =========================================================

if [ "$RDRY" = "1" ]; then
    out=$(fpmrun_root "$W" "y" -r tree test-lib fpm-hello 2>&1); rc=$?
    LEFT=$(sudox sh -c 'for p in tree test-lib fpm-hello; do [ -e "/var/lib/fpm/local/$p" ] && echo x; done | wc -l')
    if [ $rc -eq 0 ] && [ "$LEFT" = "0" ]; then
        record "remove installed packages" PASS ""
    else
        record "remove installed packages" FAIL "rc=$rc test-left=$LEFT"
    fi
else
    record "remove installed packages" BLOCKED "no sudo"
fi

# =========================================================
echo
echo "== SUMMARY =="
echo
test -t 1 && SINK=/dev/stderr || SINK=/dev/null
{
    printf '%-30s | %-7s | %s\n' "TEST" "RESULT" "DETAILS"
    echo "------------------------------- | ------- | ----------"
    sort "$RESULTS"
} | tee "$SINK"
echo
echo "PASS=$PASS FAIL=$FAIL BLOCKED=$BLOCKED"
exit $(( FAIL > 0 ? 1 : 0 ))