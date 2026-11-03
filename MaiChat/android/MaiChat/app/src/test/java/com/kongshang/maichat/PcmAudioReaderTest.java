// 文件作用：验证增长中的 16 kHz PCM16 单声道文件按采样预算分包，不跳过半个样本或丢掉尾音。
// 运行边界：仅使用本机临时文件，不依赖 Android、录音设备或网络。
package com.kongshang.maichat;

import static org.junit.Assert.*;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.file.Files;
import java.util.Arrays;
import org.junit.Test;

public class PcmAudioReaderTest {
    @Test public void sampleBudgetIsAbsoluteAndReadsOnlyTheAllowedCompleteSamples() throws Exception {
        File source = File.createTempFile("maichat-pcm-budget-", ".pcm");
        byte[] audio = pcm(20);
        Files.write(source.toPath(), audio);
        try (PcmAudioReader reader = new PcmAudioReader(source)) {
            assertEquals(0, reader.readUntilSample(0).length);
            assertArrayEquals(Arrays.copyOfRange(audio, 0, 6), reader.readUntilSample(3));
            assertEquals(3, reader.samples());
            assertEquals(0, reader.readUntilSample(3).length);
            assertEquals(0, reader.readUntilSample(2).length);
            assertEquals(0, reader.readUntilSample(Long.MIN_VALUE).length);
            assertArrayEquals(Arrays.copyOfRange(audio, 6, 10), reader.readUntilSample(5));
            assertEquals(5, reader.samples());
            assertFalse(reader.atEnd());
        } finally { source.delete(); }
    }

    @Test public void eachPacketContainsAtMost200MillisecondsEvenWhenTheBudgetIsFarAhead() throws Exception {
        File source = File.createTempFile("maichat-pcm-packet-", ".pcm");
        byte[] audio = pcm(15_000);
        Files.write(source.toPath(), audio);
        try (PcmAudioReader reader = new PcmAudioReader(source)) {
            assertArrayEquals(Arrays.copyOfRange(audio, 0, 6400), reader.readUntilSample(100_000));
            assertEquals(3200, reader.samples());
            assertArrayEquals(Arrays.copyOfRange(audio, 6400, 12_800), reader.readUntilSample(100_000));
            assertEquals(6400, reader.samples());
            assertFalse(reader.atEnd());
            assertArrayEquals(Arrays.copyOfRange(audio, 12_800, 15_000), reader.readUntilSample(100_000));
            assertEquals(7500, reader.samples());
            assertTrue(reader.atEnd());
        } finally { source.delete(); }
    }

    @Test public void anInitiallyEmptyFileCanGrowAgainAfterTheReaderReachedItsCurrentEnd() throws Exception {
        File source = File.createTempFile("maichat-pcm-growth-", ".pcm");
        try (FileOutputStream writer = new FileOutputStream(source);
             PcmAudioReader reader = new PcmAudioReader(source)) {
            assertTrue(reader.atEnd());
            assertEquals(0, reader.readUntilSample(10).length);
            writer.write(new byte[] {1, 2, 3, 4}); writer.flush();
            assertFalse(reader.atEnd());
            assertArrayEquals(new byte[] {1, 2, 3, 4}, reader.readUntilSample(10));
            assertTrue(reader.atEnd());
            writer.write(new byte[] {5, 6}); writer.flush();
            assertFalse(reader.atEnd());
            assertArrayEquals(new byte[] {5, 6}, reader.readUntilSample(10));
            assertEquals(3, reader.samples());
            assertTrue(reader.atEnd());
        } finally { source.delete(); }
    }

    @Test public void anOddTrailingByteWaitsAndIsPairedWithTheNextAppendedByte() throws Exception {
        File source = File.createTempFile("maichat-pcm-half-sample-", ".pcm");
        try (FileOutputStream writer = new FileOutputStream(source);
             PcmAudioReader reader = new PcmAudioReader(source)) {
            writer.write(new byte[] {0x34, 0x12, (byte) 0xff}); writer.flush();
            assertArrayEquals(new byte[] {0x34, 0x12}, reader.readUntilSample(10));
            assertEquals(1, reader.samples());
            assertEquals(0, reader.readUntilSample(10).length);
            assertFalse(reader.atEnd());
            writer.write(0x80); writer.flush();
            assertArrayEquals(new byte[] {(byte) 0xff, (byte) 0x80}, reader.readUntilSample(10));
            assertEquals(2, reader.samples());
            assertTrue(reader.atEnd());
        } finally { source.delete(); }
    }

    @Test public void oneByteAtTheStartCannotBeConsumedBeforeACompleteSampleExists() throws Exception {
        File source = File.createTempFile("maichat-pcm-first-byte-", ".pcm");
        try (FileOutputStream writer = new FileOutputStream(source);
             PcmAudioReader reader = new PcmAudioReader(source)) {
            writer.write(0x11); writer.flush();
            assertEquals(0, reader.readUntilSample(1).length);
            assertEquals(0, reader.samples());
            assertFalse(reader.atEnd());
            writer.write(0x22); writer.flush();
            assertArrayEquals(new byte[] {0x11, 0x22}, reader.readUntilSample(1));
            assertEquals(1, reader.samples());
            assertTrue(reader.atEnd());
        } finally { source.delete(); }
    }

    @Test public void stoppingDrainsEveryCompleteSampleIncludingAShortFinalPacketWithoutPadding() throws Exception {
        File source = File.createTempFile("maichat-pcm-stop-", ".pcm");
        byte[] audio = pcm(6400 * 2 + 22);
        Files.write(source.toPath(), audio);
        try (PcmAudioReader reader = new PcmAudioReader(source)) {
            ByteArrayOutputStream sent = new ByteArrayOutputStream();
            int packets = 0;
            while (!reader.atEnd()) {
                byte[] packet = reader.readUntilSample(Long.MAX_VALUE);
                assertTrue(packet.length > 0 && packet.length <= 6400);
                assertEquals(0, packet.length % 2);
                sent.write(packet);
                packets++;
            }
            assertEquals(3, packets);
            assertArrayEquals(audio, sent.toByteArray());
            assertEquals(audio.length / 2, reader.samples());
            assertEquals(0, reader.readUntilSample(Long.MAX_VALUE).length);
        } finally { source.delete(); }
    }

    @Test public void unreadSamplesWaitForTheBudgetEvenWhenTheWholeRecordingIsAlreadyOnDisk() throws Exception {
        File source = File.createTempFile("maichat-pcm-budget-tail-", ".pcm");
        byte[] audio = pcm(6402);
        Files.write(source.toPath(), audio);
        try (PcmAudioReader reader = new PcmAudioReader(source)) {
            assertEquals(6400, reader.readUntilSample(3200).length);
            assertEquals(0, reader.readUntilSample(3200).length);
            assertEquals(3200, reader.samples());
            assertFalse(reader.atEnd());
            assertArrayEquals(Arrays.copyOfRange(audio, 6400, 6402), reader.readUntilSample(3201));
            assertTrue(reader.atEnd());
        } finally { source.delete(); }
    }

    private static byte[] pcm(int length) {
        byte[] audio = new byte[length];
        for (int index = 0; index < length; index++) audio[index] = (byte) (index * 73 + 11);
        return audio;
    }
}
