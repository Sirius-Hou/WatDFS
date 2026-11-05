//
// WatDFS server implementation for CS 454/654 Project 2.
//

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "watdfs_client.h"
#include "debug.h"
INIT_LOG

#include "rpc.h"
#include "rw_lock.h"

#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

struct OpenState {
    int readers = 0;
    int writers = 0;
};

struct TransferLockEntry {
    rw_lock_t lock = RW_LOCK_INITIALIZER;
    bool initialized = true;
};

struct ServerState {
    std::string root_dir;

    std::mutex open_mutex;
    std::map<std::string, OpenState> open_files;

    std::mutex transfer_mutex;
    std::map<std::string, std::shared_ptr<TransferLockEntry>> transfer_locks;
};

ServerState g_state;

std::string server_path(const char *path) {
    if (path == nullptr) {
        return g_state.root_dir;
    }
    if (strcmp(path, "/") == 0) {
        return g_state.root_dir;
    }
    if (path[0] == '/') {
        return g_state.root_dir + path;
    }
    return g_state.root_dir + "/" + path;
}

bool is_write_open(int flags) {
    return (flags & O_ACCMODE) != O_RDONLY;
}

void release_open_tracking_locked(const std::string &path, bool write_mode) {
    auto it = g_state.open_files.find(path);
    if (it == g_state.open_files.end()) {
        return;
    }

    if (write_mode) {
        if (it->second.writers > 0) {
            --it->second.writers;
        }
    } else {
        if (it->second.readers > 0) {
            --it->second.readers;
        }
    }

    if (it->second.readers == 0 && it->second.writers == 0) {
        g_state.open_files.erase(it);
    }
}

int acquire_open_tracking(const std::string &path, bool write_mode) {
    std::lock_guard<std::mutex> guard(g_state.open_mutex);
    OpenState &state = g_state.open_files[path];

    if (write_mode && state.writers > 0) {
        return -EACCES;
    }

    if (write_mode) {
        ++state.writers;
    } else {
        ++state.readers;
    }

    return 0;
}

void release_open_tracking(const std::string &path, bool write_mode) {
    std::lock_guard<std::mutex> guard(g_state.open_mutex);
    release_open_tracking_locked(path, write_mode);
}

std::shared_ptr<TransferLockEntry> get_transfer_lock(const std::string &path) {
    std::lock_guard<std::mutex> guard(g_state.transfer_mutex);
    auto it = g_state.transfer_locks.find(path);
    if (it != g_state.transfer_locks.end()) {
        return it->second;
    }

    auto entry = std::make_shared<TransferLockEntry>();
    g_state.transfer_locks[path] = entry;
    return entry;
}

int skel_getattr(int *, void **args) {
    auto *path = static_cast<char *>(args[0]);
    auto *statbuf = static_cast<struct stat *>(args[1]);
    auto *retcode = static_cast<int *>(args[2]);

    *retcode = 0;
    if (stat(server_path(path).c_str(), statbuf) < 0) {
        *retcode = -errno;
        std::memset(statbuf, 0, sizeof(struct stat));
    }
    return 0;
}

int skel_mknod(int *, void **args) {
    auto *path = static_cast<char *>(args[0]);
    int mode = *static_cast<int *>(args[1]);
    dev_t dev = static_cast<dev_t>(*static_cast<long *>(args[2]));
    auto *retcode = static_cast<int *>(args[3]);

    *retcode = 0;
    if (mknod(server_path(path).c_str(), static_cast<mode_t>(mode), dev) < 0) {
        *retcode = -errno;
    }
    return 0;
}

int skel_open(int *, void **args) {
    auto *path = static_cast<char *>(args[0]);
    auto *fi = static_cast<struct fuse_file_info *>(args[1]);
    auto *retcode = static_cast<int *>(args[2]);

    std::string key(path);
    bool write_mode = is_write_open(fi->flags);

    *retcode = acquire_open_tracking(key, write_mode);
    if (*retcode < 0) {
        return 0;
    }

    int fd = open(server_path(path).c_str(), fi->flags, 0666);
    if (fd < 0) {
        *retcode = -errno;
        release_open_tracking(key, write_mode);
        return 0;
    }

    fi->fh = static_cast<uint64_t>(fd);
    *retcode = 0;
    return 0;
}

int skel_release(int *, void **args) {
    auto *path = static_cast<char *>(args[0]);
    auto *fi = static_cast<struct fuse_file_info *>(args[1]);
    auto *retcode = static_cast<int *>(args[2]);

    std::string key(path);
    bool write_mode = is_write_open(fi->flags);

    *retcode = 0;
    int fd = static_cast<int>(fi->fh);
    if (fd >= 0 && close(fd) < 0) {
        *retcode = -errno;
    }

    release_open_tracking(key, write_mode);
    fi->fh = 0;
    return 0;
}

int skel_read(int *, void **args) {
    auto *buf = static_cast<char *>(args[1]);
    size_t size = static_cast<size_t>(*static_cast<long *>(args[2]));
    off_t offset = static_cast<off_t>(*static_cast<long *>(args[3]));
    auto *fi = static_cast<struct fuse_file_info *>(args[4]);
    auto *retcode = static_cast<int *>(args[5]);

    ssize_t bytes = pread(static_cast<int>(fi->fh), buf, size, offset);
    *retcode = (bytes < 0) ? -errno : static_cast<int>(bytes);
    return 0;
}

