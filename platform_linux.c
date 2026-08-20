/* platform_linux.c - Linux 平台硬件信息采集 (从 Go platform_linux.go 移植) */
#ifdef __linux__

#define _GNU_SOURCE
#include "dm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

/* ====== 内部辅助函数 ====== */

/* 读取文件首行内容, 去除首尾空白, 返回堆分配字符串 (打开失败返回 NULL) */
static char *read_file_first_line(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    char buf[1024];
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    /* device-tree 等文件末尾可能带 \0, fgets 读入后 strlen 会自动截断;
       再做一次 str_trim 去除首尾空白 */
    return str_trim(strdup(buf));
}

/* 读取 sysfs/文件中的序列号并校验有效性 (无效或读取失败返回 NULL) */
static char *read_valid_serial(const char *path) {
    char *s = read_file_first_line(path);
    if (!s) return NULL;
    if (s[0] == '\0' || is_invalid_serial(s)) {
        free(s);
        return NULL;
    }
    return s;
}

/* 去除字符串两端的单双引号 (类似 Go strings.Trim(s, "\"'")) */
static void trim_quotes(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    /* 去除尾部引号 */
    while (len > 0 && (s[len - 1] == '"' || s[len - 1] == '\'')) {
        s[--len] = '\0';
    }
    /* 去除头部引号并左移 */
    char *start = s;
    while (*start == '"' || *start == '\'') start++;
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
}

/* 从日期时间字符串中提取前 10 字符 (YYYY-MM-DD), 失败返回 NULL */
static char *extract_date(const char *s) {
    char *t = str_trim(strdup(s));
    if (!t) return NULL;
    if (strlen(t) >= 10) {
        char *d = (char *)malloc(11);
        memcpy(d, t, 10);
        d[10] = '\0';
        free(t);
        return d;
    }
    free(t);
    return NULL;
}

/* 不区分大小写的子串查找, 返回匹配位置指针或 NULL */
static char *str_case_str(const char *s, const char *sub) {
    size_t slen = strlen(s), sublen = strlen(sub);
    if (sublen == 0 || sublen > slen) return NULL;
    for (size_t i = 0; i <= slen - sublen; i++) {
        size_t j = 0;
        for (; j < sublen; j++) {
            if (tolower((unsigned char)s[i + j]) != tolower((unsigned char)sub[j])) break;
        }
        if (j == sublen) return (char *)(s + i);
    }
    return NULL;
}

/* 判断字符串是否以指定后缀结尾 */
static int str_ends_with(const char *s, const char *suffix) {
    size_t slen = strlen(s), suflen = strlen(suffix);
    if (suflen > slen) return 0;
    return strcmp(s + slen - suflen, suffix) == 0;
}

/* 先无 sudo 执行, 失败时用 sudo 回退 (支持 /api/sudo 密码) */
static char *run_with_sudo(const char *cmd) {
    char plain[1024];
    snprintf(plain, sizeof(plain), "%s 2>/dev/null", cmd);
    char *out = run_cmd(plain);
    if (out && out[0] != '\0') return out;
    free(out);
    char *sc = build_sudo_cmd(cmd);
    out = run_cmd(sc);
    free(sc);
    return out;
}

/* ====== lsblk 解析 ====== */

typedef struct {
    char *serial;
    char *tran;
    char *model;
} lsblk_entry;

/* 从 "KEY="value" ..." 格式的行中提取指定 KEY 的值 (堆分配, 未找到返回 NULL) */
static char *extract_pair(const char *line, const char *key) {
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "%s=\"", key);
    const char *p = strstr(line, prefix);
    if (!p) return NULL;
    p += strlen(prefix);
    const char *end = strchr(p, '"');
    if (!end) return strdup(p);
    size_t len = (size_t)(end - p);
    char *val = (char *)malloc(len + 1);
    memcpy(val, p, len);
    val[len] = '\0';
    return val;
}

