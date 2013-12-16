#define DEFAULT_PROFILE_DIR "/sdcard/battery-profiles"
#define CAPACITY_FILENAME "/sys/class/power_supply/battery/capacity"
#define AC_FILENAME "/sys/class/power_supply/usb/online"
#define AC2_FILENAME "/sys/class/power_supply/ac/online"
#define SLEEP_INTERVAL_SEC 1
#define ONDEMAND_NODE "/sys/devices/system/cpu/cpufreq/ondemand"
#define MAX_PROFILES 64
#define CPU_NODE_FMT "/sys/devices/system/cpu/cpu%d/online"
#define SCREEN_OFF_NODE "/sys/power/wait_for_fb_sleep"
#define SCREEN_ON_NODE "/sys/power/wait_for_fb_wake"
#define CPUS_MAX 8

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>

static volatile int reloadp = 0;
static volatile int screen_on = 0;

typedef struct ondemand_profile {
    char name[256 + 1];
    int down_differential;
    int ignore_nice_load;
    int powersave_bias;
    int up_threshold;
    int sampling_down_factor;
    int sampling_rate;
    int battery;
    int ac;
    int cpu_mask;
    int screen;
} profile_t;

struct meta;
typedef struct meta meta_t;
typedef void (*handler_t)(const profile_t* profile, const meta_t* meta);

struct meta {
    const char* name;
    off_t offset;
    handler_t handler;
};

static void apply_ondemand(const profile_t* profile, const meta_t* meta)
{
    char path[PATH_MAX+1] = {0}, buf[256];
    char const* ptr = (void*) profile;
    int fd, value = *(int*)(ptr + meta->offset);
    if (value == -1)
        return;
    snprintf(path, PATH_MAX, "%s/%s", ONDEMAND_NODE, meta->name);
    fd = open(path, O_WRONLY);
    if (fd == -1)
    {
        fprintf(stderr, "open '%s': %s\n", path, strerror(errno));
        return;
    }
    sprintf(buf, "%d", value);
    /* fprintf(stderr, "apply %s: %d\n", path, value); */
    write(fd, buf, strlen(buf));
    close(fd);
}

static void apply_cpu_mask(const profile_t* profile, const meta_t* meta __unused)
{
    int i, fd;
    char path[PATH_MAX+1] = {0};
    for (i = 0; i < CPUS_MAX; i++)
    {
        snprintf(path, PATH_MAX, CPU_NODE_FMT, i);
        fd = open(path, O_WRONLY);
        if (fd == -1)
            break;
        write(fd, (profile->cpu_mask & (1 << (i+1))) ? "1" : "0", 1);
        close(fd);
    }
}

static void apply_noop(const profile_t* profile __unused, const meta_t* meta __unused)
{
}