int skel_write(int *, void **args) {
    auto *buf = static_cast<char *>(args[1]);
    size_t size = static_cast<size_t>(*static_cast<long *>(args[2]));
    off_t offset = static_cast<off_t>(*static_cast<long *>(args[3]));
    auto *fi = static_cast<struct fuse_file_info *>(args[4]);
    auto *retcode = static_cast<int *>(args[5]);

    ssize_t bytes = pwrite(static_cast<int>(fi->fh), buf, size, offset);
    *retcode = (bytes < 0) ? -errno : static_cast<int>(bytes);
    return 0;
}

int skel_truncate(int *, void **args) {
    auto *path = static_cast<char *>(args[0]);
    off_t newsize = static_cast<off_t>(*static_cast<long *>(args[1]));
    auto *retcode = static_cast<int *>(args[2]);

    *retcode = 0;
    if (truncate(server_path(path).c_str(), newsize) < 0) {
        *retcode = -errno;
    }
    return 0;
}

int skel_fsync(int *, void **args) {
    auto *fi = static_cast<struct fuse_file_info *>(args[1]);
    auto *retcode = static_cast<int *>(args[2]);

    *retcode = 0;
    if (fsync(static_cast<int>(fi->fh)) < 0) {
        *retcode = -errno;
    }
    return 0;
}

int skel_utimensat(int *, void **args) {
    auto *path = static_cast<char *>(args[0]);
    auto *ts = static_cast<struct timespec *>(args[1]);
    auto *retcode = static_cast<int *>(args[2]);

    *retcode = 0;
    if (utimensat(AT_FDCWD, server_path(path).c_str(), ts, 0) < 0) {
        *retcode = -errno;
    }
    return 0;
}

int skel_lock(int *, void **args) {
    auto *path = static_cast<char *>(args[0]);
    auto mode = static_cast<rw_lock_mode_t>(*static_cast<int *>(args[1]));
    auto *retcode = static_cast<int *>(args[2]);

    auto entry = get_transfer_lock(std::string(path));
    *retcode = rw_lock_lock(&entry->lock, mode);
    return 0;
}

int skel_unlock(int *, void **args) {
    auto *path = static_cast<char *>(args[0]);
    auto mode = static_cast<rw_lock_mode_t>(*static_cast<int *>(args[1]));
    auto *retcode = static_cast<int *>(args[2]);

    auto entry = get_transfer_lock(std::string(path));
    *retcode = rw_lock_unlock(&entry->lock, mode);
    return 0;
}

int register_rpc(char *name, int *arg_types, skeleton fn) {
    int rc = rpcRegister(name, arg_types, fn);
    if (rc < 0) {
        DLOG("rpcRegister failed for %s with %d", name, rc);
    }
    return rc;
}

