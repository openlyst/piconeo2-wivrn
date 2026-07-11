package org.meumeu.wivrn.oxr;

import android.app.NativeActivity;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.BatteryManager;
import android.os.Bundle;
import android.util.Log;
import android.view.Surface;
import android.graphics.SurfaceTexture;

import com.picovr.picovrlib.cvcontrollerclient.ControllerClient;
import com.picovr.picovrlib.cvcontrollerclient.BindControllerCallback;

public class MainActivity extends NativeActivity {
    private static final String TAG = "WiVRn-OXR";
    private static final String CTRL_UNITY_VERSION = "2.8.6.9";

    private final float[] mHeadData = new float[7];
    private final float[] mHapticOut = new float[2];

    private volatile boolean mCtrlRunning = false;
    private Thread mCtrlThread;
    private volatile boolean mConn0 = false;
    private volatile boolean mConn1 = false;
    private volatile boolean mCtrlThreadStarted = false;
    private long mBothConnSinceMs = 0;
    private static final long CTRL_SETTLE_MS = 1500;
    private volatile boolean mForeground = false;

    private volatile boolean mUiRenderRunning = false;
    private Thread mUiRenderThread;
    private SurfaceTexture lobbySurfaceTexture;
    private Surface lobbySurface;

    private WivrnLobbyView lobbyView;
    private WifiManager.MulticastLock multicastLock;

    private String pendingHost;
    private int pendingPort;
    private boolean pendingTcpOnly;
    private String pendingPin;
    private boolean hasPendingConnection = false;
    private long lastConnectFlushMs = 0;
    private static final long CONNECT_DEBOUNCE_MS = 2000;

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

        I18n.init(this);
        lobbyView = new WivrnLobbyView(this);

        new Thread(() -> {
            for (int i = 0; i < 50 && !nativeReady(); i++) {
                try { Thread.sleep(100); } catch (InterruptedException e) { return; }
            }
            if (nativeReady()) {
                SharedPreferences sp = getSharedPreferences("wivrn_settings", MODE_PRIVATE);
                float savedIpd;
                try {
                    savedIpd = sp.getFloat("ipd_mm", 64);
                } catch (ClassCastException e) {
                    savedIpd = sp.getInt("ipd_mm", 64);
                }
                final float finalIpd = savedIpd;
                boolean savedMic = sp.getBoolean("microphone", false);
                boolean savedDynBr = sp.getBoolean("dynamic_bitrate", true);
                runOnUiThread(() -> {
                    onIpdChanged(finalIpd);
                    if (savedMic) onMicrophoneChanged(true);
                    lobbyView.applyResolution();
                    nativeSetDynamicBitrate(savedDynBr);
                });
            }
        }).start();

        try {
            WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            multicastLock = wifi.createMulticastLock("wivrn-mdns");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
        } catch (Exception e) {
            Log.e(TAG, "Failed to acquire multicast lock", e);
        }

        handleWivrnIntent(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleWivrnIntent(intent);
    }

    private void handleWivrnIntent(Intent intent) {
        if (intent == null || intent.getData() == null) return;
        Uri uri = intent.getData();
        String scheme = uri.getScheme();
        if (scheme == null) return;
        if (!scheme.equals("wivrn") && !scheme.equals("wivrn+tcp") && !scheme.equals("wivrn+udp")) return;

        String host = uri.getHost();
        int port = uri.getPort();
        if (port <= 0) port = 9757;
        boolean tcpOnly = scheme.equals("wivrn+tcp");
        String tcpOnlyParam = uri.getQueryParameter("tcp_only");
        if (tcpOnlyParam != null && !tcpOnlyParam.isEmpty()) {
            tcpOnly = tcpOnlyParam.equals("1") || tcpOnlyParam.equalsIgnoreCase("true");
        }

        String pin = null;
        String userInfo = uri.getUserInfo();
        if (userInfo != null) {
            int colon = userInfo.indexOf(':');
            if (colon >= 0 && colon + 1 < userInfo.length()) {
                pin = userInfo.substring(colon + 1);
            }
        }
        if (pin == null || pin.isEmpty()) {
            String query = uri.getQueryParameter("pin");
            if (query != null && !query.isEmpty()) pin = query;
        }

        Log.i(TAG, "wivrn intent: host=" + host + " port=" + port + " tcp=" + tcpOnly + " pin=" + (pin != null ? "yes" : "no"));

        if (host != null && !host.isEmpty()) {
            boolean sameTarget = host.equals(pendingHost) && port == pendingPort && tcpOnly == pendingTcpOnly;
            long now = System.currentTimeMillis();
            if (sameTarget && hasPendingConnection && (now - lastConnectFlushMs) < CONNECT_DEBOUNCE_MS) {
                if (pin != null && !pin.isEmpty()) {
                    Log.i(TAG, "intent debounce: updating pin only for " + host + ":" + port);
                    pendingPin = pin;
                    nativeSetPin(pin);
                } else {
                    Log.i(TAG, "intent debounce: ignoring duplicate for " + host + ":" + port);
                }
                return;
            }
            pendingHost = host;
            pendingPort = port;
            pendingTcpOnly = tcpOnly;
            pendingPin = pin;
            hasPendingConnection = true;
            if (lobbyView != null) {
                lobbyView.addOrUpdateServer(host, host, port, tcpOnly);
                lobbyView.setConnectionState(WivrnLobbyView.STATE_CONNECTING, "Connecting...");
            }
            flushPendingConnection();
        }
    }

