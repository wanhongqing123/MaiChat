package com.kongshang.maichat.tools;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.os.Looper;
import android.provider.Telephony;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.function.BooleanSupplier;

/** Ordinary Android SMS API shared by App callers and the Agent adapter. Read-only, foreground-only. */
public final class MobileSMSInbox {
    public static final class Message {
        public final long id, timestampMilliseconds;
        public final String sender, body;
        public final boolean read;

        private Message(long id, String sender, String body, long timestampMilliseconds, boolean read) {
            this.id = id;
            this.sender = sender;
            this.body = body;
            this.timestampMilliseconds = timestampMilliseconds;
            this.read = read;
        }
    }

    private final Context context;
    private final BooleanSupplier foreground;

    public MobileSMSInbox(Context context, BooleanSupplier foreground) {
        this.context = context.getApplicationContext();
        this.foreground = foreground;
    }

    /** Run off the UI thread, after READ_SMS is granted. Never marks a message read or sends SMS. */
    public List<Message> readRecent(int limit, long afterMilliseconds) {
        if (limit < 1 || limit > 20 || afterMilliseconds < 0)
            throw new IllegalArgumentException("limit must be 1-20 and after_ms must be nonnegative");
        if (context.checkSelfPermission(Manifest.permission.READ_SMS) != PackageManager.PERMISSION_GRANTED)
            throw new SecurityException("READ_SMS is required");
        if (!foreground.getAsBoolean())
            throw new IllegalStateException("Open MaiChat before reading SMS messages.");
        if (Looper.myLooper() == Looper.getMainLooper())
            throw new IllegalStateException("SMS lookup cannot block the UI thread.");
        String[] columns = {Telephony.Sms._ID, Telephony.Sms.ADDRESS, Telephony.Sms.BODY,
            Telephony.Sms.DATE, Telephony.Sms.READ};
        String selection = afterMilliseconds > 0 ? Telephony.Sms.DATE + " >= ?" : null;
        String[] values = afterMilliseconds > 0 ? new String[]{Long.toString(afterMilliseconds)} : null;
        List<Message> messages = new ArrayList<>();
        try (Cursor cursor = context.getContentResolver().query(Telephony.Sms.Inbox.CONTENT_URI,
                 columns, selection, values, Telephony.Sms.DATE + " DESC")) {
            if (cursor == null) throw new IllegalStateException("The SMS inbox is unavailable.");
            int id = cursor.getColumnIndexOrThrow(Telephony.Sms._ID);
            int sender = cursor.getColumnIndexOrThrow(Telephony.Sms.ADDRESS);
            int body = cursor.getColumnIndexOrThrow(Telephony.Sms.BODY);
            int timestamp = cursor.getColumnIndexOrThrow(Telephony.Sms.DATE);
            int read = cursor.getColumnIndexOrThrow(Telephony.Sms.READ);
            while (messages.size() < limit && cursor.moveToNext()) {
                messages.add(new Message(cursor.getLong(id), cursor.isNull(sender) ? "" : cursor.getString(sender),
                    cursor.isNull(body) ? "" : cursor.getString(body), cursor.getLong(timestamp), cursor.getInt(read) != 0));
            }
        }
        // A permission revocation or app switch during the query must not release its result.
        if (context.checkSelfPermission(Manifest.permission.READ_SMS) != PackageManager.PERMISSION_GRANTED)
            throw new SecurityException("READ_SMS was revoked");
        if (!foreground.getAsBoolean())
            throw new IllegalStateException("Open MaiChat before reading SMS messages.");
        return Collections.unmodifiableList(messages);
    }
}
