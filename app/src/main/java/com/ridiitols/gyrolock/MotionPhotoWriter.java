package com.ridiitols.gyrolock;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/** Writes a standards-oriented Android Motion Photo: JPEG with XMP + appended MP4. */
final class MotionPhotoWriter {
    private static final String XMP_HEADER = "http://ns.adobe.com/xap/1.0/\0";
    private MotionPhotoWriter() {}

    static void write(File jpeg, File mp4, File output, long presentationTimestampUs) throws IOException {
        if (!jpeg.isFile() || jpeg.length() < 4) throw new IOException("Foto JPEG tidak valid");
        if (!mp4.isFile() || mp4.length() < 1024) throw new IOException("Video Motion Photo tidak valid");
        long videoLength = mp4.length();
        byte[] xmpBytes = (XMP_HEADER + buildXmp(videoLength, presentationTimestampUs)).getBytes(StandardCharsets.UTF_8);
        int segmentLength = xmpBytes.length + 2;
        if (segmentLength > 65535) throw new IOException("Metadata XMP terlalu besar");

        File parent = output.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) throw new IOException("Tidak dapat membuat folder output");
        try (FileInputStream in = new FileInputStream(jpeg); FileOutputStream out = new FileOutputStream(output)) {
            int a = in.read(), b = in.read();
            if (a != 0xFF || b != 0xD8) throw new IOException("Header JPEG tidak valid");
            out.write(a); out.write(b);
            out.write(0xFF); out.write(0xE1);
            out.write((segmentLength >> 8) & 0xFF); out.write(segmentLength & 0xFF);
            out.write(xmpBytes);
            byte[] buffer = new byte[64 * 1024]; int n;
            while ((n = in.read(buffer)) != -1) out.write(buffer, 0, n);
            try (FileInputStream videoIn = new FileInputStream(mp4)) {
                while ((n = videoIn.read(buffer)) != -1) out.write(buffer, 0, n);
            }
            out.flush();
        }
    }

    private static String buildXmp(long videoLength, long presentationTimestampUs) {
        long marker = Math.max(0L, presentationTimestampUs);
        // Keep both the current Motion Photo 1.0 container directory and the legacy
        // MicroVideo attributes used by older Google/OEM readers. The legacy timestamp
        // is important for readers that draw the still-frame marker on the motion strip.
        return "<x:xmpmeta xmlns:x='adobe:ns:meta/'>" +
                "<rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>" +
                "<rdf:Description rdf:about='' " +
                "xmlns:Camera='http://ns.google.com/photos/1.0/camera/' " +
                "xmlns:GCamera='http://ns.google.com/photos/1.0/camera/' " +
                "xmlns:GContainer='http://ns.google.com/photos/1.0/container/' " +
                "xmlns:Item='http://ns.google.com/photos/1.0/container/item/' " +
                "Camera:MotionPhoto='1' Camera:MotionPhotoVersion='1' " +
                "Camera:MotionPhotoPresentationTimestampUs='" + marker + "' " +
                "GCamera:MotionPhoto='1' GCamera:MotionPhotoVersion='1' " +
                "GCamera:MicroVideo='1' GCamera:MicroVideoVersion='1' " +
                "GCamera:MicroVideoOffset='" + videoLength + "' " +
                "GCamera:MicroVideoPresentationTimestampUs='" + marker + "'>" +
                "<GContainer:Directory><rdf:Seq>" +
                // Format 1.0 defines the primary item's length as zero; readers use the
                // appended video's exact length to locate the MotionPhoto item from EOF.
                "<rdf:li rdf:parseType='Resource'><Item:Mime>image/jpeg</Item:Mime><Item:Semantic>Primary</Item:Semantic><Item:Length>0</Item:Length><Item:Padding>0</Item:Padding></rdf:li>" +
                "<rdf:li rdf:parseType='Resource'><Item:Mime>video/mp4</Item:Mime><Item:Semantic>MotionPhoto</Item:Semantic><Item:Length>" + videoLength + "</Item:Length></rdf:li>" +
                "</rdf:Seq></GContainer:Directory>" +
                "</rdf:Description></rdf:RDF></x:xmpmeta>";
    }
}
