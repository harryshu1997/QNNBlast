package com.qblast.tuner;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

// Receives `am broadcast -a com.qblast.TUNE` from the host tuner driver.
//
// On Android 12+, calling startForegroundService() from a background-triggered
// broadcast throws ForegroundServiceStartNotAllowedException. Solution: do the
// JNI work *inside* onReceive(). Receivers get ~10 s before ANR, FastRPC ping
// is microseconds, so this is comfortable for Phase 1.
//
// TunerService still exists for the MainActivity-button path (UI is in
// foreground there, so startForegroundService is allowed). When Phase 2 needs
// a long-lived JNI session across many tuning iterations, revisit this.
public class TuneBroadcastReceiver extends BroadcastReceiver {
    private static final String TAG = "qblast_rx";
    private static final String DSP_LIBRARY_PATH = "/data/local/tmp";

    @Override
    public void onReceive(Context context, Intent intent) {
        int cfgId = intent.getIntExtra(TunerService.EXTRA_CFG_ID, -1);
        String shape = intent.getStringExtra(TunerService.EXTRA_SHAPE);
        Log.i(TAG, "broadcast: cfg_id=" + cfgId + " shape=" + shape);

        // Both nativeInit and nativePing live on TunerService as static methods.
        // Touching the class triggers TunerService's static initializer, which
        // does System.loadLibrary("qblast_tuner_jni") exactly once per process.
        TunerService.nativeInit(DSP_LIBRARY_PATH);

        long t0 = System.nanoTime();
        long magic;
        try {
            magic = TunerService.nativePing();
        } catch (Throwable t) {
            Log.e(TAG, "nativePing threw", t);
            magic = -2;
        }
        long rttUs = (System.nanoTime() - t0) / 1000;

        Log.i(TAG, "TUNE cfg_id=" + cfgId + " shape=" + shape
                + " ping_magic=" + magic + " java_rtt_us=" + rttUs);
    }
}
