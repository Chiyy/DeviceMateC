#ifdef __APPLE__

#include "dm.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

/* USB 调试开关: 环境变量 DM_DEBUG_USB=1 或命令行 --debug-usb 启用。
 * 日志输出到 stderr, 格式: [USB] message
 * 用法: DM_DEBUG_USB=1 ./DeviceMate --once  或  ./DeviceMate --debug-usb */
static int usb_debug_enabled(void) {
#ifdef USB_DEBUG_ALWAYS_ON
    return 1;  /* 调试构建版本始终启用 */
#else
    return (getenv("DM_DEBUG_USB") != NULL) || g_debug_usb;
#endif
}

static void usb_dbg(const char *fmt, ...) {
    if (!usb_debug_enabled()) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[USB] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* 记录命令执行和输出摘要 (调试用) */
static void usb_dbg_cmd(const char *cmd, const char *output) {
    if (!usb_debug_enabled()) return;
    if (!output || !output[0]) {
        fprintf(stderr, "[USB] 命令: %s → (无输出)\n", cmd);
        return;
    }
    size_t len = strlen(output);
    fprintf(stderr, "[USB] 命令: %s → %zu 字节\n", cmd, len);
    /* 显示前 500 字节内容 */
    size_t show = len > 500 ? 500 : len;
    fprintf(stderr, "[USB]   内容(前%zu字节): %.*s\n", show, (int)show, output);
    if (len > 500) fprintf(stderr, "[USB]   ... (省略 %zu 字节)\n", len - 500);
}

/* 记录设备添加/拒绝原因 (调试用) */
static void usb_dbg_dev(const char *action, const char *name, const char *serial, const char *reason) {
    if (!usb_debug_enabled()) return;
    fprintf(stderr, "[USB] %s: name=%s, serial=%s", action,
            name ? name : "", serial ? serial : "");
    if (reason) fprintf(stderr, " (%s)", reason);
    fprintf(stderr, "\n");
}

/* =====================================================================
 *  platform_darwin.c  -  macOS 平台硬件信息采集 (由 Go 版本移植)
 *
 *  实现以下 5 个平台特定函数 (声明于 dm.h):
 *    - get_os_version
 *    - get_os_install_date
 *    - get_device_serial
 *    - get_hdd_serials
 *    - get_usb_devices
 *
 *  命令通过 popen 执行 (run_cmd / run_cmd_timeout), 字符串工具来自
 *  collector.c (str_trim / str_join / str_contains / str_starts_with /
 *  is_invalid_serial / dstring)。
 * ===================================================================== */

/* ---------------------------------------------------------------------
 *  本地辅助函数
 * ------------------------------------------------------------------- */

/* str_trim 的安全版本: 返回一份全新 malloc 的已 trim 副本,
 * 不修改原串, 可独立 free (避免 str_trim 移动指针后无法 free 的问题)。 */
static char *str_trim_copy(const char *s) {
    if (!s) return strdup("");
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\r' || s[len - 1] == '\n'))
        len--;
    char *r = (char *)malloc(len + 1);
    memcpy(r, s, len);
    r[len] = '\0';
    return r;
}

/* 非破坏性地将 buf 按行切分, 返回的每行均为独立 malloc 的副本
 * (行内不包含换行符, 且已去除行尾 \r)。
 * 用 free_lines 释放。 */
static char **split_lines_copy(const char *buf, int *n_out) {
    *n_out = 0;
    if (!buf || !*buf) return NULL;
    int cap = 16;
    int n = 0;
    char **lines = (char **)malloc(cap * sizeof(char *));
    const char *p = buf;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0 && p[len - 1] == '\r') len--;   /* 去除行尾 \r */
        char *line = (char *)malloc(len + 1);
        memcpy(line, p, len);
        line[len] = '\0';
        if (n >= cap) { cap *= 2; lines = (char **)realloc(lines, cap * sizeof(char *)); }
        lines[n++] = line;
        if (!nl) break;
        p = nl + 1;
    }
    *n_out = n;
    return lines;
}

static void free_lines(char **lines, int n) {
    if (!lines) return;
    for (int i = 0; i < n; i++) free(lines[i]);
    free(lines);
}

/* 从 ioreg 风格的属性行中提取键对应的引号值。
 * 行形如:   "IOPlatformSerialNumber" = "ABC123"
 * 流程: 在行中定位 key -> 定位其后的 "= " -> 跳过空白 -> 去引号 -> 复制。
 * 返回 malloc 的新串, 找不到返回 NULL。 */
static char *ioreg_extract_value(const char *line, const char *key) {
    if (!line || !key) return NULL;
    const char *kp = strstr(line, key);
    if (!kp) return NULL;
    const char *eq = strstr(kp, "= ");
    if (!eq) return NULL;
    const char *val = eq + 2;
    while (*val == ' ' || *val == '\t') val++;
    const char *start = val;
    if (*start == '"') start++;                 /* 去前引号 */
    const char *end = start + strlen(start);
    while (end > start) {                        /* 去尾引号/空白 */
        char c = *(end - 1);
        if (c == '"' || c == ' ' || c == '\t' || c == '\r') end--;
        else break;
    }
    size_t len = (size_t)(end - start);
    if (len == 0) return NULL;
    char *result = (char *)malloc(len + 1);
    memcpy(result, start, len);
    result[len] = '\0';
    return result;
}

/* 从 nvram 输出中提取 key 之后的值 (兼容 tab / 冒号分隔)。 */
static char *nvram_extract(const char *out, const char *key) {
    if (!out) return NULL;
    const char *kp = strstr(out, key);
    if (!kp) return NULL;
    kp += strlen(key);
    while (*kp == ' ' || *kp == '\t' || *kp == ':') kp++;
    return str_trim_copy(kp);
}

