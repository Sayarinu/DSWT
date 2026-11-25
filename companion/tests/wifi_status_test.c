/* The Wi-Fi status decision: connected only with a usable IPv4 address. */
#include "platform/wifi_status.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

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

    /* LUNE-875: what the scan reports, as the setup screen lists it. DSWiFi's
     * security types are 0 open, 1 WEP, 2 WPA (TKIP only), 3 WPA2. */
    assert(CpWifi_Security(0, 0) == CP_WIFI_SECURITY_OPEN);
    assert(CpWifi_Security(1, 0) == CP_WIFI_SECURITY_WEP);
    assert(CpWifi_Security(2, 0) == CP_WIFI_SECURITY_WPA);
    assert(CpWifi_Security(3, 0) == CP_WIFI_SECURITY_WPA2);
    assert(CpWifi_Security(3, 1) == CP_WIFI_SECURITY_ENTERPRISE && CpWifi_Security(2, 1) == CP_WIFI_SECURITY_ENTERPRISE);
    assert(CpWifi_Unsupported(CP_WIFI_SECURITY_OPEN, 0) == CP_WIFI_SUPPORTED);
    assert(CpWifi_Unsupported(CP_WIFI_SECURITY_WEP, 0) == CP_WIFI_SUPPORTED);
    assert(CpWifi_Unsupported(CP_WIFI_SECURITY_WPA2, 1) == CP_WIFI_SUPPORTED);
    assert(CpWifi_Unsupported(CP_WIFI_SECURITY_WPA2, 0) == CP_WIFI_UNSUPPORTED_NEEDS_DSI);
    assert(CpWifi_Unsupported(CP_WIFI_SECURITY_WPA, 1) == CP_WIFI_UNSUPPORTED_TKIP);
    assert(CpWifi_Unsupported(CP_WIFI_SECURITY_ENTERPRISE, 1) == CP_WIFI_UNSUPPORTED_ENTERPRISE);
    /* Signal: dBm in DSi mode, the DS driver's 0-255 otherwise; 0 is stale. */
    assert(CpWifi_Bars(-40, 1) == 3 && CpWifi_Bars(-65, 1) == 2 && CpWifi_Bars(-75, 1) == 1 && CpWifi_Bars(-90, 1) == 0);
    assert(CpWifi_Bars(0, 1) == 0 && CpWifi_Bars(0, 0) == 0);
    assert(CpWifi_Bars(60, 0) == 3 && CpWifi_Bars(30, 0) == 2 && CpWifi_Bars(15, 0) == 1 && CpWifi_Bars(5, 0) == 0);

    /* Keys: WPA2 8-63 printable characters; WEP 5/13
     * characters or 10/26 hex digits; open none. */
    {
        unsigned char bytes[CP_WIFI_KEY_MAX];
        char longest[65];
        memset(longest, 'x', 63);
        longest[63] = 0;
        assert(!CpWifi_KeyValid(CP_WIFI_SECURITY_WPA2, "1234567") && CpWifi_KeyValid(CP_WIFI_SECURITY_WPA2, "12345678"));
        assert(CpWifi_KeyValid(CP_WIFI_SECURITY_WPA2, longest));
        longest[63] = 'x';
        longest[64] = 0;
        assert(!CpWifi_KeyValid(CP_WIFI_SECURITY_WPA2, longest));
        /* DSWiFi runs PBKDF2 on whatever it is given (wpa_calc_pmk), so a raw
         * 64-hex-digit key could never authenticate: refused. */
        memset(longest, 'A', 64);
        assert(!CpWifi_KeyValid(CP_WIFI_SECURITY_WPA2, longest));
        assert(CpWifi_KeyBytes(CP_WIFI_SECURITY_WPA2, longest, bytes) == -1);
        longest[63] = 0;
        assert(CpWifi_KeyBytes(CP_WIFI_SECURITY_WPA2, longest, bytes) == 63 && bytes[0] == 'A');
        assert(CpWifi_KeyValid(CP_WIFI_SECURITY_WPA2, "with space ~!"));
        assert(!CpWifi_KeyValid(CP_WIFI_SECURITY_WPA2, "caf\xc3\xa9-pass") && !CpWifi_KeyValid(CP_WIFI_SECURITY_WPA2, "tab\tpass"));
        assert(CpWifi_KeyValid(CP_WIFI_SECURITY_WEP, "abcde") && CpWifi_KeyValid(CP_WIFI_SECURITY_WEP, "abcdefghijklm"));
        assert(!CpWifi_KeyValid(CP_WIFI_SECURITY_WEP, "abcdef") && !CpWifi_KeyValid(CP_WIFI_SECURITY_WEP, "abcdefghij"));
        assert(CpWifi_KeyBytes(CP_WIFI_SECURITY_WEP, "0aFf1B2c3D", bytes) == 5);
        assert(bytes[0] == 0x0A && bytes[1] == 0xFF && bytes[2] == 0x1B && bytes[3] == 0x2C && bytes[4] == 0x3D);
        assert(CpWifi_KeyBytes(CP_WIFI_SECURITY_WEP, "00112233445566778899aabbcc", bytes) == 13 && bytes[12] == 0xCC);
        assert(CpWifi_KeyBytes(CP_WIFI_SECURITY_WEP, "hello", bytes) == 5 && !memcmp(bytes, "hello", 5));
        assert(CpWifi_KeyValid(CP_WIFI_SECURITY_OPEN, "") && !CpWifi_KeyValid(CP_WIFI_SECURITY_OPEN, "x"));
        assert(CpWifi_KeyBytes(CP_WIFI_SECURITY_OPEN, "", bytes) == 0);
        assert(!CpWifi_KeyValid(CP_WIFI_SECURITY_WPA, "12345678") && !CpWifi_KeyValid(CP_WIFI_SECURITY_ENTERPRISE, "12345678"));
        assert(CpWifi_KeyBytes(CP_WIFI_SECURITY_WPA2, "short", bytes) == -1 && !CpWifi_KeyValid(CP_WIFI_SECURITY_WPA2, NULL));
    }

    /* Why a join failed, from how far it got. */
    assert(CpWifi_Failure(1, CP_WIFI_STAGE_DHCP) == CP_WIFI_FAILURE_NOT_FOUND);
    assert(CpWifi_Failure(0, CP_WIFI_STAGE_NONE) == CP_WIFI_FAILURE_TIMEOUT);
    assert(CpWifi_Failure(0, CP_WIFI_STAGE_SEARCHING) == CP_WIFI_FAILURE_TIMEOUT);
    assert(CpWifi_Failure(0, CP_WIFI_STAGE_AUTHENTICATING) == CP_WIFI_FAILURE_REJECTED);
    assert(CpWifi_Failure(0, CP_WIFI_STAGE_ASSOCIATING) == CP_WIFI_FAILURE_REJECTED);
    assert(CpWifi_Failure(0, CP_WIFI_STAGE_DHCP) == CP_WIFI_FAILURE_NO_ADDRESS);
    assert(CpWifi_Failure(0, CP_WIFI_STAGE_ASSOCIATED) == CP_WIFI_FAILURE_NO_ADDRESS);
    printf("wifi_status_test: ok\n");
    return 0;
}
