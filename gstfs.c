/*
 *  gstfs - a gstreamer filesystem
 */
#include <sys/types.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>
#include <fuse.h>
#include <errno.h>
#include <glib.h>
#include <gst/gst.h>
#include "xcode.h"

#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))

#define GSTFS_OPT_KEY(templ, elem, key) \
    { templ, offsetof(struct gstfs_mount_info, elem), key }

/* per-mount options and data structures */
struct gstfs_mount_info
{
    pthread_mutex_t cache_mutex; /* protects file_cache, cache_lru accesses */
    GHashTable *file_cache;      /* cache of transcoded audio */
    GQueue *cache_lru;           /* queue of items in LRU order */
    int max_cache_entries;       /* max # of entries in the cache */
    char *src_mnt;               /* directory we are mirroring */
    char *src_ext;               /* extension of files we transcode */
    char *dst_ext;               /* extension of target files */
    char *pipeline;              /* gstreamer pipeline */
};

/* This stuff is stored into file_cache by filename */
struct gstfs_file_info
{
    char *filename;           /* hash key */
    char *src_filename;       /* filename in other mount */
    pthread_mutex_t mutex;    /* protects this file info */
    bool passthru;            /* true if this is a file in the mirror */
    size_t len;               /* size of file */
    size_t alloc_len;         /* allocated size of buf */
    char *buf;                /* completely converted file */
    GList *list_node;         /* pointer for cache_lru */
};
static char *get_source_path(const char *filename);

static struct gstfs_mount_info mount_info;

void usage(const char *prog)
{
    printf("Usage: %s -o [options] mount_point\n\n"
           "where options can be:\n"
           "   src=[source directory]    (required)\n"
           "   src_ext=[mp3|ogg|...]     (required)\n"
           "   dst_ext=[mp3|ogg|...]     (required)\n"
           "   pipeline=[gst pipeline]   (required)\n"
           "   ncache=[0-9]*             (optional)\n",
           prog);
}

/*
 *  Create a new gstfs_file_info object using the specified destination file.
 */
struct gstfs_file_info *get_file_info(const char *filename)
{
    struct gstfs_file_info *fi;
    struct stat stbuf;

    fi = calloc(1, sizeof(struct gstfs_file_info));
    fi->filename = g_strdup(filename);
    fi->src_filename = get_source_path(filename);
    fi->passthru = !is_target_type(filename) ||
                   is_target_type(fi->src_filename);

    if (stat(fi->src_filename, &stbuf) == 0)
        fi->len = stbuf.st_size;

    pthread_mutex_init(&fi->mutex, NULL);
    return fi;
}

/*
 *  Release a previously allocated gstfs_file_info object.
 */
void put_file_info(struct gstfs_file_info *fi)
{
    g_free(fi->filename);
    g_free(fi->src_filename);
    free(fi);
}

/*
 *  Given a filename with extension "search", return a possibly reallocated
 *  string with "replace" on the end.
 */
char *replace_ext(char *filename, char *search, char *replace)
{
    char *ext = strrchr(filename, '.');
    if (ext && strcmp(ext+1, search) == 0) 
    {
        *(ext+1) = 0;
        filename = g_strconcat(filename, replace, NULL);
    }
    return filename;
}

/*
 *  Return true if filename has extension dst_ext.
 */
int is_target_type(const char *filename)
{
    char *ext = strrchr(filename, '.');
    return (ext && strcmp(ext+1, mount_info.dst_ext) == 0);
}

/*
 *  Return true if filename exists in the original dir.
 */
bool exists_in_mirror(const char *filename)
{
    int result;
    struct statvfs buf;

    result = gstfs_statfs(filename, &buf);
    return result == 0;
}

/*  
 *  Remove items from the file cache until below the maximum.
 *  This is relatively quick since we can find elements by looking at the
 *  head of the lru list and then do a single hash lookup to remove from 
 *  the hash table.
 *
 *  Called with cache_mutex held.
 */
static void expire_cache()
{
    struct gstfs_file_info *fi;

    while (g_queue_get_length(mount_info.cache_lru) > 
           mount_info.max_cache_entries)
    {
        fi = (struct gstfs_file_info *) g_queue_pop_head(mount_info.cache_lru);
        g_hash_table_remove(mount_info.file_cache, fi);
        put_file_info(fi);
    }
}

/*
 *  If the path represents a file in the mirror filesystem, then
 *  look for it in the cache.  If not, create a new file info.
 *
 *  If it isn't a mirror file, return NULL.
 */
static struct gstfs_file_info *gstfs_lookup(const char *path)
{
    struct gstfs_file_info *ret;

    if (!is_target_type(path) && !exists_in_mirror(path))
        return NULL;

    pthread_mutex_lock(&mount_info.cache_mutex);
    ret = g_hash_table_lookup(mount_info.file_cache, path);
    if (!ret)
    {
        ret = get_file_info(path);
        if (!ret)
            goto out;

        g_hash_table_replace(mount_info.file_cache, ret->filename, ret);
    }

    // move to end of LRU
    if (ret->list_node)
        g_queue_unlink(mount_info.cache_lru, ret->list_node);

    g_queue_push_tail(mount_info.cache_lru, ret);
    ret->list_node = mount_info.cache_lru->tail;

    expire_cache();

out:
    pthread_mutex_unlock(&mount_info.cache_mutex);
    return ret;
}

