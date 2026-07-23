package org.meumeu.wivrn.neo2.pvr;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.net.Uri;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.net.InetAddress;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public class WivrnLobbyView {
    private static final String TAG = "WiVRn-Pico";

    public static final int TAB_SERVER_LIST = 0;
    public static final int TAB_SETTINGS = 1;
    public static final int TAB_ABOUT = 2;
    public static final int TAB_LICENSES = 3;
    public static final int TAB_EXIT = 4;

    public static final int STATE_IDLE = 0;
    public static final int STATE_CONNECTING = 1;
    public static final int STATE_PIN_ENTRY = 2;
    public static final int STATE_DISCONNECTED = 3;
    public static final int STATE_CONNECTED = 4;

    public static final int STREAM_TAB_APPLICATIONS = 0;
    public static final int STREAM_TAB_STATS = 1;
    public static final int STREAM_TAB_SETTINGS = 2;
    public static final int STREAM_TAB_LAUNCH = 3;

    private int streamTab = STREAM_TAB_APPLICATIONS;
    private int streamFps = 0;
    private int streamLatencyMs = 0;
    private int streamBitrateMbps = 0;
    private int streamCpuMs = 0;
    private int streamGpuMs = 0;
    private float streamBandwidthRx = 0;
    private float streamBandwidthTx = 0;

    private static final int STATS_HISTORY_SIZE = 150;
    private float[] fpsHistory = new float[STATS_HISTORY_SIZE];
    private float[] latencyHistory = new float[STATS_HISTORY_SIZE];
    private float[] bwRxHistory = new float[STATS_HISTORY_SIZE];
    private float[] bwTxHistory = new float[STATS_HISTORY_SIZE];
    private float[] cpuTimeHistory = new float[STATS_HISTORY_SIZE];
    private float[] gpuTimeHistory = new float[STATS_HISTORY_SIZE];
    private int statsHistoryOffset = 0;
    private int statsHistoryCount = 0;

    private float streamEncodeMs = 0;
    private float streamSendMs = 0;
    private float streamNetworkMs = 0;
    private float streamDecodeMs = 0;
    private float streamRenderWaitMs = 0;
    private float streamBlitMs = 0;
    private float streamTotalLatencyMs = 0;
    private String[] runningApps = new String[0];
    private int[] runningAppIds = new int[0];
    private boolean[] runningAppOverlays = new boolean[0];
    private boolean[] runningAppActives = new boolean[0];
    private long lastRunningAppsPoll = 0;
    private String[] availableAppIds = new String[0];
    private String[] availableAppNames = new String[0];
    private Map<String, Bitmap> appIcons = new HashMap<>();
    private boolean appListRequested = false;
    private String launchingAppName = null;
    private long launchingStartTime = 0;
    private int streamBitrateSetting = 50;
    private int streamResolutionScale = 100;
    private boolean streamMicEnabled = false;

    private float launchScrollY = 0;
    private float launchMaxScroll = 0;
    private float dragStartY = -1;
    private float dragStartScroll = 0;
    private float prevTouchY = -1;
    private float thumbstickAccum = 0;
    private float settingsScrollY = 0;
    private float settingsMaxScroll = 0;
    private float settingsThumbstickAccum = 0;
    private float pressStartX = -1;
    private float pressStartY = -1;
    private boolean pressDragged = false;
    private static final float DRAG_THRESHOLD = 20f;

    private final Context context;
    private final Bitmap bitmap;
    private final Canvas canvas;
    private final int width = 1400;
    private final int height = 900;
    private final I18n i18n;

    private int currentTab = TAB_SERVER_LIST;
    private int connectionState = STATE_IDLE;
    private String statusMessage = "";
    private String errorMessage = "";
    private String pinBuffer = "";
    private boolean dirty = true;

    private final SharedPreferences prefs;
    private final List<ServerEntry> servers = new ArrayList<>();
    private int selectedServerIndex = -1;

    private final Paint bgPaint = new Paint();
    private final Paint sidebarBgPaint = new Paint();
    private final Paint cardBgPaint = new Paint();
    private final Paint textPaint = new Paint();
    private final Paint textSmallPaint = new Paint();
    private final Paint textLargePaint = new Paint();
    private final Paint textDimPaint = new Paint();
    private final Paint buttonBgPaint = new Paint();
    private final Paint buttonHoverBgPaint = new Paint();
    private final Paint buttonConnectBgPaint = new Paint();
    private final Paint buttonDangerBgPaint = new Paint();
    private final Paint accentPaint = new Paint();
    private final Paint dimPaint = new Paint();
    private final Paint pinDisplayPaint = new Paint();
    private final Paint pinKeyBgPaint = new Paint();
    private final Paint sliderTrackPaint = new Paint();
    private final Paint sliderFillPaint = new Paint();
    private final Paint sliderHandlePaint = new Paint();

    private float touchX = -1, touchY = -1;
    private boolean touchDown = false;
    private boolean touchPressed = false;

    private int activeSlider = -1;

    private static final int SLIDER_NONE = -1;
    private static final int SLIDER_RESOLUTION = 0;
    private static final int SLIDER_FOVEATION = 1;
    private static final int SLIDER_BITRATE = 2;
    private static final int SLIDER_IPD = 3;
    private static final int SLIDER_FOV = 4;
    private static final int SLIDER_BRIGHTNESS = 5;
    private static final int SLIDER_CTRL_VIBRATION = 6;

    private int resWidth = 1664;
    private int foveationScale = 30;
    private String codec = "auto";
    private int bitrate = 50;
    private float ipdMm = 64;
    private float fovDeg = 101;
    private boolean tcpOnly = false;
    private boolean microphoneEnabled = false;
    private boolean passthroughEnabled = true;
    private int languageSetting = 0;
    private float brightnessFrac = 1.0f;
    private float ctrlVibration = 1.0f;
    private boolean eyeFoveationEnabled = false;
    private boolean eyeDebugOn = false;
    private int diagHudMode = 0;
    private boolean eyeSupported = false;

    private String addServerName = "";
    private String addServerAddress = "";
    private String addServerPort = "9757";
    private boolean addServerTcpOnly = false;
    private boolean showAddServer = false;
    private int addServerFieldFocus = 0;
    private boolean showResetConfirm = false;

    private static final int SIDEBAR_WIDTH = 280;
    private static final int TAB_HEIGHT = 70;
    private static final int CARD_HEIGHT = 100;
    private static final int BUTTON_WIDTH = 200;
    private static final int BUTTON_HEIGHT = 60;
    private static final int TOPBAR_HEIGHT = 50;
    private static final Paint topbarBgPaint = new Paint();
    private static final Paint topbarBorderPaint = new Paint();

    public static class ServerEntry {
        String name;
        String hostname;
        int port;
        boolean tcpOnly;
        boolean manual;
        boolean discovered;
        boolean autoconnect;

        ServerEntry(String name, String hostname, int port, boolean tcpOnly, boolean manual) {
            this(name, hostname, port, tcpOnly, manual, false, false);
        }

        ServerEntry(String name, String hostname, int port, boolean tcpOnly, boolean manual, boolean discovered) {
            this(name, hostname, port, tcpOnly, manual, discovered, false);
        }

        ServerEntry(String name, String hostname, int port, boolean tcpOnly, boolean manual, boolean discovered, boolean autoconnect) {
            this.name = name;
            this.hostname = hostname;
            this.port = port;
            this.tcpOnly = tcpOnly;
            this.manual = manual;
            this.discovered = discovered;
            this.autoconnect = autoconnect;
        }
    }

    private NsdManager nsdManager;
    private NsdManager.DiscoveryListener nsdListener;
    private final Map<String, ServerEntry> discoveredServers = new HashMap<>();
    private int wifiLevel = -1; // -1 = no wifi, 0-3 = signal bars
    private String wifiSsid = "";
    private int hmdBattery = -1; // -1 = unknown
    private int leftBattery = -1;
    private boolean leftConnected = false;
    private int rightBattery = -1;
    private boolean rightConnected = false;

    private boolean autoconnectAttempted = false;

    public void markAutoconnectAttempted() { autoconnectAttempted = true; }

    public WivrnLobbyView(Context context) {
        this.context = context;
        this.bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        this.canvas = new Canvas(bitmap);
        this.prefs = context.getSharedPreferences("wivrn_servers", Context.MODE_PRIVATE);
        this.i18n = I18n.init(context);

        initPaints();
        loadSettings();
        loadServers();
        startDiscovery();
        updateWifiStatus();
    }

    public void tryAutoconnect() {
        if (autoconnectAttempted) return;
        autoconnectAttempted = true;
        for (ServerEntry s : servers) {
            if (s.autoconnect) {
                Log.i(TAG, "Autoconnecting to " + s.hostname + ":" + s.port);
                connectionState = STATE_CONNECTING;
                statusMessage = "Connecting...";
                ((MainActivity) context).onServerConnect(s.hostname, s.port, s.tcpOnly);
                return;
            }
        }
    }

    public void addOrUpdateServer(String name, String hostname, int port, boolean tcpOnly) {
        for (ServerEntry s : servers) {
            if (s.hostname.equals(hostname) && s.port == port) {
                return;
            }
        }
        servers.add(new ServerEntry(name, hostname, port, tcpOnly, true));
        saveServers();
        markDirty();
    }

    public void setAutoconnect(String hostname, int port) {
        boolean currentlyOn = false;
        for (ServerEntry s : servers) {
            if (s.hostname.equals(hostname) && s.port == port && s.autoconnect) {
                currentlyOn = true;
                break;
            }
        }

        if (currentlyOn) {
            for (ServerEntry s : servers) {
                s.autoconnect = false;
            }
        } else {
            boolean found = false;
            for (ServerEntry s : servers) {
                boolean match = s.hostname.equals(hostname) && s.port == port;
                s.autoconnect = match;
                if (match) found = true;
            }
            if (!found) {
                synchronized (discoveredServers) {
                    for (ServerEntry s : discoveredServers.values()) {
                        if (s.hostname.equals(hostname) && s.port == port) {
                            ServerEntry persisted = new ServerEntry(s.name, s.hostname, s.port, s.tcpOnly, true, false, true);
                            servers.add(persisted);
                            break;
                        }
                    }
                }
            }
        }
        saveServers();
        markDirty();
    }

    public void updateWifiStatus() {
        try {
            WifiManager wifi = (WifiManager) context.getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            if (wifi == null || !wifi.isWifiEnabled()) {
                wifiLevel = -1;
                wifiSsid = "";
                return;
            }
            WifiInfo info = wifi.getConnectionInfo();
            if (info == null || info.getSupplicantState() != android.net.wifi.SupplicantState.COMPLETED) {
                wifiLevel = -1;
                wifiSsid = "";
                return;
            }
            int rssi = info.getRssi();
            int level = WifiManager.calculateSignalLevel(rssi, 4);
            wifiLevel = level;
            String ssid = info.getSSID();
            if (ssid != null && !ssid.contains("unknown") && !ssid.startsWith("<")) {
                wifiSsid = ssid.replace("\"", "");
            } else {
                wifiSsid = "";
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to get wifi status", e);
            wifiLevel = -1;
            wifiSsid = "";
        }
    }

    private void renderWifiIcon(float rightX, float centerY) {
        float iconSize = 22;
        float cx = rightX - iconSize;
        float cy = centerY;

        Paint wifiPaint = new Paint();
        wifiPaint.setAntiAlias(true);
        wifiPaint.setStyle(Paint.Style.STROKE);
        wifiPaint.setStrokeCap(Paint.Cap.ROUND);

        if (wifiLevel < 0) {
            wifiPaint.setColor(Color.rgb(180, 60, 60));
            wifiPaint.setStrokeWidth(3);
            canvas.drawCircle(cx, cy, iconSize * 0.4f, wifiPaint);
            wifiPaint.setStrokeWidth(4);
            canvas.drawLine(cx - iconSize * 0.28f, cy + iconSize * 0.28f, cx + iconSize * 0.28f, cy - iconSize * 0.28f, wifiPaint);
        } else {
            int color = wifiLevel >= 2 ? Color.rgb(80, 200, 120) : Color.rgb(220, 180, 60);
            wifiPaint.setColor(color);
            wifiPaint.setStrokeWidth(3);

            float r = iconSize * 0.15f;
            canvas.drawPoint(cx, cy + r * 2.5f, wifiPaint);

            for (int i = 0; i < 3; i++) {
                float arcR = iconSize * 0.15f * (i + 1) * 1.3f;
                RectF arcRect = new RectF(cx - arcR, cy + r * 2.5f - arcR, cx + arcR, cy + r * 2.5f + arcR);
                float startAngle = 225;
                float sweepAngle = 90;
                if (i <= wifiLevel) {
                    canvas.drawArc(arcRect, startAngle, sweepAngle, false, wifiPaint);
                } else {
                    wifiPaint.setColor(Color.rgb(80, 85, 95));
                    canvas.drawArc(arcRect, startAngle, sweepAngle, false, wifiPaint);
                    wifiPaint.setColor(color);
                }
            }
        }
    }

    public void updateBatteryStatus(int hmdBatt, int leftBatt, boolean leftConn, int rightBatt, boolean rightConn) {
        if (hmdBattery == hmdBatt && leftBattery == leftBatt && leftConnected == leftConn
                && rightBattery == rightBatt && rightConnected == rightConn)
            return;
        hmdBattery = hmdBatt;
        leftBattery = leftBatt;
        leftConnected = leftConn;
        rightBattery = rightBatt;
        rightConnected = rightConn;
        markDirty();
    }

    private int batteryColor(int pct) {
        if (pct < 0) return Color.rgb(100, 110, 125);
        if (pct < 20) return Color.rgb(220, 60, 60);
        if (pct < 40) return Color.rgb(220, 180, 60);
        return Color.rgb(80, 200, 120);
    }

    private static final int DEVICE_HMD = 0;
    private static final int DEVICE_LEFT = 1;
    private static final int DEVICE_RIGHT = 2;

    private void renderDeviceIcon(float rightX, float centerY, int deviceType, int color) {
        Paint p = new Paint();
        p.setAntiAlias(true);
        p.setStyle(Paint.Style.STROKE);
        p.setStrokeWidth(2);
        p.setColor(color);

        Paint fill = new Paint();
        fill.setAntiAlias(true);
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(color);

        float cx = rightX - 14;
        float cy = centerY;

        if (deviceType == DEVICE_HMD) {
            float w = 22, h = 14;
            RectF visor = new RectF(cx - w/2, cy - h/2, cx + w/2, cy + h/2);
            canvas.drawRoundRect(visor, 6, 6, p);
            fill.setStyle(Paint.Style.STROKE);
            fill.setStrokeWidth(1.5f);
            RectF lensL = new RectF(cx - 7, cy - 4, cx - 1, cy + 4);
            RectF lensR = new RectF(cx + 1, cy - 4, cx + 7, cy + 4);
            canvas.drawOval(lensL, fill);
            canvas.drawOval(lensR, fill);
        } else {
            float w = 10, h = 20;
            float ctrlX = cx;
            if (deviceType == DEVICE_LEFT) ctrlX -= 2;
            else ctrlX += 2;

            RectF body = new RectF(ctrlX - w/2, cy - h/2, ctrlX + w/2, cy + h/2);
            canvas.drawRoundRect(body, 4, 4, p);

            fill.setStyle(Paint.Style.STROKE);
            fill.setStrokeWidth(1.5f);
            canvas.drawCircle(ctrlX, cy - 4, 2.5f, fill);
            canvas.drawPoint(ctrlX, cy + 4, fill);

            if (deviceType == DEVICE_LEFT) {
                p.setStrokeWidth(2);
                canvas.drawArc(new RectF(ctrlX - w/2 - 4, cy - 6, ctrlX + w/2 - 2, cy + 8), 180, 90, false, p);
            } else {
                p.setStrokeWidth(2);
                canvas.drawArc(new RectF(ctrlX - w/2 + 2, cy - 6, ctrlX + w/2 + 4, cy + 8), 270, 90, false, p);
            }
        }
    }

    private void renderBatteryIcon(float rightX, float centerY, int pct, boolean connected, int deviceType) {
        float iconW = 36;
        float iconH = 18;
        float pad = 8;
        float bx = rightX - iconW;
        float by = centerY - iconH / 2f;

        Paint battPaint = new Paint();
        battPaint.setAntiAlias(true);
        battPaint.setStyle(Paint.Style.STROKE);
        battPaint.setStrokeWidth(2);

        Paint fillPaint = new Paint();
        fillPaint.setAntiAlias(true);
        fillPaint.setStyle(Paint.Style.FILL);

        int color = batteryColor(pct);
        if (!connected) color = Color.rgb(100, 110, 125);

        renderDeviceIcon(bx - pad, centerY, deviceType, color);

        battPaint.setColor(color);
        fillPaint.setColor(color);

        if (!connected) {
            RectF body = new RectF(bx, by, bx + iconW - 4, by + iconH);
            canvas.drawRoundRect(body, 2, 2, battPaint);
            RectF cap = new RectF(bx + iconW - 4, by + 4, bx + iconW, by + iconH - 4);
            canvas.drawRect(cap, fillPaint);

            battPaint.setStrokeWidth(3);
            canvas.drawLine(bx + 2, by + 2, bx + iconW - 6, by + iconH - 2, battPaint);
            return;
        }

        RectF body = new RectF(bx, by, bx + iconW - 4, by + iconH);
        canvas.drawRoundRect(body, 2, 2, battPaint);
        RectF cap = new RectF(bx + iconW - 4, by + 4, bx + iconW, by + iconH - 4);
        canvas.drawRect(cap, fillPaint);

        float fillW = (iconW - 8) * (pct / 100f);
        RectF fill = new RectF(bx + 2, by + 2, bx + 2 + fillW, by + iconH - 2);
        canvas.drawRect(fill, fillPaint);

        String pctText = pct + "%";
        textSmallPaint.setColor(color);
        float textW = textSmallPaint.measureText(pctText);
        canvas.drawText(pctText, bx - pad - textW - 28, centerY + 7, textSmallPaint);
        textSmallPaint.setColor(Color.rgb(160, 170, 185));
    }

    private void renderTopBar() {
        float contentX = SIDEBAR_WIDTH;
        float contentW = width - contentX;

        canvas.drawRect(contentX, 0, width, TOPBAR_HEIGHT, topbarBgPaint);
        canvas.drawRect(contentX, TOPBAR_HEIGHT, width, TOPBAR_HEIGHT + 1, topbarBorderPaint);

        float rightX = width - 25;
        renderWifiIcon(rightX, TOPBAR_HEIGHT / 2f);
        rightX -= 60;
        renderBatteryIcon(rightX, TOPBAR_HEIGHT / 2f, rightBattery, rightConnected, DEVICE_RIGHT);
        rightX -= 130;
        renderBatteryIcon(rightX, TOPBAR_HEIGHT / 2f, leftBattery, leftConnected, DEVICE_LEFT);
        rightX -= 130;
        renderBatteryIcon(rightX, TOPBAR_HEIGHT / 2f, hmdBattery, true, DEVICE_HMD);
    }

    public void startDiscovery() {
        try {
            if (nsdManager == null) {
                nsdManager = (NsdManager) context.getSystemService(Context.NSD_SERVICE);
            }
            if (nsdListener != null) {
                nsdManager.stopServiceDiscovery(nsdListener);
                nsdListener = null;
            }
            nsdListener = new NsdManager.DiscoveryListener() {
                @Override
                public void onStartDiscoveryFailed(String serviceType, int errorCode) {
                    Log.e(TAG, "NSD discovery start failed: " + errorCode);
                }

                @Override
                public void onStopDiscoveryFailed(String serviceType, int errorCode) {
                    Log.e(TAG, "NSD discovery stop failed: " + errorCode);
                }

                @Override
                public void onDiscoveryStarted(String serviceType) {
                    Log.i(TAG, "NSD discovery started for " + serviceType);
                }

                @Override
                public void onDiscoveryStopped(String serviceType) {
                    Log.i(TAG, "NSD discovery stopped for " + serviceType);
                }

                @Override
                public void onServiceFound(NsdServiceInfo serviceInfo) {
                    Log.i(TAG, "NSD service found: " + serviceInfo.getServiceName());
                    nsdManager.resolveService(serviceInfo, new NsdManager.ResolveListener() {
                        @Override
                        public void onResolveFailed(NsdServiceInfo serviceInfo, int errorCode) {
                            Log.e(TAG, "NSD resolve failed: " + serviceInfo.getServiceName() + " err=" + errorCode);
                        }

                        @Override
                        public void onServiceResolved(NsdServiceInfo serviceInfo) {
                            String name = serviceInfo.getServiceName();
                            InetAddress host = serviceInfo.getHost();
                            int port = serviceInfo.getPort();
                            String hostname = host != null ? host.getHostAddress() : serviceInfo.getServiceName();

                            Map<String, byte[]> txt = serviceInfo.getAttributes();
                            boolean tcpOnly = false;
                            if (txt != null && txt.containsKey("tcp_only")) {
                                byte[] val = txt.get("tcp_only");
                                if (val != null && new String(val).equals("1"))
                                    tcpOnly = true;
                            }

                            ServerEntry entry = new ServerEntry(name, hostname, port, tcpOnly, false, true);
                            synchronized (discoveredServers) {
                                discoveredServers.put(name, entry);
                            }
                            Log.i(TAG, "NSD resolved: " + name + " at " + hostname + ":" + port);
                            markDirty();
                        }
                    });
                }

                @Override
                public void onServiceLost(NsdServiceInfo serviceInfo) {
                    Log.i(TAG, "NSD service lost: " + serviceInfo.getServiceName());
                    synchronized (discoveredServers) {
                        discoveredServers.remove(serviceInfo.getServiceName());
                    }
                    markDirty();
                }
            };
            nsdManager.discoverServices("_wivrn._tcp", NsdManager.PROTOCOL_DNS_SD, nsdListener);
        } catch (Exception e) {
            Log.e(TAG, "Failed to start NSD discovery", e);
        }
    }

    public void stopDiscovery() {
        try {
            if (nsdManager != null && nsdListener != null) {
                nsdManager.stopServiceDiscovery(nsdListener);
                nsdListener = null;
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to stop NSD discovery", e);
        }
    }

    private List<ServerEntry> getAllServers() {
        List<ServerEntry> all = new ArrayList<>();
        synchronized (discoveredServers) {
            all.addAll(discoveredServers.values());
        }
        for (ServerEntry s : servers) {
            if (!all.stream().anyMatch(d -> d.hostname.equals(s.hostname) && d.port == s.port)) {
                all.add(s);
            }
        }
        return all;
    }

    public List<ServerEntry> getAllServersPublic() {
        return getAllServers();
    }

    private void initPaints() {
        bgPaint.setColor(Color.rgb(18, 20, 28));
        sidebarBgPaint.setColor(Color.rgb(10, 11, 16));
        cardBgPaint.setColor(Color.rgb(28, 32, 42));
        accentPaint.setColor(Color.rgb(40, 140, 220));
        dimPaint.setColor(Color.rgb(60, 65, 75));

        textPaint.setColor(Color.rgb(230, 235, 245));
        textPaint.setTypeface(Typeface.DEFAULT);
        textPaint.setTextSize(28);
        textPaint.setAntiAlias(true);

        textSmallPaint.setColor(Color.rgb(160, 170, 185));
        textSmallPaint.setTypeface(Typeface.DEFAULT);
        textSmallPaint.setTextSize(22);
        textSmallPaint.setAntiAlias(true);

        textLargePaint.setColor(Color.rgb(240, 245, 255));
        textLargePaint.setTypeface(Typeface.DEFAULT_BOLD);
        textLargePaint.setTextSize(36);
        textLargePaint.setAntiAlias(true);

        textDimPaint.setColor(Color.rgb(100, 110, 125));
        textDimPaint.setTypeface(Typeface.DEFAULT);
        textDimPaint.setTextSize(24);
        textDimPaint.setAntiAlias(true);

        buttonBgPaint.setColor(Color.rgb(40, 50, 65));
        buttonHoverBgPaint.setColor(Color.rgb(60, 75, 95));
        buttonConnectBgPaint.setColor(Color.rgb(30, 140, 60));
        buttonDangerBgPaint.setColor(Color.rgb(140, 30, 30));

        pinDisplayPaint.setColor(Color.rgb(30, 35, 45));
        pinKeyBgPaint.setColor(Color.rgb(45, 55, 70));

        sliderTrackPaint.setColor(Color.rgb(50, 55, 65));
        sliderFillPaint.setColor(Color.rgb(40, 140, 220));
        sliderHandlePaint.setColor(Color.rgb(80, 160, 240));

        topbarBgPaint.setColor(Color.rgb(14, 16, 22));
        topbarBorderPaint.setColor(Color.rgb(40, 45, 55));
    }

    private void loadServers() {
        servers.clear();
        String json = prefs.getString("servers", "[]");
        try {
            JSONArray arr = new JSONArray(json);
            for (int i = 0; i < arr.length(); i++) {
                JSONObject obj = arr.getJSONObject(i);
                servers.add(new ServerEntry(
                    obj.optString("name", ""),
                    obj.optString("hostname", ""),
                    obj.optInt("port", 9757),
                    obj.optBoolean("tcpOnly", false),
                    true,
                    false,
                    obj.optBoolean("autoconnect", false)
                ));
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to load servers", e);
        }

        if (servers.isEmpty()) {
            servers.add(new ServerEntry("Local", "127.0.0.1", 9757, tcpOnly, true));
        }
    }

    private void saveServers() {
        try {
            JSONArray arr = new JSONArray();
            for (ServerEntry s : servers) {
                JSONObject obj = new JSONObject();
                obj.put("name", s.name);
                obj.put("hostname", s.hostname);
                obj.put("port", s.port);
                obj.put("tcpOnly", s.tcpOnly);
                obj.put("autoconnect", s.autoconnect);
                arr.put(obj);
            }
            prefs.edit().putString("servers", arr.toString()).apply();
        } catch (Exception e) {
            Log.e(TAG, "Failed to save servers", e);
        }
    }

    private void loadSettings() {
        SharedPreferences sp = context.getSharedPreferences("wivrn_settings", Context.MODE_PRIVATE);
        resWidth = sp.getInt("res_width", 1664);
        foveationScale = sp.getInt("foveation_scale", 30);
        codec = sp.getString("codec", "auto");
        bitrate = sp.getInt("bitrate", 50);
        try {
            ipdMm = sp.getFloat("ipd_mm", 64);
        } catch (ClassCastException e) {
            ipdMm = sp.getInt("ipd_mm", 64);
            sp.edit().putFloat("ipd_mm", ipdMm).apply();
        }
        fovDeg = sp.getFloat("fov_deg", 101);
        tcpOnly = sp.getBoolean("tcp_only", false);
        microphoneEnabled = sp.getBoolean("microphone", false);
        streamBitrateSetting = sp.getInt("stream_bitrate", 50);
        streamResolutionScale = sp.getInt("stream_resolution_scale", 100);
        passthroughEnabled = sp.getBoolean("passthrough", true);
        languageSetting = sp.getInt("language", 0);
        brightnessFrac = sp.getFloat("brightness", 1.0f);
        ctrlVibration = sp.getFloat("ctrl_vibration", 1.0f);
        eyeFoveationEnabled = sp.getBoolean("eye_foveation", false);
        eyeDebugOn = sp.getBoolean("eye_debug", false);
        diagHudMode = sp.getInt("diag_hud", 0);
    }

    private void saveSettings() {
        SharedPreferences sp = context.getSharedPreferences("wivrn_settings", Context.MODE_PRIVATE);
        sp.edit()
            .putInt("res_width", resWidth)
            .putInt("foveation_scale", foveationScale)
            .putString("codec", codec)
            .putInt("bitrate", bitrate)
            .putFloat("ipd_mm", ipdMm)
            .putFloat("fov_deg", fovDeg)
            .putBoolean("tcp_only", tcpOnly)
            .putBoolean("microphone", microphoneEnabled)
            .putInt("stream_bitrate", streamBitrateSetting)
            .putInt("stream_resolution_scale", streamResolutionScale)
            .putBoolean("passthrough", passthroughEnabled)
            .putInt("language", languageSetting)
            .putFloat("brightness", brightnessFrac)
            .putFloat("ctrl_vibration", ctrlVibration)
            .putBoolean("eye_foveation", eyeFoveationEnabled)
            .putBoolean("eye_debug", eyeDebugOn)
            .putInt("diag_hud", diagHudMode)
            .apply();
    }

    public Bitmap getBitmap() {
        return bitmap;
    }

    public void setMicrophoneEnabled(boolean enabled) {
        microphoneEnabled = enabled;
        saveSettings();
        markDirty();
    }

    public void updateDebugSettings(float brightness, float ctrlVib, boolean eyeFov,
            boolean eyeDebug, int diagHud, boolean eyeSupp) {
        brightnessFrac = brightness;
        ctrlVibration = ctrlVib;
        eyeFoveationEnabled = eyeFov;
        eyeDebugOn = eyeDebug;
        diagHudMode = diagHud;
        eyeSupported = eyeSupp;
        markDirty();
    }

    public boolean isDirty() {
        return dirty;
    }

    public void markClean() {
        dirty = false;
    }

    public void markDirty() {
        dirty = true;
    }

    public void setConnectionState(int state, String message) {
        if (connectionState != state || !statusMessage.equals(message)) {
            connectionState = state;
            statusMessage = message != null ? message : "";
            if (state == STATE_IDLE || state == STATE_CONNECTING) {
                pinBuffer = "";
            }
            if (state == STATE_DISCONNECTED) {
                errorMessage = message != null ? message : "";
            }
            if (state == STATE_CONNECTED) {
                streamTab = STREAM_TAB_APPLICATIONS;
            }
            markDirty();
        }
    }

    public void updateStreamStats(int fps, int latencyMs, int bandwidthRxBps, int bandwidthTxBps, int bitrateMbps) {
        this.streamFps = fps;
        this.streamLatencyMs = latencyMs;
        this.streamBandwidthRx = bandwidthRxBps;
        this.streamBandwidthTx = bandwidthTxBps;
        this.streamBitrateMbps = bitrateMbps;
        if (connectionState == STATE_CONNECTED) {
            markDirty();
        }
    }

    public void updateStreamStatsDetailed(float[] data) {
        if (data == null || data.length < 13)
            return;

        this.streamFps = (int) data[0];
        this.streamTotalLatencyMs = data[1];
        this.streamLatencyMs = (int) data[1];
        this.streamBandwidthRx = data[2];
        this.streamBandwidthTx = data[3];
        this.streamBitrateMbps = (int) data[4];
        this.streamCpuMs = (int) data[5];
        this.streamGpuMs = (int) data[6];
        this.streamEncodeMs = data[7];
        this.streamSendMs = data[8];
        this.streamNetworkMs = data[9];
        this.streamDecodeMs = data[10];
        this.streamRenderWaitMs = data[11];
        this.streamBlitMs = data[12];

        fpsHistory[statsHistoryOffset] = data[0];
        latencyHistory[statsHistoryOffset] = data[1];
        bwRxHistory[statsHistoryOffset] = data[2] * 1e-6f;
        bwTxHistory[statsHistoryOffset] = data[3] * 1e-6f;
        cpuTimeHistory[statsHistoryOffset] = data[5];
        gpuTimeHistory[statsHistoryOffset] = data[6];
        statsHistoryOffset = (statsHistoryOffset + 1) % STATS_HISTORY_SIZE;
        if (statsHistoryCount < STATS_HISTORY_SIZE)
            statsHistoryCount++;

        if (connectionState == STATE_CONNECTED) {
            markDirty();
        }
    }

    public void updateRunningApps(String[] names, int[] ids, boolean[] overlays, boolean[] actives) {
        this.runningApps = names != null ? names : new String[0];
        this.runningAppIds = ids != null ? ids : new int[0];
        this.runningAppOverlays = overlays != null ? overlays : new boolean[0];
        this.runningAppActives = actives != null ? actives : new boolean[0];
        launchingAppName = null;
        if (connectionState == STATE_CONNECTED) {
            markDirty();
        }
    }

    public void updateAvailableApps(String[] ids, String[] names) {
        this.availableAppIds = ids != null ? ids : new String[0];
        this.availableAppNames = names != null ? names : new String[0];
        appListRequested = false;
        for (String id : this.availableAppIds) {
            if (!appIcons.containsKey(id)) {
                appIcons.put(id, null);
            }
        }
        appIcons.keySet().retainAll(java.util.Arrays.asList(this.availableAppIds));
        if (connectionState == STATE_CONNECTED) {
            markDirty();
        }
    }

    public void updateAppIcon(String appId, byte[] pngData) {
        if (pngData != null && pngData.length > 0) {
            Bitmap icon = BitmapFactory.decodeByteArray(pngData, 0, pngData.length);
            if (icon != null) {
                appIcons.put(appId, icon);
                if (connectionState == STATE_CONNECTED) {
                    markDirty();
                }
            }
        }
    }

    public int getResWidth() { return resWidth; }

    public void applyResolution() {
        int renderW = Math.max(256, resWidth);
        renderW = (renderW / 2) * 2;
        int renderH = renderW * 2160 / 2048;
        renderH = (renderH / 2) * 2;

        int streamW = Math.max(256, (int)(renderW * streamResolutionScale / 100f));
        streamW = (streamW / 2) * 2;
        int streamH = streamW * 2160 / 2048;
        streamH = (streamH / 2) * 2;

        if (context instanceof MainActivity) {
            ((MainActivity) context).onRenderResolutionChanged(renderW, renderH);
            ((MainActivity) context).onStreamResolutionChanged(streamW, streamH);
        }
    }

    public int getFoveationScale() { return foveationScale; }
    public String getCodec() { return codec; }
    public int getBitrate() { return bitrate; }
    public float getIpdMm() { return ipdMm; }
    public boolean isTcpOnly() { return tcpOnly; }
    public boolean isMicrophoneEnabled() { return microphoneEnabled; }

    public void render() {
        canvas.drawRect(0, 0, width, height, bgPaint);

        if (connectionState == STATE_PIN_ENTRY) {
            renderPinEntry();
        } else if (connectionState == STATE_CONNECTING) {
            renderConnecting();
        } else if (connectionState == STATE_DISCONNECTED) {
            renderDisconnected();
        } else if (connectionState == STATE_CONNECTED) {
            renderConnected();
        } else {
            renderSidebar();
            renderTopBar();
            renderContent();
        }

        renderTouchCursor();
        markClean();
    }

    private void renderSidebar() {
        canvas.drawRect(0, 0, SIDEBAR_WIDTH, height, sidebarBgPaint);

        String[] tabs = {
            i18n.s(R.string.tab_server_list),
            i18n.s(R.string.tab_settings),
            i18n.s(R.string.tab_about),
            i18n.s(R.string.tab_licenses),
            i18n.s(R.string.tab_exit)
        };
        int[] tabIds = {TAB_SERVER_LIST, TAB_SETTINGS, TAB_ABOUT, TAB_LICENSES, TAB_EXIT};

        float y = 30;
        for (int i = 0; i < tabs.length; i++) {
            boolean selected = currentTab == tabIds[i];
            RectF rect = new RectF(10, y, SIDEBAR_WIDTH - 10, y + TAB_HEIGHT);

            if (selected) {
                canvas.drawRoundRect(rect, 10, 10, accentPaint);
                textPaint.setColor(Color.WHITE);
            } else {
                if (touchDown && rect.contains(touchX, touchY)) {
                    canvas.drawRoundRect(rect, 10, 10, buttonHoverBgPaint);
                }
                textPaint.setColor(Color.rgb(160, 170, 185));
            }

            Paint.FontMetrics fm = textPaint.getFontMetrics();
            float textY = y + (TAB_HEIGHT - (fm.descent - fm.ascent)) / 2 - fm.ascent;
            canvas.drawText(tabs[i], 30, textY, textPaint);
            textPaint.setColor(Color.rgb(230, 235, 245));

            y += TAB_HEIGHT + 10;
        }
    }

    private void renderContent() {
        float contentX = SIDEBAR_WIDTH + 20;
        float contentW = width - contentX - 20;

        canvas.save();
        canvas.translate(0, TOPBAR_HEIGHT + 10);

        switch (currentTab) {
            case TAB_SERVER_LIST:
                renderServerList(contentX, contentW);
                break;
            case TAB_SETTINGS:
                renderSettings(contentX, contentW);
                break;
            case TAB_ABOUT:
                renderAbout(contentX, contentW);
                break;
            case TAB_LICENSES:
                renderLicenses(contentX, contentW);
                break;
            case TAB_EXIT:
                renderExit(contentX, contentW);
                break;
        }

        canvas.restore();
    }

    private void renderServerList(float x, float w) {
        float y = 30;

        textLargePaint.setColor(Color.rgb(240, 245, 255));
        canvas.drawText(i18n.s(R.string.server_list_title), x, y + 30, textLargePaint);
        y += 60;

        RectF addBtn = new RectF(x + w - BUTTON_WIDTH, y - BUTTON_HEIGHT, x + w, y);
        boolean addHover = touchDown && addBtn.contains(touchX, touchY);
        canvas.drawRoundRect(addBtn, 10, 10, addHover ? buttonHoverBgPaint : buttonBgPaint);
        drawCenteredText(i18n.s(R.string.add_server), addBtn, textPaint);

        y += 30;

        List<ServerEntry> allServers = getAllServers();
        for (int i = 0; i < allServers.size(); i++) {
            ServerEntry s = allServers.get(i);
            float cardY = y + i * (CARD_HEIGHT + 10);
            RectF card = new RectF(x, cardY, x + w, cardY + CARD_HEIGHT);

            canvas.drawRoundRect(card, 12, 12, cardBgPaint);

            canvas.drawText(s.name, x + 20, cardY + 35, textPaint);
            canvas.drawText(s.hostname + ":" + s.port + (s.tcpOnly ? " (" + i18n.s(R.string.tcp) + ")" : " (" + i18n.s(R.string.udp) + ")"), x + 20, cardY + 65, textSmallPaint);

            if (s.discovered) {
                textSmallPaint.setColor(Color.rgb(80, 200, 120));
                canvas.drawText(i18n.s(R.string.discovered), x + 20, cardY + 88, textSmallPaint);
                textSmallPaint.setColor(Color.rgb(160, 170, 185));
            }

            if (s.autoconnect) {
                textSmallPaint.setColor(Color.rgb(255, 200, 60));
                canvas.drawText(i18n.s(R.string.autoconnect), x + 20, cardY + 88, textSmallPaint);
                textSmallPaint.setColor(Color.rgb(160, 170, 185));
            }

            RectF connectBtn = new RectF(x + w - BUTTON_WIDTH - 20, cardY + 20, x + w - 20, cardY + 20 + BUTTON_HEIGHT);
            boolean connectHover = touchDown && connectBtn.contains(touchX, touchY);
            canvas.drawRoundRect(connectBtn, 10, 10, buttonConnectBgPaint);
            drawCenteredText(i18n.s(R.string.connect), connectBtn, textPaint);

            float starX = connectBtn.left - 50;
            RectF starBtn = new RectF(starX, cardY + 20, starX + 40, cardY + 20 + BUTTON_HEIGHT);
            boolean starHover = touchDown && starBtn.contains(touchX, touchY);
            textPaint.setColor(s.autoconnect ? Color.rgb(255, 200, 60) : Color.rgb(100, 110, 125));
            drawCenteredText("★", starBtn, textPaint);
            textPaint.setColor(Color.rgb(230, 235, 245));

            if (s.manual) {
                RectF delBtn = new RectF(starBtn.left - 60, cardY + 20, starBtn.left - 10, cardY + 20 + BUTTON_HEIGHT);
                boolean delHover = touchDown && delBtn.contains(touchX, touchY);
                canvas.drawRoundRect(delBtn, 10, 10, buttonDangerBgPaint);
                drawCenteredText("X", delBtn, textPaint);
            }
        }

        if (showAddServer) {
            renderAddServerPopup(x, w);
        }
    }

    private void renderAddServerPopup(float x, float w) {
        float popupW = 600;
        float popupH = 400;
        float popupX = (width - popupW) / 2;
        float popupY = (height - popupH) / 2;

        RectF popup = new RectF(popupX, popupY, popupX + popupW, popupY + popupH);
        canvas.drawRoundRect(popup, 16, 16, cardBgPaint);

        float py = popupY + 30;
        canvas.drawText(i18n.s(R.string.add_server_title), popupX + 30, py + 20, textLargePaint);
        py += 60;

        String[] labels = {i18n.s(R.string.field_name), i18n.s(R.string.field_address), i18n.s(R.string.field_port)};
        String[] values = {addServerName, addServerAddress, addServerPort};
        for (int i = 0; i < 3; i++) {
            canvas.drawText(labels[i], popupX + 30, py + 20, textSmallPaint);
            RectF field = new RectF(popupX + 180, py, popupX + popupW - 30, py + 40);
            boolean focused = addServerFieldFocus == i;
            canvas.drawRoundRect(field, 6, 6, focused ? accentPaint : dimPaint);
            canvas.drawRoundRect(new RectF(field.left + 2, field.top + 2, field.right - 2, field.bottom - 2), 4, 4, bgPaint);
            canvas.drawText(values[i], field.left + 10, py + 28, textPaint);
            py += 55;
        }

        RectF tcpCheck = new RectF(popupX + 30, py, popupX + 50, py + 20);
        canvas.drawRoundRect(tcpCheck, 4, 4, addServerTcpOnly ? accentPaint : dimPaint);
        if (addServerTcpOnly) {
            canvas.drawText("X", tcpCheck.left + 12, py + 16, textPaint);
        }
        canvas.drawText(i18n.s(R.string.tcp_only), popupX + 60, py + 16, textPaint);
        py += 50;

        RectF cancelBtn = new RectF(popupX + 30, py, popupX + 30 + 150, py + BUTTON_HEIGHT);
        boolean cancelHover = touchDown && cancelBtn.contains(touchX, touchY);
        canvas.drawRoundRect(cancelBtn, 10, 10, cancelHover ? buttonHoverBgPaint : buttonBgPaint);
        drawCenteredText(i18n.s(R.string.cancel), cancelBtn, textPaint);

        RectF saveBtn = new RectF(popupX + popupW - 180, py, popupX + popupW - 30, py + BUTTON_HEIGHT);
        boolean saveHover = touchDown && saveBtn.contains(touchX, touchY);
        canvas.drawRoundRect(saveBtn, 10, 10, saveHover ? buttonConnectBgPaint : buttonConnectBgPaint);
        drawCenteredText(i18n.s(R.string.save), saveBtn, textPaint);
    }

    private void renderSettings(float x, float w) {
        float contentTop = TOPBAR_HEIGHT + 10;
        float contentH = height - contentTop - 10;

        canvas.save();
        canvas.clipRect(0, contentTop, width, contentTop + contentH);
        canvas.translate(0, -settingsScrollY);

        float y = 30;

        canvas.drawText(i18n.s(R.string.settings_title), x, y + 30, textLargePaint);
        y += 70;

        y = drawResolutionSlider(x, y, w, i18n.s(R.string.setting_resolution), resWidth, 1024, 2048);
        y = drawSlider(x, y, w, i18n.s(R.string.setting_foveated_encoding), foveationScale, 0, 80, "%", true);
        y = drawSlider(x, y, w, i18n.s(R.string.setting_bitrate), bitrate, 5, 200, i18n.s(R.string.unit_mbit_s), false);
        y = drawSliderFloat(x, y, w, i18n.s(R.string.setting_ipd), ipdMm, 58, 72, i18n.s(R.string.unit_mm), false, 1);
        y = drawSliderFloat(x, y, w, i18n.s(R.string.setting_fov), fovDeg, 70, 101, "°", false, 0);

        y = drawDropdown(x, y, w, i18n.s(R.string.setting_codec), new String[]{i18n.s(R.string.codec_auto), i18n.s(R.string.codec_h264), i18n.s(R.string.codec_h265)},
            codec.equals("auto") ? 0 : codec.equals("h264") ? 1 : 2, true);

        y = drawCheckbox(x, y, w, i18n.s(R.string.tcp_only), tcpOnly, false);
        y = drawCheckbox(x, y, w, i18n.s(R.string.setting_microphone), microphoneEnabled, false);
        y = drawCheckbox(x, y, w, "PASSTHROUGH", passthroughEnabled, false);

        y = drawDropdown(x, y, w, i18n.s(R.string.setting_language),
            new String[]{i18n.s(R.string.lang_system), "English", "简体中文"},
            languageSetting, false);

        y += 15;
        canvas.drawText("Debug", x, y + 30, textLargePaint);
        y += 55;

        y = drawSliderFloat(x, y, w, "Brightness", brightnessFrac, 0, 1, "%", false, 0);
        y = drawSliderFloat(x, y, w, "Controller Vibration", ctrlVibration, 0, 1, "%", false, 0);
        y = drawCheckbox(x, y, w, "Eye-tracked Foveation", eyeFoveationEnabled, !eyeSupported);
        y = drawCheckbox(x, y, w, "Eye Debug", eyeDebugOn, false);
        y = drawDropdown(x, y, w, "Diagnostics HUD",
            new String[]{"Off", "Pipeline", "System"}, diagHudMode, false);

        y += 10;
        RectF recenterBtn = new RectF(x, y, x + 200, y + BUTTON_HEIGHT);
        boolean recenterHover = touchDown && recenterBtn.contains(touchX, touchY + settingsScrollY);
        canvas.drawRoundRect(recenterBtn, 10, 10, recenterHover ? buttonHoverBgPaint : buttonBgPaint);
        drawCenteredText("Recenter", recenterBtn, textPaint);
        y += BUTTON_HEIGHT + 10;

        y += 20;
        RectF resetBtn = new RectF(x, y, x + 200, y + BUTTON_HEIGHT);
        boolean resetHover = touchDown && resetBtn.contains(touchX, touchY + settingsScrollY);
        canvas.drawRoundRect(resetBtn, 10, 10, resetHover ? buttonDangerBgPaint : buttonDangerBgPaint);
        drawCenteredText(i18n.s(R.string.restore_defaults), resetBtn, textPaint);

        float totalContentH = y + BUTTON_HEIGHT + 20;
        settingsMaxScroll = Math.max(0, totalContentH - contentH);
        settingsScrollY = Math.max(0, Math.min(settingsScrollY, settingsMaxScroll));

        canvas.restore();

        if (showResetConfirm) {
            renderResetConfirmDialog();
        }
    }

    private void renderResetConfirmDialog() {
        float panelW = 500;
        float panelH = 200;
        float px = (width - panelW) / 2;
        float py = (height - panelH) / 2;

        canvas.drawRect(0, 0, width, height, new Paint() {{ setColor(Color.argb(160, 0, 0, 0)); }});
        canvas.drawRoundRect(px, py, px + panelW, py + panelH, 12, 12, cardBgPaint);

        canvas.drawText(i18n.s(R.string.reset_confirm), px + 30, py + 50, textPaint);

        RectF yesBtn = new RectF(px + 30, py + panelH - 70, px + 30 + 180, py + panelH - 20);
        boolean yesHover = touchDown && yesBtn.contains(touchX, touchY);
        canvas.drawRoundRect(yesBtn, 10, 10, yesHover ? buttonDangerBgPaint : buttonDangerBgPaint);
        drawCenteredText(i18n.s(R.string.reset), yesBtn, textPaint);

        RectF noBtn = new RectF(px + panelW - 210, py + panelH - 70, px + panelW - 30, py + panelH - 20);
        boolean noHover = touchDown && noBtn.contains(touchX, touchY);
        canvas.drawRoundRect(noBtn, 10, 10, noHover ? buttonHoverBgPaint : buttonBgPaint);
        drawCenteredText(i18n.s(R.string.cancel), noBtn, textPaint);
    }

    private float drawSlider(float x, float y, float w, String label, int value, int min, int max, String unit) {
        return drawSlider(x, y, w, label, value, min, max, unit, false);
    }

    private float drawSlider(float x, float y, float w, String label, int value, int min, int max, String unit, boolean disabled) {
        return drawSliderFloat(x, y, w, label, (float)value, min, max, unit, disabled, 0);
    }

    private float drawSliderFloat(float x, float y, float w, String label, float value, float min, float max, String unit, boolean disabled, int decimals) {
        int prevTextColor = textPaint.getColor();
        int prevSmallColor = textSmallPaint.getColor();
        if (disabled) {
            textPaint.setColor(Color.rgb(90, 95, 105));
            textSmallPaint.setColor(Color.rgb(70, 75, 85));
        }
        canvas.drawText(label, x, y + 20, textPaint);
        y += 35;

        float sliderW = w - 100;
        RectF track = new RectF(x, y, x + sliderW, y + 8);
        canvas.drawRoundRect(track, 4, 4, disabled ? dimPaint : sliderTrackPaint);

        float pct = (value - min) / (max - min);
        RectF fill = new RectF(x, y, x + sliderW * pct, y + 8);
        if (!disabled)
            canvas.drawRoundRect(fill, 4, 4, sliderFillPaint);

        float handleX = x + sliderW * pct;
        Paint handlePaint = disabled ? new Paint() {{ setAntiAlias(true); setColor(Color.rgb(80, 85, 95)); }} : sliderHandlePaint;
        canvas.drawCircle(handleX, y + 4, 14, handlePaint);

        String fmt = decimals > 0 ? "%." + decimals + "f%s" : "%.0f%s";
        canvas.drawText(String.format(fmt, value, unit), x + sliderW + 15, y + 12, textSmallPaint);

        y += 30;
        textPaint.setColor(prevTextColor);
        textSmallPaint.setColor(prevSmallColor);
        return y + 20;
    }

    private float drawResolutionSlider(float x, float y, float w, String label, int resW, int minW, int maxW) {
        return drawResolutionSlider(x, y, w, label, resW, minW, maxW, false);
    }

    private float drawResolutionSlider(float x, float y, float w, String label, int resW, int minW, int maxW, boolean disabled) {
        int resH = resW * 2160 / 2048;
        int prevTextColor = textPaint.getColor();
        int prevSmallColor = textSmallPaint.getColor();
        if (disabled) {
            textPaint.setColor(Color.rgb(90, 95, 105));
            textSmallPaint.setColor(Color.rgb(70, 75, 85));
        }
        canvas.drawText(label, x, y + 20, textPaint);
        y += 35;

        float sliderW = w - 130;
        RectF track = new RectF(x, y, x + sliderW, y + 8);
        canvas.drawRoundRect(track, 4, 4, disabled ? dimPaint : sliderTrackPaint);

        float pct = (float)(resW - minW) / (maxW - minW);
        RectF fill = new RectF(x, y, x + sliderW * pct, y + 8);
        if (!disabled)
            canvas.drawRoundRect(fill, 4, 4, sliderFillPaint);

        float handleX = x + sliderW * pct;
        Paint handlePaint = disabled ? new Paint() {{ setAntiAlias(true); setColor(Color.rgb(80, 85, 95)); }} : sliderHandlePaint;
        canvas.drawCircle(handleX, y + 4, 14, handlePaint);

        canvas.drawText(resW + "x" + resH, x + sliderW + 15, y + 12, textSmallPaint);

        y += 30;
        textPaint.setColor(prevTextColor);
        textSmallPaint.setColor(prevSmallColor);
        return y + 20;
    }

    private float drawCheckbox(float x, float y, float w, String label, boolean checked) {
        return drawCheckbox(x, y, w, label, checked, false);
    }

    private float drawCheckbox(float x, float y, float w, String label, boolean checked, boolean disabled) {
        int prevTextColor = textPaint.getColor();
        if (disabled) {
            textPaint.setColor(Color.rgb(90, 95, 105));
        }
        RectF box = new RectF(x, y, x + 24, y + 24);
        canvas.drawRoundRect(box, 4, 4, disabled ? dimPaint : (checked ? accentPaint : dimPaint));
        if (checked && !disabled) {
            canvas.drawText("X", x + 6, y + 18, textPaint);
        }
        canvas.drawText(label, x + 35, y + 20, textPaint);
        textPaint.setColor(prevTextColor);
        return y + 40;
    }

    private float drawDropdown(float x, float y, float w, String label, String[] options, int selected) {
        return drawDropdown(x, y, w, label, options, selected, false);
    }

    private float drawDropdown(float x, float y, float w, String label, String[] options, int selected, boolean disabled) {
        int prevTextColor = textPaint.getColor();
        int prevSmallColor = textSmallPaint.getColor();
        if (disabled) {
            textPaint.setColor(Color.rgb(90, 95, 105));
            textSmallPaint.setColor(Color.rgb(70, 75, 85));
        }
        canvas.drawText(label, x, y + 20, textPaint);
        y += 35;

        String selectedText = options[selected];
        RectF box = new RectF(x, y, x + 300, y + 40);
        canvas.drawRoundRect(box, 6, 6, dimPaint);
        canvas.drawText(selectedText, x + 15, y + 28, textPaint);
        canvas.drawText("v", x + 280, y + 28, textSmallPaint);

        textPaint.setColor(prevTextColor);
        textSmallPaint.setColor(prevSmallColor);
        return y + 50;
    }

    private RectF gitlabBtnRect;

    private void renderAbout(float x, float w) {
        float y = 30;
        canvas.drawText(i18n.s(R.string.about_wivrn), x, y + 30, textLargePaint);
        y += 70;

        canvas.drawText(i18n.s(R.string.about_unofficial), x, y, textPaint);
        y += 40;

        canvas.drawText(i18n.s(R.string.about_description), x, y, textSmallPaint);
        y += 35;
        canvas.drawText(i18n.s(R.string.about_based_on), x, y, textSmallPaint);
        y += 35;
        canvas.drawText(i18n.s(R.string.about_maintainer), x, y, textSmallPaint);
        y += 35;
        canvas.drawText(i18n.s(R.string.about_license), x, y, textSmallPaint);
        y += 35;

        String versionName = "";
        int versionCode = 0;
        try {
            PackageInfo pi = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
            versionName = pi.versionName;
            versionCode = pi.versionCode;
        } catch (Exception e) {
            Log.w(TAG, "Failed to get package info", e);
        }
        canvas.drawText(i18n.s(R.string.about_version, versionName, versionCode), x, y, textSmallPaint);
        y += 60;

        gitlabBtnRect = new RectF(x, y, x + 280, y + BUTTON_HEIGHT);
        boolean gitlabHover = touchDown && gitlabBtnRect.contains(touchX, touchY);
        canvas.drawRoundRect(gitlabBtnRect, 10, 10, gitlabHover ? buttonHoverBgPaint : buttonBgPaint);
        drawCenteredText(i18n.s(R.string.gitlab_repo), gitlabBtnRect, textPaint);
    }

    private void renderLicenses(float x, float w) {
        float y = 30;
        canvas.drawText(i18n.s(R.string.licenses_title), x, y + 30, textLargePaint);
        y += 70;

        String licenseText = i18n.s(R.string.license_gpl_text);

        textSmallPaint.setColor(Color.rgb(180, 190, 200));
        android.text.StaticLayout sl = new android.text.StaticLayout(
            licenseText,
            new android.text.TextPaint(textSmallPaint),
            (int) w,
            android.text.Layout.Alignment.ALIGN_NORMAL,
            1.0f,
            0.0f,
            false
        );
        canvas.save();
        canvas.translate(x, y);
        sl.draw(canvas);
        canvas.restore();
        textSmallPaint.setColor(Color.rgb(160, 170, 185));
    }

    private void renderExit(float x, float w) {
        float y = height / 2 - 50;
        canvas.drawText(i18n.s(R.string.exit_confirm), x + 50, y, textLargePaint);
        y += 60;

        RectF yesBtn = new RectF(x + 50, y, x + 50 + 200, y + BUTTON_HEIGHT);
        boolean yesHover = touchDown && yesBtn.contains(touchX, touchY);
        canvas.drawRoundRect(yesBtn, 10, 10, yesHover ? buttonDangerBgPaint : buttonDangerBgPaint);
        drawCenteredText(i18n.s(R.string.exit), yesBtn, textPaint);

        RectF noBtn = new RectF(x + 270, y, x + 270 + 200, y + BUTTON_HEIGHT);
        boolean noHover = touchDown && noBtn.contains(touchX, touchY);
        canvas.drawRoundRect(noBtn, 10, 10, noHover ? buttonHoverBgPaint : buttonBgPaint);
        drawCenteredText(i18n.s(R.string.cancel), noBtn, textPaint);
    }

    private void renderPinEntry() {
        canvas.drawRect(0, 0, width, height, bgPaint);

        float panelW = 600;
        float panelH = 500;
        float px = (width - panelW) / 2;
        float py = (height - panelH) / 2;

        RectF panel = new RectF(px, py, px + panelW, py + panelH);
        canvas.drawRoundRect(panel, 16, 16, cardBgPaint);

        canvas.drawText(i18n.s(R.string.enter_pin), px + 30, py + 40, textLargePaint);
        canvas.drawText(i18n.s(R.string.pin_hint), px + 30, py + 70, textSmallPaint);

        RectF pinDisplay = new RectF(px + 30, py + 90, px + panelW - 30, py + 140);
        canvas.drawRoundRect(pinDisplay, 8, 8, pinDisplayPaint);
        String displayPin = pinBuffer.isEmpty() ? i18n.s(R.string.pin_placeholder) : pinBuffer;
        Paint.FontMetrics fm = textLargePaint.getFontMetrics();
        float textY = pinDisplay.top + (pinDisplay.height() - (fm.descent - fm.ascent)) / 2 - fm.ascent;
        float textX = pinDisplay.left + (pinDisplay.width() - textLargePaint.measureText(displayPin)) / 2;
        canvas.drawText(displayPin, textX, textY, pinBuffer.isEmpty() ? textDimPaint : textLargePaint);

        float keyY = py + 170;
        float keySize = 130;
        float keyGap = 10;
        float keyStartX = px + (panelW - 3 * keySize - 2 * keyGap) / 2;

        for (int i = 1; i <= 9; i++) {
            int row = (i - 1) / 3;
            int col = (i - 1) % 3;
            float kx = keyStartX + col * (keySize + keyGap);
            float ky = keyY + row * (keySize + keyGap);
            RectF key = new RectF(kx, ky, kx + keySize, ky + keySize);
            boolean hover = touchDown && key.contains(touchX, touchY);
            canvas.drawRoundRect(key, 10, 10, hover ? buttonHoverBgPaint : pinKeyBgPaint);
            drawCenteredText(String.valueOf(i), key, textLargePaint);
        }

        float bottomY = keyY + 3 * (keySize + keyGap);
        RectF cancelKey = new RectF(keyStartX, bottomY, keyStartX + keySize, bottomY + keySize);
        boolean cancelHover = touchDown && cancelKey.contains(touchX, touchY);
        canvas.drawRoundRect(cancelKey, 10, 10, buttonDangerBgPaint);
        drawCenteredText("X", cancelKey, textLargePaint);

        RectF zeroKey = new RectF(keyStartX + keySize + keyGap, bottomY, keyStartX + 2 * keySize + keyGap, bottomY + keySize);
        boolean zeroHover = touchDown && zeroKey.contains(touchX, touchY);
        canvas.drawRoundRect(zeroKey, 10, 10, zeroHover ? buttonHoverBgPaint : pinKeyBgPaint);
        drawCenteredText("0", zeroKey, textLargePaint);

        RectF backKey = new RectF(keyStartX + 2 * (keySize + keyGap), bottomY, keyStartX + 3 * keySize + 2 * keyGap, bottomY + keySize);
        boolean backHover = touchDown && backKey.contains(touchX, touchY);
        canvas.drawRoundRect(backKey, 10, 10, backHover ? buttonHoverBgPaint : pinKeyBgPaint);
        drawCenteredText("<", backKey, textLargePaint);
    }

    private void renderConnecting() {
        canvas.drawRect(0, 0, width, height, bgPaint);

        float panelW = 600;
        float panelH = 250;
        float px = (width - panelW) / 2;
        float py = (height - panelH) / 2;

        RectF panel = new RectF(px, py, px + panelW, py + panelH);
        canvas.drawRoundRect(panel, 16, 16, cardBgPaint);

        canvas.drawText(i18n.s(R.string.connecting), px + 30, py + 50, textLargePaint);
        canvas.drawText(statusMessage, px + 30, py + 90, textPaint);

        RectF disconnectBtn = new RectF(px + panelW - 220, py + panelH - 80, px + panelW - 20, py + panelH - 20);
        boolean hover = touchDown && disconnectBtn.contains(touchX, touchY);
        canvas.drawRoundRect(disconnectBtn, 10, 10, hover ? buttonDangerBgPaint : buttonDangerBgPaint);
        drawCenteredText(i18n.s(R.string.disconnect), disconnectBtn, textPaint);
    }

    private void renderDisconnected() {
        canvas.drawRect(0, 0, width, height, bgPaint);

        float panelW = 600;
        float panelH = 250;
        float px = (width - panelW) / 2;
        float py = (height - panelH) / 2;

        RectF panel = new RectF(px, py, px + panelW, py + panelH);
        canvas.drawRoundRect(panel, 16, 16, cardBgPaint);

        canvas.drawText(i18n.s(R.string.disconnected), px + 30, py + 50, textLargePaint);
        canvas.drawText(errorMessage, px + 30, py + 90, textPaint);

        RectF reconnectBtn = new RectF(px + 20, py + panelH - 80, px + 220, py + panelH - 20);
        boolean reconnectHover = touchDown && reconnectBtn.contains(touchX, touchY);
        canvas.drawRoundRect(reconnectBtn, 10, 10, reconnectHover ? buttonHoverBgPaint : buttonBgPaint);
        drawCenteredText(i18n.s(R.string.reconnect), reconnectBtn, textPaint);

        RectF closeBtn = new RectF(px + panelW - 220, py + panelH - 80, px + panelW - 20, py + panelH - 20);
        boolean hover = touchDown && closeBtn.contains(touchX, touchY);
        canvas.drawRoundRect(closeBtn, 10, 10, hover ? buttonHoverBgPaint : buttonBgPaint);
        drawCenteredText(i18n.s(R.string.close), closeBtn, textPaint);
    }

    private void renderConnected() {
        canvas.drawRect(0, 0, width, height, bgPaint);

        canvas.drawRect(0, 0, SIDEBAR_WIDTH, height, sidebarBgPaint);

        String[] tabs = {
            i18n.s(R.string.stream_applications),
            i18n.s(R.string.stream_launch),
            i18n.s(R.string.stream_stats),
            i18n.s(R.string.stream_settings)
        };
        int[] tabIds = {STREAM_TAB_APPLICATIONS, STREAM_TAB_LAUNCH, STREAM_TAB_STATS, STREAM_TAB_SETTINGS};

        float ty = 30;
        for (int i = 0; i < tabs.length; i++) {
            RectF rect = new RectF(10, ty, SIDEBAR_WIDTH - 10, ty + TAB_HEIGHT);
            boolean selected = streamTab == tabIds[i];
            boolean hover = touchDown && rect.contains(touchX, touchY);

            Paint tabPaint = new Paint();
            tabPaint.setAntiAlias(true);
            if (selected) {
                tabPaint.setColor(accentPaint.getColor());
                canvas.drawRoundRect(rect, 8, 8, tabPaint);
                textPaint.setColor(Color.rgb(255, 255, 255));
            } else if (hover) {
                tabPaint.setColor(Color.rgb(30, 38, 50));
                canvas.drawRoundRect(rect, 8, 8, tabPaint);
                textPaint.setColor(Color.rgb(200, 210, 225));
            } else {
                textPaint.setColor(Color.rgb(160, 170, 185));
            }

            canvas.drawText(tabs[i], 25, ty + TAB_HEIGHT / 2f + 10, textPaint);
            ty += TAB_HEIGHT + 5;
        }

        float disconnectY = height - TAB_HEIGHT - 30;
        RectF disconnectBtn = new RectF(10, disconnectY, SIDEBAR_WIDTH - 10, disconnectY + TAB_HEIGHT);
        boolean discHover = touchDown && disconnectBtn.contains(touchX, touchY);
        Paint discPaint = new Paint();
        discPaint.setAntiAlias(true);
        discPaint.setColor(discHover ? Color.rgb(180, 40, 40) : buttonDangerBgPaint.getColor());
        canvas.drawRoundRect(disconnectBtn, 8, 8, discPaint);
        textPaint.setColor(Color.rgb(255, 255, 255));
        canvas.drawText(i18n.s(R.string.disconnect), 25, disconnectY + TAB_HEIGHT / 2f + 10, textPaint);

        textPaint.setColor(Color.rgb(230, 235, 245));

        float contentX = SIDEBAR_WIDTH + 30;
        float contentW = width - contentX - 30;

        switch (streamTab) {
            case STREAM_TAB_APPLICATIONS:
                renderStreamApplications(contentX, contentW);
                break;
            case STREAM_TAB_LAUNCH:
                renderStreamLaunch(contentX, contentW);
                break;
            case STREAM_TAB_STATS:
                renderStreamStats(contentX, contentW);
                break;
            case STREAM_TAB_SETTINGS:
                canvas.save();
                canvas.translate(0, TOPBAR_HEIGHT + 10);
                renderSettings(SIDEBAR_WIDTH + 20, width - SIDEBAR_WIDTH - 40);
                canvas.restore();
                break;
        }
    }

    private void renderStreamApplications(float x, float w) {
        long now = System.currentTimeMillis();
        if (now - lastRunningAppsPoll > 1000) {
            lastRunningAppsPoll = now;
            ((MainActivity) context).onRequestRunningApps();
        }

        float y = 40;
        canvas.drawText(i18n.s(R.string.running_xr_apps), x, y + 30, textLargePaint);
        y += 70;

        if (runningApps == null || runningApps.length == 0) {
            textDimPaint.setColor(Color.rgb(100, 110, 125));
            canvas.drawText(i18n.s(R.string.no_apps_running), x, y + 20, textDimPaint);
            textDimPaint.setColor(Color.rgb(100, 110, 125));
            return;
        }

        boolean inOverlaySection = false;
        for (int i = 0; i < runningApps.length; i++) {
            boolean isOverlay = i < runningAppOverlays.length && runningAppOverlays[i];

            if (isOverlay && !inOverlaySection) {
                inOverlaySection = true;
                Paint sepPaint = new Paint();
                sepPaint.setColor(Color.rgb(60, 70, 85));
                canvas.drawRect(x, y, x + w, y + 1, sepPaint);
                y += 15;
                textDimPaint.setColor(Color.rgb(120, 130, 145));
                canvas.drawText(i18n.s(R.string.overlays), x, y + 20, textDimPaint);
                textDimPaint.setColor(Color.rgb(100, 110, 125));
                y += 35;
            }

            boolean isActive = !isOverlay && i < runningAppActives.length && runningAppActives[i];

            RectF card = new RectF(x, y, x + w, y + 70);
            boolean hover = touchDown && card.contains(touchX, touchY);

            Paint cardPaint = new Paint();
            cardPaint.setAntiAlias(true);
            if (hover && !isActive && !isOverlay) {
                cardPaint.setColor(Color.rgb(45, 55, 75));
            } else {
                cardPaint.setColor(cardBgPaint.getColor());
            }
            canvas.drawRoundRect(card, 10, 10, cardPaint);

            float textX = x + 20;
            if (isActive) {
                textPaint.setColor(Color.rgb(120, 200, 120));
                canvas.drawText("> ", textX, y + 45, textPaint);
                textX += 30;
            }
            textPaint.setColor(Color.rgb(230, 235, 245));
            String displayName = runningApps[i];
            float maxTextW = w - 100;
            if (textPaint.measureText(displayName) > maxTextW) {
                while (textPaint.measureText(displayName + "...") > maxTextW && displayName.length() > 0)
                    displayName = displayName.substring(0, displayName.length() - 1);
                displayName += "...";
            }
            canvas.drawText(displayName, textX, y + 45, textPaint);

            RectF stopBtn = new RectF(x + w - 60, y + 15, x + w - 15, y + 55);
            boolean stopHover = touchDown && stopBtn.contains(touchX, touchY);
            Paint stopPaint = new Paint();
            stopPaint.setAntiAlias(true);
            stopPaint.setColor(stopHover ? Color.rgb(200, 50, 50) : Color.rgb(80, 30, 30));
            canvas.drawRoundRect(stopBtn, 6, 6, stopPaint);
            textSmallPaint.setColor(Color.rgb(255, 255, 255));
            canvas.drawText("X", stopBtn.centerX() - 5, stopBtn.centerY() + 8, textSmallPaint);
            textSmallPaint.setColor(Color.rgb(160, 170, 185));

            y += 85;
        }
    }

    private void renderStreamLaunch(float x, float w) {
        float y = 40;
        canvas.drawText(i18n.s(R.string.launch_application), x, y + 30, textLargePaint);
        y += 70;

        if (availableAppNames == null || availableAppNames.length == 0) {
            textDimPaint.setColor(Color.rgb(100, 110, 125));
            if (appListRequested) {
                canvas.drawText(i18n.s(R.string.loading_apps), x, y + 20, textDimPaint);
            } else {
                canvas.drawText(i18n.s(R.string.no_apps_available), x, y + 20, textDimPaint);
                canvas.drawText(i18n.s(R.string.press_to_refresh), x, y + 60, textDimPaint);
                RectF refreshBtn = new RectF(x, y + 80, x + 200, y + 80 + BUTTON_HEIGHT);
                boolean hover = touchDown && refreshBtn.contains(touchX, touchY);
                canvas.drawRoundRect(refreshBtn, 10, 10, hover ? buttonHoverBgPaint : buttonBgPaint);
                textPaint.setColor(Color.rgb(230, 235, 245));
                canvas.drawText(i18n.s(R.string.refresh), x + 50, y + 80 + BUTTON_HEIGHT / 2f + 8, textPaint);
            }
            textDimPaint.setColor(Color.rgb(100, 110, 125));
            return;
        }

        float cardH = 90;
        float gap = 12;
        float contentHeight = availableAppNames.length * (cardH + gap) - gap;
        float visibleHeight = height - y - 20;
        launchMaxScroll = Math.max(0, contentHeight - visibleHeight);
        launchScrollY = Math.max(0, Math.min(launchMaxScroll, launchScrollY));

        int clipSave = canvas.save();
        canvas.clipRect(x - 5, y - 5, x + w + 5, height);

        for (int i = 0; i < availableAppNames.length; i++) {
            float cy = y + i * (cardH + gap) - launchScrollY;
            RectF card = new RectF(x, cy, x + w, cy + cardH);
            boolean hover = touchDown && card.contains(touchX, touchY);

            Paint cardPaint = new Paint();
            cardPaint.setAntiAlias(true);
            cardPaint.setColor(hover ? Color.rgb(40, 52, 72) : cardBgPaint.getColor());
            canvas.drawRoundRect(card, 10, 10, cardPaint);

            if (hover) {
                Paint accentBar = new Paint();
                accentBar.setAntiAlias(true);
                accentBar.setColor(Color.rgb(80, 140, 220));
                canvas.drawRoundRect(new RectF(x, cy, x + 4, cy + cardH), 2, 2, accentBar);
            }

            float iconSize = 56;
            float iconCx = x + 30 + iconSize / 2;
            float iconCy = cy + cardH / 2;
            RectF iconBox = new RectF(iconCx - iconSize / 2, iconCy - iconSize / 2,
                                       iconCx + iconSize / 2, iconCy + iconSize / 2);

            Paint iconBgPaint = new Paint();
            iconBgPaint.setAntiAlias(true);
            iconBgPaint.setColor(Color.rgb(45, 55, 70));
            canvas.drawCircle(iconCx, iconCy, iconSize / 2, iconBgPaint);

            Bitmap icon = (i < availableAppIds.length) ? appIcons.get(availableAppIds[i]) : null;
            if (icon != null) {
                Rect src = new Rect(0, 0, icon.getWidth(), icon.getHeight());
                RectF dst = new RectF(iconCx - iconSize / 2 + 4, iconCy - iconSize / 2 + 4,
                                       iconCx + iconSize / 2 - 4, iconCy + iconSize / 2 - 4);
                Paint iconPaint = new Paint();
                iconPaint.setAntiAlias(true);
                iconPaint.setFilterBitmap(true);
                canvas.drawBitmap(icon, src, dst, iconPaint);
            } else {
                textPaint.setColor(Color.rgb(160, 180, 210));
                textPaint.setTextSize(16);
                String firstLetter = availableAppNames[i].length() > 0
                    ? availableAppNames[i].substring(0, 1).toUpperCase()
                    : "?";
                canvas.drawText(firstLetter, iconCx - textPaint.measureText(firstLetter) / 2,
                                iconCy + 6, textPaint);
                textPaint.setTextSize(28);
                textPaint.setColor(Color.rgb(230, 235, 245));
            }

            float textX = x + 30 + iconSize + 20;
            float maxTextW = w - (textX - x) - 80;

            textPaint.setColor(Color.rgb(235, 240, 250));
            textPaint.setTextSize(26);
            String displayName = availableAppNames[i];
            if (textPaint.measureText(displayName) > maxTextW) {
                while (textPaint.measureText(displayName + "...") > maxTextW && displayName.length() > 0)
                    displayName = displayName.substring(0, displayName.length() - 1);
                displayName += "...";
            }
            canvas.drawText(displayName, textX, cy + 38, textPaint);
            textPaint.setTextSize(28);

            if (i < availableAppIds.length) {
                textSmallPaint.setColor(Color.rgb(110, 120, 135));
                textSmallPaint.setTextSize(14);
                String idStr = availableAppIds[i];
                if (textSmallPaint.measureText(idStr) > maxTextW) {
                    while (textSmallPaint.measureText(idStr + "...") > maxTextW && idStr.length() > 0)
                        idStr = idStr.substring(0, idStr.length() - 1);
                    idStr += "...";
                }
                canvas.drawText(idStr, textX, cy + 60, textSmallPaint);
                textSmallPaint.setTextSize(18);
            }

            float btnCx = x + w - 35;
            float btnCy = cy + cardH / 2;
            Paint launchBtnPaint = new Paint();
            launchBtnPaint.setAntiAlias(true);
            launchBtnPaint.setColor(hover ? Color.rgb(70, 130, 210) : Color.rgb(50, 60, 78));
            canvas.drawCircle(btnCx, btnCy, 22, launchBtnPaint);

            Paint arrowPaint = new Paint();
            arrowPaint.setAntiAlias(true);
            arrowPaint.setColor(Color.rgb(220, 230, 245));
            arrowPaint.setStyle(Paint.Style.STROKE);
            arrowPaint.setStrokeWidth(3);
            arrowPaint.setStrokeCap(Paint.Cap.ROUND);
            canvas.drawLine(btnCx - 6, btnCy - 7, btnCx + 5, btnCy, arrowPaint);
            canvas.drawLine(btnCx + 5, btnCy, btnCx - 6, btnCy + 7, arrowPaint);
        }

        canvas.restoreToCount(clipSave);

        if (launchMaxScroll > 0) {
            float barH = Math.max(30, visibleHeight * visibleHeight / contentHeight);
            float barY = y + (visibleHeight - barH) * (launchScrollY / launchMaxScroll);
            float barX = x + w - 6;
            Paint barPaint = new Paint();
            barPaint.setAntiAlias(true);
            barPaint.setColor(Color.rgb(60, 70, 85));
            canvas.drawRoundRect(new RectF(barX, barY, barX + 4, barY + barH), 2, 2, barPaint);
        }

        if (launchingAppName != null) {
            long elapsed = (System.currentTimeMillis() - launchingStartTime) / 1000;
            Paint overlayPaint = new Paint();
            overlayPaint.setAntiAlias(true);
            overlayPaint.setColor(Color.argb(200, 10, 14, 20));
            canvas.drawRect(x - 5, 40, x + w + 5, height, overlayPaint);

            float boxW = 500;
            float boxH = 160;
            float boxX = x + (w - boxW) / 2;
            float boxY = 40 + (height - 40 - boxH) / 2;
            Paint boxPaint = new Paint();
            boxPaint.setAntiAlias(true);
            boxPaint.setColor(Color.rgb(30, 38, 52));
            canvas.drawRoundRect(new RectF(boxX, boxY, boxX + boxW, boxY + boxH), 16, 16, boxPaint);

            textPaint.setColor(Color.rgb(230, 235, 245));
            textPaint.setTextSize(28);
            canvas.drawText(i18n.s(R.string.launching_app, launchingAppName), boxX + 30, boxY + 55, textPaint);
            textPaint.setTextSize(20);
            textPaint.setColor(Color.rgb(140, 150, 165));
            canvas.drawText(i18n.s(R.string.waiting_for_server, (int)elapsed), boxX + 30, boxY + 95, textPaint);

            float spinnerCx = boxX + boxW - 50;
            float spinnerCy = boxY + boxH / 2;
            float spinnerR = 20;
            float angle = (System.currentTimeMillis() % 1000) / 1000f * 360;
            Paint spinnerPaint = new Paint();
            spinnerPaint.setAntiAlias(true);
            spinnerPaint.setColor(Color.rgb(80, 140, 220));
            spinnerPaint.setStyle(Paint.Style.STROKE);
            spinnerPaint.setStrokeWidth(4);
            spinnerPaint.setStrokeCap(Paint.Cap.ROUND);
            android.graphics.RectF arcRect = new android.graphics.RectF(
                spinnerCx - spinnerR, spinnerCy - spinnerR,
                spinnerCx + spinnerR, spinnerCy + spinnerR);
            canvas.drawArc(arcRect, angle, 270, false, spinnerPaint);

            textPaint.setTextSize(28);
            markDirty();
        }
    }

    private void renderStreamStats(float x, float w) {
        float y = 20;
        canvas.drawText(i18n.s(R.string.perf_stats), x, y + 30, textLargePaint);
        y += 55;

        String mbitUnit = i18n.s(R.string.unit_mbit_s);
        String msUnit = i18n.s(R.string.unit_ms);
        String fpsUnit = i18n.s(R.string.unit_fps);

        float colW = (w - 20) / 2;
        float col1X = x;
        float col2X = x + colW + 20;

        // Top row: FPS + latency summary
        float summaryY = y;
        drawStatSummary(col1X, summaryY, colW, i18n.s(R.string.stat_fps),
                streamFps > 0 ? streamFps + " " + fpsUnit : "--",
                fpsHistory, statsHistoryOffset, statsHistoryCount, Color.rgb(80, 200, 120), 0, 120);
        drawStatSummary(col2X, summaryY, colW, i18n.s(R.string.stat_mtp_latency),
                streamLatencyMs > 0 ? streamLatencyMs + " " + msUnit : "--",
                latencyHistory, statsHistoryOffset, statsHistoryCount, Color.rgb(220, 140, 60), 0, 100);
        y += 160;

        // Second row: bandwidth download + upload
        drawStatSummary(col1X, y, colW, i18n.s(R.string.stat_download),
                String.format("%.1f %s", streamBandwidthRx * 1e-6f, mbitUnit),
                bwRxHistory, statsHistoryOffset, statsHistoryCount, Color.rgb(80, 160, 240), 0, 0);
        drawStatSummary(col2X, y, colW, i18n.s(R.string.stat_upload),
                String.format("%.1f %s", streamBandwidthTx * 1e-6f, mbitUnit),
                bwTxHistory, statsHistoryOffset, statsHistoryCount, Color.rgb(200, 100, 200), 0, 0);
        y += 160;

        // Third row: CPU time + GPU time
        drawStatSummary(col1X, y, colW, i18n.s(R.string.stat_cpu_time),
                streamCpuMs > 0 ? String.format("%.1f %s", (float)streamCpuMs, msUnit) : "--",
                cpuTimeHistory, statsHistoryOffset, statsHistoryCount, Color.rgb(240, 180, 60), 0, 0);
        drawStatSummary(col2X, y, colW, i18n.s(R.string.stat_gpu_time),
                streamGpuMs > 0 ? String.format("%.1f %s", (float)streamGpuMs, msUnit) : "--",
                gpuTimeHistory, statsHistoryOffset, statsHistoryCount, Color.rgb(100, 200, 220), 0, 0);
        y += 160;

        // Latency breakdown bar chart
        y += 10;
        canvas.drawText(i18n.s(R.string.stat_latency_breakdown), x, y + 20, textPaint);
        textPaint.setColor(Color.rgb(230, 235, 245));
        y += 35;
        drawLatencyBreakdown(x, y, w, msUnit);
        y += 120;

        // Bitrate + resolution + mic info row
        y += 15;
        Paint sepPaint = new Paint();
        sepPaint.setColor(Color.rgb(50, 60, 75));
        canvas.drawRect(x, y, x + w, y + 1, sepPaint);
        y += 20;

        float labelW = 200;
        float valueX = x + labelW;
        float lineH = 36;

        textSmallPaint.setColor(Color.rgb(140, 150, 165));
        canvas.drawText(i18n.s(R.string.stat_bitrate), x, y, textSmallPaint);
        textPaint.setColor(Color.rgb(230, 235, 245));
        canvas.drawText(streamBitrateMbps + " " + mbitUnit, valueX, y, textPaint);
        y += lineH;

        textSmallPaint.setColor(Color.rgb(140, 150, 165));
        canvas.drawText(i18n.s(R.string.stat_total_latency), x, y, textSmallPaint);
        textPaint.setColor(Color.rgb(230, 235, 245));
        canvas.drawText(streamTotalLatencyMs > 0 ? String.format("%.1f %s", streamTotalLatencyMs, msUnit) : "--", valueX, y, textPaint);
        y += lineH;

        textSmallPaint.setColor(Color.rgb(90, 95, 105));
        canvas.drawText(i18n.s(R.string.stat_resolution), x, y, textSmallPaint);
        textPaint.setColor(Color.rgb(90, 95, 105));
        canvas.drawText(streamResolutionScale + "%", valueX, y, textPaint);
        y += lineH;

        canvas.drawText(i18n.s(R.string.stat_microphone), x, y, textSmallPaint);
        textPaint.setColor(microphoneEnabled ? Color.rgb(80, 200, 120) : Color.rgb(140, 150, 165));
        canvas.drawText(microphoneEnabled ? i18n.s(R.string.stat_enabled) : i18n.s(R.string.stat_disabled), valueX, y, textPaint);
        y += lineH;

        y += 20;
        textDimPaint.setColor(Color.rgb(100, 110, 125));
        canvas.drawText(i18n.s(R.string.toggle_overlay_hint), x, y, textDimPaint);
        textDimPaint.setColor(Color.rgb(100, 110, 125));
    }

    private void drawStatSummary(float x, float y, float w, String label, String value,
                                 float[] history, int offset, int count, int color,
                                 float fixedMin, float fixedMax) {
        Paint labelPaint = new Paint();
        labelPaint.setColor(Color.rgb(140, 150, 165));
        labelPaint.setTextSize(20);
        labelPaint.setAntiAlias(true);

        Paint valuePaint = new Paint();
        valuePaint.setColor(Color.rgb(230, 235, 245));
        valuePaint.setTextSize(26);
        valuePaint.setTypeface(Typeface.DEFAULT_BOLD);
        valuePaint.setAntiAlias(true);

        canvas.drawText(label, x, y + 20, labelPaint);
        canvas.drawText(value, x, y + 48, valuePaint);

        float chartTop = y + 60;
        float chartH = 80;
        float chartW = w;

        drawLineChart(x, chartTop, chartW, chartH, history, offset, count, color, fixedMin, fixedMax);
    }

    private void drawLineChart(float x, float y, float w, float h, float[] history,
                               int offset, int count, int color, float fixedMin, float fixedMax) {
        if (count < 2)
            return;

        Paint bgPaint = new Paint();
        bgPaint.setColor(Color.argb(40, 32, 32, 32));
        canvas.drawRect(x, y, x + w, y + h, bgPaint);

        Paint borderPaint = new Paint();
        borderPaint.setColor(Color.rgb(40, 45, 55));
        borderPaint.setStyle(Paint.Style.STROKE);
        borderPaint.setStrokeWidth(1);
        canvas.drawRect(x, y, x + w, y + h, borderPaint);

        float min = fixedMin;
        float max = fixedMax;
        if (max <= min) {
            min = Float.MAX_VALUE;
            max = Float.MIN_VALUE;
            for (int i = 0; i < count; i++) {
                int idx = (offset - count + i + STATS_HISTORY_SIZE) % STATS_HISTORY_SIZE;
                float v = history[idx];
                if (v < min) min = v;
                if (v > max) max = v;
            }
            if (max <= min) {
                max = min + 1;
            }
            float range = max - min;
            max += range * 0.1f;
            if (min > 0) min = 0;
        }

        Paint linePaint = new Paint();
        linePaint.setColor(color);
        linePaint.setAntiAlias(true);
        linePaint.setStyle(Paint.Style.STROKE);
        linePaint.setStrokeWidth(2);

        Paint fillPaint = new Paint();
        fillPaint.setColor(Color.argb(60, Color.red(color), Color.green(color), Color.blue(color)));
        fillPaint.setAntiAlias(true);
        fillPaint.setStyle(Paint.Style.FILL);

        float stepX = w / (STATS_HISTORY_SIZE - 1);

        Path linePath = new Path();
        Path fillPath = new Path();

        for (int i = 0; i < count; i++) {
            int idx = (offset - count + i + STATS_HISTORY_SIZE) % STATS_HISTORY_SIZE;
            float v = history[idx];
            float px = x + i * stepX;
            float py = y + h - (v - min) / (max - min) * h;
            py = Math.max(y, Math.min(y + h, py));

            if (i == 0) {
                linePath.moveTo(px, py);
                fillPath.moveTo(px, y + h);
                fillPath.lineTo(px, py);
            } else {
                linePath.lineTo(px, py);
                fillPath.lineTo(px, py);
            }
        }

        if (count > 0) {
            int lastIdx = (offset - 1 + STATS_HISTORY_SIZE) % STATS_HISTORY_SIZE;
            float lastX = x + (count - 1) * stepX;
            fillPath.lineTo(lastX, y + h);
            fillPath.close();
            canvas.drawPath(fillPath, fillPaint);
        }

        canvas.drawPath(linePath, linePaint);

        // Current value dot
        if (count > 0) {
            int lastIdx = (offset - 1 + STATS_HISTORY_SIZE) % STATS_HISTORY_SIZE;
            float lastV = history[lastIdx];
            float lastPx = x + (count - 1) * stepX;
            float lastPy = y + h - (lastV - min) / (max - min) * h;
            lastPy = Math.max(y, Math.min(y + h, lastPy));

            Paint dotPaint = new Paint();
            dotPaint.setColor(color);
            dotPaint.setAntiAlias(true);
            canvas.drawCircle(lastPx, lastPy, 3, dotPaint);
        }
    }

    private void drawLatencyBreakdown(float x, float y, float w, String unit) {
        float[] stages = {streamEncodeMs, streamSendMs, streamNetworkMs, streamDecodeMs, streamRenderWaitMs, streamBlitMs};
        String[] labels = {
            i18n.s(R.string.stat_encode),
            i18n.s(R.string.stat_send),
            i18n.s(R.string.stat_network),
            i18n.s(R.string.stat_decode),
            i18n.s(R.string.stat_render_wait),
            i18n.s(R.string.stat_blit)
        };
        int[] colors = {
            Color.rgb(240, 100, 100),
            Color.rgb(240, 180, 60),
            Color.rgb(100, 200, 120),
            Color.rgb(80, 160, 240),
            Color.rgb(200, 100, 200),
            Color.rgb(100, 200, 220)
        };

        float total = 0;
        for (float s : stages) total += s;
        if (total <= 0) {
            textDimPaint.setColor(Color.rgb(100, 110, 125));
            canvas.drawText("--", x + 10, y + 30, textDimPaint);
            textDimPaint.setColor(Color.rgb(100, 110, 125));
            return;
        }

        float barH = 28;
        float barY = y;
        float barX = x;
        float barW = w;

        float accumX = barX;
        for (int i = 0; i < stages.length; i++) {
            if (stages[i] <= 0) continue;
            float segW = stages[i] / total * barW;
            Paint segPaint = new Paint();
            segPaint.setColor(colors[i]);
            segPaint.setAntiAlias(true);
            canvas.drawRect(accumX, barY, accumX + segW, barY + barH, segPaint);
            accumX += segW;
        }

        Paint borderPaint = new Paint();
        borderPaint.setColor(Color.rgb(40, 45, 55));
        borderPaint.setStyle(Paint.Style.STROKE);
        borderPaint.setStrokeWidth(1);
        canvas.drawRect(barX, barY, barX + barW, barY + barH, borderPaint);

        // Legend below
        float legendY = barY + barH + 18;
        float legendX = barX;
        float legendSpacing = barW / 3;
        Paint legendPaint = new Paint();
        legendPaint.setTextSize(18);
        legendPaint.setAntiAlias(true);

        for (int i = 0; i < stages.length; i++) {
            float col = i % 3;
            float row = i / 3;
            float lx = barX + col * legendSpacing;
            float ly = legendY + row * 24;

            Paint swatchPaint = new Paint();
            swatchPaint.setColor(colors[i]);
            swatchPaint.setAntiAlias(true);
            canvas.drawRect(lx, ly - 12, lx + 12, ly, swatchPaint);

            legendPaint.setColor(Color.rgb(160, 170, 185));
            canvas.drawText(labels[i] + " " + String.format("%.1f", stages[i]) + unit,
                    lx + 16, ly, legendPaint);
        }
    }

    private void renderTouchCursor() {
        if (touchX >= 0 && touchY >= 0) {
            int baseAlpha = touchDown ? 220 : 140;
            int color = touchDown ? Color.argb(baseAlpha, 80, 160, 240) : Color.argb(baseAlpha, 255, 255, 255);

            Paint ringPaint = new Paint();
            ringPaint.setColor(color);
            ringPaint.setAntiAlias(true);
            ringPaint.setStyle(Paint.Style.STROKE);
            ringPaint.setStrokeWidth(3);
            canvas.drawCircle(touchX, touchY, touchDown ? 24 : 18, ringPaint);

            Paint linePaint = new Paint();
            linePaint.setColor(color);
            linePaint.setAntiAlias(true);
            linePaint.setStrokeWidth(2);
            float gap = 6;
            float len = 14;
            canvas.drawLine(touchX - gap - len, touchY, touchX - gap, touchY, linePaint);
            canvas.drawLine(touchX + gap, touchY, touchX + gap + len, touchY, linePaint);
            canvas.drawLine(touchX, touchY - gap - len, touchX, touchY - gap, linePaint);
            canvas.drawLine(touchX, touchY + gap, touchX, touchY + gap + len, linePaint);

            Paint dotPaint = new Paint();
            dotPaint.setColor(color);
            dotPaint.setAntiAlias(true);
            canvas.drawCircle(touchX, touchY, 3, dotPaint);
        }
    }

    private void drawCenteredText(String text, RectF rect, Paint paint) {
        Paint.FontMetrics fm = paint.getFontMetrics();
        float textY = rect.top + (rect.height() - (fm.descent - fm.ascent)) / 2 - fm.ascent;
        float textX = rect.left + (rect.width() - paint.measureText(text)) / 2;
        canvas.drawText(text, textX, textY, paint);
    }

    public void handleTouch(float x, float y, boolean down, boolean pressed, float thumbstickY) {
        float prevX = touchX, prevY = touchY;
        boolean prevDown = touchDown;
        touchX = x;
        touchY = y;
        touchDown = down;
        touchPressed = pressed;

        if (pressed) {
            android.util.Log.i("WiVRn-Lobby", "CLICK DISPATCH x=" + x + " y=" + y + " state=" + connectionState + " tab=" + currentTab);
        }

        if (connectionState == STATE_CONNECTED && streamTab == STREAM_TAB_LAUNCH) {
            if (down && prevDown && y != prevY) {
                if (dragStartY < 0) {
                    dragStartY = prevY;
                    dragStartScroll = launchScrollY;
                }
                launchScrollY = dragStartScroll + (dragStartY - y);
                launchScrollY = Math.max(0, Math.min(launchMaxScroll, launchScrollY));
                if (pressStartX >= 0 && Math.hypot(x - pressStartX, y - pressStartY) > DRAG_THRESHOLD) {
                    pressDragged = true;
                }
            }
            if (!down) {
                dragStartY = -1;
            }

            float stickMag = Math.abs(thumbstickY);
            if (stickMag > 0.3f) {
                thumbstickAccum -= thumbstickY * 15f;
                if (Math.abs(thumbstickAccum) >= 1f) {
                    launchScrollY = Math.max(0, Math.min(launchMaxScroll, launchScrollY + thumbstickAccum));
                    thumbstickAccum = 0;
                }
            } else {
                thumbstickAccum = 0;
            }
        } else if ((connectionState != STATE_CONNECTED && currentTab == TAB_SETTINGS) ||
                   (connectionState == STATE_CONNECTED && streamTab == STREAM_TAB_SETTINGS)) {
            float stickMag = Math.abs(thumbstickY);
            if (stickMag > 0.3f) {
                settingsThumbstickAccum -= thumbstickY * 15f;
                if (Math.abs(settingsThumbstickAccum) >= 1f) {
                    settingsScrollY = Math.max(0, Math.min(settingsMaxScroll, settingsScrollY + settingsThumbstickAccum));
                    settingsThumbstickAccum = 0;
                }
            } else {
                settingsThumbstickAccum = 0;
            }
        }

        prevTouchY = touchY;

        if (pressed) {
            pressStartX = x;
            pressStartY = y;
            pressDragged = false;
        }

        if (connectionState == STATE_CONNECTED && streamTab == STREAM_TAB_LAUNCH) {
            if (pressed) {
                activeSlider = SLIDER_NONE;
            } else if (!down && prevDown && !pressDragged) {
                activeSlider = SLIDER_NONE;
                handleClick(x, y);
            } else if (down && activeSlider != SLIDER_NONE) {
                handleSliderDrag(x, y);
            }
        } else {
            if (pressed) {
                activeSlider = SLIDER_NONE;
                handleClick(x, y);
            } else if (down && activeSlider != SLIDER_NONE) {
                handleSliderDrag(x, y);
            } else if (!down) {
                activeSlider = SLIDER_NONE;
            }
        }

        if (!down) {
            pressStartX = -1;
            pressStartY = -1;
            pressDragged = false;
        }
        markDirty();
    }

    private void handleSliderDrag(float x, float y) {
        float contentX, sliderW;
        float adjustedY = y - (TOPBAR_HEIGHT + 10) + settingsScrollY;

        if (connectionState == STATE_CONNECTED && streamTab == STREAM_TAB_SETTINGS) {
            handleSettingsSliderDrag(x, adjustedY);
            return;
        }

        if (currentTab != TAB_SETTINGS) return;
        if (showAddServer) return;

        handleSettingsSliderDrag(x, adjustedY);
    }

    private void handleSettingsSliderDrag(float x, float y) {
        float contentX = SIDEBAR_WIDTH + 20;
        float sliderW = (width - contentX - 20) - 100;
        float resSliderW = (width - contentX - 20) - 130;
        float pct = Math.max(0, Math.min(1, (x - contentX) / sliderW));
        switch (activeSlider) {
            case SLIDER_RESOLUTION: {
                float resPct = Math.max(0, Math.min(1, (x - contentX) / resSliderW));
                resWidth = Math.max(1024, Math.min(2048, (int)(1024 + resPct * 1024)));
                saveSettings();
                applyResolution();
                markDirty();
                break;
            }
            case SLIDER_FOVEATION:
                foveationScale = Math.max(0, Math.min(80, (int)(pct * 80)));
                saveSettings();
                markDirty();
                break;
            case SLIDER_BITRATE:
                bitrate = Math.max(5, Math.min(200, (int)(5 + pct * 195)));
                saveSettings();
                ((MainActivity) context).nativeSetBitrate(bitrate);
                markDirty();
                break;
            case SLIDER_IPD:
                ipdMm = Math.round((58 + pct * 14) * 2) / 2.0f;
                ipdMm = Math.max(58, Math.min(72, ipdMm));
                saveSettings();
                ((MainActivity) context).onIpdChanged(ipdMm);
                markDirty();
                break;
            case SLIDER_FOV:
                fovDeg = Math.round(70 + pct * 31);
                fovDeg = Math.max(70, Math.min(101, fovDeg));
                saveSettings();
                ((MainActivity) context).nativeSetFov(fovDeg);
                markDirty();
                break;
            case SLIDER_BRIGHTNESS:
                brightnessFrac = Math.max(0, Math.min(1, pct));
                saveSettings();
                ((MainActivity) context).nativeSetBrightness(brightnessFrac);
                markDirty();
                break;
            case SLIDER_CTRL_VIBRATION:
                ctrlVibration = Math.max(0, Math.min(1, pct));
                saveSettings();
                ((MainActivity) context).nativeSetCtrlVibration(ctrlVibration);
                markDirty();
                break;
        }
    }

    private void handleClick(float x, float y) {
        if (connectionState == STATE_PIN_ENTRY) {
            handlePinClick(x, y);
            return;
        }
        if (connectionState == STATE_CONNECTING) {
            handleConnectingClick(x, y);
            return;
        }
        if (connectionState == STATE_DISCONNECTED) {
            handleDisconnectedClick(x, y);
            return;
        }
        if (connectionState == STATE_CONNECTED) {
            handleStreamClick(x, y);
            return;
        }

        if (showAddServer) {
            handleAddServerClick(x, y);
            return;
        }

        if (x < SIDEBAR_WIDTH) {
            handleSidebarClick(x, y);
            return;
        }

        float adjustedY = y - (TOPBAR_HEIGHT + 10);

        switch (currentTab) {
            case TAB_SERVER_LIST:
                handleServerListClick(x, adjustedY);
                break;
            case TAB_SETTINGS:
                handleSettingsClick(x, adjustedY + settingsScrollY);
                break;
            case TAB_ABOUT:
                handleAboutClick(x, adjustedY);
                break;
            case TAB_EXIT:
                handleExitClick(x, adjustedY);
                break;
        }
    }

    private void handleAboutClick(float x, float y) {
        if (gitlabBtnRect != null && gitlabBtnRect.contains(x, y)) {
            try {
                Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse("https://gitlab.com/HttpAnimations/piconeo2-wivrn"));
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                context.startActivity(intent);
            } catch (Exception e) {
                Log.w(TAG, "Failed to open GitLab URL", e);
            }
        }
    }

    private void handleStreamClick(float x, float y) {
        if (x < SIDEBAR_WIDTH) {
            String[] tabs = {"Applications", "Launch", "Stats", "Settings"};
            int[] tabIds = {STREAM_TAB_APPLICATIONS, STREAM_TAB_LAUNCH, STREAM_TAB_STATS, STREAM_TAB_SETTINGS};
            float ty = 30;
            for (int i = 0; i < tabs.length; i++) {
                RectF rect = new RectF(10, ty, SIDEBAR_WIDTH - 10, ty + TAB_HEIGHT);
                if (rect.contains(x, y)) {
                    streamTab = tabIds[i];
                    if (streamTab == STREAM_TAB_LAUNCH && !appListRequested && availableAppNames.length == 0) {
                        ((MainActivity) context).onRequestAppList();
                        appListRequested = true;
                    }
                    markDirty();
                    return;
                }
                ty += TAB_HEIGHT + 5;
            }

            float disconnectY = height - TAB_HEIGHT - 30;
            RectF disconnectBtn = new RectF(10, disconnectY, SIDEBAR_WIDTH - 10, disconnectY + TAB_HEIGHT);
            if (disconnectBtn.contains(x, y)) {
                ((MainActivity) context).onDisconnectRequested();
                connectionState = STATE_IDLE;
                markDirty();
                return;
            }
            return;
        }

        float contentX = SIDEBAR_WIDTH + 30;
        float contentW = width - contentX - 30;

        if (streamTab == STREAM_TAB_APPLICATIONS) {
            if (runningApps == null || runningApps.length == 0)
                return;

            float cardY = 40 + 70;
            for (int i = 0; i < runningApps.length; i++) {
                boolean isOverlay = i < runningAppOverlays.length && runningAppOverlays[i];
                if (isOverlay && (i == 0 || !(i - 1 < runningAppOverlays.length && runningAppOverlays[i - 1]))) {
                    cardY += 15 + 35;
                }

                RectF stopBtn = new RectF(contentX + contentW - 60, cardY + 15, contentX + contentW - 15, cardY + 55);
                if (stopBtn.contains(x, y) && i < runningAppIds.length) {
                    ((MainActivity) context).onStopApp(runningAppIds[i]);
                    markDirty();
                    return;
                }

                RectF card = new RectF(contentX, cardY, contentX + contentW, cardY + 70);
                if (card.contains(x, y) && !isOverlay && i < runningAppIds.length) {
                    boolean isActive = i < runningAppActives.length && runningAppActives[i];
                    if (!isActive) {
                        ((MainActivity) context).onSetActiveApp(runningAppIds[i]);
                    }
                    markDirty();
                    return;
                }

                cardY += 85;
            }
            return;
        }

        if (streamTab == STREAM_TAB_LAUNCH) {
            if (availableAppNames == null || availableAppNames.length == 0) {
                float refreshY = 40 + 70 + 80;
                RectF refreshBtn = new RectF(contentX, refreshY, contentX + 200, refreshY + BUTTON_HEIGHT);
                if (refreshBtn.contains(x, y)) {
                    ((MainActivity) context).onRequestAppList();
                    appListRequested = true;
                    markDirty();
                    return;
                }
            } else {
                float cardH = 90;
                float gap = 12;
                float startY = 40 + 70;

                for (int i = 0; i < availableAppNames.length; i++) {
                    float cy = startY + i * (cardH + gap) - launchScrollY;
                    RectF card = new RectF(contentX, cy, contentX + contentW, cy + cardH);
                    if (card.contains(x, y) && i < availableAppIds.length) {
                        ((MainActivity) context).onStartApp(availableAppIds[i]);
                        launchingAppName = availableAppNames[i];
                        launchingStartTime = System.currentTimeMillis();
                        markDirty();
                        return;
                    }
                }
            }
        }

        if (streamTab == STREAM_TAB_SETTINGS) {
            float adjustedY = y - (TOPBAR_HEIGHT + 10) + settingsScrollY;
            handleSettingsClick(x, adjustedY);
            return;
        }
    }

    private void handleSidebarClick(float x, float y) {
        String[] tabs = {"Server List", "Settings", "About", "Licenses", "Exit"};
        int[] tabIds = {TAB_SERVER_LIST, TAB_SETTINGS, TAB_ABOUT, TAB_LICENSES, TAB_EXIT};
        float ty = 30;
        for (int i = 0; i < tabs.length; i++) {
            RectF rect = new RectF(10, ty, SIDEBAR_WIDTH - 10, ty + TAB_HEIGHT);
            if (rect.contains(x, y)) {
                currentTab = tabIds[i];
                if (currentTab == TAB_EXIT) {
                    currentTab = TAB_SERVER_LIST;
                    ((MainActivity) context).finish();
                }
                markDirty();
                return;
            }
            ty += TAB_HEIGHT + 10;
        }
    }

    private void handleServerListClick(float x, float y) {
        float contentX = SIDEBAR_WIDTH + 20;
        float contentW = width - contentX - 20;
        float listY = 90;

        RectF addBtn = new RectF(contentX + contentW - BUTTON_WIDTH, listY - BUTTON_HEIGHT, contentX + contentW, listY);
        if (addBtn.contains(x, y)) {
            showAddServer = true;
            addServerName = "";
            addServerAddress = "";
            addServerPort = "9757";
            addServerTcpOnly = tcpOnly;
            addServerFieldFocus = 0;
            markDirty();
            return;
        }

        List<ServerEntry> allServers = getAllServers();
        for (int i = 0; i < allServers.size(); i++) {
            ServerEntry s = allServers.get(i);
            float cardY = listY + 20 + i * (CARD_HEIGHT + 10);
            RectF connectBtn = new RectF(contentX + contentW - BUTTON_WIDTH - 20, cardY + 20, contentX + contentW - 20, cardY + 20 + BUTTON_HEIGHT);
            if (connectBtn.contains(x, y)) {
                selectedServerIndex = i;
                connectionState = STATE_CONNECTING;
                statusMessage = "Connecting...";
                ((MainActivity) context).onServerConnect(s.hostname, s.port, s.tcpOnly);
                markDirty();
                return;
            }

            float starX = connectBtn.left - 50;
            RectF starBtn = new RectF(starX, cardY + 20, starX + 40, cardY + 20 + BUTTON_HEIGHT);
            if (starBtn.contains(x, y)) {
                setAutoconnect(s.hostname, s.port);
                markDirty();
                return;
            }

            if (s.manual) {
                RectF delBtn = new RectF(starBtn.left - 60, cardY + 20, starBtn.left - 10, cardY + 20 + BUTTON_HEIGHT);
                if (delBtn.contains(x, y)) {
                    servers.remove(s);
                    saveServers();
                    markDirty();
                    return;
                }
            }
        }
    }

    private void handleAddServerClick(float x, float y) {
        float popupW = 600;
        float popupH = 400;
        float popupX = (width - popupW) / 2;
        float popupY = (height - popupH) / 2;
        float py = popupY + 90;

        for (int i = 0; i < 3; i++) {
            RectF field = new RectF(popupX + 180, py, popupX + popupW - 30, py + 40);
            if (field.contains(x, y)) {
                addServerFieldFocus = i;
                markDirty();
                return;
            }
            py += 55;
        }

        RectF tcpCheck = new RectF(popupX + 30, py, popupX + 50, py + 20);
        if (tcpCheck.contains(x, y) || (x > popupX + 30 && x < popupX + 150 && y > py && y < py + 25)) {
            addServerTcpOnly = !addServerTcpOnly;
            markDirty();
            return;
        }
        py += 50;

        RectF cancelBtn = new RectF(popupX + 30, py, popupX + 30 + 150, py + BUTTON_HEIGHT);
        if (cancelBtn.contains(x, y)) {
            showAddServer = false;
            markDirty();
            return;
        }

        RectF saveBtn = new RectF(popupX + popupW - 180, py, popupX + popupW - 30, py + BUTTON_HEIGHT);
        if (saveBtn.contains(x, y)) {
            if (!addServerName.isEmpty() && !addServerAddress.isEmpty()) {
                servers.add(new ServerEntry(addServerName, addServerAddress,
                    Integer.parseInt(addServerPort.isEmpty() ? "9757" : addServerPort),
                    addServerTcpOnly, true));
                saveServers();
            }
            showAddServer = false;
            markDirty();
            return;
        }
    }

    private void handleSettingsClick(float x, float y) {
        float contentX = SIDEBAR_WIDTH + 20;
        float contentW = width - contentX - 20;

        float sliderW = contentW - 100;

        float sy = 100;
        // Resolution
        sy += 35;
        float resSliderW = contentW - 130;
        if (y >= sy - 10 && y <= sy + 20 && x >= contentX && x <= contentX + resSliderW) {
            activeSlider = SLIDER_RESOLUTION;
            float pct = Math.max(0, Math.min(1, (x - contentX) / resSliderW));
            resWidth = Math.max(1024, Math.min(2048, (int)(1024 + pct * 1024)));
            saveSettings();
            applyResolution();
            markDirty();
            return;
        }
        sy += 50;
        // Foveation (disabled)
        sy += 35 + 50;
        // Bitrate
        sy += 35;
        if (y >= sy - 10 && y <= sy + 20 && x >= contentX && x <= contentX + sliderW) {
            activeSlider = SLIDER_BITRATE;
            float pct = Math.max(0, Math.min(1, (x - contentX) / sliderW));
            bitrate = Math.max(5, Math.min(200, (int)(5 + pct * 195)));
            saveSettings();
            ((MainActivity) context).nativeSetBitrate(bitrate);
            markDirty();
            return;
        }
        sy += 50;

        // IPD (enabled)
        sy += 35;
        resSliderW = contentW - 100;
        if (y >= sy - 10 && y <= sy + 20 && x >= contentX && x <= contentX + resSliderW) {
            activeSlider = SLIDER_IPD;
            float pct = Math.max(0, Math.min(1, (x - contentX) / resSliderW));
            ipdMm = Math.round((58 + pct * 14) * 2) / 2.0f;
            ipdMm = Math.max(58, Math.min(72, ipdMm));
            saveSettings();
            ((MainActivity) context).onIpdChanged(ipdMm);
            markDirty();
            return;
        }
        sy += 50;

        // FOV
        sy += 35;
        if (y >= sy - 10 && y <= sy + 20 && x >= contentX && x <= contentX + sliderW) {
            activeSlider = SLIDER_FOV;
            float pct = Math.max(0, Math.min(1, (x - contentX) / sliderW));
            fovDeg = Math.round(70 + pct * 31);
            fovDeg = Math.max(70, Math.min(101, fovDeg));
            saveSettings();
            ((MainActivity) context).nativeSetFov(fovDeg);
            markDirty();
            return;
        }
        sy += 50;

        // Codec (disabled)
        sy += 35 + 50;

        // TCP only (enabled)
        RectF tcpCheckbox = new RectF(contentX, sy, contentX + 30, sy + 30);
        if (tcpCheckbox.contains(x, y) || (x >= contentX && x <= contentX + contentW && y >= sy - 5 && y <= sy + 35)) {
            tcpOnly = !tcpOnly;
            saveSettings();
            markDirty();
            return;
        }
        sy += 40;

        // Microphone (enabled)
        RectF micCheckbox = new RectF(contentX, sy, contentX + 30, sy + 30);
        if (micCheckbox.contains(x, y) || (x >= contentX && x <= contentX + contentW && y >= sy - 5 && y <= sy + 35)) {
            microphoneEnabled = !microphoneEnabled;
            saveSettings();
            ((MainActivity) context).onMicrophoneChanged(microphoneEnabled);
            markDirty();
            return;
        }
        sy += 40;

        // Passthrough
        RectF ptCheckbox = new RectF(contentX, sy, contentX + 30, sy + 30);
        if (ptCheckbox.contains(x, y) || (x >= contentX && x <= contentX + contentW && y >= sy - 5 && y <= sy + 35)) {
            passthroughEnabled = !passthroughEnabled;
            saveSettings();
            ((MainActivity) context).nativeSetPassthrough(passthroughEnabled);
            markDirty();
            return;
        }
        sy += 40;

        // Language dropdown
        sy += 35;
        RectF langBox = new RectF(contentX, sy, contentX + 300, sy + 40);
        if (langBox.contains(x, y) || (x >= contentX && x <= contentX + contentW && y >= sy - 5 && y <= sy + 45)) {
            languageSetting = (languageSetting + 1) % 3;
            i18n.setLanguage(languageSetting);
            saveSettings();
            markDirty();
            return;
        }
        sy += 50;

        // Debug section header
        sy += 15 + 55;

        // Brightness slider
        sy += 35;
        if (y >= sy - 10 && y <= sy + 20 && x >= contentX && x <= contentX + sliderW) {
            activeSlider = SLIDER_BRIGHTNESS;
            float pct = Math.max(0, Math.min(1, (x - contentX) / sliderW));
            brightnessFrac = Math.max(0, Math.min(1, pct));
            saveSettings();
            ((MainActivity) context).nativeSetBrightness(brightnessFrac);
            markDirty();
            return;
        }
        sy += 50;

        // Controller vibration slider
        sy += 35;
        if (y >= sy - 10 && y <= sy + 20 && x >= contentX && x <= contentX + sliderW) {
            activeSlider = SLIDER_CTRL_VIBRATION;
            float pct = Math.max(0, Math.min(1, (x - contentX) / sliderW));
            ctrlVibration = Math.max(0, Math.min(1, pct));
            saveSettings();
            ((MainActivity) context).nativeSetCtrlVibration(ctrlVibration);
            markDirty();
            return;
        }
        sy += 50;

        // Eye-tracked foveation checkbox
        if (!eyeSupported) {
            // skip
        } else if (x >= contentX && x <= contentX + contentW && y >= sy - 5 && y <= sy + 35) {
            eyeFoveationEnabled = !eyeFoveationEnabled;
            saveSettings();
            ((MainActivity) context).nativeSetEyeFoveation(eyeFoveationEnabled);
            markDirty();
            return;
        }
        sy += 40;

        // Eye debug checkbox
        if (x >= contentX && x <= contentX + contentW && y >= sy - 5 && y <= sy + 35) {
            eyeDebugOn = !eyeDebugOn;
            saveSettings();
            ((MainActivity) context).nativeSetEyeDebug(eyeDebugOn);
            markDirty();
            return;
        }
        sy += 40;

        // Diagnostics HUD dropdown
        sy += 35;
        if (x >= contentX && x <= contentX + contentW && y >= sy - 5 && y <= sy + 45) {
            diagHudMode = (diagHudMode + 1) % 3;
            saveSettings();
            ((MainActivity) context).nativeSetDiagHud(diagHudMode);
            markDirty();
            return;
        }
        sy += 50;

        // Recenter button
        sy += 10;
        RectF recenterBtn = new RectF(contentX, sy, contentX + 200, sy + BUTTON_HEIGHT);
        if (recenterBtn.contains(x, y)) {
            ((MainActivity) context).nativeRecenter();
            markDirty();
            return;
        }
        sy += BUTTON_HEIGHT + 10;

        // Restore Defaults button
        sy += 20;
        RectF resetBtn = new RectF(contentX, sy, contentX + 200, sy + BUTTON_HEIGHT);
        if (resetBtn.contains(x, y)) {
            showResetConfirm = true;
            markDirty();
            return;
        }

        // Reset confirm dialog
        if (showResetConfirm) {
            handleResetConfirmClick(x, y);
        }
    }

    private void handleResetConfirmClick(float x, float y) {
        float panelW = 500;
        float panelH = 200;
        float px = (width - panelW) / 2;
        float py = (height - panelH) / 2;

        RectF yesBtn = new RectF(px + 30, py + panelH - 70, px + 30 + 180, py + panelH - 20);
        if (yesBtn.contains(x, y)) {
            resWidth = 1664;
            foveationScale = 30;
            codec = "auto";
            bitrate = 50;
            ipdMm = 64;
            tcpOnly = false;
            microphoneEnabled = false;
            streamBitrateSetting = 50;
            streamResolutionScale = 100;
            languageSetting = 0;
            i18n.setLanguage(0);
            brightnessFrac = 1.0f;
            ctrlVibration = 1.0f;
            eyeFoveationEnabled = false;
            eyeDebugOn = false;
            diagHudMode = 0;
            saveSettings();
            applyResolution();
            ((MainActivity) context).onIpdChanged(ipdMm);
            ((MainActivity) context).onMicrophoneChanged(false);
            ((MainActivity) context).nativeSetBrightness(brightnessFrac);
            ((MainActivity) context).nativeSetCtrlVibration(ctrlVibration);
            ((MainActivity) context).nativeSetEyeFoveation(false);
            ((MainActivity) context).nativeSetEyeDebug(false);
            ((MainActivity) context).nativeSetDiagHud(0);
            showResetConfirm = false;
            markDirty();
            return;
        }

        RectF noBtn = new RectF(px + panelW - 210, py + panelH - 70, px + panelW - 30, py + panelH - 20);
        if (noBtn.contains(x, y)) {
            showResetConfirm = false;
            markDirty();
            return;
        }
    }

    private void handleExitClick(float x, float y) {
        float contentX = SIDEBAR_WIDTH + 20;
        float y0 = height / 2 - 50 + 60;

        RectF yesBtn = new RectF(contentX + 50, y0, contentX + 50 + 200, y0 + BUTTON_HEIGHT);
        if (yesBtn.contains(x, y)) {
            ((MainActivity) context).finish();
            return;
        }

        RectF noBtn = new RectF(contentX + 270, y0, contentX + 270 + 200, y0 + BUTTON_HEIGHT);
        if (noBtn.contains(x, y)) {
            currentTab = TAB_SERVER_LIST;
            markDirty();
            return;
        }
    }

    private void handlePinClick(float x, float y) {
        float panelW = 600;
        float panelH = 500;
        float px = (width - panelW) / 2;
        float py = (height - panelH) / 2;

        float keyY = py + 170;
        float keySize = 130;
        float keyGap = 10;
        float keyStartX = px + (panelW - 3 * keySize - 2 * keyGap) / 2;

        for (int i = 1; i <= 9; i++) {
            int row = (i - 1) / 3;
            int col = (i - 1) % 3;
            float kx = keyStartX + col * (keySize + keyGap);
            float ky = keyY + row * (keySize + keyGap);
            RectF key = new RectF(kx, ky, kx + keySize, ky + keySize);
            if (key.contains(x, y)) {
                if (pinBuffer.length() < 6) {
                    pinBuffer += String.valueOf(i);
                    markDirty();
                }
                checkPinComplete();
                return;
            }
        }

        float bottomY = keyY + 3 * (keySize + keyGap);
        RectF cancelKey = new RectF(keyStartX, bottomY, keyStartX + keySize, bottomY + keySize);
        if (cancelKey.contains(x, y)) {
            pinBuffer = "";
            ((MainActivity) context).onPinCancelled();
            markDirty();
            return;
        }

        RectF zeroKey = new RectF(keyStartX + keySize + keyGap, bottomY, keyStartX + 2 * keySize + keyGap, bottomY + keySize);
        if (zeroKey.contains(x, y)) {
            if (pinBuffer.length() < 6) {
                pinBuffer += "0";
                markDirty();
            }
            checkPinComplete();
            return;
        }

        RectF backKey = new RectF(keyStartX + 2 * (keySize + keyGap), bottomY, keyStartX + 3 * keySize + 2 * keyGap, bottomY + keySize);
        if (backKey.contains(x, y)) {
            if (pinBuffer.length() > 0) {
                pinBuffer = pinBuffer.substring(0, pinBuffer.length() - 1);
                markDirty();
            }
            return;
        }
    }

    private void checkPinComplete() {
        if (pinBuffer.length() == 6) {
            ((MainActivity) context).onPinEntered(pinBuffer);
        }
    }

    private void handleConnectingClick(float x, float y) {
        float panelW = 600;
        float panelH = 250;
        float px = (width - panelW) / 2;
        float py = (height - panelH) / 2;

        RectF disconnectBtn = new RectF(px + panelW - 220, py + panelH - 80, px + panelW - 20, py + panelH - 20);
        if (disconnectBtn.contains(x, y)) {
            ((MainActivity) context).onDisconnectRequested();
            markDirty();
        }
    }

    private void handleDisconnectedClick(float x, float y) {
        float panelW = 600;
        float panelH = 250;
        float px = (width - panelW) / 2;
        float py = (height - panelH) / 2;

        RectF reconnectBtn = new RectF(px + 20, py + panelH - 80, px + 220, py + panelH - 20);
        if (reconnectBtn.contains(x, y)) {
            errorMessage = "";
            ((MainActivity) context).onReconnectRequested();
            markDirty();
            return;
        }

        RectF closeBtn = new RectF(px + panelW - 220, py + panelH - 80, px + panelW - 20, py + panelH - 20);
        if (closeBtn.contains(x, y)) {
            connectionState = STATE_IDLE;
            errorMessage = "";
            ((MainActivity) context).onDisconnectRequested();
            markDirty();
        }
    }

    public void setErrorMessage(String msg) {
        errorMessage = msg != null ? msg : "";
        markDirty();
    }
}