/* 在多行输出中查找包含 key 的行, 按 ioreg 风格提取 "= " 后的引号值,
 * 跳过无效占位符。找不到返回 NULL。 */
static char *find_ioreg_value(const char *out, const char *key) {
    if (!out) return NULL;
    int n = 0;
    char **lines = split_lines_copy(out, &n);
    char *result = NULL;
    for (int i = 0; i < n; i++) {
        char *v = ioreg_extract_value(lines[i], key);
        if (v) {
            if (!is_invalid_serial(v)) { result = v; break; }
            free(v);
        }
    }
    free_lines(lines, n);
    return result;
}

/* 在多行输出中查找 "Key: Value" 行。
 * prefix     : 行 (已 trim) 需以该前缀开头, NULL 表示不限制。
 * require_sub: 行需包含该子串, NULL 表示不限制。
 * 提取第一个冒号后的值, 跳过无效占位符。找不到返回 NULL。 */
static char *find_sp_value(const char *out, const char *prefix, const char *require_sub) {
    if (!out) return NULL;
    int n = 0;
    char **lines = split_lines_copy(out, &n);
    char *result = NULL;
    for (int i = 0; i < n; i++) {
        char *line = str_trim(lines[i]);        /* 原地 trim, 安全 (经 lines[i] 释放) */
        if (prefix && !str_starts_with(line, prefix)) continue;
        if (require_sub && !str_contains(line, require_sub)) continue;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        char *v = str_trim_copy(colon + 1);
        if (!is_invalid_serial(v)) { result = v; break; }
        free(v);
    }
    free_lines(lines, n);
    return result;
}

/* 按优先级确定 USB 设备显示名: model > vendor > "USB 存储设备" */
static const char *pick_name(const char *model, const char *vendor) {
    if (model && model[0]) return model;
    if (vendor && vendor[0]) return vendor;
    return "USB 存储设备";
}

/* ---------------------------------------------------------------------
 *  USBDevice 动态数组
 * ------------------------------------------------------------------- */

typedef struct {
    USBDevice *items;
    int count;
    int cap;
} USBList;

