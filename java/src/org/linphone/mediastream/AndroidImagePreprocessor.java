package org.linphone.mediastream;

public interface AndroidImagePreprocessor {
    public byte[] preprocessCameraFrame(byte[] imageData);
}