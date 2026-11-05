//
// Starter code for CS 454/654
// You SHOULD change this file
//

#include "watdfs_client.h"
#include "debug.h"
INIT_LOG

#include "rpc.h"

// SETUP AND TEARDOWN
void *watdfs_cli_init(struct fuse_conn_info *conn, const char *path_to_cache,
                      time_t cache_interval, int *ret_code) {
    // TODO: set up the RPC library by calling `rpcClientInit`.
    int rc = rpcClientInit();

    // TODO: check the return code of the `rpcClientInit` call
    // `rpcClientInit` may fail, for example, if an incorrect port was exported.
    if (rc < 0) {
        if (ret_code) *ret_code = rc;
        return nullptr;
    }

    // It may be useful to print to stderr or stdout during debugging.
    // Important: Make sure you turn off logging prior to submission!
    // One useful technique is to use pre-processor flags like:
    // # ifdef PRINT_ERR
    // std::cerr << "Failed to initialize RPC Client" << std::endl;
    // #endif
    // Tip: Try using a macro for the above to minimize the debugging code.

    // TODO Initialize any global state that you require for the assignment and return it.
    // The value that you return here will be passed as userdata in other functions.
    // In A1, you might not need it, so you can return `nullptr`.
    void *userdata = nullptr;

    // TODO: save `path_to_cache` and `cache_interval` (for A3).

    // TODO: set `ret_code` to 0 if everything above succeeded else some appropriate
    // non-zero value.
    if (ret_code) {
        *ret_code = 0;
    }

    // Return pointer to global state data.
    return userdata;
}

void watdfs_cli_destroy(void *userdata) {
    // TODO: clean up your userdata state.
    (void)userdata;
    // TODO: tear down the RPC library by calling `rpcClientDestroy`.
    rpcClientDestroy();
}

// GET FILE ATTRIBUTES
int watdfs_cli_getattr(void *userdata, const char *path, struct stat *statbuf) {
    // getattr has 3 arguments.
    int ARG_COUNT = 3;

    // Allocate space for the output arguments.
    void **args = new void*[ARG_COUNT];

    // Allocate the space for arg types, and one extra space for the null
    // array element.
    int arg_types[ARG_COUNT + 1];

    // The path has string length (strlen) + 1 (for the null character).
    int pathlen = strlen(path) + 1;

    // Fill in the arguments
    // The first argument is the path, it is an input only argument, and a char
    // array. The length of the array is the length of the path.
    arg_types[0] =
        (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (uint) pathlen;
    // For arrays the argument is the array pointer, not a pointer to a pointer.
    args[0] = (void *)path;

    // The second argument is the stat structure. This argument is an output
    // only argument, and we treat it as a char array. The length of the array
    // is the size of the stat structure, which we can determine with sizeof.
    arg_types[1] = (1u << ARG_OUTPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) |
                   (uint) sizeof(struct stat); // statbuf
    args[1] = (void *)statbuf;

    // The third argument is the return code, an output only argument, which is
    // an integer.
    // TODO: fill in this argument type.
    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);

    // The return code is not an array, so we need to hand args[2] an int*.
    // The int* could be the address of an integer located on the stack, or use
    // a heap allocated integer, in which case it should be freed.
    // TODO: Fill in the argument
    int ret_code = 0; 
    args[2] = (void *)&ret_code;

    // Finally, the last position of the arg types is 0. There is no
    // corresponding arg.
    arg_types[3] = 0;

    // MAKE THE RPC CALL
    int rpc_ret = rpcCall((char *)"getattr", arg_types, args);

    // HANDLE THE RETURN
    // The integer value watdfs_cli_getattr will return.
    int fxn_ret = 0;
    if (rpc_ret < 0) {
        // DLOG("getattr rpc failed with error '%d'", rpc_ret);
        // Something went wrong with the rpcCall, return a sensible return
        // value. In this case lets return, -EINVAL
        fxn_ret = -EINVAL;
    } else {
        // Our RPC call succeeded. However, it's possible that the return code
        // from the server is not 0, that is it may be -errno. Therefore, we
        // should set our function return value to the retcode from the server.

        // TODO: set the function return value to the return code from the server.
        fxn_ret = ret_code;
    }

    if (fxn_ret < 0) {
        // If the return code of watdfs_cli_getattr is negative (an error), then 
        // we need to make sure that the stat structure is filled with 0s. Otherwise,
        // FUSE will be confused by the contradicting return values.
        memset(statbuf, 0, sizeof(struct stat));
    }

    // Clean up the memory we have allocated.
    delete []args;

    // Finally return the value we got from the server.
    return fxn_ret;
}

