package org.meumeu.wivrn;

import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.view.Window;
import android.view.WindowManager;

import com.picovr.vractivity.Eye;
import com.picovr.vractivity.HmdState;
import com.picovr.vractivity.RenderInterface;
import com.picovr.vractivity.VRActivity;
import com.psmart.vrlib.PicovrSDK;

public class MainActivity extends VRActivity implements RenderInterface {
    private static final String TAG = "WiVRn-Pico";

    static {
        System.loadLibrary("wivrn-neo2");
    }

    private long nativePtr;

    @Override
    public void onCreate(Bundle savedInstanceState) {
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().setFlags(
                WindowManager.LayoutParams.FLAG_FULLSCREEN
                        | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON,
                WindowManager.LayoutParams.FLAG_FULLSCREEN
                        | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        super.onCreate(savedInstanceState);

        try {
            nativePtr = getNativePtr();
            Log.d(TAG, "nativePtr = " + nativePtr);
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "getNativePtr failed", e);
            nativePtr = 0;
        }
        setIntent(getIntent());

        try {
            nativeWivrnInit(nativePtr, getIntent());
            Log.d(TAG, "nativeWivrnInit called");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "nativeWivrnInit failed", e);
        }
    }

    @Override
    public void onResume() {
        super.onResume();
        PicovrSDK.startSensor(0);
        PicovrSDK.setTrackingOriginType(1);
        Log.d(TAG, "onResume, nativePtr=" + nativePtr);
        try {
            nativeWivrnResume(nativePtr);
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "nativeWivrnResume failed", e);
        }
    }

    @Override
    public void onPause() {
        nativeWivrnPause(nativePtr);
        PicovrSDK.stopSensor(0);
        super.onPause();
    }

    @Override
    public void onDestroy() {
        nativeWivrnDestroy(nativePtr);
        super.onDestroy();
    }

    @Override
    public void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        nativeWivrnNewIntent(nativePtr, intent);
    }

    @Override
    public void onFrameBegin(HmdState hmdState) {
        // Tracking handled by OpenXR on the native side
    }

    @Override
    public void onDrawEye(Eye eye) {
        nativeWivrnDrawEye(nativePtr, eye.getType());
    }

    @Override
    public void onFrameEnd() {
        nativeWivrnFrameEnd(nativePtr);
    }

    @Override
    public void onTouchEvent() {
    }

    @Override
    public void initGL(int w, int h) {
        PicovrSDK.SetEyeBufferSize(w, h);
        nativeWivrnInitGL(nativePtr, w, h);
    }

    @Override
    public void deInitGL() {
        nativeWivrnDeInitGL(nativePtr);
    }

    @Override
    public void surfaceChangedCallBack(int w, int h) {
        nativeWivrnSurfaceChanged(nativePtr, w, h);
    }

    @Override
    public void onRenderPause() {
        nativeWivrnRenderPause(nativePtr);
    }

    @Override
    public void onRenderResume() {
        nativeWivrnRenderResume(nativePtr);
    }

    @Override
    public void onRendererShutdown() {
        nativeWivrnRendererShutdown(nativePtr);
    }

    @Override
    public void renderEventCallBack(int event) {
        nativeWivrnRenderEvent(nativePtr, event);
    }

    // Native methods - WiVRn-specific (PVR SDK handles its own via VRActivity)
    public native void nativeWivrnInit(long ptr, Intent intent);
    public native void nativeWivrnDestroy(long ptr);
    public native void nativeWivrnPause(long ptr);
    public native void nativeWivrnResume(long ptr);
    public native void nativeWivrnNewIntent(long ptr, Intent intent);
    public native void nativeWivrnDrawEye(long ptr, int eye);
    public native void nativeWivrnFrameEnd(long ptr);
    public native void nativeWivrnInitGL(long ptr, int w, int h);
    public native void nativeWivrnDeInitGL(long ptr);
    public native void nativeWivrnSurfaceChanged(long ptr, int w, int h);
    public native void nativeWivrnRenderPause(long ptr);
    public native void nativeWivrnRenderResume(long ptr);
    public native void nativeWivrnRendererShutdown(long ptr);
    public native void nativeWivrnRenderEvent(long ptr, int event);
}
