#!/bin/sh
# builds ring_test from the gameplay audio ring as it is in src/main.c right
# now (defines, variables, core_audio_ring_fill, audio_sample_batch_cb,
# core_audio_thread_func) and runs it plain and under tsan
set -e
cd "$(dirname "$0")"
mkdir -p build/ring

python3 - ../../src/main.c build/ring/ring_extract.h <<'EOF'
import re, sys

src = open(sys.argv[1]).read().split('\n')
out = []

def grab_line(pattern):
    for l in src:
        if re.match(pattern, l):
            out.append(l)
            return
    sys.exit('missing ' + pattern)

def grab_func(name):
    for i, l in enumerate(src):
        if re.match(r'^static .*\b' + name + r'\(', l):
            j = i
            while src[j] != '}':
                j += 1
            out.extend(src[i:j + 1])
            out.append('')
            return
    sys.exit('missing function ' + name)

grab_line(r'#define AUDIO_PORT_GRAIN ')
grab_line(r'#define CORE_AUDIO_RING_GRAINS ')
grab_line(r'#define CORE_AUDIO_RING_FRAMES ')
grab_line(r'#define CORE_AUDIO_RING_WRAP ')
grab_line(r'static int16_t +core_audio_ring\[')
grab_line(r'static volatile unsigned +core_audio_ring_write')
grab_line(r'static volatile unsigned +core_audio_ring_read')
grab_line(r'static volatile bool +core_audio_thread_run')
grab_line(r'static SceUID +core_audio_sema')
for f in ('core_audio_ring_fill', 'audio_sample_batch_cb', 'core_audio_thread_func'):
    grab_func(f)

open(sys.argv[2], 'w').write('\n'.join(out) + '\n')
EOF

cc -O1 -g -Wall -Wno-unused-function -Ibuild/ring ring_test.c -lpthread -o build/ring/ring_test
cc -O1 -g -Wall -Wno-unused-function -fsanitize=thread -Ibuild/ring ring_test.c -lpthread -o build/ring/ring_test_tsan

./build/ring/ring_test realtime 8
./build/ring/ring_test stress 4
./build/ring/ring_test_tsan realtime 4
./build/ring/ring_test_tsan stress 4
