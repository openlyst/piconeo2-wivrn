package org.meumeu.wivrn.oxr;

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
    private String[] runningApps = new String[0];
    private int[] runningAppIds = new int[0];
    private boolean[] runningAppOverlays = new boolean[0];
    private boolean[] runningAppActives = new boolean[0];
    private long lastRunningAppsPoll = 0;
    private String[] availableAppIds = new String[0];
    private String[] availableAppNames = new String[0];
    private Map<String, Bitmap> appIcons = new HashMap<>();
    private boolean appListRequested = false;
    private int streamBitrateSetting = 50;
    private int streamResolutionScale = 100;
    private boolean streamMicEnabled = false;
    private boolean streamHighPower = false;

    private float launchScrollY = 0;
    private float launchMaxScroll = 0;
    private float dragStartY = -1;
    private float dragStartScroll = 0;
    private float prevTouchY = -1;
    private float thumbstickAccum = 0;

    private final Context context;
    private final Bitmap bitmap;
    private final Canvas canvas;
    private final int width = 1400;
    private final int height = 900;

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

    private float sliderValue = -1;
    private boolean sliderDragging = false;

    private int resolutionScale = 100;
    private int foveationScale = 30;
    private String codec = "auto";
    private int bitrate = 50;
    private boolean tcpOnly = false;
    private boolean microphoneEnabled = false;
    private boolean highPowerMode = false;

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

        ServerEntry(String name, String hostname, int port, boolean tcpOnly, boolean manual) {
            this(name, hostname, port, tcpOnly, manual, false);
        }

        ServerEntry(String name, String hostname, int port, boolean tcpOnly, boolean manual, boolean discovered) {
            this.name = name;
            this.hostname = hostname;
            this.port = port;
            this.tcpOnly = tcpOnly;
            this.manual = manual;
            this.discovered = discovered;
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

    public WivrnLobbyView(Context context) {
        this.context = context;
        this.bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        this.canvas = new Canvas(bitmap);
        this.prefs = context.getSharedPreferences("wivrn_servers", Context.MODE_PRIVATE);

        initPaints();
        loadServers();
        loadSettings();
        startDiscovery();
        updateWifiStatus();
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
                    true
                ));
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to load servers", e);
        }

        if (servers.isEmpty()) {
            servers.add(new ServerEntry("Local", "127.0.0.1", 9757, true, true));
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
                arr.put(obj);
            }
            prefs.edit().putString("servers", arr.toString()).apply();
        } catch (Exception e) {
            Log.e(TAG, "Failed to save servers", e);
        }
    }

    private void loadSettings() {
        SharedPreferences sp = context.getSharedPreferences("wivrn_settings", Context.MODE_PRIVATE);
        resolutionScale = sp.getInt("resolution_scale", 100);
        foveationScale = sp.getInt("foveation_scale", 30);
        codec = sp.getString("codec", "auto");
        bitrate = sp.getInt("bitrate", 50);
        tcpOnly = sp.getBoolean("tcp_only", false);
        microphoneEnabled = sp.getBoolean("microphone", false);
        highPowerMode = sp.getBoolean("high_power", false);
    }

    private void saveSettings() {
        SharedPreferences sp = context.getSharedPreferences("wivrn_settings", Context.MODE_PRIVATE);
        sp.edit()
            .putInt("resolution_scale", resolutionScale)
            .putInt("foveation_scale", foveationScale)
            .putString("codec", codec)
            .putInt("bitrate", bitrate)
            .putBoolean("tcp_only", tcpOnly)
            .putBoolean("microphone", microphoneEnabled)
            .putBoolean("high_power", highPowerMode)
            .apply();
    }

    public Bitmap getBitmap() {
        return bitmap;
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

    public void updateRunningApps(String[] names, int[] ids, boolean[] overlays, boolean[] actives) {
        this.runningApps = names != null ? names : new String[0];
        this.runningAppIds = ids != null ? ids : new int[0];
        this.runningAppOverlays = overlays != null ? overlays : new boolean[0];
        this.runningAppActives = actives != null ? actives : new boolean[0];
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

    public int getResolutionScale() { return resolutionScale; }
    public int getFoveationScale() { return foveationScale; }
    public String getCodec() { return codec; }
    public int getBitrate() { return bitrate; }
    public boolean isTcpOnly() { return tcpOnly; }
    public boolean isMicrophoneEnabled() { return microphoneEnabled; }
    public boolean isHighPowerMode() { return highPowerMode; }

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

        markClean();
    }

    private void renderSidebar() {
        canvas.drawRect(0, 0, SIDEBAR_WIDTH, height, sidebarBgPaint);

        String[] tabs = {"Server List", "Settings", "About", "Licenses", "Exit"};
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
        canvas.drawText("Server List", x, y + 30, textLargePaint);
        y += 60;

        RectF addBtn = new RectF(x + w - BUTTON_WIDTH, y - BUTTON_HEIGHT, x + w, y);
        boolean addHover = touchDown && addBtn.contains(touchX, touchY);
        canvas.drawRoundRect(addBtn, 10, 10, addHover ? buttonHoverBgPaint : buttonBgPaint);
        drawCenteredText("+ Add Server", addBtn, textPaint);

        y += 30;

        List<ServerEntry> allServers = getAllServers();
        for (int i = 0; i < allServers.size(); i++) {
            ServerEntry s = allServers.get(i);
            float cardY = y + i * (CARD_HEIGHT + 10);
            RectF card = new RectF(x, cardY, x + w, cardY + CARD_HEIGHT);

            canvas.drawRoundRect(card, 12, 12, cardBgPaint);

            canvas.drawText(s.name, x + 20, cardY + 35, textPaint);
            canvas.drawText(s.hostname + ":" + s.port + (s.tcpOnly ? " (TCP)" : ""), x + 20, cardY + 65, textSmallPaint);

            if (s.discovered) {
                textSmallPaint.setColor(Color.rgb(80, 200, 120));
                canvas.drawText("● discovered", x + 20, cardY + 88, textSmallPaint);
                textSmallPaint.setColor(Color.rgb(160, 170, 185));
            }

            RectF connectBtn = new RectF(x + w - BUTTON_WIDTH - 20, cardY + 20, x + w - 20, cardY + 20 + BUTTON_HEIGHT);
            boolean connectHover = touchDown && connectBtn.contains(touchX, touchY);
            canvas.drawRoundRect(connectBtn, 10, 10, buttonConnectBgPaint);
            drawCenteredText("Connect", connectBtn, textPaint);

            if (s.manual) {
                RectF delBtn = new RectF(connectBtn.left - 60, cardY + 20, connectBtn.left - 10, cardY + 20 + BUTTON_HEIGHT);
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
        canvas.drawText("Add Server", popupX + 30, py + 20, textLargePaint);
        py += 60;

        String[] labels = {"Name", "Address", "Port"};
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
        canvas.drawText("TCP only", popupX + 60, py + 16, textPaint);
        py += 50;

        RectF cancelBtn = new RectF(popupX + 30, py, popupX + 30 + 150, py + BUTTON_HEIGHT);
        boolean cancelHover = touchDown && cancelBtn.contains(touchX, touchY);
        canvas.drawRoundRect(cancelBtn, 10, 10, cancelHover ? buttonHoverBgPaint : buttonBgPaint);
        drawCenteredText("Cancel", cancelBtn, textPaint);

        RectF saveBtn = new RectF(popupX + popupW - 180, py, popupX + popupW - 30, py + BUTTON_HEIGHT);
        boolean saveHover = touchDown && saveBtn.contains(touchX, touchY);
        canvas.drawRoundRect(saveBtn, 10, 10, saveHover ? buttonConnectBgPaint : buttonConnectBgPaint);
        drawCenteredText("Save", saveBtn, textPaint);
    }

    private void renderSettings(float x, float w) {
        float y = 30;

        canvas.drawText("Settings", x, y + 30, textLargePaint);
        y += 70;

        y = drawSlider(x, y, w, "Resolution Scale", resolutionScale, 50, 150, "%");
        y = drawSlider(x, y, w, "Foveated Encoding", foveationScale, 0, 80, "%");
        y = drawSlider(x, y, w, "Bitrate", bitrate, 5, 100, "Mbit/s");

        y = drawDropdown(x, y, w, "Codec", new String[]{"Automatic", "H.264", "H.265"},
            codec.equals("auto") ? 0 : codec.equals("h264") ? 1 : 2);

        y = drawCheckbox(x, y, w, "TCP only", tcpOnly);
        y = drawCheckbox(x, y, w, "Enable microphone", microphoneEnabled);
        y = drawCheckbox(x, y, w, "High power mode", highPowerMode);

        y += 20;
        RectF resetBtn = new RectF(x, y, x + 200, y + BUTTON_HEIGHT);
        boolean resetHover = touchDown && resetBtn.contains(touchX, touchY);
        canvas.drawRoundRect(resetBtn, 10, 10, resetHover ? buttonDangerBgPaint : buttonDangerBgPaint);
        drawCenteredText("Restore Defaults", resetBtn, textPaint);

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

        canvas.drawText("Restore all settings to defaults?", px + 30, py + 50, textPaint);

        RectF yesBtn = new RectF(px + 30, py + panelH - 70, px + 30 + 180, py + panelH - 20);
        boolean yesHover = touchDown && yesBtn.contains(touchX, touchY);
        canvas.drawRoundRect(yesBtn, 10, 10, yesHover ? buttonDangerBgPaint : buttonDangerBgPaint);
        drawCenteredText("Reset", yesBtn, textPaint);

        RectF noBtn = new RectF(px + panelW - 210, py + panelH - 70, px + panelW - 30, py + panelH - 20);
        boolean noHover = touchDown && noBtn.contains(touchX, touchY);
        canvas.drawRoundRect(noBtn, 10, 10, noHover ? buttonHoverBgPaint : buttonBgPaint);
        drawCenteredText("Cancel", noBtn, textPaint);
    }

    private float drawSlider(float x, float y, float w, String label, int value, int min, int max, String unit) {
        canvas.drawText(label, x, y + 20, textPaint);
        y += 35;

        float sliderW = w - 100;
        RectF track = new RectF(x, y, x + sliderW, y + 8);
        canvas.drawRoundRect(track, 4, 4, sliderTrackPaint);

        float pct = (float)(value - min) / (max - min);
        RectF fill = new RectF(x, y, x + sliderW * pct, y + 8);
        canvas.drawRoundRect(fill, 4, 4, sliderFillPaint);

        float handleX = x + sliderW * pct;
        canvas.drawCircle(handleX, y + 4, 14, sliderHandlePaint);

        canvas.drawText(value + unit, x + sliderW + 15, y + 12, textSmallPaint);

        y += 30;
        return y + 20;
    }

    private float drawCheckbox(float x, float y, float w, String label, boolean checked) {
        RectF box = new RectF(x, y, x + 24, y + 24);
        canvas.drawRoundRect(box, 4, 4, checked ? accentPaint : dimPaint);
        if (checked) {
            canvas.drawText("X", x + 6, y + 18, textPaint);
        }
        canvas.drawText(label, x + 35, y + 20, textPaint);
        return y + 40;
    }

    private float drawDropdown(float x, float y, float w, String label, String[] options, int selected) {
        canvas.drawText(label, x, y + 20, textPaint);
        y += 35;

        String selectedText = options[selected];
        RectF box = new RectF(x, y, x + 300, y + 40);
        canvas.drawRoundRect(box, 6, 6, dimPaint);
        canvas.drawText(selectedText, x + 15, y + 28, textPaint);
        canvas.drawText("v", x + 280, y + 28, textSmallPaint);

        return y + 50;
    }

    private RectF gitlabBtnRect;

    private void renderAbout(float x, float w) {
        float y = 30;
        canvas.drawText("WiVRn", x, y + 30, textLargePaint);
        y += 70;

        canvas.drawText("Unofficial WiVRn Port", x, y, textPaint);
        y += 40;

        canvas.drawText("VR streaming client for Pico Neo2", x, y, textSmallPaint);
        y += 35;
        canvas.drawText("Based on WiVRn by Guillaume Meunier & Patrick Nicolas", x, y, textSmallPaint);
        y += 35;
        canvas.drawText("Maintainer: HttpAnimations", x, y, textSmallPaint);
        y += 35;
        canvas.drawText("Licensed under GPLv3", x, y, textSmallPaint);
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
        canvas.drawText("Version " + versionName + " (Build " + versionCode + ")", x, y, textSmallPaint);
        y += 60;

        gitlabBtnRect = new RectF(x, y, x + 280, y + BUTTON_HEIGHT);
        boolean gitlabHover = touchDown && gitlabBtnRect.contains(touchX, touchY);
        canvas.drawRoundRect(gitlabBtnRect, 10, 10, gitlabHover ? buttonHoverBgPaint : buttonBgPaint);
        drawCenteredText("GitLab Repository", gitlabBtnRect, textPaint);
    }

    private void renderLicenses(float x, float w) {
        float y = 30;
        canvas.drawText("Licenses", x, y + 30, textLargePaint);
        y += 70;

        String licenseText = "WiVRn - GPL v3\n\n"
            + "This program is free software: you can redistribute it and/or modify "
            + "it under the terms of the GNU General Public License as published by "
            + "the Free Software Foundation, either version 3 of the License, or "
            + "(at your option) any later version.\n\n"
            + "This program is distributed in the hope that it will be useful, "
            + "but WITHOUT ANY WARRANTY; without even the implied warranty of "
            + "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n\n"
            + "See the GNU General Public License for more details.";

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
        canvas.drawText("Exit WiVRn?", x + 50, y, textLargePaint);
        y += 60;

        RectF yesBtn = new RectF(x + 50, y, x + 50 + 200, y + BUTTON_HEIGHT);
        boolean yesHover = touchDown && yesBtn.contains(touchX, touchY);
        canvas.drawRoundRect(yesBtn, 10, 10, yesHover ? buttonDangerBgPaint : buttonDangerBgPaint);
        drawCenteredText("Exit", yesBtn, textPaint);

        RectF noBtn = new RectF(x + 270, y, x + 270 + 200, y + BUTTON_HEIGHT);
        boolean noHover = touchDown && noBtn.contains(touchX, touchY);
        canvas.drawRoundRect(noBtn, 10, 10, noHover ? buttonHoverBgPaint : buttonBgPaint);
        drawCenteredText("Cancel", noBtn, textPaint);
    }

    private void renderPinEntry() {
        canvas.drawRect(0, 0, width, height, bgPaint);

        float panelW = 600;
        float panelH = 500;
        float px = (width - panelW) / 2;
        float py = (height - panelH) / 2;

        RectF panel = new RectF(px, py, px + panelW, py + panelH);
        canvas.drawRoundRect(panel, 16, 16, cardBgPaint);

        canvas.drawText("Enter PIN", px + 30, py + 40, textLargePaint);
        canvas.drawText("Displayed on the server dashboard", px + 30, py + 70, textSmallPaint);

        RectF pinDisplay = new RectF(px + 30, py + 90, px + panelW - 30, py + 140);
        canvas.drawRoundRect(pinDisplay, 8, 8, pinDisplayPaint);
        String displayPin = pinBuffer.isEmpty() ? "PIN" : pinBuffer;
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

        canvas.drawText("Connecting", px + 30, py + 50, textLargePaint);
        canvas.drawText(statusMessage, px + 30, py + 90, textPaint);

        RectF disconnectBtn = new RectF(px + panelW - 220, py + panelH - 80, px + panelW - 20, py + panelH - 20);
        boolean hover = touchDown && disconnectBtn.contains(touchX, touchY);
        canvas.drawRoundRect(disconnectBtn, 10, 10, hover ? buttonDangerBgPaint : buttonDangerBgPaint);
        drawCenteredText("Disconnect", disconnectBtn, textPaint);
    }

    private void renderDisconnected() {
        canvas.drawRect(0, 0, width, height, bgPaint);

        float panelW = 600;
        float panelH = 250;
        float px = (width - panelW) / 2;
        float py = (height - panelH) / 2;

        RectF panel = new RectF(px, py, px + panelW, py + panelH);
        canvas.drawRoundRect(panel, 16, 16, cardBgPaint);

        canvas.drawText("Disconnected", px + 30, py + 50, textLargePaint);
        canvas.drawText(errorMessage, px + 30, py + 90, textPaint);

        RectF closeBtn = new RectF(px + panelW - 220, py + panelH - 80, px + panelW - 20, py + panelH - 20);
        boolean hover = touchDown && closeBtn.contains(touchX, touchY);
        canvas.drawRoundRect(closeBtn, 10, 10, hover ? buttonHoverBgPaint : buttonBgPaint);
        drawCenteredText("Close", closeBtn, textPaint);
    }

    private void renderConnected() {
        canvas.drawRect(0, 0, width, height, bgPaint);

        canvas.drawRect(0, 0, SIDEBAR_WIDTH, height, sidebarBgPaint);

        String[] tabs = {"Applications", "Launch", "Stats", "Settings"};
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
        canvas.drawText("Disconnect", 25, disconnectY + TAB_HEIGHT / 2f + 10, textPaint);

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
                renderStreamSettings(contentX, contentW);
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
        canvas.drawText("Running XR Applications", x, y + 30, textLargePaint);
        y += 70;

        if (runningApps == null || runningApps.length == 0) {
            textDimPaint.setColor(Color.rgb(100, 110, 125));
            canvas.drawText("No applications running", x, y + 20, textDimPaint);
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
                canvas.drawText("Overlays", x, y + 20, textDimPaint);
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
        canvas.drawText("Launch Application", x, y + 30, textLargePaint);
        y += 70;

        if (availableAppNames == null || availableAppNames.length == 0) {
            textDimPaint.setColor(Color.rgb(100, 110, 125));
            if (appListRequested) {
                canvas.drawText("Loading applications...", x, y + 20, textDimPaint);
            } else {
                canvas.drawText("No applications available", x, y + 20, textDimPaint);
                canvas.drawText("Press to refresh", x, y + 60, textDimPaint);
                RectF refreshBtn = new RectF(x, y + 80, x + 200, y + 80 + BUTTON_HEIGHT);
                boolean hover = touchDown && refreshBtn.contains(touchX, touchY);
                canvas.drawRoundRect(refreshBtn, 10, 10, hover ? buttonHoverBgPaint : buttonBgPaint);
                textPaint.setColor(Color.rgb(230, 235, 245));
                canvas.drawText("Refresh", x + 50, y + 80 + BUTTON_HEIGHT / 2f + 8, textPaint);
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
    }

    private void renderStreamStats(float x, float w) {
        float y = 40;
        canvas.drawText("Performance Statistics", x, y + 30, textLargePaint);
        y += 70;

        String[][] stats = {
            {"FPS", streamFps > 0 ? streamFps + " fps" : "--"},
            {"Motion to Photon Latency", streamLatencyMs > 0 ? streamLatencyMs + " ms" : "--"},
            {"Download", String.format("%.1f Mbit/s", streamBandwidthRx * 1e-6f)},
            {"Upload", String.format("%.1f Mbit/s", streamBandwidthTx * 1e-6f)},
            {"Bitrate", streamBitrateMbps + " Mbit/s"},
        };

        float labelW = 320;
        float valueX = x + labelW;

        for (String[] stat : stats) {
            textSmallPaint.setColor(Color.rgb(140, 150, 165));
            canvas.drawText(stat[0], x, y, textSmallPaint);
            textPaint.setColor(Color.rgb(230, 235, 245));
            canvas.drawText(stat[1], valueX, y, textPaint);
            y += 42;
        }

        textSmallPaint.setColor(Color.rgb(140, 150, 165));

        y += 20;
        Paint sepPaint = new Paint();
        sepPaint.setColor(Color.rgb(50, 60, 75));
        canvas.drawRect(x, y, x + w, y + 1, sepPaint);
        y += 20;

        canvas.drawText("Resolution", x, y, textSmallPaint);
        textPaint.setColor(Color.rgb(230, 235, 245));
        canvas.drawText(streamResolutionScale + "%", valueX, y, textPaint);
        y += 42;

        canvas.drawText("Microphone", x, y, textSmallPaint);
        textPaint.setColor(streamMicEnabled ? Color.rgb(120, 200, 120) : Color.rgb(200, 120, 120));
        canvas.drawText(streamMicEnabled ? "Enabled" : "Disabled", valueX, y, textPaint);
        y += 42;

        canvas.drawText("High Power Mode", x, y, textSmallPaint);
        textPaint.setColor(streamHighPower ? Color.rgb(120, 200, 120) : Color.rgb(200, 120, 120));
        canvas.drawText(streamHighPower ? "Enabled" : "Disabled", valueX, y, textPaint);
        y += 42;

        y += 30;
        textDimPaint.setColor(Color.rgb(100, 110, 125));
        canvas.drawText("Press both thumbsticks to toggle this overlay", x, y, textDimPaint);
        textDimPaint.setColor(Color.rgb(100, 110, 125));
    }

    private void renderStreamSettings(float x, float w) {
        float y = 40;
        canvas.drawText("Stream Settings", x, y + 30, textLargePaint);
        y += 70;

        canvas.drawText("Bitrate", x, y, textPaint);
        canvas.drawText(streamBitrateSetting + " Mbit/s", x + w - 150, y, textPaint);
        y += 35;

        RectF track = new RectF(x, y, x + w - 150, y + 8);
        canvas.drawRoundRect(track, 4, 4, sliderTrackPaint);
        float fillW = (w - 150) * (streamBitrateSetting / 100f);
        RectF fill = new RectF(x, y, x + fillW, y + 8);
        canvas.drawRoundRect(fill, 4, 4, sliderFillPaint);
        float handleX = x + fillW;
        canvas.drawCircle(handleX, y + 4, 12, sliderHandlePaint);
        y += 50;

        canvas.drawText("Resolution Scale", x, y, textPaint);
        canvas.drawText(streamResolutionScale + "%", x + w - 150, y, textPaint);
        y += 35;

        RectF track2 = new RectF(x, y, x + w - 150, y + 8);
        canvas.drawRoundRect(track2, 4, 4, sliderTrackPaint);
        float fillW2 = (w - 150) * (streamResolutionScale / 200f);
        RectF fill2 = new RectF(x, y, x + fillW2, y + 8);
        canvas.drawRoundRect(fill2, 4, 4, sliderFillPaint);
        float handleX2 = x + fillW2;
        canvas.drawCircle(handleX2, y + 4, 12, sliderHandlePaint);
        y += 50;

        canvas.drawText("Microphone", x, y + 10, textPaint);
        RectF micToggle = new RectF(x + w - 80, y, x + w - 20, y + 40);
        Paint togglePaint = new Paint();
        togglePaint.setAntiAlias(true);
        togglePaint.setColor(streamMicEnabled ? Color.rgb(30, 140, 60) : Color.rgb(50, 55, 65));
        canvas.drawRoundRect(micToggle, 20, 20, togglePaint);
        float knobX = streamMicEnabled ? micToggle.right - 20 : micToggle.left + 20;
        canvas.drawCircle(knobX, micToggle.centerY(), 16, new Paint());
        Paint knobPaint = new Paint();
        knobPaint.setAntiAlias(true);
        knobPaint.setColor(Color.WHITE);
        canvas.drawCircle(knobX, micToggle.centerY(), 16, knobPaint);
        y += 60;

        canvas.drawText("High Power Mode", x, y + 10, textPaint);
        RectF powerToggle = new RectF(x + w - 80, y, x + w - 20, y + 40);
        Paint powerPaint = new Paint();
        powerPaint.setAntiAlias(true);
        powerPaint.setColor(streamHighPower ? Color.rgb(30, 140, 60) : Color.rgb(50, 55, 65));
        canvas.drawRoundRect(powerToggle, 20, 20, powerPaint);
        float knobX2 = streamHighPower ? powerToggle.right - 20 : powerToggle.left + 20;
        Paint knobPaint2 = new Paint();
        knobPaint2.setAntiAlias(true);
        knobPaint2.setColor(Color.WHITE);
        canvas.drawCircle(knobX2, powerToggle.centerY(), 16, knobPaint2);
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

        if (connectionState == STATE_CONNECTED && streamTab == STREAM_TAB_LAUNCH) {
            if (down && prevDown && y != prevY) {
                if (dragStartY < 0) {
                    dragStartY = prevY;
                    dragStartScroll = launchScrollY;
                }
                launchScrollY = dragStartScroll + (dragStartY - y);
                launchScrollY = Math.max(0, Math.min(launchMaxScroll, launchScrollY));
            }
            if (!down) {
                dragStartY = -1;
            }

            float stickMag = Math.abs(thumbstickY);
            if (stickMag > 0.3f) {
                thumbstickAccum += thumbstickY * 15f;
                if (Math.abs(thumbstickAccum) >= 1f) {
                    launchScrollY = Math.max(0, Math.min(launchMaxScroll, launchScrollY + thumbstickAccum));
                    thumbstickAccum = 0;
                }
            } else {
                thumbstickAccum = 0;
            }
        }

        prevTouchY = touchY;

        if (pressed) {
            handleClick(x, y);
        }
        markDirty();
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
                handleSettingsClick(x, adjustedY);
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
                        markDirty();
                        return;
                    }
                }
            }
        }

        if (streamTab == STREAM_TAB_SETTINGS) {
            float sy = 40 + 70;
            float sliderY = sy + 35;
            RectF bitrateTrack = new RectF(contentX, sliderY, contentX + contentW - 150, sliderY + 20);
            if (bitrateTrack.contains(x, y)) {
                float pct = (x - contentX) / (contentW - 150);
                streamBitrateSetting = (int)Math.max(1, Math.min(100, pct * 100));
                markDirty();
                return;
            }

            sliderY += 85;
            RectF resTrack = new RectF(contentX, sliderY, contentX + contentW - 150, sliderY + 20);
            if (resTrack.contains(x, y)) {
                float pct = (x - contentX) / (contentW - 150);
                streamResolutionScale = (int)Math.max(10, Math.min(200, pct * 200));
                markDirty();
                return;
            }

            sliderY += 50;
            RectF micToggle = new RectF(contentX + contentW - 80, sliderY, contentX + contentW - 20, sliderY + 40);
            if (micToggle.contains(x, y)) {
                streamMicEnabled = !streamMicEnabled;
                markDirty();
                return;
            }

            sliderY += 60;
            RectF powerToggle = new RectF(contentX + contentW - 80, sliderY, contentX + contentW - 20, sliderY + 40);
            if (powerToggle.contains(x, y)) {
                streamHighPower = !streamHighPower;
                markDirty();
                return;
            }
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
            addServerTcpOnly = false;
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
                ((MainActivity) context).onServerConnect(s.hostname, s.port, s.tcpOnly);
                markDirty();
                return;
            }

            if (s.manual) {
                RectF delBtn = new RectF(connectBtn.left - 60, cardY + 20, connectBtn.left - 10, cardY + 20 + BUTTON_HEIGHT);
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
        float sy = 100;

        float sliderW = contentW - 100;
        sy += 35;
        if (y >= sy - 10 && y <= sy + 20 && x >= contentX && x <= contentX + sliderW) {
            float pct = (x - contentX) / sliderW;
            resolutionScale = (int)(50 + pct * 100);
            resolutionScale = Math.max(50, Math.min(150, resolutionScale));
            saveSettings();
            markDirty();
            return;
        }
        sy += 50;

        sy += 35;
        if (y >= sy - 10 && y <= sy + 20 && x >= contentX && x <= contentX + sliderW) {
            float pct = (x - contentX) / sliderW;
            foveationScale = (int)(pct * 80);
            foveationScale = Math.max(0, Math.min(80, foveationScale));
            saveSettings();
            markDirty();
            return;
        }
        sy += 50;

        sy += 35;
        if (y >= sy - 10 && y <= sy + 20 && x >= contentX && x <= contentX + sliderW) {
            float pct = (x - contentX) / sliderW;
            bitrate = (int)(5 + pct * 95);
            bitrate = Math.max(5, Math.min(100, bitrate));
            saveSettings();
            markDirty();
            return;
        }
        sy += 50;

        RectF codecBox = new RectF(contentX, sy, contentX + 300, sy + 40);
        if (codecBox.contains(x, y)) {
            String[] codecs = {"auto", "h264", "h265"};
            int idx = 0;
            for (int i = 0; i < codecs.length; i++) {
                if (codecs[i].equals(codec)) { idx = i; break; }
            }
            idx = (idx + 1) % codecs.length;
            codec = codecs[idx];
            saveSettings();
            markDirty();
            return;
        }
        sy += 50;

        RectF tcpBox = new RectF(contentX, sy, contentX + 24, sy + 24);
        if (tcpBox.contains(x, y) || (x > contentX && x < contentX + 200 && y > sy && y < sy + 30)) {
            tcpOnly = !tcpOnly;
            saveSettings();
            markDirty();
            return;
        }
        sy += 40;

        RectF micBox = new RectF(contentX, sy, contentX + 24, sy + 24);
        if (micBox.contains(x, y) || (x > contentX && x < contentX + 250 && y > sy && y < sy + 30)) {
            microphoneEnabled = !microphoneEnabled;
            saveSettings();
            markDirty();
            return;
        }
        sy += 40;

        RectF hpBox = new RectF(contentX, sy, contentX + 24, sy + 24);
        if (hpBox.contains(x, y) || (x > contentX && x < contentX + 250 && y > sy && y < sy + 30)) {
            highPowerMode = !highPowerMode;
            saveSettings();
            markDirty();
            return;
        }

        // Restore Defaults button
        sy += 40;
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
            resolutionScale = 100;
            foveationScale = 30;
            codec = "auto";
            bitrate = 50;
            tcpOnly = false;
            microphoneEnabled = false;
            highPowerMode = false;
            saveSettings();
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

        RectF closeBtn = new RectF(px + panelW - 220, py + panelH - 80, px + panelW - 20, py + panelH - 20);
        if (closeBtn.contains(x, y)) {
            connectionState = STATE_IDLE;
            errorMessage = "";
            markDirty();
        }
    }

    public void setErrorMessage(String msg) {
        errorMessage = msg != null ? msg : "";
        markDirty();
    }
}
