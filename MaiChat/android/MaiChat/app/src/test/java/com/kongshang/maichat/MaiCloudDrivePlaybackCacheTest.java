package com.kongshang.maichat;

import static org.junit.Assert.*;

import org.junit.Test;

public final class MaiCloudDrivePlaybackCacheTest {
    @Test public void diskBudgetKeepsLongLengthsWithoutAnOldVideoPolicyCap() {
        long bytes = 20_000_000_000L;
        long reserve = 64L * 1024 * 1024;
        assertTrue(MaiCloudDrivePlaybackCache.hasSpace(bytes + reserve, bytes));
        assertFalse(MaiCloudDrivePlaybackCache.hasSpace(bytes + reserve - 1, bytes));
    }

    @Test public void reserveAdditionCannotWrapAtLongMaximum() {
        assertFalse(MaiCloudDrivePlaybackCache.hasSpace(Long.MAX_VALUE, Long.MAX_VALUE));
    }
}