    private void flushPendingConnection() {
        if (!hasPendingConnection) return;
        if (!nativeReady()) {
            Log.d(TAG, "native not ready, waiting to flush pending connection");
            new Thread(() -> {
                for (int i = 0; i < 50 && !nativeReady(); i++) {
                    try { Thread.sleep(100); } catch (InterruptedException e) { return; }
                }
                runOnUiThread(() -> flushPendingConnection());
            }, "pending-conn").start();
            return;
        }
        Log.i(TAG, "flushing pending connection: " + pendingHost + ":" + pendingPort + " tcp=" + pendingTcpOnly);
        lastConnectFlushMs = System.currentTimeMillis();
        if (lobbyView != null) {
            nativeSetBitrate(lobbyView.getBitrate());
        }
        nativeConnectServer(pendingHost, pendingPort, pendingTcpOnly);
        if (pendingPin != null && !pendingPin.isEmpty()) {
            nativeSetPin(pendingPin);
        }
        hasPendingConnection = false;
    }

    @Override
    protected void onResume() {
        super.onResume();
        mForeground = true;
        if (lobbyView != null) {
            lobbyView.updateWifiStatus();
            lobbyView.startDiscovery();
        }
        setupControllers();
        flushPendingConnection();
        if (!hasPendingConnection && lobbyView != null) {
            lobbyView.tryAutoconnect();
        }
    }

