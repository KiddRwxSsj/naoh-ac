#!/bin/sh
# builds the linux harness out of the real src/ files, without ever
# writing into src/. usage: ./build.sh [plain|asan|tsan]
#
# netplay.c gets two edits on the way: the log path comes from $NP_LOG
# (default /dev/null) and the topic root moves to a test one, so real
# players never see harness rooms
set -e

VARIANT=${1:-asan}

# paths with spaces break the unquoted bits below, so everything from here
# on is relative to the harness folder
cd "$(dirname "$0")"
HERE=.
SRC=../../src
OUT=build/$VARIANT
TOPIC=${NP_TOPIC:-naohac/selftest3}

case "$VARIANT" in
   plain) SAN="" ;;
   asan)  SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined" ;;
   tsan)  SAN="-fsanitize=thread" ;;
   *) echo "unknown variant $VARIANT"; exit 2 ;;
esac

CFLAGS="-O1 -g -fno-omit-frame-pointer $SAN"
MBEDTLS_DEFS="-I$SRC/deps/mbedtls/include -I$SRC/deps/mbedtls -DMBEDTLS_USER_CONFIG_FILE=\"naohac_mbedtls_config.h\""

mkdir -p "$OUT/src" "$OUT/mbedtls"

# mbedtls only gets rebuilt when its archive is missing
if [ ! -f "$OUT/libmbedtls.a" ]; then
   echo "building host mbedtls ($VARIANT)..."
   export CFLAGS MBEDTLS_DEFS OUT
   ls "$SRC"/deps/mbedtls/library/*.c | xargs -P "$(nproc)" -n 1 sh -c \
      'cc $CFLAGS $MBEDTLS_DEFS -c "$1" -o "$OUT/mbedtls/$(basename "$1" .c).o"' sh
   ar rcs "$OUT/libmbedtls.a" "$OUT"/mbedtls/*.o
fi

cp "$SRC"/netplay.h "$SRC"/mqtt.h "$SRC"/rng.h "$SRC"/lobby_ca.h "$SRC"/mqtt.c "$SRC"/rng.c "$OUT/src/"
sed -e 's|^#define NET_LOG_PATH .*|#define NET_LOG_PATH (getenv("NP_LOG") ? getenv("NP_LOG") : "/dev/null")|' \
    -e 's|^#define TOPIC_ROOT .*|#define TOPIC_ROOT "'"$TOPIC"'"|' \
    "$SRC/netplay.c" > "$OUT/src/netplay.c"

grep -q "getenv(\"NP_LOG\")" "$OUT/src/netplay.c"
grep -q "\"$TOPIC\"" "$OUT/src/netplay.c"

cc $CFLAGS -std=gnu11 -Wall -Wno-unused-parameter -I"$HERE/inc" -I"$SRC" $MBEDTLS_DEFS \
   "$OUT/src/netplay.c" "$OUT/src/mqtt.c" "$OUT/src/rng.c" \
   "$SRC/deps/monocypher/monocypher.c" "$HERE/stubs.c" "$HERE/driver.c" \
   -I"$OUT/src" "$OUT/libmbedtls.a" -lpthread -o "$OUT/driver"

echo "built $OUT/driver (topic root $TOPIC)"
