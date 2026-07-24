package org.meumeu.wivrn.neo2.pvr;

import android.content.Context;
import android.graphics.Bitmap;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Wraps WivrnLobbyView (pure Canvas/Paint 2D UI) and exposes pixels
 * for upload to an OpenGL texture by the native render thread.
 *
 * Rendering happens on a dedicated background thread so the native
 * render loop never blocks waiting for Java. renderIfNeeded() just
 * returns the latest pixel buffer immediately.
 */
public class VrUiPanel {
    private static final String TAG = "wivrn";

    public static final int UI_WIDTH = 1400;
    public static final int UI_HEIGHT = 900;

    private final Context mContext;
    private final Handler mMainHandler;
    private WivrnLobbyView mLobbyView;
    private Bitmap mBitmap;

    // Double-buffered pixel storage: render thread writes to backBuffer,
    // native reads from frontBuffer. Swap is atomic.
    private final ByteBuffer mBufA = ByteBuffer.allocateDirect(UI_WIDTH * UI_HEIGHT * 4).order(ByteOrder.nativeOrder());
    private final ByteBuffer mBufB = ByteBuffer.allocateDirect(UI_WIDTH * UI_HEIGHT * 4).order(ByteOrder.nativeOrder());
    private volatile ByteBuffer mFrontBuffer = mBufA;
    private volatile ByteBuffer mBackBuffer = mBufB;
    private final int[] mPixelInts = new int[UI_WIDTH * UI_HEIGHT];
    private final Object mSwapLock = new Object();

    private final AtomicBoolean mDirty = new AtomicBoolean(true);
    private final AtomicBoolean mAttached = new AtomicBoolean(false);
    private final AtomicBoolean mRenderRunning = new AtomicBoolean(false);
    private Thread mRenderThread;

    // Native methods
    private native void nativeInit();
    public native ByteBuffer nativeGetPixels();

    public VrUiPanel(Context context) {
        mContext = context;
        mMainHandler = new Handler(Looper.getMainLooper());
    }

    public void attach() {
        if (mAttached.getAndSet(true)) return;
        mMainHandler.post(() -> {
            mLobbyView = new WivrnLobbyView(mContext);
            mBitmap = mLobbyView.getBitmap();
            markDirty();
            try { nativeInit(); } catch (Throwable t) { Log.e(TAG, "nativeInit failed", t); }
            try {
                boolean eyeSupp = ((MainActivity) mContext).nativeIsEyeSupported();
                mLobbyView.updateDebugSettings(1.0f, 1.0f, false, false, 0, eyeSupp);
            } catch (Throwable t) { /* native not up yet */ }
            startRenderThread();
            Log.i(TAG, "VrUiPanel attached " + UI_WIDTH + "x" + UI_HEIGHT);
        });
    }

    public void detach() {
        if (!mAttached.getAndSet(false)) return;
        stopRenderThread();
        mMainHandler.post(() -> {
            mLobbyView = null;
            mBitmap = null;
        });
    }

    public WivrnLobbyView getLobbyView() { return mLobbyView; }

    public void markDirty() { mDirty.set(true); }
    public boolean isDirty() { return mDirty.get(); }

    private void startRenderThread() {
        if (mRenderRunning.getAndSet(true)) return;
        mRenderThread = new Thread(() -> {
            while (mRenderRunning.get()) {
                try {
                    if (!mDirty.get() && (mLobbyView == null || !mLobbyView.isDirty())) {
                        Thread.sleep(4);
                        continue;
                    }
                    mDirty.set(false);
                    if (mLobbyView == null || mBitmap == null) continue;

                    mLobbyView.render();
                    mBitmap.getPixels(mPixelInts, 0, UI_WIDTH, 0, 0, UI_WIDTH, UI_HEIGHT);

                    ByteBuffer back = mBackBuffer;
                    back.position(0);
                    back.asIntBuffer().put(mPixelInts);
                    back.rewind();

                    synchronized (mSwapLock) {
                        mBackBuffer = mFrontBuffer;
                        mFrontBuffer = back;
                    }
                } catch (InterruptedException e) {
                    break;
                } catch (Throwable t) {
                    Log.e(TAG, "render thread error", t);
                    try { Thread.sleep(16); } catch (InterruptedException e) { break; }
                }
            }
        }, "ui-canvas-render");
        mRenderThread.start();
    }

