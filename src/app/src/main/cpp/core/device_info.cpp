#include "device_info.h"
#include "log.h"
#include <cstdio>
#include <cstring>
#include <sys/system_properties.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

char gIpText[24]       = "Check Wi-Fi";
char gStatusText[32]   = "Disconnected";
char gModelText[32]    = "Pico";
char gHostnameText[24] = "";
bool gIsEyeHw = false;

void toUpperAscii(char *s) { for (; *s; ++s) if (*s >= 'a' && *s <= 'z') *s -= 32; }

// Build.MODEL is "Pico Neo 2" on both Neo 2 and Neo 2 EYE, so use the vendor
// property pxr.vendorhw.product.model to distinguish them. pxr.vendorhw.eye=1
// is the definitive eye-hardware flag.
void readHeadsetModel(JNIEnv *env) {
    (void) env;
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get("pxr.vendorhw.product.model", buf) > 0 && buf[0]) {
        strncpy(gModelText, buf, sizeof(gModelText)-1);
        gModelText[sizeof(gModelText)-1] = 0;
    } else {
        snprintf(gModelText, sizeof(gModelText), "Pico Neo 2");
    }
    char eb[PROP_VALUE_MAX] = {0};
    gIsEyeHw = (__system_property_get("pxr.vendorhw.eye", eb) > 0 && eb[0] == '1');
    LOGI("hw model='%s' isEyeHw=%d", gModelText, gIsEyeHw ? 1 : 0);
}

void refreshDeviceIp() {
    struct ifaddrs *ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return;
    char best[24] = "Check Wi-Fi"; bool gotWlan = false;
    for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        char buf[INET_ADDRSTRLEN];
        auto *sin = (struct sockaddr_in *) p->ifa_addr;
        inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
        bool isWlan = p->ifa_name && strncmp(p->ifa_name, "wlan", 4) == 0;
        if (isWlan) { strncpy(best, buf, sizeof(best)-1); best[sizeof(best)-1]=0; gotWlan = true; break; }
        if (!gotWlan) { strncpy(best, buf, sizeof(best)-1); best[sizeof(best)-1]=0; }
    }
    freeifaddrs(ifa);
    strncpy(gIpText, best, sizeof(gIpText)-1); gIpText[sizeof(gIpText)-1]=0;
}
