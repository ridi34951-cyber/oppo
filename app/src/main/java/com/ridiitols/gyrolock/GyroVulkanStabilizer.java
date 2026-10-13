package com.ridiitols.gyrolock;

import android.content.Context;
import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.media.MediaMetadataRetriever;
import android.media.MediaMuxer;
import android.view.Surface;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.concurrent.TimeUnit;

/**
 * Experimental Vulkan video stabilizer. Video is decoded to YUV images by MediaCodec,
 * converted to RGBA, rendered by native Vulkan to the MediaCodec encoder input surface,
 * then original audio is remuxed without re-encoding.
 */
final class GyroVulkanStabilizer {
    interface Progress { void onProgress(String message); }
    private static final String MIME_AVC = "video/avc";
    private static final long CODEC_TIMEOUT_US = 10_000;
    private GyroVulkanStabilizer() {}

    static void stabilize(Context context, File input, File silentOutput, File finalOutput,
                          float[] times, float[] corrX, float[] corrY, float[] corrZ,
                          Progress progress) throws Exception {
        if (times == null || corrX == null || corrY == null || corrZ == null || times.length < 2
                || corrX.length != times.length || corrY.length != times.length || corrZ.length != times.length) {
            throw new IOException("Lintasan gyro 240 titik tidak valid");
        }
        if (progress != null) progress.onProgress("Vulkan: menyiapkan decoder, GPU, dan encoder...");
        int rotation = readRotation(input);
        transcodeWithVulkan(context, input, silentOutput, rotation, times, corrX, corrY, corrZ, progress);
        if (progress != null) progress.onProgress("Audio: menggabungkan audio asli...");
        remuxOriginalAudio(input, silentOutput, finalOutput, 0);
        if (!finalOutput.exists() || finalOutput.length() < 1024) throw new IOException("File hasil stabilisasi kosong atau terlalu kecil");
        //noinspection ResultOfMethodCallIgnored
        silentOutput.delete();
    }

