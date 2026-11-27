package com.kongshang.maichat;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import org.junit.Test;

public final class AndroidNetworkRoutePolicyTest {
    @Test public void fallbackRequiresForegroundUnvalidatedWifiWithoutRestrictions() {
        assertTrue(MaiAndroidNetworkPolicy.canFallback(true, false, true, false, false, false));
        assertFalse(MaiAndroidNetworkPolicy.canFallback(false, false, true, false, false, false));
        assertFalse(MaiAndroidNetworkPolicy.canFallback(true, true, true, false, false, false));
        assertFalse(MaiAndroidNetworkPolicy.canFallback(true, false, true, true, false, false));
        assertFalse(MaiAndroidNetworkPolicy.canFallback(true, false, false, false, false, false));
        assertFalse(MaiAndroidNetworkPolicy.canFallback(true, false, true, false, true, false));
        assertFalse(MaiAndroidNetworkPolicy.canFallback(true, false, true, false, false, true));
    }

    @Test public void noCapabilityOrBlockedObservationIsAssumedAllowed() {
        assertTrue(MaiAndroidNetworkPolicy.isCandidate(true, true, true, true, true, true, false));
        for (int missing = 0; missing < 6; ++missing) {
            boolean[] caps = {true, true, true, true, true, true};
            caps[missing] = false;
            assertFalse(MaiAndroidNetworkPolicy.isCandidate(caps[0], caps[1], caps[2], caps[3],
                    caps[4], caps[5], false));
        }
        assertFalse(MaiAndroidNetworkPolicy.isCandidate(true, true, true, true, true, true, true));
    }
}
