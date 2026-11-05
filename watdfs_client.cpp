//
// Starter code for CS 454/654
// You SHOULD change this file
//

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "watdfs_client.h"
#include "debug.h"
INIT_LOG

#include "rpc.h"
#include "rw_lock.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

using uint = unsigned int;

struct WatdfsOpenFile {
    std::string path;
    std::string cache_path;
    int original_flags = 0;
    int local_fd = -1;
    bool write_mode = false;
    time_t tc = 0;
    struct timespec t_client = {0, 0};
    struct timespec t_server = {0, 0};
    struct fuse_file_info server_fi = {};
};

struct WatdfsClientState {
    std::string cache_dir;
    time_t cache_interval = 0;
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<WatdfsOpenFile>> open_files;
};

bool is_root_path(const char *path) {
    return (path != nullptr) && (strcmp(path, "/") == 0);
}

bool is_write_mode(int flags) {
    return (flags & O_ACCMODE) != O_RDONLY;
}

std::string cache_path_for(WatdfsClientState *state, const char *path) {
    if (path[0] == '/') {
        return state->cache_dir + path;
    }
    return state->cache_dir + "/" + path;
}

bool same_timespec(const struct timespec &lhs, const struct timespec &rhs) {
    return lhs.tv_sec == rhs.tv_sec && lhs.tv_nsec == rhs.tv_nsec;
}

int ensure_parent_dirs(const std::string &path) {
    size_t limit = path.find_last_of('/');
    if (limit == std::string::npos) {
        return 0;
    }

    size_t pos = 0;
    while (true) {
        pos = path.find('/', pos + 1);
        if (pos == std::string::npos || pos > limit) {
            break;
        }

        std::string dir = path.substr(0, pos);
        if (dir.empty()) {
            continue;
        }

        if (mkdir(dir.c_str(), 0777) < 0 && errno != EEXIST) {
            return -errno;
        }
    }

    return 0;
}

int stat_local_path(const std::string &path, struct stat *statbuf) {
    if (stat(path.c_str(), statbuf) < 0) {
        return -errno;
    }
    return 0;
}

struct timespec mtime_from_stat(const struct stat &statbuf) {
    return statbuf.st_mtim;
}

int write_all_local(int fd, const char *buf, size_t size, off_t offset) {
    size_t total = 0;
    while (total < size) {
        ssize_t written = pwrite(fd, buf + total, size - total,
                                 offset + static_cast<off_t>(total));
        if (written < 0) {
            return -errno;
        }
        total += static_cast<size_t>(written);
    }
    return 0;
}

int read_all_local(int fd, char *buf, size_t size, off_t offset) {
    size_t total = 0;
    while (total < size) {
        ssize_t bytes = pread(fd, buf + total, size - total,
                              offset + static_cast<off_t>(total));
        if (bytes < 0) {
            return -errno;
        }
        if (bytes == 0) {
            return -EIO;
        }
        total += static_cast<size_t>(bytes);
    }
    return 0;
}

std::shared_ptr<WatdfsOpenFile> find_open_file(WatdfsClientState *state,
                                               const char *path) {
    std::lock_guard<std::mutex> guard(state->mutex);
    auto it = state->open_files.find(path);
    if (it == state->open_files.end()) {
        return nullptr;
    }
    return it->second;
}

int add_open_file(WatdfsClientState *state,
                  const std::shared_ptr<WatdfsOpenFile> &file) {
    std::lock_guard<std::mutex> guard(state->mutex);
    if (state->open_files.count(file->path) > 0) {
        return -EMFILE;
    }
    state->open_files[file->path] = file;
    return 0;
}

void remove_open_file(WatdfsClientState *state, const std::string &path) {
    std::lock_guard<std::mutex> guard(state->mutex);
    state->open_files.erase(path);
}

WatdfsOpenFile *file_from_fi(struct fuse_file_info *fi) {
    if (fi == nullptr || fi->fh == 0) {
        return nullptr;
    }
    return reinterpret_cast<WatdfsOpenFile *>(fi->fh);
}

