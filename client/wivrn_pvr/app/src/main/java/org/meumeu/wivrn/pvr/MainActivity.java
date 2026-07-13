package org.meumeu.wivrn.pvr;

import android.app.NativeActivity;
import android.os.Bundle;
import android.util.Log;

public class MainActivity extends NativeActivity {
    private static final String TAG = "WiVRn-PVR";

    static {
        System.loadLibrary("wivrn_pvr");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i(TAG, "NativeActivity created");
    }
}
