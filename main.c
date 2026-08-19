#ifndef _WIN32
#define _DEFAULT_SOURCE
#endif

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
    #include <shlobj.h>
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "shell32.lib")
    typedef SOCKET socket_t;
    #define INVALID_SOCK INVALID_SOCKET
    #define sock_close closesocket
    #define sock_errno WSAGetLastError()
#else
    #include <unistd.h>
    #include <fcntl.h>
    #include <sys/file.h>
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <signal.h>
    #include <pwd.h>
    typedef int socket_t;
    #define INVALID_SOCK (-1)
    #define sock_close close
    #define sock_errno errno
#endif

/* ====== 全局变量 ====== */

static int g_port = 0;
static int g_headless = 0;
static int g_once = 0;
static int g_no_browser = 0;
static int g_json_output = 0;
static char g_json_path[512] = {0};
static volatile int g_running = 1;
static socket_t g_listen_fd = INVALID_SOCK;

/* 单例锁句柄: 进程退出时自动释放 */
#ifdef _WIN32
static HANDLE g_mutex = NULL;
#else
static int g_lock_fd = -1;
#endif

/* 嵌入的 HTML (由 Makefile 生成 web_ui.h, 或运行时读取) */
#ifdef USE_EMBEDDED_HTML
#include "web_ui.h"
#include "logo_png.h"
#else
static char *g_html_data = NULL;
static size_t g_html_len = 0;
#endif

/* ====== 前端 HTML (运行时从文件读取的兜底) ====== */

static const char *FALLBACK_HTML =
"<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
"<title>DeviceMate</title></head><body>"
"<p>请将 index.html 放在程序同目录下</p>"
"</body></html>";

static void load_html(void) {
#ifdef USE_EMBEDDED_HTML
    return; /* 编译时嵌入, 无需加载 */
#else
    /* 尝试从可执行文件同目录读取 index.html */
    char path[1024] = {0};
#ifdef _WIN32
    GetModuleFileNameA(NULL, path, sizeof(path) - 32);
    char *p = strrchr(path, '\\');
    if (p) p[1] = '\0'; else path[0] = '\0';
#else
    readlink("/proc/self/exe", path, sizeof(path) - 32);
    char *p = strrchr(path, '/');
    if (p) p[1] = '\0'; else strcpy(path, "./");
#endif
    strcat(path, "index.html");
    FILE *f = fopen(path, "rb");
    if (!f) {
        /* 尝试当前工作目录 */
        f = fopen("index.html", "rb");
    }
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0) {
            g_html_data = (char *)malloc(sz + 1);
            g_html_len = fread(g_html_data, 1, sz, f);
            g_html_data[g_html_len] = '\0';
        }
        fclose(f);
    }
#endif
}

static const char *get_html(size_t *len) {
#ifdef USE_EMBEDDED_HTML
    *len = sizeof(index_html);
    return (const char *)index_html;
#else
    if (g_html_data) {
        *len = g_html_len;
        return g_html_data;
    }
    *len = strlen(FALLBACK_HTML);
    return FALLBACK_HTML;
#endif
}

/* 嵌入的 logo 图片 (/logo.png) */
static const char *get_logo(size_t *len) {
#ifdef USE_EMBEDDED_HTML
    *len = sizeof(logo_png);
    return (const char *)logo_png;
#else
    *len = 0;
    return NULL;
#endif
}

/* ====== 浏览器打开 ====== */

static void open_browser(const char *url) {
#ifdef _WIN32
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "open '%s' 2>/dev/null", url);
    system(cmd);
#else
    const char *browsers[] = {"xdg-open", "gio", "gvfs-open", "kde-open", "wslview", NULL};
    for (int i = 0; browsers[i]; i++) {
        char cmd[600];
        snprintf(cmd, sizeof(cmd), "%s '%s' 2>/dev/null", browsers[i], url);
        if (system(cmd) == 0) return;
    }
#endif
}

/* ====== 单例锁 ====== */

static void get_lock_path(char *buf, int bufsize) {
#ifdef _WIN32
    char appdata[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata))) {
        snprintf(buf, bufsize, "%s\\DeviceMate\\devicemate.lock", appdata);
    } else {
        snprintf(buf, bufsize, "%s\\devicemate.lock", getenv("TEMP") ? getenv("TEMP") : "/tmp");
    }