int rpc_getattr_remote(const char *path, struct stat *statbuf) {
    const int arg_count = 3;
    void **args = new void *[arg_count];
    int arg_types[arg_count + 1];

    int path_len = static_cast<int>(strlen(path)) + 1;
    arg_types[0] =
        (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) |
        static_cast<uint>(path_len);
    args[0] = const_cast<char *>(path);

    arg_types[1] =
        (1u << ARG_OUTPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) |
        static_cast<uint>(sizeof(struct stat));
    args[1] = statbuf;

    int retcode = 0;
    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    args[2] = &retcode;

    arg_types[3] = 0;

    int rpc_ret = rpcCall(const_cast<char *>("getattr"), arg_types, args);
    delete[] args;

    if (rpc_ret < 0) {
        memset(statbuf, 0, sizeof(struct stat));
        return -EINVAL;
    }
    if (retcode < 0) {
        memset(statbuf, 0, sizeof(struct stat));
    }
    return retcode;
}

int rpc_mknod_remote(const char *path, mode_t mode, dev_t dev) {
    const int arg_count = 4;
    void **args = new void *[arg_count];
    int arg_types[arg_count + 1];

    int path_len = static_cast<int>(strlen(path)) + 1;
    arg_types[0] =
        (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) |
        static_cast<uint>(path_len);
    args[0] = const_cast<char *>(path);

    int mode_i = static_cast<int>(mode);
    arg_types[1] = (1u << ARG_INPUT) | (ARG_INT << 16u);
    args[1] = &mode_i;

    long dev_l = static_cast<long>(dev);
    arg_types[2] = (1u << ARG_INPUT) | (ARG_LONG << 16u);
    args[2] = &dev_l;

    int retcode = 0;
    arg_types[3] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    args[3] = &retcode;

    arg_types[4] = 0;

    int rpc_ret = rpcCall(const_cast<char *>("mknod"), arg_types, args);
    delete[] args;

    if (rpc_ret < 0) {
        return -EINVAL;
    }
    return retcode;
}

int rpc_open_remote(const char *path, struct fuse_file_info *fi) {
    const int arg_count = 3;
    void **args = new void *[arg_count];
    int arg_types[arg_count + 1];

    int path_len = static_cast<int>(strlen(path)) + 1;
    arg_types[0] =
        (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) |
        static_cast<uint>(path_len);
    args[0] = const_cast<char *>(path);

    arg_types[1] =
        (1u << ARG_INPUT) | (1u << ARG_OUTPUT) | (1u << ARG_ARRAY) |
        (ARG_CHAR << 16u) | static_cast<uint>(sizeof(struct fuse_file_info));
    args[1] = fi;

    int retcode = 0;
    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    args[2] = &retcode;

    arg_types[3] = 0;

    int rpc_ret = rpcCall(const_cast<char *>("open"), arg_types, args);
    delete[] args;

    if (rpc_ret < 0) {
        return -EINVAL;
    }
    return retcode;
}

int rpc_release_remote(const char *path, struct fuse_file_info *fi) {
    const int arg_count = 3;
    void **args = new void *[arg_count];
    int arg_types[arg_count + 1];

    int path_len = static_cast<int>(strlen(path)) + 1;
    arg_types[0] =
        (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) |
        static_cast<uint>(path_len);
    args[0] = const_cast<char *>(path);

    arg_types[1] = (1u << ARG_INPUT) | (1u << ARG_ARRAY) |
                   (ARG_CHAR << 16u) |
                   static_cast<uint>(sizeof(struct fuse_file_info));
    args[1] = fi;

    int retcode = 0;
    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    args[2] = &retcode;

    arg_types[3] = 0;

    int rpc_ret = rpcCall(const_cast<char *>("release"), arg_types, args);
    delete[] args;

    if (rpc_ret < 0) {
        return -EINVAL;
    }
    return retcode;
}

int rpc_read_remote(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi) {
    size_t total = 0;
    while (total < size) {
        size_t chunk = std::min(size - total, static_cast<size_t>(MAX_ARRAY_LEN));

        const int arg_count = 6;
        void **args = new void *[arg_count];
        int arg_types[arg_count + 1];

        int path_len = static_cast<int>(strlen(path)) + 1;
        arg_types[0] =
            (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) |
            static_cast<uint>(path_len);
        args[0] = const_cast<char *>(path);

        arg_types[1] =
            (1u << ARG_OUTPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) |
            static_cast<uint>(chunk);
        args[1] = buf + total;

        long size_l = static_cast<long>(chunk);
        arg_types[2] = (1u << ARG_INPUT) | (ARG_LONG << 16u);
        args[2] = &size_l;

        long offset_l = static_cast<long>(offset + static_cast<off_t>(total));
        arg_types[3] = (1u << ARG_INPUT) | (ARG_LONG << 16u);
        args[3] = &offset_l;

        arg_types[4] = (1u << ARG_INPUT) | (1u << ARG_ARRAY) |
                       (ARG_CHAR << 16u) |
                       static_cast<uint>(sizeof(struct fuse_file_info));
        args[4] = fi;

        int retcode = 0;
        arg_types[5] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
        args[5] = &retcode;

        arg_types[6] = 0;

        int rpc_ret = rpcCall(const_cast<char *>("read"), arg_types, args);
        delete[] args;

        if (rpc_ret < 0) {
            return -EINVAL;
        }
        if (retcode < 0) {
            return retcode;
        }

        total += static_cast<size_t>(retcode);
        if (static_cast<size_t>(retcode) < chunk) {
            break;
        }
    }

    return static_cast<int>(total);
}

