// 文件作用：按累计采样时间预算读取仍在增长的 16 kHz、PCM16 小端、单声道录音。
// 边界：不转换或填充音频；每次最多 200 ms；不足一个完整样本的尾字节留待后续写入。
package com.kongshang.maichat;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;

/** Reads bounded, complete PCM samples without outrunning a recording's elapsed sample budget. */
final class PcmAudioReader implements AutoCloseable {
    private static final int BYTES_PER_SAMPLE = 2;
    private static final int MAX_PACKET_SAMPLES = 3200;
    private final RandomAccessFile file;
    private long offset, samples;

    PcmAudioReader(File source) throws IOException {
        if (source == null) throw new IOException("PCM recording file is required");
        file = new RandomAccessFile(source, "r");
    }

    // sampleBudget 是从本次录音开始计算的绝对样本数，不是本次允许多读的数量。
    synchronized byte[] readUntilSample(long sampleBudget) throws IOException {
        long length = file.length();
        if (length < offset) throw new IOException("PCM recording was truncated");
        if (sampleBudget <= samples) return new byte[0];
        long availableSamples = (length - offset) / BYTES_PER_SAMPLE;
        long count = Math.min(availableSamples,
            Math.min(sampleBudget - samples, MAX_PACKET_SAMPLES));
        if (count <= 0) return new byte[0];
        byte[] packet = new byte[(int) count * BYTES_PER_SAMPLE];
        file.seek(offset);
        file.readFully(packet);
        offset += packet.length;
        samples += count;
        return packet;
    }

    synchronized long samples() { return samples; }

    // 当前读到尾部不意味着录音已经停止；宿主应先停采集，再循环排完完整 PCM，最后发 Stop。
    // 一个尚未配齐的尾字节也不是结束，不能为了提前发 Stop 而将其丢弃。
    synchronized boolean atEnd() throws IOException { return file.length() == offset; }

    @Override public synchronized void close() throws IOException { file.close(); }
}
