package org.meumeu.wivrn.oxr;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.util.Log;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

public class MainActivity extends Activity {
    private static final String TAG = "WiVRn-OXR";

    private WivrnLobbyView lobbyView;
    private WifiManager.MulticastLock multicastLock;

    private String pendingHost;
    private int pendingPort;
    private boolean pendingTcpOnly;
    private String pendingPin;
    private boolean hasPendingConnection = false;

    private String selectedHost;
    private int selectedPort;
    private boolean selectedTcpOnly;

    private LobbySurfaceView surfaceView;
    private volatile boolean renderRunning = false;
    private Thread renderThread;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        lobbyView = new WivrnLobbyView(this);

        try {
            WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            multicastLock = wifi.createMulticastLock("wivrn-mdns");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
        } catch (Exception e) {
            Log.e(TAG, "Failed to acquire multicast lock", e);
        }

        surfaceView = new LobbySurfaceView(this);
        setContentView(surfaceView);

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
        if (!scheme.equals("wivrn") && !scheme.equals("wivrn+tcp")) return;

        String host = uri.getHost();
        int port = uri.getPort();
        if (port <= 0) port = 9757;
        boolean tcpOnly = scheme.equals("wivrn+tcp");

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
            pendingHost = host;
            pendingPort = port;
            pendingTcpOnly = tcpOnly;
            pendingPin = pin;
            hasPendingConnection = true;
            flushPendingConnection();
        }
    }

    private void flushPendingConnection() {
        if (!hasPendingConnection) return;
        if (pendingPin != null && !pendingPin.isEmpty()) {
            startStreaming(pendingHost, pendingPort, pendingTcpOnly, pendingPin);
            hasPendingConnection = false;
        } else {
            selectedHost = pendingHost;
            selectedPort = pendingPort;
            selectedTcpOnly = pendingTcpOnly;
            lobbyView.setConnectionState(WivrnLobbyView.STATE_PIN_ENTRY, "");
            hasPendingConnection = false;
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (lobbyView != null) {
            lobbyView.updateWifiStatus();
            lobbyView.startDiscovery();
        }
        startRenderThread();
        flushPendingConnection();
    }

    @Override
    protected void onPause() {
        stopRenderThread();
        if (lobbyView != null) lobbyView.stopDiscovery();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (multicastLock != null && multicastLock.isHeld()) {
            multicastLock.release();
        }
        super.onDestroy();
    }

    private void startRenderThread() {
        if (renderRunning) return;
        renderRunning = true;
        renderThread = new Thread(() -> {
            int wifiPollCount = 0;
            while (renderRunning) {
                try {
                    if (lobbyView != null) {
                        if (wifiPollCount++ % 200 == 0) {
                            lobbyView.updateWifiStatus();
                        }
                        if (lobbyView.isDirty()) {
                            lobbyView.render();
                            SurfaceHolder holder = surfaceView.getHolder();
                            Canvas canvas = holder.lockCanvas();
                            if (canvas != null) {
                                Rect src = new Rect(0, 0, 1400, 900);
                                Rect dst = new Rect(0, 0, canvas.getWidth(), canvas.getHeight());
                                canvas.drawBitmap(lobbyView.getBitmap(), src, dst, null);
                                holder.unlockCanvasAndPost(canvas);
                            }
                            lobbyView.markClean();
                        }
                    }
                } catch (Throwable t) {
                    Log.e(TAG, "render thread error", t);
                }
                try { Thread.sleep(16); } catch (InterruptedException e) { break; }
            }
        }, "lobby-render");
        renderThread.start();
    }

    private void stopRenderThread() {
        renderRunning = false;
        if (renderThread != null) { renderThread.interrupt(); renderThread = null; }
    }

    private void startStreaming(String host, int port, boolean tcpOnly, String pin) {
        Log.i(TAG, "Starting streaming: " + host + ":" + port + " tcp=" + tcpOnly + " pin=" + (pin != null ? "yes" : "no"));
        Intent intent = new Intent(this, StreamingActivity.class);
        intent.putExtra("host", host);
        intent.putExtra("port", port);
        intent.putExtra("tcpOnly", tcpOnly);
        if (pin != null) intent.putExtra("pin", pin);
        startActivity(intent);
    }

    public void onConnectionStateChanged(int state, String message) {
    }

    public void onApplicationList(String[] ids, String[] names) {
    }

    public void onApplicationIcon(String appId, byte[] pngData) {
    }

    public void onRunningApplications(String[] names, int[] ids, boolean[] overlays, boolean[] actives) {
    }

    public void onStreamStats(int fps, int latencyMs, int bandwidthRxBps, int bandwidthTxBps, int bitrateMbps) {
    }

    public void requestPinEntry() {
        lobbyView.setConnectionState(WivrnLobbyView.STATE_PIN_ENTRY, "");
    }

    public void onServerConnect(String hostname, int port, boolean tcpOnly) {
        Log.d(TAG, "Connect requested: " + hostname + ":" + port + " tcp=" + tcpOnly);
        selectedHost = hostname;
        selectedPort = port;
        selectedTcpOnly = tcpOnly;
        lobbyView.setConnectionState(WivrnLobbyView.STATE_PIN_ENTRY, "");
    }

    public void onPinEntered(String pin) {
        Log.d(TAG, "PIN entered: " + pin);
        lobbyView.setConnectionState(WivrnLobbyView.STATE_CONNECTING, "Starting streaming...");
        startStreaming(selectedHost, selectedPort, selectedTcpOnly, pin);
    }

    public void onPinCancelled() {
        Log.d(TAG, "PIN entry cancelled");
        lobbyView.setConnectionState(WivrnLobbyView.STATE_IDLE, "");
    }

    public void onDisconnectRequested() {
        Log.d(TAG, "Disconnect requested");
    }

    public void onRequestAppList() {
    }

    public void onStartApp(String appId) {
    }

    public void onRequestRunningApps() {
    }

    public void onSetActiveApp(int appId) {
    }

    public void onStopApp(int appId) {
    }

    private class LobbySurfaceView extends SurfaceView implements SurfaceHolder.Callback {
        public LobbySurfaceView(Context context) {
            super(context);
            getHolder().addCallback(this);
            getHolder().setFixedSize(1400, 900);
        }

        @Override
        public boolean onTouchEvent(MotionEvent event) {
            float scaleX = 1400.0f / getWidth();
            float scaleY = 900.0f / getHeight();
            float x = event.getX() * scaleX;
            float y = event.getY() * scaleY;
            boolean down = event.getAction() != MotionEvent.ACTION_UP;
            boolean pressed = event.getAction() == MotionEvent.ACTION_DOWN;
            lobbyView.handleTouch(x, y, down, pressed, 0);
            return true;
        }

        @Override
        public void surfaceCreated(SurfaceHolder holder) {
        }

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder) {
        }
    }
}
