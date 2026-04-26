package com.qblast.tuner;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

import java.io.File;

// Receives `am broadcast -a com.qblast.TUNE` from the host tuner driver.
//
// Shape-string dispatch:
//   "ping"              -> qblast_hello roundtrip (sanity check FastRPC plumbing)
//   "<M>_<K>_<q_block>" -> gemv_w4a16 baseline; allocates rpcmem buffers,
//                          generates seeded test data, runs warmup + iters reps,
//                          validates against an FP32 reference, writes
//                          <externalFilesDir>/results/<cfg_id>.json.
//
// Intent extras:
//   cfg_id  int     identifier; also names the result JSON file
//   shape   string  see above
//   seed    int     PRNG seed for test data (default 1234)
//   warmup  int     discarded reps before timing (default 5)
//   iters   int     timed reps; median reported (default 50)
public class TuneBroadcastReceiver extends BroadcastReceiver {
    private static final String TAG = "qblast_rx";
    private static final String DSP_LIBRARY_PATH = "/data/local/tmp";

    @Override
    public void onReceive(Context context, Intent intent) {
        int cfgId = intent.getIntExtra(TunerService.EXTRA_CFG_ID, -1);
        String shape = intent.getStringExtra(TunerService.EXTRA_SHAPE);
        int seed = intent.getIntExtra("seed", 1234);
        int warmup = intent.getIntExtra("warmup", 5);
        int iters = intent.getIntExtra("iters", 50);
        Log.i(TAG, "broadcast: cfg_id=" + cfgId + " shape=" + shape
                + " seed=" + seed + " warmup=" + warmup + " iters=" + iters);

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

        // App-specific external storage — no permission needed, adb-pullable
        // from /sdcard/Android/data/com.qblast.tuner/files/results/.
        File extDir = context.getExternalFilesDir(null);
        if (extDir == null) {
            Log.e(TAG, "getExternalFilesDir returned null");
            return;
        }
        String resultsDir = new File(extDir, "results").getAbsolutePath();

        long t0 = System.nanoTime();
        int rc;
        try {
            rc = TunerService.nativeRunGemv(cfgId, M, K, q, seed, warmup, iters, resultsDir);
        } catch (Throwable t) {
            Log.e(TAG, "nativeRunGemv threw", t);
            rc = -200;
        }
        long rttUs = (System.nanoTime() - t0) / 1000;
        Log.i(TAG, "TUNE cfg_id=" + cfgId + " gemv M=" + M + " K=" + K + " q=" + q
                + " rc=" + rc + " java_rtt_us=" + rttUs);
    }
}
