/*
 * evdev.c - Linux/FreeBSD evdev input device ingestion for Y11.
 *
 * Scans /dev/input/event* devices and polls them in the main server loop.
 * Translates evdev key codes, buttons, relative pointer motions and
 * absolute tablet/mouse events into the y11_input_* subsystem.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <time.h>
#if defined(__linux__)
#include <linux/input.h>
#elif defined(__FreeBSD__)
#include <dev/evdev/input.h>
#endif

#include "y11.h"
#include "y11_drm.h"

#ifndef INPUT_PROP_DIRECT
#define INPUT_PROP_DIRECT 0x01
#endif
#ifndef INPUT_PROP_MAX
#define INPUT_PROP_MAX 0x1f
#endif
#ifndef EVIOCGPROP
#define EVIOCGPROP(len) _IOC(_IOC_READ, 'E', 0x09, len)
#endif
#ifndef ABS_MT_POSITION_X
#define ABS_MT_POSITION_X 0x35
#endif
#ifndef ABS_MT_POSITION_Y
#define ABS_MT_POSITION_Y 0x36
#endif
#ifndef BTN_TOOL_FINGER
#define BTN_TOOL_FINGER 0x145
#endif
#ifndef ABS_MT_TRACKING_ID
#define ABS_MT_TRACKING_ID 0x39
#endif

#define Y11_BITS_PER_LONG (sizeof(unsigned long) * 8u)
#define Y11_TEST_BIT(b, a) \
    (((a)[(size_t)(b) / Y11_BITS_PER_LONG] & \
      (1ul << ((size_t)(b) % Y11_BITS_PER_LONG))) != 0)

struct y11_evdev_dev {
    int     fd;
    int     dev_id;     /* libseat device ID, or -1 if opened directly */
    dev_t   rdev;       /* device node ID to prevent duplicate opens */
    int     has_keys;
    int     has_rel;
    int     has_abs;
    int     is_direct;
    int     touch_down;
    int     touch_first;
    int     touch_moved;
    int32_t touch_prev_x;
    int32_t touch_prev_y;
    struct timespec touch_down_time;
    int32_t abs_min_x;
    int32_t abs_max_x;
    int32_t abs_min_y;
    int32_t abs_max_y;
    int32_t abs_x;
    int32_t abs_y;
    int     has_abs_x;
    int     has_abs_y;
    int16_t rel_dx;
    int16_t rel_dy;
};

static struct y11_evdev_dev y11_evdev_devs[Y11_MAX_EVDEV_DEVICES];
static size_t y11_evdev_count;

