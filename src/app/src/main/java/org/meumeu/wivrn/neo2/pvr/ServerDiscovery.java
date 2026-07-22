package org.meumeu.wivrn.neo2.pvr;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.net.InetAddress;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

// Server discovery via NSD (mDNS _wivrn._tcp) + persisted manual server list.
// Extracted from the old WivrnLobbyView so the native 3D UI can use it directly.
public class ServerDiscovery {
    private static final String TAG = "ServerDiscovery";

    public static class ServerEntry {
        public String name;
        public String hostname;
        public int port;
        public boolean tcpOnly;
        public boolean manual;
        public boolean discovered;
        public boolean autoconnect;

        public ServerEntry(String name, String hostname, int port, boolean tcpOnly,
                           boolean manual, boolean discovered, boolean autoconnect) {
            this.name = name;
            this.hostname = hostname;
            this.port = port;
            this.tcpOnly = tcpOnly;
            this.manual = manual;
            this.discovered = discovered;
            this.autoconnect = autoconnect;
        }
    }

    private final Context context;
    private final SharedPreferences prefs;
    private final List<ServerEntry> servers = new ArrayList<>();
    private NsdManager nsdManager;
    private NsdManager.DiscoveryListener nsdListener;
    private final Map<String, ServerEntry> discoveredServers = new HashMap<>();
    private boolean autoconnectAttempted = false;

    public ServerDiscovery(Context context) {
        this.context = context;
        this.prefs = context.getSharedPreferences("wivrn_servers", Context.MODE_PRIVATE);
        loadServers();
    }

    public void startDiscovery() {
        try {
            if (nsdManager == null) {
                nsdManager = (NsdManager) context.getSystemService(Context.NSD_SERVICE);
            }
            if (nsdListener != null) {
                nsdManager.stopServiceDiscovery(nsdListener);
                nsdListener = null;
            }
            nsdListener = new NsdManager.DiscoveryListener() {
                @Override
                public void onStartDiscoveryFailed(String serviceType, int errorCode) {
                    Log.e(TAG, "NSD discovery start failed: " + errorCode);
                }

                @Override
                public void onStopDiscoveryFailed(String serviceType, int errorCode) {
                    Log.e(TAG, "NSD discovery stop failed: " + errorCode);
                }

                @Override
                public void onDiscoveryStarted(String serviceType) {
                    Log.i(TAG, "NSD discovery started for " + serviceType);
                }

                @Override
                public void onDiscoveryStopped(String serviceType) {
                    Log.i(TAG, "NSD discovery stopped for " + serviceType);
                }

                @Override
                public void onServiceFound(NsdServiceInfo serviceInfo) {
                    Log.i(TAG, "NSD service found: " + serviceInfo.getServiceName());
                    nsdManager.resolveService(serviceInfo, new NsdManager.ResolveListener() {
                        @Override
                        public void onResolveFailed(NsdServiceInfo serviceInfo, int errorCode) {
                            Log.e(TAG, "NSD resolve failed: " + serviceInfo.getServiceName() + " err=" + errorCode);
                        }

                        @Override
                        public void onServiceResolved(NsdServiceInfo serviceInfo) {
                            String name = serviceInfo.getServiceName();
                            InetAddress host = serviceInfo.getHost();
                            int port = serviceInfo.getPort();
                            String hostname = host != null ? host.getHostAddress() : serviceInfo.getServiceName();

                            Map<String, byte[]> txt = serviceInfo.getAttributes();
                            boolean tcpOnly = false;
                            if (txt != null && txt.containsKey("tcp_only")) {
                                byte[] val = txt.get("tcp_only");
                                if (val != null && new String(val).equals("1"))
                                    tcpOnly = true;
                            }

                            ServerEntry entry = new ServerEntry(name, hostname, port, tcpOnly, false, true, false);
                            synchronized (discoveredServers) {
                                discoveredServers.put(name, entry);
                            }
                            Log.i(TAG, "NSD resolved: " + name + " at " + hostname + ":" + port);
                        }
                    });
                }

                @Override
                public void onServiceLost(NsdServiceInfo serviceInfo) {
                    Log.i(TAG, "NSD service lost: " + serviceInfo.getServiceName());
                    synchronized (discoveredServers) {
                        discoveredServers.remove(serviceInfo.getServiceName());
                    }
                }
            };
            nsdManager.discoverServices("_wivrn._tcp", NsdManager.PROTOCOL_DNS_SD, nsdListener);
        } catch (Exception e) {
            Log.e(TAG, "Failed to start NSD discovery", e);
        }
    }

