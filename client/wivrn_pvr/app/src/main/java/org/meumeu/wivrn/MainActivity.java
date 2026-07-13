package org.meumeu.wivrn;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.util.Log;
import android.view.KeyEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.WindowManager;
import android.graphics.PixelFormat;

import com.picovr.picovrlib.cvcontrollerclient.ControllerClient;
import com.picovr.picovrlib.cvcontrollerclient.BindControllerCallback;
import com.unity3d.player.UnityPlayer;

public class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "WiVRn-Pico";

    static {
        System.loadLibrary("tracking_module");
        System.loadLibrary("Pvr_UnitySDK");
        System.loadLibrary("wivrn_pvr");
    }

    // The Pico SDK reaches into the activity for this field and pulls the
    // render Surface out of its first child. Shape must match exactly.
    public UnityPlayer mUnityPlayer;

    private volatile boolean mCtrlRunning = false;
    private Thread mCtrlThread;
    private volatile boolean mCtrlThreadStarted = false;
    private volatile boolean mConn0 = false;
    private volatile boolean mConn1 = false;
    private volatile boolean mForeground = false;
    private long mBothConnSinceMs = 0;
    private static final long CTRL_SETTLE_MS = 1500;
    private static final String CTRL_UNITY_VERSION = "2.8.6.9";

    private final float[] mHeadData = new float[7];
    private final float[] mHapticOut = new float[2];

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

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        try {
            WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            WifiManager.MulticastLock lock = wifi.createMulticastLock("wivrn-mdns");
            lock.setReferenceCounted(false);
            lock.acquire();
        } catch (Exception e) {
            Log.e(TAG, "multicast lock failed", e);
        }

        mUnityPlayer = new UnityPlayer(this);
        setContentView(mUnityPlayer);
        mUnityPlayer.surfaceView.getHolder().addCallback(this);

        nativeStart(this, getIntent());
        handleWivrnIntent(getIntent());
    }

    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleWivrnIntent(intent);
        nativeNewIntent(intent);
    }

    private void handleWivrnIntent(Intent intent) {
        if (intent == null || intent.getData() == null) return;
        Uri uri = intent.getData();
        String scheme = uri.getScheme();
        if (scheme == null) return;
        if (!scheme.equals("wivrn") && !scheme.equals("wivrn+tcp")) return;

        String host = uri.getHost();
        int port = uri.getPort();
        if (port <= 0) port = 9757;
        boolean tcpOnly = scheme.equals("wivrn+tcp");

        String pin = null;
        String userInfo = uri.getUserInfo();
        if (userInfo != null) {
            int colon = userInfo.indexOf(':');
            if (colon >= 0 && colon + 1 < userInfo.length()) pin = userInfo.substring(colon + 1);
        }
        if (pin == null || pin.isEmpty()) pin = uri.getQueryParameter("pin");

        Log.i(TAG, "intent host=" + host + " port=" + port + " tcp=" + tcpOnly);
        if (host != null && !host.isEmpty()) {
            nativeConnect(host, port, tcpOnly);
            if (pin != null && !pin.isEmpty()) nativeSubmitPin(pin);
        }
    }

    @Override protected void onResume() {
        super.onResume();
        mForeground = true;
        nativeResume();
        setupControllers();
    }

    @Override protected void onPause() {
        mForeground = false;
        stopControllerPoll();
        nativePause();
        try { ControllerClient.unbindControllerService(this); } catch (Throwable t) {}
        super.onPause();
    }

    @Override protected void onDestroy() {
        nativeStop();
        super.onDestroy();
    }

    @Override public void surfaceCreated(SurfaceHolder holder) {
        holder.setFormat(PixelFormat.RGBA_8888);
    }

    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        Surface surface = holder.getSurface();
        if (surface != null && surface.isValid()) {
            nativeSurfaceChanged(surface);
        }
    }

    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        nativeSurfaceDestroyed();
    }

    private void setupControllers() {
        try {
            ControllerClient.registerBindCallback(new BindControllerCallback() {
                @Override public void bindSuccess() {
                    try { ControllerClient.setUnityVersion(CTRL_UNITY_VERSION); } catch (Throwable t) {}
                    startControllerPoll();
                }
                @Override public void unbindSuccess() { stopControllerPoll(); }
                @Override public void controllerConnectStateChanged(int hand, int state) {
                    if (hand == 0) mConn0 = (state == 1);
                    else if (hand == 1) mConn1 = (state == 1);
                }
                @Override public void onCVChannelChanged(int device, int channel) {}
                @Override public void onHandNessChanged(int hand) {}
                @Override public void onMainControllerSerialNumChanged(int serial) {}
                @Override public void onControllerThreadStarted() {}
            });
            ControllerClient.bindControllerService(this);
        } catch (Throwable t) {
            Log.e(TAG, "setupControllers failed", t);
        }
    }

    private synchronized void maybeStartControllerThread() {
        if (mCtrlThreadStarted) return;
        if (!(mConn0 && mConn1)) { mBothConnSinceMs = 0; return; }
        long now = android.os.SystemClock.uptimeMillis();
        if (mBothConnSinceMs == 0) { mBothConnSinceMs = now; return; }
        if (now - mBothConnSinceMs < CTRL_SETTLE_MS) return;
        try {
            ControllerClient.setUnityVersion(CTRL_UNITY_VERSION);
            ControllerClient.startControllerThread(1, 1);
            mCtrlThreadStarted = true;
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

                        nativeControllerState(h, mForeground ? conn : 0, sensor, angVel, keys);

                        boolean haveHaptic = nativeDrainHaptic(h, mHapticOut);
                        if (mForeground && conn == 1 && haveHaptic) {
                            try {
                                ControllerClient.vibrateCV2ControllerStrength(mHapticOut[0], (int) mHapticOut[1], h);
                            } catch (Throwable t) {}
                        }
                    } catch (Throwable t) {}
                }
                try { Thread.sleep(8); } catch (InterruptedException e) { break; }
            }
        }, "ctrl-poll");
        mCtrlThread.start();
    }

    private void stopControllerPoll() {
        mCtrlRunning = false;
        if (mCtrlThread != null) { mCtrlThread.interrupt(); mCtrlThread = null; }
    }

    public void onConnectionStateChanged(int state, String message) {
        Log.i(TAG, "state=" + state + " msg=" + message);
    }
    public void onApplicationList(String[] ids, String[] names) {}
    public void onApplicationIcon(String appId, byte[] pngData) {}
    public void onRunningApplications(String[] names, int[] ids, boolean[] overlays, boolean[] actives) {}
    public void onStreamStats(int fps, int latencyMs, int bandwidthRxBps, int bandwidthTxBps, int bitrateMbps) {}
    public void requestPinEntry() {}

    private native void nativeStart(Activity activity, Intent intent);
    private native void nativeStop();
    private native void nativePause();
    private native void nativeResume();
    private native void nativeNewIntent(Intent intent);
    private native void nativeSurfaceChanged(Surface surface);
    private native void nativeSurfaceDestroyed();
    private native void nativeGetHeadData(float[] out);
    private native void nativeControllerState(int hand, int conn, float[] sensor, float[] angVel, int[] keys);
    private native boolean nativeDrainHaptic(int hand, float[] out);
    private native void nativeSubmitPin(String pin);
    private native void nativeConnect(String hostname, int port, boolean tcpOnly);
    private native void nativeDisconnect();
    private native void nativeRequestAppList();
    private native void nativeStartApp(String appId);
    private native void nativeRequestRunningApps();
    private native void nativeSetActiveApp(int appId);
    private native void nativeStopApp(int appId);
}
