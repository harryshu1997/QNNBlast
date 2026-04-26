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

    @Override
    public void onCreate() {
        super.onCreate();
        ensureChannel();
        startForeground(NOTIFICATION_ID, buildNotification("Idle"));
        Log.i(TAG, "TunerService.onCreate");
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_TUNE.equals(intent.getAction())) {
            int cfgId = intent.getIntExtra(EXTRA_CFG_ID, -1);
            String shape = intent.getStringExtra(EXTRA_SHAPE);
            Log.i(TAG, "TUNE cfg_id=" + cfgId + " shape=" + shape);

            // Phase-1 stub: log only. Week 2 will dispatch JNI runner here:
            //   long elapsed = TunerJni.runVariant(cfgId, shape, ...);
            //   ResultLogger.write(cfgId, shape, elapsed, validationError);
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
