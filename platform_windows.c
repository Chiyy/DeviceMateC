/*
 * platform_windows.c - Windows 平台硬件信息采集 (从 Go 移植)
 *
 * 实现 OS 版本、安装日期、设备序列号、硬盘序列号、USB 设备等采集。
 * 优先使用 wmic, 失败时回退到 PowerShell。
 */
#ifdef _WIN32

#include "dm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ====== 内部数据结构 ====== */

/* 磁盘信息 (仅本文件内部使用) */
typedef struct {
    char *interface_type;  /* 接口类型 (IDE/USB/SCSI 等) */
    char *model;           /* 磁盘型号 */
    char *serial;         /* 序列号 */
    char *pnp_device_id;  /* PNP 设备 ID (USBSTOR\... 开头为 USB 设备) */
} DiskInfo;

/* ====== 内部工具函数 ====== */

/* 执行 PowerShell 脚本 (强制 UTF-8 输出), 返回堆分配字符串。
   对应 Go 的 psRun: 自动添加 -NoProfile -NonInteractive 及编码修复前缀。 */
static char *ps_run(const char *script) {
    dstring ds;
    ds_init(&ds);
    ds_append(&ds, "powershell -NoProfile -NonInteractive -Command \"[Console]::OutputEncoding=[Text.Encoding]::UTF8; ");
    ds_append(&ds, script);
    ds_append_char(&ds, '"');
    char *result = run_cmd(ds.data);
    ds_free(&ds);
    return result;
}

/* 从 *cursor 处提取下一行 (到 '\n' 或字符串结尾), 复制到 line 缓冲区并 trim,
   推进 cursor 指向下一行开头。返回 1=成功提取, 0=已到末尾。
   注意: str_trim 不会移动数据到缓冲区开头, 故用 memmove 处理前导空白。 */
static int next_line(const char **cursor, char *line, size_t linesize) {
    const char *p = *cursor;
    if (*p == '\0') return 0;
    const char *end = strchr(p, '\n');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= linesize) len = linesize - 1;
    memcpy(line, p, len);
    line[len] = '\0';
    char *trimmed = str_trim(line);
    if (trimmed != line) {
        memmove(line, trimmed, strlen(trimmed) + 1);
    }
    *cursor = end ? (end + 1) : (p + len);
    return 1;
}

/* 从 wmic /format:list 输出中提取指定字段的值。
   wmic 输出每行格式为 "Key=Value", 返回第一个非空值。
   成功返回堆分配字符串, 未找到或值为空返回 NULL。 */
static char *parse_wmic_value(const char *output, const char *key) {
    if (!output || !key) return NULL;
    size_t keylen = strlen(key);
    const char *cursor = output;
    char line[2048];
    while (next_line(&cursor, line, sizeof(line))) {
        if (line[0] == '\0') continue;
        if (strncmp(line, key, keylen) == 0 && line[keylen] == '=') {
            char *val = str_trim(line + keylen + 1);
            if (val[0] != '\0') {
                return strdup(val);
            }
        }
    }
    return NULL;
}

/* 释放磁盘信息数组 (含内部字符串) */
static void free_disk_drives(DiskInfo *disks, int count) {
    if (!disks) return;
    for (int i = 0; i < count; i++) {
        free(disks[i].interface_type);
        free(disks[i].model);
        free(disks[i].serial);
        free(disks[i].pnp_device_id);
    }
    free(disks);
}

/* 获取所有磁盘信息 (接口类型、型号、序列号、PNP设备ID)。
   wmic /format:list 输出特点: 每个字段单独一行, 记录间有空行,
   每条记录以 InterfaceType= 开始。逐行解析, 遇到 InterfaceType= 即开始新记录。
   返回堆分配数组, *count 设为记录数; 无数据返回 NULL。 */
