package com.kongshang.maichat;

/** Pure selection policy, independent of Android callbacks and native transport. */
final class MaiAndroidNetworkPolicy {
    private MaiAndroidNetworkPolicy() {}

    static boolean canFallback(boolean foreground, boolean vpn, boolean defaultWifi,
            boolean defaultValidated, boolean proxy, boolean dataSaver) {
        return foreground && !vpn && defaultWifi && !defaultValidated && !proxy && !dataSaver;
    }

    static boolean isCandidate(boolean cellular, boolean internet, boolean validated,
            boolean unrestricted, boolean notVpn, boolean policyAllowed, boolean isDefault) {
        return cellular && internet && validated && unrestricted && notVpn && policyAllowed &&
                !isDefault;
    }
}
