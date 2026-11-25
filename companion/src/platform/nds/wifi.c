#include "platform/nds/wifi.h"

#include <string.h>

#include <nds.h>
#include <dswifi9.h>

static struct {
    int started, failed, scanning, pending, notFound;
    unsigned stage; /* enum CpWifiStage: the furthest the attempt got */
    uint64_t since;
    unsigned timeoutMs, scanMs;
    char ssid[33];
    unsigned char key[CP_WIFI_KEY_MAX];
    size_t keyLength;
} sWifi = { .timeoutMs = 45000 };

static void WipeKey(void)
{
    volatile unsigned char *key = sWifi.key;
    unsigned i;
    for (i = 0; i < sizeof(sWifi.key); i++) key[i] = 0;
    sWifi.keyLength = 0;
}

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
    sWifi.failed = sWifi.notFound = 0;
    sWifi.stage = CP_WIFI_STAGE_NONE;
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
    return CpWifi_ConnectKey(ssid, (const unsigned char *)passphrase, passphrase ? strlen(passphrase) : 0, scanMs);
}

int CpWifi_ConnectKey(const char *ssid, const unsigned char *key, size_t keyLength, unsigned scanMs)
{
    size_t ssidLength = ssid ? strlen(ssid) : 0;
    if (!sWifi.started || !ssidLength || ssidLength >= sizeof(sWifi.ssid) || keyLength > sizeof(sWifi.key)) return -1;
    memcpy(sWifi.ssid, ssid, ssidLength + 1);
    WipeKey();
    if (keyLength) memcpy(sWifi.key, key, keyLength);
    sWifi.keyLength = keyLength;
    Begin();
    sWifi.scanning = 1;
    sWifi.scanMs = scanMs;
    /* No old attempt may resume while the network is looked up (CpWifi_Scan). */
    Wifi_DisconnectAP();
    Wifi_ScanModeFilter(WSCAN_LIST_AP_ALL);
    return 0;
}

/* While scanning for a passphrase connection: connect once the network is
 * seen, to the strongest of its access points. RSSI grows with the signal in
 * both drivers' units; 0 means no recent beacon. */
static void Scan(void)
{
    int count = Wifi_GetNumAP(), i, found = 0;
    size_t length = strlen(sWifi.ssid);
    Wifi_AccessPoint best;
    memset(&best, 0, sizeof(best));
    for (i = 0; i < count; i++) {
        Wifi_AccessPoint ap;
        if (Wifi_GetAPData(i, &ap) != WIFI_RETURN_OK || ap.ssid_len != length || memcmp(ap.ssid, sWifi.ssid, length) != 0) continue;
        if (!found || (ap.rssi != 0 && (best.rssi == 0 || ap.rssi > best.rssi))) best = ap;
        found = 1;
    }
    if (found) {
        sWifi.scanning = 0;
        sWifi.stage = CP_WIFI_STAGE_SEARCHING;
        Wifi_SetIP(0, 0, 0, 0, 0);
        Wifi_ConnectSecureAP(&best, sWifi.keyLength ? sWifi.key : NULL, sWifi.keyLength);
        WipeKey();
        return;
    }
    if (ElapsedMs() > sWifi.scanMs) {
        sWifi.scanning = sWifi.pending = 0;
        sWifi.failed = sWifi.notFound = 1;
        WipeKey();
    }
}

int CpWifi_Scan(void)
{
    if (!sWifi.started) return -1;
    sWifi.scanning = sWifi.pending = sWifi.failed = 0;
    WipeKey();
    /* Leave any network and end any attempt first: Wifi_ScanMode alone keeps
     * DSWiFi's connect state, so Wifi_AssocStatus would resume the old
     * attempt (stopping the scan) or keep reporting ASSOCIATED on a radio
     * that has left the network. Wifi_DisconnectAP puts the ARM7 back in
     * normal mode and the state machine in CANNOTCONNECT, as
     * Wifi_ConnectSecureAP does before its own scan (access_point.c, 84d91e8). */
    Wifi_DisconnectAP();
    Wifi_ScanMode();
    return 0;
}

