package com.ridiitols.gyrolock;

import java.nio.ByteBuffer;
import java.util.Arrays;

/**
 * Lightweight, robust global optical-flow estimator for offline stabilization.
 * Uses a tiny 80x45 luma image and sparse block matching; no OpenCV dependency.
 * The gyro remains the primary rotation estimate. Flow contributes a bounded,
 * smoothed visual-motion correction to help with residual translation/parallax.
 */
final class OpticalFlowEstimator {
    private static final int FLOW_W = 80;
    private static final int FLOW_H = 45;
    private static final int SEARCH = 3;
    private static final int PATCH = 2;
    private static final float FLOW_BLEND = 0.38f;
    private static final float PATH_TIME_CONSTANT = 0.72f;
    private static final int MAX_FEATURES = 15;

    static final class Correction {
        final float yawRadians;
        final float pitchRadians;
        Correction(float yaw, float pitch) { yawRadians = yaw; pitchRadians = pitch; }
    }

    private final byte[] previous = new byte[FLOW_W * FLOW_H];
    private final byte[] current = new byte[FLOW_W * FLOW_H];
    private final int[] dxValues = new int[MAX_FEATURES];
    private final int[] dyValues = new int[MAX_FEATURES];
    private boolean initialized;
    private long previousPtsUs = -1;
    private float pathYaw;
    private float pathPitch;
    private float smoothYaw;
    private float smoothPitch;

    Correction update(ByteBuffer luma, int width, int height, long ptsUs) {
        downsample(luma, width, height, current);
        if (!initialized) {
            System.arraycopy(current, 0, previous, 0, current.length);
            initialized = true;
            previousPtsUs = ptsUs;
            return new Correction(0f, 0f);
        }

        int dtX = 0, dtY = 0;
        int count = estimateTranslation(previous, current);
        if (count >= 4) {
            int dx = median(dxValues, count);
            int dy = median(dyValues, count);
            // Reject impossible one-frame jumps and low-confidence outliers.
            dx = Math.max(-SEARCH, Math.min(SEARCH, dx));
            dy = Math.max(-SEARCH, Math.min(SEARCH, dy));
            dtX = dx;
            dtY = dy;
        }

        long deltaUs = previousPtsUs < 0 ? 33333L : Math.max(1L, Math.min(100000L, ptsUs - previousPtsUs));
        float dt = deltaUs / 1_000_000f;
        previousPtsUs = ptsUs;
        System.arraycopy(current, 0, previous, 0, current.length);

        // Approximate normalized focal length in the reduced image. A camera-motion
        // path is integrated, then separated from a slow reference path, as in EIS.
        float focal = FLOW_W * 0.90f;
        pathYaw += -dtX / focal;
        pathPitch += dtY / focal;
        pathYaw = clamp(pathYaw, -0.75f, 0.75f);
        pathPitch = clamp(pathPitch, -0.75f, 0.75f);
        float alpha = dt / (PATH_TIME_CONSTANT + dt);
        smoothYaw += alpha * (pathYaw - smoothYaw);
        smoothPitch += alpha * (pathPitch - smoothPitch);

        float yaw = clamp((pathYaw - smoothYaw) * FLOW_BLEND, -0.075f, 0.075f);
        float pitch = clamp((pathPitch - smoothPitch) * FLOW_BLEND, -0.075f, 0.075f);
        return new Correction(yaw, pitch);
    }

    private int estimateTranslation(byte[] prev, byte[] cur) {
        int count = 0;
        // Five columns x three rows. Sparse features keep CPU cost small at FHD.
        int[] xs = {10, 25, 40, 55, 70};
        int[] ys = {8, 22, 36};
        for (int cy : ys) {
            for (int cx : xs) {
                if (!hasTexture(prev, cx, cy)) continue;
                int bestDx = 0, bestDy = 0;
                long best = Long.MAX_VALUE;
                long second = Long.MAX_VALUE;
                for (int dy = -SEARCH; dy <= SEARCH; dy++) {
                    for (int dx = -SEARCH; dx <= SEARCH; dx++) {
                        long sad = 0;
                        for (int py = -PATCH; py <= PATCH; py++) {
                            int pBase = (cy + py) * FLOW_W + cx;
                            int cBase = (cy + dy + py) * FLOW_W + cx + dx;
                            for (int px = -PATCH; px <= PATCH; px++) {
                                int a = prev[pBase + px] & 255;
                                int b = cur[cBase + px] & 255;
                                sad += Math.abs(a - b);
                            }
                        }
                        if (sad < best) {
                            second = best; best = sad; bestDx = dx; bestDy = dy;
                        } else if (sad < second) second = sad;
                    }
                }
                // Reject ambiguous matches where multiple offsets look nearly identical.
                if (second != Long.MAX_VALUE && best * 100L < second * 97L) {
                    dxValues[count] = bestDx;
                    dyValues[count] = bestDy;
                    count++;
                    if (count == MAX_FEATURES) return count;
                }
            }
        }
        return count;
    }

    private boolean hasTexture(byte[] image, int x, int y) {
        int center = image[y * FLOW_W + x] & 255;
        int horizontal = Math.abs(center - (image[y * FLOW_W + x - 2] & 255));
        int vertical = Math.abs(center - (image[(y - 2) * FLOW_W + x] & 255));
        int diagonal = Math.abs(center - (image[(y + 2) * FLOW_W + x + 2] & 255));
        return horizontal + vertical + diagonal >= 24;
    }

    private static void downsample(ByteBuffer src, int width, int height, byte[] out) {
        // ByteBuffer is direct and positioned at zero by packYuvPlanes.
        int sx = Math.max(1, width / FLOW_W);
        int sy = Math.max(1, height / FLOW_H);
        for (int y = 0; y < FLOW_H; y++) {
            int sourceY = Math.min(height - 1, y * height / FLOW_H + sy / 2);
            int sourceRow = sourceY * width;
            int outRow = y * FLOW_W;
            for (int x = 0; x < FLOW_W; x++) {
                int sourceX = Math.min(width - 1, x * width / FLOW_W + sx / 2);
                out[outRow + x] = src.get(sourceRow + sourceX);
            }
        }
    }

    private static int median(int[] values, int length) {
        int[] copy = Arrays.copyOf(values, length);
        Arrays.sort(copy);
        return copy[length / 2];
    }

    private static float clamp(float value, float low, float high) {
        return Math.max(low, Math.min(high, value));
    }
}