/* 解析 lsblk -P 输出, 填入 entries 数组, 返回条目数 (会就地修改 out) */
static int parse_lsblk(char *out, lsblk_entry *entries, int max_entries) {
    int count = 0;
    char *saveptr = NULL;
    if (!out) return 0;
    char *line = strtok_r(out, "\n", &saveptr);
    while (line && count < max_entries) {
        char *t = str_trim(line);
        if (t[0] != '\0') {
            char *serial = extract_pair(t, "SERIAL");
            /* 仅保留序列号非空且不为 "0" 的条目 */
            if (serial && serial[0] != '\0' && strcmp(serial, "0") != 0) {
                char *tran = extract_pair(t, "TRAN");
                char *model = extract_pair(t, "MODEL");
                entries[count].serial = serial;
                entries[count].tran = tran ? tran : strdup("");
                entries[count].model = model ? model : strdup("");
                count++;
            } else {
                free(serial);
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    return count;
}

/* 调用 lsblk 获取磁盘条目 (序列号/传输类型/型号), 返回条目数 */
static int get_lsblk_entries(lsblk_entry *entries, int max_entries) {
    char *out = run_cmd("lsblk -d -o SERIAL,TRAN,MODEL -P");
    int n = parse_lsblk(out, entries, max_entries);
    free(out);
    return n;
}

/* 释放 lsblk_entry 数组中的字符串 */
static void free_lsblk_entries(lsblk_entry *entries, int n) {
    for (int i = 0; i < n; i++) {
        free(entries[i].serial);
        free(entries[i].tran);
        free(entries[i].model);
    }
}

/* ====== 平台函数实现 ====== */

/* 获取 Linux 操作系统版本: /etc/os-release → lsb_release -d → "Linux" */
char *get_os_version(void) {
    FILE *fp = fopen("/etc/os-release", "r");
    if (fp) {
        char name[256] = {0};
        char version[128] = {0};
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            str_trim(line);
            if (str_starts_with(line, "NAME=")) {
                char *val = line + 5;  /* 跳过 "NAME=" */
                trim_quotes(val);
                strncpy(name, val, sizeof(name) - 1);
                name[sizeof(name) - 1] = '\0';
            } else if (str_starts_with(line, "VERSION_ID=")) {
                char *val = line + 11;  /* 跳过 "VERSION_ID=" */
                trim_quotes(val);
                strncpy(version, val, sizeof(version) - 1);
                version[sizeof(version) - 1] = '\0';
            }
        }
        fclose(fp);
        if (name[0] != '\0') {
            if (version[0] != '\0') {
                char result[400];
                snprintf(result, sizeof(result), "%s %s", name, version);
                return strdup(result);
            }
            return strdup(name);
        }
    }
    /* 回退: lsb_release -d, 取冒号后的描述 */
    char *out = run_cmd("lsb_release -d");
    if (!out) return strdup("Linux");
    char *colon = strchr(out, ':');
    if (colon) {
        char *val = str_trim(strdup(colon + 1));
        free(out);
        if (val && val[0] != '\0') return val;
        free(val);
    } else {
        free(out);
    }
    return strdup("Linux");
}

/* 获取系统安装日期: stat / → stat /etc → /var/log 安装日志 → "未知" */
char *get_os_install_date(void) {
    /* 方式1: stat / 的 birth time (文件系统创建时间, 最准确) */
    char *out = run_cmd("stat / -c %w");
    if (out && out[0] != '\0' && strcmp(out, "-") != 0) {
        char *d = extract_date(out);
        free(out);
        if (d) return d;
    } else {
        free(out);
    }
    /* 方式2: stat /etc 的修改时间 */
    out = run_cmd("stat /etc -c %y");
    if (out && out[0] != '\0') {
        char *d = extract_date(out);
        free(out);
        if (d) return d;
    } else {
        free(out);
    }
    /* 方式3: /var/log 下的安装日志修改时间 */
    const char *paths[] = {"/var/log/installer", "/var/log/anaconda", "/var/log/yum.log"};
    for (int i = 0; i < 3; i++) {
        struct stat st;
        if (stat(paths[i], &st) == 0) {
            struct tm *tm = localtime(&st.st_mtime);
            if (tm) {
                char buf[32];
                strftime(buf, sizeof(buf), "%Y-%m-%d", tm);
                return strdup(buf);
            }
        }
    }
    return strdup("未知");
}

/* 获取设备序列号 (多级回退: sysfs → dmidecode → 设备树 → UUID → machine-id) */
char *get_device_serial(void) {
    /* a/b/c: DMI sysfs 序列号文件 (product/board/chassis) */
    const char *sysfs_paths[] = {
        "/sys/class/dmi/id/product_serial",
        "/sys/class/dmi/id/board_serial",
    };
    for (int i = 0; i < 2; i++) {
        char *s = read_valid_serial(sysfs_paths[i]);
        if (s) return s;
    }

    /* d/e/f: dmidecode -s (run_with_sudo: 先无 sudo, 失败用 sudo -S 密码) */
    const char *dtypes[] = {"system-serial-number", "baseboard-serial-number"};
    for (int i = 0; i < 2; i++) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "dmidecode -s %s", dtypes[i]);
        char *out = run_with_sudo(cmd);
        if (!out) continue;
        char *t = str_trim(out);
        if (t[0] != '\0' && !is_invalid_serial(t)) {
            char *result = strdup(t);
            free(out);
            return result;
        }
        free(out);
    }

    /* g: 解析 dmidecode -t 1,2,3 中的 "Serial Number:" 等行 */
    {
        char *out = run_with_sudo("dmidecode -t 1,2,3");
        if (!out) out = strdup("");
        char *copy = strdup(out);
        free(out);
        const char *keys[] = {"Serial Number:", "Board Serial:"};
        char *saveptr = NULL;
        char *line = strtok_r(copy, "\n", &saveptr);
        while (line) {
            char *t = str_trim(line);
            for (int k = 0; k < 2; k++) {
                if (str_starts_with(t, keys[k])) {
                    char *val = str_trim(strdup(t + strlen(keys[k])));
                    if (val && val[0] != '\0' && !is_invalid_serial(val)) {
                        free(copy);
                        return val;
                    }
                    free(val);
                }
            }
            line = strtok_r(NULL, "\n", &saveptr);
        }
        free(copy);
    }

    /* h: 树莓派等设备的设备树序列号 */
    {
        char *s = read_valid_serial("/proc/device-tree/serial-number");
        if (s) return s;
    }

    /* i/j: product_uuid 和 system-uuid (run_with_sudo 自动先无 sudo 后 sudo) */
    {
        const char *cmds[] = {
            "cat /sys/class/dmi/id/product_uuid",
            "dmidecode -s system-uuid"
        };
        for (int i = 0; i < 2; i++) {
            char *out = run_with_sudo(cmds[i]);
            if (!out) continue;
            char *t = str_trim(out);
            if (t[0] != '\0' && !is_invalid_serial(t)) {
                char *result = strdup(t);
                free(out);
                return result;
            }
            free(out);
        }
    }

    /* k: 解析 dmidecode -t 1 中的 "UUID:" 行 */
    {
        char *out = run_with_sudo("dmidecode -t 1");
        if (!out) out = strdup("");
        char *copy = strdup(out);
        free(out);
        char *saveptr = NULL;
        char *line = strtok_r(copy, "\n", &saveptr);
        while (line) {
            char *t = str_trim(line);
            if (str_starts_with(t, "UUID:")) {
                char *val = str_trim(strdup(t + 5));  /* 跳过 "UUID:" */
                if (val && val[0] != '\0' && !is_invalid_serial(val)) {
                    free(copy);
                    return val;
                }
                free(val);
            }
            line = strtok_r(NULL, "\n", &saveptr);
        }
        free(copy);
    }

    /* l: machine-id (非 root 可读, 作为最后回退) */
    {
        const char *paths[] = {"/etc/machine-id", "/var/lib/dbus/machine-id"};
        for (int i = 0; i < 2; i++) {
            char *s = read_file_first_line(paths[i]);
            if (s && s[0] != '\0') return s;
            free(s);
        }
    }

    return strdup("未知");
}