#else
    const char *home = getenv("HOME");
    if (!home) home = getpwuid(getuid())->pw_dir;
#ifdef __APPLE__
    snprintf(buf, bufsize, "%s/Library/Application Support/DeviceMate/devicemate.lock", home);
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0])
        snprintf(buf, bufsize, "%s/DeviceMate/devicemate.lock", xdg);
    else
        snprintf(buf, bufsize, "%s/.config/DeviceMate/devicemate.lock", home);
#endif
#endif
}

static void ensure_dir(const char *path) {
    char dir[512];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *p = strrchr(dir, PATH_SEP);
    if (p) {
        *p = '\0';
#ifdef _WIN32
        /* 使用 SHCreateDirectoryExA 替代 system("mkdir"), 避免控制台窗口闪烁 */
        int ret = SHCreateDirectoryExA(NULL, dir, NULL);
        (void)ret;  /* ERROR_SUCCESS=0 或 ERROR_ALREADY_EXISTS=183 均可接受 */
#else
        char cmd[600];
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s' 2>/dev/null", dir);
        system(cmd);
#endif
    }
}

static int try_connect_existing(void) {
    char lock_path[512];
    get_lock_path(lock_path, sizeof(lock_path));
    FILE *f = fopen(lock_path, "r");
    if (!f) return 0;
    char port_str[32] = {0};
    if (!fgets(port_str, sizeof(port_str), f)) { fclose(f); return 0; }
    fclose(f);
    str_trim(port_str);
    if (!port_str[0]) return 0;

    /* 尝试连接已有实例 */
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%s/", port_str);

#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    int result = 0;
    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCK) goto done;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(atoi(port_str));

    /* 设置超时 */
#ifdef _WIN32
    DWORD timeout = 1500;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
#else
    struct timeval tv = {1, 500000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        sock_close(sock);
        goto done;
    }
    /* 发送 HTTP GET */
    const char *req = "GET / HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    send(sock, req, strlen(req), 0);
    char resp[256];
    int n = recv(sock, resp, sizeof(resp) - 1, 0);
    sock_close(sock);

    if (n <= 0) goto done;
    resp[n] = '\0';
    if (!str_contains(resp, "200")) goto done;

    if (!g_headless) {
        open_browser(url);
        printf("检测到程序已在运行, 已打开已有实例的浏览器页面\n");
    } else {
        printf("检测到程序已在运行 (端口 %s), API 地址: %sapi/collect\n", port_str, url);
    }
    result = 1;

done:
#ifdef _WIN32
    WSACleanup();
#endif
    return result;
}

static void write_lock_file(int port) {
    char lock_path[512];
    get_lock_path(lock_path, sizeof(lock_path));
    ensure_dir(lock_path);
    FILE *f = fopen(lock_path, "w");
    if (f) {
        fprintf(f, "%d", port);
        fclose(f);
    }
}

static void remove_lock_file(void) {
    char lock_path[512];
    get_lock_path(lock_path, sizeof(lock_path));
    remove(lock_path);
}

/* 尝试获取独占锁, 成功返回 1 (锁保持到进程退出), 失败返回 0 (已有实例运行)
   Windows 用命名互斥量, Linux/macOS 用 flock 文件锁, 进程退出自动释放 */
static int try_acquire_lock(void) {
    char lock_path[512];
    get_lock_path(lock_path, sizeof(lock_path));
    ensure_dir(lock_path);

#ifdef _WIN32
    g_mutex = CreateMutexA(NULL, TRUE, "DeviceMate_SingleInstance");
    if (g_mutex == NULL) return 0;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_mutex);
        g_mutex = NULL;
        return 0;
    }
    return 1;
#else
    g_lock_fd = open(lock_path, O_CREAT | O_RDWR, 0644);
    if (g_lock_fd < 0) return 0;
    if (flock(g_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        close(g_lock_fd);
        g_lock_fd = -1;
        return 0;
    }
    return 1;
#endif
}