static void usb_list_init(USBList *l) {
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

/* 追加一个设备, 各字段用 strdup 复制 (name 为空时回退为 "USB 存储设备")。 */
static void usb_list_push(USBList *l, const char *name, const char *model, const char *serial) {
    if (l->count >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = (USBDevice *)realloc(l->items, (size_t)l->cap * sizeof(USBDevice));
    }
    l->items[l->count].name   = strdup(name && name[0] ? name : "USB 存储设备");
    l->items[l->count].model  = strdup(model ? model : "");
    l->items[l->count].serial = strdup(serial ? serial : "");
    l->count++;
}

static void usb_list_free(USBList *l) {
    for (int i = 0; i < l->count; i++) {
        free(l->items[i].name);
        free(l->items[i].model);
        free(l->items[i].serial);
    }
    free(l->items);
    l->items = NULL;
    l->count = l->cap = 0;
}

/* ---------------------------------------------------------------------
 *  1. 操作系统版本
 * ------------------------------------------------------------------- */

char *get_os_version(void) {
    char *product = run_cmd("sw_vers -productName");
    char *version = run_cmd("sw_vers -productVersion");
    char *p = str_trim_copy(product);
    char *v = str_trim_copy(version);
    free(product);
    free(version);

    if (p[0] != '\0' && v[0] != '\0') {
        size_t len = strlen(p) + 1 + strlen(v) + 1;
        char *result = (char *)malloc(len);
        snprintf(result, len, "%s %s", p, v);
        free(p);
        free(v);
        return result;
    }
    free(p);
    free(v);
    return strdup("macOS");
}

/* ---------------------------------------------------------------------
 *  2. 操作系统安装日期 (根目录创建时间)
 * ------------------------------------------------------------------- */

char *get_os_install_date(void) {
    char *out = run_cmd("stat -f %SB -t %Y-%m-%d /");
    if (out) {
        char *t = str_trim_copy(out);
        free(out);
        if (t[0] != '\0') return t;
        free(t);
    }
    return strdup("未知");
}

/* ---------------------------------------------------------------------
 *  3. 设备序列号 (8 级回退)
 * ------------------------------------------------------------------- */

char *get_device_serial(void) {
    char *out, *v;

    /* 方式1: ioreg - IOPlatformSerialNumber */
    out = run_cmd("ioreg -rd1 -c IOPlatformExpertDevice");
    v = find_ioreg_value(out, "IOPlatformSerialNumber");
    free(out);
    if (v) return v;

    /* 方式2: system_profiler SPHardwareDataType - Serial Number */
    out = run_cmd("system_profiler SPHardwareDataType");
    v = find_sp_value(out, "Serial Number", NULL);
    if (v) { free(out); return v; }

    /* 方式3: 同一输出 - IOPlatformUUID (ioreg 风格, 通常不匹配, 留作兜底) */
    v = find_ioreg_value(out, "IOPlatformUUID");
    free(out);
    if (v) return v;

    /* 方式4: ioreg - IOBoardSerialNumber (主板序列号) */
    out = run_cmd("ioreg -rd1 -c IOPlatformExpertDevice");
    v = find_ioreg_value(out, "IOBoardSerialNumber");
    free(out);
    if (v) return v;

    /* 方式5: system_profiler SPHardwareDataType - Hardware ... UUID */
    out = run_cmd("system_profiler SPHardwareDataType");
    v = find_sp_value(out, "Hardware", "UUID");
    free(out);
    if (v) return v;

    /* 方式6: ioreg -d 2 深度遍历 - IOPlatformUUID */
    out = run_cmd("ioreg -d 2 -c IOPlatformExpertDevice");
    v = find_ioreg_value(out, "IOPlatformUUID");
    free(out);
    if (v) return v;

    /* 方式7: nvram platform-uuid */
    out = run_cmd("nvram platform-uuid");
    v = NULL;
    if (out && out[0]) v = nvram_extract(out, "platform-uuid");
    free(out);
    if (v && !is_invalid_serial(v)) return v;
    free(v);

    /* 方式8: system_profiler SPPlatformDataType - 含 UUID 的行 */
    out = run_cmd("system_profiler SPPlatformDataType");
    v = find_sp_value(out, NULL, "UUID");
    free(out);
    if (v) return v;

    return strdup("未知");
}

/* ---------------------------------------------------------------------
 *  4. 硬盘序列号
 * ------------------------------------------------------------------- */

char *get_hdd_serials(void) {
    char **serials = NULL;
    int count = 0, cap = 0;
    char *out;

    /* 方式1: system_profiler NVMe + SATA 内置硬盘 */
    out = run_cmd_timeout(20, "system_profiler SPNVMeDataType SPSerialATADataType");
    if (out) {
        int n = 0;
        char **lines = split_lines_copy(out, &n);
        for (int i = 0; i < n; i++) {
            char *line = str_trim(lines[i]);
            if (str_starts_with(line, "Serial Number")) {
                char *colon = strchr(line, ':');
                if (colon) {
                    char *v = str_trim_copy(colon + 1);
                    if (!is_invalid_serial(v)) {
                        if (count >= cap) { cap = cap ? cap * 2 : 8; serials = (char **)realloc(serials, (size_t)cap * sizeof(char *)); }
                        serials[count++] = v;
                        continue;
                    }
                    free(v);
                }
            }
        }
        free_lines(lines, n);
        free(out);
    }

    /* 方式2: ioreg IOBlockStorageDevice - device-serial-number (收集全部) */
    if (count == 0) {
        out = run_cmd("ioreg -r -d 5 -w 0 -l -c IOBlockStorageDevice");
        if (out) {
            int n = 0;
            char **lines = split_lines_copy(out, &n);
            for (int i = 0; i < n; i++) {
                char *line = str_trim(lines[i]);
                if (str_contains(line, "\"device-serial-number\"")) {
                    char *v = ioreg_extract_value(line, "device-serial-number");
                    if (v && !is_invalid_serial(v)) {
                        if (count >= cap) { cap = cap ? cap * 2 : 8; serials = (char **)realloc(serials, (size_t)cap * sizeof(char *)); }
                        serials[count++] = v;
                        continue;
                    }
                    free(v);
                }
            }
            free_lines(lines, n);
            free(out);
        }
    }

    /* 用分号拼接, 空则返回 "未检测到" */
    char *result = str_join(serials, count, ";");
    for (int i = 0; i < count; i++) free(serials[i]);
    free(serials);
    if (!result) return strdup("未检测到");
    return result;
}

/* ---------------------------------------------------------------------
 *  5. USB 存储设备 (两级检测)
 *
 *  检测策略 (两级回退):
 *    方式1 (主路径, 快速): diskutil list external physical + ioreg
 *      - diskutil 列出外部物理磁盘 (基本即 USB 存储)
 *      - ioreg 按 BSD Name 匹配, 取 serial/model/vendor
 *    方式2 (兼容回退): system_profiler SPUSBDataType
 *      - 解析缩进树, 找出含 BSD Name / Mass Storage / Removable Media 的节点
 *      - 适用于 diskutil 未列出但 system_profiler 可见的设备
 * ------------------------------------------------------------------- */

/* 5a. 从 diskutil 输出解析外部磁盘标识符 (/dev/diskN, 排除分区 diskNsM) */
static char **parse_external_disk_ids(const char *out, int *count) {
    *count = 0;
    if (!out) return NULL;
    int cap = 8, n = 0;
    char **ids = (char **)malloc((size_t)cap * sizeof(char *));
    int ln = 0;
    char **lines = split_lines_copy(out, &ln);
    for (int i = 0; i < ln; i++) {
        char *line = str_trim(lines[i]);
        if (!str_starts_with(line, "/dev/disk")) continue;
        char *p = line + 5;                       /* 跳过 "/dev/" */
        char *end = p;
        while (*end && *end != ' ' && *end != '\t') end++;
        size_t len = (size_t)(end - p);
        if (len <= 4) continue;                   /* 至少 "diskN" */
        /* BSD Name 格式: 整盘 "diskN", 分区 "diskNsM"
         * 跳过 "disk" 前缀后, 若含 's' 则为分区, 排除 */
        int has_s = 0;
        for (size_t k = 4; k < len; k++) if (p[k] == 's') { has_s = 1; break; }
        if (!has_s) {
            char *id = (char *)malloc(len + 1);
            memcpy(id, p, len);
            id[len] = '\0';
            if (n >= cap) { cap *= 2; ids = (char **)realloc(ids, (size_t)cap * sizeof(char *)); }
            ids[n++] = id;
        }
    }
    free_lines(lines, ln);
    *count = n;
    return ids;
}

/* 5b. 通过 ioreg 查找指定磁盘的 序列号/型号/厂商 (匹配 BSD Name)。
 *     主路径: ioreg -c IOBlockStorageDevice (仅块存储设备, 快速)
 *     备选路径: ioreg -c IOUSBHostDevice (USB 设备树, 含子节点 BSD Name)
 *     返回值均通过 out_* 以 malloc 新串返回 (可能为 NULL)。 */
static void get_disk_info_from_ioreg(const char *disk_id,
                                     char **out_serial,
                                     char **out_model,
                                     char **out_vendor) {
    *out_serial = NULL;
    *out_model = NULL;
    *out_vendor = NULL;

    char *cur_serial = NULL, *cur_model = NULL, *cur_vendor = NULL;

    char bsd_match[80];
    snprintf(bsd_match, sizeof(bsd_match), "\"BSD Name\" = \"%s", disk_id);

    /* 主路径: ioreg -c IOBlockStorageDevice (仅列出块存储设备, 远快于全量 dump) */
    char *out = run_cmd("ioreg -r -d 5 -w 0 -c IOBlockStorageDevice 2>&1");
    usb_dbg_cmd("ioreg -r -d 5 -w 0 -c IOBlockStorageDevice", out);
    if (out) {
        int n = 0;
        char **lines = split_lines_copy(out, &n);
        int found_disk = 0;
        for (int i = 0; i < n; i++) {
            char *line = str_trim(lines[i]);

            /* 命中目标磁盘的 BSD Name */
            if (str_contains(line, bsd_match)) found_disk = 1;

            /* 进入下一个设备节点 */
            if (str_contains(line, "+-o ") || str_contains(line, "\\-o ")) {
                if (found_disk && cur_serial) break;          /* 已拿到序列号, 结束 */
                if (found_disk) {                            /* 同磁盘新节点, 清空缓冲 */
                    free(cur_serial); cur_serial = NULL;
                    free(cur_model);   cur_model = NULL;
                    free(cur_vendor);  cur_vendor = NULL;
                }
                continue;
            }
            if (!found_disk) continue;

            if (str_contains(line, "\"device-serial-number\"") ||
                str_contains(line, "\"USB Serial Number\"")) {
                char *v = ioreg_extract_value(line, "device-serial-number");
                if (!v) v = ioreg_extract_value(line, "USB Serial Number");
                if (v && v[0] && strcmp(v, "None") != 0) { free(cur_serial); cur_serial = v; }
                else free(v);
            } else if (str_contains(line, "\"device-model\"") ||
                       str_contains(line, "\"USB Product Name\"")) {
                char *v = ioreg_extract_value(line, "device-model");
                if (!v) v = ioreg_extract_value(line, "USB Product Name");
                if (v && v[0] && !str_starts_with(v, "0x")) { free(cur_model); cur_model = v; }
                else free(v);
            } else if (str_contains(line, "\"device-vendor\"") ||
                       str_contains(line, "\"USB Vendor Name\"")) {
                char *v = ioreg_extract_value(line, "device-vendor");
                if (!v) v = ioreg_extract_value(line, "USB Vendor Name");
                if (v && v[0] && !str_starts_with(v, "0x")) { free(cur_vendor); cur_vendor = v; }
                else free(v);
            }
        }
        free_lines(lines, n);
        free(out);
    }

    *out_serial = cur_serial;
    *out_model = cur_model;
    *out_vendor = cur_vendor;
}

/* 通过 USB 产品名在 IOUSBHostDevice 树中查找对应的 USB 序列号。
 *
 * 关键: BSD Name 只存在于 IOBlockStorageDevice 树, 不在 IOUSBHostDevice 树中,
 * 因此无法通过 BSD Name 关联两棵树。改用 USB Product Name 关联
 * (diskutil info 的 "Device Name" 与 ioreg 的 "USB Product Name" 通常一致)。
 *
 * 返回 malloc 的新串, 找不到返回 NULL。 */
static char *find_usb_serial_by_product(const char *product_name) {
    if (!product_name || !product_name[0]) return NULL;

    char *out = run_cmd("ioreg -r -d 5 -w 0 -l -c IOUSBHostDevice 2>&1");
    usb_dbg_cmd("ioreg -r -d 5 -w 0 -l -c IOUSBHostDevice (find_usb_serial_by_product)", out);
    if (!out) return NULL;

    int n = 0;
    char **lines = split_lines_copy(out, &n);
    char *result = NULL;
    char *cur_product = NULL;
    char *cur_serial = NULL;
    int matched = 0;

    for (int i = 0; i < n && !result; i++) {
        char *line = str_trim(lines[i]);

        /* 进入新的 IOUSBHostDevice 节点: 先检查上一个设备是否匹配, 再重置 */
        if ((str_contains(line, "+-o ") || str_contains(line, "\\-o ")) &&
            str_contains(line, "IOUSBHostDevice")) {
            if (matched && cur_serial) {
                result = strdup(cur_serial);
                break;
            }
            free(cur_product); cur_product = NULL;
            free(cur_serial);   cur_serial = NULL;
            matched = 0;
            continue;
        }

        /* 追踪当前 IOUSBHostDevice 节点的 USB 属性 */
        if (str_contains(line, "\"USB Product Name\"")) {
            char *v = ioreg_extract_value(line, "USB Product Name");
            if (v && v[0]) {
                free(cur_product); cur_product = v;
                matched = (strcmp(v, product_name) == 0);
                usb_dbg("USB Product Name='%s' vs 目标='%s' → %s",
                        v, product_name, matched ? "匹配" : "不匹配");
            } else free(v);
        } else if (str_contains(line, "\"USB Serial Number\"")) {
            char *v = ioreg_extract_value(line, "USB Serial Number");
            if (v && v[0] && strcmp(v, "None") != 0) { free(cur_serial); cur_serial = v; }
            else free(v);
        }
    }

    /* 处理最后一个设备 (循环结束未被边界触发) */
    if (!result && matched && cur_serial) {
        result = strdup(cur_serial);
    }

    free(cur_product);
    free(cur_serial);
    free_lines(lines, n);
    free(out);
    return result;
}

/* 通过 USB 产品名在 IOUSBHostDevice 树中查找对应的 USB 厂商名。
 * 逻辑同 find_usb_serial_by_product, 但提取 "USB Vendor Name"。 */
static char *find_usb_vendor_by_product(const char *product_name) {
    if (!product_name || !product_name[0]) return NULL;

    char *out = run_cmd("ioreg -r -d 5 -w 0 -l -c IOUSBHostDevice 2>&1");
    if (!out) return NULL;

    int n = 0;
    char **lines = split_lines_copy(out, &n);
    char *result = NULL;
    char *cur_product = NULL;
    char *cur_vendor = NULL;
    int matched = 0;

    for (int i = 0; i < n && !result; i++) {
        char *line = str_trim(lines[i]);

        if ((str_contains(line, "+-o ") || str_contains(line, "\\-o ")) &&
            str_contains(line, "IOUSBHostDevice")) {
            if (matched && cur_vendor) {
                result = strdup(cur_vendor);
                break;
            }
            free(cur_product); cur_product = NULL;
            free(cur_vendor);  cur_vendor = NULL;
            matched = 0;
            continue;
        }

        if (str_contains(line, "\"USB Product Name\"")) {
            char *v = ioreg_extract_value(line, "USB Product Name");
            if (v && v[0]) {
                free(cur_product); cur_product = v;
                matched = (strcmp(v, product_name) == 0);
            } else free(v);
        } else if (str_contains(line, "\"USB Vendor Name\"")) {
            char *v = ioreg_extract_value(line, "USB Vendor Name");
            if (v && v[0] && !str_starts_with(v, "0x")) { free(cur_vendor); cur_vendor = v; }
            else free(v);
        }
    }

    if (!result && matched && cur_vendor) {
        result = strdup(cur_vendor);
    }

    free(cur_product);
    free(cur_vendor);
    free_lines(lines, n);
    free(out);
    return result;
}

/* 从 diskutil info <disk> 输出中提取指定字段值 (如 "Protocol", "Device Name")。
 * 返回 malloc 的新串, 找不到返回 NULL。 */
static char *diskutil_info_get(const char *disk_id, const char *key) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "diskutil info %s 2>&1", disk_id);
    char *out = run_cmd(cmd);
    if (!out) return NULL;

    char needle[64];
    snprintf(needle, sizeof(needle), "%s:", key);
    size_t nlen = strlen(needle);

    int n = 0;
    char **lines = split_lines_copy(out, &n);
    char *result = NULL;
    for (int i = 0; i < n; i++) {
        char *line = str_trim(lines[i]);
        /* 跳过前导空白后, 行应以 "Protocol:" 等开头 */
        if (strncmp(line, needle, nlen) == 0) {
            const char *p = line + nlen;
            while (*p == ' ' || *p == '\t') p++;
            result = strdup(p);
            /* 去尾空白 */
            size_t rlen = strlen(result);
            while (rlen > 0 && (result[rlen - 1] == ' ' || result[rlen - 1] == '\t' ||
                                result[rlen - 1] == '\r')) {
                result[--rlen] = '\0';
            }
            break;
        }
    }
    free_lines(lines, n);
    free(out);
    return result;
}

