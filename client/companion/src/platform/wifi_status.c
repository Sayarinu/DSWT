#include "platform/wifi_status.h"

#include <string.h>

int CpWifi_UsableIPv4(uint32_t address)
{
    return address != 0 && address != 0xFFFFFFFFu;
}

enum CpWifiStatus CpWifi_Decide(const CpWifiPoll *poll)
{
    if (poll->associated && CpWifi_UsableIPv4(poll->ipv4)) return CP_WIFI_CONNECTED;
    if (!poll->pending) return CP_WIFI_IDLE;
    if (poll->cannotConnect || poll->elapsedMs > poll->timeoutMs) return CP_WIFI_FAILED;
    return CP_WIFI_CONNECTING;
}

enum CpWifiSecurity CpWifi_Security(int apSecurityType, int auth8021x)
{
    switch (apSecurityType) {
    case 0: return CP_WIFI_SECURITY_OPEN;
    case 1: return CP_WIFI_SECURITY_WEP;
    case 2: return auth8021x ? CP_WIFI_SECURITY_ENTERPRISE : CP_WIFI_SECURITY_WPA;
    default: return auth8021x ? CP_WIFI_SECURITY_ENTERPRISE : CP_WIFI_SECURITY_WPA2;
    }
}

unsigned CpWifi_Bars(int rssi, int dsiMode)
{
    if (rssi == 0) return 0;
    if (dsiMode) return rssi >= -60 ? 3 : rssi >= -70 ? 2 : rssi >= -80 ? 1 : 0;
    return rssi >= 36 ? 3 : rssi >= 24 ? 2 : rssi >= 12 ? 1 : 0;
}

enum CpWifiUnsupported CpWifi_Unsupported(unsigned security, int wpa2Capable)
{
    switch (security) {
    case CP_WIFI_SECURITY_OPEN:
    case CP_WIFI_SECURITY_WEP: return CP_WIFI_SUPPORTED;
    case CP_WIFI_SECURITY_WPA2: return wpa2Capable ? CP_WIFI_SUPPORTED : CP_WIFI_UNSUPPORTED_NEEDS_DSI;
    case CP_WIFI_SECURITY_WPA: return CP_WIFI_UNSUPPORTED_TKIP;
    default: return CP_WIFI_UNSUPPORTED_ENTERPRISE;
    }
}

static int Hex(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int AllHex(const char *key)
{
    for (; *key; ++key) if (Hex((unsigned char)*key) < 0) return 0;
    return 1;
}

static int Printable(const char *key)
{
    for (; *key; ++key) if ((unsigned char)*key < 0x20 || (unsigned char)*key > 0x7E) return 0;
    return 1;
}

int CpWifi_KeyValid(unsigned security, const char *key)
{
    size_t length;
    if (!key || !Printable(key)) return 0;
    length = strlen(key);
    switch (security) {
    case CP_WIFI_SECURITY_OPEN: return length == 0;
    case CP_WIFI_SECURITY_WEP: return length == 5 || length == 13 || ((length == 10 || length == 26) && AllHex(key));
    case CP_WIFI_SECURITY_WPA2: return length >= 8 && length <= CP_WIFI_PASSPHRASE_MAX;
    default: return 0;
    }
}

int CpWifi_KeyBytes(unsigned security, const char *key, unsigned char out[CP_WIFI_KEY_MAX])
{
    size_t length, i;
    if (!CpWifi_KeyValid(security, key)) return -1;
    length = strlen(key);
    if (security == CP_WIFI_SECURITY_WEP && (length == 10 || length == 26)) {
        for (i = 0; i < length / 2; ++i) out[i] = (unsigned char)(Hex(key[2 * i]) << 4 | Hex(key[2 * i + 1]));
        return (int)(length / 2);
    }
    if (length) memcpy(out, key, length);
    return (int)length;
}

enum CpWifiFailure CpWifi_Failure(int notFound, unsigned furthestStage)
{
    if (notFound) return CP_WIFI_FAILURE_NOT_FOUND;
    switch (furthestStage) {
    case CP_WIFI_STAGE_AUTHENTICATING:
    case CP_WIFI_STAGE_ASSOCIATING: return CP_WIFI_FAILURE_REJECTED;
    case CP_WIFI_STAGE_DHCP:
    case CP_WIFI_STAGE_ASSOCIATED: return CP_WIFI_FAILURE_NO_ADDRESS;
    default: return CP_WIFI_FAILURE_TIMEOUT;
    }
}
