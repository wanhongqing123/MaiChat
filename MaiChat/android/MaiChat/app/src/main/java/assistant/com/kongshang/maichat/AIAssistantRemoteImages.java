// Temporary image identity and completed-file cache. HTTP/OSS validation stays in the shared C++ core.
package com.kongshang.maichat;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.util.UUID;
import java.util.function.Function;
import org.json.JSONArray;
import org.json.JSONObject;

final class AIAssistantRemoteImages {
    /** The original publication login can differ from today's catalog login after the owner signs in again. */
    static final class Request {
        final String account, slot, login, session, message, part, attachment, name, contentType;
        final long size, expiry;
        final String accountKey, cacheKey, signature;

        private Request(String account, String slot, String login, String session, String message, String part,
                        String attachment, String name, String contentType, long size, long expiry) throws Exception {
            this.account = account; this.slot = slot; this.login = login; this.session = session;
            this.message = message; this.part = part; this.attachment = attachment;
            this.name = name; this.contentType = contentType; this.size = size; this.expiry = expiry;
            accountKey = digest(account.getBytes(StandardCharsets.UTF_8));
            JSONArray identity = new JSONArray().put(account).put(slot).put(login).put(session)
                .put(message).put(part).put(attachment);
            cacheKey = digest(identity.toString().getBytes(StandardCharsets.UTF_8));
            signature = digest(identity.put(name).put(contentType).put(size).put(expiry)
                .toString().getBytes(StandardCharsets.UTF_8));
        }

        boolean expired(long now) { return expiry <= now; }

        JSONObject downloadRequest() throws Exception {
            return new JSONObject().put("account_id", account).put("owner_slot", slot)
                .put("owner_login_session_id", login).put("session_id", session)
                .put("message_id", message).put("part_id", part).put("attachment_id", attachment)
                .put("name", name).put("content_type", contentType).put("size_bytes", size).put("expires_at", expiry);
        }

        static Request parse(String account, JSONObject owner, String messageID, JSONObject part,
                             Function<String, MaiCloudDrive.MediaInfo> mediaInfo) {
            try {
                JSONObject image = part == null ? null : part.optJSONObject("remoteImage");
                if (image == null || owner == null || account == null || account.isEmpty()
                    || !"image".equals(text(part, "kind")) || !"temporary".equals(text(image, "retention"))
                    || !("ready".equals(text(image, "state")) || "expired".equals(text(image, "state")))) return null;
                String slot = text(owner, "device_slot"), session = text(owner, "id"), id = text(part, "id");
                String login = text(image, "owner_login_session_id"), attachment = text(image, "attachment_id");
                String name = text(image, "name"), type = text(image, "content_type");
                if (!java.util.Arrays.asList("mac", "windows", "ios_phone", "android_phone", "tablet").contains(slot)
                    || session.isEmpty() || messageID == null || messageID.isEmpty() || id.isEmpty()
                    || part.has("remotePartId") && !part.isNull("remotePartId") && !id.equals(text(part, "remotePartId"))
                    || login.isEmpty() || login.getBytes(StandardCharsets.UTF_8).length > 128
                    || !attachment.matches("[0-9a-fA-F]{8}(-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}")
                    || !text(image, "room_key").equals(slot + ":" + session)
                    || !"image".equals(text(image, "media_type"))) return null;
                long size = integer(image, "size_bytes"), expiry = integer(image, "expires_at");
                MaiCloudDrive.MediaInfo info = mediaInfo.apply(name);
                if (info == null || !"image".equals(info.mediaType) || !type.equals(info.contentType)
                    || size <= 0 || size > info.maxBytes || expiry <= 0 || expiry > 9_007_199_254_740_991L) return null;
                return new Request(account, slot, login, session, messageID, id, attachment, name, type, size, expiry);
            } catch (Exception invalid) { return null; }
        }
    }

    static String text(JSONObject value, String key) {
        Object raw = value == null ? null : value.opt(key);
        return raw instanceof String ? (String) raw : "";
    }

    private static long integer(JSONObject value, String key) {
        Object raw = value.opt(key);
        return raw instanceof Byte || raw instanceof Short || raw instanceof Integer || raw instanceof Long
            ? ((Number) raw).longValue() : -1;
    }

    /** Only a matching receipt, exact byte count and SHA-256 make a local copy reusable, even after OSS expiry. */
    static final class Cache {
        private final File root;
        Cache(File root) {
            // Android may expose its sandbox through /data/user/0 aliases; resolve that trusted root once.
            try { this.root = root.getCanonicalFile(); }
            catch (IOException invalid) { throw new IllegalArgumentException("Invalid remote image cache root", invalid); }
        }

