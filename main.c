/*
 * main.c
 *
 * the command line user interface
 */
#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <linux/limits.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include "logic.h"
#include "sh.h"

#define STRING_BUFFER_SIZE PATH_MAX * 2

void print_help() {
    puts("Usage: %s command options");
    puts("");
    puts("Commands:");
    puts("  vacuum - remove duplicated files in upperdir where copy_up is done but the file is not actually modified");
    puts("  diff   - show the list of actually changed files");
    puts("  merge  - merge all changes from upperdir to lowerdir, and clear upperdir");
    puts("");
    puts("Options:");
    puts("  -l, --lowerdir=LOWERDIR    the lowerdir of OverlayFS (required)");
    puts("  -u, --upperdir=UPPERDIR    the upperdir of OverlayFS (required)");
    puts("  -v, --verbose              with diff action only: when a directory only exists in one version, still list every file of the directory");
    puts("  -h, --help                 show this help text");
    puts("");
    puts("See https://github.com/kmxz/overlayfs-tools/ for warnings and more information.");
}

bool starts_with(const char *haystack, const char* needle) {
    return strncmp(needle, haystack, strlen(needle)) == 0;
}

bool is_mounted(const char *lower, const char *upper) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) {
        fprintf(stderr, "Cannot read /proc/mounts to test whether OverlayFS is mounted.\n");
        return true;
    }
    char buf[STRING_BUFFER_SIZE];
    while (fgets(buf, STRING_BUFFER_SIZE, f)) {
        if (!starts_with(buf, "overlay")) {
            continue;
        }
        if (strlen(buf) == STRING_BUFFER_SIZE) {
            fprintf(stderr, "OverlayFS line in /proc/mounts is too long.\n");
            return true;
        }
        char *m_lower = strstr(buf, "lowerdir=");
        char *m_upper = strstr(buf, "upperdir=");
        if (m_lower == NULL || m_upper == NULL) {
            fprintf(stderr, "Cannot extract information from OverlayFS line in /proc/mounts.\n");
            return true;
        }
        m_lower = &(m_lower[strlen("lowerdir=")]);
        m_upper = &(m_upper[strlen("upperdir=")]);
        if (!(strncmp(lower, m_lower, strlen(lower)) && strncmp(upper, m_upper, strlen(upper)))) {
            printf("The OverlayFS involved is still mounted.\n");
            return true;
        }
    }
    return false;
}

bool check_mounted(const char *lower, const char *upper) {
    if (is_mounted(lower, upper)) {
        printf("It is strongly recommended to unmount OverlayFS first. Still continue (not recommended)?: \n");
        int r = getchar();
        if (r != 'Y' && r != 'y') {
            return true;
        }
    }
    return false;
}

bool directory_exists(const char *path) {
    struct stat sb;
    if (lstat(path, &sb) != 0) { return false; }
    return (sb.st_mode & S_IFMT) == S_IFDIR;
}

bool real_check_xattr_trusted(const char *tmp_path, int tmp_file) {
    int ret = fsetxattr(tmp_file, "trusted.overlay.test", "naive", 5, 0);
    close(tmp_file);
    if (ret) { return false; }
    char verify_buffer[10];
    if (getxattr(tmp_path, "trusted.overlay.test", verify_buffer, 10) != 5) { return false; }
    return !strncmp(verify_buffer, "naive", 5);
}

bool check_xattr_trusted(const char *upper) {
    char tmp_path[PATH_MAX];
    strcpy(tmp_path, upper);
    strcat(tmp_path, "/.xattr_test_XXXXXX.tmp");
    int tmp_file = mkstemps(tmp_path, 4);
    if (tmp_file < 0) { return false; }
    bool ret = real_check_xattr_trusted(tmp_path, tmp_file);
    unlink(tmp_path);
    return ret;
}

int main(int argc, char *argv[]) {

    char lower[PATH_MAX] = "";
    char upper[PATH_MAX] = "";
    bool verbose = false;

    static struct option long_options[] = {
        { "lowerdir", required_argument, 0, 'l' },
        { "upperdir", required_argument, 0, 'u' },
        { "help",     no_argument      , 0, 'h' },
        { "verbose",  no_argument      , 0, 'v' },
        { 0,          0,                 0,  0  }
    };

    int opt = 0;
    int long_index = 0;
    while ((opt = getopt_long_only(argc, argv, "", long_options, &long_index)) != -1) {
        switch (opt) {
            case 'l':
                if (realpath(optarg, lower) == NULL) { lower[0] = '\0'; }
                break;
            case 'u':
                if (realpath(optarg, upper) == NULL) { upper[0] = '\0'; }
                break;
            case 'h':
                print_help();
                return EXIT_SUCCESS;
            case 'v':
                verbose = true;
                break;
            default:
                fprintf(stderr, "Option %c is not supported.\n", opt);
                goto see_help;
        }
    }

    if (lower[0] == '\0') {
        fprintf(stderr, "Lower directory not specified.\n");
        goto see_help;
    }
    if (!directory_exists(lower)) {
        fprintf(stderr, "Lower directory cannot be opened.\n");
        goto see_help;
    }
    if (upper[0] == '\0') {
        fprintf(stderr, "Upper directory not specified.\n");
        goto see_help;
    }
    if (!directory_exists(upper)) {
        fprintf(stderr, "Lower directory cannot be opened.\n");
        goto see_help;
    }
    if (!check_xattr_trusted(upper)) {
        fprintf(stderr, "The program cannot write trusted.* xattr. Try run again as root.\n");
        return EXIT_FAILURE;
    }
    if (check_mounted(lower, upper)) {
        return EXIT_FAILURE;
    }

    if (optind == argc - 1) {
        int out;
        char filename_template[] = "overlay-tools-XXXXXX.sh";
        FILE *script = NULL;
        if (strcmp(argv[optind], "diff") == 0) {
            out = diff(lower, upper, verbose);
        } else if (strcmp(argv[optind], "vacuum") == 0) {
            script = create_shell_script(filename_template);
            if (script == NULL) { fprintf(stderr, "Script file cannot be created.\n"); return EXIT_FAILURE; }
            out = vacuum(lower, upper, verbose, script);
        } else if (strcmp(argv[optind], "merge") == 0) {
            script = create_shell_script(filename_template);
            if (script == NULL) { fprintf(stderr, "Script file cannot be created.\n"); return EXIT_FAILURE; }
            out = merge(lower, upper, verbose, script);
        } else {
            fprintf(stderr, "Action not supported.\n");
            goto see_help;
        }
        if (script != NULL) {
            printf("The script %s is created. Run the script to do the actual work please. Remember to run it when the OverlayFS is not mounted.\n", filename_template);
            fclose(script);
        }
        if (out) {
            fprintf(stderr, "Action aborted due to fatal error.\n");
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    fprintf(stderr, "Please specify one action.\n");

see_help:
    fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
    return EXIT_FAILURE;

}
