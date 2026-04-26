package com.qblast.tuner;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

public class TunerService extends Service {
    public static final String ACTION_TUNE = "com.qblast.tuner.action.TUNE";
    public static final String EXTRA_CFG_ID = "cfg_id";
    public static final String EXTRA_SHAPE = "shape";

    private static final String TAG = "qblast_svc";
    private static final String CHANNEL_ID = "qblast_tuner";
    private static final int NOTIFICATION_ID = 1;
    private static final String DSP_LIBRARY_PATH = "/data/local/tmp";

    static {
        // libqblast_tuner_jni.so is built by AGP/CMake and packaged into the APK
        // under lib/arm64-v8a/. Loading it pulls in libcdsprpc.so (dynamic dep)
        // and our qaic-generated qblast_hello_stub.c.
        System.loadLibrary("qblast_tuner_jni");
    }

    public static native void nativeInit(String dspLibraryPath);
    public static native long nativePing();

    @Override
    public void onCreate() {
        super.onCreate();
        ensureChannel();
        startForeground(NOTIFICATION_ID, buildNotification("Idle"));

        // Set DSP_LIBRARY_PATH so fastrpc finds /data/local/tmp/libqblast_hello_skel.so
        // (and future libgemv_v*.so variants).
        nativeInit(DSP_LIBRARY_PATH);

        Log.i(TAG, "TunerService.onCreate");
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_TUNE.equals(intent.getAction())) {
            int cfgId = intent.getIntExtra(EXTRA_CFG_ID, -1);
            String shape = intent.getStringExtra(EXTRA_SHAPE);

            long t0 = System.nanoTime();
            long magic;
            try {
                magic = nativePing();
            } catch (Throwable t) {
                Log.e(TAG, "nativePing threw", t);
                magic = -2;
            }
            long rttUs = (System.nanoTime() - t0) / 1000;

            Log.i(TAG, "TUNE cfg_id=" + cfgId + " shape=" + shape
                    + " ping_magic=" + magic + " java_rtt_us=" + rttUs);
        } else {
            Log.w(TAG, "onStartCommand: unexpected intent " + intent);
        }
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void ensureChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel ch = new NotificationChannel(
                    CHANNEL_ID, "QBlast Tuner",
                    NotificationManager.IMPORTANCE_LOW);
            NotificationManager nm = getSystemService(NotificationManager.class);
            if (nm != null) {
                nm.createNotificationChannel(ch);
            }
        }
    }

    private Notification buildNotification(String text) {
        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            b = new Notification.Builder(this, CHANNEL_ID);
        } else {
            b = new Notification.Builder(this);
        }
        return b.setContentTitle("QBlast Tuner")
                .setContentText(text)
                .setSmallIcon(android.R.drawable.stat_notify_sync)
                .build();
    }
}
