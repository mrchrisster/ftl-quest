package io.github.mrchrisster.ftlquest;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;

import java.io.File;

public class MainActivity extends Activity implements SurfaceHolder.Callback {

    static {
        System.loadLibrary("ftl_quest");
    }

    public static native void nativeInit(Surface surface, String internalPath, String externalPath);
    public static native void nativeSetActivity(Activity activity);
    public static native void nativeSurfaceDestroyed();
    public static native void nativeRenderFrame();
    public static native void nativeTouch(int pointerId, float x, float y, int phase);
    public static native void nativeGamepadButton(int buttonCode, boolean pressed);
    public static native void nativeGamepadAxis(int stickId, float axisX, float axisY);

    // Controller button code enum matching uikit_shim.h
    public static final int QUEST_BTN_A = 0;
    public static final int QUEST_BTN_B = 1;
    public static final int QUEST_BTN_X = 2;
    public static final int QUEST_BTN_Y = 3;
    public static final int QUEST_BTN_GRIP_L = 4;
    public static final int QUEST_BTN_GRIP_R = 5;
    public static final int QUEST_BTN_TRIGGER_L = 6;
    public static final int QUEST_BTN_TRIGGER_R = 7;
    public static final int QUEST_BTN_THUMB_L = 8;
    public static final int QUEST_BTN_THUMB_R = 9;
    public static final int QUEST_BTN_MENU = 10;

    private SurfaceView mSurfaceView;
    private boolean mRunning = false;
    private Thread mRenderThread;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (!SetupActivity.gameFilesPresent(this)) {
            // First run (or files removed): import the player's own FTL copy in a 2D panel.
            startActivity(new Intent(this, SetupActivity.class));
            finish();
            return;
        }
        nativeSetActivity(this); // OpenXR loader/instance need the Activity (immersive mode)
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        mSurfaceView = new SurfaceView(this);
        mSurfaceView.getHolder().addCallback(this);
        setContentView(mSurfaceView);

        // Laser pointer & Direct Touch tracking
        mSurfaceView.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View v, MotionEvent event) {
                int action = event.getActionMasked();
                int pointerIndex = event.getActionIndex();
                int pointerId = event.getPointerId(pointerIndex);
                float x = event.getX(pointerIndex);
                float y = event.getY(pointerIndex);

                int phase = 1; // Moved
                switch (action) {
                    case MotionEvent.ACTION_DOWN:
                    case MotionEvent.ACTION_POINTER_DOWN:
                        phase = 0; // Began
                        break;
                    case MotionEvent.ACTION_MOVE:
                        phase = 1; // Moved
                        break;
                    case MotionEvent.ACTION_UP:
                    case MotionEvent.ACTION_POINTER_UP:
                        phase = 3; // Ended
                        break;
                    case MotionEvent.ACTION_CANCEL:
                        phase = 4; // Cancelled
                        break;
                }

                nativeTouch(pointerId, x, y, phase);
                return true;
            }
        });

        mSurfaceView.postDelayed(new Runnable() {
            @Override
            public void run() {
                File internalDir = getFilesDir();
                File externalDir = getExternalFilesDir(null);
                String internalPath = internalDir != null ? internalDir.getAbsolutePath() : "/data/data/io.github.mrchrisster.ftlquest/files";
                String externalPath = externalDir != null ? externalDir.getAbsolutePath() : "/sdcard/Android/data/io.github.mrchrisster.ftlquest/files";
                nativeInit(null, internalPath, externalPath);
            }
        }, 300);
    }

    // ========================================================================
    // Quest 3 Physical Controller Buttons & Shortcuts
    // ========================================================================

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        int btn = mapKeyCodeToQuestButton(keyCode);
        if (btn >= 0) {
            nativeGamepadButton(btn, true);
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        int btn = mapKeyCodeToQuestButton(keyCode);
        if (btn >= 0) {
            nativeGamepadButton(btn, false);
            return true;
        }
        return super.onKeyUp(keyCode, event);
    }

    private int mapKeyCodeToQuestButton(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A:
                return QUEST_BTN_A;
            case KeyEvent.KEYCODE_BUTTON_B:
                return QUEST_BTN_B;
            case KeyEvent.KEYCODE_BUTTON_X:
                return QUEST_BTN_X;
            case KeyEvent.KEYCODE_BUTTON_Y:
                return QUEST_BTN_Y;
            case KeyEvent.KEYCODE_BUTTON_L1:
                return QUEST_BTN_GRIP_L;
            case KeyEvent.KEYCODE_BUTTON_R1:
                return QUEST_BTN_GRIP_R;
            case KeyEvent.KEYCODE_BUTTON_THUMBL:
            case KeyEvent.KEYCODE_SPACE:
                return QUEST_BTN_THUMB_L; // Spacebar / Left stick click = Pause
            case KeyEvent.KEYCODE_BUTTON_THUMBR:
                return QUEST_BTN_THUMB_R;
            case KeyEvent.KEYCODE_BUTTON_START:
            case KeyEvent.KEYCODE_MENU:
                return QUEST_BTN_MENU;
            default:
                return -1;
        }
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        if ((event.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK ||
            (event.getSource() & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD) {

            float leftX = event.getAxisValue(MotionEvent.AXIS_X);
            float leftY = event.getAxisValue(MotionEvent.AXIS_Y);
            nativeGamepadAxis(0, leftX, leftY);

            float rightX = event.getAxisValue(MotionEvent.AXIS_Z);
            float rightY = event.getAxisValue(MotionEvent.AXIS_RZ);
            nativeGamepadAxis(1, rightX, rightY);

            // Grip / Trigger analog thresholds
            float leftGrip = event.getAxisValue(MotionEvent.AXIS_BRAKE);
            float rightGrip = event.getAxisValue(MotionEvent.AXIS_GAS);
            if (leftGrip > 0.6f) nativeGamepadButton(QUEST_BTN_GRIP_L, true);
            if (rightGrip > 0.6f) nativeGamepadButton(QUEST_BTN_GRIP_R, true);

            return true;
        }
        return super.onGenericMotionEvent(event);
    }

    // ========================================================================
    // Surface Lifecycle & 90Hz Choreographed Frame Render Loop
    // ========================================================================

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        File internalDir = getFilesDir();
        File externalDir = getExternalFilesDir(null);

        String internalPath = internalDir != null ? internalDir.getAbsolutePath() : "/data/data/io.github.mrchrisster.ftlquest/files";
        String externalPath = externalDir != null ? externalDir.getAbsolutePath() : "/sdcard/Android/data/io.github.mrchrisster.ftlquest/files";

        nativeInit(holder.getSurface(), internalPath, externalPath);

        mRunning = true;
        mRenderThread = new Thread(new Runnable() {
            @Override
            public void run() {
                while (mRunning) {
                    nativeRenderFrame();
                    try {
                        Thread.sleep(11); // ~90Hz for Quest 3
                    } catch (InterruptedException ignored) {}
                }
            }
        });
        mRenderThread.start();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeSurfaceDestroyed();
        mRunning = false;
        if (mRenderThread != null) {
            try {
                mRenderThread.join();
            } catch (InterruptedException ignored) {}
        }
    }
}