int rpc_write_remote(const char *path, const char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi) {
    size_t total = 0;
    while (total < size) {
        size_t chunk = std::min(size - total, static_cast<size_t>(MAX_ARRAY_LEN));

        const int arg_count = 6;
        void **args = new void *[arg_count];
        int arg_types[arg_count + 1];

        int path_len = static_cast<int>(strlen(path)) + 1;
        arg_types[0] =
            (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) |
            static_cast<uint>(path_len);
        args[0] = const_cast<char *>(path);

        arg_types[1] =
            (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) |
            static_cast<uint>(chunk);
        args[1] = const_cast<char *>(buf + total);

        long size_l = static_cast<long>(chunk);
        arg_types[2] = (1u << ARG_INPUT) | (ARG_LONG << 16u);
        args[2] = &size_l;

        long offset_l = static_cast<long>(offset + static_cast<off_t>(total));
        arg_types[3] = (1u << ARG_INPUT) | (ARG_LONG << 16u);
        args[3] = &offset_l;

        arg_types[4] = (1u << ARG_INPUT) | (1u << ARG_ARRAY) |
                       (ARG_CHAR << 16u) |
                       static_cast<uint>(sizeof(struct fuse_file_info));
        args[4] = fi;

        int retcode = 0;
        arg_types[5] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
        args[5] = &retcode;

        arg_types[6] = 0;

        int rpc_ret = rpcCall(const_cast<char *>("write"), arg_types, args);
        delete[] args;

        if (rpc_ret < 0) {
            return -EINVAL;
        }
        if (retcode < 0) {
            return retcode;
        }

        total += static_cast<size_t>(retcode);
        if (retcode == 0) {
            return -EIO;
        }
    }

    return static_cast<int>(total);
}

int rpc_truncate_remote(const char *path, off_t newsize) {
    const int arg_count = 3;
    void **args = new void *[arg_count];
    int arg_types[arg_count + 1];

    int path_len = static_cast<int>(strlen(path)) + 1;
    arg_types[0] =
        (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) |
        static_cast<uint>(path_len);
    args[0] = const_cast<char *>(path);

    long newsize_l = static_cast<long>(newsize);
    arg_types[1] = (1u << ARG_INPUT) | (ARG_LONG << 16u);
    args[1] = &newsize_l;

    int retcode = 0;
    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    args[2] = &retcode;

    arg_types[3] = 0;

    int rpc_ret = rpcCall(const_cast<char *>("truncate"), arg_types, args);
    delete[] args;

    if (rpc_ret < 0) {
        return -EINVAL;
    }
    return retcode;
}

int rpc_fsync_remote(const char *path, struct fuse_file_info *fi) {
    const int arg_count = 3;
    void **args = new void *[arg_count];
    int arg_types[arg_count + 1];

    int path_len = static_cast<int>(strlen(path)) + 1;
    arg_types[0] =
        (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) |
        static_cast<uint>(path_len);
    args[0] = const_cast<char *>(path);

    arg_types[1] = (1u << ARG_INPUT) | (1u << ARG_ARRAY) |
                   (ARG_CHAR << 16u) |
                   static_cast<uint>(sizeof(struct fuse_file_info));
    args[1] = fi;

    int retcode = 0;
    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    args[2] = &retcode;

    arg_types[3] = 0;

    int rpc_ret = rpcCall(const_cast<char *>("fsync"), arg_types, args);
    delete[] args;

    if (rpc_ret < 0) {
        return -EINVAL;
    }
    return retcode;
}

