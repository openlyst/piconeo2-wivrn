package com.pico.openxrpoc;

public class MainActivity extends android.app.NativeActivity {
    static {
        System.loadLibrary("openxr_loader");
        System.loadLibrary("openxrpoc");
    }
}