/* ====== 信号处理 ====== */

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
    if (g_listen_fd != INVALID_SOCK) {
        sock_close(g_listen_fd);
        g_listen_fd = INVALID_SOCK;
    }
    /* 单例锁在进程退出时由 OS 自动释放, 无需手动清理 */
    exit(0);
}

/* ====== 格式化输出 ====== */

static void print_formatted(const DeviceInfo *info) {
    printf("========================================\n");
    printf("  DeviceMate 设备信息采集结果\n");
    printf("========================================\n");
    printf("  采集时间:         %s\n", info->collect_time);
    printf("----------------------------------------\n");
    printf("  操作系统类型:     %s\n", info->os_name);
    printf("  操作系统信息:     %s\n", info->os_version);
    printf("  操作系统安装日期: %s\n", info->os_install_date);
    printf("----------------------------------------\n");
    printf("  设备序列号:       %s\n", info->device_serial);
    printf("  硬盘序列号:       %s\n", info->hdd_serials);
    printf("  IP 地址:          %s\n", info->ip_addresses);
    printf("  MAC 地址:         %s\n", info->mac_addresses);
    printf("----------------------------------------\n");
    if (info->usb_count > 0) {
        printf("  USB 存储设备:\n");
        for (int i = 0; i < info->usb_count; i++) {
            printf("    [%d] %s | 序列号: %s\n", i + 1,
                info->usb_devices[i].name ? info->usb_devices[i].name : "USB 存储设备",
                info->usb_devices[i].serial ? info->usb_devices[i].serial : "");
        }
    } else {
        printf("  USB 存储设备:     未检测到\n");
    }
    printf("========================================\n");
}

static void save_json(const DeviceInfo *info, const char *path) {
    char *json = info_to_json(info);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(json, f);
        fclose(f);
        printf("已保存到: %s\n", path);
    } else {
        fprintf(stderr, "保存 JSON 失败: 无法写入 %s\n", path);
    }
    free(json);
}

/* ====== 帮助信息 ====== */

static void show_help(void) {
    printf("DeviceMate - 跨平台设备信息采集工具 (C 版)\n\n");
    printf("用法:\n");
    printf("  DeviceMate                  # GUI 模式 (自动打开浏览器)\n");
    printf("  DeviceMate --headless       # 无桌面模式\n");
    printf("  DeviceMate --once           # 一次性采集并输出到控制台\n");
    printf("  DeviceMate --once --json result.json  # 采集并保存为 JSON\n");
    printf("  DeviceMate --port 8080       # 指定端口启动 API 服务\n");
    printf("  DeviceMate --no-browser      # 不自动打开浏览器\n\n");
    printf("参数:\n");
    printf("  --headless    无桌面模式 (Linux 无 DISPLAY 时自动启用)\n");
    printf("  --once        一次性模式: 采集后输出并退出\n");
    printf("  --json PATH   将采集结果保存为 JSON 文件\n");
    printf("  --port N      指定端口 (0=自动分配)\n");
    printf("  --no-browser  不自动打开浏览器\n");
    printf("  --help        显示帮助信息\n");
}

/* ====== 参数解析 ====== */

static void parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            show_help();
            exit(0);
        } else if (strcmp(argv[i], "--headless") == 0) {
            g_headless = 1;
        } else if (strcmp(argv[i], "--once") == 0) {
            g_once = 1;
        } else if (strcmp(argv[i], "--no-browser") == 0) {
            g_no_browser = 1;
        } else if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
            g_json_output = 1;
            strncpy(g_json_path, argv[++i], sizeof(g_json_path) - 1);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_port = atoi(argv[++i]);
        } else if (strncmp(argv[i], "--port=", 7) == 0) {
            g_port = atoi(argv[i] + 7);
        } else if (strncmp(argv[i], "--json=", 7) == 0) {
            g_json_output = 1;
            strncpy(g_json_path, argv[i] + 7, sizeof(g_json_path) - 1);
        }
    }
}

static int detect_headless(void) {
    if (g_headless || g_once || g_no_browser) return 1;
#ifndef _WIN32
    /* Linux: 无 DISPLAY 环境变量时自动启用 */
    #ifdef __linux__
    if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) return 1;
    #endif
#endif
    return 0;
}

