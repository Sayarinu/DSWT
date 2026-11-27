#include "platform/wifi_config.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LINE_MAX_BYTES 512
#define TMP_SUFFIX ".tmp"

static const char *const kSecurity[] = { "open", "wep", "wpa2" };

void CpWifiConfig_Wipe(void *bytes, size_t size)
{
    volatile unsigned char *p = bytes;
    while (size--) *p++ = 0;
}

static int HexDigit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Undo the escapes into out (capacity bytes plus the NUL); -1 when malformed,
 * too long or holding a NUL. */
static int Unescape(const char *value, size_t length, char *out, size_t capacity)
{
    size_t i, used = 0;
    for (i = 0; i < length; ++i) {
        int c = (unsigned char)value[i];
        if (c == '\\') {
            if (i + 1 < length && value[i + 1] == '\\') {
                c = '\\';
                i += 1;
            } else if (i + 3 < length && value[i + 1] == 'x' && HexDigit((unsigned char)value[i + 2]) >= 0
                && HexDigit((unsigned char)value[i + 3]) >= 0) {
                c = HexDigit((unsigned char)value[i + 2]) << 4 | HexDigit((unsigned char)value[i + 3]);
                i += 3;
            } else {
                return -1;
            }
        } else if (c < 0x20 || c == 0x7F) {
            return -1;
        }
        if (c == 0 || used >= capacity) return -1;
        out[used++] = (char)c;
    }
    out[used] = 0;
    return (int)used;
}

static size_t Escape(const char *value, char *out, size_t size)
{
    static const char digits[] = "0123456789ABCDEF";
    size_t used = 0;
    for (; *value; ++value) {
        unsigned char c = (unsigned char)*value;
        if (c == '\\') {
            if (used + 2 >= size) return 0;
            out[used++] = '\\';
            out[used++] = '\\';
        } else if (c < 0x20 || c == 0x7F) {
            if (used + 4 >= size) return 0;
            out[used++] = '\\';
            out[used++] = 'x';
            out[used++] = digits[c >> 4];
            out[used++] = digits[c & 15];
        } else {
            if (used + 1 >= size) return 0;
            out[used++] = (char)c;
        }
    }
    out[used] = 0;
    return used;
}

int CpWifiConfig_Parse(const char *text, size_t size, CpWifiConfig *out)
{
    size_t start = 0;
    int haveVersion = 0, haveSecurity = 0, haveSsid = 0, haveKey = 0;
    CpWifiConfig config;
    memset(&config, 0, sizeof(config));
    if (!text || size > CP_WIFI_CONFIG_FILE_MAX || memchr(text, 0, size)) return -1;
    while (start < size) {
        const char *line = text + start, *newline = memchr(line, '\n', size - start), *equals;
        size_t length = newline ? (size_t)(newline - line) : size - start, nameLength;
        start += length + (newline ? 1 : 0);
        if (length && line[length - 1] == '\r') length--;
        if (length > LINE_MAX_BYTES) goto bad;
        if (!length || line[0] == '#') continue;
        equals = memchr(line, '=', length);
        if (!equals) goto bad;
        nameLength = (size_t)(equals - line);
        {
            const char *value = equals + 1;
            size_t valueLength = length - nameLength - 1;
#define NAME(literal) (nameLength == sizeof(literal) - 1 && !memcmp(line, literal, nameLength))
            if (NAME("version")) {
                if (haveVersion++ || valueLength != 1 || value[0] != '1') goto bad;
            } else if (NAME("security")) {
                unsigned i;
                if (haveSecurity++) goto bad;
                for (i = 0; i < sizeof(kSecurity) / sizeof(*kSecurity); ++i)
                    if (strlen(kSecurity[i]) == valueLength && !memcmp(value, kSecurity[i], valueLength)) break;
                if (i == sizeof(kSecurity) / sizeof(*kSecurity)) goto bad;
                config.security = i == 0 ? CP_WIFI_SECURITY_OPEN : i == 1 ? CP_WIFI_SECURITY_WEP : CP_WIFI_SECURITY_WPA2;
            } else if (NAME("ssid")) {
                if (haveSsid++ || Unescape(value, valueLength, config.ssid, CP_WIFI_SSID_MAX) <= 0) goto bad;
            } else if (NAME("key")) {
                if (haveKey++ || Unescape(value, valueLength, config.key, CP_WIFI_KEY_MAX) < 0) goto bad;
            }
#undef NAME
        }
    }
    if (!haveVersion || !haveSecurity || !haveSsid) goto bad;
    if (!CpWifi_KeyValid(config.security, config.key)) goto bad;
    *out = config;
    CpWifiConfig_Wipe(&config, sizeof(config));
    return 0;
bad:
    CpWifiConfig_Wipe(&config, sizeof(config));
    return -1;
}