/* 获取硬盘序列号 (排除 USB 设备) */
char *get_hdd_serials(void) {
    char *serials[64];
    int count = 0;

    /* 方式1: lsblk (排除 TRAN=usb) */
    {
        lsblk_entry entries[64];
        int n = get_lsblk_entries(entries, 64);
        for (int i = 0; i < n; i++) {
            if (!str_eq_ignore_case(entries[i].tran, "usb")) {
                if (count < 64) serials[count++] = strdup(entries[i].serial);
            }
        }
        free_lsblk_entries(entries, n);
    }

    /* 方式2: /sys/block/<dev>/device/serial (跳过 loop/ram/USB) */
    if (count == 0) {
        DIR *d = opendir("/sys/block");
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                const char *name = de->d_name;
                if (str_starts_with(name, "loop") || str_starts_with(name, "ram")) continue;
                /* 通过 device/subsystem 符号链接判断是否 USB 设备 */
                char subpath[512];
                snprintf(subpath, sizeof(subpath), "/sys/block/%s/device/subsystem", name);
                char linkbuf[512];
                ssize_t linklen = readlink(subpath, linkbuf, sizeof(linkbuf) - 1);
                if (linklen > 0) {
                    linkbuf[linklen] = '\0';
                    if (str_ends_with(linkbuf, "usb")) continue;  /* 跳过 USB */
                }
                char spath[512];
                snprintf(spath, sizeof(spath), "/sys/block/%s/device/serial", name);
                char *s = read_file_first_line(spath);
                if (s && s[0] != '\0' && strcmp(s, "0") != 0) {
                    if (count < 64) serials[count++] = s;
                    else free(s);
                } else {
                    free(s);
                }
            }
            closedir(d);
        }
    }

    /* 方式3: smartctl (遍历 sd?/nvme?n?, grep "Serial Number") */
    if (count == 0) {
        char *out = run_cmd("sh -c 'for dev in /dev/sd? /dev/nvme?n?; do [ -e \"$dev\" ] && smartctl -i \"$dev\" 2>/dev/null | grep -i \"Serial Number\"; done'");
        if (!out) out = strdup("");
        char *copy = strdup(out);
        free(out);
        char *saveptr = NULL;
        char *line = strtok_r(copy, "\n", &saveptr);
        while (line) {
            char *p = str_case_str(line, "serial number:");
            if (p) {
                char *val = str_trim(strdup(p + strlen("serial number:")));
                if (val && val[0] != '\0') {
                    if (count < 64) serials[count++] = val;
                    else free(val);
                } else {
                    free(val);
                }
            }
            line = strtok_r(NULL, "\n", &saveptr);
        }
        free(copy);
    }

    /* 合并所有序列号 (str_join 在全部为空时返回 NULL) */
    char *result = str_join(serials, count, ";");
    for (int i = 0; i < count; i++) free(serials[i]);
    if (!result) return strdup("未检测到");
    return result;
}

