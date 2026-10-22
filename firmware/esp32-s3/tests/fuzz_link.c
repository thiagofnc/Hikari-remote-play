// libFuzzer harness for the wired-link parser that runs on the bridge.
//
//   clang -g -O1 -fsanitize=fuzzer,address -I../../../shared/protocol \
//         fuzz_link.c ../../../shared/protocol/hk_link.c -o fuzz_link
//   ./fuzz_link -max_total_time=300
//
// The same parser is exercised natively by tests/test_link.cpp in CI.

#include <stddef.h>
#include <stdint.h>

#include "hk_link.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    hk_link_decoder d;
    hk_link_decoder_init(&d);
    uint8_t scratch[HK_LINK_MAX_FRAME];
    for (size_t i = 0; i < size; ++i) {
        hk_link_frame f;
        if (!hk_link_decoder_push(&d, data[i], scratch, sizeof(scratch), &f)) continue;
        hk_pad_state p;
        hk_hello h;
        hk_ping pi;
        hk_wake w;
        hk_telemetry t;
        hk_pad_decode(f.payload, f.len, &p);
        hk_hello_decode(f.payload, f.len, &h);
        hk_ping_decode(f.payload, f.len, &pi);
        hk_wake_decode(f.payload, f.len, &w);
        hk_telemetry_decode(f.payload, f.len, &t);
    }
    return 0;
}
