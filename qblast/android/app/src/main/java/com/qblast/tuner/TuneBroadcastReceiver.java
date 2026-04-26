package com.qblast.tuner;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

// Receives `am broadcast -a com.qblast.TUNE` from the host tuner driver.
//
// Shape-string dispatch:
//   "ping"              -> qblast_hello roundtrip (sanity check FastRPC plumbing)
//   "<M>_<K>_<q_block>" -> gemv_w4a16 baseline; allocates rpcmem buffers,
//                          generates seeded test data, runs on cDSP, validates
//                          against an FP32 reference. Default seed=1234, override
//                          via --ei seed N on the broadcast.
//
// All metrics (dsp_cycles, arm_us, max_rel_err) are emitted by the JNI layer
// via __android_log_print. The receiver only logs the call dispatch + return
// code; the host tuner driver greps logcat for the qblast_jni lines.
public class TuneBroadcastReceiver extends BroadcastReceiver {
    private static final String TAG = "qblast_rx";
    private static final String DSP_LIBRARY_PATH = "/data/local/tmp";

    @Override
    public void onReceive(Context context, Intent intent) {
        int cfgId = intent.getIntExtra(TunerService.EXTRA_CFG_ID, -1);
        String shape = intent.getStringExtra(TunerService.EXTRA_SHAPE);
        int seed = intent.getIntExtra("seed", 1234);
        Log.i(TAG, "broadcast: cfg_id=" + cfgId + " shape=" + shape + " seed=" + seed);

        // Touching TunerService triggers its static initializer (loadLibrary).
        TunerService.nativeInit(DSP_LIBRARY_PATH);

        if ("ping".equals(shape)) {
            long t0 = System.nanoTime();
            long magic;
            try {
                magic = TunerService.nativePing();
            } catch (Throwable t) {
                Log.e(TAG, "nativePing threw", t);
                magic = -100;
            }
            long rttUs = (System.nanoTime() - t0) / 1000;
            Log.i(TAG, "TUNE cfg_id=" + cfgId + " ping_magic=" + magic
                    + " java_rtt_us=" + rttUs);
            return;
        }

        // Otherwise expect "M_K_q" (decimal). Anything else logs a parse error.
        String[] parts = shape == null ? new String[0] : shape.split("_");
        if (parts.length != 3) {
            Log.e(TAG, "shape must be 'ping' or 'M_K_q', got: " + shape);
            return;
        }
        int M, K, q;
        try {
            M = Integer.parseInt(parts[0]);
            K = Integer.parseInt(parts[1]);
            q = Integer.parseInt(parts[2]);
        } catch (NumberFormatException e) {
            Log.e(TAG, "shape parse failed: " + shape, e);
            return;
        }

        long t0 = System.nanoTime();
        int rc;
        try {
            rc = TunerService.nativeRunGemv(M, K, q, seed);
        } catch (Throwable t) {
            Log.e(TAG, "nativeRunGemv threw", t);
            rc = -200;
        }
        long rttUs = (System.nanoTime() - t0) / 1000;
        Log.i(TAG, "TUNE cfg_id=" + cfgId + " gemv M=" + M + " K=" + K + " q=" + q
                + " rc=" + rc + " java_rtt_us=" + rttUs);
    }
}
