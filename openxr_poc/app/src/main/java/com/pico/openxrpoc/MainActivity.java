package com.pico.openxrpoc;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

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
        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);
        setContentView(surfaceView);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        Log.i(TAG, "surfaceCreated");
        nativeHandle = nativeInit();
        nativeOnSurfaceCreated(nativeHandle, holder.getSurface());
        nativeResume(nativeHandle);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        Log.i(TAG, "surfaceChanged: " + width + "x" + height);
        nativeOnSurfaceChanged(nativeHandle, width, height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Log.i(TAG, "surfaceDestroyed");
        nativePause(nativeHandle);
        nativeOnSurfaceDestroyed(nativeHandle);
        nativeShutdown(nativeHandle);
        nativeHandle = 0;
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

    private native long nativeInit();
    private native void nativeOnSurfaceCreated(long handle, Object surface);
    private native void nativeOnSurfaceChanged(long handle, int width, int height);
    private native void nativeOnSurfaceDestroyed(long handle);
    private native void nativeResume(long handle);
    private native void nativePause(long handle);
    private native void nativeShutdown(long handle);
}
