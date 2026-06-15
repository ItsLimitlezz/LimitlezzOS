#include "sim_fs.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
typedef struct _stat sim_stat_t;
#define SIM_MKDIR(path) _mkdir(path)
#define SIM_RMDIR(path) _rmdir(path)
#define SIM_UNLINK(path) _unlink(path)
#define SIM_STAT(path, st) _stat(path, st)
#define SIM_ISDIR(mode) (((mode) & _S_IFDIR) != 0)
#else
#include <unistd.h>
typedef struct stat sim_stat_t;
#define SIM_MKDIR(path) mkdir(path, 0755)
#define SIM_RMDIR(path) rmdir(path)
#define SIM_UNLINK(path) unlink(path)
#define SIM_STAT(path, st) stat(path, st)
#define SIM_ISDIR(mode) S_ISDIR(mode)
#endif

static void sim_remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if(d) {
        struct dirent *ent;
        while((ent = readdir(d)) != NULL) {
            if(strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            char child[512];
            snprintf(child, sizeof child, "%s/%s", path, ent->d_name);
            sim_stat_t st;
            if(SIM_STAT(child, &st) == 0 && SIM_ISDIR(st.st_mode)) sim_remove_tree(child);
            else SIM_UNLINK(child);
        }
        closedir(d);
    }
    SIM_RMDIR(path);
}

void sim_reset_dir(const char *path)
{
    sim_remove_tree(path);
    SIM_MKDIR(path);
}