int register_all_rpcs() {
    int getattr_types[] = {
        static_cast<int>((1u << ARG_INPUT) | (1u << ARG_ARRAY) |
                         (ARG_CHAR << 16u) | 1u),
        static_cast<int>((1u << ARG_OUTPUT) | (1u << ARG_ARRAY) |
                         (ARG_CHAR << 16u) | 1u),
        static_cast<int>((1u << ARG_OUTPUT) | (ARG_INT << 16u)),
        0};

    int mknod_types[] = {
        static_cast<int>((1u << ARG_INPUT) | (1u << ARG_ARRAY) |
                         (ARG_CHAR << 16u) | 1u),
        static_cast<int>((1u << ARG_INPUT) | (ARG_INT << 16u)),
        static_cast<int>((1u << ARG_INPUT) | (ARG_LONG << 16u)),
        static_cast<int>((1u << ARG_OUTPUT) | (ARG_INT << 16u)),
        0};

    int open_types[] = {
        static_cast<int>((1u << ARG_INPUT) | (1u << ARG_ARRAY) |
                         (ARG_CHAR << 16u) | 1u),
        static_cast<int>((1u << ARG_INPUT) | (1u << ARG_OUTPUT) |
                         (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | 1u),
        static_cast<int>((1u << ARG_OUTPUT) | (ARG_INT << 16u)),
        0};

    int release_types[] = {
        static_cast<int>((1u << ARG_INPUT) | (1u << ARG_ARRAY) |
                         (ARG_CHAR << 16u) | 1u),
        static_cast<int>((1u << ARG_INPUT) | (1u << ARG_ARRAY) |
                         (ARG_CHAR << 16u) | 1u),
        static_cast<int>((1u << ARG_OUTPUT) | (ARG_INT << 16u)),
        0};

    int read_types[] = {
        static_cast<int>((1u << ARG_INPUT) | (1u << ARG_ARRAY) |
                         (ARG_CHAR << 16u) | 1u),
        static_cast<int>((1u << ARG_OUTPUT) | (1u << ARG_ARRAY) |
                         (ARG_CHAR << 16u) | 1u),
        static_cast<int>((1u << ARG_INPUT) | (ARG_LONG << 16u)),
        static_cast<int>((1u << ARG_INPUT) | (ARG_LONG << 16u)),
        static_cast<int>((1u << ARG_INPUT) | (1u << ARG_ARRAY) |
                         (ARG_CHAR << 16u) | 1u),
        static_cast<int>((1u << ARG_OUTPUT) | (ARG_INT << 16u)),
        0};

    int write_types[] = {
        static_cast<int>((1u << ARG_INPUT) | (1u << ARG_ARRAY) |
                         (ARG_CHAR << 16u) | 1u),
        static_cast<int>((1u << ARG_INPUT) | (1u << ARG_ARRAY) |
                         (ARG_CHAR << 16u) | 1u),
        static_cast<int>((1u << ARG_INPUT) | (ARG_LONG << 16u)),
        static_cast<int>((1u << ARG_INPUT) | (ARG_LONG << 16u)),
        static_cast<int>((1u << ARG_INPUT) | (1u << ARG_ARRAY) |
                         (ARG_CHAR << 16u) | 1u),
        static_cast<int>((1u << ARG_OUTPUT) | (ARG_INT << 16u)),
        0};

    int truncate_types[] = {
        static_cast<int>((1u << ARG_INPUT) | (1u << ARG_ARRAY) |
                         (ARG_CHAR << 16u) | 1u),
        static_cast<int>((1u << ARG_INPUT) | (ARG_LONG << 16u)),
        static_cast<int>((1u << ARG_OUTPUT) | (ARG_INT << 16u)),
        0};

    int fsync_types[] = {
        static_cast<int>((1u << ARG_INPUT) | (1u << ARG_ARRAY) |
                         (ARG_CHAR << 16u) | 1u),
        static_cast<int>((1u << ARG_INPUT) | (1u << ARG_ARRAY) |
                         (ARG_CHAR << 16u) | 1u),
        static_cast<int>((1u << ARG_OUTPUT) | (ARG_INT << 16u)),
        0};

    int utimensat_types[] = {
        static_cast<int>((1u << ARG_INPUT) | (1u << ARG_ARRAY) |
                         (ARG_CHAR << 16u) | 1u),
        static_cast<int>((1u << ARG_INPUT) | (1u << ARG_ARRAY) |
                         (ARG_CHAR << 16u) | 1u),
        static_cast<int>((1u << ARG_OUTPUT) | (ARG_INT << 16u)),
        0};

    int lock_types[] = {
        static_cast<int>((1u << ARG_INPUT) | (1u << ARG_ARRAY) |
                         (ARG_CHAR << 16u) | 1u),
        static_cast<int>((1u << ARG_INPUT) | (ARG_INT << 16u)),
        static_cast<int>((1u << ARG_OUTPUT) | (ARG_INT << 16u)),
        0};

    int unlock_types[] = {
        static_cast<int>((1u << ARG_INPUT) | (1u << ARG_ARRAY) |
                         (ARG_CHAR << 16u) | 1u),
        static_cast<int>((1u << ARG_INPUT) | (ARG_INT << 16u)),
        static_cast<int>((1u << ARG_OUTPUT) | (ARG_INT << 16u)),
        0};

    if (register_rpc(const_cast<char *>("getattr"), getattr_types, skel_getattr) < 0) return -1;
    if (register_rpc(const_cast<char *>("mknod"), mknod_types, skel_mknod) < 0) return -1;
    if (register_rpc(const_cast<char *>("open"), open_types, skel_open) < 0) return -1;
    if (register_rpc(const_cast<char *>("release"), release_types, skel_release) < 0) return -1;
    if (register_rpc(const_cast<char *>("read"), read_types, skel_read) < 0) return -1;
    if (register_rpc(const_cast<char *>("write"), write_types, skel_write) < 0) return -1;
    if (register_rpc(const_cast<char *>("truncate"), truncate_types, skel_truncate) < 0) return -1;
    if (register_rpc(const_cast<char *>("fsync"), fsync_types, skel_fsync) < 0) return -1;
    if (register_rpc(const_cast<char *>("utimensat"), utimensat_types, skel_utimensat) < 0) return -1;
    if (register_rpc(const_cast<char *>("lock"), lock_types, skel_lock) < 0) return -1;
    if (register_rpc(const_cast<char *>("unlock"), unlock_types, skel_unlock) < 0) return -1;

    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <server_root_dir>\n", argv[0]);
        return 1;
    }

    g_state.root_dir = argv[1];

    struct stat st = {};
    if (stat(g_state.root_dir.c_str(), &st) < 0) {
        perror("stat");
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "server root must be a directory\n");
        return 1;
    }

    int rc = rpcServerInit();
    if (rc < 0) {
        fprintf(stderr, "rpcServerInit failed: %d\n", rc);
        return 1;
    }

    rc = register_all_rpcs();
    if (rc < 0) {
        fprintf(stderr, "rpcRegister failed\n");
        return 1;
    }

    rc = rpcExecute();
    if (rc < 0) {
        fprintf(stderr, "rpcExecute failed: %d\n", rc);
        return 1;
    }

    return 0;
}
