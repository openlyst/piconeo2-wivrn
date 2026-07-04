package org.meumeu.wivrn;

import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.util.Log;
import android.view.Window;
import android.view.WindowManager;

import com.picovr.cvclient.CVController;
import com.picovr.cvclient.CVControllerListener;
import com.picovr.cvclient.CVControllerManager;
import com.picovr.cvclient.ButtonNum;
import com.picovr.picovrlib.cvcontrollerclient.ControllerClient;
import com.picovr.vractivity.Eye;
import com.picovr.vractivity.HmdState;
import com.picovr.vractivity.RenderInterface;
import com.picovr.vractivity.VRActivity;
import com.psmart.vrlib.PicovrSDK;

public class MainActivity extends VRActivity implements RenderInterface {
    private static final String TAG = "WiVRn-Pico";

    static {
        System.loadLibrary("wivrn-neo2");
    }

    private static final String CTRL_UNITY_VERSION = "2.8.6.9";

    private CVControllerManager cvManager;
    private CVController leftController;
    private CVController rightController;
    private final float[] mHeadData = new float[7];

    private long nativePtr;
    private WivrnLobbyView lobbyView;
    private WifiManager.MulticastLock multicastLock;
    private int frameCount = 0;

    private CVControllerListener cvListener = new CVControllerListener() {
        @Override
        public void onBindSuccess() {
            Log.d(TAG, "Controller service bound");
        }

        @Override
        public void onBindFail() {
            Log.e(TAG, "Controller service bind failed");
        }

        @Override
        public void onThreadStart() {
            leftController = cvManager.getMainController();
            rightController = cvManager.getSubController();
            Log.d(TAG, "Controller thread started");
        }

        @Override
        public void onConnectStateChanged(int serialNum, int state) {
            Log.d(TAG, "Controller " + serialNum + " state: " + state);
        }

        @Override
        public void onMainControllerChanged(int serialNum) {
            leftController = cvManager.getMainController();
            rightController = cvManager.getSubController();
        }

        @Override
        public void onChannelChanged(int device, int channel) {
        }
    };