    @Override
    protected void onPause() {
        mForeground = false;
        stopControllerPoll();
        stopUiRenderThread();
        if (lobbyView != null) lobbyView.stopDiscovery();
        try { ControllerClient.unbindControllerService(this); } catch (Throwable t) {}
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (multicastLock != null && multicastLock.isHeld()) {
            multicastLock.release();
        }
        super.onDestroy();
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
                    startUiRenderThread();
                }
                @Override public void unbindSuccess() {
                    Log.i(TAG, "controller service unbound");
                    stopControllerPoll();
                    stopUiRenderThread();
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
                int leftBatt = -1, rightBatt = -1;
                boolean leftConn = false, rightConn = false;

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

                        boolean haveHaptic = nativeDrainHaptic(h, mHapticOut);
                        if (mForeground && conn == 1 && haveHaptic) {
                            try {
                                ControllerClient.vibrateCV2ControllerStrength(
                                        mHapticOut[0], (int) mHapticOut[1], h);
                            } catch (Throwable t) {
                                // old service: no CV2 vibrate
                            }
                        }

                        if (conn == 1) {
                            int batt = pick(ext, EXT_BATTERY);
                            if (h == 0) {
                                leftBatt = batt;
                                leftConn = true;
                            } else {
                                rightBatt = batt;
                                rightConn = true;
                            }
                        }
                    } catch (Throwable t) {}
                }

                if (mForeground && lobbyView != null) {
                    int hmdBatt = getHmdBatteryLevel();
                    lobbyView.updateBatteryStatus(hmdBatt, leftBatt, leftConn, rightBatt, rightConn);
                }

                try { Thread.sleep(8); } catch (InterruptedException e) { break; }
            }
        }, "ctrl-poll");
        mCtrlThread.start();
        Log.i(TAG, "controller poll thread started");
    }

    private void stopControllerPoll() {
        mCtrlRunning = false;
        if (mCtrlThread != null) { mCtrlThread.interrupt(); mCtrlThread = null; }
    }

    private void startUiRenderThread() {
        if (mUiRenderRunning) return;
        mUiRenderRunning = true;

        mUiRenderThread = new Thread(() -> {
            int texId = 0;
            for (int i = 0; i < 50 && texId == 0; i++) {
                texId = nativeGetTextureId();
                if (texId == 0) {
                    try { Thread.sleep(100); } catch (InterruptedException e) { mUiRenderRunning = false; return; }
                }
            }
            if (texId == 0) {
                Log.e(TAG, "nativeGetTextureId returned 0 after retries");
                mUiRenderRunning = false;
                return;
            }

            lobbySurfaceTexture = new SurfaceTexture(texId);
            lobbySurfaceTexture.setDefaultBufferSize(1400, 900);
            lobbySurfaceTexture.setOnFrameAvailableListener(st -> nativeOnFrameAvailable());
            nativeSetSurfaceTexture(lobbySurfaceTexture);
            lobbySurface = new Surface(lobbySurfaceTexture);
            Log.i(TAG, "SurfaceTexture created for texId=" + texId + " bufSize=1400x900");

            int frameCount = 0;
            int wifiPollCount = 0;
            while (mUiRenderRunning) {
                try {
                    if (lobbyView != null) {
                        if (wifiPollCount++ % 200 == 0) {
                            lobbyView.updateWifiStatus();
                        }
                        if (lobbyView.isDirty()) {
                            lobbyView.render();
                            Canvas canvas = lobbySurface.lockCanvas(new Rect(0, 0, 1400, 900));
                            canvas.drawBitmap(lobbyView.getBitmap(), null, new Rect(0, 0, 1400, 900), null);
                            lobbySurface.unlockCanvasAndPost(canvas);
                            lobbyView.markClean();
                            frameCount++;
                            if (frameCount % 100 == 0)
                                Log.i(TAG, "UI frame " + frameCount + " posted");
                        }
                    }
                } catch (Throwable t) {
                    Log.e(TAG, "ui render thread error", t);
                }
                try { Thread.sleep(16); } catch (InterruptedException e) { break; }
            }
        }, "ui-render");
        mUiRenderThread.start();
        Log.i(TAG, "ui render thread started");
    }

    private void stopUiRenderThread() {
        mUiRenderRunning = false;
        if (mUiRenderThread != null) { mUiRenderThread.interrupt(); mUiRenderThread = null; }
        if (lobbySurface != null) { lobbySurface.release(); lobbySurface = null; }
        if (lobbySurfaceTexture != null) { lobbySurfaceTexture.release(); lobbySurfaceTexture = null; }
    }

    private static int touchLogCount = 0;

    public void onLobbyTouch(float x, float y, boolean down, boolean pressed, float thumbstickY) {
        if (touchLogCount++ % 30 == 0 || pressed) {
            Log.i(TAG, "onLobbyTouch x=" + x + " y=" + y + " down=" + down + " pressed=" + pressed + " thumb=" + thumbstickY);
        }
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
        Log.i(TAG, "Connect requested: " + hostname + ":" + port + " tcp=" + tcpOnly);
        pendingHost = hostname;
        pendingPort = port;
        pendingTcpOnly = tcpOnly;
        pendingPin = null;
        hasPendingConnection = false;
        if (lobbyView != null) {
            lobbyView.applyResolution();
            nativeSetBitrate(lobbyView.getBitrate());
            nativeSetDynamicBitrate(lobbyView.isDynamicBitrate());
            lobbyView.setConnectionState(WivrnLobbyView.STATE_CONNECTING, "Connecting...");
        }
        nativeConnectServer(hostname, port, tcpOnly);
    }

    public void onPinEntered(String pin) {
        Log.d(TAG, "PIN entered: " + pin);
        nativeSetPin(pin);
    }

    public void onPinCancelled() {
        Log.d(TAG, "PIN entry cancelled");
        nativeDisconnectServer();
        if (lobbyView != null) {
            lobbyView.setConnectionState(WivrnLobbyView.STATE_IDLE, "");
        }
    }

    public void onDisconnectRequested() {
        Log.d(TAG, "Disconnect requested");
        nativeDisconnectServer();
        if (lobbyView != null) {
            lobbyView.setConnectionState(WivrnLobbyView.STATE_IDLE, "");
        }
    }

    public void onReconnectRequested() {
        Log.d(TAG, "Reconnect requested");
        if (pendingHost == null || pendingHost.isEmpty()) {
            Log.w(TAG, "Reconnect: no previous connection info");
            return;
        }
        if (lobbyView != null) {
            nativeSetBitrate(lobbyView.getBitrate());
            lobbyView.setConnectionState(WivrnLobbyView.STATE_CONNECTING, "Connecting...");
        }
        nativeConnectServer(pendingHost, pendingPort, pendingTcpOnly);
    }

    public void onIpdChanged(float ipdMm) {
        Log.i(TAG, "IPD changed: " + ipdMm + " mm");
        nativeSetIpd(ipdMm);
    }

    public void onMicrophoneChanged(boolean enabled) {
        Log.i(TAG, "Microphone changed: " + enabled);
        if (enabled) {
            if (checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[]{android.Manifest.permission.RECORD_AUDIO}, 1001);
                return;
            }
        }
        nativeSetMicrophone(enabled);
    }

    public void onRenderResolutionChanged(int width, int height) {
        Log.i(TAG, "Render resolution changed: " + width + "x" + height);
        nativeSetRenderResolution(width, height);
    }

    public void onStreamResolutionChanged(int width, int height) {
        Log.i(TAG, "Stream resolution changed: " + width + "x" + height);
        nativeSetStreamResolution(width, height);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == 1001) {
            boolean granted = grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED;
            Log.i(TAG, "RECORD_AUDIO permission: " + (granted ? "granted" : "denied"));
            if (granted && lobbyView != null) {
                nativeSetMicrophone(true);
            } else if (lobbyView != null) {
                lobbyView.setMicrophoneEnabled(false);
            }
        }
    }

    public void onRequestAppList() {
        Log.i(TAG, "Requesting app list");
        nativeRequestAppList();
    }

    public void onStartApp(String appId) {
        Log.i(TAG, "Starting app: " + appId);
        nativeStartApp(appId);
    }

    public void onRequestRunningApps() {
        nativeRequestRunningApps();
    }

    public void onSetActiveApp(int appId) {
        Log.i(TAG, "Setting active app: " + appId);
        nativeSetActiveApp(appId);
    }

    public void onStopApp(int appId) {
        Log.i(TAG, "Stopping app: " + appId);
        nativeStopApp(appId);
    }

    private int getHmdBatteryLevel() {
        try {
            IntentFilter filter = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
            Intent battery = registerReceiver(null, filter);
            if (battery != null) {
                int level = battery.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
                int scale = battery.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
                if (level >= 0 && scale > 0) {
                    return (level * 100) / scale;
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to read HMD battery", e);
        }
        return -1;
    }

    public native void nativeGetHeadData(float[] out);
    public native void nativeControllerState(int hand, int conn, float[] sensor, float[] angVel, int[] keys);
    public native boolean nativeDrainHaptic(int hand, float[] out);
    public native int nativeGetTextureId();
    public native void nativeSetSurfaceTexture(SurfaceTexture surfaceTexture);
    public native void nativeOnFrameAvailable();
    public native void nativeConnectServer(String host, int port, boolean tcpOnly);
    public native void nativeDisconnectServer();
    public native void nativeSetPin(String pin);
    public native void nativeSetBitrate(int bitrateMbps);
    public native void nativeSetDynamicBitrate(boolean enabled);
    public native boolean nativeReady();
    public native void nativeRequestAppList();
    public native void nativeStartApp(String appId);
    public native void nativeRequestRunningApps();
    public native void nativeSetActiveApp(int appId);
    public native void nativeStopApp(int appId);
    public native void nativeSetIpd(float ipdMm);
    public native void nativeSetMicrophone(boolean enabled);
    public native void nativeSetStreamResolution(int width, int height);
    public native void nativeSetRenderResolution(int width, int height);
}