        private File directory(Request request) {
            return new File(new File(new File(root, request.accountKey), request.cacheKey), request.signature);
        }

        private static boolean regular(File file, long expectedSize) throws Exception {
            return file.getCanonicalFile().equals(file.getAbsoluteFile()) && file.isFile() && file.length() == expectedSize;
        }

        synchronized File cached(Request request) {
            try {
                File directory = directory(request), receipt = new File(directory, "complete.json");
                File image = new File(directory, request.name);
                if (!receipt.getCanonicalFile().equals(receipt.getAbsoluteFile()) || !receipt.isFile()
                    || receipt.length() < 1 || receipt.length() > 4096 || !regular(image, request.size)) return null;
                JSONObject parsed = new JSONObject(new String(Files.readAllBytes(receipt.toPath()), StandardCharsets.UTF_8));
                return request.signature.equals(parsed.optString("signature")) && fileDigest(image).equals(parsed.optString("sha256"))
                    ? image : null;
            } catch (Exception invalid) { return null; }
        }

        File staging(Request request) throws Exception {
            File folder = new File(new File(root, request.accountKey), "staging-" + UUID.randomUUID());
            if (!folder.mkdirs() || !folder.getCanonicalFile().equals(folder.getAbsoluteFile()))
                throw new IOException("无法准备图片缓存");
            return new File(folder, request.name);
        }

        synchronized File commit(File staged, Request request) throws Exception {
            if (!request.name.equals(staged.getName()) || !regular(staged, request.size)) throw new IOException("图片下载不完整");
            File existing = cached(request);
            if (existing != null) { discard(staged); return existing; }
            File folder = staged.getParentFile(), destination = directory(request);
            if (!folder.getName().startsWith("staging-")
                || !folder.getParentFile().equals(new File(root, request.accountKey))) throw new IOException("图片缓存归属无效");
            JSONObject receipt = new JSONObject().put("signature", request.signature).put("sha256", fileDigest(staged));
            Files.write(new File(folder, "complete.json").toPath(), receipt.toString().getBytes(StandardCharsets.UTF_8));
            File parent = destination.getParentFile();
            if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("无法保存图片缓存");
            if (!parent.getCanonicalFile().equals(parent.getAbsoluteFile())) throw new IOException("图片缓存归属无效");
            // Each signature folder contains these two known files only. Unexpected files are never recursively removed.
            if (destination.exists()) {
                if (!destination.getCanonicalFile().equals(destination.getAbsoluteFile())) throw new IOException("图片缓存归属无效");
                Files.deleteIfExists(new File(destination, request.name).toPath());
                Files.deleteIfExists(new File(destination, "complete.json").toPath());
                Files.delete(destination.toPath());
            }
            try { Files.move(folder.toPath(), destination.toPath(), StandardCopyOption.ATOMIC_MOVE); }
            catch (AtomicMoveNotSupportedException unsupported) { Files.move(folder.toPath(), destination.toPath()); }
            return new File(destination, request.name);
        }

        void discard(File staged) {
            if (staged == null) return;
            try {
                File folder = staged.getParentFile();
                if (!folder.getName().startsWith("staging-") || !folder.getCanonicalFile().equals(folder.getAbsoluteFile())
                    || !folder.getParentFile().getParentFile().equals(root)) return;
                Files.deleteIfExists(staged.toPath());
                Files.deleteIfExists(new File(folder, "complete.json").toPath());
                Files.deleteIfExists(folder.toPath());
            } catch (Exception ignored) { /* A later OS cache cleanup can reclaim an interrupted private stage. */ }
        }
    }

    private static String fileDigest(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream input = Files.newInputStream(file.toPath())) {
            byte[] buffer = new byte[64 * 1024]; int count;
            while ((count = input.read(buffer)) != -1) digest.update(buffer, 0, count);
        }
        return hex(digest.digest());
    }
    private static String digest(byte[] bytes) throws Exception { return hex(MessageDigest.getInstance("SHA-256").digest(bytes)); }
    private static String hex(byte[] bytes) {
        StringBuilder value = new StringBuilder(bytes.length * 2);
        for (byte item : bytes) { value.append(Character.forDigit((item >>> 4) & 15, 16)); value.append(Character.forDigit(item & 15, 16)); }
        return value.toString();
    }
}