    private static int readRotation(File input) {
        MediaMetadataRetriever retriever = new MediaMetadataRetriever();
        try {
            retriever.setDataSource(input.getAbsolutePath());
            String value = retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION);
            return value == null ? 0 : normalizeRotation(Integer.parseInt(value));
        } catch (Exception ignored) { return 0; }
        finally { try { retriever.release(); } catch (Exception ignored) {} }
    }

    private static void transcodeWithVulkan(Context context, File input, File output, int metadataRotation,
            float[] times, float[] corrX, float[] corrY, float[] corrZ, Progress progress) throws Exception {
        MediaExtractor extractor = new MediaExtractor();
        MediaCodec decoder = null, encoder = null;
        Surface encoderSurface = null;
        GyroVulkanRenderer renderer = null;
        MediaMuxer muxer = null;
        boolean muxerStarted = false;
        int muxerVideoTrack = -1;
        try {
            extractor.setDataSource(input.getAbsolutePath());
            int track = findTrack(extractor, "video/");
            if (track < 0) throw new IOException("Video tidak ditemukan di file asli");
            extractor.selectTrack(track);
            MediaFormat inFormat = extractor.getTrackFormat(track);
            String mime = inFormat.getString(MediaFormat.KEY_MIME);
            int sourceWidth = inFormat.getInteger(MediaFormat.KEY_WIDTH);
            int sourceHeight = inFormat.getInteger(MediaFormat.KEY_HEIGHT);
            sourceWidth &= ~1; sourceHeight &= ~1;
            if (sourceWidth < 2 || sourceHeight < 2) throw new IOException("Resolusi video tidak valid");
            int rotation = normalizeRotation(metadataRotation);
            try {
                if (inFormat.containsKey("rotation-degrees")) {
                    int trackRotation = normalizeRotation(inFormat.getInteger("rotation-degrees"));
                    if (trackRotation != 0) rotation = trackRotation;
                }
            } catch (RuntimeException ignored) {}
            int outWidth = sourceWidth, outHeight = sourceHeight;
            if (rotation == 90 || rotation == 270) { outWidth = sourceHeight; outHeight = sourceWidth; }

            MediaFormat encFormat = MediaFormat.createVideoFormat(MIME_AVC, outWidth, outHeight);
            encFormat.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
            long pixels = (long) outWidth * outHeight;
            int bitrate = (int) Math.max(3_000_000L, Math.min(18_000_000L, pixels * 4L));
            encFormat.setInteger(MediaFormat.KEY_BIT_RATE, bitrate);
            encFormat.setInteger(MediaFormat.KEY_FRAME_RATE, 30);
            encFormat.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2);
            encoder = MediaCodec.createEncoderByType(MIME_AVC);
            encoder.configure(encFormat, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            encoderSurface = encoder.createInputSurface();
            renderer = new GyroVulkanRenderer(encoderSurface, sourceWidth, sourceHeight,
                    outWidth, outHeight, context.getAssets());
            encoder.start();

            // Request a CPU-readable flexible YUV output. No SurfaceTexture/EGL/OpenGL path is used.
            inFormat.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
            decoder = MediaCodec.createDecoderByType(mime);
            decoder.configure(inFormat, null, null, 0);
            decoder.start();

            muxer = new MediaMuxer(output.getAbsolutePath(), MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            boolean inputDone = false, decoderDone = false, encoderDone = false;
            long frameCount = 0, encodedFrameCount = 0, lastProgressMs = System.currentTimeMillis();
            ByteBuffer yPlane = ByteBuffer.allocateDirect(sourceWidth * sourceHeight);
            ByteBuffer uPlane = ByteBuffer.allocateDirect((sourceWidth / 2) * (sourceHeight / 2));
            ByteBuffer vPlane = ByteBuffer.allocateDirect((sourceWidth / 2) * (sourceHeight / 2));
            // Gyro is the single authoritative motion source in this revision.
            // The previous sparse-flow estimator integrated image displacement and then
            // added it to the gyro correction. Because both often measured the same
            // camera rotation, that fusion double-corrected motion and caused jelly/bounce.
            // Keep the estimator out of the render path until it can be fused as a
            // confidence-weighted pose measurement rather than an extra correction.
            while (!encoderDone) {
                if (!inputDone) {
                    int inputIndex = decoder.dequeueInputBuffer(CODEC_TIMEOUT_US);
                    if (inputIndex >= 0) {
                        ByteBuffer inputBuffer = decoder.getInputBuffer(inputIndex);
                        if (inputBuffer == null) throw new IOException("Buffer input decoder tidak tersedia");
                        inputBuffer.clear();
                        int size = extractor.readSampleData(inputBuffer, 0);
                        if (size < 0) {
                            decoder.queueInputBuffer(inputIndex, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inputDone = true;
                        } else {
                            long pts = Math.max(0, extractor.getSampleTime());
                            decoder.queueInputBuffer(inputIndex, 0, size, pts, 0);
                            extractor.advance();
                        }
                    }
                }
                int outIndex = decoder.dequeueOutputBuffer(info, CODEC_TIMEOUT_US);
                if (outIndex >= 0) {
                    boolean eos = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                    if (info.size > 0) {
                        long ptsUs = Math.max(0L, info.presentationTimeUs);
                        Image image = decoder.getOutputImage(outIndex);
                        if (image == null) {
                            decoder.releaseOutputBuffer(outIndex, false);
                            throw new IOException("Decoder tidak menyediakan frame YUV yang dapat dibaca. Perangkat/codec mungkin tidak mendukung mode Vulkan eksperimental ini.");
                        }
                        try { packYuvPlanes(image, yPlane, uPlane, vPlane, sourceWidth, sourceHeight); }
                        finally { image.close(); }
                        decoder.releaseOutputBuffer(outIndex, false);
                        float seconds = ptsUs / 1_000_000f;
                        float x = interpolate(times, corrX, seconds);
                        float y = interpolate(times, corrY, seconds);
                        float z = interpolate(times, corrZ, seconds);
                        // Apply the offline gyro correction once. Do not add a second
                        // image-motion correction here: the old additive flow path could
                        // oppose the gyro transform and make the scene appear elastic.
                        yPlane.position(0); uPlane.position(0); vPlane.position(0);
                        try { renderer.render(yPlane, uPlane, vPlane, ptsUs * 1000L, x, y, z, rotation); }
                        catch (RuntimeException e) { throw new IOException("Renderer Vulkan gagal: " + e.getMessage(), e); }
                        frameCount++;
                    } else decoder.releaseOutputBuffer(outIndex, false);
                    if (eos) { decoderDone = true; encoder.signalEndOfInputStream(); }
                }
                while (true) {
                    int encIndex = encoder.dequeueOutputBuffer(info, CODEC_TIMEOUT_US);
                    if (encIndex == MediaCodec.INFO_TRY_AGAIN_LATER) break;
                    if (encIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                        if (muxerStarted) throw new IOException("Format encoder berubah setelah muxer dimulai");
                        muxerVideoTrack = muxer.addTrack(encoder.getOutputFormat()); muxer.start(); muxerStarted = true;
                    } else if (encIndex >= 0) {
                        ByteBuffer encoded = encoder.getOutputBuffer(encIndex);
                        if (encoded == null) throw new IOException("Buffer output encoder tidak tersedia");
                        if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) info.size = 0;
                        if (info.size > 0) {
                            if (!muxerStarted) throw new IOException("Encoder belum mengirim format output");
                            // Surface-input encoders on Vulkan/ANativeWindow may stamp frames using
                            // wall-clock presentation time. That made a short clip become much longer
                            // when rendering took longer than real time. Normalize encoded video PTS
                            // to a fixed 30 FPS timeline before muxing.
                            info.presentationTimeUs = encodedFrameCount * 1_000_000L / 30L;
                            encodedFrameCount++;
                            encoded.position(info.offset); encoded.limit(info.offset + info.size);
                            muxer.writeSampleData(muxerVideoTrack, encoded, info);
                        }
                        boolean encEos = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                        encoder.releaseOutputBuffer(encIndex, false);
                        if (encEos) { encoderDone = true; break; }
                    }
                }
                long now = System.currentTimeMillis();
                if (progress != null && now - lastProgressMs > 1200) {
                    progress.onProgress("Vulkan: memproses frame " + frameCount + "..."); lastProgressMs = now;
                }
                if (decoderDone && !encoderDone) { /* Drain encoder until EOS. */ }
            }
            if (!muxerStarted || frameCount == 0 || encodedFrameCount == 0) throw new IOException("Vulkan tidak menghasilkan frame video");
        } finally {
            try { if (decoder != null) decoder.stop(); } catch (Exception ignored) {}
            try { if (decoder != null) decoder.release(); } catch (Exception ignored) {}
            if (renderer != null) try { renderer.close(); } catch (Exception ignored) {}
            try { if (encoder != null) encoder.stop(); } catch (Exception ignored) {}
            try { if (encoder != null) encoder.release(); } catch (Exception ignored) {}
            try { if (encoderSurface != null) encoderSurface.release(); } catch (Exception ignored) {}
            try { if (muxer != null) muxer.stop(); } catch (Exception ignored) {}
            try { if (muxer != null) muxer.release(); } catch (Exception ignored) {}
            try { extractor.release(); } catch (Exception ignored) {}
        }
    }

    private static void packYuvPlanes(Image image, ByteBuffer yOut, ByteBuffer uOut, ByteBuffer vOut,
                                      int width, int height) throws IOException {
        if (image.getFormat() != ImageFormat.YUV_420_888) throw new IOException("Format decoder bukan YUV_420_888");
        Image.Plane[] planes = image.getPlanes();
        if (planes == null || planes.length < 3) throw new IOException("Plane YUV tidak lengkap");
        Rect crop = image.getCropRect();
        if (crop.width() < width || crop.height() < height) throw new IOException("Crop decoder lebih kecil dari ukuran frame yang diminta");
        ByteBuffer yBuf = planes[0].getBuffer(), uBuf = planes[1].getBuffer(), vBuf = planes[2].getBuffer();
        int yRow = planes[0].getRowStride(), yPix = planes[0].getPixelStride();
        int uRow = planes[1].getRowStride(), uPix = planes[1].getPixelStride();
        int vRow = planes[2].getRowStride(), vPix = planes[2].getPixelStride();
        int left = Math.max(0, crop.left), top = Math.max(0, crop.top);
        yOut.clear(); uOut.clear(); vOut.clear();
        for (int row = 0; row < height; row++) {
            int yy = row + top;
            for (int col = 0; col < width; col++) yOut.put(yBuf.get(yy * yRow + (col + left) * yPix));
        }
        int chromaWidth = width / 2, chromaHeight = height / 2;
        for (int row = 0; row < chromaHeight; row++) {
            int yy = (row + top / 2);
            for (int col = 0; col < chromaWidth; col++) {
                int xx = col + left / 2;
                uOut.put(uBuf.get(yy * uRow + xx * uPix));
                vOut.put(vBuf.get(yy * vRow + xx * vPix));
            }
        }
        yOut.flip(); uOut.flip(); vOut.flip();
    }
    private static int normalizeRotation(int value) { int r=((value%360)+360)%360;return (r==0||r==90||r==180||r==270)?r:0; }
    private static int findTrack(MediaExtractor extractor,String prefix) {
        for(int i=0;i<extractor.getTrackCount();i++){String mime=extractor.getTrackFormat(i).getString(MediaFormat.KEY_MIME);if(mime!=null&&mime.startsWith(prefix))return i;}return -1;
    }
    private static float clamp(float value, float low, float high) {
        return Math.max(low, Math.min(high, value));
    }
    private static float interpolate(float[] times,float[] values,float time) {
        if(time<=times[0])return values[0];int last=times.length-1;if(time>=times[last])return values[last];int lo=0,hi=last;
        while(hi-lo>1){int mid=(lo+hi)>>>1;if(times[mid]<=time)lo=mid;else hi=mid;}float span=times[hi]-times[lo];if(span<=0f)return values[lo];float f=(time-times[lo])/span;return values[lo]+(values[hi]-values[lo])*f;
    }
    private static void remuxOriginalAudio(File original,File silentVideo,File output,int rotation)throws IOException {
        MediaExtractor videoExtractor=new MediaExtractor(),audioExtractor=new MediaExtractor();MediaMuxer muxer=null;boolean started=false;
        try {
            videoExtractor.setDataSource(silentVideo.getAbsolutePath());int vTrack=findTrack(videoExtractor,"video/");if(vTrack<0)throw new IOException("Video hasil Vulkan tidak ditemukan");videoExtractor.selectTrack(vTrack);MediaFormat vFormat=videoExtractor.getTrackFormat(vTrack);
            audioExtractor.setDataSource(original.getAbsolutePath());int aTrack=findTrack(audioExtractor,"audio/");MediaFormat aFormat=null;if(aTrack>=0){audioExtractor.selectTrack(aTrack);aFormat=audioExtractor.getTrackFormat(aTrack);}
            muxer=new MediaMuxer(output.getAbsolutePath(),MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);if(rotation==90||rotation==180||rotation==270)muxer.setOrientationHint(rotation);int outVideo=muxer.addTrack(vFormat);int outAudio=aFormat==null?-1:muxer.addTrack(aFormat);muxer.start();started=true;
            ByteBuffer buffer=ByteBuffer.allocateDirect(4*1024*1024);MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();boolean videoDone=false,audioDone=aTrack<0;
            while(!videoDone||!audioDone){long vt=videoDone?Long.MAX_VALUE:videoExtractor.getSampleTime(),at=audioDone?Long.MAX_VALUE:audioExtractor.getSampleTime();if(vt<0){videoDone=true;vt=Long.MAX_VALUE;}if(at<0){audioDone=true;at=Long.MAX_VALUE;}if(videoDone&&audioDone)break;boolean takeVideo=vt<=at;MediaExtractor chosen=takeVideo?videoExtractor:audioExtractor;int dest=takeVideo?outVideo:outAudio;buffer.clear();int size=chosen.readSampleData(buffer,0);if(size<0){if(takeVideo)videoDone=true;else audioDone=true;continue;}info.offset=0;info.size=size;info.presentationTimeUs=Math.max(0,chosen.getSampleTime());info.flags=chosen.getSampleFlags();muxer.writeSampleData(dest,buffer,info);chosen.advance();}
        } catch(RuntimeException e){throw new IOException("Gagal menggabungkan audio/video: "+e.getMessage(),e);} finally {try{if(started&&muxer!=null)muxer.stop();}catch(Exception ignored){}try{if(muxer!=null)muxer.release();}catch(Exception ignored){}try{videoExtractor.release();}catch(Exception ignored){}try{audioExtractor.release();}catch(Exception ignored){}}
    }
}
