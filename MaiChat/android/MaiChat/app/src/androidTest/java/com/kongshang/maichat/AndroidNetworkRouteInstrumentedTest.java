package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assume.assumeTrue;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.NetworkCapabilities;
import android.os.Bundle;
import android.os.SystemClock;
import java.io.File;
import androidx.test.platform.app.InstrumentationRegistry;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Requires an operator-controlled HTTPS endpoint and the actual Wi-Fi/cellular setup. */
@RunWith(AndroidJUnit4.class)
public final class AndroidNetworkRouteInstrumentedTest {
    @Test public void getUsesOnePolicySelectedNetworkForDnsAndSocket() throws Exception {
        String url = InstrumentationRegistry.getArguments().getString("networkProbeUrl", "");
        assumeTrue("Pass networkProbeUrl for a no-charge controlled HTTPS GET", !url.isEmpty());
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        System.loadLibrary("maichat_agent");
        try (MaiAndroidNetworkRouter router = MaiAndroidNetworkRouter.start(context)) {
            router.setForeground(true);
            SystemClock.sleep(2000); // Await ordered capabilities and blocked-status callbacks.
            File trust = new File(context.getCacheDir(), "network-route-system-roots.pem");
            SystemCertificateBundle.writeCurrentPem(trust);
            long[] result = MaiAndroidNetworkRouter.nativeProbe(url, trust.getAbsolutePath());
            assertNotNull(result);
            assertEquals(5, result.length);
            Bundle report = new Bundle();
            report.putLong("curlCode", result[0]);
            report.putBoolean("alternateUsed", result[1] != 0);
            report.putLong("alternateDnsCalls", result[2]);
            report.putLong("alternateSocketCalls", result[3]);
            report.putBoolean("generationChanged", result[4] != 0);
            InstrumentationRegistry.getInstrumentation().sendStatus(0, report);
            if (result[1] != 0) {
                assertTrue("NDK selected-network DNS executed", result[2] > 0);
                assertTrue("NDK selected-network socket binding executed", result[3] > 0);
                ConnectivityManager manager = context.getSystemService(ConnectivityManager.class);
                NetworkCapabilities caps = manager.getNetworkCapabilities(manager.getActiveNetwork());
                assertNotNull(caps);
                assertTrue(caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI));
                assertTrue(!caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED));
                assertTrue(!caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN));
            }
            if (Boolean.parseBoolean(InstrumentationRegistry.getArguments()
                    .getString("requireAlternateNetwork", "false")))
                assertTrue("Controlled unvalidated Wi-Fi + allowed cellular must select alternate", result[1] != 0);
            assertEquals("Controlled HTTPS request must complete", 0, result[0]);
        }
    }
}
