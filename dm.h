#ifndef DM_H
#define DM_H

#include <stddef.h>

/* ====== 数据结构 ====== */

typedef struct {
    char *name;
    char *model;
    char *serial;
} USBDevice;

typedef struct {
    char *os_name;
    char *os_version;
    char *os_install_date;
    char *device_serial;
    char *hdd_serials;
    char *ip_addresses;
    char *mac_addresses;
    USBDevice *usb_devices;
    int usb_count;
    char collect_time[32];
} DeviceInfo;

/* ====== 跨平台采集 (collector.c) ====== */

DeviceInfo collect_all(void);
void free_info(DeviceInfo *info);

/* 跨平台获取 IP 地址 */
char *get_ip_addresses(void);

/* 跨平台获取 MAC 地址 */
char *get_mac_addresses(void);

/* 执行系统命令, 返回输出 (跨平台) */
char *run_cmd(const char *cmd);
char *run_cmd_args(const char *prog, const char *const *args, int n_args);
char *run_cmd_timeout(int timeout_sec, const char *cmd);

/* 字符串工具 */
char *str_trim(char *s);
char *str_join(char **items, int count, const char *sep);
int str_contains(const char *s, const char *sub);
int str_eq_ignore_case(const char *a, const char *b);
int str_starts_with(const char *s, const char *prefix);

/* 动态字符串 */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} dstring;

void ds_init(dstring *ds);
void ds_append(dstring *ds, const char *str);
void ds_append_char(dstring *ds, char c);
void ds_appendf(dstring *ds, const char *fmt, ...);
void ds_free(dstring *ds);

/* JSON 序列化 */
char *info_to_json(const DeviceInfo *info);

/* 获取当前时间字符串 */
void get_current_time(char *buf, int bufsize);

/* ====== 平台特定函数 (由 platform_*.c 实现) ====== */

char *get_os_version(void);
char *get_os_install_date(void);
char *get_device_serial(void);
char *get_hdd_serials(void);
void get_usb_devices(USBDevice **devices, int *count);

/* ====== 平台定义 ====== */

#ifdef _WIN32
    #define PLATFORM_NAME "windows"
    #define PATH_SEP '\\'
    #define snprintf _snprintf
    #define popen _popen
    #define pclose _pclose
#else
    #define PATH_SEP '/'
    #ifdef __linux__
        #define PLATFORM_NAME "linux"
    #elif defined(__APPLE__)
        #define PLATFORM_NAME "darwin"
    #else
        #define PLATFORM_NAME "unknown"
    #endif
#endif

/* 判断序列号是否为无效占位符 */
int is_invalid_serial(const char *s);

/* ====== sudo 密码支持 (main.c 定义, platform_linux.c 使用) ====== */

extern char g_sudo_password[256];

/* 构建带 sudo 密码的命令 (无密码时返回 strdup(cmd)) */
char *build_sudo_cmd(const char *cmd);

#endif /* DM_H */
