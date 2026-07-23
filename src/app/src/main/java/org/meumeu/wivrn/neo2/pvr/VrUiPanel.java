package org.meumeu.wivrn.neo2.pvr;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.nio.ByteBuffer;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Wraps WivrnLobbyView (pure Canvas/Paint 2D UI) and exposes pixels
 * for upload to an OpenGL texture by the native render thread.
 */
public class VrUiPanel {
    private static final String TAG = "wivrn";

    public static final int UI_WIDTH = 1400;
    public static final int UI_HEIGHT = 900;

    private final Context mContext;
    private final Handler mMainHandler;
    private WivrnLobbyView mLobbyView;
    private Bitmap mBitmap;
    private ByteBuffer mPixelBuffer;
    private int[] mPixelInts;
    private final AtomicBoolean mDirty = new AtomicBoolean(true);
    private final AtomicBoolean mAttached = new AtomicBoolean(false);

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
            mPixelBuffer = ByteBuffer.allocateDirect(UI_WIDTH * UI_HEIGHT * 4);
            mPixelInts = new int[UI_WIDTH * UI_HEIGHT];
            markDirty();
            try { nativeInit(); } catch (Throwable t) { Log.e(TAG, "nativeInit failed", t); }
            Log.i(TAG, "VrUiPanel attached " + UI_WIDTH + "x" + UI_HEIGHT);
        });
    }

    public void detach() {
        if (!mAttached.getAndSet(false)) return;
        mMainHandler.post(() -> {
            mLobbyView = null;
            mBitmap = null;
            mPixelBuffer = null;
            mPixelInts = null;
        });
    }

    public WivrnLobbyView getLobbyView() { return mLobbyView; }

    public void markDirty() { mDirty.set(true); }
    public boolean isDirty() { return mDirty.get(); }

    /**
     * Render the WivrnLobbyView to the bitmap if dirty, return pixels for GL upload.
     * Called from the native render thread.
     */
    public ByteBuffer renderIfNeeded() {
        if (!mAttached.get() || mBitmap == null || mPixelBuffer == null) return null;
        if (!mDirty.getAndSet(false)) {
            // Still check if lobbyView is dirty (internal state changes)
            if (mLobbyView == null || !mLobbyView.isDirty()) return mPixelBuffer;
        }

        CountDownLatch latch = new CountDownLatch(1);
        mMainHandler.post(() -> {
            try {
                if (mLobbyView == null || mBitmap == null || mPixelBuffer == null) return;
                mLobbyView.render();
                mBitmap.getPixels(mPixelInts, 0, UI_WIDTH, 0, 0, UI_WIDTH, UI_HEIGHT);
                mPixelBuffer.position(0);
                mPixelBuffer.asIntBuffer().put(mPixelInts);
                mPixelBuffer.rewind();
            } catch (Throwable t) {
                Log.e(TAG, "renderIfNeeded failed", t);
            } finally {
                latch.countDown();
            }
        });
        try {
            if (!latch.await(50, TimeUnit.MILLISECONDS))
                Log.w(TAG, "renderIfNeeded timed out");
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        return mPixelBuffer;
    }

    /**
     * Set touch state from controller raycast.
     */
    public void setTouchState(float x, float y, boolean pressed, boolean clickEdge) {
        if (mLobbyView == null) return;
        // WivrnLobbyView.handleTouch uses normalized 0..1 coordinates
        float nx = (x < 0) ? -1 : x / UI_WIDTH;
        float ny = (y < 0) ? -1 : y / UI_HEIGHT;
        mLobbyView.handleTouch(nx, ny, pressed, clickEdge, 0);
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
            // WivrnLobbyView manages its own settings; this is a no-op for now
            markDirty();
        });
    }
}
