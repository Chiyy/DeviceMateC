#include "dm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "iphlpapi.lib")
#else
    #include <unistd.h>
    #include <sys/socket.h>
    #include <sys/ioctl.h>
    #include <net/if.h>
    #include <arpa/inet.h>
    #include <strings.h>
    #include <ifaddrs.h>
    #include <netdb.h>
    #ifdef __linux__
        #include <linux/if.h>
        #include <linux/if_packet.h>
    #endif
    #ifdef __APPLE__
        #include <net/if_dl.h>
    #endif
#endif

/* ====== 动态字符串 ====== */

void ds_init(dstring *ds) {
    ds->cap = 256;
    ds->len = 0;
    ds->data = (char *)malloc(ds->cap);
    if (ds->data) ds->data[0] = '\0';
}

void ds_grow(dstring *ds, size_t need) {
    if (ds->len + need + 1 <= ds->cap) return;
    while (ds->len + need + 1 > ds->cap) ds->cap *= 2;
    char *p = (char *)realloc(ds->data, ds->cap);
    if (p) ds->data = p;
}

void ds_append(dstring *ds, const char *str) {
    if (!ds->data) return;
    size_t n = strlen(str);
    ds_grow(ds, n);
    if (!ds->data) return;
    memcpy(ds->data + ds->len, str, n);
    ds->len += n;
    ds->data[ds->len] = '\0';
}

void ds_append_char(dstring *ds, char c) {
    if (!ds->data) return;
    ds_grow(ds, 1);
    if (!ds->data) return;
    ds->data[ds->len++] = c;
    ds->data[ds->len] = '\0';
}

void ds_appendf(dstring *ds, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) ds_append(ds, buf);
}

void ds_free(dstring *ds) {
    free(ds->data);
    ds->data = NULL;
    ds->len = ds->cap = 0;
}

/* ====== 字符串工具 ====== */

char *str_trim(char *s) {
    if (!s) return s;
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
    if (*start == '\0') {
        s[0] = '\0';
        return s;
    }
    char *end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) end--;
    *(end + 1) = '\0';
    /* 用 memmove 把 trim 后的内容移到 buffer 起始, 保证返回的指针
       始终是 malloc 的起始地址, 可被 free() (修复 free(): invalid pointer) */
    if (start != s) {
        size_t len = (size_t)(end - start) + 1;
        memmove(s, start, len + 1);
    }
    return s;
}

char *str_join(char **items, int count, const char *sep) {
    if (count <= 0) return NULL;
    dstring ds;
    ds_init(&ds);
    for (int i = 0; i < count; i++) {
        char *t = str_trim(items[i]);
        if (t[0] == '\0') continue;
        if (ds.len > 0) ds_append(&ds, sep);
        ds_append(&ds, t);
    }
    if (ds.len == 0) {
        ds_free(&ds);
        return strdup("未检测到");
    }
    return ds.data;
}

int str_contains(const char *s, const char *sub) {
    return strstr(s, sub) != NULL;
}

int str_eq_ignore_case(const char *a, const char *b) {
#ifdef _WIN32
    return _stricmp(a, b) == 0;
#else
    return strcasecmp(a, b) == 0;
#endif
}

int str_starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* ====== 序列号校验 ====== */

int is_invalid_serial(const char *s) {
    if (!s || s[0] == '\0') return 1;
    char *t = str_trim(strdup(s));
    int invalid = 0;
    if (strcmp(t, "0") == 0 ||
        str_eq_ignore_case(t, "none") ||
        str_eq_ignore_case(t, "null") ||
        str_eq_ignore_case(t, "default") ||
        str_eq_ignore_case(t, "unknown") ||
        str_eq_ignore_case(t, "unspecified") ||
        str_eq_ignore_case(t, "to be filled by o.e.m.") ||
        str_eq_ignore_case(t, "default string") ||
        str_eq_ignore_case(t, "not specified") ||
        str_eq_ignore_case(t, "not present")) {
        invalid = 1;
    }
    free(t);
    return invalid;
}

/* ====== 命令执行 ====== */

#ifdef _WIN32
/* Windows: 使用 CreateProcess 替代 popen, 避免控制台窗口闪烁 */