static void y11_evdev_add_device(struct y11_session *s, const char *path)
{
    struct stat st;
    int fd = -1;
    int dev_id = -1;
    unsigned long evbits[(EV_MAX + sizeof(unsigned long) * 8 - 1) /
                         (sizeof(unsigned long) * 8)];
    int has_keys, has_rel, has_abs;
    struct y11_evdev_dev *dev;
    size_t i;

    if (y11_evdev_count >= Y11_MAX_EVDEV_DEVICES)
        return;

    if (s != NULL && s->seat != NULL) {
        int id = libseat_open_device(s->seat, path, &fd);
        if (id >= 0 && fd >= 0)
            dev_id = id;
    }

    if (fd < 0) {
        fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    }
    if (fd < 0)
        return;

    if (fstat(fd, &st) != 0) {
        if (dev_id >= 0 && s != NULL && s->seat != NULL)
            (void)libseat_close_device(s->seat, dev_id);
        close(fd);
        return;
    }

    /* Avoid duplicates */
    for (i = 0; i < y11_evdev_count; i++) {
        if (y11_evdev_devs[i].rdev == st.st_rdev) {
            if (dev_id >= 0 && s != NULL && s->seat != NULL)
                (void)libseat_close_device(s->seat, dev_id);
            close(fd);
            return;
        }
    }

    memset(evbits, 0, sizeof(evbits));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) {
        if (dev_id >= 0 && s != NULL && s->seat != NULL)
            (void)libseat_close_device(s->seat, dev_id);
        close(fd);
        return;
    }

    has_keys = Y11_TEST_BIT(EV_KEY, evbits);
    has_rel  = Y11_TEST_BIT(EV_REL, evbits);
    has_abs  = Y11_TEST_BIT(EV_ABS, evbits);

    if (!has_keys && !has_rel && !has_abs) {
        if (dev_id >= 0 && s != NULL && s->seat != NULL)
            (void)libseat_close_device(s->seat, dev_id);
        close(fd);
        return;
    }

    /* Ensure nonblocking and close-on-exec */
    {
        int flags = fcntl(fd, F_GETFL);
        if (flags >= 0)
            (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    }

    dev = &y11_evdev_devs[y11_evdev_count];
    memset(dev, 0, sizeof(*dev));
    dev->fd = fd;
    dev->dev_id = dev_id;
    dev->rdev = st.st_rdev;
    dev->has_keys = has_keys;
    dev->has_rel = has_rel;
    dev->has_abs = has_abs;

    if (has_abs) {
        struct input_absinfo abs;
        int ok_x = 0, ok_y = 0;
        unsigned long props[(INPUT_PROP_MAX + sizeof(unsigned long) * 8 - 1) /
                            (sizeof(unsigned long) * 8)];

        memset(props, 0, sizeof(props));
        (void)ioctl(fd, EVIOCGPROP(sizeof(props)), props);

        if (ioctl(fd, EVIOCGABS(ABS_X), &abs) == 0 &&
            abs.maximum > abs.minimum) {
            dev->abs_min_x = abs.minimum;
            dev->abs_max_x = abs.maximum;
            dev->abs_x = abs.value;
            ok_x = 1;
        } else if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs) == 0 &&
                   abs.maximum > abs.minimum) {
            dev->abs_min_x = abs.minimum;
            dev->abs_max_x = abs.maximum;
            dev->abs_x = abs.value;
            ok_x = 1;
        }

        if (ioctl(fd, EVIOCGABS(ABS_Y), &abs) == 0 &&
            abs.maximum > abs.minimum) {
            dev->abs_min_y = abs.minimum;
            dev->abs_max_y = abs.maximum;
            dev->abs_y = abs.value;
            ok_y = 1;
        } else if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs) == 0 &&
                   abs.maximum > abs.minimum) {
            dev->abs_min_y = abs.minimum;
            dev->abs_max_y = abs.maximum;
            dev->abs_y = abs.value;
            ok_y = 1;
        }

        if (!ok_x || !ok_y) {
            dev->has_abs = 0;
        } else {
            dev->is_direct = Y11_TEST_BIT(INPUT_PROP_DIRECT, props);
        }
    }

    if (y11_debug) {
        char name[128];
        memset(name, 0, sizeof(name));
        (void)ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
        fprintf(stderr,
                "y11: input device %s (\"%s\"): keys=%d rel=%d abs=%d\n",
                path, name[0] ? name : "unknown",
                dev->has_keys, dev->has_rel, dev->has_abs);
    }

    y11_evdev_count++;
}

void y11_evdev_rescan(struct y11_session *s)
{
    DIR *dir = opendir("/dev/input");
    struct dirent *de;

    if (dir == NULL)
        return;

    while ((de = readdir(dir)) != NULL) {
        char path[512];
        const char *p;

        if (strncmp(de->d_name, "event", 5) != 0)
            continue;
        p = de->d_name + 5;
        if (*p < '0' || *p > '9')
            continue;

        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        y11_evdev_add_device(s, path);
    }
    closedir(dir);
}

void y11_evdev_init(struct y11_session *s)
{
    y11_evdev_count = 0;
    y11_evdev_rescan(s);
}

void y11_evdev_shutdown(struct y11_session *s)
{
    size_t i;

    for (i = 0; i < y11_evdev_count; i++) {
        struct y11_evdev_dev *dev = &y11_evdev_devs[i];
        if (dev->fd >= 0) {
            if (dev->dev_id >= 0 && s != NULL && s->seat != NULL)
                (void)libseat_close_device(s->seat, dev->dev_id);
            close(dev->fd);
            dev->fd = -1;
        }
    }
    y11_evdev_count = 0;
}

size_t y11_evdev_get_count(void)
{
    return y11_evdev_count;
}

int y11_evdev_get_fd(size_t index)
{
    if (index < y11_evdev_count)
        return y11_evdev_devs[index].fd;
    return -1;
}