#define FIELD(dtype, member, handler) \
    { #member, offsetof(dtype, member), handler }
#define FIELD_END { NULL, -1, NULL }
#define ONDEMAND_FIELD(member) \
    FIELD(profile_t, member, apply_ondemand)
#define PROFILE_FIELD(member, handler) \
    FIELD(profile_t, member, handler)

static const meta_t meta_profile[] = {
    ONDEMAND_FIELD(down_differential),
    ONDEMAND_FIELD(ignore_nice_load),
    ONDEMAND_FIELD(powersave_bias),
    ONDEMAND_FIELD(up_threshold),
    ONDEMAND_FIELD(sampling_down_factor),
    ONDEMAND_FIELD(sampling_rate),
    PROFILE_FIELD(battery, apply_noop),
    PROFILE_FIELD(ac, apply_noop),
    PROFILE_FIELD(cpu_mask, apply_cpu_mask),
    PROFILE_FIELD(screen, apply_noop),
    FIELD_END
};

static void load_setting(void* data,
                         const meta_t* meta,
                         const char* string)
{
    unsigned char* ptr = data;
    ptr += meta->offset;
    *(int*) ptr = atoi(string);
}

static const meta_t* get_meta(const meta_t* meta, const char* string)
{
    int i;
    for (i = 0; meta[i].name; i++)
    {
        if (!strcmp(string, meta[i].name))
            return &meta[i];
    }

    return NULL;
}

static profile_t load_settings(const meta_t* meta,
                               const char* filename,
                               int* ok)
{
    profile_t ret;
    char buf[BUFSIZ];
    FILE* stream = fopen(filename, "r");
    int i;
    int lineno = 0;

    memset(&ret, 0, sizeof(ret));

    fprintf(stderr, "[%s]\n", filename);

    *ok = 0;

    if (!stream)
    {
        int error = errno;
        fprintf(stderr, "can't open '%s' for reading: %s\n", filename, strerror(error));
        return ret;
    }

    for (i = 0; meta[i].name; i++)
        load_setting(&ret, &meta[i], "-1");

    buf[BUFSIZ-1] = '\0';

    while (lineno++, fgets(buf, BUFSIZ-1, stream))
    {
        if (*buf)
        {
            char* lf = &buf[strlen(buf)-1];
            if (*lf == '\n')
                *lf = '\0';
        }
        {
            char* ptr = strchr(buf, '#');
            if (ptr)
                *ptr = '\0';
        }
        {
            const meta_t* meta;
            char* spc = strchr(buf, ' ');
            if (spc)
            {
                *spc = '\0';
                spc++;
                while (*spc == ' ')
                    spc++;
                meta = get_meta(meta_profile, buf);
                if (!meta)
                {
                    fprintf(stderr, "can't find setting '%s' in '%s' lineno %d\n", buf, filename, lineno);
                    continue;
                }
                load_setting(&ret, meta, spc);
                fprintf(stderr, "%s %d\n", buf, atoi(spc));
            }
            else if (*buf)
            {
                fprintf(stderr, "cannot find value '%s' in '%s' line %d\n", buf, filename, lineno);
            }
        }
    }

    *ok = 1;

    fclose(stream);

    return ret;
}

static int load_profiles(const char* profile_dir, profile_t* profiles)
{
    struct stat sb;
    int i, ok, error;
    struct dirent* entry;
    DIR* dir = opendir(profile_dir);
    char name[PATH_MAX+1] = {0};
    if (!dir)
    {
        fprintf(stderr, "can't open profile dir '%s': %s\n", profile_dir, strerror(errno));
        return 0;
    }
    for (i = 0; i < MAX_PROFILES; i++)
    {
        entry = readdir(dir);
        if (!entry)
            break;
        snprintf(name, PATH_MAX, "%s/%s", DEFAULT_PROFILE_DIR, entry->d_name);
        error = stat(name, &sb);
        if (error != 0 || !S_ISREG(sb.st_mode))
        {
            i--;
            continue;
        }
        profiles[i] = load_settings(meta_profile, name, &ok);
        strncpy(profiles[i].name, entry->d_name, 256);
        if (!ok)
            i--;
    }

    closedir(dir);

    return i;
}

static void apply_profile(const profile_t* profile, const meta_t* meta)
{
    int i;
    for (i = 0; meta[i].name; i++)
        meta[i].handler(profile, &meta[i]);
}

static int profilecmp(const void* p1, const void* p2)
{
    const profile_t* one = p1;
    const profile_t* two = p2;

    return strcmp(one->name, two->name);
}

static int read_int_from_sysfs_node(const char* filename)
{
    int fd;
    int ret = -1;
    char buf[64] = {0};

    fd = open(filename, O_RDONLY);
    if (fd == -1)
    {
        fprintf(stderr, "can't read from file '%s': %s\n", filename, strerror(errno));
        return ret;
    }

    (void) read(fd, buf, 63);

    ret = atoi(buf);

    close(fd);

    return ret;
}

static int profile_matchp(profile_t* p)
{
    int ac_p, bat;

    if (p->ac == -1 && p->battery == -1)
        return 0;

    ac_p = read_int_from_sysfs_node(AC_FILENAME) ||
           read_int_from_sysfs_node(AC2_FILENAME);

    bat = read_int_from_sysfs_node(CAPACITY_FILENAME);

    return ((p->ac == -1 || !!p->ac == !!ac_p) &&
            (p->battery == -1 || bat < p->battery) &&
            (p->screen == -1 || !!p->screen == screen_on));
}

static void sighup_handler(int signo __unused)
{
    reloadp = 1;
}

static void* screen_worker(void* param __unused)
{
    while (1)
    {
        (void) read_int_from_sysfs_node(SCREEN_ON_NODE);
        screen_on = 1;
        (void) read_int_from_sysfs_node(SCREEN_OFF_NODE);
        screen_on = 0;
    }
    return NULL;
}

int main(int argc, char** argv)
{
    pthread_t thr;
    const char* profile_dir;
    int cnt, i, last_profile = -1;
    static profile_t profiles[MAX_PROFILES]; /* better BSS than stackalloc */

    memset(profiles, 0, sizeof(profile_t[MAX_PROFILES]));

    (void) signal(SIGHUP, sighup_handler);

    switch (argc)
    {
    case 0:
    case 1:
        profile_dir = DEFAULT_PROFILE_DIR;
        break;
    case 2:
        profile_dir = argv[1];
        break;
    default:
        return 1;
    }

    if (pthread_create(&thr, NULL, screen_worker, NULL))
    {
        return 1;
    }

start:
    last_profile = -1;
    cnt = load_profiles(profile_dir, profiles);

    fprintf(stderr, "loaded %d profiles\n", cnt);

    if (cnt == 0)
        return 1;

    qsort(profiles, cnt, sizeof(*profiles), profilecmp);

    for(;;)
    {
        for (i = 0; i < cnt; i++)
        {
            int matchp = profile_matchp(&profiles[i]);
            if (matchp)
            {
                if (last_profile != i)
                    fprintf(stderr, "%ld -- applying profile %s\n", time(NULL), profiles[i].name);
                last_profile = i;
                apply_profile(&profiles[i], meta_profile);
                break;
            }
        }

        if (i == cnt)
        {
            last_profile = -1;
            fprintf(stderr, "applied no profile!\n");
        }

        sleep(SLEEP_INTERVAL_SEC);
        if (reloadp)
        {
            reloadp = 0;
            goto start;
        }
    }

    return 0;
}