    @Override
    public void onCreate(Bundle savedInstanceState) {
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().setFlags(
                WindowManager.LayoutParams.FLAG_FULLSCREEN
                        | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON,
                WindowManager.LayoutParams.FLAG_FULLSCREEN
                        | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        super.onCreate(savedInstanceState);

        try {
            nativePtr = getNativePtr();
            Log.d(TAG, "nativePtr = " + nativePtr);
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "getNativePtr failed", e);
            nativePtr = 0;
        }
        setIntent(getIntent());

        cvManager = new CVControllerManager(this.getApplicationContext());
        cvManager.setListener(cvListener);

        lobbyView = new WivrnLobbyView(this);

        try {
            WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            multicastLock = wifi.createMulticastLock("wivrn-mdns");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
        } catch (Exception e) {
            Log.e(TAG, "Failed to acquire multicast lock", e);
        }

        try {
            nativeWivrnInit(nativePtr, getIntent());
            Log.d(TAG, "nativeWivrnInit called");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "nativeWivrnInit failed", e);
        }
    }

    @Override
    public void onResume() {
        super.onResume();
        cvManager.bindService();
        try { ControllerClient.setUnityVersion(CTRL_UNITY_VERSION); } catch (Throwable t) {
            Log.e(TAG, "setUnityVersion failed", t);
        }
        try { ControllerClient.startControllerThread(1, 1); } catch (Throwable t) {
            Log.e(TAG, "startControllerThread failed", t);
        }
        PicovrSDK.startSensor(0);
        PicovrSDK.setTrackingOriginType(1);
        Log.d(TAG, "onResume, nativePtr=" + nativePtr);
        try {
            nativeWivrnResume(nativePtr);
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "nativeWivrnResume failed", e);
        }
    }

    @Override
    public void onPause() {
        nativeWivrnPause(nativePtr);
        PicovrSDK.stopSensor(0);
        try { ControllerClient.stopControllerThread(1, 1); } catch (Throwable t) {}
        cvManager.unbindService();
        super.onPause();
    }

    @Override
    public void onDestroy() {
        nativeWivrnDestroy(nativePtr);
        super.onDestroy();
    }

    @Override
    public void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        nativeWivrnNewIntent(nativePtr, intent);
    }

    @Override
    public void onFrameBegin(HmdState hmdState) {
        float[] orientation = hmdState.getOrientation();
        float[] position = hmdState.getPos();
        float[] hmdData = new float[7];
        hmdData[0] = orientation[0];
        hmdData[1] = orientation[1];
        hmdData[2] = orientation[2];
        hmdData[3] = orientation[3];
        hmdData[4] = position[0];
        hmdData[5] = position[1];
        hmdData[6] = position[2];

        cvManager.updateControllerData(hmdData);

        nativeGetHeadData(nativePtr, mHeadData);

        float[] leftPose = null, rightPose = null;
        float[] leftOrient = null, rightOrient = null;
        int leftTrigger = 0, rightTrigger = 0;
        int[] leftTouch = null, rightTouch = null;
        int leftBattery = 0, rightBattery = 0;
        boolean leftA = false, leftB = false, leftGrip = false, leftClick = false, leftMenu = false;
        boolean rightA = false, rightB = false, rightGrip = false, rightClick = false, rightMenu = false;

        int leftConn = 0, rightConn = 0;
        try { leftConn = ControllerClient.getControllerConnectionState(0); } catch (Throwable t) {}
        try { rightConn = ControllerClient.getControllerConnectionState(1); } catch (Throwable t) {}

        if (leftConn == 1) {
            try {
                float[] sensor = ControllerClient.getControllerSensorState(0, mHeadData);
                if (sensor != null && sensor.length >= 7) {
                    leftOrient = new float[]{sensor[0], sensor[1], sensor[2], sensor[3]};
                    leftPose = new float[]{sensor[4], sensor[5], sensor[6]};
                    // Filter out default/fake positions from Pico SDK
                    if (Math.abs(leftPose[0] - 100.0f) < 1.0f && Math.abs(leftPose[2] + 300.0f) < 1.0f) {
                        leftOrient = null;
                        leftPose = null;
                    }
                }
            } catch (Throwable t) {}
        }
        if (rightConn == 1) {
            try {
                float[] sensor = ControllerClient.getControllerSensorState(1, mHeadData);
                if (sensor != null && sensor.length >= 7) {
                    rightOrient = new float[]{sensor[0], sensor[1], sensor[2], sensor[3]};
                    rightPose = new float[]{sensor[4], sensor[5], sensor[6]};
                    // Filter out default/fake positions from Pico SDK
                    if (Math.abs(rightPose[0] - 100.0f) < 1.0f && Math.abs(rightPose[2] + 300.0f) < 1.0f) {
                        rightOrient = null;
                        rightPose = null;
                    }
                }
            } catch (Throwable t) {}
        }

        if (leftController != null) {
            leftTrigger = leftController.getTriggerNum();
            leftTouch = leftController.getTouchPad();
            leftBattery = leftController.getBatteryLevel();
            leftA = leftController.getButtonState(ButtonNum.buttonAX);
            leftB = leftController.getButtonState(ButtonNum.buttonBY);
            leftClick = leftController.getButtonState(ButtonNum.click);
            leftMenu = leftController.getButtonState(ButtonNum.app);
        }

        if (rightController != null) {
            rightTrigger = rightController.getTriggerNum();
            rightTouch = rightController.getTouchPad();
            rightBattery = rightController.getBatteryLevel();
            rightA = rightController.getButtonState(ButtonNum.buttonAX);
            rightB = rightController.getButtonState(ButtonNum.buttonBY);
            rightClick = rightController.getButtonState(ButtonNum.click);
            rightMenu = rightController.getButtonState(ButtonNum.app);
        }

        // Grip via ControllerClient.getControllerKeyEventUnityExt (like ALVR)
        // Index 55 = right grip, 60 = left grip
        try {
            int[] leftExt = ControllerClient.getControllerKeyEventUnityExt(0);
            if (leftExt != null && leftExt.length > 60) {
                leftGrip = leftExt[60] != 0;
            }
            if (leftExt != null) {
                StringBuilder sb = new StringBuilder("LEFT ext[" + leftExt.length + "]:");
                for (int i = 0; i < leftExt.length; i++) {
                    if (leftExt[i] != 0) sb.append(" [" + i + "]=" + leftExt[i]);
                }
                Log.d(TAG, sb.toString());
            }
        } catch (Throwable t) {
            Log.e(TAG, "LEFT getControllerKeyEventUnityExt failed", t);
        }
        try {
            int[] rightExt = ControllerClient.getControllerKeyEventUnityExt(1);
            if (rightExt != null && rightExt.length > 55) {
                rightGrip = rightExt[55] != 0;
            }
            if (rightExt != null) {
                StringBuilder sb = new StringBuilder("RIGHT ext[" + rightExt.length + "]:");
                for (int i = 0; i < rightExt.length; i++) {
                    if (rightExt[i] != 0) sb.append(" [" + i + "]=" + rightExt[i]);
                }
                Log.d(TAG, sb.toString());
            }
        } catch (Throwable t) {
            Log.e(TAG, "RIGHT getControllerKeyEventUnityExt failed", t);
        }

        nativeWivrnOnFrameBegin(nativePtr, orientation, position,
                leftOrient, leftPose, leftTrigger, leftTouch, leftBattery,
                leftA, leftB, leftGrip, leftClick, leftMenu,
                rightOrient, rightPose, rightTrigger, rightTouch, rightBattery,
                rightA, rightB, rightGrip, rightClick, rightMenu);

        if (frameCount % 60 == 0 && lobbyView != null) {
            lobbyView.updateWifiStatus();
        }
        frameCount++;
    }

    @Override
    public void onDrawEye(Eye eye) {
        if (lobbyView != null && lobbyView.isDirty()) {
            lobbyView.render();
            nativeUpdateLobbyTexture(nativePtr, lobbyView.getBitmap());
            lobbyView.markClean();
        }
        nativeWivrnDrawEye(nativePtr, eye.getType());
    }

    @Override
    public void onFrameEnd() {
        nativeWivrnFrameEnd(nativePtr);
    }

    @Override
    public void onTouchEvent() {
    }

    @Override
    public void initGL(int w, int h) {
        PicovrSDK.SetEyeBufferSize(w, h);
        nativeWivrnInitGL(nativePtr, w, h);
    }

    @Override
    public void deInitGL() {
        nativeWivrnDeInitGL(nativePtr);
    }

    @Override
    public void surfaceChangedCallBack(int w, int h) {
        nativeWivrnSurfaceChanged(nativePtr, w, h);
    }

    @Override
    public void onRenderPause() {
        nativeWivrnRenderPause(nativePtr);
    }

    @Override
    public void onRenderResume() {
        nativeWivrnRenderResume(nativePtr);
    }

    @Override
    public void onRendererShutdown() {
        nativeWivrnRendererShutdown(nativePtr);
    }

    @Override
    public void renderEventCallBack(int event) {
        nativeWivrnRenderEvent(nativePtr, event);
    }

    // Native methods - WiVRn-specific (PVR SDK handles its own via VRActivity)
    public native void nativeWivrnInit(long ptr, Intent intent);
    public native void nativeGetHeadData(long ptr, float[] out);
    public native void nativeWivrnDestroy(long ptr);
    public native void nativeWivrnPause(long ptr);
    public native void nativeWivrnResume(long ptr);
    public native void nativeWivrnNewIntent(long ptr, Intent intent);
    public native void nativeWivrnOnFrameBegin(long ptr, float[] headOrient, float[] headPos,
            float[] leftOrient, float[] leftPos, int leftTrigger, int[] leftTouch, int leftBattery,
            boolean leftA, boolean leftB, boolean leftGrip, boolean leftClick, boolean leftMenu,
            float[] rightOrient, float[] rightPos, int rightTrigger, int[] rightTouch, int rightBattery,
            boolean rightA, boolean rightB, boolean rightGrip, boolean rightClick, boolean rightMenu);
    public native void nativeWivrnDrawEye(long ptr, int eye);
    public native void nativeWivrnFrameEnd(long ptr);
    public native void nativeWivrnInitGL(long ptr, int w, int h);
    public native void nativeWivrnDeInitGL(long ptr);
    public native void nativeWivrnSurfaceChanged(long ptr, int w, int h);
    public native void nativeWivrnRenderPause(long ptr);
    public native void nativeWivrnRenderResume(long ptr);
    public native void nativeWivrnRendererShutdown(long ptr);
    public native void nativeWivrnRenderEvent(long ptr, int event);
    public native void nativeWivrnSubmitPin(long ptr, String pin);
    public native void nativeUpdateLobbyTexture(long ptr, Bitmap bitmap);
    public native void nativeWivrnConnect(long ptr, String hostname, int port, boolean tcpOnly);
    public native void nativeWivrnDisconnect(long ptr);

    public void onLobbyTouch(float x, float y, boolean down, boolean pressed) {
        if (lobbyView != null) {
            lobbyView.handleTouch(x, y, down, pressed);
        }
    }

    public void onConnectionStateChanged(int state, String message) {
        if (lobbyView != null) {
            lobbyView.setConnectionState(state, message);
        }
    }

    public void requestPinEntry() {
        if (lobbyView != null) {
            lobbyView.setConnectionState(WivrnLobbyView.STATE_PIN_ENTRY, "");
        }
    }

    public void onServerConnect(String hostname, int port, boolean tcpOnly) {
        Log.d(TAG, "Connect requested: " + hostname + ":" + port + " tcp=" + tcpOnly);
        nativeWivrnConnect(nativePtr, hostname, port, tcpOnly);
    }

    public void onPinEntered(String pin) {
        Log.d(TAG, "PIN entered: " + pin);
        nativeWivrnSubmitPin(nativePtr, pin);
    }

    public void onPinCancelled() {
        Log.d(TAG, "PIN entry cancelled");
        nativeWivrnSubmitPin(nativePtr, "");
    }

    public void onDisconnectRequested() {
        Log.d(TAG, "Disconnect requested");
        nativeWivrnDisconnect(nativePtr);
    }
}
