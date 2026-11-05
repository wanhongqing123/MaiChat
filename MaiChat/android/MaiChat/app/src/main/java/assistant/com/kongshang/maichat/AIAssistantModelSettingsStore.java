// One configure-and-persist transaction, shared by ordinary source and option settings calls.
package com.kongshang.maichat;

import android.util.AtomicFile;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.function.BooleanSupplier;

final class AIAssistantModelSettingsStore {
    interface Operation { void run() throws Exception; }

    static void apply(File file, AIAssistantModelSettings proposed, BooleanSupplier ownsAccount,
                      Operation configure, Operation rollback) throws Exception {
        requireOwner(ownsAccount);
        // Native configure validates before replacing the old Agent, including its authoritative
        // anyBusy gate. A rejected configuration must not touch persisted settings.
        configure.run();
        AtomicFile storage = new AtomicFile(file);
        FileOutputStream output = null;
        try {
            requireOwner(ownsAccount);
            byte[] bytes = proposed.toJson().toString().getBytes(StandardCharsets.UTF_8);
            output = storage.startWrite();
            output.write(bytes);
            // AtomicFile.finishWrite logs sync/close/rename failures instead of throwing them.
            // Explicitly sync while the descriptor is open, then verify the actual base file.
            output.getFD().sync();
            requireOwner(ownsAccount);
            storage.finishWrite(output);
            output = null;
            verifyCommitted(storage, bytes);
        } catch (Exception failure) {
            if (output != null) storage.failWrite(output);
            // A closed account is waiting for this worker to retire its native handle. Do not
            // recreate or configure it after the deletion/identity barrier has invalidated it.
            if (ownsAccount.getAsBoolean()) {
                try { rollback.run(); }
                catch (Exception rollbackFailure) { failure.addSuppressed(rollbackFailure); }
            }
            throw failure;
        }
    }

    private static void requireOwner(BooleanSupplier ownsAccount) throws IOException {
        if (!ownsAccount.getAsBoolean()) throw new IOException("账号已切换，未保存模型设置");
    }

    private static void verifyCommitted(AtomicFile storage, byte[] expected) throws IOException {
        // openRead also retires a failed .new file/legacy backup using the platform's own
        // recovery rules. Read only the expected length plus one byte, not an unbounded file.
        try (FileInputStream input = storage.openRead()) {
            byte[] actual = new byte[expected.length];
            int offset = 0;
            while (offset < actual.length) {
                int count = input.read(actual, offset, actual.length - offset);
                if (count < 0) throw new IOException("主模型设置未完整写入，原配置已保留");
                offset += count;
            }
            if (input.read() != -1 || !Arrays.equals(actual, expected))
                throw new IOException("主模型设置提交校验失败，原配置已保留");
        }
    }
}