/* 不区分大小写比较字符串 (POSIX strcasecmp 的便携替代) */
static int strieq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

/* 方式1: diskutil list external physical + ioreg (主路径, 快速) */
static void detect_usb_via_diskutil(USBList *list) {
    /* 2>&1 合并 stderr 到 stdout, 便于捕获权限/错误信息 */
    const char *cmd = "diskutil list external physical 2>&1";
    char *out = run_cmd(cmd);
    if (!out) { usb_dbg("%s: 无输出", cmd); return; }
    usb_dbg_cmd(cmd, out);

    int count = 0;
    char **ids = parse_external_disk_ids(out, &count);
    usb_dbg("diskutil: 解析到 %d 个外部磁盘", count);
    free(out);

    for (int i = 0; i < count; i++) {
        /* 用 diskutil info 验证 Protocol=USB, 过滤 Thunderbolt/SD/虚拟磁盘等
         * "external, physical" 不等于 USB, 必须显式校验协议 */
        char *protocol = diskutil_info_get(ids[i], "Protocol");
        usb_dbg("disk %s: protocol=%s", ids[i], protocol ? protocol : "(未知)");
        if (protocol && !strieq(protocol, "USB")) {
            usb_dbg_dev("跳过(非USB协议)", "", ids[i], protocol);
            free(protocol);
            free(ids[i]);
            continue;
        }
        free(protocol);

        char *serial = NULL, *model = NULL, *vendor = NULL;
        get_disk_info_from_ioreg(ids[i], &serial, &model, &vendor);
        /* diskutil info 提供 Device Name 作为 model 兜底 (ioreg 可能查不到) */
        if (!model) {
            char *dev_name = diskutil_info_get(ids[i], "Device Name");
            if (dev_name && dev_name[0] && !strieq(dev_name, "Unknown") &&
                !strieq(dev_name, "Disk")) {
                model = dev_name;
            } else {
                free(dev_name);
            }
        }
        /* 关键: 若仍无序列号, 通过 USB 产品名在 IOUSBHostDevice 树中查找
         * (BSD Name 不在 IOUSBHostDevice 树中, 必须通过产品名关联) */
        if (!serial && model) {
            usb_dbg("通过产品名 '%s' 查找 USB 序列号...", model);
            serial = find_usb_serial_by_product(model);
            if (serial) {
                usb_dbg("找到 USB 序列号: %s", serial);
            } else {
                usb_dbg("未找到匹配的 USB 序列号");
            }
        }
        /* 同样通过产品名查找 USB 厂商 */
        if (!vendor && model) {
            vendor = find_usb_vendor_by_product(model);
        }
        const char *name = pick_name(model, vendor);
        if (serial || model || vendor) {
            usb_dbg_dev("添加", name, serial, ids[i]);
        } else {
            usb_dbg_dev("添加(无详情)", name, "", ids[i]);
        }
        usb_list_push(list, name, model ? model : "", serial ? serial : "");
        free(serial);
        free(model);
        free(vendor);
        free(ids[i]);
    }
    free(ids);
}