unsigned CpWifi_Networks(CpWifiNetwork *out, unsigned max)
{
    int count = sWifi.started ? Wifi_GetNumAP() : 0, i;
    unsigned used = 0;
    for (i = 0; i < count && used < max; i++) {
        Wifi_AccessPoint ap;
        CpWifiNetwork *network = &out[used];
        if (Wifi_GetAPData(i, &ap) != WIFI_RETURN_OK || !ap.ssid_len || ap.ssid_len > CP_WIFI_SSID_MAX) continue;
        /* A name with a NUL in it cannot be told apart from a shorter one. */
        if (memchr(ap.ssid, 0, ap.ssid_len)) continue;
        memset(network, 0, sizeof(*network));
        memcpy(network->ssid, ap.ssid, ap.ssid_len);
        network->security = (uint8_t)CpWifi_Security(ap.security_type, ap.auth_type == AP_AUTH_8021X);
        network->bars = (uint8_t)CpWifi_Bars(ap.rssi, isDSiMode());
        used++;
    }
    return used;
}

int CpWifi_LastFailure(void)
{
    return CpWifi_Failure(sWifi.notFound, sWifi.stage);
}

/* How far the attempt got, from DSWiFi's association state. */
static void Track(int status, int associated)
{
    unsigned stage = CP_WIFI_STAGE_NONE;
    switch (status) {
    case ASSOCSTATUS_SEARCHING: stage = CP_WIFI_STAGE_SEARCHING; break;
    case ASSOCSTATUS_AUTHENTICATING: stage = CP_WIFI_STAGE_AUTHENTICATING; break;
    case ASSOCSTATUS_ASSOCIATING: stage = CP_WIFI_STAGE_ASSOCIATING; break;
    case ASSOCSTATUS_ACQUIRINGDHCP: stage = CP_WIFI_STAGE_DHCP; break;
    default: if (associated) stage = CP_WIFI_STAGE_ASSOCIATED; break;
    }
    if (stage > sWifi.stage) sWifi.stage = stage;
}

enum CpWifiStatus CpWifi_Status(void)
{
    CpWifiPoll poll;
    enum CpWifiStatus result;
    int status;
    if (!sWifi.started) return CP_WIFI_OFF;
    if (sWifi.scanning) Scan();
    if (sWifi.failed) return CP_WIFI_FAILED;
    if (sWifi.scanning) return CP_WIFI_CONNECTING;
    status = Wifi_AssocStatus();
    poll.pending = sWifi.pending;
    poll.associated = status == ASSOCSTATUS_ASSOCIATED;
    poll.cannotConnect = status == ASSOCSTATUS_CANNOTCONNECT;
    poll.ipv4 = poll.associated ? Wifi_GetIP() : 0;
    poll.elapsedMs = sWifi.pending ? ElapsedMs() : 0;
    poll.timeoutMs = sWifi.timeoutMs;
    if (sWifi.pending) Track(status, poll.associated);
    result = CpWifi_Decide(&poll);
    if (result == CP_WIFI_CONNECTED) sWifi.pending = 0;
    else if (result == CP_WIFI_FAILED) {
        sWifi.pending = 0; sWifi.failed = 1;
        Wifi_DisconnectAP();
    }
    return result;
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
    uint32_t address;
    if (!sWifi.started || Wifi_AssocStatus() != ASSOCSTATUS_ASSOCIATED) return 0;
    address = Wifi_GetIP();
    return CpWifi_UsableIPv4(address) ? address : 0;
}

void CpWifi_Disconnect(void)
{
    if (!sWifi.started) return;
    sWifi.scanning = sWifi.pending = 0;
    WipeKey();
    Wifi_DisconnectAP();
}