/*
 *  Given a filename from the fuse mount, return the corresponding filename 
 *  in the mirror.
 */
static char *get_source_path(const char *filename)
{
    struct stat buf;
    char *source;

    source = g_strdup_printf("%s%s", mount_info.src_mnt, filename);

    /* if target file already exists in source, then don't transcode */
    if (stat(source, &buf) == 0)
        return source;

    source = replace_ext(source, mount_info.dst_ext, mount_info.src_ext);
    return source;
}

static char *canonize(const char *cwd, const char *filename)
{
    if (filename[0] == '/')
        return g_strdup(filename);
    else
        return g_strdup_printf("%s/%s", cwd, filename);
}

int gstfs_statfs(const char *path, struct statvfs *buf)
{
    char *source_path;

    source_path = get_source_path(path);
    if (statvfs(source_path, buf))
        return -errno;

    g_free(source_path);
    return 0;
}

int gstfs_getattr(const char *path, struct stat *stbuf)
{
    int ret = 0;
    char *source_path;
    struct gstfs_file_info *converted;

    source_path = get_source_path(path);

    if (stat(source_path, stbuf))
        ret = -errno;
    else if ((converted = gstfs_lookup(path)))
        stbuf->st_size = converted->len;

    g_free(source_path);
    return ret;
}

static int read_cb(char *buf, size_t size, void *data)
{
    struct gstfs_file_info *info = (struct gstfs_file_info *) data;

    size_t newsz = info->len + size;
   
    if (info->alloc_len < newsz)
    {
        info->alloc_len = max(info->alloc_len * 2, newsz);
        info->buf = realloc(info->buf, info->alloc_len);
        if (!info->buf)
            return -ENOMEM;
    }

    memcpy(&info->buf[info->len], buf, size);
    info->len += size;
    return 0;
}

int gstfs_read_passthru(const char *path, char *buf, size_t size, off_t offset)
{
    size_t count;
    int fd = open(path, O_RDONLY);

    lseek(fd, offset, SEEK_SET);
    count = read(fd, buf, size);
    close(fd);
    return count;
}

int gstfs_read(const char *path, char *buf, size_t size, off_t offset, 
    struct fuse_file_info *fi)
{
    struct gstfs_file_info *info = gstfs_lookup(path);
    size_t count = 0;

    if (!info)
        return -ENOENT;

    pthread_mutex_lock(&info->mutex);
    if (info->passthru)
        return gstfs_read_passthru(info->src_filename, buf, size, offset);

    if (!info->buf)
        transcode(mount_info.pipeline, info->src_filename, read_cb, info);
    
    if (info->len <= offset)
        goto out;

    count = min(info->len - offset, size);

    memcpy(buf, &info->buf[offset], count);

out:
    pthread_mutex_unlock(&info->mutex);
    return count;
}

int gstfs_open(const char *path, struct fuse_file_info *fi)
{
    struct gstfs_file_info *info = gstfs_lookup(path);
    if (!info)
        return -ENOENT;

    return 0;
}

/*
 *  Copy all entries from source mount, replacing extensions along the way.
 */
int gstfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
    off_t offset, struct fuse_file_info *fi)
{
    struct dirent *dirent;
    DIR *dir;
    char *source_path;

    source_path = get_source_path(path);
    dir = opendir(source_path);

    if (!dir)
        return -ENOENT;

    while ((dirent = readdir(dir)))
    {
        char *s = g_strdup(dirent->d_name);
        s = replace_ext(s, mount_info.src_ext, mount_info.dst_ext);
        filler(buf, s, NULL, 0);

        g_free(s);
    }
    closedir(dir);

    return 0;
}

static struct fuse_operations gstfs_opers = {
    .readdir = gstfs_readdir,
    .statfs = gstfs_statfs,
    .getattr = gstfs_getattr,
    .open = gstfs_open,
    .read = gstfs_read
};

static struct fuse_opt gstfs_opts[] = {
    GSTFS_OPT_KEY("src=%s", src_mnt, 0),
    GSTFS_OPT_KEY("src_ext=%s", src_ext, 0),
    GSTFS_OPT_KEY("dst_ext=%s", dst_ext, 0),
    GSTFS_OPT_KEY("ncache=%d", max_cache_entries, 0),
    GSTFS_OPT_KEY("pipeline=%s", pipeline, 0),
    FUSE_OPT_END
};


int main(int argc, char *argv[])
{
    char pwd[2048];
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    if (fuse_opt_parse(&args, &mount_info, gstfs_opts, NULL) == -1)
        return -1;

    if (!mount_info.src_mnt ||
        !mount_info.src_ext ||
        !mount_info.dst_ext ||
        !mount_info.pipeline)
    {
        usage(argv[0]);
        return -1;
    }

    if (!getcwd(pwd, sizeof(pwd)))
    {
        perror("gstfs");
        return -1;
    }

    mount_info.src_mnt = canonize(pwd, mount_info.src_mnt);

    if (mount_info.max_cache_entries == 0)
        mount_info.max_cache_entries = 50;

    pthread_mutex_init(&mount_info.cache_mutex, NULL);
    mount_info.file_cache = g_hash_table_new(g_str_hash, g_str_equal);
    mount_info.cache_lru = g_queue_new();

    gst_init(&argc, &argv);
    return fuse_main(args.argc, args.argv, &gstfs_opers, NULL);
}