int rpc_utimensat_remote(const char *path, const struct timespec ts[2]) {
    const int arg_count = 3;
    void **args = new void *[arg_count];
    int arg_types[arg_count + 1];

    int path_len = static_cast<int>(strlen(path)) + 1;
    arg_types[0] =
        (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) |
        static_cast<uint>(path_len);
    args[0] = const_cast<char *>(path);

    arg_types[1] =
        (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) |
        static_cast<uint>(sizeof(struct timespec) * 2);
    args[1] = const_cast<struct timespec *>(ts);

    int retcode = 0;
    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    args[2] = &retcode;

    arg_types[3] = 0;

    int rpc_ret = rpcCall(const_cast<char *>("utimensat"), arg_types, args);
    delete[] args;

    if (rpc_ret < 0) {
        return -EINVAL;
    }
    return retcode;
}

int rpc_transfer_lock(const char *path, rw_lock_mode_t mode) {
    const int arg_count = 3;
    void **args = new void *[arg_count];
    int arg_types[arg_count + 1];

    int path_len = static_cast<int>(strlen(path)) + 1;
    arg_types[0] =
        (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) |
        static_cast<uint>(path_len);
    args[0] = const_cast<char *>(path);

    int mode_i = static_cast<int>(mode);
    arg_types[1] = (1u << ARG_INPUT) | (ARG_INT << 16u);
    args[1] = &mode_i;

    int retcode = 0;
    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    args[2] = &retcode;

    arg_types[3] = 0;

    int rpc_ret = rpcCall(const_cast<char *>("lock"), arg_types, args);
    delete[] args;

    if (rpc_ret < 0) {
        return -EINVAL;
    }
    return retcode;
}

int rpc_transfer_unlock(const char *path, rw_lock_mode_t mode) {
    const int arg_count = 3;
    void **args = new void *[arg_count];
    int arg_types[arg_count + 1];

    int path_len = static_cast<int>(strlen(path)) + 1;
    arg_types[0] =
        (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) |
        static_cast<uint>(path_len);
    args[0] = const_cast<char *>(path);

    int mode_i = static_cast<int>(mode);
    arg_types[1] = (1u << ARG_INPUT) | (ARG_INT << 16u);
    args[1] = &mode_i;

    int retcode = 0;
    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    args[2] = &retcode;

    arg_types[3] = 0;

    int rpc_ret = rpcCall(const_cast<char *>("unlock"), arg_types, args);
    delete[] args;

    if (rpc_ret < 0) {
        return -EINVAL;
    }
    return retcode;
}

int server_transfer_flags(int original_flags) {
    int flags = original_flags & ~(O_CREAT | O_EXCL);
    int access_mode = original_flags & O_ACCMODE;
    if (access_mode == O_RDONLY) {
        flags = (flags & ~O_ACCMODE) | O_RDONLY;
    } else {
        flags = (flags & ~O_ACCMODE) | O_RDWR;
    }
    return flags;
}

int application_open_flags(int original_flags) {
    return original_flags & ~(O_CREAT | O_EXCL);
}

int copy_server_file_to_cache_locked(WatdfsOpenFile *file,
                                     const struct stat &server_stat) {
    int ret = ensure_parent_dirs(file->cache_path);
    if (ret < 0) {
        return ret;
    }

    int transfer_fd =
        open(file->cache_path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0666);
    if (transfer_fd < 0) {
        return -errno;
    }

    std::unique_ptr<char[]> buffer(new (std::nothrow) char[MAX_ARRAY_LEN]);
    if (!buffer) {
        ret = -ENOMEM;
    } else if (fchmod(transfer_fd, server_stat.st_mode & 07777) < 0) {
        ret = -errno;
    } else {
        off_t offset = 0;
        while (ret == 0 && offset < server_stat.st_size) {
            size_t chunk = static_cast<size_t>(
                std::min<off_t>(server_stat.st_size - offset, MAX_ARRAY_LEN));
            int bytes_read =
                rpc_read_remote(file->path.c_str(), buffer.get(), chunk, offset,
                                &file->server_fi);
            if (bytes_read < 0) {
                ret = bytes_read;
            } else if (bytes_read == 0) {
                ret = -EIO;
            } else {
                ret = write_all_local(transfer_fd, buffer.get(),
                                      static_cast<size_t>(bytes_read), offset);
                offset += bytes_read;
            }
        }

        if (ret == 0 && ftruncate(transfer_fd, server_stat.st_size) < 0) {
            ret = -errno;
        }

        if (ret == 0) {
            struct timespec ts[2] = {server_stat.st_atim, server_stat.st_mtim};
            if (futimens(transfer_fd, ts) < 0) {
                ret = -errno;
            }
        }

        if (ret == 0) {
            struct stat local_stat = {};
            if (fstat(transfer_fd, &local_stat) < 0) {
                ret = -errno;
            } else {
                file->t_client = mtime_from_stat(local_stat);
                file->t_server = mtime_from_stat(server_stat);
                file->tc = time(nullptr);
            }
        }
    }

    {
        int saved_errno = errno;
        if (close(transfer_fd) < 0 && ret == 0) {
            ret = -errno;
        }
        errno = saved_errno;
    }
    return ret;
}

