#define DEFAULT_PROFILE_DIR "/sdcard/battery-profiles"
#define CAPACITY_FILENAME "/sys/class/power_supply/battery/capacity"
#define AC_FILENAME "/sys/class/power_supply/usb/online"
#define AC2_FILENAME "/sys/class/power_supply/ac/online"
#define SLEEP_INTERVAL_SEC 7
#define ONDEMAND_NODE "/sys/devices/system/cpu/cpufreq/ondemand"
#define MAX_PROFILES 64
#define CPU_NODE_FMT "/sys/devices/system/cpu/cpu%d/online"

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
} profile_t;

typedef struct meta {
    const char* name;
    off_t offset;
} meta_t;

#define FIELD(dtype, member) \
    { #member, offsetof(dtype, member) }
#define FIELD_END { NULL, -1 }

static const meta_t meta_profile[] = {
    FIELD(profile_t, down_differential),
    FIELD(profile_t, ignore_nice_load),
    FIELD(profile_t, powersave_bias),
    FIELD(profile_t, up_threshold),
    FIELD(profile_t, sampling_down_factor),
    FIELD(profile_t, sampling_rate),
    FIELD(profile_t, battery),
    FIELD(profile_t, ac),
    FIELD(profile_t, cpu_mask),
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
    profile_t ret = { {0} };
    char buf[BUFSIZ];
    FILE* stream = fopen(filename, "r");
    int i;

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

    int lineno = 0;

    while (lineno++, fgets(buf, BUFSIZ-1, stream))
    {
        if (*buf)
            buf[strlen(buf)-1] = '\0';
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
    int i, fd, value;
    char path[PATH_MAX+1] = {0}, buf[256];
    char const* ptr = (void*) profile;
    for (i = 0; meta[i].name; i++)
    {
        /* XXX hack */
        if (!strcmp(meta[i].name, "cpu_mask"))
        {
            for (i = 0; i < 8; i++)
            {
                snprintf(path, PATH_MAX, CPU_NODE_FMT, i);
                fd = open(path, O_WRONLY);
                if (fd == -1)
                    continue;
                write(fd, (profile->cpu_mask & (1 << (i+1))) ? "1" : "0", 1);
                close(fd);
            }
            continue;
        }
        /* XXX hack */
        if (!strcmp(meta[i].name, "ac") || !strcmp(meta[i].name, "battery"))
            continue;
        value = *(int*)(ptr + meta[i].offset);
        if (value == -1)
            continue;
        snprintf(path, PATH_MAX, "%s/%s", ONDEMAND_NODE, meta[i].name);
        fd = open(path, O_WRONLY);
        if (fd == -1)
        {
            fprintf(stderr, "open '%s': %s\n", path, strerror(errno));
            continue;
        }
        sprintf(buf, "%d", value);
        fprintf(stderr, "apply %s: %d\n", path, value);
        write(fd, buf, strlen(buf));
        close(fd);
    }
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

    if (ac_p && p->ac > 0)
        return 1;

    if(bat < p->battery)
        return 1;

    return 0;
}

int main(int argc, char** argv)
{
    const char* profile_dir;
    int cnt, i;
    static profile_t profiles[MAX_PROFILES] = { { {0} } }; /* no stackalloc */

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
                fprintf(stderr, "%ld -- applying profile %s\n", time(NULL), profiles[i].name);
                apply_profile(&profiles[i], meta_profile);
                break;
            }
        }

        if (i == cnt)
            fprintf(stderr, "applied no profile!\n");

        sleep(SLEEP_INTERVAL_SEC);
    }

    return 0;
}
