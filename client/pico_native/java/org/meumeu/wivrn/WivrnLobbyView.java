package org.meumeu.wivrn;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

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

    private static final int SIDEBAR_WIDTH = 280;
    private static final int TAB_HEIGHT = 70;
    private static final int CARD_HEIGHT = 100;
    private static final int BUTTON_WIDTH = 200;
    private static final int BUTTON_HEIGHT = 60;

    public static class ServerEntry {
        String name;
        String hostname;
        int port;
        boolean tcpOnly;
        boolean manual;

        ServerEntry(String name, String hostname, int port, boolean tcpOnly, boolean manual) {
            this.name = name;
            this.hostname = hostname;
            this.port = port;
            this.tcpOnly = tcpOnly;
            this.manual = manual;
        }
    }

    public WivrnLobbyView(Context context) {
        this.context = context;
        this.bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        this.canvas = new Canvas(bitmap);
        this.prefs = context.getSharedPreferences("wivrn_servers", Context.MODE_PRIVATE);

        initPaints();
        loadServers();
        loadSettings();
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
            markDirty();
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
            renderContent();
        }

        renderTouchCursor();
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

        for (int i = 0; i < servers.size(); i++) {
            ServerEntry s = servers.get(i);
            float cardY = y + i * (CARD_HEIGHT + 10);
            RectF card = new RectF(x, cardY, x + w, cardY + CARD_HEIGHT);

            canvas.drawRoundRect(card, 12, 12, cardBgPaint);

            canvas.drawText(s.name, x + 20, cardY + 35, textPaint);
            canvas.drawText(s.hostname + ":" + s.port + (s.tcpOnly ? " (TCP)" : ""), x + 20, cardY + 65, textSmallPaint);

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

        y = drawDropdown(x, y, w, "Codec", new String[]{"Automatic", "H.264", "H.265", "AV1"},
            codec.equals("auto") ? 0 : codec.equals("h264") ? 1 : codec.equals("h265") ? 2 : 3);

        y = drawCheckbox(x, y, w, "TCP only", tcpOnly);
        y = drawCheckbox(x, y, w, "Enable microphone", microphoneEnabled);
        y = drawCheckbox(x, y, w, "High power mode", highPowerMode);
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

    private void renderAbout(float x, float w) {
        float y = 50;
        canvas.drawText("WiVRn", x, y + 30, textLargePaint);
        y += 70;

        canvas.drawText("VR streaming client for Pico Neo2", x, y, textPaint);
        y += 40;
        canvas.drawText("Based on WiVRn by Guillaume Meunier & Patrick Nicolas", x, y, textSmallPaint);
        y += 35;
        canvas.drawText("Licensed under GPLv3", x, y, textSmallPaint);
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
        for (String line : licenseText.split("\n")) {
            canvas.drawText(line, x, y, textSmallPaint);
            y += 28;
        }
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
        canvas.drawText("Connected", 50, 50, textLargePaint);
        canvas.drawText("Streaming active", 50, 90, textPaint);
    }

    private void renderTouchCursor() {
        if (touchX >= 0 && touchY >= 0) {
            Paint cursorPaint = new Paint();
            cursorPaint.setColor(touchDown ? Color.argb(180, 80, 160, 240) : Color.argb(100, 200, 200, 200));
            canvas.drawCircle(touchX, touchY, touchDown ? 20 : 15, cursorPaint);
        }
    }

    private void drawCenteredText(String text, RectF rect, Paint paint) {
        Paint.FontMetrics fm = paint.getFontMetrics();
        float textY = rect.top + (rect.height() - (fm.descent - fm.ascent)) / 2 - fm.ascent;
        float textX = rect.left + (rect.width() - paint.measureText(text)) / 2;
        canvas.drawText(text, textX, textY, paint);
    }

    public void handleTouch(float x, float y, boolean down, boolean pressed) {
        float prevX = touchX, prevY = touchY;
        boolean prevDown = touchDown;
        touchX = x;
        touchY = y;
        touchDown = down;
        touchPressed = pressed;

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

        switch (currentTab) {
            case TAB_SERVER_LIST:
                handleServerListClick(x, y);
                break;
            case TAB_SETTINGS:
                handleSettingsClick(x, y);
                break;
            case TAB_EXIT:
                handleExitClick(x, y);
                break;
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

        for (int i = 0; i < servers.size(); i++) {
            ServerEntry s = servers.get(i);
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
                    servers.remove(i);
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
            String[] codecs = {"auto", "h264", "h265", "av1"};
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
