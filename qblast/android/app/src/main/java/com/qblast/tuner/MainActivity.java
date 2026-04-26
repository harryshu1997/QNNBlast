package com.qblast.tuner;

import androidx.appcompat.app.AppCompatActivity;

import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;

// MainActivity holds no native state — TunerService owns the JNI library and
// FastRPC lifecycle. This activity exists only to give a tap-to-trigger UI for
// manual smoke tests; the real tuner control surface is the
// `com.qblast.TUNE` broadcast receiver.

public class MainActivity extends AppCompatActivity {
    private static final String TAG = "qblast";

    private TextView statusView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

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

        Log.i(TAG, "MainActivity.onCreate");
    }
}