int download_server_copy(WatdfsOpenFile *file) {
    int ret = rpc_transfer_lock(file->path.c_str(), RW_READ_LOCK);
    if (ret < 0) {
        return ret;
    }

    struct stat server_stat = {};
    ret = rpc_getattr_remote(file->path.c_str(), &server_stat);
    if (ret == 0) {
        ret = copy_server_file_to_cache_locked(file, server_stat);
    }

    int unlock_ret = rpc_transfer_unlock(file->path.c_str(), RW_READ_LOCK);
    if (ret == 0 && unlock_ret < 0) {
        ret = unlock_ret;
    }

    return ret;
}

int upload_cache_to_server(WatdfsOpenFile *file) {
    int ret = rpc_transfer_lock(file->path.c_str(), RW_WRITE_LOCK);
    if (ret < 0) {
        return ret;
    }

    int local_fd = open(file->cache_path.c_str(), O_RDONLY);
    if (local_fd < 0) {
        ret = -errno;
    } else {
        struct stat local_stat = {};
        std::unique_ptr<char[]> buffer(new (std::nothrow) char[MAX_ARRAY_LEN]);

        if (fstat(local_fd, &local_stat) < 0) {
            ret = -errno;
        } else if (!buffer) {
            ret = -ENOMEM;
        } else {
            ret = rpc_truncate_remote(file->path.c_str(), local_stat.st_size);

            off_t offset = 0;
            while (ret == 0 && offset < local_stat.st_size) {
                size_t chunk = static_cast<size_t>(
                    std::min<off_t>(local_stat.st_size - offset, MAX_ARRAY_LEN));
                ret = read_all_local(local_fd, buffer.get(), chunk, offset);
                if (ret < 0) {
                    break;
                }

                int bytes_written =
                    rpc_write_remote(file->path.c_str(), buffer.get(), chunk,
                                     offset, &file->server_fi);
                if (bytes_written < 0) {
                    ret = bytes_written;
                } else if (static_cast<size_t>(bytes_written) != chunk) {
                    ret = -EIO;
                } else {
                    offset += static_cast<off_t>(chunk);
                }
            }

            if (ret == 0) {
                struct timespec ts[2] = {local_stat.st_atim, local_stat.st_mtim};
                ret = rpc_utimensat_remote(file->path.c_str(), ts);
            }

            if (ret == 0) {
                ret = rpc_fsync_remote(file->path.c_str(), &file->server_fi);
            }

            if (ret == 0) {
                file->t_client = mtime_from_stat(local_stat);
                file->t_server = file->t_client;
                file->tc = time(nullptr);
            }
        }

        int saved_errno = errno;
        if (close(local_fd) < 0 && ret == 0) {
            ret = -errno;
        }
        errno = saved_errno;
    }

    {
        int unlock_ret = rpc_transfer_unlock(file->path.c_str(), RW_WRITE_LOCK);
        if (ret == 0 && unlock_ret < 0) {
            ret = unlock_ret;
        }
    }
    return ret;
}

int refresh_readonly_file(WatdfsClientState *state, WatdfsOpenFile *file) {
    time_t now = time(nullptr);
    if ((now - file->tc) < state->cache_interval) {
        return 0;
    }

    int ret = rpc_transfer_lock(file->path.c_str(), RW_READ_LOCK);
    if (ret < 0) {
        return ret;
    }

    struct stat server_stat = {};
    ret = rpc_getattr_remote(file->path.c_str(), &server_stat);
    if (ret == 0) {
        struct timespec server_mtime = mtime_from_stat(server_stat);
        if (same_timespec(file->t_client, server_mtime)) {
            file->t_server = server_mtime;
            file->tc = now;
        } else {
            ret = copy_server_file_to_cache_locked(file, server_stat);
        }
    }

    int unlock_ret = rpc_transfer_unlock(file->path.c_str(), RW_READ_LOCK);
    if (ret == 0 && unlock_ret < 0) {
        ret = unlock_ret;
    }
    return ret;
}

