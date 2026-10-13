package com.ridiitols.gyrolock;

import android.content.res.AssetManager;
import android.view.Surface;

import java.nio.ByteBuffer;

/** JNI bridge to the native Vulkan renderer. This class does not use EGL or OpenGL. */
final class GyroVulkanRenderer implements AutoCloseable {
    static { System.loadLibrary("gyrolock_vulkan"); }
    private long handle;

    GyroVulkanRenderer(Surface encoderSurface, int inputWidth, int inputHeight,
                       int outputWidth, int outputHeight, AssetManager assets) {
        handle = nativeCreate(encoderSurface, inputWidth, inputHeight, outputWidth, outputHeight, assets);
        if (handle == 0) throw new IllegalStateException("Renderer Vulkan gagal dibuat");
    }

    void render(ByteBuffer yPlane, ByteBuffer uPlane, ByteBuffer vPlane, long presentationTimeNs,
                float correctionX, float correctionY, float correctionZ, int rotationDegrees) {
        if (handle == 0) throw new IllegalStateException("Renderer Vulkan sudah ditutup");
        nativeRender(handle, yPlane, uPlane, vPlane, presentationTimeNs, correctionX,
                correctionY, correctionZ, rotationDegrees);
    }

    @Override public void close() {
        if (handle != 0) {
            nativeDestroy(handle);
            handle = 0;
        }
    }

    private static native long nativeCreate(Surface surface, int inputWidth, int inputHeight,
            int outputWidth, int outputHeight, AssetManager assets);
    private static native void nativeRender(long handle, ByteBuffer yPlane, ByteBuffer uPlane,
            ByteBuffer vPlane, long presentationTimeNs, float correctionX, float correctionY,
            float correctionZ, int rotationDegrees);
    private static native void nativeDestroy(long handle);
}
