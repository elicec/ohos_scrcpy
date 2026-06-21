/*
 * 适用于 OpenHarmony (aarch64) 的简化版 getevent 工具
 * 用法:
 *   getevent                列出所有 /dev/input/event* 设备
 *   getevent -l             列出设备并打印基本信息
 *   getevent /dev/input/eventN  读取并打印该设备的输入事件
 *
 * 交叉编译:
 *   ./build_arm64.sh
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <stdint.h>
#include <poll.h>

#include <linux/input.h>

/* 老版本内核头文件可能没有 EV_VERSION，补一个默认值 */
#ifndef EV_VERSION
#define EV_VERSION 0x010001
#endif

/* 部分老版本头文件没有 test_bit / BITS_PER_LONG，提供兜底实现 */
#ifndef BITS_PER_LONG
#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#endif
#ifndef test_bit
#define test_bit(nr, array) \
    (((array)[(nr) / BITS_PER_LONG] >> ((nr) % BITS_PER_LONG)) & 1)
#endif

static int is_event_device(const struct dirent *entry)
{
    return strncmp(entry->d_name, "event", 5) == 0;
}

static int read_device_name(const char *path, char *name, size_t len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int ret = ioctl(fd, EVIOCGNAME(len), name);
    close(fd);
    return ret;
}

static void list_devices(int verbose)
{
    struct dirent **entries = NULL;
    int n = scandir("/dev/input", &entries, is_event_device, alphasort);
    if (n < 0) {
        perror("scandir /dev/input");
        return;
    }

    for (int i = 0; i < n; i++) {
        char path[128];
        char name[256] = {0};
        snprintf(path, sizeof(path), "/dev/input/%s", entries[i]->d_name);
        read_device_name(path, name, sizeof(name));
        printf("add device %d: %s\n", i + 1, path);
        printf("  name: \"%s\"\n", name[0] ? name : "<unknown>");

        if (verbose) {
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                struct input_id id;
                if (ioctl(fd, EVIOCGID, &id) == 0) {
                    printf("  bus: %04x, vendor %04x, product %04x, version %04x\n",
                           id.bustype, id.vendor, id.product, id.version);
                }
                unsigned long ev_bits[EV_MAX / BITS_PER_LONG + 1] = {0};
                if (ioctl(fd, EVIOCGBIT(0, EV_MAX), ev_bits) >= 0) {
                    printf("  events:");
                    for (int ev = 0; ev < EV_MAX; ev++) {
                        if (test_bit(ev, ev_bits)) {
                            printf(" %04x", ev);
                        }
                    }
                    printf("\n");
                }
                close(fd);
            }
        }
        free(entries[i]);
    }
    free(entries);
}

static const char *get_event_type_name(unsigned int type)
{
    switch (type) {
        case EV_SYN: return "SYN";
        case EV_KEY: return "KEY";
        case EV_REL: return "REL";
        case EV_ABS: return "ABS";
        case EV_MSC: return "MSC";
        case EV_SW:  return "SW";
        case EV_LED: return "LED";
        case EV_SND: return "SND";
        case EV_REP: return "REP";
        case EV_FF:  return "FF";
        case EV_PWR: return "PWR";
        case EV_FF_STATUS: return "FF_STATUS";
        default: return "???";
    }
}

static void print_event(const struct input_event *ev)
{
    /* 与 Android getevent -l 类似: 类型 编码 数值 */
    printf("[%ld.%06ld] %-6s %04x %08d\n",
           (long)ev->time.tv_sec,
           (long)ev->time.tv_usec,
           get_event_type_name(ev->type),
           ev->code,
           ev->value);
}

static int read_events(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }

    /* 不使用 EVIOCGRAB 抓取设备，避免阻止输入服务读取事件，
     * 允许同时监听事件流和系统正常处理触摸 */
    int grab = 0;
    ioctl(fd, EVIOCGRAB, &grab);

    printf("Reading from %s...\n", path);

    struct input_event ev;
    while (1) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "read error: %s\n", strerror(errno));
            break;
        }
        if (n == 0) {
            fprintf(stderr, "EOF\n");
            break;
        }
        if (n != sizeof(ev)) {
            fprintf(stderr, "short read: %zd\n", n);
            continue;
        }
        print_event(&ev);
        fflush(stdout);
    }

    close(fd);
    return 0;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s              List input devices\n"
            "  %s -l           List input devices with details\n"
            "  %s /dev/input/eventN  Read events from device\n",
            prog, prog, prog);
}

int main(int argc, char *argv[])
{
    if (argc == 1) {
        list_devices(0);
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "-l") == 0) {
        list_devices(1);
        return 0;
    }

    if (argc == 2) {
        return read_events(argv[1]);
    }

    print_usage(argv[0]);
    return 1;
}