size_t CpWifiConfig_Format(const CpWifiConfig *config, char *out, size_t size)
{
    char ssid[CP_WIFI_SSID_MAX * 4 + 1], key[CP_WIFI_KEY_MAX * 4 + 1];
    const char *security;
    int written;
    if (!config->ssid[0] || strlen(config->ssid) > CP_WIFI_SSID_MAX || !CpWifi_KeyValid(config->security, config->key))
        return 0;
    security = config->security == CP_WIFI_SECURITY_OPEN ? kSecurity[0]
        : config->security == CP_WIFI_SECURITY_WEP ? kSecurity[1] : kSecurity[2];
    if (!Escape(config->ssid, ssid, sizeof(ssid))) return 0;
    Escape(config->key, key, sizeof(key));
    written = snprintf(out, size,
        "# DSWT: the Wi-Fi network chosen in its Wi-Fi settings.\n"
        "# The password is stored here as plain text. Choose Forget network\n"
        "# in the companion, or delete this file, to remove it.\n"
        "version=1\nsecurity=%s\nssid=%s\nkey=%s\n", security, ssid, key);
    CpWifiConfig_Wipe(key, sizeof(key));
    if (written < 0 || (size_t)written >= size || (size_t)written > CP_WIFI_CONFIG_FILE_MAX) {
        CpWifiConfig_Wipe(out, size);
        return 0;
    }
    return (size_t)written;
}

static int ReadAll(const char *path, char *buffer, size_t capacity, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) return 1;
    *size = fread(buffer, 1, capacity, file);
    if (ferror(file)) { fclose(file); return -1; }
    fclose(file);
    return 0;
}

static void TmpPath(const char *path, char *out, size_t size)
{
    snprintf(out, size, "%s" TMP_SUFFIX, path);
}

int CpWifiConfig_Load(const char *path, CpWifiConfig *out)
{
    char buffer[CP_WIFI_CONFIG_FILE_MAX + 1], tmp[512];
    size_t size = 0;
    int result = ReadAll(path, buffer, sizeof(buffer), &size);
    if (result == 1) {
        /* A save cut off between its rename steps leaves only the .tmp. */
        TmpPath(path, tmp, sizeof(tmp));
        result = ReadAll(tmp, buffer, sizeof(buffer), &size);
        if (result == 1) return 1;
    }
    if (result == 0) result = CpWifiConfig_Parse(buffer, size, out) == 0 ? 0 : -1;
    CpWifiConfig_Wipe(buffer, sizeof(buffer));
    return result;
}

int CpWifiConfig_Save(const char *path, const CpWifiConfig *config)
{
    char buffer[CP_WIFI_CONFIG_FILE_MAX + 1], tmp[512];
    size_t size = CpWifiConfig_Format(config, buffer, sizeof(buffer));
    FILE *file;
    int ok;
    if (!size) return -1;
    TmpPath(path, tmp, sizeof(tmp));
    file = fopen(tmp, "wb");
    if (!file) { CpWifiConfig_Wipe(buffer, sizeof(buffer)); return -1; }
    ok = fwrite(buffer, 1, size, file) == size && fflush(file) == 0 && fsync(fileno(file)) == 0;
    ok = fclose(file) == 0 && ok;
    CpWifiConfig_Wipe(buffer, sizeof(buffer));
    if (!ok) { remove(tmp); return -1; }
    /* FAT does not rename over an existing file: remove it first. Until the
     * rename lands, Load reads the .tmp. */
    if (rename(tmp, path) != 0 && (remove(path), rename(tmp, path) != 0)) {
        remove(tmp);
        return -1;
    }
    return 0;
}

int CpWifiConfig_Forget(const char *path)
{
    char tmp[512];
    FILE *file;
    TmpPath(path, tmp, sizeof(tmp));
    remove(tmp);
    remove(path);
    file = fopen(path, "rb");
    if (file) { fclose(file); return -1; }
    return 0;
}