/* 获取 USB 存储设备列表 (含名称/型号/序列号) */
void get_usb_devices(USBDevice **devices, int *count) {
    *devices = NULL;
    *count = 0;

    USBDevice tmp[64];
    int n = 0;

    /* 方式1: lsblk 查找 TRAN=usb */
    {
        lsblk_entry entries[64];
        int en = get_lsblk_entries(entries, 64);
        for (int i = 0; i < en && n < 64; i++) {
            if (str_eq_ignore_case(entries[i].tran, "usb")) {
                const char *dname = entries[i].model[0] ? entries[i].model : "USB 存储设备";
                tmp[n].name = strdup(dname);
                tmp[n].model = strdup(entries[i].model);
                tmp[n].serial = strdup(entries[i].serial);
                n++;
            }
        }
        free_lsblk_entries(entries, en);
    }

    /* 方式2: 扫描 /sys/block 查找 usb 子系统设备 */
    if (n == 0) {
        DIR *d = opendir("/sys/block");
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL && n < 64) {
                const char *name = de->d_name;
                if (str_starts_with(name, "loop") || str_starts_with(name, "ram")) continue;
                char subpath[512];
                snprintf(subpath, sizeof(subpath), "/sys/block/%s/device/subsystem", name);
                char linkbuf[512];
                ssize_t linklen = readlink(subpath, linkbuf, sizeof(linkbuf) - 1);
                if (linklen <= 0) continue;
                linkbuf[linklen] = '\0';
                if (!str_ends_with(linkbuf, "usb")) continue;
                char spath[512], mpath[512];
                snprintf(spath, sizeof(spath), "/sys/block/%s/device/serial", name);
                snprintf(mpath, sizeof(mpath), "/sys/block/%s/device/model", name);
                char *serial = read_file_first_line(spath);
                char *model = read_file_first_line(mpath);
                if (serial && serial[0] != '\0' && strcmp(serial, "0") != 0) {
                    const char *dname = (model && model[0]) ? model : "USB 存储设备";
                    tmp[n].name = strdup(dname);
                    tmp[n].model = strdup(model ? model : "");
                    tmp[n].serial = strdup(serial);
                    n++;
                }
                free(serial);
                free(model);
            }
            closedir(d);
        }
    }

    /* 方式3: udevadm 查找 USB 存储设备 (lsblk/sys 解析失败时的回退) */
    if (n == 0) {
        DIR *d = opendir("/dev");
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL && n < 64) {
                const char *name = de->d_name;
                if (!str_starts_with(name, "sd")) continue;
                char devpath[256];
                snprintf(devpath, sizeof(devpath), "/dev/%s", name);
                char cmd[512];
                snprintf(cmd, sizeof(cmd), "udevadm info --query=property --name=%s 2>/dev/null", devpath);
                char *out = run_cmd(cmd);
                if (out && out[0]) {
                    int is_usb = (strstr(out, "ID_BUS=usb") != NULL);
                    char *serial = NULL;
                    char *model = NULL;
                    char *sp = strstr(out, "ID_SERIAL_SHORT=");
                    if (sp) {
                        sp += strlen("ID_SERIAL_SHORT=");
                        char *nl = strchr(sp, '\n');
                        if (nl) *nl = '\0';
                        char *t = str_trim(strdup(sp));
                        if (t && t[0] != '\0' && strcmp(t, "0") != 0) serial = t;
                        else free(t);
                    }
                    char *mp = strstr(out, "ID_MODEL=");
                    if (mp) {
                        mp += strlen("ID_MODEL=");
                        char *nl = strchr(mp, '\n');
                        if (nl) *nl = '\0';
                        model = str_trim(strdup(mp));
                    }
                    if (is_usb && serial) {
                        const char *dname = (model && model[0]) ? model : "USB 存储设备";
                        tmp[n].name = strdup(dname);
                        tmp[n].model = strdup(model ? model : "");
                        tmp[n].serial = strdup(serial);
                        n++;
                    }
                    free(serial);
                    free(model);
                }
                free(out);
            }
            closedir(d);
        }
    }

    if (n == 0) {
        *devices = NULL;
        *count = 0;
        return;
    }

    /* 拷贝到堆分配数组中返回 */
    USBDevice *arr = (USBDevice *)malloc(sizeof(USBDevice) * n);
    for (int i = 0; i < n; i++) {
        arr[i] = tmp[i];
    }
    *devices = arr;
    *count = n;
}

#endif /* __linux__ */
