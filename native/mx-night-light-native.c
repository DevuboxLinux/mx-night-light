#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_TEMP 5500
#define MIN_TEMP 1500
#define MAX_TEMP 5500

typedef struct {
    bool status;
    int temp;
    bool autostart;
} Settings;

static void settings_defaults(Settings *s) {
    s->status = false;
    s->temp = DEFAULT_TEMP;
    s->autostart = false;
}

static void trim(char *s) {
    char *start = s;
    char *end;

    while (*start && isspace((unsigned char)*start)) {
        start++;
    }

    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }

    end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) {
        *(--end) = '\0';
    }
}

static int parse_bool(const char *value, bool *out) {
    if (!value || !out) {
        return -1;
    }

    if (strcasecmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0) {
        *out = true;
        return 0;
    }

    if (strcasecmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "no") == 0 || strcasecmp(value, "off") == 0) {
        *out = false;
        return 0;
    }

    return -1;
}

static int parse_temp(const char *value, int *out) {
    char *endptr = NULL;
    long parsed;

    if (!value || !out) {
        return -1;
    }

    errno = 0;
    parsed = strtol(value, &endptr, 10);
    if (errno != 0 || endptr == value || *endptr != '\0') {
        return -1;
    }

    if (parsed < MIN_TEMP || parsed > MAX_TEMP) {
        return -1;
    }

    *out = (int)parsed;
    return 0;
}

static int join2(char *dst, size_t dst_size, const char *a, const char *b) {
    size_t a_len;
    size_t b_len;

    if (!dst || !a || !b || dst_size == 0) {
        return -1;
    }

    a_len = strlen(a);
    b_len = strlen(b);
    if (a_len + b_len + 1 > dst_size) {
        return -1;
    }

    memcpy(dst, a, a_len);
    memcpy(dst + a_len, b, b_len + 1);
    return 0;
}

static int ensure_dir(const char *path) {
    char tmp[PATH_MAX];
    size_t len;

    if (!path) {
        return -1;
    }

    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) {
        return -1;
    }

    len = strlen(tmp);
    if (len == 0) {
        return -1;
    }

    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

static int get_paths(char *config_dir, size_t config_dir_size,
                     char *config_file, size_t config_file_size,
                     char *autostart_link, size_t autostart_link_size) {
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    const char *base = NULL;
    char fallback[PATH_MAX];

    if (xdg_config && *xdg_config) {
        base = xdg_config;
    } else {
        if (!home || !*home) {
            home = "/tmp";
        }
        if (join2(fallback, sizeof(fallback), home, "/.config") != 0) {
            return -1;
        }
        base = fallback;
    }

    if (join2(config_dir, config_dir_size, base, "/mx-night-light") != 0 ||
        join2(config_file, config_file_size, config_dir, "/settings.ini") != 0 ||
        join2(autostart_link, autostart_link_size, base,
              "/autostart/mx-night-light-autostart.desktop") != 0) {
        return -1;
    }

    return 0;
}

static int load_settings(const char *path, Settings *s) {
    FILE *fp = NULL;
    char line[256];
    bool in_main = false;

    settings_defaults(s);

    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *eq;
        trim(line);

        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
            continue;
        }

        if (line[0] == '[') {
            in_main = (strcasecmp(line, "[Main]") == 0);
            continue;
        }

        if (!in_main) {
            continue;
        }

        eq = strchr(line, '=');
        if (!eq) {
            continue;
        }

        *eq = '\0';
        eq++;
        trim(line);
        trim(eq);

        if (strcmp(line, "status") == 0) {
            parse_bool(eq, &s->status);
        } else if (strcmp(line, "autostart") == 0) {
            parse_bool(eq, &s->autostart);
        } else if (strcmp(line, "temp") == 0) {
            int parsed;
            if (parse_temp(eq, &parsed) == 0) {
                s->temp = parsed;
            }
        }
    }

    fclose(fp);
    return 0;
}

static int save_settings(const char *config_dir, const char *path, const Settings *s) {
    FILE *fp;

    if (ensure_dir(config_dir) != 0) {
        perror("failed to create config directory");
        return -1;
    }

    fp = fopen(path, "w");
    if (!fp) {
        perror("failed to open settings file");
        return -1;
    }

    fprintf(fp, "[Main]\n");
    fprintf(fp, "status=%s\n", s->status ? "True" : "False");
    fprintf(fp, "temp=%d\n", s->temp);
    fprintf(fp, "autostart=%s\n", s->autostart ? "True" : "False");

    fclose(fp);
    return 0;
}

static int run_command(char *const argv[]) {
    pid_t pid;
    int status;

    pid = fork();
    if (pid < 0) {
        perror("fork failed");
        return -1;
    }

    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid failed");
        return -1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    return -1;
}

