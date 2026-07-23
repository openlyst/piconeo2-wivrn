package org.meumeu.wivrn.neo2.pvr;

import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.TypedValue;
import android.view.ContextThemeWrapper;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.CompoundButton;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;

import com.google.android.material.button.MaterialButton;
import com.google.android.material.card.MaterialCardView;
import com.google.android.material.slider.Slider;
import com.google.android.material.tabs.TabLayout;
import com.google.android.material.textfield.TextInputLayout;

import java.util.ArrayList;
import java.util.List;

/**
 * Builds and manages the VR UI with Material Design components.
 * All 8 tabs: Servers, Settings, Stats, Apps, Launch, About, Licenses, Exit.
 */
public class VrUiController {
    private static final String TAG = "wivrn";

    private final Context mContext;
    private final VrUiPanel mPanel;
    private final Handler mHandler;
    private int mCurrentTab = 0;
    private boolean mStreamingMode = false;

    // Server list data
    public static class ServerEntry {
        public String name, hostname;
        public int port;
        public boolean tcpOnly, discovered, autoconnect, manual;
    }
    private volatile List<ServerEntry> mServers = new ArrayList<>();
    private volatile boolean mConnecting = false;
    private volatile String mConnectionError = "";

    // Stats data (updated from native)
    public static class StatsData {
        public int fps;
        public float totalLatency, downloadMbps, uploadMbps, cpuMs, gpuMs;
        public float encodeMs, sendMs, networkMs, decodeMs, renderMs, blitMs;
        public int bitrate, streamW, streamH;
        public boolean micOn;
    }
    private volatile StatsData mStats = new StatsData();

    // App list data
    public static class AppEntry {
        public String id, name;
        public boolean overlay, active;
    }
    private volatile List<AppEntry> mRunningApps = new ArrayList<>();
    private volatile List<AppEntry> mAvailableApps = new ArrayList<>();

    // Callbacks to native
    public interface Callbacks {
        void onConnect(String hostname, int port, boolean tcpOnly);
        void onDisconnect();
        void onServerRemove(String hostname, int port);
        void onServerAutoconnect(String hostname, int port);
        void onRefreshServers();
        void onSetIpd(float mm);
        void onSetBrightness(float frac);
        void onSetFov(float deg);
        void onSetResolutionScale(float scale);
        void onSetBitrate(int mbps);
        void onSetPassthrough(boolean on);
        void onSetMicrophone(boolean on);
        void onSetEyeFoveation(boolean on);
        void onSetControllerVibration(float frac);
        void onRecenter();
        void onSetDiagHud(int mode);
        void onQuit();
        void onStartApp(String appId);
        void onStopApp(int appId);
    }
    private Callbacks mCb;

    // Settings state (mirrors native atomics)
    private float mIpd = 65.0f;
    private float mBrightness = 1.0f;
    private float mFov = 101.0f;
    private float mResScale = 1.0f;
    private float mBitrate = 50.0f;
    private boolean mEyeFoveation = true;
    private boolean mMicrophone = false;
    private boolean mPassthrough = false;
    private float mCtrlVibration = 1.0f;
    private int mDiagHud = 0;
    private boolean mEyeSupported = true;

    public VrUiController(Context context, VrUiPanel panel) {
        mContext = new ContextThemeWrapper(context, R.style.VrMaterialTheme);
        mPanel = panel;
        mHandler = new Handler(Looper.getMainLooper());
    }

    public void setCallbacks(Callbacks cb) { mCb = cb; }

    public void setStreamingMode(boolean streaming) {
        mStreamingMode = streaming;
        if (streaming && mCurrentTab == 0) mCurrentTab = 1;
        mHandler.post(this::rebuild);
    }

    public void setServers(List<ServerEntry> servers) {
        mServers = servers;
        mHandler.post(() -> { if (mCurrentTab == 0) rebuild(); });
    }

    public void setConnecting(boolean connecting) {
        mConnecting = connecting;
        mHandler.post(() -> { if (mCurrentTab == 0) rebuild(); });
    }

    public void setConnectionError(String err) {
        mConnectionError = err;
        mHandler.post(() -> { if (mCurrentTab == 0) rebuild(); });
    }

    public void setStats(StatsData stats) {
        mStats = stats;
        mHandler.post(() -> { if (mCurrentTab == 2) rebuild(); });
    }

