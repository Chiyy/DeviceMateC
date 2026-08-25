# DeviceMate C 版 - 跨平台构建
# 用法:
#   make            # 构建当前平台
#   make windows    # 交叉编译 Windows (需 mingw)
#   make linux      # 交叉编译 Linux
#   make macos      # 交叉编译 macOS
#   make clean      # 清理

CFLAGS  = -std=gnu11 -Os -Wall -Wextra -ffunction-sections -fdata-sections

# 检测操作系统 (归一化 MINGW/MSYS/CYGWIN 为 Windows)
UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)
ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)))
    UNAME_S := Windows
endif

# HTML/logo 嵌入: 优先 xxd, 回退 python3/python, 都不可用则运行时读取
xxd := $(shell command -v xxd 2>/dev/null)
python := $(shell (command -v python3 2>/dev/null || command -v python 2>/dev/null))
ifneq (,$(or $(xxd),$(python)))
    EMBED_FLAG = -DUSE_EMBEDDED_HTML
    EMBED_DEP = web_ui.h logo_png.h
endif

# windres 工具 (用于嵌入图标和版本信息, 可通过命令行覆盖)
WINDRES ?= windres

# 平台选择 (含默认编译器和链接参数)
ifeq ($(UNAME_S),Windows)
    PLATFORM_SRC     = platform_windows.c
    PLATFORM_LIBS    = -lws2_32 -liphlpapi -lshell32
    PLATFORM_LDFLAGS = -Wl,--gc-sections -Wl,-s -static -mwindows
    EXE              = DeviceMate.exe
    DEFAULT_CC       = gcc
    # 检查 windres 是否可用 (用于嵌入图标和版本信息)
    windres_path := $(shell command -v $(WINDRES) 2>/dev/null)
    ifneq (,$(windres_path))
        PLATFORM_RES = app_resource.o
    else
        PLATFORM_RES =
    endif
else ifeq ($(UNAME_S),Linux)
    PLATFORM_SRC     = platform_linux.c
    PLATFORM_LIBS    =
    PLATFORM_LDFLAGS = -Wl,--gc-sections -Wl,-s -static
    PLATFORM_RES     =
    EXE              = DeviceMate
    DEFAULT_CC       = cc
else ifeq ($(UNAME_S),Darwin)
    PLATFORM_SRC     = platform_darwin.c
    PLATFORM_LIBS    =
    PLATFORM_LDFLAGS = -Wl,-dead_strip -Wl,-s
    PLATFORM_RES     =
    EXE              = DeviceMate
    DEFAULT_CC       = clang
endif

# CC: 命令行可覆盖, 否则用平台默认 (覆盖 make 内置 cc)
CC = $(DEFAULT_CC)

SRCS = main.c collector.c $(PLATFORM_SRC)
OBJS = $(SRCS:.c=.o)
HEADERS = dm.h $(EMBED_DEP)

.PHONY: all clean windows linux macos macos-debug debug

all: $(EXE)

$(EXE): $(OBJS) $(PLATFORM_RES)
	$(CC) $(CFLAGS) $(EMBED_FLAG) -o $@ $(OBJS) $(PLATFORM_RES) $(PLATFORM_LIBS) $(PLATFORM_LDFLAGS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) $(EMBED_FLAG) -c $< -o $@

# 生成嵌入 HTML 的头文件 (xxd 优先, 回退 Python, 兼容 Windows runner)
web_ui.h: index.html
	@if [ -n "$(xxd)" ]; then \
		echo "  GEN  web_ui.h (xxd)"; \
		xxd -i index.html > web_ui.h; \
	else \
		echo "  GEN  web_ui.h (python)"; \
		python3 -c "import sys;d=open('index.html','rb').read();sys.stdout.write('unsigned char index_html[] = {%s};\nunsigned int index_html_len = %d;\n'%(','.join(map(str,d)),len(d)))" > web_ui.h 2>/dev/null \
		|| python -c "import sys;d=open('index.html','rb').read();sys.stdout.write('unsigned char index_html[] = {%s};\nunsigned int index_html_len = %d;\n'%(','.join(map(str,d)),len(d)))" > web_ui.h; \
	fi

# 生成嵌入 logo 的头文件 (xxd -i logo.png 生成的变量名为 logo_png)
logo_png.h: logo.png
	@if [ -n "$(xxd)" ]; then \
		echo "  GEN  logo_png.h (xxd)"; \
		xxd -i logo.png > logo_png.h; \
	else \
		echo "  GEN  logo_png.h (python)"; \
		python3 -c "import sys;d=open('logo.png','rb').read();sys.stdout.write('unsigned char logo_png[] = {%s};\nunsigned int logo_png_len = %d;\n'%(','.join(map(str,d)),len(d)))" > logo_png.h 2>/dev/null \
		|| python -c "import sys;d=open('logo.png','rb').read();sys.stdout.write('unsigned char logo_png[] = {%s};\nunsigned int logo_png_len = %d;\n'%(','.join(map(str,d)),len(d)))" > logo_png.h; \
	fi

# Windows 资源 (嵌入图标和版本信息到 exe)
app_resource.o: app.rc icon.ico
	$(WINDRES) app.rc -o app_resource.o

# 交叉编译目标
windows:
	$(MAKE) CC=x86_64-w64-mingw32-gcc \
		WINDRES=x86_64-w64-mingw32-windres \
		UNAME_S=Windows \
		CFLAGS="-std=gnu11 -Os -Wall -Wextra -ffunction-sections -fdata-sections -I/usr/x86_64-w64-mingw32/include" \
		PLATFORM_LDFLAGS="-Wl,--gc-sections -Wl,-s -static -mwindows"

linux:
	$(MAKE) CC=x86_64-linux-gnu-gcc \
		UNAME_S=Linux \
		PLATFORM_LDFLAGS="-Wl,--gc-sections -Wl,-s -static"

macos:
	$(MAKE) CC=o64-clang \
		UNAME_S=Darwin

# macOS 调试版本: 不优化, 含调试符号, USB 检测日志默认开启
macos-debug:
	$(MAKE) CC=o64-clang \
		UNAME_S=Darwin \
		CFLAGS="-std=gnu11 -g -O0 -Wall -Wextra -ffunction-sections -fdata-sections -DUSB_DEBUG_ALWAYS_ON"

# 当前平台调试版本
debug:
	$(MAKE) CFLAGS="-std=gnu11 -g -O0 -Wall -Wextra -ffunction-sections -fdata-sections"

clean:
	rm -f $(OBJS) $(EXE) DeviceMate DeviceMate.exe web_ui.h logo_png.h app_resource.o
