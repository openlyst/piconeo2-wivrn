package com.pico.openxrpoc;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;

public class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "OpenXRPoc";
    private SurfaceView surfaceView;
    private long nativeHandle = 0;

    static {
        System.loadLibrary("openxrpoc");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);
        setContentView(surfaceView);
    }

    // Called by Pico OpenXR runtime via JNI to get the SurfaceHolder
    public SurfaceHolder waitGetSurfaceHolder(int timeout) {
        return surfaceView.getHolder();
    }

    // Called by Pico compositor to take the surface from SurfaceView for VR presentation
    public void NativeCallTakeSurfaceFromSurfaceView(android.view.SurfaceView sv) {
        Log.i(TAG, "NativeCallTakeSurfaceFromSurfaceView");
    }

    // Called by Pico compositor as fallback to take surface from Activity
    public void NativeCallTakeSurfaceFromActivity() {
        Log.i(TAG, "NativeCallTakeSurfaceFromActivity");
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        Log.i(TAG, "surfaceCreated");
        nativeHandle = nativeInit(surfaceView, holder.getSurface());
        if (nativeHandle != 0) {
            nativeResume(nativeHandle);
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Log.i(TAG, "surfaceDestroyed");
        if (nativeHandle != 0) {
            nativePause(nativeHandle);
            nativeShutdown(nativeHandle);
            nativeHandle = 0;
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (nativeHandle != 0) nativeResume(nativeHandle);
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (nativeHandle != 0) nativePause(nativeHandle);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (nativeHandle != 0) {
            nativeShutdown(nativeHandle);
            nativeHandle = 0;
        }
    }

    private native long nativeInit(Object surfaceView, Object surface);
    private native void nativeResume(long handle);
    private native void nativePause(long handle);
    private native void nativeShutdown(long handle);
}