void y11_evdev_handle(struct y11_session *s, int fd)
{
    struct y11_evdev_dev *dev = NULL;
    size_t dev_idx;
    struct input_event evs[64];
    ssize_t n;

    for (dev_idx = 0; dev_idx < y11_evdev_count; dev_idx++) {
        if (y11_evdev_devs[dev_idx].fd == fd) {
            dev = &y11_evdev_devs[dev_idx];
            break;
        }
    }
    if (dev == NULL)
        return;

    while ((n = read(dev->fd, evs, sizeof(evs))) > 0) {
        size_t count = (size_t)n / sizeof(struct input_event);
        size_t i;

        for (i = 0; i < count; i++) {
            struct input_event *ev = &evs[i];

            if (s != NULL && !s->active)
                continue;

            if (ev->type == EV_REL) {
                if (ev->code == REL_X) {
                    dev->rel_dx += (int16_t)ev->value;
                } else if (ev->code == REL_Y) {
                    dev->rel_dy += (int16_t)ev->value;
                } else if (ev->code == REL_WHEEL) {
                    uint8_t btn = ev->value > 0 ? 4 : 5;
                    y11_input_button(1, btn);
                    y11_input_button(0, btn);
                } else if (ev->code == REL_HWHEEL) {
                    uint8_t btn = ev->value > 0 ? 7 : 6;
                    y11_input_button(1, btn);
                    y11_input_button(0, btn);
                }
            } else if (ev->type == EV_ABS) {
                if (ev->code == ABS_X || ev->code == ABS_MT_POSITION_X) {
                    dev->abs_x = ev->value;
                    dev->has_abs_x = 1;
                } else if (ev->code == ABS_Y || ev->code == ABS_MT_POSITION_Y) {
                    dev->abs_y = ev->value;
                    dev->has_abs_y = 1;
                } else if (ev->code == ABS_MT_TRACKING_ID) {
                    if (ev->value >= 0) {
                        dev->touch_down = 1;
                        dev->touch_first = 1;
                        dev->touch_moved = 0;
                        dev->touch_prev_x = dev->abs_x;
                        dev->touch_prev_y = dev->abs_y;
                        clock_gettime(CLOCK_MONOTONIC, &dev->touch_down_time);
                    } else {
                        if (dev->touch_down && !dev->touch_moved && !dev->is_direct) {
                            struct timespec now;
                            clock_gettime(CLOCK_MONOTONIC, &now);
                            long ms = (now.tv_sec - dev->touch_down_time.tv_sec) * 1000 +
                                      (now.tv_nsec - dev->touch_down_time.tv_nsec) / 1000000;
                            if (ms < 300) {
                                y11_input_button(1, 1);
                                y11_input_button(0, 1);
                            }
                        }
                        dev->touch_down = 0;
                        dev->touch_first = 0;
                        dev->touch_moved = 0;
                    }
                }
            } else if (ev->type == EV_KEY) {
                if (ev->code == BTN_TOUCH) {
                    if (ev->value != 0) {
                        dev->touch_down = 1;
                        dev->touch_first = 1;
                        dev->touch_moved = 0;
                        dev->touch_prev_x = dev->abs_x;
                        dev->touch_prev_y = dev->abs_y;
                        clock_gettime(CLOCK_MONOTONIC, &dev->touch_down_time);
                    } else {
                        if (dev->touch_down && !dev->touch_moved && !dev->is_direct) {
                            struct timespec now;
                            clock_gettime(CLOCK_MONOTONIC, &now);
                            long ms = (now.tv_sec - dev->touch_down_time.tv_sec) * 1000 +
                                      (now.tv_nsec - dev->touch_down_time.tv_nsec) / 1000000;
                            if (ms < 300) {
                                y11_input_button(1, 1);
                                y11_input_button(0, 1);
                            }
                        }
                        dev->touch_down = 0;
                        dev->touch_first = 0;
                        dev->touch_moved = 0;
                    }
                    if (dev->is_direct)
                        y11_input_button(ev->value != 0, 1);
                } else if (ev->code == BTN_TOOL_FINGER && !dev->touch_down) {
                    if (ev->value != 0) {
                        dev->touch_down = 1;
                        dev->touch_first = 1;
                        dev->touch_moved = 0;
                        dev->touch_prev_x = dev->abs_x;
                        dev->touch_prev_y = dev->abs_y;
                        clock_gettime(CLOCK_MONOTONIC, &dev->touch_down_time);
                    }
                } else if (ev->code >= BTN_MISC) {
                    uint8_t btn = 0;
                    if (ev->code == BTN_LEFT)
                        btn = 1;
                    else if (ev->code == BTN_MIDDLE)
                        btn = 2;
                    else if (ev->code == BTN_RIGHT)
                        btn = 3;
                    else if (ev->code == BTN_SIDE || ev->code == BTN_BACK)
                        btn = 8;
                    else if (ev->code == BTN_EXTRA ||
                             ev->code == BTN_FORWARD)
                        btn = 9;
                    if (btn != 0)
                        y11_input_button(ev->value != 0, btn);
                } else {
                    if (ev->code + 8u <= 255u) {
                        uint8_t keycode = (uint8_t)(ev->code + 8u);
                        if (ev->value == 0)
                            y11_input_key(0, keycode);
                        else if (ev->value == 1 || ev->value == 2)
                            y11_input_key(1, keycode);
                    }
                }
            } else if (ev->type == EV_SYN) {
                if (ev->code == SYN_REPORT) {
                    if (dev->rel_dx != 0 || dev->rel_dy != 0) {
                        y11_input_motion(dev->rel_dx, dev->rel_dy);
                        dev->rel_dx = 0;
                        dev->rel_dy = 0;
                    }
                    if (dev->has_abs_x || dev->has_abs_y) {
                        if (dev->is_direct) {
                            int32_t val_x = dev->abs_x;
                            int32_t val_y = dev->abs_y;
                            int32_t sx = 0, sy = 0;

                            if (val_x < dev->abs_min_x)
                                val_x = dev->abs_min_x;
                            if (val_x > dev->abs_max_x)
                                val_x = dev->abs_max_x;
                            if (val_y < dev->abs_min_y)
                                val_y = dev->abs_min_y;
                            if (val_y > dev->abs_max_y)
                                val_y = dev->abs_max_y;

                            if (dev->abs_max_x > dev->abs_min_x)
                                sx = (int32_t)((int64_t)(val_x - dev->abs_min_x) *
                                               (y11_screen_width - 1) /
                                               (dev->abs_max_x - dev->abs_min_x));
                            if (dev->abs_max_y > dev->abs_min_y)
                                sy = (int32_t)((int64_t)(val_y - dev->abs_min_y) *
                                               (y11_screen_height - 1) /
                                               (dev->abs_max_y - dev->abs_min_y));

                            y11_input_motion_abs((int16_t)sx, (int16_t)sy);
                        } else {
                            if (dev->touch_down) {
                                if (dev->touch_first) {
                                    dev->touch_prev_x = dev->abs_x;
                                    dev->touch_prev_y = dev->abs_y;
                                    dev->touch_first = 0;
                                } else {
                                    int32_t dx = dev->abs_x - dev->touch_prev_x;
                                    int32_t dy = dev->abs_y - dev->touch_prev_y;

                                    dev->touch_prev_x = dev->abs_x;
                                    dev->touch_prev_y = dev->abs_y;

                                    /* Scale down high-resolution trackpads (e.g. MacBook T2 ~95 units/mm) */
                                    if (dev->abs_max_x - dev->abs_min_x > 4000) {
                                        dx = dx / 8;
                                        dy = dy / 8;
                                    }

                                    if (dx > 1 || dx < -1 || dy > 1 || dy < -1)
                                        dev->touch_moved = 1;

                                    if (dx > 250)
                                        dx = 250;
                                    else if (dx < -250)
                                        dx = -250;
                                    if (dy > 250)
                                        dy = 250;
                                    else if (dy < -250)
                                        dy = -250;
                                    if (dx != 0 || dy != 0)
                                        y11_input_motion((int16_t)dx, (int16_t)dy);
                                }
                            }
                        }
                        dev->has_abs_x = 0;
                        dev->has_abs_y = 0;
                    }
                }
            }
        }
    }

    if (n < 0 && (errno == ENODEV || errno == EACCES || errno == EIO)) {
        /* Device disconnected or revoked */
        if (dev->dev_id >= 0 && s != NULL && s->seat != NULL)
            (void)libseat_close_device(s->seat, dev->dev_id);
        close(dev->fd);
        for (; dev_idx + 1 < y11_evdev_count; dev_idx++)
            y11_evdev_devs[dev_idx] = y11_evdev_devs[dev_idx + 1];
        y11_evdev_count--;
    }
}
