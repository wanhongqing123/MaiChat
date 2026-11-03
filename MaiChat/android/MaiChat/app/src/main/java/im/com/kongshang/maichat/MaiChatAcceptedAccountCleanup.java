// 文件作用：已获 202 回执的本机账号清理计划与安全文件删除；不接触远端或外部工作区。
// 边界：目录只由 App 私有根和账号 SHA-256 推导；预检拒绝父级路径、符号链接及 junction。
package com.kongshang.maichat;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.StandardCopyOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import org.json.JSONObject;

final class MaiChatAcceptedAccountCleanup {
    static final class Pending {
        final String owner, account, authIdentity, requestId;
        Pending(String owner, String account, String authIdentity, String requestId) {
            this.owner = owner; this.account = account; this.authIdentity = authIdentity; this.requestId = requestId;
        }
    }
    private MaiChatAcceptedAccountCleanup() { }

    static String accountKey(String owner) throws IOException {
        if (owner == null || owner.isEmpty() || !owner.equals(owner.trim()) || owner.length() > 512)
            throw new IOException("Invalid accepted account owner");
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(owner.getBytes(StandardCharsets.UTF_8));
            StringBuilder name = new StringBuilder(64);
            for (byte value : digest) name.append(Character.forDigit((value >>> 4) & 15, 16)).append(Character.forDigit(value & 15, 16));
            return name.toString();
        } catch (java.security.NoSuchAlgorithmException failure) { throw new IOException("SHA-256 unavailable"); }
    }

    static void removeWorkspace(File noBackup, String owner) throws IOException {
        String key = accountKey(owner);
        Path base = guarded(noBackup, new File(noBackup, "AIAssistant"));
        if (!Files.exists(base, LinkOption.NOFOLLOW_LINKS)) return;
        Path marker = guarded(noBackup, base.resolve("legacy-owner-user-id.txt").toFile());
        String legacyOwner = Files.isRegularFile(marker, LinkOption.NOFOLLOW_LINKS)
            ? new String(readSmall(marker), StandardCharsets.UTF_8).trim() : "";
        List<Path> targets = new ArrayList<>();
        boolean legacy = owner.equals(legacyOwner);
        if (legacy) {
            try (java.nio.file.DirectoryStream<Path> children = Files.newDirectoryStream(base)) {
                for (Path child : children) if (!child.getFileName().toString().equals("Accounts")
                    && !child.getFileName().toString().equals("Guest")
                    && !child.getFileName().toString().equals("legacy-owner-user-id.txt")) targets.add(child);
            }
        } else targets.add(base.resolve("Accounts").resolve(key));
        // 任一目标有越界链接时不开始删除，保留 pending 供下次重试和人工定位。
        for (Path target : targets) preflight(noBackup, target);
        for (Path target : targets) deleteTree(noBackup, target);
        // 归属标记最后删除；中断时仍能识别剩余 legacy 数据，pending 重试不会误走 hash 目录。
        if (legacy) Files.deleteIfExists(guarded(noBackup, marker.toFile()));
    }

    static String mediaIdentity(File root, String relative) throws IOException {
        if (relative == null || relative.isEmpty() || new File(relative).isAbsolute()
            || relative.matches("^[A-Za-z]:.*")) throw new IOException("Invalid managed media path");
        String portable = relative.replace('\\', '/');
        for (String segment : portable.split("/", -1))
            if (segment.isEmpty() || segment.equals(".") || segment.equals("..")) throw new IOException("Invalid managed media path");
        return guarded(root, new File(root, portable.replace('/', File.separatorChar))).toString();
    }
    static void deleteMedia(File root, String relative) throws IOException {
        Path file = new File(mediaIdentity(root, relative)).toPath();
        if (Files.isDirectory(file, LinkOption.NOFOLLOW_LINKS)) throw new IOException("Media path is not a file");
        Files.deleteIfExists(file);
    }

    static Pending markPending(File noBackup, String owner, String account, String authIdentity, String requestId) throws Exception {
        String key = accountKey(owner);
        if (account == null || account.isEmpty() || account.length() > 128 || authIdentity == null
            || authIdentity.length() > 1024 || requestId == null || !requestId.matches("[0-9a-f-]{36}"))
            throw new IOException("Invalid accepted account proof");
        Pending proof = new Pending(owner, account, authIdentity, requestId);
        Path directory = guarded(noBackup, new File(noBackup, "AcceptedAccountDeletions"));
        Files.createDirectories(directory);
        Path target = guarded(noBackup, directory.resolve(key + ".json").toFile());
        Path temporary = Files.createTempFile(directory, ".pending-", ".tmp");
        try {
            JSONObject value = new JSONObject().put("owner", owner).put("account", account)
                .put("auth_identity", authIdentity).put("request_id", requestId);
            Files.write(temporary, value.toString().getBytes(StandardCharsets.UTF_8));
            try { Files.move(temporary, target, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE); }
            catch (AtomicMoveNotSupportedException unsupported) { Files.move(temporary, target, StandardCopyOption.REPLACE_EXISTING); }
        } finally { Files.deleteIfExists(temporary); }
        return proof;
    }
    static List<Pending> pending(File noBackup) throws Exception {
        List<Pending> result = new ArrayList<>();
        Path directory = guarded(noBackup, new File(noBackup, "AcceptedAccountDeletions"));
        if (!Files.exists(directory, LinkOption.NOFOLLOW_LINKS)) return result;
        try (java.nio.file.DirectoryStream<Path> children = Files.newDirectoryStream(directory)) {
            for (Path child : children) {
                if (!child.getFileName().toString().matches("[0-9a-f]{64}\\.json")) continue;
                Path safe = guarded(noBackup, child.toFile());
                JSONObject value = new JSONObject(new String(readSmall(safe), StandardCharsets.UTF_8));
                Pending proof = new Pending(value.getString("owner"), value.getString("account"),
                    value.getString("auth_identity"), value.getString("request_id"));
                if (!child.getFileName().toString().equals(accountKey(proof.owner) + ".json")
                    || proof.account.isEmpty() || proof.account.length() > 128 || proof.authIdentity.length() > 1024
                    || !proof.requestId.matches("[0-9a-f-]{36}")) throw new IOException("Invalid pending cleanup proof");
                result.add(proof);
            }
        }
        return result;
    }
    static void finishPending(File noBackup, Pending proof) throws Exception {
        Path marker = guarded(noBackup, new File(new File(noBackup, "AcceptedAccountDeletions"), accountKey(proof.owner) + ".json"));
        if (!Files.exists(marker, LinkOption.NOFOLLOW_LINKS)) return;
        JSONObject current = new JSONObject(new String(readSmall(marker), StandardCharsets.UTF_8));
        if (proof.requestId.equals(current.optString("request_id")) && proof.authIdentity.equals(current.optString("auth_identity")))
            Files.delete(marker);
    }

    private static byte[] readSmall(Path path) throws IOException {
        if (Files.size(path) > 8192) throw new IOException("Cleanup metadata is too large");
        byte[] bytes = Files.readAllBytes(path);
        if (bytes.length > 8192) throw new IOException("Cleanup metadata is too large");
        return bytes;
    }
    private static Path guarded(File root, File candidate) throws IOException {
        Path lexicalRoot = root.toPath().toAbsolutePath().normalize();
        Path base = root.getCanonicalFile().toPath().toAbsolutePath().normalize();
        Path absolute = candidate.toPath().toAbsolutePath();
        if (!absolute.equals(absolute.normalize())) throw new IOException("Unnormalized cleanup target");
        Path relative;
        if (absolute.startsWith(lexicalRoot)) relative = lexicalRoot.relativize(absolute);
        else if (absolute.startsWith(base)) relative = base.relativize(absolute);
        else throw new IOException("Cleanup target is outside managed root");
        if (relative.getNameCount() == 0 || relative.toString().isEmpty()) throw new IOException("Cannot delete managed root");
        Path resolved = base;
        for (Path segment : relative) {
            resolved = resolved.resolve(segment);
            if (Files.isSymbolicLink(resolved) || Files.exists(resolved, LinkOption.NOFOLLOW_LINKS)
                && !resolved.toRealPath().equals(resolved)) throw new IOException("Cleanup target contains a link");
        }
        return resolved;
    }
    private static void preflight(File root, Path target) throws IOException {
        Path safe = guarded(root, target.toFile());
        if (!Files.exists(safe, LinkOption.NOFOLLOW_LINKS)) return;
        Files.walkFileTree(safe, new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult preVisitDirectory(Path path, BasicFileAttributes attributes) throws IOException {
                guarded(root, path.toFile()); return FileVisitResult.CONTINUE;
            }
            @Override public FileVisitResult visitFile(Path path, BasicFileAttributes attributes) throws IOException {
                guarded(root, path.toFile()); return FileVisitResult.CONTINUE;
            }
        });
    }
    private static void deleteTree(File root, Path target) throws IOException {
        Path safe = guarded(root, target.toFile());
        if (!Files.exists(safe, LinkOption.NOFOLLOW_LINKS)) return;
        Files.walkFileTree(safe, new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult visitFile(Path path, BasicFileAttributes attributes) throws IOException {
                Files.delete(guarded(root, path.toFile())); return FileVisitResult.CONTINUE;
            }
            @Override public FileVisitResult postVisitDirectory(Path path, IOException failure) throws IOException {
                if (failure != null) throw failure;
                Files.delete(guarded(root, path.toFile())); return FileVisitResult.CONTINUE;
            }
        });
    }
}
