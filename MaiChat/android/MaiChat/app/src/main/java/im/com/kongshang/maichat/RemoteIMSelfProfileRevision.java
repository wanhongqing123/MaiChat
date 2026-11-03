// 文件作用：头像提交使已在途的本人资料读取失效，不影响同一响应中的好友资料。
package com.kongshang.maichat;

final class RemoteIMSelfProfileRevision {
    private final java.util.concurrent.atomic.AtomicLong revision = new java.util.concurrent.atomic.AtomicLong();
    long snapshot() { return revision.get(); }
    void avatarCommitted() { revision.incrementAndGet(); }
    boolean accepts(String owner, String userId, long requestRevision) {
        return !owner.equals(userId) || requestRevision == revision.get();
    }
}