    public void stopDiscovery() {
        try {
            if (nsdManager != null && nsdListener != null) {
                nsdManager.stopServiceDiscovery(nsdListener);
                nsdListener = null;
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to stop NSD discovery", e);
        }
    }

    public void refreshDiscovery() {
        Log.i(TAG, "Refreshing server discovery");
        synchronized (discoveredServers) {
            discoveredServers.clear();
        }
        stopDiscovery();
        startDiscovery();
    }

    public List<ServerEntry> getAllServers() {
        List<ServerEntry> all = new ArrayList<>();
        synchronized (discoveredServers) {
            all.addAll(discoveredServers.values());
        }
        for (ServerEntry s : servers) {
            boolean found = false;
            for (ServerEntry d : all) {
                if (d.hostname.equals(s.hostname) && d.port == s.port) { found = true; break; }
            }
            if (!found) all.add(s);
        }
        return all;
    }

    public void addOrUpdateServer(String name, String hostname, int port, boolean tcpOnly) {
        for (ServerEntry s : servers) {
            if (s.hostname.equals(hostname) && s.port == port) return;
        }
        servers.add(new ServerEntry(name, hostname, port, tcpOnly, true, false, false));
        saveServers();
    }

    public boolean removeServer(String hostname, int port) {
        boolean removed = false;
        for (int i = servers.size() - 1; i >= 0; i--) {
            ServerEntry s = servers.get(i);
            if (s.hostname.equals(hostname) && s.port == port) {
                servers.remove(i);
                removed = true;
            }
        }
        if (removed) saveServers();
        // Also clear from discovered so it doesn't reappear while still on the network
        synchronized (discoveredServers) {
            discoveredServers.values().removeIf(s -> s.hostname.equals(hostname) && s.port == port);
        }
        return removed;
    }

    public void setAutoconnect(String hostname, int port) {
        boolean currentlyOn = false;
        for (ServerEntry s : servers) {
            if (s.hostname.equals(hostname) && s.port == port && s.autoconnect) {
                currentlyOn = true;
                break;
            }
        }
        if (currentlyOn) {
            for (ServerEntry s : servers) s.autoconnect = false;
        } else {
            boolean found = false;
            for (ServerEntry s : servers) {
                boolean match = s.hostname.equals(hostname) && s.port == port;
                s.autoconnect = match;
                if (match) found = true;
            }
            if (!found) {
                synchronized (discoveredServers) {
                    for (ServerEntry s : discoveredServers.values()) {
                        if (s.hostname.equals(hostname) && s.port == port) {
                            servers.add(new ServerEntry(s.name, s.hostname, s.port, s.tcpOnly, true, false, true));
                            break;
                        }
                    }
                }
            }
        }
        saveServers();
    }

    public void tryAutoconnect(MainActivity activity) {
        if (autoconnectAttempted) return;
        autoconnectAttempted = true;
        for (ServerEntry s : servers) {
            if (s.autoconnect) {
                Log.i(TAG, "Autoconnecting to " + s.hostname + ":" + s.port);
                activity.onServerConnect(s.hostname, s.port, s.tcpOnly);
                return;
            }
        }
    }

    public void markAutoconnectAttempted() { autoconnectAttempted = true; }

    private void loadServers() {
        servers.clear();
        String json = prefs.getString("servers", "[]");
        try {
            JSONArray arr = new JSONArray(json);
            for (int i = 0; i < arr.length(); i++) {
                JSONObject obj = arr.getJSONObject(i);
                servers.add(new ServerEntry(
                    obj.optString("name", ""),
                    obj.optString("hostname", ""),
                    obj.optInt("port", 9757),
                    obj.optBoolean("tcpOnly", false),
                    true, false,
                    obj.optBoolean("autoconnect", false)
                ));
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to load servers", e);
        }
        if (servers.isEmpty()) {
            servers.add(new ServerEntry("Local", "127.0.0.1", 9757, false, true, false, false));
        }
    }

    private void saveServers() {
        try {
            JSONArray arr = new JSONArray();
            for (ServerEntry s : servers) {
                JSONObject obj = new JSONObject();
                obj.put("name", s.name);
                obj.put("hostname", s.hostname);
                obj.put("port", s.port);
                obj.put("tcpOnly", s.tcpOnly);
                obj.put("autoconnect", s.autoconnect);
                arr.put(obj);
            }
            prefs.edit().putString("servers", arr.toString()).apply();
        } catch (Exception e) {
            Log.e(TAG, "Failed to save servers", e);
        }
    }
}
