// 文件作用：Android IM 身份、消息同步与本地历史；RemoteIMMediaStore 负责持久化读写与缓存管理，本文件给出其实现。
// 主要入口：copyPickedImage、createVoiceRecordingFile、createRealtimeVoiceRecordingFile、createCameraPhotoFile。
package com.kongshang.maichat;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Locale;
import java.util.function.LongSupplier;

/**
 * 本端发出的媒体的落盘处：选图、录音、拍照、选文件。
 *
 * 这些文件的路径会进聊天记录并被长期引用，所以一律落在 RemoteIMMediaPaths 的
 * 持久根下，不再放缓存目录——放缓存的话系统清一次，历史里的图就永远打不开了。
 */
public final class RemoteIMMediaStore {
    private final RemoteIMMediaPaths paths;
    private final LongSupplier timestampProvider;

    public RemoteIMMediaStore(RemoteIMMediaPaths paths) {
        this(paths, System::currentTimeMillis);
    }

    RemoteIMMediaStore(RemoteIMMediaPaths paths, LongSupplier timestampProvider) {
        this.paths = paths;
        this.timestampProvider = timestampProvider;
    }

    public File copyPickedImage(InputStream input, String sourceName) throws IOException {
        if (input == null) throw new IOException("image input is empty");
        File file = new File(
            outgoing(RemoteIMMediaPaths.IMAGES),
            "remote-im-image-" + timestampProvider.getAsLong() + imageExtension(sourceName)
        );
        try (InputStream source = input; FileOutputStream output = new FileOutputStream(file)) {
            byte[] buffer = new byte[8192];
            int read;
            while ((read = source.read(buffer)) != -1) {
                output.write(buffer, 0, read);
            }
        }
        return file;
    }

    public File createVoiceRecordingFile() throws IOException {
        return new File(
            outgoing(RemoteIMMediaPaths.VOICES),
            "remote-im-voice-" + timestampProvider.getAsLong() + ".m4a"
        );
    }

    public File createRealtimeVoiceRecordingFile() throws IOException {
        return new File(outgoing(RemoteIMMediaPaths.VOICES), "voice-" + java.util.UUID.randomUUID() + ".aac");
    }
    /** 阿里实时识别的私有PCM落点，只供当前录音流使用，完成或取消后由宿主删除。 */
    public File createPcmVoiceRecordingFile() throws IOException {
        return new File(outgoing(RemoteIMMediaPaths.VOICES), "voice-" + java.util.UUID.randomUUID() + ".pcm");
    }

    /** 拍照的落点。相机通过 FileProvider 写入，所以必须在 file_paths.xml 覆盖的树里。 */
    public File createCameraPhotoFile() throws IOException {
        return new File(
            outgoing(RemoteIMMediaPaths.IMAGES),
            "camera-" + timestampProvider.getAsLong() + ".jpg"
        );
    }

    public File createOutgoingFile(String sourceName) throws IOException {
        return new File(
            outgoing(RemoteIMMediaPaths.FILES),
            "file-" + timestampProvider.getAsLong() + fileSuffix(sourceName)
        );
    }

    /** 普通发送与 Agent/绑定发送复用的 Android 视频元数据和封面准备；调用方在工作线程执行。 */
    public RemoteIMVideoAttachment prepareVideo(File file) throws IOException {
        android.media.MediaMetadataRetriever retriever = new android.media.MediaMetadataRetriever();
        android.graphics.Bitmap cover = null;
        File poster = null;
        try {
            retriever.setDataSource(file.getAbsolutePath());
            int width = mediaNumber(retriever.extractMetadata(android.media.MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH));
            int height = mediaNumber(retriever.extractMetadata(android.media.MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT));
            int duration = mediaNumber(retriever.extractMetadata(android.media.MediaMetadataRetriever.METADATA_KEY_DURATION));
            if (width <= 0 || height <= 0 || duration <= 0) throw new IOException("视频没有可播放的画面或时长");
            if (android.os.Build.VERSION.SDK_INT >= 27)
                cover = retriever.getScaledFrameAtTime(0, android.media.MediaMetadataRetriever.OPTION_CLOSEST_SYNC,
                    480, Math.max(1, Math.min(480, height * 480 / width)));
            if (cover == null) cover = retriever.getFrameAtTime(0, android.media.MediaMetadataRetriever.OPTION_CLOSEST_SYNC);
            if (cover == null) throw new IOException("无法生成视频封面");
            poster = new File(outgoing(RemoteIMMediaPaths.VIDEO_COVERS), "machine-cover-" + java.util.UUID.randomUUID() + ".jpg");
            try (FileOutputStream output = new FileOutputStream(poster)) {
                if (!cover.compress(android.graphics.Bitmap.CompressFormat.JPEG, 85, output)) throw new IOException("无法保存视频封面");
            }
            return new RemoteIMVideoAttachment(file.getAbsolutePath(), poster.getAbsolutePath(),
                Math.max(1, (duration + 999) / 1000), width, height, file.length());
        } catch (Exception failure) {
            if (poster != null) poster.delete();
            if (failure instanceof IOException) throw (IOException) failure;
            throw new IOException("视频准备失败", failure);
        } finally {
            if (cover != null) cover.recycle();
            retriever.release();
        }
    }

    private static int mediaNumber(String value) {
        try { return Integer.parseInt(value); } catch (NumberFormatException invalid) { return 0; }
    }

    private File outgoing(String kind) throws IOException {
        return paths.directory(RemoteIMMediaPaths.OUTGOING, kind);
    }

    private static String fileSuffix(String sourceName) {
        if (sourceName == null) return ".bin";
        int dotIndex = sourceName.lastIndexOf('.');
        if (dotIndex < 0 || dotIndex == sourceName.length() - 1) return ".bin";
        return sourceName.substring(dotIndex);
    }

    private static String imageExtension(String sourceName) {
        if (sourceName == null) return ".jpg";
        int queryIndex = sourceName.indexOf('?');
        String cleanName = queryIndex >= 0 ? sourceName.substring(0, queryIndex) : sourceName;
        int dotIndex = cleanName.lastIndexOf('.');
        if (dotIndex < 0 || dotIndex == cleanName.length() - 1) return ".jpg";
        String extension = cleanName.substring(dotIndex + 1).toLowerCase(Locale.US);
        if (!extension.matches("[a-z0-9]{1,8}")) return ".jpg";
        return "." + extension;
    }
}
