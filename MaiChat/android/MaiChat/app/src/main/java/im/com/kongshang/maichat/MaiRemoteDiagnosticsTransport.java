// Platform adapter for ordinary persisted IM sends and safe local metadata.
// No log file, chat body, credential, environment variable or model API is exported.
package com.kongshang.maichat;

import android.app.Activity;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.Callable;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;
import org.json.JSONObject;

final class MaiRemoteDiagnosticsTransport implements MaiRemoteDiagnosticsCoordinator.Transport {
    private final Activity activity;
    private final RemoteIMSessionController session;
    private final BooleanSupplier visible;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final String peer, identity, owner;
    private final int generation;
    private final MaiRemoteDiagnosticsOperation permission = new MaiRemoteDiagnosticsOperation();

    // Constructed on the UI thread when the user opens the confirmation page.
    // A later login with the same visible account still has a different identity.
    MaiRemoteDiagnosticsTransport(Activity activity, RemoteIMSessionController session,
                                  String peer, BooleanSupplier visible) {
        this.activity = activity; this.session = session; this.peer = peer; this.visible = visible;
        this.generation = session.accountGeneration();
        this.owner = session.chatState().ownerUserId();
        this.identity = currentIdentity();
    }

    String identity() { return identity; }

    private String currentIdentity() {
        return session.accountId() + "|" + session.accountGeneration() + "|" +
            MaiChatAuthClient.shared(activity).accountSessionGeneration() + "|" +
            session.currentSdkAppId() + "|" + session.chatState().ownerUserId();
    }

    private boolean current(String expected, String targetPeer, String recipient) {
        return permission.isAllowed() && visible.getAsBoolean() && !activity.isFinishing() && !activity.isDestroyed()
            && identity.equals(expected) && identity.equals(currentIdentity())
            && session.connectionState() == TencentIMClient.ConnectionState.CONNECTED
            && session.acceptsOutgoing(generation, owner, targetPeer)
            && session.acceptsOutgoing(generation, owner, recipient)
            && !owner.equals(recipient);
    }

    @Override public boolean mayContinue(String expected, String targetPeer, String recipient) throws Exception {
        return onMain(() -> current(expected, targetPeer, recipient));
    }

    @Override public void cancelPendingWrites() { permission.cancel(); }

    @Override public String sendText(String targetPeer, String text, String expected) throws Exception {
        return onMain(() -> permission.write(
            () -> peer.equals(targetPeer) && current(expected, targetPeer, targetPeer),
            () -> session.sendTextMessageTo(targetPeer, text, null).id()));
    }

    @Override public MaiRemoteDiagnosticsCoordinator.Delivery delivery(String id) throws Exception {
        return onMain(() -> {
            for (RemoteIMMessage message : session.chatState().messages()) {
                if (!id.equals(message.id())) continue;
                switch (message.status()) {
                    case SENT: return MaiRemoteDiagnosticsCoordinator.Delivery.SENT;
                    case FAILED: return MaiRemoteDiagnosticsCoordinator.Delivery.FAILED;
                    default: return MaiRemoteDiagnosticsCoordinator.Delivery.PENDING;
                }
            }
            return MaiRemoteDiagnosticsCoordinator.Delivery.UNKNOWN;
        });
    }

    @Override public List<MaiRemoteDiagnosticsCoordinator.Receipt> receipts(String targetPeer) throws Exception {
        return onMain(() -> {
            List<MaiRemoteDiagnosticsCoordinator.Receipt> result = new ArrayList<>();
            for (RemoteIMMessage message : session.chatState().recentMessagesWith(targetPeer, 1000)) {
                if (message.direction() != RemoteIMMessage.Direction.INCOMING ||
                    !targetPeer.equals(message.fromUserId())) continue;
                RemoteIMFileAttachment file = message.fileAttachment();
                // Text is used only to recognize fixed nonce-bound failure
                // receipts. It never enters local metadata or the merged report.
                result.add(new MaiRemoteDiagnosticsCoordinator.Receipt(message.id(), message.fromUserId(), true,
                    message.text(), file == null ? "" : file.fileName(),
                    file == null ? null : new File(file.localPath())));
            }
            return result;
        });
    }

    @Override public JSONObject localEvidence(String targetPeer, long began) throws Exception {
        List<MaiRemoteDiagnosticsProtocol.MessageMetadata> metadata = onMain(() -> {
            List<MaiRemoteDiagnosticsProtocol.MessageMetadata> result = new ArrayList<>();
            for (RemoteIMMessage message : session.chatState().messagesWith(targetPeer)) {
                String kind = message.fileAttachment() != null ? "file" :
                    message.imageAttachment() != null ? "image" : message.voiceAttachment() != null ? "voice" :
                    message.videoAttachment() != null ? "video" : "message";
                result.add(new MaiRemoteDiagnosticsProtocol.MessageMetadata(message.id(), message.remoteId(),
                    message.direction().name().toLowerCase(Locale.ROOT),
                    message.status().name().toLowerCase(Locale.ROOT), message.createdAtMillis(), kind));
            }
            return result;
        });
        boolean displayed = onMain(() -> targetPeer.equals(session.chatState().selectedPeerId()));
        return MaiRemoteDiagnosticsProtocol.localEvidence(BuildConfig.VERSION_NAME, Build.VERSION.RELEASE,
            owner, targetPeer, displayed, began, metadata);
    }

    @Override public File writeReport(String id, byte[] data) throws Exception {
        // Use the existing outgoing media tree so history stores a portable
        // relative path and existing backup/account cleanup rules also apply.
        // The filename contains only a validated nonce; account IDs are hashed.
        MaiRemoteDiagnosticsProtocol.command(id);
        File outgoing = RemoteIMMediaPaths.forApp(activity).directory(RemoteIMMediaPaths.OUTGOING,
            RemoteIMMediaPaths.FILES);
        File directory = new File(outgoing, "remote-diagnostics/" +
            MaiRemoteDiagnosticsProtocol.tag(owner, "a"));
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("无法准备报告目录");
        File target = new File(directory, "maichat-diagnostics-" + id + ".md");
        try (FileOutputStream output = new FileOutputStream(target)) {
            output.write(data);
            output.getFD().sync();
        } catch (Exception failure) {
            target.delete();
            throw failure;
        }
        return target;
    }

    @Override public String sendReport(String recipient, File report, String expected) throws Exception {
        return onMain(() -> permission.write(() -> current(expected, peer, recipient),
            () -> session.sendFileMessageTo(recipient, report.getAbsolutePath(), report.getName(),
                "text/markdown", report.length()).id()));
    }

    private <T> T onMain(Callable<T> action) throws Exception {
        if (Looper.myLooper() == Looper.getMainLooper()) return action.call();
        FutureTask<T> pending = new FutureTask<>(action);
        main.post(pending);
        try { return pending.get(5, TimeUnit.SECONDS); }
        finally {
            // If the Activity/main loop stops before starting the task, the
            // canceled FutureTask must not issue an IM write after the timeout.
            if (!pending.isDone()) pending.cancel(false);
        }
    }
}
