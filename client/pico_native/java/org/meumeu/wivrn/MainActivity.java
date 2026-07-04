package org.meumeu.wivrn;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Bitmap;
import android.net.wifi.WifiManager;
import android.os.BatteryManager;
import android.os.Bundle;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.Window;
import android.view.WindowManager;

import com.unity3d.player.UnityPlayer;

import com.picovr.picovrlib.cvcontrollerclient.ControllerClient;
import com.picovr.picovrlib.cvcontrollerclient.BindControllerCallback;

public class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "WiVRn-Pico";

    static {
        System.loadLibrary("wivrn-neo2");
    }

    private static final String CTRL_UNITY_VERSION = "2.8.6.9";

    public UnityPlayer mUnityPlayer;

    private WivrnLobbyView lobbyView;
    private WifiManager.MulticastLock multicastLock;
    private final float[] mRumble = new float[2];

    private volatile boolean mCtrlRunning = false;
    private Thread mCtrlThread;
    private volatile boolean mConn0 = false;
    private volatile boolean mConn1 = false;
    private volatile boolean mCtrlThreadStarted = false;
    private long mBothConnSinceMs = 0;
    private static final long CTRL_SETTLE_MS = 1500;
    private volatile boolean mForeground = false;
    private volatile long mSettleMs = CTRL_SETTLE_MS;

    private static final int EXT_JOY_X      = 0;
    private static final int EXT_JOY_Y      = 5;
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
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().setFlags(
                WindowManager.LayoutParams.FLAG_FULLSCREEN
                        | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON,
                WindowManager.LayoutParams.FLAG_FULLSCREEN
                        | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        super.onCreate(savedInstanceState);

        mUnityPlayer = new UnityPlayer(this);
        setContentView(mUnityPlayer);
        mUnityPlayer.surfaceView.getHolder().addCallback(this);

        lobbyView = new WivrnLobbyView(this);

        try {
            WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            multicastLock = wifi.createMulticastLock("wivrn-mdns");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
        } catch (Exception e) {
            Log.e(TAG, "Failed to acquire multicast lock", e);
        }

        nativeStart(this, getIntent());
        Log.d(TAG, "nativeStart called");
    }

    @Override
    protected void onResume() {
        super.onResume();
        mForeground = true;
        setupControllers();
        nativeResume();
    }

    @Override
    protected void onPause() {
        mForeground = false;
        nativePause();
        stopControllerPoll();
        try { ControllerClient.unbindControllerService(this); } catch (Throwable t) {}
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        nativeStop();
        super.onDestroy();
    }

    @Override
    public void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        nativeNewIntent(intent);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        Log.i(TAG, "surfaceCreated");
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        Log.i(TAG, "surfaceChanged " + width + "x" + height);
        if (width > 0 && height > 0) {
            nativeSurfaceChanged(holder.getSurface());
        }
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Log.i(TAG, "surfaceDestroyed");
        nativeSurfaceDestroyed();
    }

    private void setupControllers() {
        try {
            ControllerClient.registerBindCallback(new BindControllerCallback() {
                @Override public void bindSuccess() {
                    try { ControllerClient.setUnityVersion(CTRL_UNITY_VERSION); } catch (Throwable t) {
                        Log.e(TAG, "setUnityVersion failed", t);
                    }
                    Log.i(TAG, "controller service bound; deferring 6DoF start");
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
                @Override public void onCVChannelChanged(int device, int channel) {
                }
                @Override public void onHandNessChanged(int hand) {
                }
                @Override public void onMainControllerSerialNumChanged(int serial) {
                }
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
            Log.i(TAG, "both controllers connected -> settling " + mSettleMs + "ms");
            return;
        }
        if (now - mBothConnSinceMs < mSettleMs) return;
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
            int lobbyTick = 0;
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
                        float[] sensor = null;
                        try { sensor = ControllerClient.getControllerSensorState(h, new float[7]); } catch (Throwable t) {}
                        float[] angVel = null;
                        try { angVel = ControllerClient.getControllerAngularVelocity(h); } catch (Throwable t) {}
                        int[] ext = null;
                        try { ext = ControllerClient.getControllerKeyEventUnityExt(h); } catch (Throwable t) {}
                        int[] legacy = null;
                        try { legacy = ControllerClient.getControllerKeyEvent(h); } catch (Throwable t) {}
                        int gripAnalog = 0;

                        int[] keys = new int[11];
                        keys[0] = pick(ext, EXT_JOY_X);
                        keys[1] = pick(ext, EXT_JOY_Y);
                        keys[2] = pick(ext, EXT_TRIGGER);
                        keys[3] = pick(ext, h == 1 ? EXT_GRIP_RIGHT : EXT_GRIP_LEFT);
                        keys[4] = pick(ext, EXT_JOY_CLICK);
                        keys[5] = pick(ext, EXT_MENU);
                        keys[6] = pick(ext, EXT_AX);
                        keys[7] = pick(ext, EXT_BY);
                        keys[8] = pick(legacy, LEGACY_TRIG);
                        keys[9] = gripAnalog;
                        keys[10] = pick(ext, EXT_BATTERY);

                        int sendConn = mForeground ? conn : 0;
                        nativeControllerState(h, sendConn, sensor, angVel, keys);

                        boolean haveHaptic = nativeDrainHaptic(h, mRumble);
                        if (mForeground && conn == 1 && haveHaptic) {
                            try {
                                ControllerClient.vibrateCV2ControllerStrength(
                                        mRumble[0], (int) mRumble[1], h);
                            } catch (Throwable t) {}
                        }
                    } catch (Throwable t) {}
                }

                lobbyTick++;
                if (lobbyView != null && (lobbyTick % 6 == 0)) {
                    try {
                        if (lobbyView.isDirty()) {
                            lobbyView.render();
                            nativeUpdateLobbyTexture(lobbyView.getBitmap());
                            lobbyView.markClean();
                        }
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

    public void onLobbyTouch(float x, float y, boolean down, boolean pressed, float thumbstickY) {
        if (lobbyView != null) {
            lobbyView.handleTouch(x, y, down, pressed, thumbstickY);
        }
    }

    public void onConnectionStateChanged(int state, String message) {
        if (lobbyView != null) {
            lobbyView.setConnectionState(state, message);
        }
    }

    public void onApplicationList(String[] ids, String[] names) {
        if (lobbyView != null) {
            lobbyView.updateAvailableApps(ids, names);
        }
    }

    public void onApplicationIcon(String appId, byte[] pngData) {
        if (lobbyView != null) {
            lobbyView.updateAppIcon(appId, pngData);
        }
    }

    public void onRunningApplications(String[] names, int[] ids, boolean[] overlays, boolean[] actives) {
        if (lobbyView != null) {
            lobbyView.updateRunningApps(names, ids, overlays, actives);
        }
    }

    public void onStreamStats(int fps, int latencyMs, int bandwidthRxBps, int bandwidthTxBps, int bitrateMbps) {
        if (lobbyView != null) {
            lobbyView.updateStreamStats(fps, latencyMs, bandwidthRxBps, bandwidthTxBps, bitrateMbps);
        }
    }

    public void requestPinEntry() {
        if (lobbyView != null) {
            lobbyView.setConnectionState(WivrnLobbyView.STATE_PIN_ENTRY, "");
        }
    }

    public void onServerConnect(String hostname, int port, boolean tcpOnly) {
        Log.d(TAG, "Connect requested: " + hostname + ":" + port + " tcp=" + tcpOnly);
        nativeConnect(hostname, port, tcpOnly);
    }

    public void onPinEntered(String pin) {
        Log.d(TAG, "PIN entered: " + pin);
        nativeSubmitPin(pin);
    }

    public void onPinCancelled() {
        Log.d(TAG, "PIN entry cancelled");
        nativeSubmitPin("");
    }

    public void onDisconnectRequested() {
        Log.d(TAG, "Disconnect requested");
        nativeDisconnect();
    }

    public void onRequestAppList() {
        Log.d(TAG, "Requesting app list");
        nativeRequestAppList();
    }

    public void onStartApp(String appId) {
        Log.d(TAG, "Starting app: " + appId);
        nativeStartApp(appId);
    }

    public void onRequestRunningApps() {
        nativeRequestRunningApps();
    }

    public void onSetActiveApp(int appId) {
        Log.d(TAG, "Setting active app: " + appId);
        nativeSetActiveApp(appId);
    }

    public void onStopApp(int appId) {
        Log.d(TAG, "Stopping app: " + appId);
        nativeStopApp(appId);
    }

    public native void nativeStart(Activity activity, Intent intent);
    public native void nativeSurfaceChanged(Surface surface);
    public native void nativeSurfaceDestroyed();
    public native void nativeStop();
    public native void nativePause();
    public native void nativeResume();
    public native void nativeNewIntent(Intent intent);
    public native void nativeControllerState(int hand, int conn, float[] sensor, float[] angVel, int[] keys);
    public native boolean nativeDrainHaptic(int hand, float[] out);
    public native void nativeSubmitPin(String pin);
    public native void nativeConnect(String hostname, int port, boolean tcpOnly);
    public native void nativeDisconnect();
    public native void nativeRequestAppList();
    public native void nativeStartApp(String appId);
    public native void nativeRequestRunningApps();
    public native void nativeSetActiveApp(int appId);
    public native void nativeStopApp(int appId);
    public native void nativeUpdateLobbyTexture(android.graphics.Bitmap bitmap);
}
