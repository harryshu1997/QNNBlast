package com.qblast.tuner;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.util.Log;

public class TuneBroadcastReceiver extends BroadcastReceiver {
    private static final String TAG = "qblast_rx";

    @Override
    public void onReceive(Context context, Intent intent) {
        int cfgId = intent.getIntExtra(TunerService.EXTRA_CFG_ID, -1);
        String shape = intent.getStringExtra(TunerService.EXTRA_SHAPE);
        Log.i(TAG, "broadcast: cfg_id=" + cfgId + " shape=" + shape);

        Intent svc = new Intent(context, TunerService.class);
        svc.setAction(TunerService.ACTION_TUNE);
        svc.putExtra(TunerService.EXTRA_CFG_ID, cfgId);
        svc.putExtra(TunerService.EXTRA_SHAPE, shape);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            context.startForegroundService(svc);
        } else {
            context.startService(svc);
        }
    }
}
