package org.meumeu.wivrn.neo2.pvr;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Rect;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import java.nio.ByteBuffer;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Renders an Android View hierarchy to a Bitmap and exposes the pixels
 * for upload to an OpenGL texture by the native render thread.
 *
 * The native side creates a GL texture, calls nativeGetUiPixels each frame,
 * and uploads the returned ByteBuffer with glTexSubImage2D.
 */
public class VrUiPanel {
    private static final String TAG = "wivrn";

    public static final int UI_WIDTH = 1400;
    public static final int UI_HEIGHT = 1000;

    private final Context mContext;
    private final Handler mMainHandler;
    private FrameLayout mRoot;
    private Bitmap mBitmap;
    private Canvas mCanvas;
    private ByteBuffer mPixelBuffer;
    private final AtomicBoolean mDirty = new AtomicBoolean(true);
    private final AtomicBoolean mAttached = new AtomicBoolean(false);

    // Touch state from controller
    private volatile float mTouchX = -1, mTouchY = -1;
    private volatile boolean mTouchPressed = false;
    private volatile boolean mTouchClickEdge = false;
    private long mLastEventTime = 0;

    public VrUiPanel(Context context) {
        mContext = context;
        mMainHandler = new Handler(Looper.getMainLooper());
    }

    /**
     * Inflate the root layout and attach it. Must be called on the main thread.
     */
    public void attach() {
        if (mAttached.getAndSet(true)) return;
        mMainHandler.post(() -> {
            mRoot = new FrameLayout(mContext);
            mRoot.setLayoutParams(new ViewGroup.LayoutParams(UI_WIDTH, UI_HEIGHT));
            mRoot.setBackgroundColor(Color.parseColor("#1e1e2e"));

            mBitmap = Bitmap.createBitmap(UI_WIDTH, UI_HEIGHT, Bitmap.Config.ARGB_8888);
            mCanvas = new Canvas(mBitmap);
            mPixelBuffer = ByteBuffer.allocateDirect(UI_WIDTH * UI_HEIGHT * 4);

            measureAndLayout();
            markDirty();
            Log.i(TAG, "VrUiPanel attached " + UI_WIDTH + "x" + UI_HEIGHT);
        });
    }

    public void detach() {
        if (!mAttached.getAndSet(false)) return;
        mMainHandler.post(() -> {
            if (mRoot != null) {
                mRoot.removeAllViews();
                mRoot = null;
            }
            if (mBitmap != null) {
                mBitmap.recycle();
                mBitmap = null;
            }
            mCanvas = null;
            mPixelBuffer = null;
        });
    }

    /**
     * Set the content view for the panel. Must be called on the main thread.
     */
    public void setContentView(View view) {
        mMainHandler.post(() -> {
            if (mRoot == null) return;
            mRoot.removeAllViews();
            mRoot.addView(view);
            measureAndLayout();
            markDirty();
        });
    }

    public FrameLayout getRoot() {
        return mRoot;
    }

    public Context getContext() {
        return mContext;
    }

    public void markDirty() {
        mDirty.set(true);
    }

    public boolean isDirty() {
        return mDirty.get();
    }

    /**
     * Render the View hierarchy to the bitmap if dirty.
     * Returns the pixel buffer for GL texture upload.
     * Called from the native render thread.
     */
    public ByteBuffer renderIfNeeded() {
        if (!mAttached.get() || mBitmap == null || mPixelBuffer == null) return null;
        if (!mDirty.getAndSet(false)) return mPixelBuffer;

        mMainHandler.post(() -> {
            if (mRoot == null || mCanvas == null || mBitmap == null) return;
            mCanvas.drawColor(Color.parseColor("#1e1e2e"));
            mRoot.draw(mCanvas);

            mBitmap.copyPixelsToBuffer(mPixelBuffer);
            mPixelBuffer.rewind();
        });

        return mPixelBuffer;
    }

    private void measureAndLayout() {
        if (mRoot == null) return;
        int specW = View.MeasureSpec.makeMeasureSpec(UI_WIDTH, View.MeasureSpec.EXACTLY);
        int specH = View.MeasureSpec.makeMeasureSpec(UI_HEIGHT, View.MeasureSpec.EXACTLY);
        mRoot.measure(specW, specH);
        mRoot.layout(0, 0, UI_WIDTH, UI_HEIGHT);
    }

    /**
     * Set touch state from controller raycast.
     * @param x pixel x in UI coordinates, or -1 if off-panel
     * @param y pixel y in UI coordinates, or -1 if off-panel
     * @param pressed whether the trigger/button is held
     * @param clickEdge true on the frame the button was released (click)
     */
    public void setTouchState(float x, float y, boolean pressed, boolean clickEdge) {
        mTouchX = x;
        mTouchY = y;
        mTouchPressed = pressed;
        mTouchClickEdge = clickEdge;
    }

    /**
     * Dispatch a touch event to the View hierarchy based on controller state.
     * Called from the native render thread via JNI.
     */
    public void dispatchTouch() {
        if (!mAttached.get() || mRoot == null) return;
        if (mTouchX < 0 || mTouchY < 0) return;

        mMainHandler.post(() -> {
            if (mRoot == null) return;
            long now = System.currentTimeMillis();
            if (mLastEventTime == 0) mLastEventTime = now;
            int action = mTouchPressed ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_UP;
            if (mTouchClickEdge) action = MotionEvent.ACTION_UP;

            MotionEvent ev = MotionEvent.obtain(
                mLastEventTime, now, action, mTouchX, mTouchY, 1, 1,
                0, 1f, 1f, 0, 0);
            mRoot.dispatchTouchEvent(ev);
            ev.recycle();
            mLastEventTime = now;
            markDirty();
        });
    }
}