int maybe_writeback_file(WatdfsClientState *state, WatdfsOpenFile *file) {
    if (!file->write_mode) {
        return 0;
    }

    struct stat local_stat = {};
    int ret = stat_local_path(file->cache_path, &local_stat);
    if (ret < 0) {
        return ret;
    }
    file->t_client = mtime_from_stat(local_stat);

    time_t now = time(nullptr);
    if ((now - file->tc) < state->cache_interval) {
        return 0;
    }

    struct stat server_stat = {};
    ret = rpc_getattr_remote(file->path.c_str(), &server_stat);
    if (ret < 0) {
        return ret;
    }
    file->t_server = mtime_from_stat(server_stat);
    if (same_timespec(file->t_client, file->t_server)) {
        file->tc = now;
        return 0;
    }

    return upload_cache_to_server(file);
}

int close_local_and_remote(WatdfsOpenFile *file) {
    int ret = 0;

    if (file->local_fd >= 0) {
        if (close(file->local_fd) < 0) {
            ret = -errno;
        }
        file->local_fd = -1;
    }

    int rpc_ret = rpc_release_remote(file->path.c_str(), &file->server_fi);
    if (ret == 0 && rpc_ret < 0) {
        ret = rpc_ret;
    }

    return ret;
}

int prepare_open_file(WatdfsClientState *state, const char *path, int flags,
                      WatdfsOpenFile *file, bool use_application_flags) {
    file->path = path;
    file->cache_path = cache_path_for(state, path);
    file->original_flags = flags;
    file->write_mode = is_write_mode(flags);
    memset(&file->server_fi, 0, sizeof(file->server_fi));
    file->server_fi.flags = server_transfer_flags(flags);

    int ret = rpc_open_remote(path, &file->server_fi);
    if (ret < 0) {
        return ret;
    }

    ret = download_server_copy(file);
    if (ret < 0) {
        int release_ret = rpc_release_remote(path, &file->server_fi);
        if (release_ret < 0) {
            ret = release_ret;
        }
        return ret;
    }

    int local_flags = use_application_flags
                          ? application_open_flags(flags)
                          : (file->write_mode ? O_RDWR : O_RDONLY);
    file->local_fd = open(file->cache_path.c_str(), local_flags, 0666);
    if (file->local_fd < 0) {
        ret = -errno;
        int release_ret = rpc_release_remote(path, &file->server_fi);
        if (release_ret < 0) {
            ret = release_ret;
        }
        return ret;
    }

    return 0;
}

int perform_temp_writeback_op(WatdfsClientState *state, const char *path,
                              int flags,
                              int (*local_op)(WatdfsOpenFile *, void *),
                              void *op_arg) {
    WatdfsOpenFile temp_file;
    int ret = prepare_open_file(state, path, flags, &temp_file, false);
    if (ret < 0) {
        return ret;
    }

    ret = local_op(&temp_file, op_arg);
    if (ret == 0) {
        ret = upload_cache_to_server(&temp_file);
    }

    int close_ret = close_local_and_remote(&temp_file);
    if (ret == 0 && close_ret < 0) {
        ret = close_ret;
    }
    return ret;
}

int local_truncate_file(WatdfsOpenFile *file, void *arg) {
    off_t newsize = *reinterpret_cast<off_t *>(arg);
    if (truncate(file->cache_path.c_str(), newsize) < 0) {
        return -errno;
    }
    struct stat local_stat = {};
    int ret = stat_local_path(file->cache_path, &local_stat);
    if (ret < 0) {
        return ret;
    }
    file->t_client = mtime_from_stat(local_stat);
    return 0;
}

int local_utimensat_file(WatdfsOpenFile *file, void *arg) {
    const struct timespec *ts = reinterpret_cast<const struct timespec *>(arg);
    if (utimensat(AT_FDCWD, file->cache_path.c_str(), ts, 0) < 0) {
        return -errno;
    }
    struct stat local_stat = {};
    int ret = stat_local_path(file->cache_path, &local_stat);
    if (ret < 0) {
        return ret;
    }
    file->t_client = mtime_from_stat(local_stat);
    return 0;
}

} // namespace

