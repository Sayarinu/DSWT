/* The Wi-Fi status decision: connected only with a usable IPv4 address. */
#include "platform/wifi_status.h"

#include <assert.h>
#include <stdio.h>

static enum CpWifiStatus Decide(int pending, int associated, int cannotConnect, uint32_t ipv4, uint32_t elapsedMs)
{
    CpWifiPoll poll = { pending, associated, cannotConnect, ipv4, elapsedMs, 45000 };
    return CpWifi_Decide(&poll);
}

int main(void)
{
    const uint32_t ip = 0x1000400Au; /* 10.64.0.16 in network order */

    assert(Decide(1, 0, 0, 0, 100) == CP_WIFI_CONNECTING);
    assert(Decide(1, 1, 0, ip, 100) == CP_WIFI_CONNECTED);
    /* DSWiFi says associated once IPv6 is ready: without IPv4, keep waiting. */
    assert(Decide(1, 1, 0, 0, 100) == CP_WIFI_CONNECTING);
    assert(Decide(1, 1, 0, 0xFFFFFFFFu, 100) == CP_WIFI_CONNECTING);
    /* ...but only within the timeout. */
    assert(Decide(1, 1, 0, 0, 45000) == CP_WIFI_CONNECTING);
    assert(Decide(1, 1, 0, 0, 45001) == CP_WIFI_FAILED);
    assert(Decide(1, 0, 0, 0, 45001) == CP_WIFI_FAILED);
    assert(Decide(1, 0, 1, 0, 100) == CP_WIFI_FAILED);
    /* A usable address wins even at the deadline. */
    assert(Decide(1, 1, 0, ip, 50000) == CP_WIFI_CONNECTED);
    /* No attempt under way. */
    assert(Decide(0, 1, 0, ip, 0) == CP_WIFI_CONNECTED);
    assert(Decide(0, 1, 0, 0, 0) == CP_WIFI_IDLE);
    assert(Decide(0, 0, 0, 0, 0) == CP_WIFI_IDLE);

    assert(!CpWifi_UsableIPv4(0) && !CpWifi_UsableIPv4(0xFFFFFFFFu) && CpWifi_UsableIPv4(ip));
    printf("wifi_status_test: ok\n");
    return 0;
}