    private void stopRenderThread() {
        mRenderRunning.set(false);
        if (mRenderThread != null) {
            mRenderThread.interrupt();
            try { mRenderThread.join(100); } catch (InterruptedException e) {}
            mRenderThread = null;
        }
    }

    /**
     * Returns the latest rendered pixel buffer for GL texture upload.
     * Never blocks - the background render thread keeps the buffer fresh.
     */
    public ByteBuffer renderIfNeeded() {
        if (!mAttached.get()) return null;
        return mFrontBuffer;
    }

    /**
     * Set touch state from controller raycast.
     * x, y are pixel coordinates in the UI texture (0..UI_WIDTH, 0..UI_HEIGHT),
     * or -1 if the pointer is off-panel.
     */
    public void setTouchState(float x, float y, boolean pressed, boolean clickEdge) {
        if (mLobbyView == null) return;
        mLobbyView.handleTouch(x, y, pressed, clickEdge, 0);
        markDirty();
    }

    // ---- Methods called from native via JNI ----

    public void setServersFromNative(String[] names, String[] hosts, int[] ports,
            boolean[] tcpOnly, boolean[] discovered, boolean[] autoconnect) {
        mMainHandler.post(() -> {
            if (mLobbyView == null) return;
            for (int i = 0; i < names.length; i++) {
                mLobbyView.addOrUpdateServer(names[i], hosts[i], ports[i], tcpOnly[i]);
            }
            markDirty();
        });
    }

    public void setConnectingFromNative(boolean connecting) {
        mMainHandler.post(() -> {
            if (mLobbyView == null) return;
            if (connecting) {
                mLobbyView.setConnectionState(WivrnLobbyView.STATE_CONNECTING, "Connecting...");
            }
            markDirty();
        });
    }

    public void setConnectionErrorFromNative(String err) {
        mMainHandler.post(() -> {
            if (mLobbyView == null) return;
            mLobbyView.setConnectionState(WivrnLobbyView.STATE_DISCONNECTED, err != null ? err : "");
            markDirty();
        });
    }

    public void setStatsFromNative(int fps, float totalLatency, float download, float upload,
            float cpuMs, float gpuMs, float encodeMs, float sendMs, float networkMs,
            float decodeMs, float renderMs, float blitMs,
            int bitrate, int streamW, int streamH, boolean micOn) {
        mMainHandler.post(() -> {
            if (mLobbyView == null) return;
            mLobbyView.updateStreamStats(fps, (int)totalLatency, (int)(download * 1000000), (int)(upload * 1000000), bitrate);
            float[] detailed = {cpuMs, gpuMs, encodeMs, sendMs, networkMs, decodeMs, renderMs, blitMs, totalLatency};
            mLobbyView.updateStreamStatsDetailed(detailed);
            markDirty();
        });
    }

    public void setRunningAppsFromNative(String[] names, int[] ids, boolean[] active) {
        mMainHandler.post(() -> {
            if (mLobbyView == null) return;
            boolean[] overlays = new boolean[names.length];
            mLobbyView.updateRunningApps(names, ids, overlays, active);
            markDirty();
        });
    }

    public void setAvailableAppsFromNative(String[] names, String[] ids) {
        mMainHandler.post(() -> {
            if (mLobbyView == null) return;
            mLobbyView.updateAvailableApps(ids, names);
            markDirty();
        });
    }

    public void setStreamingFromNative(boolean streaming) {
        mMainHandler.post(() -> {
            if (mLobbyView == null) return;
            if (streaming) {
                mLobbyView.setConnectionState(WivrnLobbyView.STATE_CONNECTED, "");
            } else {
                mLobbyView.setConnectionState(WivrnLobbyView.STATE_IDLE, "");
            }
            markDirty();
        });
    }

    public void updateSettingsFromNative(float ipd, float brightness, float fov,
            float resScale, float bitrate, boolean eyeFov, boolean mic,
            boolean passthrough, float ctrlVib, int diagHud, boolean eyeSupported) {
        mMainHandler.post(() -> {
            if (mLobbyView == null) return;
            mLobbyView.updateDebugSettings(brightness, ctrlVib, eyeFov,
                    false, diagHud, eyeSupported);
            markDirty();
        });
    }
}