// SETUP AND TEARDOWN
void *watdfs_cli_init(struct fuse_conn_info *conn, const char *path_to_cache,
                      time_t cache_interval, int *ret_code) {
    (void)conn;

    int rc = rpcClientInit();
    if (rc < 0) {
        if (ret_code) {
            *ret_code = rc;
        }
        return nullptr;
    }

    auto *state = new (std::nothrow) WatdfsClientState();
    if (state == nullptr) {
        if (ret_code) {
            *ret_code = -ENOMEM;
        }
        rpcClientDestroy();
        return nullptr;
    }

    state->cache_dir = path_to_cache;
    state->cache_interval = cache_interval;

    if (mkdir(state->cache_dir.c_str(), 0777) < 0 && errno != EEXIST) {
        if (ret_code) {
            *ret_code = -errno;
        }
        delete state;
        rpcClientDestroy();
        return nullptr;
    }

    if (ret_code) {
        *ret_code = 0;
    }
    return state;
}

void watdfs_cli_destroy(void *userdata) {
    auto *state = reinterpret_cast<WatdfsClientState *>(userdata);
    if (state != nullptr) {
        std::vector<std::shared_ptr<WatdfsOpenFile>> open_files;
        {
            std::lock_guard<std::mutex> guard(state->mutex);
            for (const auto &entry : state->open_files) {
                open_files.push_back(entry.second);
            }
            state->open_files.clear();
        }

        for (const auto &file : open_files) {
            if (file->write_mode) {
                upload_cache_to_server(file.get());
            }
            if (file->local_fd >= 0) {
                close(file->local_fd);
                file->local_fd = -1;
            }
            rpc_release_remote(file->path.c_str(), &file->server_fi);
        }

        delete state;
    }

    rpcClientDestroy();
}

// GET FILE ATTRIBUTES
int watdfs_cli_getattr(void *userdata, const char *path, struct stat *statbuf) {
    auto *state = reinterpret_cast<WatdfsClientState *>(userdata);

    if (is_root_path(path)) {
        return rpc_getattr_remote(path, statbuf);
    }

    auto open_file = find_open_file(state, path);
    if (open_file != nullptr) {
        if (!open_file->write_mode) {
            int ret = refresh_readonly_file(state, open_file.get());
            if (ret < 0) {
                memset(statbuf, 0, sizeof(struct stat));
                return ret;
            }
        }

        int ret = stat_local_path(open_file->cache_path, statbuf);
        if (ret < 0) {
            memset(statbuf, 0, sizeof(struct stat));
        }
        return ret;
    }

    struct stat server_stat = {};
    int ret = rpc_getattr_remote(path, &server_stat);
    if (ret < 0) {
        memset(statbuf, 0, sizeof(struct stat));
        return ret;
    }
    if (S_ISDIR(server_stat.st_mode)) {
        *statbuf = server_stat;
        return 0;
    }

    WatdfsOpenFile temp_file;
    ret = prepare_open_file(state, path, O_RDONLY, &temp_file, false);
    if (ret < 0) {
        memset(statbuf, 0, sizeof(struct stat));
        return ret;
    }

    ret = stat_local_path(temp_file.cache_path, statbuf);
    if (ret < 0) {
        memset(statbuf, 0, sizeof(struct stat));
    }

    int close_ret = close_local_and_remote(&temp_file);
    if (ret == 0 && close_ret < 0) {
        ret = close_ret;
    }

    return ret;
}

// CREATE, OPEN AND CLOSE
int watdfs_cli_mknod(void *userdata, const char *path, mode_t mode, dev_t dev) {
    auto *state = reinterpret_cast<WatdfsClientState *>(userdata);
    auto open_file = find_open_file(state, path);
    if (open_file != nullptr) {
        return -EMFILE;
    }

    std::string cache_path = cache_path_for(state, path);
    int ret = ensure_parent_dirs(cache_path);
    if (ret < 0) {
        return ret;
    }

    if (mknod(cache_path.c_str(), mode, dev) < 0) {
        return -errno;
    }

    ret = rpc_mknod_remote(path, mode, dev);
    if (ret < 0) {
        unlink(cache_path.c_str());
        return ret;
    }

    return 0;
}

