package com.qblast.tuner;

import androidx.appcompat.app.AppCompatActivity;

import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;

public class MainActivity extends AppCompatActivity {
    private static final String TAG = "qblast";

    static {
        System.loadLibrary("qblast_tuner_jni");
    }

    private TextView statusView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        // DSP skel libraries (libgemv_v*.so) are pushed to /data/local/tmp/ by the host
        // tuner_driver. fastrpc loader looks them up via DSP_LIBRARY_PATH.
        init("/data/local/tmp");

        statusView = findViewById(R.id.statusView);
        statusView.setText(getString(R.string.status_default));

        Button trigger = findViewById(R.id.triggerButton);
        trigger.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                Intent svc = new Intent(MainActivity.this, TunerService.class);
                svc.setAction(TunerService.ACTION_TUNE);
                svc.putExtra(TunerService.EXTRA_CFG_ID, 0);
                svc.putExtra(TunerService.EXTRA_SHAPE, "manual_smoke");
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    startForegroundService(svc);
                } else {
                    startService(svc);
                }
                statusView.setText("manual trigger: cfg_id=0 shape=manual_smoke");
            }
        });

        Log.i(TAG, "MainActivity.onCreate complete");
    }

    public native void init(String location);
}