static DiskInfo *get_disk_drives(int *count) {
    *count = 0;
    char *out = run_cmd("wmic diskdrive get Model,SerialNumber,InterfaceType,PNPDeviceID /format:list");
    if (!out) return NULL;

    int cap = 8;
    DiskInfo *disks = (DiskInfo *)malloc(cap * sizeof(DiskInfo));
    if (!disks) { free(out); return NULL; }

    DiskInfo cur;
    memset(&cur, 0, sizeof(cur));
    int has_data = 0;

    const char *cursor = out;
    char line[2048];
    while (next_line(&cursor, line, sizeof(line))) {
        if (line[0] == '\0') continue;
        char *eq = strchr(line, '=');
        if (!eq || eq == line) continue;
        *eq = '\0';
        char *key = str_trim(line);
        char *val = str_trim(eq + 1);
        if (strcmp(key, "InterfaceType") == 0) {
            /* 遇到新记录: 先保存上一条 */
            if (has_data) {
                if (*count >= cap) {
                    cap *= 2;
                    disks = (DiskInfo *)realloc(disks, cap * sizeof(DiskInfo));
                }
                disks[(*count)++] = cur;
                memset(&cur, 0, sizeof(cur));
            }
            cur.interface_type = strdup(val);
            has_data = 1;
        } else if (strcmp(key, "Model") == 0) {
            free(cur.model);
            cur.model = strdup(val);
        } else if (strcmp(key, "SerialNumber") == 0) {
            free(cur.serial);
            cur.serial = strdup(val);
        } else if (strcmp(key, "PNPDeviceID") == 0) {
            free(cur.pnp_device_id);
            cur.pnp_device_id = strdup(val);
        }
    }
    /* 保存最后一条记录 */
    if (has_data) {
        if (*count >= cap) {
            cap += 1;
            disks = (DiskInfo *)realloc(disks, cap * sizeof(DiskInfo));
        }
        disks[(*count)++] = cur;
    }

    free(out);

    if (*count == 0) {
        free(disks);
        return NULL;
    }
    return disks;
}

/* ====== 公开接口 ====== */

/* 获取 Windows 版本信息 (优先 wmic, 不受 PS 执行策略限制)
   组合格式: "Caption Version (Architecture)" */
char *get_os_version(void) {
    char *out = run_cmd("wmic os get Caption,Version,OSArchitecture /format:list");
    char *caption = parse_wmic_value(out, "Caption");
    char *version = parse_wmic_value(out, "Version");
    char *arch = parse_wmic_value(out, "OSArchitecture");
    free(out);

    if (caption && caption[0] != '\0') {
        dstring ds;
        ds_init(&ds);
        ds_append(&ds, caption);
        ds_append_char(&ds, ' ');
        ds_append(&ds, version ? version : "");
        ds_append(&ds, " (");
        ds_append(&ds, arch ? arch : "");
        ds_append_char(&ds, ')');
        char *result = strdup(ds.data);
        ds_free(&ds);
        free(caption);
        free(version);
        free(arch);
        return result;
    }
    free(caption);
    free(version);
    free(arch);

    /* 兜底: PowerShell */
    out = ps_run("(Get-CimInstance Win32_OperatingSystem).Caption + ' ' + "
                 "(Get-CimInstance Win32_OperatingSystem).Version + ' (' + "
                 "(Get-CimInstance Win32_OperatingSystem).OSArchitecture + ')'");
    char *trimmed = strdup(str_trim(out));
    free(out);
    if (trimmed[0] != '\0') {
        return trimmed;
    }
    free(trimmed);
    return strdup("Windows");
}

/* 获取 Windows 安装日期 (优先注册表, 解析十六进制时间戳)
   reg query 输出: "InstallDate    REG_DWORD    0x17721a2d"
   时间戳为 Unix 秒, 格式化为 "YYYY-MM-DD" */
