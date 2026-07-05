package org.meumeu.wivrn.oxr;

import android.app.NativeActivity;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;

import com.picovr.picovrlib.cvcontrollerclient.ControllerClient;
import com.picovr.picovrlib.cvcontrollerclient.BindControllerCallback;

public class MainActivity extends NativeActivity {
    private static final String TAG = "WiVRn-OXR";
    private static final String CTRL_UNITY_VERSION = "2.8.6.9";

    private final float[] mHeadData = new float[7];

    private volatile boolean mCtrlRunning = false;
    private Thread mCtrlThread;
    private volatile boolean mConn0 = false;
    private volatile boolean mConn1 = false;
    private volatile boolean mCtrlThreadStarted = false;
    private long mBothConnSinceMs = 0;
    private static final long CTRL_SETTLE_MS = 1500;
    private volatile boolean mForeground = false;

    private static final int EXT_JOY_X      = 0;
    private static final int EXT_JOY_Y      = 5;
    private static final int EXT_HOME       = 10;
    private static final int EXT_MENU       = 15;
    private static final int EXT_TRIGGER    = 35;
    private static final int EXT_BATTERY    = 40;
    private static final int EXT_AX         = 45;
    private static final int EXT_BY         = 50;
    private static final int EXT_GRIP_RIGHT = 55;
    private static final int EXT_GRIP_LEFT  = 60;
    private static final int EXT_JOY_CLICK  = 20;
    private static final int LEGACY_TRIG    = 7;
    private static int pick(int[] a, int i) { return (a != null && i < a.length) ? a[i] : 0; }

    static {
        System.loadLibrary("openxr_loader");
        System.loadLibrary("pico_oxr");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
    }

    @Override
    protected void onResume() {
        super.onResume();
        mForeground = true;
        setupControllers();
    }

    @Override
    protected void onPause() {
        mForeground = false;
        stopControllerPoll();
        try { ControllerClient.unbindControllerService(this); } catch (Throwable t) {}
        super.onPause();
    }

    private void setupControllers() {
        try {
            ControllerClient.registerBindCallback(new BindControllerCallback() {
                @Override public void bindSuccess() {
                    try { ControllerClient.setUnityVersion(CTRL_UNITY_VERSION); } catch (Throwable t) {
                        Log.e(TAG, "setUnityVersion failed", t);
                    }
                    Log.i(TAG, "controller service bound");
                    startControllerPoll();
                }
                @Override public void unbindSuccess() {
                    Log.i(TAG, "controller service unbound");
                    stopControllerPoll();
                }
                @Override public void controllerConnectStateChanged(int hand, int state) {
                    Log.i(TAG, "controller " + hand + " connect state -> " + state);
                    if (hand == 0) mConn0 = (state == 1);
                    else if (hand == 1) mConn1 = (state == 1);
                }
                @Override public void onCVChannelChanged(int device, int channel) {}
                @Override public void onHandNessChanged(int hand) {}
                @Override public void onMainControllerSerialNumChanged(int serial) {}
                @Override public void onControllerThreadStarted() {
                    Log.i(TAG, "controller thread started");
                }
            });
            ControllerClient.bindControllerService(this);
            Log.i(TAG, "controller service bind requested");
        } catch (Throwable t) {
            Log.e(TAG, "setupControllers failed", t);
        }
    }

    private synchronized void maybeStartControllerThread() {
        if (mCtrlThreadStarted) return;
        if (!(mConn0 && mConn1)) {
            mBothConnSinceMs = 0;
            return;
        }
        long now = android.os.SystemClock.uptimeMillis();
        if (mBothConnSinceMs == 0) {
            mBothConnSinceMs = now;
            Log.i(TAG, "both controllers connected -> settling " + CTRL_SETTLE_MS + "ms");
            return;
        }
        if (now - mBothConnSinceMs < CTRL_SETTLE_MS) return;
        try {
            try { ControllerClient.setUnityVersion(CTRL_UNITY_VERSION); } catch (Throwable t) {}
            ControllerClient.startControllerThread(1, 1);
            mCtrlThreadStarted = true;
            Log.i(TAG, "controller link settled -> startControllerThread");
        } catch (Throwable t) {
            Log.e(TAG, "startControllerThread failed", t);
        }
    }

    private void startControllerPoll() {
        if (mCtrlRunning) return;
        mCtrlRunning = true;
        mCtrlThread = new Thread(() -> {
            while (mCtrlRunning) {
                if (!mCtrlThreadStarted) {
                    try {
                        mConn0 = (ControllerClient.getControllerConnectionState(0) == 1);
                        mConn1 = (ControllerClient.getControllerConnectionState(1) == 1);
                        maybeStartControllerThread();
                    } catch (Throwable t) {}
                }
                for (int h = 0; h < 2; h++) {
                    try {
                        int conn = ControllerClient.getControllerConnectionState(h);
                        nativeGetHeadData(mHeadData);
                        float[] sensor = null;
                        try { sensor = ControllerClient.getControllerSensorState(h, mHeadData); } catch (Throwable t) {}
                        float[] angVel = null;
                        try { angVel = ControllerClient.getControllerAngularVelocity(h); } catch (Throwable t) {}
                        int[] ext = null;
                        try { ext = ControllerClient.getControllerKeyEventUnityExt(h); } catch (Throwable t) {}
                        int[] legacy = null;
                        try { legacy = ControllerClient.getControllerKeyEvent(h); } catch (Throwable t) {}

                        int[] keys = new int[12];
                        keys[0] = pick(ext, EXT_JOY_X);
                        keys[1] = pick(ext, EXT_JOY_Y);
                        keys[2] = pick(ext, EXT_TRIGGER);
                        keys[3] = pick(ext, h == 1 ? EXT_GRIP_RIGHT : EXT_GRIP_LEFT);
                        keys[4] = pick(ext, EXT_JOY_CLICK);
                        keys[5] = pick(ext, EXT_MENU);
                        keys[6] = pick(ext, EXT_AX);
                        keys[7] = pick(ext, EXT_BY);
                        keys[8] = pick(legacy, LEGACY_TRIG);
                        keys[9] = 0;
                        keys[10] = pick(ext, EXT_BATTERY);
                        keys[11] = pick(ext, EXT_HOME);

                        int sendConn = mForeground ? conn : 0;
                        nativeControllerState(h, sendConn, sensor, angVel, keys);
                    } catch (Throwable t) {}
                }

                try { Thread.sleep(11); } catch (InterruptedException e) { break; }
            }
        }, "ctrl-poll");
        mCtrlThread.start();
        Log.i(TAG, "controller poll thread started");
    }

    private void stopControllerPoll() {
        mCtrlRunning = false;
        if (mCtrlThread != null) { mCtrlThread.interrupt(); mCtrlThread = null; }
    }

    public native void nativeGetHeadData(float[] out);
    public native void nativeControllerState(int hand, int conn, float[] sensor, float[] angVel, int[] keys);
}