/* 检测字符串是否为有效 UTF-8 (用于判断是否需要从 ANSI 转换) */
static int is_valid_utf8(const char *s, size_t len) {
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < len) {
        if (p[i] < 0x80) {
            i++;
        } else if ((p[i] & 0xE0) == 0xC0) {
            if (i + 1 >= len || (p[i + 1] & 0xC0) != 0x80) return 0;
            i += 2;
        } else if ((p[i] & 0xF0) == 0xE0) {
            if (i + 2 >= len || (p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80) return 0;
            i += 3;
        } else if ((p[i] & 0xF8) == 0xF0) {
            if (i + 3 >= len || (p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80 || (p[i + 3] & 0xC0) != 0x80) return 0;
            i += 4;
        } else {
            return 0;
        }
    }
    return 1;
}

/* 将 ANSI (CP_ACP) 字符串转换为 UTF-8, 返回堆分配的新字符串 */
static char *ansi_to_utf8(const char *ansi, size_t len) {
    int wide_len = MultiByteToWideChar(CP_ACP, 0, ansi, (int)len, NULL, 0);
    if (wide_len <= 0) return NULL;
    wchar_t *wide = (wchar_t *)malloc((wide_len + 1) * sizeof(wchar_t));
    if (!wide) return NULL;
    MultiByteToWideChar(CP_ACP, 0, ansi, (int)len, wide, wide_len);
    wide[wide_len] = 0;
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide, wide_len, NULL, 0, NULL, NULL);
    if (utf8_len <= 0) { free(wide); return NULL; }
    char *utf8 = (char *)malloc(utf8_len + 1);
    if (!utf8) { free(wide); return NULL; }
    WideCharToMultiByte(CP_UTF8, 0, wide, wide_len, utf8, utf8_len, NULL, NULL);
    utf8[utf8_len] = '\0';
    free(wide);
    return utf8;
}

char *run_cmd_timeout(int timeout_sec, const char *cmd) {
    (void)timeout_sec;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE pipe_read, pipe_write;
    if (!CreatePipe(&pipe_read, &pipe_write, &sa, 0)) {
        return strdup("");
    }
    SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = pipe_write;
    si.hStdError = pipe_write;
    si.hStdInput = NULL;

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    /* 构建命令行: cmd /c <command> */
    char cmdline[8192];
    snprintf(cmdline, sizeof(cmdline), "cmd /c %s", cmd);

    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    /* 必须关闭写端, 否则 ReadFile 会阻塞 */
    CloseHandle(pipe_write);

    if (!ok) {
        CloseHandle(pipe_read);
        return strdup("");
    }

    /* 读取输出 (使用 raw buffer 避免 \0 截断) */
    char *data = NULL;
    size_t data_len = 0, data_cap = 0;
    char buf[4096];
    DWORD n;
    while (ReadFile(pipe_read, buf, sizeof(buf), &n, NULL) && n > 0) {
        if (data_len + n + 1 > data_cap) {
            while (data_len + n + 1 > data_cap) data_cap = data_cap ? data_cap * 2 : 4096;
            char *p = (char *)realloc(data, data_cap);
            if (!p) break;
            data = p;
        }
        memcpy(data + data_len, buf, n);
        data_len += n;
    }
    CloseHandle(pipe_read);
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (!data) return strdup("");
    data[data_len] = '\0';

    /* 去除尾部换行 */
    while (data_len > 0 && (data[data_len - 1] == '\n' || data[data_len - 1] == '\r')) {
        data[--data_len] = '\0';
    }

    if (data_len == 0) {
        free(data);
        return strdup("");
    }

    /* 编码转换: 如果不是有效 UTF-8, 从 ANSI 转换为 UTF-8
       (wmic/reg 输出为 GBK, PowerShell 已设 UTF-8) */
    if (is_valid_utf8(data, data_len)) {
        return data;  /* 已是 UTF-8 */
    }
    char *utf8 = ansi_to_utf8(data, data_len);
    free(data);
    return utf8 ? utf8 : strdup("");
}

#else  /* 非 Windows: 使用 popen */

char *run_cmd_timeout(int timeout_sec, const char *cmd) {
    (void)timeout_sec;
    FILE *fp = popen(cmd, "r");
    if (!fp) return strdup("");
    dstring ds;
    ds_init(&ds);
    if (!ds.data) {
        pclose(fp);
        return strdup("");
    }
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) {
        ds_append(&ds, buf);
    }
    pclose(fp);
    /* 去除尾部换行 */
    if (ds.len > 0 && ds.data[ds.len - 1] == '\n') {
        ds.data[--ds.len] = '\0';
    }
    if (ds.len > 0 && ds.data[ds.len - 1] == '\r') {
        ds.data[--ds.len] = '\0';
    }
    if (ds.len == 0) {
        ds_free(&ds);
        return strdup("");
    }
    return ds.data;
}

#endif /* _WIN32 */

char *run_cmd(const char *cmd) {
    return run_cmd_timeout(15, cmd);
}

char *run_cmd_args(const char *prog, const char *const *args, int n_args) {
    /* 拼接命令字符串 */
    dstring ds;
    ds_init(&ds);
    /* 处理 prog 中可能包含的路径 */
    ds_append(&ds, prog);
    for (int i = 0; i < n_args; i++) {
        ds_append_char(&ds, ' ');
        ds_append(&ds, args[i]);
    }
    char *result = run_cmd(ds.data);
    ds_free(&ds);
    return result;
}

