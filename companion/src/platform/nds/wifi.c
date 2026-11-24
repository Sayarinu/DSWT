#include "platform/nds/wifi.h"

#include <string.h>

#include <nds.h>
#include <dswifi9.h>

static struct {
    int started, failed, scanning, pending;
    uint64_t since;
    unsigned timeoutMs, scanMs;
    char ssid[33], key[65];
} sWifi = { .timeoutMs = 45000 };

static uint32_t ElapsedMs(void)
{
    return systemCounterTicksToMsec((u32)(systemCounterGetTicks() - sWifi.since));
}

int CpWifi_Init(void)
{
    if (sWifi.started) return 0;
    if (!Wifi_InitDefault(INIT_ONLY | WIFI_ATTEMPT_DSI_MODE)) return -1;
    Wifi_EnableWifi();
    sWifi.started = 1;
    return 0;
}

int CpWifi_DsiMode(void)
{
    return sWifi.started && isDSiMode();
}

void CpWifi_Mac(unsigned char mac[6])
{
    memset(mac, 0, 6);
    if (sWifi.started) Wifi_GetData(WIFIGETDATA_MACADDRESS, 6, mac);
}

static void Begin(void)
{
    sWifi.failed = 0;
    sWifi.pending = 1;
    sWifi.since = systemCounterGetTicks();
}

int CpWifi_AutoConnect(void)
{
    if (!sWifi.started) return -1;
    Begin();
    sWifi.scanning = 0;
    Wifi_AutoConnect();
    return 0;
}

int CpWifi_ConnectPassphrase(const char *ssid, const char *passphrase, unsigned scanMs)
{
    size_t ssidLength = ssid ? strlen(ssid) : 0, keyLength = passphrase ? strlen(passphrase) : 0;
    if (!sWifi.started || !ssidLength || ssidLength >= sizeof(sWifi.ssid) || keyLength >= sizeof(sWifi.key)) return -1;
    memcpy(sWifi.ssid, ssid, ssidLength + 1);
    memset(sWifi.key, 0, sizeof(sWifi.key));
    if (keyLength) memcpy(sWifi.key, passphrase, keyLength);
    Begin();
    sWifi.scanning = 1;
    sWifi.scanMs = scanMs;
    Wifi_ScanModeFilter(WSCAN_LIST_AP_ALL);
    return 0;
}

/* While scanning for a passphrase connection: connect once the network is seen. */
static void Scan(void)
{
    int count = Wifi_GetNumAP(), i;
    size_t length = strlen(sWifi.ssid);
    for (i = 0; i < count; i++) {
        Wifi_AccessPoint ap;
        if (Wifi_GetAPData(i, &ap) != WIFI_RETURN_OK || ap.ssid_len != length || memcmp(ap.ssid, sWifi.ssid, length) != 0) continue;
        sWifi.scanning = 0;
        Wifi_SetIP(0, 0, 0, 0, 0);
        Wifi_ConnectSecureAP(&ap, sWifi.key[0] ? sWifi.key : NULL, strlen(sWifi.key));
        memset(sWifi.key, 0, sizeof(sWifi.key));
        return;
    }
    if (ElapsedMs() > sWifi.scanMs) {
        sWifi.scanning = sWifi.pending = 0;
        sWifi.failed = 1;
        memset(sWifi.key, 0, sizeof(sWifi.key));
    }
}

enum CpWifiStatus CpWifi_Status(void)
{
    int status;
    if (!sWifi.started) return CP_WIFI_OFF;
    if (sWifi.scanning) Scan();
    if (sWifi.failed) return CP_WIFI_FAILED;
    if (sWifi.scanning) return CP_WIFI_CONNECTING;
    status = Wifi_AssocStatus();
    if (status == ASSOCSTATUS_ASSOCIATED) { sWifi.pending = 0; return CP_WIFI_CONNECTED; }
    if (!sWifi.pending) return CP_WIFI_IDLE;
    if (status == ASSOCSTATUS_CANNOTCONNECT || ElapsedMs() > sWifi.timeoutMs) {
        sWifi.pending = 0; sWifi.failed = 1;
        Wifi_DisconnectAP();
        return CP_WIFI_FAILED;
    }
    return CP_WIFI_CONNECTING;
}

void CpWifi_SetTimeout(unsigned ms)
{
    sWifi.timeoutMs = ms;
}

const char *CpWifi_StatusText(void)
{
    int status;
    if (!sWifi.started) return "off";
    if (sWifi.scanning) return "scanning";
    status = Wifi_AssocStatus();
    return status >= 0 && status <= ASSOCSTATUS_CANNOTCONNECT ? ASSOCSTATUS_STRINGS[status] : "?";
}

uint32_t CpWifi_Address(void)
{
    return sWifi.started && Wifi_AssocStatus() == ASSOCSTATUS_ASSOCIATED ? Wifi_GetIP() : 0;
}

void CpWifi_Disconnect(void)
{
    if (!sWifi.started) return;
    sWifi.scanning = sWifi.pending = 0;
    memset(sWifi.key, 0, sizeof(sWifi.key));
    Wifi_DisconnectAP();
}