/* 方式2: system_profiler SPUSBDataType, 解析缩进树
 * 移植自 Go getUSBDevicesViaSystemProfiler。 */
typedef struct {
    char *name;          /* 设备节点名 (去尾 ':') */
    char *serial;
    char *manufacturer;
    char *product;
    int has_serial;
    int is_storage;    /* 是否为存储设备 (有 BSD Name 或 Mass Storage 属性) */
} SPUSBDev;

static void spusbd_init(SPUSBDev *d) {
    d->name = d->serial = d->manufacturer = d->product = NULL;
    d->has_serial = 0;
    d->is_storage = 0;
}

static void spusbd_free(SPUSBDev *d) {
    free(d->name);
    free(d->serial);
    free(d->manufacturer);
    free(d->product);
    spusbd_init(d);
}

static void detect_usb_via_system_profiler(USBList *list) {
    /* 2>&1 合并 stderr 到 stdout, 便于捕获权限/错误信息 (popen 仅捕获 stdout) */
    const char *cmd = "system_profiler SPUSBDataType 2>&1";
    char *out = run_cmd_timeout(20, cmd);
    if (!out) { usb_dbg("%s: 无输出", cmd); return; }
    usb_dbg_cmd(cmd, out);
    int n = 0;
    char **lines = split_lines_copy(out, &n);

    SPUSBDev cur;
    spusbd_init(&cur);
    SPUSBDev *all = NULL;
    int all_count = 0, all_cap = 0;
    int device_indent = -1;

    for (int i = 0; i < n; i++) {
        char *line = lines[i];                 /* 保留原始起点用于计算缩进 */
        /* 必须先算缩进: str_trim 会用 memmove 覆盖前导空白 */
        int indent = 0;
        while (line[indent] == ' ' || line[indent] == '\t') indent++;
        char *trimmed = str_trim(line);
        if (trimmed[0] == '\0') continue;

        size_t tlen = strlen(trimmed);
        int ends_with_colon = (tlen > 0 && trimmed[tlen - 1] == ':');

        /* 判断是否为属性行 (以已知属性前缀开头)
         * 防止以 ':' 结尾的属性行被误判为设备节点 */
        int is_property = (str_starts_with(trimmed, "Serial Number") ||
                           str_starts_with(trimmed, "Manufacturer") ||
                           str_starts_with(trimmed, "Product") ||
                           str_starts_with(trimmed, "Vendor") ||
                           str_starts_with(trimmed, "Version") ||
                           str_starts_with(trimmed, "Speed") ||
                           str_starts_with(trimmed, "Total Number") ||
                           str_starts_with(trimmed, "Volumes") ||
                           str_starts_with(trimmed, "BSD") ||
                           str_starts_with(trimmed, "Capacity") ||
                           str_starts_with(trimmed, "Mass Storage") ||
                           str_starts_with(trimmed, "Removable") ||
                           str_starts_with(trimmed, "Ejectable") ||
                           str_starts_with(trimmed, "Hub") ||
                           str_starts_with(trimmed, "Location") ||
                           str_starts_with(trimmed, "Bus") ||
                           str_starts_with(trimmed, "PCI") ||
                           str_starts_with(trimmed, "Host Controller") ||
                           str_starts_with(trimmed, "Root") ||
                           str_starts_with(trimmed, "Companion") ||
                           str_starts_with(trimmed, "Extra") ||
                           str_starts_with(trimmed, "Port") ||
                           str_starts_with(trimmed, "Current") ||
                           str_starts_with(trimmed, "Power") ||
                           str_starts_with(trimmed, "Media") ||
                           str_starts_with(trimmed, "Width") ||
                           str_starts_with(trimmed, "Sleep") ||
                           str_starts_with(trimmed, "Wake") ||
                           str_starts_with(trimmed, "Description"));

        if (ends_with_colon && !is_property) {
            /* 任何以 ':' 结尾的非属性行都视为设备节点 -> flush 上一个
             * (旧逻辑仅在 indent<=device_indent 时 flush, 导致嵌套子设备
             *  被 USB Hub 遮蔽而漏掉; 改为每次进入新节点都 flush) */
            if (device_indent != -1) {
                if (cur.is_storage) {
                    if (all_count >= all_cap) { all_cap = all_cap ? all_cap * 2 : 8; all = (SPUSBDev *)realloc(all, (size_t)all_cap * sizeof(SPUSBDev)); }
                    all[all_count++] = cur;
                    spusbd_init(&cur);
                } else {
                    spusbd_free(&cur);
                }
            }
            device_indent = indent;
            /* 记录设备名 (去掉末尾 ':') */
            size_t nl = tlen;
            if (nl > 0 && trimmed[nl - 1] == ':') nl--;
            char *nm = (char *)malloc(nl + 1);
            memcpy(nm, trimmed, nl);
            nm[nl] = '\0';
            free(cur.name);
            cur.name = nm;
            continue;
        }

        /* 属性行: 必须位于设备之下 (缩进更深) */
        if (device_indent >= 0 && indent > device_indent) {
            char *colon = strchr(trimmed, ':');
            if (colon && colon > trimmed) {
                size_t klen = (size_t)(colon - trimmed);
                char *key = (char *)malloc(klen + 1);
                memcpy(key, trimmed, klen);
                key[klen] = '\0';
                char *kt = str_trim(key);          /* trim 后用于前缀匹配 */
                char *val = str_trim_copy(colon + 1);

                if (str_starts_with(kt, "Serial Number")) {
                    if (val[0] && strcmp(val, "None") != 0) {
                        free(cur.serial); cur.serial = strdup(val);
                        cur.has_serial = 1;
                    }
                } else if (str_starts_with(kt, "Manufacturer")) {
                    free(cur.manufacturer); cur.manufacturer = strdup(val);
                } else if (str_starts_with(kt, "Product Name")) {
                    if (val[0] && !str_starts_with(val, "0x")) {
                        free(cur.product); cur.product = strdup(val);
                    }
                } else if (str_starts_with(kt, "Product") &&
                           !str_starts_with(kt, "Product ID")) {
                    if (val[0] && !str_starts_with(val, "0x")) {
                        free(cur.product); cur.product = strdup(val);
                    }
                } else if (str_starts_with(kt, "BSD Name")) {
                    /* 有 BSD Name 说明是磁盘/存储设备 */
                    cur.is_storage = 1;
                } else if (str_starts_with(kt, "Mass Storage")) {
                    cur.is_storage = 1;
                } else if (str_starts_with(kt, "Removable Media")) {
                    /* 可移动介质也是 USB 存储设备的强标志 */
                    cur.is_storage = 1;
                } else if (str_starts_with(kt, "Ejectable Media")) {
                    /* 可弹出介质也是 USB 存储设备的标志 */
                    cur.is_storage = 1;
                } else if (str_starts_with(kt, "Ejectable")) {
                    cur.is_storage = 1;
                }
                free(key);
                free(val);
            }
        }
    }

    /* flush 末尾设备 */
    if (cur.is_storage) {
        if (all_count >= all_cap) { all_cap = all_cap ? all_cap * 2 : 8; all = (SPUSBDev *)realloc(all, (size_t)all_cap * sizeof(SPUSBDev)); }
        all[all_count++] = cur;
        spusbd_init(&cur);
    } else {
        spusbd_free(&cur);
    }

    /* 构造 USBDevice: name = product > manufacturer > name > 默认 */
    usb_dbg("system_profiler: all_count=%d", all_count);
    for (int i = 0; i < all_count; i++) {
        const char *name = all[i].product;
        if (!name || !name[0]) name = all[i].manufacturer;
        if (!name || !name[0]) name = all[i].name;
        if (!name || !name[0]) name = "USB 存储设备";
        usb_dbg_dev("添加", name, all[i].serial, "system_profiler");
        usb_list_push(list, name,
                      all[i].product ? all[i].product : "",
                      all[i].serial ? all[i].serial : "");
    }

    for (int i = 0; i < all_count; i++) spusbd_free(&all[i]);
    free(all);
    free_lines(lines, n);
    free(out);
}