/* ====== sudo 密码支持 ====== */

/* 全局变量: sudo 密码 (由 /api/sudo 接口设置) */
char g_sudo_password[256] = {0};

/* 构建带 sudo 密码的命令
   有密码: "echo '<escaped>' | sudo -S <cmd> 2>/dev/null"
   无密码: 返回 strdup(cmd) */
char *build_sudo_cmd(const char *cmd) {
    if (g_sudo_password[0] == '\0') {
        return strdup(cmd);
    }
    /* 转义密码中的单引号: ' -> '\'' */
    dstring escaped;
    ds_init(&escaped);
    ds_append_char(&escaped, '\'');
    for (const char *p = g_sudo_password; *p; p++) {
        if (*p == '\'') {
            ds_append(&escaped, "'\\''");
        } else {
            ds_append_char(&escaped, *p);
        }
    }
    ds_append_char(&escaped, '\'');

    size_t need = strlen(cmd) + escaped.len + 32;
    char *result = (char *)malloc(need);
    snprintf(result, need,
             "echo %s | sudo -S %s 2>/dev/null", escaped.data, cmd);
    ds_free(&escaped);
    return result;
}

/* ====== IP 地址获取 ====== */

char *get_ip_addresses(void) {
    dstring ds;
    ds_init(&ds);
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    char host[256];
    if (gethostname(host, sizeof(host)) == SOCKET_ERROR) {
        WSACleanup();
        ds_free(&ds);
        return strdup("未检测到有效IP地址");
    }

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &res) == 0) {
        struct addrinfo *p;
        for (p = res; p; p = p->ai_next) {
            struct sockaddr_in *sin = (struct sockaddr_in *)p->ai_addr;
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
            if (strcmp(ip, "127.0.0.1") != 0) {
                if (ds.len > 0) ds_append_char(&ds, ';');
                ds_append(&ds, ip);
            }
        }
        freeaddrinfo(res);
    }
    WSACleanup();
#else
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == 0) {
        for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr) continue;
            if (ifa->ifa_addr->sa_family == AF_INET) {
                struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
                if (strcmp(ip, "127.0.0.1") != 0) {
                    if (ds.len > 0) ds_append_char(&ds, ';');
                    ds_append(&ds, ip);
                }
            }
        }
        freeifaddrs(ifaddr);
    }
#endif
    if (ds.len == 0) {
        ds_free(&ds);
        return strdup("未检测到有效IP地址");
    }
    return ds.data;
}

/* ====== MAC 地址获取 ====== */

#ifdef _WIN32
#include <iphlpapi.h>

char *get_mac_addresses(void) {
    dstring ds;
    ds_init(&ds);
    ULONG outBufLen = 0;
    GetAdaptersAddresses(AF_UNSPEC, 0, NULL, NULL, &outBufLen);
    PIP_ADAPTER_ADDRESSES adapters = (PIP_ADAPTER_ADDRESSES)malloc(outBufLen);
    DWORD ret = GetAdaptersAddresses(AF_UNSPEC, 0, NULL, adapters, &outBufLen);
    if (ret == NO_ERROR) {
        PIP_ADAPTER_ADDRESSES p;
        for (p = adapters; p; p = p->Next) {
            if (p->PhysicalAddressLength == 6) {
                char mac[32];
                snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                    p->PhysicalAddress[0], p->PhysicalAddress[1],
                    p->PhysicalAddress[2], p->PhysicalAddress[3],
                    p->PhysicalAddress[4], p->PhysicalAddress[5]);
                /* 跳过全零 MAC */
                if (str_contains(mac, "00:00:00:00:00:00")) continue;
                if (ds.len > 0) ds_append_char(&ds, ';');
                ds_append(&ds, mac);
            }
        }
    }
    free(adapters);
    if (ds.len == 0) {
        ds_free(&ds);
        return strdup("未检测到有效MAC地址");
    }
    return ds.data;
}
#else
char *get_mac_addresses(void) {
    dstring ds;
    ds_init(&ds);
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == 0) {
        for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr) continue;
