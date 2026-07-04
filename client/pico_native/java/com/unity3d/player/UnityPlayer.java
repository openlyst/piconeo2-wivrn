package com.unity3d.player;

import android.content.Context;
import android.view.SurfaceView;
import android.view.View;
import android.widget.FrameLayout;

public class UnityPlayer extends FrameLayout {

    public final SurfaceView surfaceView;

    public UnityPlayer(Context context) {
        super(context);
        surfaceView = new SurfaceView(context);
        addView(surfaceView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));
    }

    public View getView() {
        return surfaceView;
    }
}
