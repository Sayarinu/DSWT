#include "platform/wifi_status.h"

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