char *get_os_install_date(void) {
    /* 方式1: reg query 读取注册表 InstallDate */
    char *out = run_cmd("reg query \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\" /v InstallDate");
    const char *cursor = out;
    char line[2048];
    while (next_line(&cursor, line, sizeof(line))) {
        if (str_contains(line, "InstallDate") && str_contains(line, "0x")) {
            char *hex = strstr(line, "0x");
            if (hex) {
                unsigned long long ts = 0;
                if (sscanf(hex, "%llx", &ts) == 1 && ts > 0) {
                    time_t t = (time_t)ts;
                    struct tm *tm = localtime(&t);
                    if (tm) {
                        char date[32];
                        strftime(date, sizeof(date), "%Y-%m-%d", tm);
                        free(out);
                        return strdup(date);
                    }
                }
            }
        }
    }
    free(out);

    /* 方式2: PowerShell 兜底 */
    out = ps_run("$d=(Get-ItemProperty 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion').InstallDate; "
                 "[DateTimeOffset]::FromUnixTimeSeconds($d).LocalDateTime.ToString('yyyy-MM-dd')");
    char *trimmed = strdup(str_trim(out));
    free(out);
    if (trimmed[0] != '\0' && !str_contains(trimmed, "错误")) {
        return trimmed;
    }
    free(trimmed);
    return strdup("未知");
}

/* 获取设备序列号 (优先 wmic baseboard, 过滤占位符)
   占位符: "To be filled by O.E.M."、"Default string"、"None" 等
   回退: wmic csproduct → PowerShell */
char *get_device_serial(void) {
    /* 方式1: wmic baseboard 序列号 */
    char *out = run_cmd("wmic baseboard get SerialNumber /format:list");
    char *serial = parse_wmic_value(out, "SerialNumber");
    free(out);
    if (serial && !is_invalid_serial(serial)) {
        return serial;
    }
    free(serial);

    /* 方式2: wmic csproduct 标识号 */
    out = run_cmd("wmic csproduct get IdentifyingNumber /format:list");
    serial = parse_wmic_value(out, "IdentifyingNumber");
    free(out);
    if (serial && serial[0] != '\0') {
        return serial;
    }
    free(serial);

    /* 方式3: PowerShell 兜底 */
    out = ps_run("(Get-CimInstance Win32_ComputerSystemProduct).IdentifyingNumber");
    char *trimmed = strdup(str_trim(out));
    free(out);
    if (trimmed[0] != '\0') {
        return trimmed;
    }
    free(trimmed);
    return strdup("未知");
}

/* 获取硬盘序列号 (排除 USB 接口设备)
   优先 wmic diskdrive, 回退 PowerShell。
   多个序列号用 ';' 拼接, 无则返回 "未检测到"。 */
char *get_hdd_serials(void) {
    int disk_count = 0;
    DiskInfo *disks = get_disk_drives(&disk_count);

    /* 收集非 USB 硬盘的序列号 (排除 InterfaceType=USB 和 PNPDeviceID=USBSTOR*) */
    int cap = 8;
    char **serials = (char **)malloc(cap * sizeof(char *));
    int serial_count = 0;

    for (int i = 0; i < disk_count; i++) {
        int is_usb = str_eq_ignore_case(disks[i].interface_type, "USB");
        if (!is_usb && disks[i].pnp_device_id) {
            is_usb = (strnicmp(disks[i].pnp_device_id, "USBSTOR", 7) == 0);
        }
        if (!is_usb) {
            if (serial_count >= cap) {
                cap *= 2;
                serials = (char **)realloc(serials, cap * sizeof(char *));
            }
            serials[serial_count++] = strdup(disks[i].serial ? disks[i].serial : "");
        }
    }
    free_disk_drives(disks, disk_count);

    /* 兜底: PowerShell (无法区分 USB 时返回非 USB 全部) */
    if (serial_count == 0) {
        char *out = ps_run("Get-CimInstance Win32_DiskDrive | Where-Object { $_.InterfaceType -ne 'USB' -and $_.PNPDeviceID -notlike 'USBSTOR*' } | "
                           "ForEach-Object { $_.SerialNumber }");
        const char *cursor = out;
        char line[2048];
        while (next_line(&cursor, line, sizeof(line))) {
            if (line[0] == '\0') continue;
            if (!is_invalid_serial(line)) {
                if (serial_count >= cap) {
                    cap *= 2;
                    serials = (char **)realloc(serials, cap * sizeof(char *));
                }
                serials[serial_count++] = strdup(line);
            }
        }
        free(out);
    }

    /* 拼接结果 */
    char *result;
    if (serial_count > 0) {
        result = str_join(serials, serial_count, ";");
    } else {
        result = strdup("未检测到");
    }

    for (int i = 0; i < serial_count; i++) free(serials[i]);
    free(serials);
    return result;
}

