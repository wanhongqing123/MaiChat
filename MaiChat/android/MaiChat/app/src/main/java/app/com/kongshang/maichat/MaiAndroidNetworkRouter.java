package com.kongshang.maichat;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.os.Build;

import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;

/** Observes available networks only. Never requests cellular or binds the process. */
public final class MaiAndroidNetworkRouter implements AutoCloseable {
    static { System.loadLibrary("maichat_agent"); }
    private final Context context;
    private final ConnectivityManager manager;
    private final Map<Network, NetworkCapabilities> networks = new HashMap<>();
    private final Map<Network, Boolean> blocked = new HashMap<>();
    private final Map<Network, LinkProperties> links = new HashMap<>();
    private Network defaultNetwork;
    private NetworkCapabilities defaultCapabilities;
    private boolean foreground;
    private boolean closed;
    private static final AtomicLong generation = new AtomicLong();

    private static native void nativePublish(long alternateNetwork, long generation);
    // Instrumentation only: GET without credentials; contains counts/codes, never network content.
    static native long[] nativeProbe(String url, String caBundlePath);

    private final ConnectivityManager.NetworkCallback availableCallback =
            new ConnectivityManager.NetworkCallback() {
        @Override public void onCapabilitiesChanged(Network network, NetworkCapabilities caps) {
            synchronized (MaiAndroidNetworkRouter.this) {
                NetworkCapabilities old = networks.put(network, caps);
                if (routingCapabilities(old) != routingCapabilities(caps)) publish();
            }
        }
        @Override public void onBlockedStatusChanged(Network network, boolean isBlocked) {
            synchronized (MaiAndroidNetworkRouter.this) {
                Boolean old = blocked.put(network, isBlocked);
                if (old == null || old != isBlocked) publish();
            }
        }
        @Override public void onLinkPropertiesChanged(Network network, LinkProperties properties) {
            synchronized (MaiAndroidNetworkRouter.this) {
                if (!properties.equals(links.put(network, properties))) publish();
            }
        }
        @Override public void onLost(Network network) {
            synchronized (MaiAndroidNetworkRouter.this) {
                networks.remove(network);
                blocked.remove(network);
                links.remove(network);
                publish();
            }
        }
    };

    private final ConnectivityManager.NetworkCallback defaultCallback =
            new ConnectivityManager.NetworkCallback() {
        @Override public void onAvailable(Network network) {
            synchronized (MaiAndroidNetworkRouter.this) {
                if (!network.equals(defaultNetwork)) {
                    defaultNetwork = network;
                    defaultCapabilities = null;
                    publish();
                }
            }
        }
        @Override public void onCapabilitiesChanged(Network network, NetworkCapabilities caps) {
            synchronized (MaiAndroidNetworkRouter.this) {
                if (network.equals(defaultNetwork)) {
                    NetworkCapabilities old = defaultCapabilities;
                    defaultCapabilities = caps;
                    if (routingCapabilities(old) != routingCapabilities(caps)) publish();
                }
            }
        }
        @Override public void onLinkPropertiesChanged(Network network, LinkProperties properties) {
            synchronized (MaiAndroidNetworkRouter.this) {
                if (!properties.equals(links.put(network, properties))) publish();
            }
        }
        @Override public void onLost(Network network) {
            synchronized (MaiAndroidNetworkRouter.this) {
                if (network.equals(defaultNetwork)) {
                    defaultNetwork = null;
                    defaultCapabilities = null;
                }
                publish();
            }
        }
    };

    private final BroadcastReceiver policyReceiver = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) {
            synchronized (MaiAndroidNetworkRouter.this) { publish(); }
        }
    };

    public static MaiAndroidNetworkRouter start(Context context) {
        return new MaiAndroidNetworkRouter(context.getApplicationContext());
    }

    private MaiAndroidNetworkRouter(Context context) {
        this.context = context;
        manager = (ConnectivityManager) context.getSystemService(Context.CONNECTIVITY_SERVICE);
        // An empty initial policy keeps curl on the system default until callbacks establish facts.
        nativePublish(0, generation.incrementAndGet());
        if (manager == null) return;
        manager.registerDefaultNetworkCallback(defaultCallback);
        manager.registerNetworkCallback(new NetworkRequest.Builder()
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VPN).build(),
                availableCallback);
        IntentFilter filter = new IntentFilter(ConnectivityManager.ACTION_RESTRICT_BACKGROUND_CHANGED);
        filter.addAction(android.net.Proxy.PROXY_CHANGE_ACTION);
        if (Build.VERSION.SDK_INT >= 33) context.registerReceiver(policyReceiver, filter,
                Context.RECEIVER_NOT_EXPORTED);
        else context.registerReceiver(policyReceiver, filter);
    }

    public synchronized void setForeground(boolean value) {
        if (foreground == value) return;
        foreground = value;
        publish();
    }

    private static long routingCapabilities(NetworkCapabilities caps) {
        if (caps == null) return -1;
        long facts = 0;
        for (int transport : new int[] {NetworkCapabilities.TRANSPORT_WIFI,
                NetworkCapabilities.TRANSPORT_CELLULAR, NetworkCapabilities.TRANSPORT_VPN})
            if (caps.hasTransport(transport)) facts |= 1L << transport;
        for (int capability : new int[] {NetworkCapabilities.NET_CAPABILITY_INTERNET,
                NetworkCapabilities.NET_CAPABILITY_VALIDATED,
                NetworkCapabilities.NET_CAPABILITY_NOT_RESTRICTED,
                NetworkCapabilities.NET_CAPABILITY_NOT_VPN})
            if (caps.hasCapability(capability)) facts |= 1L << (capability + 16);
        return facts;
    }

    private void publish() {
        if (closed) return;
        Network selected = null;
        boolean vpn = defaultCapabilities != null &&
                defaultCapabilities.hasTransport(NetworkCapabilities.TRANSPORT_VPN);
        for (NetworkCapabilities caps : networks.values())
            vpn |= caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN);
        // Conservative policy: no bypass of VPN/proxy/data saver/background restrictions.
        boolean eligible = manager != null && defaultCapabilities != null &&
                MaiAndroidNetworkPolicy.canFallback(foreground, vpn,
                        defaultCapabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI),
                        defaultCapabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED),
                        manager.getDefaultProxy() != null,
                        manager.getRestrictBackgroundStatus() ==
                                ConnectivityManager.RESTRICT_BACKGROUND_STATUS_ENABLED);
        if (eligible) for (Map.Entry<Network, NetworkCapabilities> entry : networks.entrySet()) {
            NetworkCapabilities caps = entry.getValue();
            boolean policyAllowed = Build.VERSION.SDK_INT < 29 ||
                    Boolean.FALSE.equals(blocked.get(entry.getKey()));
            if (MaiAndroidNetworkPolicy.isCandidate(
                    caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR),
                    caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET),
                    caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED),
                    caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_RESTRICTED),
                    caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VPN),
                    policyAllowed, entry.getKey().equals(defaultNetwork)) &&
                    (selected == null || entry.getKey().getNetworkHandle() < selected.getNetworkHandle()))
                selected = entry.getKey();
        }
        nativePublish(selected == null ? 0 : selected.getNetworkHandle(), generation.incrementAndGet());
    }

    @Override public synchronized void close() {
        if (closed) return;
        closed = true;
        nativePublish(0, generation.incrementAndGet());
        if (manager != null) {
            manager.unregisterNetworkCallback(defaultCallback);
            manager.unregisterNetworkCallback(availableCallback);
            context.unregisterReceiver(policyReceiver);
        }
    }
}