/* ====== HTTP 服务器 ====== */

static void send_response(socket_t client, int status, const char *content_type,
                          const char *body, size_t body_len) {
    char header[512];
    const char *status_text = (status == 200) ? "OK" : "Not Found";
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.0 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        status, status_text, content_type, body_len);
    send(client, header, hlen, 0);
    if (body && body_len > 0) {
        /* 分块发送 (大内容) */
        size_t sent = 0;
        while (sent < body_len) {
            int n = send(client, body + sent, body_len - sent, 0);
            if (n <= 0) break;
            sent += n;
        }
    }
}

static void handle_request(socket_t client, const char *method, const char *path, const char *buf) {
    /* 允许 GET 和 POST */
    if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0) {
        const char *msg = "{\"error\":\"method not allowed\"}";
        send_response(client, 404, "application/json", msg, strlen(msg));
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/sudo") == 0) {
        /* 设置 sudo 密码: 从请求体中提取 */
        const char *body = strstr(buf, "\r\n\r\n");
        if (body) {
            body += 4;
            strncpy(g_sudo_password, body, sizeof(g_sudo_password) - 1);
            g_sudo_password[sizeof(g_sudo_password) - 1] = '\0';
            /* 去除尾部换行 */
            char *nl = strpbrk(g_sudo_password, "\r\n");
            if (nl) *nl = '\0';
        }
        const char *msg = "{\"ok\":true}";
        send_response(client, 200, "application/json", msg, strlen(msg));
        return;
    }

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        size_t html_len = 0;
        const char *html = get_html(&html_len);
        send_response(client, 200, "text/html; charset=utf-8", html, html_len);
    } else if (strcmp(path, "/logo.png") == 0) {
        size_t logo_len = 0;
        const char *logo = get_logo(&logo_len);
        send_response(client, 200, "image/png", logo, logo_len);
    } else if (strcmp(path, "/api/collect") == 0) {
        DeviceInfo info = collect_all();
        char *json = info_to_json(&info);
        send_response(client, 200, "application/json; charset=utf-8", json, strlen(json));
        free(json);
        free_info(&info);
    } else if (strcmp(path, "/api/health") == 0) {
        const char *msg = "{\"status\":\"ok\"}";
        send_response(client, 200, "application/json", msg, strlen(msg));
    } else if (strcmp(path, "/api/shutdown") == 0) {
        const char *msg = "{\"status\":\"ok\"}";
        send_response(client, 200, "application/json", msg, strlen(msg));
        g_running = 0;
    } else {
        const char *msg = "{\"error\":\"not found\"}";
        send_response(client, 404, "application/json", msg, strlen(msg));
    }
}

static int start_server(void) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup 失败\n");
        return -1;
    }
#endif

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd == INVALID_SOCK) {
        fprintf(stderr, "无法创建 socket\n");
        return -1;
    }

    /* 允许端口复用 */
    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(g_port);
    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "无法绑定端口 %d\n", g_port);
        sock_close(g_listen_fd);
        return -1;
    }

    if (listen(g_listen_fd, 5) != 0) {
        fprintf(stderr, "listen 失败\n");
        sock_close(g_listen_fd);
        return -1;
    }

    /* 获取实际端口 */
    if (g_port == 0) {
        struct sockaddr_in actual;
        socklen_t addrlen = sizeof(actual);
        getsockname(g_listen_fd, (struct sockaddr *)&actual, &addrlen);
        g_port = ntohs(actual.sin_port);
    }

    return 0;
}

static void server_loop(void) {
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        socket_t client = accept(g_listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client == INVALID_SOCK) {
            if (!g_running) break;
            continue;
        }

        /* 读取请求 */
        char buf[4096] = {0};
        int n = recv(client, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            /* 解析 method 和 path */
            char method[16] = {0};
            char path[256] = {0};
            sscanf(buf, "%15s %255s", method, path);
            handle_request(client, method, path, buf);
        }

        sock_close(client);

        /* 如果收到 shutdown 请求 */
        if (!g_running) {
#ifdef _WIN32
            Sleep(300);
#else
            usleep(300000);
#endif
            break;
        }
    }

    if (g_listen_fd != INVALID_SOCK) {
        sock_close(g_listen_fd);
        g_listen_fd = INVALID_SOCK;
    }