void get_usb_devices(USBDevice **devices, int *count) {
    USBList list;
    usb_list_init(&list);

    /* 方式1: diskutil + ioreg (主路径, 快速) */
    detect_usb_via_diskutil(&list);
    usb_dbg("方式1 diskutil: %d 个", list.count);
    if (list.count > 0) {
        *devices = list.items;
        *count = list.count;
        return;
    }

    /* 方式2: system_profiler SPUSBDataType (兼容回退) */
    detect_usb_via_system_profiler(&list);
    usb_dbg("方式2 system_profiler: %d 个", list.count);

    if (list.count > 0) {
        *devices = list.items;
        *count = list.count;
    } else {
        usb_list_free(&list);
        *devices = NULL;
        *count = 0;
    }
}

/* 诊断函数: 输出各命令原始结果 + 最终检测结果, 供排查 USB 检测问题。
 * 由 main.c 在 --debug-usb 参数下调用, 输出到 stdout。 */
void debug_usb_devices(void) {
    printf("========================================\n");
    printf("  USB 存储设备诊断\n");
    printf("========================================\n");

    /* 系统信息 */
    char *osver = get_os_version();
    printf("  macOS 版本: %s\n", osver ? osver : "未知");
    free(osver);
    printf("----------------------------------------\n");

    /* 启用 USB 调试日志 (输出到 stderr) */
    g_debug_usb = 1;

    /* 1. diskutil list external physical */
    printf("\n[1/5] diskutil list external physical\n");
    {
        const char *cmd = "diskutil list external physical";
        char *out = run_cmd(cmd);
        if (out && out[0]) {
            size_t len = strlen(out);
            size_t show = len > 2000 ? 2000 : len;
            printf("输出 (%zu 字节, 显示前 %zu):\n", len, show);
            printf("%.*s\n", (int)show, out);
            if (len > 2000) printf("... (省略 %zu 字节)\n", len - 2000);
        } else {
            printf("(无输出)\n");
        }
        free(out);
    }

    /* 2. ioreg -r -d 5 -w 0 -c IOBlockStorageDevice */
    printf("\n[2/5] ioreg -r -d 5 -w 0 -c IOBlockStorageDevice\n");
    {
        const char *cmd = "ioreg -r -d 5 -w 0 -c IOBlockStorageDevice";
        char *out = run_cmd(cmd);
        if (out && out[0]) {
            size_t len = strlen(out);
            size_t show = len > 5000 ? 5000 : len;
            printf("输出 (%zu 字节, 显示前 %zu):\n", len, show);
            printf("%.*s\n", (int)show, out);
            if (len > 5000) printf("... (省略 %zu 字节)\n", len - 5000);
        } else {
            printf("(无输出)\n");
        }
        free(out);
    }

    /* 3. ioreg -r -d 5 -w 0 -l -c IOUSBHostDevice (USB 设备树, 含 USB Serial Number) */
    printf("\n[3/5] ioreg -r -d 5 -w 0 -l -c IOUSBHostDevice\n");
    {
        const char *cmd = "ioreg -r -d 5 -w 0 -l -c IOUSBHostDevice";
        char *out = run_cmd(cmd);
        if (out && out[0]) {
            size_t len = strlen(out);
            size_t show = len > 8000 ? 8000 : len;
            printf("输出 (%zu 字节, 显示前 %zu):\n", len, show);
            printf("%.*s\n", (int)show, out);
            if (len > 8000) printf("... (省略 %zu 字节)\n", len - 8000);
        } else {
            printf("(无输出)\n");
        }
        free(out);
    }

    /* 4. system_profiler SPUSBDataType */
    printf("\n[4/5] system_profiler SPUSBDataType\n");
    {
        const char *cmd = "system_profiler SPUSBDataType";
        char *out = run_cmd_timeout(20, cmd);
        if (out && out[0]) {
            size_t len = strlen(out);
            size_t show = len > 5000 ? 5000 : len;
            printf("输出 (%zu 字节, 显示前 %zu):\n", len, show);
            printf("%.*s\n", (int)show, out);
            if (len > 5000) printf("... (省略 %zu 字节)\n", len - 5000);
        } else {
            printf("(无输出)\n");
        }
        free(out);
    }

    /* 5. 对每个检测到的外部磁盘执行 diskutil info, 显示实际协议/名称等
     * 这是关键诊断: 若 Protocol != USB, 该磁盘不会被加入 USB 列表 */
    printf("\n[5/5] diskutil info <disk> (每个外部磁盘)\n");
    {
        const char *cmd = "diskutil list external physical";
        char *out = run_cmd(cmd);
        if (out && out[0]) {
            int cnt = 0;
            char **ids = parse_external_disk_ids(out, &cnt);
            if (cnt == 0) {
                printf("(未检测到外部物理磁盘)\n");
            }
            for (int i = 0; i < cnt; i++) {
                char info_cmd[128];
                snprintf(info_cmd, sizeof(info_cmd), "diskutil info %s 2>&1", ids[i]);
                char *info = run_cmd(info_cmd);
                printf("\n--- disk %s ---\n", ids[i]);
                if (info && info[0]) {
                    /* 显示完整输出 (一般 < 2KB) */
                    size_t len = strlen(info);
                    size_t show = len > 3000 ? 3000 : len;
                    printf("%.*s\n", (int)show, info);
                    if (len > 3000) printf("... (省略 %zu 字节)\n", len - 3000);
                } else {
                    printf("(无输出)\n");
                }
                free(info);
                free(ids[i]);
            }
            free(ids);
        } else {
            printf("(diskutil list external physical 无输出)\n");
        }
        free(out);
    }

    /* 运行 get_usb_devices 进行最终检测 */
    printf("\n========================================\n");
    printf("  最终检测结果\n");
    printf("========================================\n");
    USBDevice *devs = NULL;
    int cnt = 0;
    get_usb_devices(&devs, &cnt);

    if (cnt > 0) {
        printf("  检测到 %d 个 USB 存储设备:\n", cnt);
        printf("  %-4s %-30s %-20s\n", "序号", "名称", "序列号");
        printf("  %-4s %-30s %-20s\n", "----", "------------------------------", "--------------------");
        for (int i = 0; i < cnt; i++) {
            printf("  %-4d %-30s %-20s\n", i + 1,
                devs[i].name ? devs[i].name : "USB 存储设备",
                devs[i].serial ? devs[i].serial : "");
        }
    } else {
        printf("  未检测到 USB 存储设备\n");
    }

    printf("\n----------------------------------------\n");
    if (cnt > 0) {
        printf("  诊断完成: 共检测到 %d 个 USB 存储设备\n", cnt);
    } else {
        printf("  诊断完成: 未检测到 USB 存储设备\n");
        printf("  建议:\n");
        printf("    1. 查看 [5/5] 输出, 确认外部磁盘的 Protocol 是否为 USB\n");
        printf("       (SATA/Virtual/PCI 等协议的磁盘会被跳过, 这是预期行为)\n");
        printf("    1b. 查看 [3/5] 输出, 确认 USB 设备树中是否有 USB Serial Number 属性\n");
        printf("        (部分廉价 USB 设备不暴露序列号, 这是设备限制非代码问题)\n");
        printf("    2. 确认 USB 存储设备已正确插入\n");
        printf("    3. 在「磁盘工具」中查看设备是否被识别\n");
        printf("    4. 在「系统信息」-> USB 中查看设备树\n");
        printf("    5. 尝试重新插拔 USB 设备或更换 USB 端口\n");
        printf("    6. 虚拟机环境 (VMware/VirtualBox) 通常无真实 USB 控制器,\n");
        printf("       需通过 USB 直通方式将宿主机 USB 设备映射进虚拟机\n");
    }
    printf("========================================\n");

    /* 释放设备列表 */
    for (int i = 0; i < cnt; i++) {
        free(devs[i].name);
        free(devs[i].model);
        free(devs[i].serial);
    }
    free(devs);
}

#endif /* __APPLE__ */