#ifdef __APPLE__
            if (ifa->ifa_addr->sa_family == AF_LINK) {
                struct sockaddr_dl *sdl = (struct sockaddr_dl *)ifa->ifa_addr;
                if (sdl->sdl_alen == 6) {
                    unsigned char *mac = (unsigned char *)LLADDR(sdl);
                    char macstr[32];
                    snprintf(macstr, sizeof(macstr), "%02X:%02X:%02X:%02X:%02X:%02X",
                        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                    int all_zero = 1;
                    for (int i = 0; i < 6; i++) {
                        if (mac[i] != 0) { all_zero = 0; break; }
                    }
                    if (!all_zero) {
                        if (ds.len > 0) ds_append_char(&ds, ';');
                        ds_append(&ds, macstr);
                    }
                }
            }
#else
            if (ifa->ifa_addr->sa_family == AF_PACKET) {
                struct sockaddr_ll *sll = (struct sockaddr_ll *)ifa->ifa_addr;
                if (sll->sll_halen == 6) {
                    unsigned char *mac = sll->sll_addr;
                    char macstr[32];
                    snprintf(macstr, sizeof(macstr), "%02X:%02X:%02X:%02X:%02X:%02X",
                        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                    int all_zero = 1;
                    for (int i = 0; i < 6; i++) {
                        if (mac[i] != 0) { all_zero = 0; break; }
                    }
                    if (!all_zero) {
                        if (ds.len > 0) ds_append_char(&ds, ';');
                        ds_append(&ds, macstr);
                    }
                }
            }
#endif
        }
        freeifaddrs(ifaddr);
    }
    if (ds.len == 0) {
        ds_free(&ds);
        return strdup("未检测到有效MAC地址");
    }
    return ds.data;
}
#endif

/* ====== 时间 ====== */

void get_current_time(char *buf, int bufsize) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(buf, bufsize, "%Y-%m-%d %H:%M:%S", tm);
}

/* ====== JSON 序列化 ====== */

static void json_escape(dstring *ds, const char *s) {
    if (!s) s = "";
    ds_append_char(ds, '"');
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '"':  ds_append(ds, "\\\""); break;
            case '\\': ds_append(ds, "\\\\"); break;
            case '\n': ds_append(ds, "\\n"); break;
            case '\r': ds_append(ds, "\\r"); break;
            case '\t': ds_append(ds, "\\t"); break;
            default:
                if ((unsigned char)*p < 0x20) {
                    ds_appendf(ds, "\\u%04x", (unsigned char)*p);
                } else {
                    ds_append_char(ds, *p);
                }
        }
    }
    ds_append_char(ds, '"');
}

char *info_to_json(const DeviceInfo *info) {
    dstring ds;
    ds_init(&ds);
    ds_append_char(&ds, '{');
    ds_append(&ds, "\"os_name\":");        json_escape(&ds, info->os_name);
    ds_append(&ds, ",\"os_version\":");     json_escape(&ds, info->os_version);
    ds_append(&ds, ",\"os_install_date\":");json_escape(&ds, info->os_install_date);
    ds_append(&ds, ",\"device_serial\":");  json_escape(&ds, info->device_serial);
    ds_append(&ds, ",\"hdd_serials\":");     json_escape(&ds, info->hdd_serials);
    ds_append(&ds, ",\"ip_addresses\":");   json_escape(&ds, info->ip_addresses);
    ds_append(&ds, ",\"mac_addresses\":");  json_escape(&ds, info->mac_addresses);
    ds_append(&ds, ",\"usb_devices\":[");
    for (int i = 0; i < info->usb_count; i++) {
        if (i > 0) ds_append_char(&ds, ',');
        ds_append_char(&ds, '{');
        ds_append(&ds, "\"name\":");   json_escape(&ds, info->usb_devices[i].name);
        ds_append(&ds, ",\"model\":"); json_escape(&ds, info->usb_devices[i].model);
        ds_append(&ds, ",\"serial\":");json_escape(&ds, info->usb_devices[i].serial);
        ds_append_char(&ds, '}');
    }
    ds_append(&ds, "],\"collect_time\":");
    json_escape(&ds, info->collect_time);
    ds_append_char(&ds, '}');
    return ds.data;
}

/* ====== 采集入口 ====== */

DeviceInfo collect_all(void) {
    DeviceInfo info;
    memset(&info, 0, sizeof(info));

    info.os_name        = strdup(PLATFORM_NAME);
    info.os_version     = get_os_version();
    info.os_install_date= get_os_install_date();
    info.device_serial  = get_device_serial();
    info.hdd_serials    = get_hdd_serials();
    info.ip_addresses   = get_ip_addresses();
    info.mac_addresses  = get_mac_addresses();
    get_usb_devices(&info.usb_devices, &info.usb_count);
    get_current_time(info.collect_time, sizeof(info.collect_time));

    return info;
}

void free_info(DeviceInfo *info) {
    if (!info) return;
    free(info->os_name);
    free(info->os_version);
    free(info->os_install_date);
    free(info->device_serial);
    free(info->hdd_serials);
    free(info->ip_addresses);
    free(info->mac_addresses);
    for (int i = 0; i < info->usb_count; i++) {
        free(info->usb_devices[i].name);
        free(info->usb_devices[i].model);
        free(info->usb_devices[i].serial);
    }
    free(info->usb_devices);
    memset(info, 0, sizeof(*info));
}