#ifdef _WIN32
    WSACleanup();
#endif
}

/* ====== main ====== */

int main(int argc, char **argv) {
#ifdef _WIN32
    /* 设置控制台为 UTF-8 编码, 解决中文乱码 */
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    parse_args(argc, argv);
    int headless = detect_headless();
    /* 同步全局变量, 使 try_connect_existing 能正确判断是否打开浏览器 */
    g_headless = headless;
    load_html();

#ifdef _WIN32
    /* GUI 模式下隐藏控制台窗口 (即使 -mwindows 未生效也能隐藏) */
    if (!headless) {
        HWND console = GetConsoleWindow();
        if (console) ShowWindow(console, SW_HIDE);
    }
#endif

    /* 一次性模式 或 无桌面环境(未指定 --port) */
    if (g_once || (headless && g_port == 0)) {
        DeviceInfo info = collect_all();
        print_formatted(&info);
        if (g_json_output) {
            save_json(&info, g_json_path);
        }
        if (headless && !g_once && g_port == 0) {
            printf("\n(检测到无桌面环境, 已自动退出。如需启动 API 服务请使用 --port 参数)\n");
        }
        free_info(&info);
        return 0;
    }

    /* 单例检测: 若已有实例在运行, 仅打开浏览器, 不启动新进程 */
    if (try_connect_existing()) {
        return 0;
    }

    /* 获取独占锁, 确保只有一个实例 (原子操作, 无竞态) */
    if (!try_acquire_lock()) {
        /* 锁失败: 另一实例刚启动, 等待其写入端口后重试连接 */
#ifdef _WIN32
        Sleep(500);
#else
        usleep(500000);
#endif
        if (try_connect_existing()) {
            return 0;
        }
        /* 仍无法连接: 可能旧实例正在退出, 再等一次 */
#ifdef _WIN32
        Sleep(500);
#else
        usleep(500000);
#endif
        if (!try_acquire_lock()) {
            fprintf(stderr, "检测到另一实例正在运行, 请勿重复打开\n");
            return 1;
        }
    }

#ifdef __linux__
    /* Linux GUI 模式: fork 后台运行, 避免终端窗口 (支持双击打开) */
    /* fork 后子进程继承 g_lock_fd, 锁保持到子进程退出 */
    if (!headless && !g_once) {
        pid_t pid = fork();
        if (pid > 0) return 0;  /* 父进程退出, 终端关闭 */
        if (pid < 0) return 1;
        setsid();
        freopen("/dev/null", "r", stdin);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
    }
#endif

    /* 启动 HTTP 服务器 */
    if (start_server() != 0) {
        return 1;
    }

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", g_port);
    write_lock_file(g_port);

    /* 信号处理 */
#ifndef _WIN32
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#else
    /* Windows: 使用 Ctrl+C 处理 (简化) */
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)signal_handler, TRUE);
#endif

    if (headless) {
        /* 无桌面 API 服务模式 */
        DeviceInfo info = collect_all();
        print_formatted(&info);
        if (g_json_output) {
            save_json(&info, g_json_path);
        }
        free_info(&info);
        printf("\n========================================\n");
        printf("  DeviceMate API 服务已启动 (--port 模式)\n");
        printf("  HTTP API: %sapi/collect\n", url);
        printf("  健康检查: %sapi/health\n", url);
        printf("  关闭服务: curl %sapi/shutdown\n", url);
        printf("  按 Ctrl+C 退出程序\n");
        printf("========================================\n");
    } else {
        /* GUI 模式: 打开浏览器 */
        printf("========================================\n");
        printf("  DeviceMate 设备信息采集工具已启动\n");
        printf("========================================\n");
        printf("  浏览器访问: %s\n", url);
        printf("  按 Ctrl+C 退出程序\n");
        printf("========================================\n");
#ifdef _WIN32
        Sleep(300);
#else
        usleep(300000);
#endif
        open_browser(url);
    }

    server_loop();
    /* 单例锁在进程退出时由 OS 自动释放, 无需手动清理 */
    return 0;
}
