#ifndef COMPANION_PLATFORM_NDS_WIFI_H
#define COMPANION_PLATFORM_NDS_WIFI_H

#include <stddef.h>
#include <stdint.h>

/* Wi-Fi bring-up with DSWiFi. In DSi mode this uses the DSi's own Wi-Fi
 * (WPA2 capable); in DS mode DSWiFi falls back to the DS driver (open and WEP
 * only). No UI: the session model and its Wi-Fi settings (LUNE-875) drive
 * this through app_platform.c. */

#include "platform/wifi_status.h"

/* Start the driver and enable the radio. Zero on success. The caller has
 * already started libnds's systemCounter, which times connections. */
int CpWifi_Init(void);
/* Nonzero when DSWiFi runs the DSi driver: WIFI_ATTEMPT_DSI_MODE falls back
 * to the DS driver only outside DSi mode. */
int CpWifi_DsiMode(void);
/* The console's MAC address, valid after CpWifi_Init. */
void CpWifi_Mac(unsigned char mac[6]);

/* Connect with the console's saved connections (Wifi_AutoConnect). On a DSi or
 * 3DS in DSi mode this includes the WPA2 slots. Poll CpWifi_Status. */
int CpWifi_AutoConnect(void);
/* Connect to a network by name with a typed passphrase (an empty one for an
 * open network). The network must be in range; it is looked up by a scan of
 * up to scanMs milliseconds. */
int CpWifi_ConnectPassphrase(const char *ssid, const char *passphrase, unsigned scanMs);
/* The same with the key's bytes (Wifi_ConnectSecureAP): a WEP key as 5 or
 * 13 bytes, a WPA2 passphrase (always run through PBKDF2 by the driver),
 * none for an open network.
 * Of several access points with the name, the strongest is joined. */
int CpWifi_ConnectKey(const char *ssid, const unsigned char *key, size_t keyLength, unsigned scanMs);

/* List the Internet access points in range (Wifi_ScanMode), ending any
 * connection attempt. CpWifi_Networks then reads what was found so far. */
int CpWifi_Scan(void);
unsigned CpWifi_Networks(CpWifiNetwork *out, unsigned max);
/* Why the last connection attempt failed (enum CpWifiFailure): the network
 * was not found, or how far it got. */
int CpWifi_LastFailure(void);

/* Poll the connection (CpWifi_Decide); a connection without an IPv4 address
 * after the timeout given to CpWifi_SetTimeout (45 s by default) counts as
 * failed. */
enum CpWifiStatus CpWifi_Status(void);
void CpWifi_SetTimeout(unsigned ms);
/* DSWiFi's association state string, for diagnostics. */
const char *CpWifi_StatusText(void);
/* The IPv4 address in network order, 0 while there is no usable one. */
uint32_t CpWifi_Address(void);
void CpWifi_Disconnect(void);

#endif