// CREATE, OPEN AND CLOSE
int watdfs_cli_mknod(void *userdata, const char *path, mode_t mode, dev_t dev) {
    // Called to create a file.
    (void)userdata;

    const int ARG_COUNT = 4;
    void **args = new void*[ARG_COUNT];
    int arg_types[ARG_COUNT + 1];

    int pathlen = strlen(path) + 1;

    // path
    arg_types[0] = (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (uint)pathlen;
    args[0] = (void *)path;

    // mode
    arg_types[1] = (1u << ARG_INPUT) | (ARG_INT << 16u);
    int mode_i = (int)mode;
    args[1] = (void *)&mode_i;

    // dev
    arg_types[2] = (1u << ARG_INPUT) | (ARG_LONG << 16u);
    long dev_l = (long)dev;
    args[2] = (void *)&dev_l;

    // retcode
    arg_types[3] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    int retcode = 0;
    args[3] = (void *)&retcode;

    arg_types[4] = 0;

    int rpc_ret = rpcCall((char *)"mknod", arg_types, args);

    int fxn_ret;
    if (rpc_ret < 0) fxn_ret = -EINVAL;
    else fxn_ret = retcode;

    delete [] args;
    return fxn_ret;
}

int watdfs_cli_open(void *userdata, const char *path,
                    struct fuse_file_info *fi) {
    // Called during open.
    // You should fill in fi->fh.
    (void)userdata;

    const int ARG_COUNT = 3;
    void **args = new void*[ARG_COUNT];
    int arg_types[ARG_COUNT + 1];

    int pathlen = strlen(path) + 1;

    // path
    arg_types[0] = (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (uint)pathlen;
    args[0] = (void *)path;

    // fi
    arg_types[1] = (1u << ARG_INPUT) | (1u << ARG_OUTPUT) | (1u << ARG_ARRAY) |
                   (ARG_CHAR << 16u) | (uint)sizeof(struct fuse_file_info);
    args[1] = (void *)fi;

    // retcode
    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    int retcode = 0;
    args[2] = (void *)&retcode;

    arg_types[3] = 0;

    int rpc_ret = rpcCall((char *)"open", arg_types, args);

    int fxn_ret;
    if (rpc_ret < 0) fxn_ret = -EINVAL;
    else fxn_ret = retcode;

    delete [] args;
    return fxn_ret;
}

int watdfs_cli_release(void *userdata, const char *path,
                       struct fuse_file_info *fi) {
    // Called during close, but possibly asynchronously.
    (void)userdata;

    const int ARG_COUNT = 3;
    void **args = new void*[ARG_COUNT];
    int arg_types[ARG_COUNT + 1];

    int pathlen = strlen(path) + 1;

    // path
    arg_types[0] = (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (uint)pathlen;
    args[0] = (void *)path;

    // fi
    arg_types[1] = (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (uint)sizeof(struct fuse_file_info);
    args[1] = (void *)fi;

    // retcode
    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    int retcode = 0;
    args[2] = (void *)&retcode;

    arg_types[3] = 0;

    int rpc_ret = rpcCall((char *)"release", arg_types, args);

    int fxn_ret;
    if (rpc_ret < 0) fxn_ret = -EINVAL;
    else fxn_ret = retcode;

    delete [] args;
    return fxn_ret;
}

// READ AND WRITE DATA
int watdfs_cli_read(void *userdata, const char *path, char *buf, size_t size,
                    off_t offset, struct fuse_file_info *fi) {
    // Read size amount of data at offset of file into buf.

    // Remember that size may be greater than the maximum array size of the RPC
    // library.
    (void)userdata;

    size_t total = 0;

    while (total < size) {
        size_t chunk = size - total;
        if (chunk > (size_t)MAX_ARRAY_LEN) chunk = (size_t)MAX_ARRAY_LEN;

        const int ARG_COUNT = 6;
        void **args = new void*[ARG_COUNT];
        int arg_types[ARG_COUNT + 1];

        int pathlen = (int)strlen(path) + 1;

        // path
        arg_types[0] = (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (uint)pathlen;
        args[0] = (void*)path;

        // buf
        arg_types[1] = (1u << ARG_OUTPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (uint)chunk;
        args[1] = (void*)(buf + total);

        // size
        long size_l = (long)chunk;
        arg_types[2] = (1u << ARG_INPUT) | (ARG_LONG << 16u);
        args[2] = (void*)&size_l;

        // offset
        long off_l = (long)(offset + (off_t)total);
        arg_types[3] = (1u << ARG_INPUT) | (ARG_LONG << 16u);
        args[3] = (void*)&off_l;

        // fi
        arg_types[4] = (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (uint)sizeof(struct fuse_file_info);
        args[4] = (void*)fi;

        // retcode (bytes read)
        int retcode = 0;
        arg_types[5] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
        args[5] = (void*)&retcode;

        arg_types[6] = 0;

        int rpc_ret = rpcCall((char*)"read", arg_types, args);
        delete [] args;

        if (rpc_ret < 0) return -EINVAL;
        if (retcode < 0) return retcode;

        // retcode (bytes read)
        total += (size_t)retcode;

        if ((size_t)retcode < chunk) break;
    }

    return (int)total;
}
int watdfs_cli_write(void *userdata, const char *path, const char *buf,
                     size_t size, off_t offset, struct fuse_file_info *fi) {
    // Write size amount of data at offset of file from buf.

    // Remember that size may be greater than the maximum array size of the RPC
    // library.
    (void)userdata;

    size_t total = 0;

    while (total < size) {
        size_t chunk = size - total;
        if (chunk > (size_t)MAX_ARRAY_LEN) chunk = (size_t)MAX_ARRAY_LEN;

        const int ARG_COUNT = 6;
        void **args = new void*[ARG_COUNT];
        int arg_types[ARG_COUNT + 1];

        int pathlen = (int)strlen(path) + 1;

        // path
        arg_types[0] = (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (uint)pathlen;
        args[0] = (void*)path;

        // buf
        arg_types[1] = (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (uint)chunk;
        args[1] = (void*)(buf + total);

        // size
        long size_l = (long)chunk;
        arg_types[2] = (1u << ARG_INPUT) | (ARG_LONG << 16u);
        args[2] = (void*)&size_l;

        // offset
        long off_l = (long)(offset + (off_t)total);
        arg_types[3] = (1u << ARG_INPUT) | (ARG_LONG << 16u);
        args[3] = (void*)&off_l;

        // fi
        arg_types[4] = (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (uint)sizeof(struct fuse_file_info);
        args[4] = (void*)fi;

        // retcode
        int retcode = 0;
        arg_types[5] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
        args[5] = (void*)&retcode;

        arg_types[6] = 0;

        int rpc_ret = rpcCall((char*)"write", arg_types, args);
        delete [] args;

        if (rpc_ret < 0) return -EINVAL;
        if (retcode < 0) return retcode;

        total += (size_t)retcode;

        if ((size_t)retcode < chunk) break;
    }

    return (int)total;
}
int watdfs_cli_truncate(void *userdata, const char *path, off_t newsize) {
    // Change the file size to newsize.
    (void)userdata;

    const int ARG_COUNT = 3;
    void **args = new void*[ARG_COUNT];
    int arg_types[ARG_COUNT + 1];

    int pathlen = (int)strlen(path) + 1;

    arg_types[0] = (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (uint)pathlen;
    args[0] = (void*)path;

    long newsize_l = (long)newsize;
    arg_types[1] = (1u << ARG_INPUT) | (ARG_LONG << 16u);
    args[1] = (void*)&newsize_l;

    int retcode = 0;
    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    args[2] = (void*)&retcode;

    arg_types[3] = 0;

    int rpc_ret = rpcCall((char*)"truncate", arg_types, args);
    delete [] args;

    if (rpc_ret < 0) return -EINVAL;
    return retcode;
}

int watdfs_cli_fsync(void *userdata, const char *path,
                     struct fuse_file_info *fi) {
    // Force a flush of file data.
    (void)userdata;

    const int ARG_COUNT = 3;
    void **args = new void *[ARG_COUNT];
    int arg_types[ARG_COUNT + 1];

    int pathlen = (int)strlen(path) + 1;

    arg_types[0] = (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (uint)pathlen;
    args[0] = (void *)path;

    arg_types[1] = (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (uint)sizeof(struct fuse_file_info);
    args[1] = (void *)fi;

    int retcode = 0;
    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    args[2] = (void *)&retcode;

    arg_types[3] = 0;

    int rpc_ret = rpcCall((char *)"fsync", arg_types, args);
    delete[] args;

    if (rpc_ret < 0) return -EINVAL;
    return retcode;
    
}

// CHANGE METADATA
int watdfs_cli_utimensat(void *userdata, const char *path,
                       const struct timespec ts[2]) {
    // Change file access and modification times.
    (void)userdata;

    const int ARG_COUNT = 3;
    void **args = new void *[ARG_COUNT];
    int arg_types[ARG_COUNT + 1];

    int pathlen = (int)strlen(path) + 1;

    arg_types[0] = (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) | (uint)pathlen;
    args[0] = (void *)path;

    arg_types[1] = (1u << ARG_INPUT) | (1u << ARG_ARRAY) | (ARG_CHAR << 16u) |
                   (uint)(sizeof(struct timespec) * 2);
    args[1] = (void *)ts;

    int retcode = 0;
    arg_types[2] = (1u << ARG_OUTPUT) | (ARG_INT << 16u);
    args[2] = (void *)&retcode;

    arg_types[3] = 0;

    int rpc_ret = rpcCall((char *)"utimensat", arg_types, args);
    delete[] args;

    if (rpc_ret < 0) return -EINVAL;
    return retcode;
}