/* 获取 USB 存储设备列表 (含名称/型号/序列号)
   优先 wmic diskdrive 筛选 InterfaceType=USB, 回退 PowerShell。
   无设备时 *devices=NULL, *count=0。 */
void get_usb_devices(USBDevice **devices, int *count) {
    *devices = NULL;
    *count = 0;

    int disk_count = 0;
    DiskInfo *disks = get_disk_drives(&disk_count);

    int cap = 8;
    int n = 0;
    USBDevice *devs = (USBDevice *)malloc(cap * sizeof(USBDevice));

    /* 从 wmic 结果中筛选 USB 设备 (InterfaceType=USB 或 PNPDeviceID 以 USBSTOR 开头) */
    for (int i = 0; i < disk_count; i++) {
        int is_usb = str_eq_ignore_case(disks[i].interface_type, "USB");
        if (!is_usb && disks[i].pnp_device_id) {
            /* PNPDeviceID 以 USBSTOR\ 开头则为 USB 存储设备 */
            is_usb = (strnicmp(disks[i].pnp_device_id, "USBSTOR", 7) == 0);
        }
        if (is_usb) {
            if (n >= cap) {
                cap *= 2;
                devs = (USBDevice *)realloc(devs, cap * sizeof(USBDevice));
            }
            char *model = disks[i].model ? disks[i].model : "";
            devs[n].name = (model[0] != '\0') ? strdup(model) : strdup("USB 存储设备");
            devs[n].model = strdup(model);
            devs[n].serial = strdup(disks[i].serial ? disks[i].serial : "");
            n++;
        }
    }
    free_disk_drives(disks, disk_count);

    if (n > 0) {
        *devices = devs;
        *count = n;
        return;
    }

    /* wmic 未找到 USB 设备, 尝试 PowerShell */
    free(devs);
    char *out = ps_run("Get-CimInstance Win32_DiskDrive | Where-Object { $_.InterfaceType -eq 'USB' -or $_.PNPDeviceID -like 'USBSTOR*' } | "
                       "ForEach-Object { $_.Model + '|' + $_.SerialNumber }");

    cap = 8;
    n = 0;
    devs = (USBDevice *)malloc(cap * sizeof(USBDevice));

    const char *cursor = out;
    char line[2048];
    while (next_line(&cursor, line, sizeof(line))) {
        if (line[0] == '\0') continue;
        /* 分割 "Model|Serial" */
        char *bar = strchr(line, '|');
        char *model = line;
        char *serial_str = "";
        if (bar) {
            *bar = '\0';
            serial_str = bar + 1;
        }
        str_trim(model);
        str_trim(serial_str);
        /* 跳过无效序列号 (对应 Go 的 isInvalidSerial 检查) */
        if (is_invalid_serial(serial_str)) continue;

        if (n >= cap) {
            cap *= 2;
            devs = (USBDevice *)realloc(devs, cap * sizeof(USBDevice));
        }
        devs[n].name = (model[0] != '\0') ? strdup(model) : strdup("USB 存储设备");
        devs[n].model = strdup(model);
        devs[n].serial = strdup(serial_str);
        n++;
    }
    free(out);

    if (n > 0) {
        *devices = devs;
        *count = n;
    } else {
        free(devs);
        *devices = NULL;
        *count = 0;
    }
}

#endif /* _WIN32 */
