// DSP-side implementation of the qblast_hello FastRPC interface.
// Runs on Hexagon (v81 baseline), no HVX/HMX/VTCM yet — this exists only to
// validate the IDL -> stub/skel -> APK -> FastRPC -> cDSP -> PD roundtrip.

#include <stdlib.h>
#include "HAP_farf.h"
#include "HAP_perf.h"
#include "qblast_hello.h"

int qblast_hello_open(const char* uri, remote_handle64* handle) {
    void* tptr = malloc(1);
    if (tptr == NULL) {
        return -1;
    }
    *handle = (remote_handle64)tptr;
    return 0;
}

int qblast_hello_close(remote_handle64 handle) {
    if (handle) {
        free((void*)handle);
    }
    return 0;
}

int qblast_hello_ping(remote_handle64 h, int64* magic, uint64* dsp_cycles) {
    uint64 t0 = HAP_perf_get_pcycles();
    *magic = 4950;  // sum of 0..99
    uint64 t1 = HAP_perf_get_pcycles();
    *dsp_cycles = t1 - t0;
    FARF(HIGH, "qblast_hello_ping: magic=%lld pcycles=%llu",
         (long long)*magic, (unsigned long long)*dsp_cycles);
    return 0;
}