int watdfs_cli_open(void *userdata, const char *path,
                    struct fuse_file_info *fi) {
    auto *state = reinterpret_cast<WatdfsClientState *>(userdata);

    auto open_file = std::make_shared<WatdfsOpenFile>();
    open_file->path = path;

    int ret = add_open_file(state, open_file);
    if (ret < 0) {
        return ret;
    }

    ret = prepare_open_file(state, path, fi->flags, open_file.get(), true);
    if (ret < 0) {
        remove_open_file(state, path);
        return ret;
    }

    fi->fh = reinterpret_cast<uint64_t>(open_file.get());
    return 0;
}

int watdfs_cli_release(void *userdata, const char *path,
                       struct fuse_file_info *fi) {
    auto *state = reinterpret_cast<WatdfsClientState *>(userdata);
    WatdfsOpenFile *open_file = file_from_fi(fi);
    if (open_file == nullptr) {
        return -EBADF;
    }

    int ret = 0;
    if (open_file->write_mode) {
        ret = upload_cache_to_server(open_file);
    }

    int close_ret = close_local_and_remote(open_file);
    if (ret == 0 && close_ret < 0) {
        ret = close_ret;
    }

    remove_open_file(state, path);
    fi->fh = 0;
    return ret;
}

// READ AND WRITE DATA
int watdfs_cli_read(void *userdata, const char *path, char *buf, size_t size,
                    off_t offset, struct fuse_file_info *fi) {
    auto *state = reinterpret_cast<WatdfsClientState *>(userdata);
    (void)path;

    WatdfsOpenFile *open_file = file_from_fi(fi);
    if (open_file == nullptr) {
        return -EBADF;
    }

    if (!open_file->write_mode) {
        int ret = refresh_readonly_file(state, open_file);
        if (ret < 0) {
            return ret;
        }
    }

    ssize_t bytes = pread(open_file->local_fd, buf, size, offset);
    if (bytes < 0) {
        return -errno;
    }
    return static_cast<int>(bytes);
}

int watdfs_cli_write(void *userdata, const char *path, const char *buf,
                     size_t size, off_t offset, struct fuse_file_info *fi) {
    auto *state = reinterpret_cast<WatdfsClientState *>(userdata);
    (void)path;

    WatdfsOpenFile *open_file = file_from_fi(fi);
    if (open_file == nullptr) {
        return -EBADF;
    }

    ssize_t bytes = pwrite(open_file->local_fd, buf, size, offset);
    if (bytes < 0) {
        return -errno;
    }

    int ret = maybe_writeback_file(state, open_file);
    if (ret < 0) {
        return ret;
    }

    return static_cast<int>(bytes);
}

int watdfs_cli_truncate(void *userdata, const char *path, off_t newsize) {
    auto *state = reinterpret_cast<WatdfsClientState *>(userdata);
    auto open_file = find_open_file(state, path);

    if (open_file != nullptr) {
        if (!open_file->write_mode) {
            return -EMFILE;
        }

        if (truncate(open_file->cache_path.c_str(), newsize) < 0) {
            return -errno;
        }

        return maybe_writeback_file(state, open_file.get());
    }

    return perform_temp_writeback_op(state, path, O_RDWR, local_truncate_file,
                                     &newsize);
}

int watdfs_cli_fsync(void *userdata, const char *path,
                     struct fuse_file_info *fi) {
    (void)userdata;
    (void)path;

    WatdfsOpenFile *open_file = file_from_fi(fi);
    if (open_file == nullptr) {
        return -EBADF;
    }
    if (!open_file->write_mode) {
        return -EMFILE;
    }

    return upload_cache_to_server(open_file);
}

// CHANGE METADATA
int watdfs_cli_utimensat(void *userdata, const char *path,
                         const struct timespec ts[2]) {
    auto *state = reinterpret_cast<WatdfsClientState *>(userdata);
    auto open_file = find_open_file(state, path);

    if (open_file != nullptr) {
        if (!open_file->write_mode) {
            return -EMFILE;
        }

        if (utimensat(AT_FDCWD, open_file->cache_path.c_str(), ts, 0) < 0) {
            return -errno;
        }

        return maybe_writeback_file(state, open_file.get());
    }

    struct timespec ts_copy[2] = {ts[0], ts[1]};
    return perform_temp_writeback_op(state, path, O_RDWR, local_utimensat_file,
                                     ts_copy);
}