    public void setRunningApps(List<AppEntry> apps) {
        mRunningApps = apps;
        mHandler.post(() -> { if (mCurrentTab == 3) rebuild(); });
    }

    public void setAvailableApps(List<AppEntry> apps) {
        mAvailableApps = apps;
        mHandler.post(() -> { if (mCurrentTab == 4) rebuild(); });
    }

    public void updateSettings(float ipd, float brightness, float fov, float resScale,
            float bitrate, boolean eyeFov, boolean mic, boolean passthrough,
            float ctrlVib, int diagHud, boolean eyeSupported) {
        mIpd = ipd; mBrightness = brightness; mFov = fov; mResScale = resScale;
        mBitrate = bitrate; mEyeFoveation = eyeFov; mMicrophone = mic;
        mPassthrough = passthrough; mCtrlVibration = ctrlVib; mDiagHud = diagHud;
        mEyeSupported = eyeSupported;
        mHandler.post(() -> { if (mCurrentTab == 1) rebuild(); });
    }

    public void rebuild() {
        View root = buildLayout();
        mPanel.setContentView(root);
    }

    private int dp(int val) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, val,
                mContext.getResources().getDisplayMetrics());
    }

    private int sp(int val) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, val,
                mContext.getResources().getDisplayMetrics());
    }

    private LinearLayout rootLayout() {
        LinearLayout root = new LinearLayout(mContext);
        root.setOrientation(LinearLayout.HORIZONTAL);
        root.setLayoutParams(new ViewGroup.LayoutParams(VrUiPanel.UI_WIDTH, VrUiPanel.UI_HEIGHT));
        root.setBackgroundColor(Color.parseColor("#1e1e2e"));
        return root;
    }

    private LinearLayout sidebar() {
        LinearLayout sidebar = new LinearLayout(mContext);
        sidebar.setOrientation(LinearLayout.VERTICAL);
        sidebar.setBackgroundColor(Color.parseColor("#000000"));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(220), ViewGroup.LayoutParams.MATCH_PARENT);
        sidebar.setLayoutParams(lp);
        sidebar.setPadding(0, dp(16), 0, dp(16));
        return sidebar;
    }

    private ScrollView contentArea() {
        ScrollView scroll = new ScrollView(mContext);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f);
        scroll.setLayoutParams(lp);
        scroll.setBackgroundColor(Color.parseColor("#0d0d14"));
        scroll.setPadding(dp(24), dp(20), dp(24), dp(20));
        scroll.setFillViewport(true);
        return scroll;
    }

    private View buildLayout() {
        LinearLayout root = rootLayout();
        LinearLayout sidebar = sidebar();
        ScrollView content = contentArea();

        // Tab definitions
        String[] topTabs = {"Servers", "Settings", "Stats", "Apps", "Launch"};
        boolean[] topTabStreamingOnly = {false, false, true, true, true};
        boolean[] topTabHideWhileStreaming = {true, false, false, false, false};
        String[] bottomTabs = {"About", "Licenses", "Exit"};

        for (int i = 0; i < topTabs.length; i++) {
            if (topTabStreamingOnly[i] && !mStreamingMode) continue;
            if (topTabHideWhileStreaming[i] && mStreamingMode) continue;
            sidebar.addView(tabButton(topTabs[i], i, mCurrentTab == i));
        }

        // Spacer
        View spacer = new View(mContext);
        spacer.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        sidebar.addView(spacer);

        for (int i = 0; i < bottomTabs.length; i++) {
            int tabId = 5 + i;
            sidebar.addView(tabButton(bottomTabs[i], tabId, mCurrentTab == tabId));
        }

        // Content
        LinearLayout contentInner = new LinearLayout(mContext);
        contentInner.setOrientation(LinearLayout.VERTICAL);
        content.addView(contentInner);

        switch (mCurrentTab) {
            case 0: buildServersTab(contentInner); break;
            case 1: buildSettingsTab(contentInner); break;
            case 2: buildStatsTab(contentInner); break;
            case 3: buildAppsTab(contentInner); break;
            case 4: buildLaunchTab(contentInner); break;
            case 5: buildAboutTab(contentInner); break;
            case 6: buildLicensesTab(contentInner); break;
            case 7: buildExitTab(contentInner); break;
        }

        root.addView(sidebar);
        root.addView(content);
        return root;
    }

    private View tabButton(String name, int id, boolean selected) {
        MaterialButton btn = new MaterialButton(mContext);
        btn.setText(name);
        btn.setGravity(Gravity.CENTER);
        btn.setCornerRadius(dp(8));
        btn.setTextSize(sp(14));
        btn.setAllCaps(false);
        btn.setPadding(0, dp(12), 0, dp(12));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(44));
        lp.setMargins(dp(8), 0, dp(8), dp(6));
        btn.setLayoutParams(lp);

        if (selected) {
            btn.setBackgroundColor(Color.parseColor("#1a5fc4"));
            btn.setTextColor(Color.WHITE);
        } else {
            btn.setBackgroundColor(Color.parseColor("#0a1a33"));
            btn.setTextColor(Color.parseColor("#e0e0e6"));
        }

        btn.setOnClickListener(v -> {
            mCurrentTab = id;
            rebuild();
        });
        return btn;
    }

    private TextView sectionHeader(String text) {
        TextView tv = new TextView(mContext);
        tv.setText(text);
        tv.setTextColor(Color.parseColor("#e8e8ee"));
        tv.setTextSize(sp(18));
        tv.setPadding(0, dp(8), 0, dp(4));
        return tv;
    }

    private TextView labelText(String text) {
        TextView tv = new TextView(mContext);
        tv.setText(text);
        tv.setTextColor(Color.parseColor("#e0e0e6"));
        tv.setTextSize(sp(14));
        return tv;
    }

    private TextView dimText(String text) {
        TextView tv = new TextView(mContext);
        tv.setText(text);
        tv.setTextColor(Color.parseColor("#80808a"));
        tv.setTextSize(sp(13));
        return tv;
    }

    private TextView valueText(String text) {
        TextView tv = new TextView(mContext);
        tv.setText(text);
        tv.setTextColor(Color.WHITE);
        tv.setTextSize(sp(14));
        return tv;
    }

    private MaterialCardView card(View content) {
        MaterialCardView card = new MaterialCardView(mContext);
        card.setCardBackgroundColor(Color.parseColor("#161620"));
        card.setCardElevation(0);
        card.setRadius(dp(8));
        card.setUseCompatPadding(true);
        card.setContentPadding(dp(16), dp(12), dp(16), dp(12));
        card.addView(content);
        return card;
    }

    // ---- Servers tab ----
    private void buildServersTab(LinearLayout container) {
        container.addView(sectionHeader("Servers"));

        if (!mConnectionError.isEmpty()) {
            TextView err = new TextView(mContext);
            err.setText(mConnectionError);
            err.setTextColor(Color.parseColor("#ff6b6b"));
            err.setTextSize(sp(13));
            err.setPadding(dp(12), dp(10), dp(12), dp(10));
            err.setBackgroundColor(Color.parseColor("#2a1515"));
            container.addView(err);
            container.addView(spacer(dp(8)));
        }

        if (mServers.isEmpty()) {
            container.addView(dimText("Start a WiVRn server on your local network"));
            container.addView(spacer(dp(16)));
        }

        for (ServerEntry srv : mServers) {
            LinearLayout row = new LinearLayout(mContext);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(dp(16), dp(12), dp(16), dp(12));
            row.setBackgroundColor(Color.parseColor("#161620"));

            // Left: name + host
            LinearLayout info = new LinearLayout(mContext);
            info.setOrientation(LinearLayout.VERTICAL);
            info.setLayoutParams(new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

            TextView name = new TextView(mContext);
            name.setText(srv.name);
            name.setTextColor(Color.parseColor("#e8e8ee"));
            name.setTextSize(sp(16));
            info.addView(name);

            TextView host = new TextView(mContext);
            host.setText(srv.hostname + ":" + srv.port);
            host.setTextColor(Color.parseColor("#80808a"));
            host.setTextSize(sp(13));
            info.addView(host);

            if (srv.discovered) {
                TextView disc = new TextView(mContext);
                disc.setText("Discovered");
                disc.setTextColor(Color.parseColor("#4daa6a"));
                disc.setTextSize(sp(11));
                info.addView(disc);
            }
            row.addView(info);

            // Auto checkbox
            if (!srv.manual) {
                CheckBox autoCb = new CheckBox(mContext);
                autoCb.setText("Auto");
                autoCb.setTextColor(Color.parseColor("#e0e0e6"));
                autoCb.setTextSize(sp(12));
                autoCb.setChecked(srv.autoconnect);
                autoCb.setOnCheckedChangeListener((v, checked) -> {
                    if (mCb != null) mCb.onServerAutoconnect(srv.hostname, srv.port);
                });
                row.addView(autoCb);
            }

            // Connect button
            MaterialButton connBtn = new MaterialButton(mContext);
            connBtn.setText(mConnecting ? "..." : "Connect");
            connBtn.setCornerRadius(dp(6));
            connBtn.setAllCaps(false);
            connBtn.setBackgroundColor(Color.parseColor("#1a6630"));
            connBtn.setTextColor(Color.WHITE);
            connBtn.setOnClickListener(v -> {
                if (mCb != null) mCb.onConnect(srv.hostname, srv.port, srv.tcpOnly);
            });
            row.addView(connBtn);

            // X button
            MaterialButton xBtn = new MaterialButton(mContext);
            xBtn.setText("X");
            xBtn.setCornerRadius(dp(6));
            xBtn.setAllCaps(false);
            xBtn.setBackgroundColor(Color.parseColor("#661a1a"));
            xBtn.setTextColor(Color.WHITE);
            LinearLayout.LayoutParams xLp = new LinearLayout.LayoutParams(dp(40), dp(40));
            xLp.setMargins(dp(8), 0, 0, 0);
            xBtn.setLayoutParams(xLp);
            xBtn.setOnClickListener(v -> {
                if (mCb != null) mCb.onServerRemove(srv.hostname, srv.port);
            });
            row.addView(xBtn);

            MaterialCardView rowCard = card(row);
            LinearLayout.LayoutParams rowLp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            rowLp.setMargins(0, 0, 0, dp(6));
            rowCard.setLayoutParams(rowLp);
            container.addView(rowCard);
        }

        container.addView(spacer(dp(12)));

        MaterialButton refreshBtn = new MaterialButton(mContext);
        refreshBtn.setText("Refresh");
        refreshBtn.setCornerRadius(dp(6));
        refreshBtn.setAllCaps(false);
        refreshBtn.setBackgroundColor(Color.parseColor("#1a2a3a"));
        refreshBtn.setTextColor(Color.parseColor("#a0c0e0"));
        LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(dp(140), dp(40));
        rlp.gravity = Gravity.CENTER;
        refreshBtn.setLayoutParams(rlp);
        refreshBtn.setOnClickListener(v -> {
            if (mCb != null) mCb.onRefreshServers();
        });
        container.addView(refreshBtn);
    }

    // ---- Settings tab ----
    private void buildSettingsTab(LinearLayout container) {
        container.addView(sectionHeader("Settings"));

        // Software IPD
        LinearLayout ipdRow = settingRow("Software IPD", String.format("%.1f mm", mIpd));
        MaterialButton ipdMinus = stepButton("-");
        ipdMinus.setOnClickListener(v -> {
            mIpd = Math.max(55.0f, mIpd - 0.5f);
            if (mCb != null) mCb.onSetIpd(mIpd);
            rebuild();
        });
        MaterialButton ipdPlus = stepButton("+");
        ipdPlus.setOnClickListener(v -> {
            mIpd = Math.min(80.0f, mIpd + 0.5f);
            if (mCb != null) mCb.onSetIpd(mIpd);
            rebuild();
        });
        ipdRow.addView(ipdMinus);
        ipdRow.addView(ipdPlus);
        container.addView(card(ipdRow));
        container.addView(spacer(dp(6)));

        // Brightness slider
        container.addView(card(sliderRow("Brightness", (int)(mBrightness * 100), 0, 100, "%d%%",
                val -> {
                    mBrightness = val / 100.0f;
                    if (mCb != null) mCb.onSetBrightness(mBrightness);
                })));
        container.addView(spacer(dp(6)));

        // FOV slider
        container.addView(card(sliderRow("Field of view", (int)mFov, 80, 110, "%d deg",
                val -> {
                    mFov = val;
                    if (mCb != null) mCb.onSetFov(val);
                })));
        container.addView(spacer(dp(6)));

        // Resolution scale slider
        int rw = (int)((1664 * mResScale) / 2) * 2;
        int rh = (int)((1756 * mResScale) / 2) * 2;
        container.addView(card(sliderRow("Resolution scale " + String.format("%d%% (%dx%d)",
                (int)(mResScale * 100), rw, rh),
                (int)(mResScale * 100), 50, 200, "%d%%",
                val -> {
                    mResScale = val / 100.0f;
                    if (mCb != null) mCb.onSetResolutionScale(mResScale);
                    rebuild();
                })));
        container.addView(spacer(dp(6)));

        // Bitrate slider
        container.addView(card(sliderRow("Bitrate", (int)mBitrate, 5, 200, "%d Mbps",
                val -> {
                    mBitrate = val;
                    if (mCb != null) mCb.onSetBitrate(val);
                })));
        container.addView(spacer(dp(6)));

        // Eye-tracked foveation toggle
        if (mEyeSupported) {
            container.addView(card(toggleRow("Eye-tracked foveation", mEyeFoveation, (v, val) -> {
                mEyeFoveation = val;
                if (mCb != null) mCb.onSetEyeFoveation(val);
            })));
            container.addView(spacer(dp(6)));
        }

        // Microphone toggle
        container.addView(card(toggleRow("Microphone", mMicrophone, (v, val) -> {
            mMicrophone = val;
            if (mCb != null) mCb.onSetMicrophone(val);
        })));
        container.addView(spacer(dp(6)));

        // Controller vibration slider
        container.addView(card(sliderRow("Controller vibration", (int)(mCtrlVibration * 100), 0, 100, "%d%%",
                val -> {
                    mCtrlVibration = val / 100.0f;
                    if (mCb != null) mCb.onSetControllerVibration(mCtrlVibration);
                })));
        container.addView(spacer(dp(6)));

        // Passthrough toggle
        container.addView(card(toggleRow("Passthrough", mPassthrough, (v, val) -> {
            mPassthrough = val;
            if (mCb != null) mCb.onSetPassthrough(val);
        })));
        container.addView(spacer(dp(6)));

        // Recenter button
        MaterialButton recenterBtn = new MaterialButton(mContext);
        recenterBtn.setText("Recenter");
        recenterBtn.setCornerRadius(dp(6));
        recenterBtn.setAllCaps(false);
        recenterBtn.setBackgroundColor(Color.parseColor("#2a2a3a"));
        recenterBtn.setTextColor(Color.WHITE);
        recenterBtn.setOnClickListener(v -> { if (mCb != null) mCb.onRecenter(); });
        container.addView(recenterBtn);
        container.addView(spacer(dp(6)));

        // Diagnostics HUD
        String[] diagOpts = {"Off", "Pipeline", "System"};
        LinearLayout diagRow = settingRow("Diagnostics HUD", diagOpts[mDiagHud]);
        for (int i = 0; i < 3; i++) {
            final int mode = i;
            MaterialButton btn = new MaterialButton(mContext);
            btn.setText(diagOpts[i]);
            btn.setAllCaps(false);
            btn.setCornerRadius(dp(4));
            btn.setTextSize(sp(12));
            if (mDiagHud == i) {
                btn.setBackgroundColor(Color.parseColor("#1a5fc4"));
                btn.setTextColor(Color.WHITE);
            } else {
                btn.setBackgroundColor(Color.parseColor("#2a2a3a"));
                btn.setTextColor(Color.parseColor("#a0a0aa"));
            }
            btn.setOnClickListener(v -> {
                mDiagHud = mode;
                if (mCb != null) mCb.onSetDiagHud(mode);
                rebuild();
            });
            diagRow.addView(btn);
        }
        container.addView(card(diagRow));
    }

    // ---- Stats tab ----
    private void buildStatsTab(LinearLayout container) {
        container.addView(sectionHeader("Performance"));

        if (!mStreamingMode) {
            container.addView(dimText("Not streaming"));
            return;
        }

        StatsData s = mStats;
        container.addView(statRow("Framerate", s.fps + " fps"));
        container.addView(statRow("Total latency", String.format("%.0f ms", s.totalLatency)));
        container.addView(spacer(dp(8)));
        container.addView(statRow("Download", String.format("%.1f Mbit/s", s.downloadMbps)));
        container.addView(statRow("Upload", String.format("%.1f Mbit/s", s.uploadMbps)));
        container.addView(spacer(dp(8)));
        container.addView(statRow("CPU time", String.format("%.1f ms", s.cpuMs)));
        container.addView(statRow("GPU time", String.format("%.1f ms", s.gpuMs)));
        container.addView(spacer(dp(12)));

        container.addView(sectionHeader("Latency breakdown"));
        container.addView(latencyBar("Encode", s.encodeMs, Color.parseColor("#e69933")));
        container.addView(latencyBar("Send", s.sendMs, Color.parseColor("#ccb344")));
        container.addView(latencyBar("Network", s.networkMs, Color.parseColor("#66b3e6")));
        container.addView(latencyBar("Decode", s.decodeMs, Color.parseColor("#80cc80")));
        container.addView(latencyBar("Render", s.renderMs, Color.parseColor("#b380cc")));
        container.addView(latencyBar("Blit", s.blitMs, Color.parseColor("#9999b3")));
        container.addView(spacer(dp(12)));

        container.addView(sectionHeader("Stream info"));
        container.addView(statRow("Bitrate", s.bitrate + " Mbit/s"));
        container.addView(statRow("Resolution", s.streamW + "x" + s.streamH));
        container.addView(statRow("Microphone", s.micOn ? "On" : "Off"));
    }

    private View latencyBar(String name, float ms, int color) {
        LinearLayout row = new LinearLayout(mContext);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);

        TextView label = new TextView(mContext);
        label.setText(name);
        label.setTextColor(Color.parseColor("#80808a"));
        label.setTextSize(sp(13));
        label.setLayoutParams(new LinearLayout.LayoutParams(dp(100), ViewGroup.LayoutParams.WRAP_CONTENT));
        row.addView(label);

        ProgressBar bar = new ProgressBar(mContext, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(100);
        bar.setProgress((int)(Math.min(ms / 50.0f, 1.0f) * 100));
        bar.getProgressDrawable().setColorFilter(color, android.graphics.PorterDuff.Mode.SRC_IN);
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(0, dp(16), 1f);
        blp.setMargins(dp(8), 0, dp(8), 0);
        bar.setLayoutParams(blp);
        row.addView(bar);

        TextView val = new TextView(mContext);
        val.setText(String.format("%.1f ms", ms));
        val.setTextColor(Color.WHITE);
        val.setTextSize(sp(13));
        val.setLayoutParams(new LinearLayout.LayoutParams(dp(80), ViewGroup.LayoutParams.WRAP_CONTENT));
        row.addView(val);

        return row;
    }

    private View statRow(String label, String value) {
        LinearLayout row = new LinearLayout(mContext);
        row.setOrientation(LinearLayout.HORIZONTAL);
        TextView l = new TextView(mContext);
        l.setText(label);
        l.setTextColor(Color.parseColor("#80808a"));
        l.setTextSize(sp(14));
        l.setLayoutParams(new LinearLayout.LayoutParams(dp(160), ViewGroup.LayoutParams.WRAP_CONTENT));
        row.addView(l);
        TextView v = new TextView(mContext);
        v.setText(value);
        v.setTextColor(Color.WHITE);
        v.setTextSize(sp(14));
        row.addView(v);
        return row;
    }

    // ---- Apps tab ----
    private void buildAppsTab(LinearLayout container) {
        container.addView(sectionHeader("Running apps"));

        if (!mStreamingMode) {
            container.addView(dimText("Not streaming"));
            return;
        }

        if (mRunningApps.isEmpty()) {
            container.addView(dimText("No apps running"));
            return;
        }

        for (AppEntry app : mRunningApps) {
            LinearLayout row = new LinearLayout(mContext);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(dp(16), dp(12), dp(16), dp(12));
            row.setBackgroundColor(Color.parseColor("#161620"));

            TextView name = new TextView(mContext);
            name.setText((app.active ? "> " : "  ") + app.name + (app.overlay ? " (overlay)" : ""));
            name.setTextColor(app.active ? Color.parseColor("#66cc66") : Color.WHITE);
            name.setTextSize(sp(14));
            name.setLayoutParams(new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            row.addView(name);

            MaterialButton stopBtn = new MaterialButton(mContext);
            stopBtn.setText("Stop");
            stopBtn.setAllCaps(false);
            stopBtn.setCornerRadius(dp(6));
            stopBtn.setBackgroundColor(Color.parseColor("#661a1a"));
            stopBtn.setTextColor(Color.WHITE);
            stopBtn.setOnClickListener(v -> {
                if (mCb != null) mCb.onStopApp(0); // app id will be set properly
            });
            row.addView(stopBtn);

            container.addView(card(row));
            container.addView(spacer(dp(4)));
        }
    }

    // ---- Launch tab ----
    private void buildLaunchTab(LinearLayout container) {
        container.addView(sectionHeader("Launch application"));

        if (!mStreamingMode) {
            container.addView(dimText("Not streaming"));
            return;
        }

        if (mAvailableApps.isEmpty()) {
            container.addView(dimText("Loading..."));
            return;
        }

        for (AppEntry app : mAvailableApps) {
            LinearLayout row = new LinearLayout(mContext);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(dp(16), dp(12), dp(16), dp(12));
            row.setBackgroundColor(Color.parseColor("#161620"));

            TextView name = new TextView(mContext);
            name.setText(app.name);
            name.setTextColor(Color.WHITE);
            name.setTextSize(sp(14));
            name.setLayoutParams(new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            row.addView(name);

            MaterialButton launchBtn = new MaterialButton(mContext);
            launchBtn.setText("Launch");
            launchBtn.setAllCaps(false);
            launchBtn.setCornerRadius(dp(6));
            launchBtn.setBackgroundColor(Color.parseColor("#1a6630"));
            launchBtn.setTextColor(Color.WHITE);
            launchBtn.setOnClickListener(v -> {
                if (mCb != null) mCb.onStartApp(app.id);
            });
            row.addView(launchBtn);

            container.addView(card(row));
            container.addView(spacer(dp(4)));
        }
    }

    // ---- About tab ----
    private void buildAboutTab(LinearLayout container) {
        TextView banner = new TextView(mContext);
        banner.setText("WiVRn for Pico Neo 2");
        banner.setTextColor(Color.WHITE);
        banner.setTextSize(sp(20));
        banner.setPadding(dp(16), dp(14), dp(16), dp(14));
        banner.setBackgroundColor(Color.parseColor("#1a5fc4"));
        container.addView(banner);
        container.addView(spacer(dp(12)));

        container.addView(labelText("Stream PC VR games to your Pico Neo 2"));
        container.addView(labelText("over Wi-Fi or USB with low latency"));
        container.addView(spacer(dp(16)));

        container.addView(linkText("Upstream", "https://github.com/wivrn/wivrn"));
        container.addView(linkText("github.com/wivrn/wivrn", "https://github.com/wivrn/wivrn"));
        container.addView(spacer(dp(8)));
        container.addView(linkText("This client", "https://gitlab.com/httpanimations/piconeo2-wivrn"));
        container.addView(linkText("gitlab.com/httpanimations/piconeo2-wivrn", "https://gitlab.com/httpanimations/piconeo2-wivrn"));
        container.addView(spacer(dp(16)));

        container.addView(labelText("Licensed under AGPL v3"));
        container.addView(dimText("See Licenses tab for details"));
    }

    // ---- Licenses tab ----
    private void buildLicensesTab(LinearLayout container) {
        TextView banner = new TextView(mContext);
        banner.setText("AGPL v3");
        banner.setTextColor(Color.WHITE);
        banner.setTextSize(sp(20));
        banner.setPadding(dp(16), dp(14), dp(16), dp(14));
        banner.setBackgroundColor(Color.parseColor("#1a5fc4"));
        container.addView(banner);
        container.addView(spacer(dp(12)));

        container.addView(labelText("This project is licensed under"));
        container.addView(labelText("the GNU Affero General Public License v3"));
        container.addView(spacer(dp(12)));

        container.addView(linkText("Full license text",
                "https://gitlab.com/httpanimations/piconeo2-wivrn/-/raw/main/LICENSE"));
        container.addView(spacer(dp(16)));

        container.addView(sectionHeader("Third-party components"));
        container.addView(spacer(dp(8)));
        container.addView(labelText("3D controller models - Pico Interactive"));
        container.addView(dimText("Owned by Pico Interactive"));
        container.addView(spacer(dp(8)));
        container.addView(linkText("ALVR Pico Legacy - MIT License",
                "https://github.com/Juspertinry/alvr-pico-legacy"));
    }

    // ---- Exit tab ----
    private void buildExitTab(LinearLayout container) {
        container.addView(sectionHeader("Exit"));
        container.addView(spacer(dp(20)));

        MaterialButton exitBtn = new MaterialButton(mContext);
        if (mStreamingMode) {
            exitBtn.setText("Disconnect");
            exitBtn.setOnClickListener(v -> { if (mCb != null) mCb.onDisconnect(); });
        } else {
            exitBtn.setText("Quit WiVRn");
            exitBtn.setOnClickListener(v -> { if (mCb != null) mCb.onQuit(); });
        }
        exitBtn.setAllCaps(false);
        exitBtn.setCornerRadius(dp(8));
        exitBtn.setBackgroundColor(Color.parseColor("#661a1a"));
        exitBtn.setTextColor(Color.WHITE);
        exitBtn.setTextSize(sp(16));
        exitBtn.setPadding(dp(48), dp(16), dp(48), dp(16));
        LinearLayout.LayoutParams elp = new LinearLayout.LayoutParams(dp(250), dp(56));
        exitBtn.setLayoutParams(elp);
        container.addView(exitBtn);
    }

    // ---- Helpers ----
    private View spacer(int height) {
        View s = new View(mContext);
        s.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, height));
        return s;
    }

    private LinearLayout settingRow(String label, String value) {
        LinearLayout row = new LinearLayout(mContext);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        TextView l = new TextView(mContext);
        l.setText(label);
        l.setTextColor(Color.parseColor("#e0e0e6"));
        l.setTextSize(sp(14));
        l.setLayoutParams(new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        row.addView(l);
        if (!value.isEmpty()) {
            TextView v = new TextView(mContext);
            v.setText(value);
            v.setTextColor(Color.WHITE);
            v.setTextSize(sp(14));
            v.setPadding(dp(8), 0, dp(8), 0);
            row.addView(v);
        }
        return row;
    }

    private MaterialButton stepButton(String text) {
        MaterialButton btn = new MaterialButton(mContext);
        btn.setText(text);
        btn.setAllCaps(false);
        btn.setCornerRadius(dp(4));
        btn.setBackgroundColor(Color.parseColor("#2a2a3a"));
        btn.setTextColor(Color.WHITE);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(40), dp(40));
        lp.setMargins(dp(4), 0, 0, 0);
        btn.setLayoutParams(lp);
        return btn;
    }

    private interface OnIntValueChange {
        void onChange(int value);
    }

    private View sliderRow(String label, int current, int min, int max, String fmt,
                           OnIntValueChange listener) {
        LinearLayout col = new LinearLayout(mContext);
        col.setOrientation(LinearLayout.VERTICAL);

        TextView labelTv = new TextView(mContext);
        labelTv.setText(label);
        labelTv.setTextColor(Color.parseColor("#e0e0e6"));
        labelTv.setTextSize(sp(14));
        col.addView(labelTv);

        Slider slider = new Slider(mContext);
        slider.setValueFrom(min);
        slider.setValueTo(max);
        slider.setValue(current);
        slider.setStepSize(1);
        slider.addOnChangeListener((s, val, fromUser) -> listener.onChange((int)val));
        col.addView(slider);

        return col;
    }

    private View toggleRow(String label, boolean on, CompoundButton.OnCheckedChangeListener listener) {
        LinearLayout row = new LinearLayout(mContext);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);

        TextView l = new TextView(mContext);
        l.setText(label);
        l.setTextColor(Color.parseColor("#e0e0e6"));
        l.setTextSize(sp(14));
        l.setLayoutParams(new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        row.addView(l);

        Switch sw = new Switch(mContext);
        sw.setChecked(on);
        sw.setOnCheckedChangeListener(listener);
        row.addView(sw);

        return row;
    }

    private View linkText(String text, final String url) {
        TextView tv = new TextView(mContext);
        tv.setText(text);
        tv.setTextColor(Color.parseColor("#5aa6ff"));
        tv.setTextSize(sp(14));
        tv.setPadding(0, dp(2), 0, dp(2));
        tv.setOnClickListener(v -> {
            try {
                Intent i = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
                i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                mContext.startActivity(i);
            } catch (Exception e) {
                Log.e(TAG, "Failed to open URL: " + url, e);
            }
        });
        return tv;
    }
}