static int run_redshift_off(void) {
    char *const argv[] = {"redshift", "-x", NULL};
    return run_command(argv);
}

static int run_redshift_temp(int temperature) {
    char temp_value[16];
    char *const argv[] = {"redshift", "-P", "-O", temp_value, NULL};

    if (snprintf(temp_value, sizeof(temp_value), "%d", temperature) >= (int)sizeof(temp_value)) {
        return -1;
    }

    return run_command(argv);
}

static int handle_autostart(bool enabled, const char *autostart_link, const char *desktop_target) {
    char autostart_dir[PATH_MAX];
    char *last_slash;

    if (snprintf(autostart_dir, sizeof(autostart_dir), "%s", autostart_link) >= (int)sizeof(autostart_dir)) {
        return -1;
    }

    last_slash = strrchr(autostart_dir, '/');
    if (!last_slash) {
        return -1;
    }

    *last_slash = '\0';

    if (ensure_dir(autostart_dir) != 0) {
        perror("failed to create autostart directory");
        return -1;
    }

    if (enabled) {
        unlink(autostart_link);
        if (symlink(desktop_target, autostart_link) != 0) {
            perror("failed to create autostart symlink");
            return -1;
        }
    } else {
        if (unlink(autostart_link) != 0 && errno != ENOENT) {
            perror("failed to remove autostart symlink");
            return -1;
        }
    }

    return 0;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage:\n"
            "  %s --set <0|1> [--temp <1500-5500>]\n"
            "  %s --color <1500-5500>\n"
            "  %s --reset\n"
            "  %s --autostart <0|1> [--desktop /path/to/mx-night-light-autostart.desktop]\n"
            "  %s --print-config\n",
            prog, prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    char config_dir[PATH_MAX];
    char config_file[PATH_MAX];
    char autostart_link[PATH_MAX];
    const char *desktop_target = "/usr/share/mx-night-light/data/mx-night-light-autostart.desktop";
    Settings settings;

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (get_paths(config_dir, sizeof(config_dir),
                  config_file, sizeof(config_file),
                  autostart_link, sizeof(autostart_link)) != 0) {
        fprintf(stderr, "Failed to build config paths\n");
        return 1;
    }

    load_settings(config_file, &settings);

    if (strcmp(argv[1], "--set") == 0) {
        bool enable;

        if (argc < 3 || parse_bool(argv[2], &enable) != 0) {
            print_usage(argv[0]);
            return 1;
        }

        if (argc == 5) {
            if (strcmp(argv[3], "--temp") != 0 || parse_temp(argv[4], &settings.temp) != 0) {
                print_usage(argv[0]);
                return 1;
            }
        } else if (argc != 3) {
            print_usage(argv[0]);
            return 1;
        }

        settings.status = enable;
        if (settings.status) {
            if (run_redshift_temp(settings.temp) != 0) {
                fprintf(stderr, "redshift failed to apply temperature\n");
                return 1;
            }
        } else {
            if (run_redshift_off() != 0) {
                fprintf(stderr, "redshift failed to reset\n");
                return 1;
            }
        }

        return save_settings(config_dir, config_file, &settings) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "--color") == 0) {
        if (argc != 3 || parse_temp(argv[2], &settings.temp) != 0) {
            print_usage(argv[0]);
            return 1;
        }

        if (settings.status && run_redshift_temp(settings.temp) != 0) {
            fprintf(stderr, "redshift failed to apply temperature\n");
            return 1;
        }

        return save_settings(config_dir, config_file, &settings) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "--reset") == 0) {
        if (argc != 2) {
            print_usage(argv[0]);
            return 1;
        }

        settings.status = false;
        if (run_redshift_off() != 0) {
            fprintf(stderr, "redshift failed to reset\n");
            return 1;
        }

        return save_settings(config_dir, config_file, &settings) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "--autostart") == 0) {
        bool enable;

        if (argc != 3 && argc != 5) {
            print_usage(argv[0]);
            return 1;
        }

        if (parse_bool(argv[2], &enable) != 0) {
            print_usage(argv[0]);
            return 1;
        }

        if (argc == 5) {
            if (strcmp(argv[3], "--desktop") != 0) {
                print_usage(argv[0]);
                return 1;
            }
            desktop_target = argv[4];
        }

        settings.autostart = enable;
        if (handle_autostart(enable, autostart_link, desktop_target) != 0) {
            return 1;
        }

        return save_settings(config_dir, config_file, &settings) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "--print-config") == 0) {
        if (argc != 2) {
            print_usage(argv[0]);
            return 1;
        }

        printf("status=%d\n", settings.status ? 1 : 0);
        printf("temp=%d\n", settings.temp);
        printf("autostart=%d\n", settings.autostart ? 1 : 0);
        return 0;
    }

    print_usage(argv[0]);
    return 1;
}
