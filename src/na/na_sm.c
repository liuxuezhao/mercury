/*
 * Copyright (C) 2013-2019 Argonne National Laboratory, Department of Energy,
 *                    UChicago Argonne, LLC and The HDF Group.
 * All rights reserved.
 *
 * The full copyright notice, including terms governing use, modification,
 * and redistribution, is contained in the COPYING file that can be
 * found at the root of the source code distribution tree.
 */

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#    define _GNU_SOURCE
#endif
#include "na_sm.h"
#include "na_plugin.h"

#include "mercury_event.h"
#include "mercury_hash_table.h"
#include "mercury_list.h"
#include "mercury_mem.h"
#include "mercury_poll.h"
#include "mercury_queue.h"
#include "mercury_thread_rwlock.h"
#include "mercury_thread_spin.h"
#include "mercury_time.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NA_SM_HAS_UUID
#    include <uuid/uuid.h>
#endif

#ifdef _WIN32
#    include <process.h>
#else
#    include <fcntl.h>
#    include <ftw.h>
#    include <pwd.h>
#    include <sys/mman.h>
#    include <sys/socket.h>
#    include <sys/stat.h>
#    include <sys/types.h>
#    include <sys/un.h>
#    include <unistd.h>
#    if defined(NA_SM_HAS_CMA)
#        include <limits.h>
#        include <sys/uio.h>
#    elif defined(__APPLE__)
#        include <mach/mach.h>
#        include <mach/mach_vm.h>
#    endif
#endif

/****************/
/* Local Macros */
/****************/

/* Default cache line size */
#define NA_SM_CACHE_LINE_SIZE HG_MEM_CACHE_LINE_SIZE

/* Default page size */
#define NA_SM_PAGE_SIZE HG_MEM_PAGE_SIZE

/* Default filenames/paths */
#define NA_SM_SHM_PATH  "/dev/shm"
#define NA_SM_SOCK_NAME "/sock"

/* Max filename length used for shared files */
#define NA_SM_MAX_FILENAME 64

/* Max number of shared-memory buffers (reserved by 64-bit atomic integer) */
#define NA_SM_NUM_BUFS 64

/* Size of shared-memory buffer */
#define NA_SM_COPY_BUF_SIZE NA_SM_PAGE_SIZE

/* Max number of fds used for cleanup */
#define NA_SM_CLEANUP_NFDS 16

/* Max number of peers */
#define NA_SM_MAX_PEERS (NA_CONTEXT_ID_MAX + 1)

/* Msg sizes */
#define NA_SM_UNEXPECTED_SIZE NA_SM_COPY_BUF_SIZE
#define NA_SM_EXPECTED_SIZE   NA_SM_UNEXPECTED_SIZE

/* Max tag */
#define NA_SM_MAX_TAG NA_TAG_MAX

/* Max events */
#define NA_SM_MAX_EVENTS 16

/* Op ID status bits */
#define NA_SM_OP_COMPLETED (1 << 0)
#define NA_SM_OP_CANCELED  (1 << 1)
#define NA_SM_OP_QUEUED    (1 << 2)

/* Private data access */
#define NA_SM_CLASS(na_class) ((struct na_sm_class *) (na_class->plugin_class))
#define NA_SM_CONTEXT(context)                                                 \
    ((struct na_sm_context *) (context->plugin_context))

/* Generate SHM file name */
#define NA_SM_GEN_SHM_NAME(filename, maxlen, username, pid, id)                \
    snprintf(                                                                  \
        filename, maxlen, "%s_%s-%d-%u", NA_SM_SHM_PREFIX, username, pid, id)

/* Generate socket path */
#define NA_SM_GEN_SOCK_PATH(pathname, maxlen, username, pid, id)               \
    snprintf(pathname, maxlen, "%s/%s_%s/%d/%u", NA_SM_TMP_DIRECTORY,          \
        NA_SM_SHM_PREFIX, username, pid, id);

#ifndef HG_UTIL_HAS_SYSEVENTFD_H
#    define NA_SM_GEN_FIFO_NAME(                                               \
        filename, maxlen, username, pid, id, index, pair)                      \
        snprintf(filename, maxlen, "%s/%s_%s/%d/%u/fifo-%u-%c",                \
            NA_SM_TMP_DIRECTORY, NA_SM_SHM_PREFIX, username, pid, id, index,   \
            pair)
#endif

/************************************/
/* Local Type and Struct Definition */
/************************************/

/* Msg header */
typedef union {
    struct {
        unsigned int tag : 32;      /* Message tag : UINT MAX */
        unsigned int buf_size : 16; /* Buffer length: 4KB MAX */
        unsigned int buf_idx : 8;   /* Index reserved: 64 MAX */
        unsigned int type : 8;      /* Message type */
    } hdr;
    na_uint64_t val;
} na_sm_msg_hdr_t;

/* Make sure this is cache-line aligned */
typedef union {
    hg_atomic_int64_t val;
    char pad[NA_SM_CACHE_LINE_SIZE];
} na_sm_cacheline_atomic_int64_t;

typedef union {
    hg_atomic_int64_t val[4];
    char pad[NA_SM_CACHE_LINE_SIZE];
} na_sm_cacheline_atomic_int256_t;

/* Msg buffers (page aligned) */
struct na_sm_copy_buf {
    hg_thread_spin_t buf_locks[NA_SM_NUM_BUFS];    /* Locks on buffers */
    char buf[NA_SM_NUM_BUFS][NA_SM_COPY_BUF_SIZE]; /* Array of buffers */
    na_sm_cacheline_atomic_int64_t available;      /* Available bitmask */
};

/* Msg queue (allocate queue's flexible array member statically) */
struct na_sm_msg_queue {
    hg_atomic_int32_t prod_head;
    hg_atomic_int32_t prod_tail;
    unsigned int prod_size;
    unsigned int prod_mask;
    hg_util_uint64_t drops;
    hg_atomic_int32_t cons_head
        __attribute__((aligned(HG_MEM_CACHE_LINE_SIZE)));
    hg_atomic_int32_t cons_tail;
    unsigned int cons_size;
    unsigned int cons_mask;
    hg_atomic_int64_t ring[NA_SM_NUM_BUFS]
        __attribute__((aligned(HG_MEM_CACHE_LINE_SIZE)));
};

/* Shared queue pair */
struct na_sm_queue_pair {
    struct na_sm_msg_queue tx_queue; /* Send queue */
    struct na_sm_msg_queue rx_queue; /* Recv queue */
};

/* Cmd values */
typedef enum { NA_SM_RESERVED = 1, NA_SM_RELEASED } na_sm_cmd_t;

/* Cmd header */
typedef union {
    struct {
        unsigned int pid : 32;     /* PID */
        unsigned int id : 8;       /* ID */
        unsigned int pair_idx : 8; /* Index reserved */
        unsigned int type : 8;     /* Cmd type */
        unsigned int pad : 8;      /* 8 bits left */
    } hdr;
    na_uint64_t val;
} na_sm_cmd_hdr_t;

/* Cmd queue (allocate queue's flexible array member statically) */
struct na_sm_cmd_queue {
    hg_atomic_int32_t prod_head;
    hg_atomic_int32_t prod_tail;
    unsigned int prod_size;
    unsigned int prod_mask;
    hg_util_uint64_t drops;
    hg_atomic_int32_t cons_head
        __attribute__((aligned(HG_MEM_CACHE_LINE_SIZE)));
    hg_atomic_int32_t cons_tail;
    unsigned int cons_size;
    unsigned int cons_mask;
    /* To be safe, make the queue twice as large */
    hg_atomic_int64_t ring[NA_SM_MAX_PEERS * 2]
        __attribute__((aligned(HG_MEM_CACHE_LINE_SIZE)));
};

/* Shared region */
struct na_sm_region {
    struct na_sm_copy_buf copy_bufs; /* Pool of msg buffers */
    struct na_sm_queue_pair queue_pairs[NA_SM_MAX_PEERS]
        __attribute__((aligned(NA_SM_PAGE_SIZE))); /* Msg queue pairs */
    struct na_sm_cmd_queue cmd_queue;              /* Cmd queue */
    na_sm_cacheline_atomic_int256_t available;     /* Available pairs */
};

/* Poll type */
typedef enum na_sm_poll_type {
    NA_SM_POLL_SOCK = 1,
    NA_SM_POLL_RX_NOTIFY,
    NA_SM_POLL_TX_NOTIFY
} na_sm_poll_type_t;

/* Address */
struct na_sm_addr {
    HG_LIST_ENTRY(na_sm_addr) entry;    /* Entry in poll list */
    struct na_sm_region *shared_region; /* Shared-memory region */
    struct na_sm_msg_queue *tx_queue;   /* Pointer to shared tx queue */
    struct na_sm_msg_queue *rx_queue;   /* Pointer to shared rx queue */
    int tx_notify;                      /* Notify fd for tx queue */
    int rx_notify;                      /* Notify fd for rx queue */
    na_sm_poll_type_t tx_poll_type;     /* Tx poll type */
    na_sm_poll_type_t rx_poll_type;     /* Rx poll type */
    hg_atomic_int32_t ref_count;        /* Ref count */
    pid_t pid;                          /* PID */
    na_uint8_t id;                      /* SM ID */
    na_uint8_t queue_pair_idx;          /* Shared queue pair index */
    na_bool_t unexpected;               /* Unexpected address */
};

/* Address list */
struct na_sm_addr_list {
    HG_LIST_HEAD(na_sm_addr) list;
    hg_thread_spin_t lock;
};

/* Map (used to cache addresses) */
struct na_sm_map {
    hg_thread_rwlock_t lock;
    hg_hash_table_t *map;
};

/* Map insert cb args */
struct na_sm_lookup_args {
    struct na_sm_endpoint *endpoint;
    const char *username;
    pid_t pid;
    na_uint8_t id;
};

/* Memory handle */
struct na_sm_mem_handle {
    struct iovec *iov;    /* I/O segments */
    unsigned long iovcnt; /* Segment count */
    size_t len;           /* Size of region */
    na_uint8_t flags;     /* Flag of operation access */
};

/* Msg info */
struct na_sm_msg_info {
    union {
        const void *const_ptr;
        void *ptr;
    } buf;
    size_t buf_size;
    na_size_t actual_buf_size;
    na_tag_t tag;
};

/* Unexpected msg info */
struct na_sm_unexpected_info {
    HG_QUEUE_ENTRY(na_sm_unexpected_info) entry;
    struct na_sm_addr *na_sm_addr;
    void *buf;
    na_size_t buf_size;
    na_tag_t tag;
};

/* Unexpected msg queue */
struct na_sm_unexpected_msg_queue {
    HG_QUEUE_HEAD(na_sm_unexpected_info) queue;
    hg_thread_spin_t lock;
};

/* Operation ID */
struct na_sm_op_id {
    struct na_cb_completion_data completion_data; /* Completion data */
    union {
        struct na_sm_msg_info msg;
    } info;                            /* Op info                  */
    HG_QUEUE_ENTRY(na_sm_op_id) entry; /* Entry in queue           */
    na_class_t *na_class;              /* NA class associated      */
    na_context_t *context;             /* NA context associated    */
    struct na_sm_addr *na_sm_addr;     /* Address associated       */
    hg_atomic_int32_t status;          /* Operation status         */
    hg_atomic_int32_t ref_count;       /* Refcount                 */
};

/* Op ID queue */
struct na_sm_op_queue {
    HG_QUEUE_HEAD(na_sm_op_id) queue;
    hg_thread_spin_t lock;
};

/* Endpoint */
struct na_sm_endpoint {
    struct na_sm_map addr_map; /* Address map */
    struct na_sm_unexpected_msg_queue
        unexpected_msg_queue;                  /* Unexpected msg queue */
    struct na_sm_op_queue unexpected_op_queue; /* Unexpected op queue */
    struct na_sm_op_queue expected_op_queue;   /* Expected op queue */
    struct na_sm_op_queue retry_op_queue;      /* Retry op queue */
    struct na_sm_addr_list poll_addr_list;     /* List of addresses to poll */
    struct na_sm_addr *source_addr;            /* Source addr */
    hg_poll_set_t *poll_set;                   /* Poll set */
    int sock;                                  /* Sock fd */
    na_sm_poll_type_t sock_poll_type;          /* Sock poll type */
    na_bool_t listen;                          /* Listen on sock */
};

/* Private context */
struct na_sm_context {
    struct hg_poll_event events[NA_SM_MAX_EVENTS];
};

/* Private data */
struct na_sm_class {
    struct na_sm_endpoint endpoint; /* Endpoint */
    char *username;                 /* Username */
    na_uint8_t max_contexts;        /* Max number of contexts */
    na_bool_t no_wait;              /* Ignore wait object */
};

/********************/
/* Local Prototypes */
/********************/

/**
 * utility function: wrapper around getlogin().
 * Allows graceful handling of directory name generation.
 */
static char *
getlogin_safe(void);

/**
 * Get value from ptrace_scope.
 */
static int
na_sm_get_ptrace_scope_value(void);

/**
 * Convert errno to NA return values.
 */
static na_return_t
na_sm_errno_to_na(int rc);

/**
 * Map shared-memory object.
 */
static void *
na_sm_shm_map(const char *name, na_size_t length, na_bool_t create);

/**
 * Unmap shared-memory object.
 */
static na_return_t
na_sm_shm_unmap(const char *name, void *addr, na_size_t length);

/**
 * Clean up dangling shm segments.
 */
static int
na_sm_shm_cleanup(
    const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf);

/**
 * Initialize queue.
 */
static void
na_sm_msg_queue_init(struct na_sm_msg_queue *na_sm_queue);

/**
 * Multi-producer enqueue.
 */
static NA_INLINE na_bool_t
na_sm_msg_queue_push(
    struct na_sm_msg_queue *na_sm_queue, na_sm_msg_hdr_t msg_hdr);

/**
 * Multi-consumer dequeue.
 */
static NA_INLINE na_bool_t
na_sm_msg_queue_pop(
    struct na_sm_msg_queue *na_sm_msg_queue, na_sm_msg_hdr_t *msg_hdr_ptr);

/**
 * Check whether queue is empty.
 */
static NA_INLINE na_bool_t
na_sm_msg_queue_is_empty(struct na_sm_msg_queue *na_sm_queue);

/**
 * Initialize queue.
 */
static void
na_sm_cmd_queue_init(struct na_sm_cmd_queue *na_sm_queue);

/**
 * Multi-producer enqueue.
 */
static NA_INLINE na_bool_t
na_sm_cmd_queue_push(
    struct na_sm_cmd_queue *na_sm_queue, na_sm_cmd_hdr_t cmd_hdr);

/**
 * Multi-consumer dequeue.
 */
static NA_INLINE na_bool_t
na_sm_cmd_queue_pop(
    struct na_sm_cmd_queue *na_sm_queue, na_sm_cmd_hdr_t *cmd_hdr_ptr);

/**
 * Check whether queue is empty.
 */
static NA_INLINE na_bool_t
na_sm_cmd_queue_is_empty(struct na_sm_cmd_queue *na_sm_queue);

/**
 * Generate key for addr map.
 */
static NA_INLINE na_uint64_t
na_sm_addr_to_key(pid_t pid, na_uint8_t id);

/**
 * Key hash for hash table.
 */
static NA_INLINE unsigned int
na_sm_addr_key_hash(hg_hash_table_key_t vlocation);

/**
 * Compare key.
 */
static NA_INLINE int
na_sm_addr_key_equal(
    hg_hash_table_key_t vlocation1, hg_hash_table_key_t vlocation2);

/**
 * Get SM address from string.
 */
static na_return_t
na_sm_string_to_addr(const char *str, pid_t *pid, na_uint8_t *id);

/**
 * Open shared-memory region.
 */
static na_return_t
na_sm_region_open(const char *username, pid_t pid, na_uint8_t id,
    na_bool_t create, struct na_sm_region **region);

/**
 * Close shared-memory region.
 */
static na_return_t
na_sm_region_close(const char *username, pid_t pid, na_uint8_t id,
    na_bool_t remove, struct na_sm_region *region);

/**
 * Open UNIX domain socket.
 */
static na_return_t
na_sm_sock_open(const char *username, pid_t pid, na_uint8_t id,
    na_bool_t create, int *sock);

/**
 * Close socket.
 */
static na_return_t
na_sm_sock_close(
    const char *username, pid_t pid, na_uint8_t id, na_bool_t remove, int sock);

/**
 * Create tmp path for UNIX socket.
 */
static na_return_t
na_sm_sock_path_create(const char *pathname);

/**
 * Remove tmp path for UNIX socket.
 */
static na_return_t
na_sm_sock_path_remove(const char *pathname);

/**
 * Clean up tmp paths for UNIX socket.
 */
static int
na_sm_sock_path_cleanup(
    const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf);

/**
 * Create event.
 */
static na_return_t
na_sm_event_create(const char *username, pid_t pid, na_uint8_t id,
    na_uint8_t pair_index, unsigned char pair, int *event);

/**
 * Destroy event.
 */
static na_return_t
na_sm_event_destroy(const char *username, pid_t pid, na_uint8_t id,
    na_uint8_t pair_index, unsigned char pair, na_bool_t remove, int event);

/**
 * Set event.
 */
static NA_INLINE na_return_t
na_sm_event_set(int event);

/**
 * Get event.
 */
static NA_INLINE na_return_t
na_sm_event_get(int event, na_bool_t *signaled);

/**
 * Register addr to poll set.
 */
static na_return_t
na_sm_poll_register(hg_poll_set_t *poll_set, int fd, void *ptr);

/**
 * Deregister addr from poll set.
 */
static na_return_t
na_sm_poll_deregister(hg_poll_set_t *poll_set, int fd);

/**
 * Open shared-memory endpoint.
 */
static na_return_t
na_sm_endpoint_open(struct na_sm_endpoint *na_sm_endpoint, const char *username,
    pid_t pid, na_uint8_t id, na_bool_t listen, na_bool_t no_wait);

/**
 * Close shared-memory endpoint.
 */
static na_return_t
na_sm_endpoint_close(
    struct na_sm_endpoint *na_sm_endpoint, const char *username);

/**
 * Reserve queue pair.
 */
static NA_INLINE na_return_t
na_sm_queue_pair_reserve(struct na_sm_region *na_sm_region, na_uint8_t *index);

/**
 * Release queue pair.
 */
static NA_INLINE void
na_sm_queue_pair_release(struct na_sm_region *na_sm_region, na_uint8_t index);

/**
 * Lookup addr key from map.
 */
static NA_INLINE struct na_sm_addr *
na_sm_addr_map_lookup(struct na_sm_map *na_sm_map, na_uint64_t key);

/**
 * Insert new addr key into map. Execute callback while write lock is acquired.
 */
static na_return_t
na_sm_addr_map_insert(struct na_sm_map *na_sm_map, na_uint64_t key,
    na_return_t (*insert_cb)(void *, struct na_sm_addr **), void *arg,
    struct na_sm_addr **addr);

/**
 * Reserve new queue pair and send event signals to target.
 */
static na_return_t
na_sm_addr_lookup_insert_cb(void *arg, struct na_sm_addr **addr);

/**
 * Create new address.
 */
static na_return_t
na_sm_addr_create(struct na_sm_endpoint *na_sm_endpoint,
    struct na_sm_region *shared_region, pid_t pid, na_uint8_t id,
    na_uint8_t queue_pair_idx, int tx_notify, int rx_notify,
    na_bool_t unexpected, struct na_sm_addr **addr);

/**
 * Destroy address.
 */
static na_return_t
na_sm_addr_destroy(struct na_sm_endpoint *na_sm_endpoint, const char *username,
    struct na_sm_addr *na_sm_addr);

/**
 * Send events as ancillary data.
 */
static na_return_t
na_sm_addr_event_send(int sock, const char *username, pid_t pid, na_uint8_t id,
    na_sm_cmd_hdr_t cmd_hdr, int tx_notify, int rx_notify,
    na_bool_t ignore_error);

/**
 * Recv events as ancillary data.
 */
static na_return_t
na_sm_addr_event_recv(int sock, na_sm_cmd_hdr_t *cmd_hdr, int *tx_notify,
    int *rx_notify, na_bool_t *received);

/**
 * Reserve shared buffer.
 */
static NA_INLINE na_return_t
na_sm_buf_reserve(struct na_sm_copy_buf *na_sm_copy_buf, unsigned int *index);

/**
 * Release shared buffer.
 */
static NA_INLINE void
na_sm_buf_release(struct na_sm_copy_buf *na_sm_copy_buf, unsigned int index);

/**
 * Copy src to shared buffer.
 */
static NA_INLINE void
na_sm_buf_copy_to(struct na_sm_copy_buf *na_sm_copy_buf, unsigned int index,
    const void *src, size_t n);

/**
 * Copy from shared buffer to dest.
 */
static NA_INLINE void
na_sm_buf_copy_from(struct na_sm_copy_buf *na_sm_copy_buf, unsigned int index,
    void *dest, size_t n);

/**
 * Translate offset from mem_handle into usable iovec.
 */
static void
na_sm_offset_translate(struct na_sm_mem_handle *mem_handle, na_offset_t offset,
    na_size_t length, struct iovec *iov, unsigned long *iovcnt);

/**
 * Progress on endpoint sock.
 */
static na_return_t
na_sm_progress_sock(struct na_sm_endpoint *na_sm_endpoint, const char *username,
    na_bool_t *progressed);

/**
 * Process cmd.
 */
static na_return_t
na_sm_process_cmd(struct na_sm_endpoint *na_sm_endpoint, const char *username,
    na_sm_cmd_hdr_t cmd_hdr, int tx_notify, int rx_notify);

/**
 * Progress on tx notifications.
 */
static na_return_t
na_sm_progress_tx_notify(struct na_sm_addr *poll_addr, na_bool_t *progressed);

/**
 * Progress on rx notifications.
 */
static na_return_t
na_sm_progress_rx_notify(struct na_sm_addr *poll_addr, na_bool_t *progressed);

/**
 * Progress rx queue.
 */
static na_return_t
na_sm_progress_rx_queue(struct na_sm_endpoint *na_sm_endpoint,
    struct na_sm_addr *poll_addr, na_bool_t *progressed);

/**
 * Process unexpected messages.
 */
static na_return_t
na_sm_process_unexpected(struct na_sm_op_queue *unexpected_op_queue,
    struct na_sm_addr *poll_addr, na_sm_msg_hdr_t msg_hdr,
    struct na_sm_unexpected_msg_queue *unexpected_msg_queue);

/**
 * Process expected messages.
 */
static na_return_t
na_sm_process_expected(struct na_sm_op_queue *expected_op_queue,
    struct na_sm_addr *poll_addr, na_sm_msg_hdr_t msg_hdr);

/**
 * Process retries.
 */
static na_return_t
na_sm_process_retries(struct na_sm_op_queue *retry_op_queue);

/**
 * Complete operation.
 */
static na_return_t
na_sm_complete(struct na_sm_op_id *na_sm_op_id, int notify);

/**
 * Release memory.
 */
static NA_INLINE void
na_sm_release(void *arg);

/* check_protocol */
static na_bool_t
na_sm_check_protocol(const char *protocol_name);

/* initialize */
static na_return_t
na_sm_initialize(
    na_class_t *na_class, const struct na_info *na_info, na_bool_t listen);

/* finalize */
static na_return_t
na_sm_finalize(na_class_t *na_class);

/* context_create */
static na_return_t
na_sm_context_create(na_class_t *na_class, void **context, na_uint8_t id);

/* context_destroy */
static na_return_t
na_sm_context_destroy(na_class_t *na_class, void *context);

/* cleanup */
static void
na_sm_cleanup(void);

/* op_create */
static na_op_id_t
na_sm_op_create(na_class_t *na_class);

/* op_destroy */
static na_return_t
na_sm_op_destroy(na_class_t *na_class, na_op_id_t op_id);

/* addr_lookup */
static na_return_t
na_sm_addr_lookup(na_class_t *na_class, const char *name, na_addr_t *addr);

/* addr_free */
static na_return_t
na_sm_addr_free(na_class_t *na_class, na_addr_t addr);

/* addr_self */
static na_return_t
na_sm_addr_self(na_class_t *na_class, na_addr_t *addr);

/* addr_dup */
static na_return_t
na_sm_addr_dup(na_class_t *na_class, na_addr_t addr, na_addr_t *new_addr);

/* addr_cmp */
static na_bool_t
na_sm_addr_cmp(na_class_t *na_class, na_addr_t addr1, na_addr_t addr2);

/* addr_is_self */
static NA_INLINE na_bool_t
na_sm_addr_is_self(na_class_t *na_class, na_addr_t addr);

/* addr_to_string */
static na_return_t
na_sm_addr_to_string(
    na_class_t *na_class, char *buf, na_size_t *buf_size, na_addr_t addr);

/* addr_get_serialize_size */
static NA_INLINE na_size_t
na_sm_addr_get_serialize_size(na_class_t *na_class, na_addr_t addr);

/* addr_serialize */
static na_return_t
na_sm_addr_serialize(
    na_class_t *na_class, void *buf, na_size_t buf_size, na_addr_t addr);

/* addr_deserialize */
static na_return_t
na_sm_addr_deserialize(
    na_class_t *na_class, na_addr_t *addr, const void *buf, na_size_t buf_size);

/* msg_get_max_unexpected_size */
static NA_INLINE na_size_t
na_sm_msg_get_max_unexpected_size(const na_class_t *na_class);

/* msg_get_max_expected_size */
static NA_INLINE na_size_t
na_sm_msg_get_max_expected_size(const na_class_t *na_class);

/* msg_get_max_tag */
static NA_INLINE na_tag_t
na_sm_msg_get_max_tag(const na_class_t *na_class);

/* msg_send_unexpected */
static na_return_t
na_sm_msg_send_unexpected(na_class_t *na_class, na_context_t *context,
    na_cb_t callback, void *arg, const void *buf, na_size_t buf_size,
    void *plugin_data, na_addr_t dest_addr, na_uint8_t dest_id, na_tag_t tag,
    na_op_id_t *op_id);

/* msg_recv_unexpected */
static na_return_t
na_sm_msg_recv_unexpected(na_class_t *na_class, na_context_t *context,
    na_cb_t callback, void *arg, void *buf, na_size_t buf_size,
    void *plugin_data, na_op_id_t *op_id);

/* msg_send_expected */
static na_return_t
na_sm_msg_send_expected(na_class_t *na_class, na_context_t *context,
    na_cb_t callback, void *arg, const void *buf, na_size_t buf_size,
    void *plugin_data, na_addr_t dest_addr, na_uint8_t dest_id, na_tag_t tag,
    na_op_id_t *op_id);

/* msg_recv_expected */
static na_return_t
na_sm_msg_recv_expected(na_class_t *na_class, na_context_t *context,
    na_cb_t callback, void *arg, void *buf, na_size_t buf_size,
    void *plugin_data, na_addr_t source_addr, na_uint8_t source_id,
    na_tag_t tag, na_op_id_t *op_id);

/* mem_handle_create */
static na_return_t
na_sm_mem_handle_create(na_class_t *na_class, void *buf, na_size_t buf_size,
    unsigned long flags, na_mem_handle_t *mem_handle);

#ifdef NA_SM_HAS_CMA
/* mem_handle_create_segments */
static na_return_t
na_sm_mem_handle_create_segments(na_class_t *na_class,
    struct na_segment *segments, na_size_t segment_count, unsigned long flags,
    na_mem_handle_t *mem_handle);
#endif

/* mem_handle_free */
static na_return_t
na_sm_mem_handle_free(na_class_t *na_class, na_mem_handle_t mem_handle);

/* mem_handle_get_serialize_size */
static NA_INLINE na_size_t
na_sm_mem_handle_get_serialize_size(
    na_class_t *na_class, na_mem_handle_t mem_handle);

/* mem_handle_serialize */
static na_return_t
na_sm_mem_handle_serialize(na_class_t *na_class, void *buf, na_size_t buf_size,
    na_mem_handle_t mem_handle);

/* mem_handle_deserialize */
static na_return_t
na_sm_mem_handle_deserialize(na_class_t *na_class, na_mem_handle_t *mem_handle,
    const void *buf, na_size_t buf_size);

/* put */
static na_return_t
na_sm_put(na_class_t *na_class, na_context_t *context, na_cb_t callback,
    void *arg, na_mem_handle_t local_mem_handle, na_offset_t local_offset,
    na_mem_handle_t remote_mem_handle, na_offset_t remote_offset,
    na_size_t length, na_addr_t remote_addr, na_uint8_t remote_id,
    na_op_id_t *op_id);

/* get */
static na_return_t
na_sm_get(na_class_t *na_class, na_context_t *context, na_cb_t callback,
    void *arg, na_mem_handle_t local_mem_handle, na_offset_t local_offset,
    na_mem_handle_t remote_mem_handle, na_offset_t remote_offset,
    na_size_t length, na_addr_t remote_addr, na_uint8_t remote_id,
    na_op_id_t *op_id);

/* poll_get_fd */
static NA_INLINE int
na_sm_poll_get_fd(na_class_t *na_class, na_context_t *context);

/* poll_try_wait */
static NA_INLINE na_bool_t
na_sm_poll_try_wait(na_class_t *na_class, na_context_t *context);

/* progress */
static na_return_t
na_sm_progress(
    na_class_t *na_class, na_context_t *context, unsigned int timeout);

/* cancel */
static na_return_t
na_sm_cancel(na_class_t *na_class, na_context_t *context, na_op_id_t op_id);

/*******************/
/* Local Variables */
/*******************/

const struct na_class_ops NA_PLUGIN_OPS(sm) = {
    "na",                              /* name */
    na_sm_check_protocol,              /* check_protocol */
    na_sm_initialize,                  /* initialize */
    na_sm_finalize,                    /* finalize */
    na_sm_cleanup,                     /* cleanup */
    na_sm_context_create,              /* context_create */
    na_sm_context_destroy,             /* context_destroy */
    na_sm_op_create,                   /* op_create */
    na_sm_op_destroy,                  /* op_destroy */
    na_sm_addr_lookup,                 /* addr_lookup */
    na_sm_addr_free,                   /* addr_free */
    NULL,                              /* addr_set_remove */
    na_sm_addr_self,                   /* addr_self */
    na_sm_addr_dup,                    /* addr_dup */
    na_sm_addr_cmp,                    /* addr_cmp */
    na_sm_addr_is_self,                /* addr_is_self */
    na_sm_addr_to_string,              /* addr_to_string */
    na_sm_addr_get_serialize_size,     /* addr_get_serialize_size */
    na_sm_addr_serialize,              /* addr_serialize */
    na_sm_addr_deserialize,            /* addr_deserialize */
    na_sm_msg_get_max_unexpected_size, /* msg_get_max_unexpected_size */
    na_sm_msg_get_max_expected_size,   /* msg_get_max_expected_size */
    NULL,                              /* msg_get_unexpected_header_size */
    NULL,                              /* msg_get_expected_header_size */
    na_sm_msg_get_max_tag,             /* msg_get_max_tag */
    NULL,                              /* msg_buf_alloc */
    NULL,                              /* msg_buf_free */
    NULL,                              /* msg_init_unexpected */
    na_sm_msg_send_unexpected,         /* msg_send_unexpected */
    na_sm_msg_recv_unexpected,         /* msg_recv_unexpected */
    NULL,                              /* msg_init_expected */
    na_sm_msg_send_expected,           /* msg_send_expected */
    na_sm_msg_recv_expected,           /* msg_recv_expected */
    na_sm_mem_handle_create,           /* mem_handle_create */
#ifdef NA_SM_HAS_CMA
    na_sm_mem_handle_create_segments, /* mem_handle_create_segments */
#else
    NULL, /* mem_handle_create_segments */
#endif
    na_sm_mem_handle_free,               /* mem_handle_free */
    NULL,                                /* mem_register */
    NULL,                                /* mem_deregister */
    NULL,                                /* mem_publish */
    NULL,                                /* mem_unpublish */
    na_sm_mem_handle_get_serialize_size, /* mem_handle_get_serialize_size */
    na_sm_mem_handle_serialize,          /* mem_handle_serialize */
    na_sm_mem_handle_deserialize,        /* mem_handle_deserialize */
    na_sm_put,                           /* put */
    na_sm_get,                           /* get */
    na_sm_poll_get_fd,                   /* poll_get_fd */
    na_sm_poll_try_wait,                 /* poll_try_wait */
    na_sm_progress,                      /* progress */
    na_sm_cancel                         /* cancel */
};

/********************/
/* Plugin callbacks */
/********************/

/* Debug information */
#ifdef NA_HAS_DEBUG
static char *
lltoa(hg_util_uint64_t val, char *string, int radix)
{
    int i = sizeof(val) * 8;

    for (; val && i; --i, val /= (hg_util_uint64_t) radix)
        string[i - 1] = "0123456789abcdef"[val % (hg_util_uint64_t) radix];

    return &string[i];
}
#endif

/*---------------------------------------------------------------------------*/
na_return_t
NA_SM_Host_id_get(na_sm_id_t *id)
{
#ifdef NA_SM_HAS_UUID
    char uuid_str[NA_SM_HOST_ID_LEN + 1];
    FILE *uuid_config = NULL;
    uuid_t new_uuid;
    char pathname[NA_SM_MAX_FILENAME] = {'\0'};
    char *username = getlogin_safe();
    na_return_t ret = NA_SUCCESS;
    int rc;

    rc = snprintf(pathname, NA_SM_MAX_FILENAME, "%s/%s_%s/uuid.cfg",
        NA_SM_TMP_DIRECTORY, NA_SM_SHM_PREFIX, username);
    NA_CHECK_ERROR(rc < 0 || rc > NA_SM_MAX_FILENAME, done, ret, NA_OVERFLOW,
        "snprintf() failed, rc: %d", rc);

    uuid_config = fopen(pathname, "r");
    if (!uuid_config) {
        /* Generate a new one */
        uuid_generate(new_uuid);

        uuid_config = fopen(pathname, "w");
        NA_CHECK_ERROR(uuid_config == NULL, done, ret, na_sm_errno_to_na(errno),
            "Could not open %s for write (%s)", pathname, strerror(errno));
        uuid_unparse(new_uuid, uuid_str);
        fprintf(uuid_config, "%s\n", uuid_str);
    } else {
        /* Get the existing one */
        fgets(uuid_str, NA_SM_HOST_ID_LEN + 1, uuid_config);
        uuid_parse(uuid_str, new_uuid);
    }
    fclose(uuid_config);
    uuid_copy(*id, new_uuid);

done:
    return ret;
#else
    *id = gethostid();

    return NA_SUCCESS;
#endif
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_SM_Host_id_to_string(na_sm_id_t id, char *string)
{
#ifdef NA_SM_HAS_UUID
    uuid_unparse(id, string);

    return NA_SUCCESS;
#else
    na_return_t ret = NA_SUCCESS;
    int rc = snprintf(string, NA_SM_HOST_ID_LEN + 1, "%ld", id);
    NA_CHECK_ERROR(rc < 0 || rc > NA_SM_HOST_ID_LEN + 1, done, ret, NA_OVERFLOW,
        "snprintf() failed, rc: %d", rc);

done:
    return ret;
#endif
}

/*---------------------------------------------------------------------------*/
na_return_t
NA_SM_String_to_host_id(const char *string, na_sm_id_t *id)
{
#ifdef NA_SM_HAS_UUID
    return (uuid_parse(string, *id) == 0) ? NA_SUCCESS : NA_PROTOCOL_ERROR;
#else
    na_return_t ret = NA_SUCCESS;
    int rc = sscanf(string, "%ld", id);
    NA_CHECK_ERROR(
        rc != 1, done, ret, NA_PROTOCOL_ERROR, "sscanf() failed, rc: %d", rc);

done:
    return ret;
#endif
}

/*---------------------------------------------------------------------------*/
void
NA_SM_Host_id_copy(na_sm_id_t *dst, na_sm_id_t src)
{
#ifdef NA_SM_HAS_UUID
    uuid_copy(*dst, src);
#else
    *dst = src;
#endif
}

/*---------------------------------------------------------------------------*/
na_bool_t
NA_SM_Host_id_cmp(na_sm_id_t id1, na_sm_id_t id2)
{
#ifdef NA_SM_HAS_UUID
    return (uuid_compare(id1, id2) == 0) ? NA_TRUE : NA_FALSE;
#else
    return (id1 == id2);
#endif
}

/*---------------------------------------------------------------------------*/
static char *
getlogin_safe(void)
{
    struct passwd *passwd;

    /* statically allocated */
    passwd = getpwuid(getuid());

    return passwd ? passwd->pw_name : "unknown";
}

/*---------------------------------------------------------------------------*/
static int
na_sm_get_ptrace_scope_value(void)
{
    FILE *file;
    int val = 0, rc;

    /* Try to open ptrace_scope */
    file = fopen("/proc/sys/kernel/yama/ptrace_scope", "r");
    if (file) {
        rc = fscanf(file, "%d", &val);
        NA_CHECK_ERROR_NORET(
            rc != 1, done, "Could not get value from ptrace_scope");

        rc = fclose(file);
        NA_CHECK_ERROR_NORET(
            rc != 0, done, "fclose() failed (%s)", strerror(errno));
    }

done:
    return val;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_errno_to_na(int rc)
{
    na_return_t ret;

    switch (rc) {
        case EPERM:
            ret = NA_PERMISSION;
            break;
        case ENOENT:
            ret = NA_NOENTRY;
            break;
        case EINTR:
            ret = NA_INTERRUPT;
            break;
        case EAGAIN:
            ret = NA_AGAIN;
            break;
        case ENOMEM:
            ret = NA_NOMEM;
            break;
        case EACCES:
            ret = NA_ACCESS;
            break;
        case EFAULT:
            ret = NA_FAULT;
            break;
        case EBUSY:
            ret = NA_BUSY;
            break;
        case EEXIST:
            ret = NA_EXIST;
            break;
        case ENODEV:
            ret = NA_NODEV;
            break;
        case EINVAL:
            ret = NA_INVALID_ARG;
            break;
        case EOVERFLOW:
        case ENAMETOOLONG:
            ret = NA_OVERFLOW;
            break;
        case EMSGSIZE:
            ret = NA_MSGSIZE;
            break;
        case EPROTONOSUPPORT:
            ret = NA_PROTONOSUPPORT;
            break;
        case EOPNOTSUPP:
            ret = NA_OPNOTSUPPORTED;
            break;
        case EADDRINUSE:
            ret = NA_ADDRINUSE;
            break;
        case EADDRNOTAVAIL:
            ret = NA_ADDRNOTAVAIL;
            break;
        case ETIMEDOUT:
            ret = NA_TIMEOUT;
            break;
        case ECANCELED:
            ret = NA_CANCELED;
            break;
        default:
            ret = NA_PROTOCOL_ERROR;
            break;
    }

    return ret;
}

/*---------------------------------------------------------------------------*/
static void *
na_sm_shm_map(const char *name, na_size_t length, na_bool_t create)
{
    na_size_t page_size = (na_size_t) hg_mem_get_page_size();

    /* Check alignment */
    NA_CHECK_WARNING(length / page_size * page_size != length,
        "Not aligned properly, page size=%zu bytes, length=%zu bytes",
        page_size, length);

    return hg_mem_shm_map(name, length, create);
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_shm_unmap(const char *name, void *addr, na_size_t length)
{
    return (hg_mem_shm_unmap(name, addr, length) == HG_UTIL_SUCCESS)
               ? NA_SUCCESS
               : na_sm_errno_to_na(errno);
}

/*---------------------------------------------------------------------------*/
static int
na_sm_shm_cleanup(const char *fpath, const struct stat NA_UNUSED *sb,
    int NA_UNUSED typeflag, struct FTW NA_UNUSED *ftwbuf)
{
    const char *prefix = NA_SM_SHM_PATH "/" NA_SM_SHM_PREFIX "_";
    int ret = 0;

    if (strncmp(fpath, prefix, strlen(prefix)) == 0) {
        const char *shm_name = fpath + strlen(NA_SM_SHM_PATH "/");
        char *username = getlogin_safe();

        if (strncmp(shm_name + strlen(NA_SM_SHM_PREFIX "_"), username,
                strlen(username)) == 0) {
            NA_LOG_DEBUG("shm_unmap() %s", shm_name);
            ret = hg_mem_shm_unmap(shm_name, NULL, 0);
        }
    }

    return ret;
}

/*---------------------------------------------------------------------------*/
static void
na_sm_msg_queue_init(struct na_sm_msg_queue *na_sm_queue)
{
    struct hg_atomic_queue *hg_atomic_queue =
        (struct hg_atomic_queue *) na_sm_queue;
    unsigned int count = NA_SM_NUM_BUFS;

    hg_atomic_queue->prod_size = hg_atomic_queue->cons_size = count;
    hg_atomic_queue->prod_mask = hg_atomic_queue->cons_mask = count - 1;
    hg_atomic_init32(&hg_atomic_queue->prod_head, 0);
    hg_atomic_init32(&hg_atomic_queue->cons_head, 0);
    hg_atomic_init32(&hg_atomic_queue->prod_tail, 0);
    hg_atomic_init32(&hg_atomic_queue->cons_tail, 0);
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_bool_t
na_sm_msg_queue_push(
    struct na_sm_msg_queue *na_sm_queue, na_sm_msg_hdr_t msg_hdr)
{
    int rc = hg_atomic_queue_push(
        (struct hg_atomic_queue *) na_sm_queue, (void *) msg_hdr.val);

    return (likely(rc == HG_UTIL_SUCCESS)) ? NA_TRUE : NA_FALSE;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_bool_t
na_sm_msg_queue_pop(
    struct na_sm_msg_queue *na_sm_queue, na_sm_msg_hdr_t *msg_hdr_ptr)
{
    msg_hdr_ptr->val = (na_uint64_t) hg_atomic_queue_pop_mc(
        (struct hg_atomic_queue *) na_sm_queue);

    return (likely(msg_hdr_ptr->val)) ? NA_TRUE : NA_FALSE;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_bool_t
na_sm_msg_queue_is_empty(struct na_sm_msg_queue *na_sm_queue)
{
    return hg_atomic_queue_is_empty((struct hg_atomic_queue *) na_sm_queue);
}

/*---------------------------------------------------------------------------*/
static void
na_sm_cmd_queue_init(struct na_sm_cmd_queue *na_sm_queue)
{
    struct hg_atomic_queue *hg_atomic_queue =
        (struct hg_atomic_queue *) na_sm_queue;
    unsigned int count = NA_SM_MAX_PEERS * 2;

    hg_atomic_queue->prod_size = hg_atomic_queue->cons_size = count;
    hg_atomic_queue->prod_mask = hg_atomic_queue->cons_mask = count - 1;
    hg_atomic_init32(&hg_atomic_queue->prod_head, 0);
    hg_atomic_init32(&hg_atomic_queue->cons_head, 0);
    hg_atomic_init32(&hg_atomic_queue->prod_tail, 0);
    hg_atomic_init32(&hg_atomic_queue->cons_tail, 0);
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_bool_t
na_sm_cmd_queue_push(
    struct na_sm_cmd_queue *na_sm_queue, na_sm_cmd_hdr_t cmd_hdr)
{
    int rc = hg_atomic_queue_push(
        (struct hg_atomic_queue *) na_sm_queue, (void *) cmd_hdr.val);

    return (likely(rc == HG_UTIL_SUCCESS)) ? NA_TRUE : NA_FALSE;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_bool_t
na_sm_cmd_queue_pop(
    struct na_sm_cmd_queue *na_sm_queue, na_sm_cmd_hdr_t *cmd_hdr_ptr)
{
    cmd_hdr_ptr->val = (na_uint64_t) hg_atomic_queue_pop_mc(
        (struct hg_atomic_queue *) na_sm_queue);

    return (likely(cmd_hdr_ptr->val)) ? NA_TRUE : NA_FALSE;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_bool_t
na_sm_cmd_queue_is_empty(struct na_sm_cmd_queue *na_sm_queue)
{
    return hg_atomic_queue_is_empty((struct hg_atomic_queue *) na_sm_queue);
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_uint64_t
na_sm_addr_to_key(pid_t pid, na_uint8_t id)
{
    return (((na_uint64_t) pid) << 32 | id);
}

/*---------------------------------------------------------------------------*/
static NA_INLINE unsigned int
na_sm_addr_key_hash(hg_hash_table_key_t vlocation)
{
    /* Hashing through PIDs should be sufficient in practice */
    return (unsigned int) (*((na_uint64_t *) vlocation) >> 32);
}

/*---------------------------------------------------------------------------*/
static NA_INLINE int
na_sm_addr_key_equal(
    hg_hash_table_key_t vlocation1, hg_hash_table_key_t vlocation2)
{
    return *((na_uint64_t *) vlocation1) == *((na_uint64_t *) vlocation2);
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_string_to_addr(const char *str, pid_t *pid, na_uint8_t *id)
{
    char *name = NULL, *short_name = NULL;
    na_return_t ret = NA_SUCCESS;

    /**
     * Clean up name, strings can be of the format:
     *   <protocol>://<host string>
     */
    name = strdup(str);
    NA_CHECK_ERROR(
        name == NULL, done, ret, NA_NOMEM, "Could not duplicate string");

    if (strstr(name, ":") != NULL) {
        strtok_r(name, ":", &short_name);
        short_name += 2;
    } else
        short_name = name;

    /* Get PID / ID from name */
    sscanf(short_name, "%d/%" SCNu8, pid, id);

done:
    free(name);

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_region_open(const char *username, pid_t pid, na_uint8_t id,
    na_bool_t create, struct na_sm_region **region)
{
    char shm_name[NA_SM_MAX_FILENAME] = {'\0'};
    struct na_sm_region *na_sm_region = NULL;
    na_return_t ret = NA_SUCCESS;
    int rc;

    /* Generate SHM object name */
    rc = NA_SM_GEN_SHM_NAME(
        shm_name, NA_SM_MAX_FILENAME, username, (int) pid, id);
    NA_CHECK_ERROR(rc < 0 || rc > NA_SM_MAX_FILENAME, done, ret, NA_OVERFLOW,
        "NA_SM_GEN_SHM_NAME() failed, rc: %d", rc);

    /* Open SHM object */
    NA_LOG_DEBUG("shm_map() %s", shm_name);
    na_sm_region = (struct na_sm_region *) na_sm_shm_map(
        shm_name, sizeof(struct na_sm_region), create);
    NA_CHECK_ERROR(na_sm_region == NULL, done, ret, NA_NODEV,
        "Could not map new SM region (%s)", shm_name);

    if (create) {
        int i;

        /* Initialize copy buf (all buffers are available by default) */
        hg_atomic_init64(
            &na_sm_region->copy_bufs.available.val, ~((hg_util_int64_t) 0));
        memset(&na_sm_region->copy_bufs.buf, 0,
            sizeof(na_sm_region->copy_bufs.buf));

        /* Initialize locks */
        for (i = 0; i < NA_SM_NUM_BUFS; i++)
            hg_thread_spin_init(&na_sm_region->copy_bufs.buf_locks[i]);

        /* Initialize queue pairs */
        for (i = 0; i < 4; i++)
            hg_atomic_init64(
                &na_sm_region->available.val[i], ~((hg_util_int64_t) 0));

        for (i = 0; i < NA_SM_MAX_PEERS; i++) {
            na_sm_msg_queue_init(&na_sm_region->queue_pairs[i].rx_queue);
            na_sm_msg_queue_init(&na_sm_region->queue_pairs[i].tx_queue);
        }

        /* Initialize command queue */
        na_sm_cmd_queue_init(&na_sm_region->cmd_queue);
    }

    *region = na_sm_region;

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_region_close(const char *username, pid_t pid, na_uint8_t id,
    na_bool_t remove, struct na_sm_region *region)
{
    char shm_name[NA_SM_MAX_FILENAME] = {'\0'};
    const char *shm_name_ptr = NULL;
    na_return_t ret = NA_SUCCESS;

    if (remove) {
        int rc = NA_SM_GEN_SHM_NAME(
            shm_name, NA_SM_MAX_FILENAME, username, (int) pid, id);
        NA_CHECK_ERROR(rc < 0 || rc > NA_SM_MAX_FILENAME, done, ret,
            NA_OVERFLOW, "NA_SM_GEN_SHM_NAME() failed, rc: %d", rc);
        shm_name_ptr = shm_name;
    }

    NA_LOG_DEBUG("shm_unmap() %s", shm_name_ptr);
    ret = na_sm_shm_unmap(shm_name_ptr, region, sizeof(struct na_sm_region));
    NA_CHECK_NA_ERROR(
        done, ret, "Could not unmap SM region (%s)", shm_name_ptr);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_sock_open(
    const char *username, pid_t pid, na_uint8_t id, na_bool_t create, int *sock)
{
    int socket_type = SOCK_DGRAM, /* reliable with AF_UNIX */
        fd = -1, rc;
    char pathname[NA_SM_MAX_FILENAME] = {'\0'};
    na_bool_t created_sock_path = NA_FALSE;
    na_return_t ret = NA_SUCCESS;

    /* Create a non-blocking socket so that we can poll for incoming connections
     */
#ifdef SOCK_NONBLOCK
    socket_type |= SOCK_NONBLOCK;
#endif
    fd = socket(AF_UNIX, socket_type, 0);
    NA_CHECK_ERROR(fd == -1, error, ret, na_sm_errno_to_na(errno),
        "socket() failed (%s)", strerror(errno));

#ifndef SOCK_NONBLOCK
    rc = fcntl(fd, F_SETFL, O_NONBLOCK);
    NA_CHECK_ERROR(rc == -1, error, ret, na_sm_errno_to_na(errno),
        "fcntl() failed (%s)", strerror(errno));
#endif

    if (create) {
        struct sockaddr_un addr;

        /* Generate named socket path */
        rc = NA_SM_GEN_SOCK_PATH(
            pathname, NA_SM_MAX_FILENAME, username, pid, id);
        NA_CHECK_ERROR(rc < 0 || rc > NA_SM_MAX_FILENAME, error, ret,
            NA_OVERFLOW, "NA_SM_GEN_SOCK_PATH() failed, rc: %d", rc);

        memset(&addr, 0, sizeof(struct sockaddr_un));
        addr.sun_family = AF_UNIX;
        NA_CHECK_ERROR(strlen(pathname) + strlen(NA_SM_SOCK_NAME) >
                           sizeof(addr.sun_path) - 1,
            error, ret, NA_OVERFLOW,
            "Exceeds maximum AF UNIX socket path length");
        strcpy(addr.sun_path, pathname);
        strcat(addr.sun_path, NA_SM_SOCK_NAME);

        /* Create path */
        ret = na_sm_sock_path_create(pathname);
        NA_CHECK_NA_ERROR(
            error, ret, "Could not create socket path (%s)", pathname);
        created_sock_path = NA_TRUE;

        /* Bind and create named socket */
        NA_LOG_DEBUG("bind() %s", addr.sun_path);
        rc = bind(
            fd, (const struct sockaddr *) &addr, (socklen_t) SUN_LEN(&addr));
        NA_CHECK_ERROR(rc == -1, error, ret, na_sm_errno_to_na(errno),
            "bind() failed (%s)", strerror(errno));
    }

    *sock = fd;

    return ret;

error:
    if (fd != -1) {
        rc = close(fd);
        NA_CHECK_ERROR_DONE(rc == -1, "close() failed (%s)", strerror(errno));
    }
    if (created_sock_path) {
        na_return_t err_ret = na_sm_sock_path_remove(pathname);
        NA_CHECK_ERROR_DONE(err_ret != NA_SUCCESS,
            "na_sm_remove_sock_path() failed (%s)", pathname);
    }

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_sock_close(
    const char *username, pid_t pid, na_uint8_t id, na_bool_t remove, int sock)
{
    na_return_t ret = NA_SUCCESS;
    int rc;

    NA_LOG_DEBUG("Closing sock %d", sock);
    rc = close(sock);
    NA_CHECK_ERROR(rc == -1, done, ret, na_sm_errno_to_na(errno),
        "close() failed (%s)", strerror(errno));

    if (remove) {
        char pathname[NA_SM_MAX_FILENAME] = {'\0'};
        struct sockaddr_un addr;

        /* Generate named socket path */
        rc = NA_SM_GEN_SOCK_PATH(
            pathname, NA_SM_MAX_FILENAME, username, pid, id);
        NA_CHECK_ERROR(rc < 0 || rc > NA_SM_MAX_FILENAME, done, ret,
            NA_OVERFLOW, "NA_SM_GEN_SOCK_PATH() failed, rc: %d", rc);

        strcpy(addr.sun_path, pathname);
        strcat(addr.sun_path, NA_SM_SOCK_NAME);

        NA_LOG_DEBUG("unlink() %s", addr.sun_path);
        rc = unlink(addr.sun_path);
        NA_CHECK_ERROR(rc == -1, done, ret, na_sm_errno_to_na(errno),
            "unlink() failed (%s)", strerror(errno));

        ret = na_sm_sock_path_remove(pathname);
        NA_CHECK_NA_ERROR(done, ret, "Could not remove %s path", pathname);
    }

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_sock_path_create(const char *pathname)
{
    char *dup_path = NULL, *path_ptr;
    char stat_path[NA_SM_MAX_FILENAME] = {'\0'};
    na_return_t ret = NA_SUCCESS;

    dup_path = strdup(pathname);
    NA_CHECK_ERROR(
        dup_path == NULL, done, ret, NA_NOMEM, "Could not dup pathname");
    path_ptr = dup_path;

    /* Skip leading '/' */
    if (dup_path[0] == '/') {
        path_ptr++;
        stat_path[0] = '/';
    }

    /* Create path */
    while (path_ptr != NULL) {
        char *current = strtok_r(path_ptr, "/", &path_ptr);
        struct stat sb;

        if (!current)
            break;

        strcat(stat_path, current);
        if (stat(stat_path, &sb) == -1) {
            int rc;
            NA_LOG_DEBUG("mkdir %s", stat_path);
            rc = mkdir(stat_path, 0775);
            NA_CHECK_ERROR(rc == -1 && errno != EEXIST, done, ret,
                na_sm_errno_to_na(errno), "Could not create directory: %s (%s)",
                stat_path, strerror(errno));
        }
        strcat(stat_path, "/");
    }

done:
    free(dup_path);

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_sock_path_remove(const char *pathname)
{
    char dup_path[NA_SM_MAX_FILENAME] = {'\0'};
    char *path_ptr = NULL;
    na_return_t ret = NA_SUCCESS;

    strcpy(dup_path, pathname);

    /* Delete path */
    path_ptr = strrchr(dup_path, '/');
    while (path_ptr) {
        *path_ptr = '\0';
        NA_LOG_DEBUG("rmdir %s", dup_path);
        if (rmdir(dup_path) == -1) {
            /* Silently ignore */
        }
        path_ptr = strrchr(dup_path, '/');
    }

    return ret;
}

/*---------------------------------------------------------------------------*/
static int
na_sm_sock_path_cleanup(const char *fpath, const struct stat NA_UNUSED *sb,
    int NA_UNUSED typeflag, struct FTW NA_UNUSED *ftwbuf)
{
    return remove(fpath);
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_event_create(const char NA_UNUSED *username, pid_t NA_UNUSED pid,
    na_uint8_t NA_UNUSED id, na_uint8_t NA_UNUSED pair_index,
    unsigned char NA_UNUSED pair, int *event)
{
    na_return_t ret = NA_SUCCESS;
    int fd = -1;

#ifdef HG_UTIL_HAS_SYSEVENTFD_H
    fd = hg_event_create();
    NA_CHECK_ERROR(fd == -1, error, ret, na_sm_errno_to_na(errno),
        "hg_event_create() failed");
#else
    char fifo_name[NA_SM_MAX_FILENAME] = {'\0'};
    int rc;

    /**
     * If eventfd is not supported, we need to explicitly use named pipes in
     * this case as kqueue file descriptors cannot be exchanged through
     * ancillary data.
     */
    rc = NA_SM_GEN_FIFO_NAME(
        fifo_name, NA_SM_MAX_FILENAME, username, pid, id, pair_index, pair);
    NA_CHECK_ERROR(rc < 0 || rc > NA_SM_MAX_FILENAME, error, ret, NA_OVERFLOW,
        "NA_SM_GEN_FIFO_NAME() failed, rc: %d", rc);

    /* Create FIFO */
    NA_LOG_DEBUG("mkfifo() %s", fifo_name);
    rc = mkfifo(fifo_name, S_IRUSR | S_IWUSR);
    NA_CHECK_ERROR(rc == -1, error, ret, na_sm_errno_to_na(errno),
        "mkfifo() failed (%s)", strerror(errno));

    /* Open FIFO (RDWR for convenience) */
    fd = open(fifo_name, O_RDWR);
    NA_CHECK_ERROR(fd == -1, error, ret, na_sm_errno_to_na(errno),
        "open() failed (%s)", strerror(errno));

    /* Set FIFO to be non-blocking */
    rc = fcntl(fd, F_SETFL, O_NONBLOCK);
    NA_CHECK_ERROR(rc == -1, error, ret, na_sm_errno_to_na(errno),
        "fcntl() failed (%s)", strerror(errno));
#endif
    NA_LOG_DEBUG("Created event %d", fd);

    *event = fd;

    return ret;

error:
#ifndef HG_UTIL_HAS_SYSEVENTFD_H
    if (fd != -1) {
        rc = close(fd);
        NA_CHECK_ERROR_DONE(rc == -1, "close() failed (%s)", strerror(errno));
    }
#endif

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_event_destroy(const char NA_UNUSED *username, pid_t NA_UNUSED pid,
    na_uint8_t NA_UNUSED id, na_uint8_t NA_UNUSED pair_index,
    unsigned char NA_UNUSED pair, na_bool_t NA_UNUSED remove, int event)
{
    na_return_t ret = NA_SUCCESS;
    int rc;

    NA_LOG_DEBUG("Closing event %d", event);
#ifdef HG_UTIL_HAS_SYSEVENTFD_H
    rc = hg_event_destroy(event);
    NA_CHECK_ERROR(rc == HG_UTIL_FAIL, done, ret, na_sm_errno_to_na(errno),
        "hg_event_destroy() failed");
#else
    rc = close(event);
    NA_CHECK_ERROR(rc == -1, done, ret, na_sm_errno_to_na(errno),
        "close() failed (%s)", strerror(errno));

    if (remove) {
        char fifo_name[NA_SM_MAX_FILENAME] = {'\0'};

        rc = NA_SM_GEN_FIFO_NAME(
            fifo_name, NA_SM_MAX_FILENAME, username, pid, id, pair_index, pair);
        NA_CHECK_ERROR(rc < 0 || rc > NA_SM_MAX_FILENAME, done, ret,
            NA_OVERFLOW, "NA_SM_GEN_FIFO_NAME() failed, rc: %d", rc);

        NA_LOG_DEBUG("unlink() %s", fifo_name);
        rc = unlink(fifo_name);
        NA_CHECK_ERROR(rc == -1, done, ret, na_sm_errno_to_na(errno),
            "unlink() failed (%s)", strerror(errno));
    }
#endif

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_return_t
na_sm_event_set(int event)
{
    na_return_t ret = NA_SUCCESS;
#ifdef HG_UTIL_HAS_SYSEVENTFD_H
    int rc;

    rc = hg_event_set(event);
    NA_CHECK_ERROR(rc != HG_UTIL_SUCCESS, done, ret, na_sm_errno_to_na(errno),
        "hg_event_set() failed");
#else
    uint64_t count = 1;
    ssize_t s;

    s = write(event, &count, sizeof(uint64_t));
    NA_CHECK_ERROR(s != sizeof(uint64_t), done, ret, na_sm_errno_to_na(errno),
        "write() failed (%s)", strerror(errno));
#endif

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_return_t
na_sm_event_get(int event, na_bool_t *signaled)
{
    na_return_t ret = NA_SUCCESS;
#ifdef HG_UTIL_HAS_SYSEVENTFD_H
    int rc;

    rc = hg_event_get(event, (hg_util_bool_t *) signaled);
    NA_CHECK_ERROR(rc != HG_UTIL_SUCCESS, done, ret, na_sm_errno_to_na(errno),
        "hg_event_get() failed");
#else
    uint64_t count = 1;
    ssize_t s;

    s = read(event, &count, sizeof(uint64_t));
    if (s != sizeof(uint64_t)) {
        if (likely(errno == EAGAIN)) {
            *signaled = NA_FALSE;
            goto done;
        } else
            NA_GOTO_ERROR(done, ret, na_sm_errno_to_na(errno),
                "read() failed (%s)", strerror(errno));
    }

    *signaled = NA_TRUE;
#endif

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_poll_register(hg_poll_set_t *poll_set, int fd, void *ptr)
{
    struct hg_poll_event event = {.events = HG_POLLIN, .data.ptr = ptr};
    na_return_t ret = NA_SUCCESS;
    int rc;

    rc = hg_poll_add(poll_set, fd, &event);
    NA_CHECK_ERROR(rc != HG_UTIL_SUCCESS, done, ret, na_sm_errno_to_na(errno),
        "hg_poll_add() failed");

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_poll_deregister(hg_poll_set_t *poll_set, int fd)
{
    na_return_t ret = NA_SUCCESS;
    int rc;

    rc = hg_poll_remove(poll_set, fd);
    NA_CHECK_ERROR(rc != HG_UTIL_SUCCESS, done, ret, na_sm_errno_to_na(errno),
        "hg_poll_remove() failed");

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_endpoint_open(struct na_sm_endpoint *na_sm_endpoint, const char *username,
    pid_t pid, na_uint8_t id, na_bool_t listen, na_bool_t no_wait)
{
    struct na_sm_region *shared_region = NULL;
    na_uint8_t queue_pair_idx = 0;
    na_bool_t queue_pair_reserved = NA_FALSE, sock_registered = NA_FALSE;
    int tx_notify = -1;
    na_return_t ret = NA_SUCCESS, err_ret;

    /* Save listen state */
    na_sm_endpoint->listen = listen;

    /* Initialize queues */
    HG_QUEUE_INIT(&na_sm_endpoint->unexpected_msg_queue.queue);
    hg_thread_spin_init(&na_sm_endpoint->unexpected_msg_queue.lock);

    HG_QUEUE_INIT(&na_sm_endpoint->unexpected_op_queue.queue);
    hg_thread_spin_init(&na_sm_endpoint->unexpected_op_queue.lock);

    HG_QUEUE_INIT(&na_sm_endpoint->expected_op_queue.queue);
    hg_thread_spin_init(&na_sm_endpoint->expected_op_queue.lock);

    HG_QUEUE_INIT(&na_sm_endpoint->retry_op_queue.queue);
    hg_thread_spin_init(&na_sm_endpoint->retry_op_queue.lock);

    /* Initialize poll addr list */
    HG_LIST_INIT(&na_sm_endpoint->poll_addr_list.list);
    hg_thread_spin_init(&na_sm_endpoint->poll_addr_list.lock);

    /* Create addr hash-table */
    na_sm_endpoint->addr_map.map =
        hg_hash_table_new(na_sm_addr_key_hash, na_sm_addr_key_equal);
    NA_CHECK_ERROR(na_sm_endpoint->addr_map.map == NULL, error, ret, NA_NOMEM,
        "hg_hash_table_new() failed");
    hg_hash_table_register_free_functions(
        na_sm_endpoint->addr_map.map, free, free);
    hg_thread_rwlock_init(&na_sm_endpoint->addr_map.lock);

    if (listen) {
        /* If we're listening, create a new shm region */
        ret = na_sm_region_open(username, pid, id, NA_TRUE, &shared_region);
        NA_CHECK_NA_ERROR(error, ret, "Could not open shared-memory region");

        /* Reserve queue pair for loopback */
        ret = na_sm_queue_pair_reserve(shared_region, &queue_pair_idx);
        NA_CHECK_NA_ERROR(error, ret, "Could not reserve queue pair");
        queue_pair_reserved = NA_TRUE;
    }

    if (!no_wait) {
        /* Create poll set to wait for events */
        na_sm_endpoint->poll_set = hg_poll_create();
        NA_CHECK_ERROR(na_sm_endpoint->poll_set == NULL, error, ret,
            na_sm_errno_to_na(errno), "Cannot create poll set");

        /* Create endpoint sock */
        ret = na_sm_sock_open(username, pid, id, listen, &na_sm_endpoint->sock);
        NA_CHECK_NA_ERROR(error, ret, "Could not open sock");

        if (listen) {
            na_sm_endpoint->sock_poll_type = NA_SM_POLL_SOCK;
            NA_LOG_DEBUG(
                "Registering sock %d for polling", na_sm_endpoint->sock);
            /* Add sock to poll set (ony required if we're listening) */
            ret = na_sm_poll_register(na_sm_endpoint->poll_set,
                na_sm_endpoint->sock, &na_sm_endpoint->sock_poll_type);
            NA_CHECK_NA_ERROR(error, ret, "Could not add sock to poll set");
            sock_registered = NA_TRUE;
        }

        /* Create local tx signaling event */
        tx_notify = hg_event_create();
        NA_CHECK_ERROR(tx_notify == -1, error, ret, na_sm_errno_to_na(errno),
            "hg_event_create() failed");
    } else {
        na_sm_endpoint->sock = -1;
    }

    /* Allocate source address */
    ret = na_sm_addr_create(na_sm_endpoint, shared_region, pid, id,
        queue_pair_idx, tx_notify, -1, NA_FALSE, &na_sm_endpoint->source_addr);
    NA_CHECK_NA_ERROR(error, ret, "Could not allocate source address");

    /* Add source tx notify to poll set for local notifications */
    if (!no_wait) {
        na_sm_endpoint->source_addr->tx_poll_type = NA_SM_POLL_TX_NOTIFY;
        NA_LOG_DEBUG("Registering tx notify %d for polling", tx_notify);
        ret = na_sm_poll_register(na_sm_endpoint->poll_set, tx_notify,
            &na_sm_endpoint->source_addr->tx_poll_type);
        NA_CHECK_NA_ERROR(error, ret, "Could not add tx notify to poll set");
    }

    return ret;

error:
    if (na_sm_endpoint->source_addr)
        free(na_sm_endpoint->source_addr);
    if (tx_notify > 0)
        hg_event_destroy(tx_notify);
    if (sock_registered) {
        err_ret = na_sm_poll_deregister(
            na_sm_endpoint->poll_set, na_sm_endpoint->sock);
        NA_CHECK_ERROR_DONE(
            err_ret != NA_SUCCESS, "na_sm_poll_deregister() failed");
    }
    if (na_sm_endpoint->sock > 0) {
        err_ret =
            na_sm_sock_close(username, pid, id, listen, na_sm_endpoint->sock);
        NA_CHECK_ERROR_DONE(err_ret != NA_SUCCESS, "na_sm_sock_close() failed");
    }
    if (na_sm_endpoint->poll_set)
        hg_poll_destroy(na_sm_endpoint->poll_set);
    if (queue_pair_reserved)
        na_sm_queue_pair_release(shared_region, queue_pair_idx);
    if (shared_region)
        na_sm_region_close(username, pid, id, NA_TRUE, shared_region);
    if (na_sm_endpoint->addr_map.map) {
        hg_hash_table_free(na_sm_endpoint->addr_map.map);
        hg_thread_rwlock_destroy(&na_sm_endpoint->addr_map.lock);
    }

    hg_thread_spin_destroy(&na_sm_endpoint->unexpected_msg_queue.lock);
    hg_thread_spin_destroy(&na_sm_endpoint->unexpected_op_queue.lock);
    hg_thread_spin_destroy(&na_sm_endpoint->expected_op_queue.lock);
    hg_thread_spin_destroy(&na_sm_endpoint->retry_op_queue.lock);
    hg_thread_spin_destroy(&na_sm_endpoint->poll_addr_list.lock);

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_endpoint_close(
    struct na_sm_endpoint *na_sm_endpoint, const char *username)
{
    struct na_sm_addr *source_addr = na_sm_endpoint->source_addr;
    na_return_t ret = NA_SUCCESS;
    na_bool_t empty;

    /* Check that poll addr list is empty */
    hg_thread_spin_lock(&na_sm_endpoint->poll_addr_list.lock);
    empty = HG_LIST_IS_EMPTY(&na_sm_endpoint->poll_addr_list.list);
    hg_thread_spin_unlock(&na_sm_endpoint->poll_addr_list.lock);

    if (!empty) {
        struct na_sm_addr *na_sm_addr;

        hg_thread_spin_lock(&na_sm_endpoint->poll_addr_list.lock);
        na_sm_addr = HG_LIST_FIRST(&na_sm_endpoint->poll_addr_list.list);
        while (na_sm_addr) {
            struct na_sm_addr *next = HG_LIST_NEXT(na_sm_addr, entry);
            HG_LIST_REMOVE(na_sm_addr, entry);

            /* Destroy remaining addresses */
            ret = na_sm_addr_destroy(na_sm_endpoint, username, na_sm_addr);
            NA_CHECK_NA_ERROR(done, ret, "Could not remove address");

            na_sm_addr = next;
        }
        /* Sanity check */
        empty = HG_LIST_IS_EMPTY(&na_sm_endpoint->poll_addr_list.list);
        hg_thread_spin_unlock(&na_sm_endpoint->poll_addr_list.lock);
    }
    NA_CHECK_ERROR(empty == NA_FALSE, done, ret, NA_BUSY,
        "Poll addr list should be empty");

    /* Check that unexpected message queue is empty */
    hg_thread_spin_lock(&na_sm_endpoint->unexpected_msg_queue.lock);
    empty = HG_QUEUE_IS_EMPTY(&na_sm_endpoint->unexpected_msg_queue.queue);
    hg_thread_spin_unlock(&na_sm_endpoint->unexpected_msg_queue.lock);
    NA_CHECK_ERROR(empty == NA_FALSE, done, ret, NA_BUSY,
        "Unexpected msg queue should be empty");

    /* Check that unexpected op queue is empty */
    hg_thread_spin_lock(&na_sm_endpoint->unexpected_op_queue.lock);
    empty = HG_QUEUE_IS_EMPTY(&na_sm_endpoint->unexpected_op_queue.queue);
    hg_thread_spin_unlock(&na_sm_endpoint->unexpected_op_queue.lock);
    NA_CHECK_ERROR(empty == NA_FALSE, done, ret, NA_BUSY,
        "Unexpected op queue should be empty");

    /* Check that expected op queue is empty */
    hg_thread_spin_lock(&na_sm_endpoint->expected_op_queue.lock);
    empty = HG_QUEUE_IS_EMPTY(&na_sm_endpoint->expected_op_queue.queue);
    hg_thread_spin_unlock(&na_sm_endpoint->expected_op_queue.lock);
    NA_CHECK_ERROR(empty == NA_FALSE, done, ret, NA_BUSY,
        "Expected op queue should be empty");

    /* Check that retry op queue is empty */
    hg_thread_spin_lock(&na_sm_endpoint->retry_op_queue.lock);
    empty = HG_QUEUE_IS_EMPTY(&na_sm_endpoint->retry_op_queue.queue);
    hg_thread_spin_unlock(&na_sm_endpoint->retry_op_queue.lock);
    NA_CHECK_ERROR(empty == NA_FALSE, done, ret, NA_BUSY,
        "Retry op queue should be empty");

    if (source_addr) {
        if (source_addr->shared_region) {
            na_sm_queue_pair_release(
                source_addr->shared_region, source_addr->queue_pair_idx);

            ret = na_sm_region_close(username, source_addr->pid,
                source_addr->id, NA_TRUE, source_addr->shared_region);
            NA_CHECK_NA_ERROR(done, ret, "na_sm_region_close() failed");
        }
        if (source_addr->tx_notify > 0) {
            int rc;

            ret = na_sm_poll_deregister(
                na_sm_endpoint->poll_set, source_addr->tx_notify);
            NA_CHECK_NA_ERROR(done, ret, "na_sm_poll_deregister() failed");

            rc = hg_event_destroy(source_addr->tx_notify);
            NA_CHECK_ERROR(rc != HG_UTIL_SUCCESS, done, ret,
                na_sm_errno_to_na(errno), "hg_event_destroy() failed");
        }
        if (na_sm_endpoint->sock > 0) {
            if (na_sm_endpoint->listen) {
                ret = na_sm_poll_deregister(
                    na_sm_endpoint->poll_set, na_sm_endpoint->sock);
                NA_CHECK_NA_ERROR(done, ret, "na_sm_poll_deregister() failed");
            }
            ret = na_sm_sock_close(username, source_addr->pid, source_addr->id,
                na_sm_endpoint->listen, na_sm_endpoint->sock);
            NA_CHECK_NA_ERROR(done, ret, "na_sm_sock_close() failed");

            na_sm_endpoint->sock = -1;
        }
        free(source_addr);
        na_sm_endpoint->source_addr = NULL;
    }

    if (na_sm_endpoint->poll_set) {
        int rc = hg_poll_destroy(na_sm_endpoint->poll_set);
        NA_CHECK_ERROR(rc != HG_UTIL_SUCCESS, done, ret,
            na_sm_errno_to_na(errno), "hg_poll_destroy() failed");

        na_sm_endpoint->poll_set = NULL;
    }

    /* Free hash table */
    if (na_sm_endpoint->addr_map.map) {
        hg_hash_table_free(na_sm_endpoint->addr_map.map);
        hg_thread_rwlock_destroy(&na_sm_endpoint->addr_map.lock);
    }

    /* Destroy mutexes */
    hg_thread_spin_destroy(&na_sm_endpoint->unexpected_msg_queue.lock);
    hg_thread_spin_destroy(&na_sm_endpoint->unexpected_op_queue.lock);
    hg_thread_spin_destroy(&na_sm_endpoint->expected_op_queue.lock);
    hg_thread_spin_destroy(&na_sm_endpoint->retry_op_queue.lock);
    hg_thread_spin_destroy(&na_sm_endpoint->poll_addr_list.lock);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_return_t
na_sm_queue_pair_reserve(struct na_sm_region *na_sm_region, na_uint8_t *index)
{
    unsigned int j = 0;

    do {
        hg_util_int64_t bits = 1LL;
        unsigned int i = 0;

        do {
            hg_util_int64_t available =
                hg_atomic_get64(&na_sm_region->available.val[j]);
            if (!available) {
                j++;
                break;
            }

            if ((available & bits) != bits) {
                /* Already reserved */
                hg_atomic_fence();
                i++;
                bits <<= 1;
                continue;
            }

            if (hg_atomic_cas64(&na_sm_region->available.val[j], available,
                    available & ~bits)) {
#ifdef NA_HAS_DEBUG
                char buf[65] = {'\0'};
                available = hg_atomic_get64(&na_sm_region->available.val[j]);
                NA_LOG_DEBUG("Reserved pair index %u\n### Available: %s",
                    (i + (j * 64)),
                    lltoa((hg_util_uint64_t) available, buf, 2));
#endif
                *index = (na_uint8_t)(i + (j * 64));
                return NA_SUCCESS;
            }

            /* Can't use atomic XOR directly, if there is a race and the cas
             * fails, we should be able to pick the next one available */
        } while (i < 64);
    } while (j < 4);

    return NA_AGAIN;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE void
na_sm_queue_pair_release(struct na_sm_region *na_sm_region, na_uint8_t index)
{
    hg_atomic_or64(&na_sm_region->available.val[index / 64], 1LL << index % 64);
    NA_LOG_DEBUG("Released pair index %u", index);
}

/*---------------------------------------------------------------------------*/
static NA_INLINE struct na_sm_addr *
na_sm_addr_map_lookup(struct na_sm_map *na_sm_map, na_uint64_t key)
{
    hg_hash_table_value_t value = NULL;

    /* Lookup key */
    hg_thread_rwlock_rdlock(&na_sm_map->lock);
    value = hg_hash_table_lookup(na_sm_map->map, (hg_hash_table_key_t) &key);
    hg_thread_rwlock_release_rdlock(&na_sm_map->lock);

    return (value == HG_HASH_TABLE_NULL) ? NULL : *(struct na_sm_addr **) value;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_addr_map_insert(struct na_sm_map *na_sm_map, na_uint64_t key,
    na_return_t (*insert_cb)(void *, struct na_sm_addr **), void *arg,
    struct na_sm_addr **addr)
{
    struct na_sm_addr *na_sm_addr = NULL;
    hg_hash_table_value_t value = NULL;
    hg_hash_table_key_t key_ptr = (hg_hash_table_key_t) &key;
    na_return_t ret = NA_SUCCESS;
    na_bool_t inserted = NA_FALSE;
    int rc;

    hg_thread_rwlock_wrlock(&na_sm_map->lock);

    /* Look up again to prevent race between lock release/acquire */
    value = hg_hash_table_lookup(na_sm_map->map, key_ptr);
    if (value != HG_HASH_TABLE_NULL) {
        ret = NA_EXIST; /* Entry already exists */
        goto done;
    }

    /* Allocate new key */
    key_ptr = (hg_hash_table_key_t) malloc(sizeof(na_uint64_t));
    NA_CHECK_ERROR(key_ptr == NULL, error, ret, NA_NOMEM,
        "Cannot allocate memory for new addr key");
    *((na_uint64_t *) key_ptr) = key;

    /* Allocate new value */
    value = (hg_hash_table_value_t) malloc(sizeof(struct na_sm_addr *));
    NA_CHECK_ERROR(value == NULL, error, ret, NA_NOMEM,
        "cannot allocate memory for pointer to address");

    /* Insert new value */
    rc = hg_hash_table_insert(na_sm_map->map, key_ptr, value);
    NA_CHECK_ERROR(
        rc == 0, error, ret, NA_NOMEM, "hg_hash_table_insert() failed");
    inserted = NA_TRUE;

    /* This is a new address, look it up */
    ret = insert_cb(arg, &na_sm_addr);
    NA_CHECK_NA_ERROR(error, ret, "Could not execute insertion callback");

    *((struct na_sm_addr **) value) = na_sm_addr;

done:
    hg_thread_rwlock_release_wrlock(&na_sm_map->lock);

    *addr = *((struct na_sm_addr **) value);

    return ret;

error:
    if (inserted) {
        rc = hg_hash_table_remove(na_sm_map->map, key_ptr);
        NA_CHECK_ERROR_DONE(rc == 0, "Could not remove key");
    } else {
        free(value);
        free(key_ptr);
    }
    hg_thread_rwlock_release_wrlock(&na_sm_map->lock);

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_addr_lookup_insert_cb(void *arg, struct na_sm_addr **addr)
{
    struct na_sm_lookup_args *args = (struct na_sm_lookup_args *) arg;
    struct na_sm_addr *na_sm_addr = NULL;
    na_uint8_t queue_pair_idx = 0;
    na_sm_cmd_hdr_t cmd_hdr = {.val = 0};
    struct na_sm_region *shared_region = NULL;
    na_bool_t queue_pair_reserved = NA_FALSE;
    int tx_notify = -1, rx_notify = -1;
    na_return_t ret = NA_SUCCESS;
    int rc;

    /* Open shm region */
    ret = na_sm_region_open(
        args->username, args->pid, args->id, NA_FALSE, &shared_region);
    NA_CHECK_NA_ERROR(error, ret, "Could not open shared-memory region");

    /* Reserve queue pair */
    ret = na_sm_queue_pair_reserve(shared_region, &queue_pair_idx);
    NA_CHECK_NA_ERROR(error, ret, "Could not reserve queue pair");
    queue_pair_reserved = NA_TRUE;

    /* Fill cmd header */
    cmd_hdr.hdr.type = NA_SM_RESERVED;
    cmd_hdr.hdr.pid = (unsigned int) args->endpoint->source_addr->pid;
    cmd_hdr.hdr.id = args->endpoint->source_addr->id & 0xff;
    cmd_hdr.hdr.pair_idx = queue_pair_idx & 0xff;

    /* Do not create signals if not waiting */
    if (args->endpoint->poll_set) {
        /* Create tx event */
        ret = na_sm_event_create(args->username, args->pid, args->id,
            queue_pair_idx, 't', &tx_notify);
        NA_CHECK_NA_ERROR(error, ret, "Could not create event");

        /* Create rx event */
        ret = na_sm_event_create(args->username, args->pid, args->id,
            queue_pair_idx, 'r', &rx_notify);
        NA_CHECK_NA_ERROR(error, ret, "Could not create event");

        /* Send events to remote process */
        ret = na_sm_addr_event_send(args->endpoint->sock, args->username,
            args->pid, args->id, cmd_hdr, tx_notify, rx_notify, NA_FALSE);
        NA_CHECK_NA_ERROR(error, ret, "Could not send addr events");
    } else {
        NA_LOG_DEBUG("Pushing cmd with %d for %d/%" SCNu8 "/%" SCNu8 " val=%lu",
            cmd_hdr.hdr.type, cmd_hdr.hdr.pid, cmd_hdr.hdr.id,
            cmd_hdr.hdr.pair_idx, cmd_hdr.val);

        /* TODO would be nice to also do that with poll set */
        rc = na_sm_cmd_queue_push(&shared_region->cmd_queue, cmd_hdr);
        NA_CHECK_ERROR(rc == NA_FALSE, error, ret, NA_AGAIN, "Full queue");
    }

    /* Allocate address */
    ret = na_sm_addr_create(args->endpoint, shared_region, args->pid, args->id,
        queue_pair_idx, tx_notify, rx_notify, NA_FALSE, &na_sm_addr);
    NA_CHECK_NA_ERROR(error, ret, "Could not allocate address");

    /* Add address to list of addresses to poll */
    hg_thread_spin_lock(&args->endpoint->poll_addr_list.lock);
    HG_LIST_INSERT_HEAD(
        &args->endpoint->poll_addr_list.list, na_sm_addr, entry);
    hg_thread_spin_unlock(&args->endpoint->poll_addr_list.lock);

    *addr = na_sm_addr;

    return ret;

error:
    if (shared_region) {
        na_return_t err_ret;

        if (queue_pair_reserved) {
            na_sm_queue_pair_release(shared_region, queue_pair_idx);

            if (tx_notify > 0) {
                err_ret = na_sm_event_destroy(args->username, args->pid,
                    args->id, queue_pair_idx, 't', NA_TRUE, tx_notify);
                NA_CHECK_ERROR_DONE(
                    err_ret != NA_SUCCESS, "na_sm_event_destroy() failed");
            }
            if (rx_notify > 0) {
                err_ret = na_sm_event_destroy(args->username, args->pid,
                    args->id, queue_pair_idx, 'r', NA_TRUE, rx_notify);
                NA_CHECK_ERROR_DONE(
                    err_ret != NA_SUCCESS, "na_sm_event_destroy() failed");
            }
        }

        err_ret = na_sm_region_close(
            args->username, args->pid, args->id, NA_FALSE, shared_region);
        NA_CHECK_ERROR_DONE(
            err_ret != NA_SUCCESS, "Could not close shared-memory region");
    }

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_addr_create(struct na_sm_endpoint *na_sm_endpoint,
    struct na_sm_region *shared_region, pid_t pid, na_uint8_t id,
    na_uint8_t queue_pair_idx, int tx_notify, int rx_notify,
    na_bool_t unexpected, struct na_sm_addr **addr)
{
    struct na_sm_addr *na_sm_addr = NULL;
    na_return_t ret = NA_SUCCESS;

    /* Allocate new addr */
    na_sm_addr = (struct na_sm_addr *) malloc(sizeof(struct na_sm_addr));
    NA_CHECK_ERROR(na_sm_addr == NULL, done, ret, NA_NOMEM,
        "Could not allocate NA SM addr");
    memset(na_sm_addr, 0, sizeof(struct na_sm_addr));
    na_sm_addr->unexpected = unexpected;
    hg_atomic_init32(&na_sm_addr->ref_count, 1);

    /* Assign PID/ID */
    na_sm_addr->pid = pid;
    na_sm_addr->id = id;

    /* Assign queue pair index / shared region */
    na_sm_addr->queue_pair_idx = queue_pair_idx;
    na_sm_addr->shared_region = shared_region;

    if (!unexpected) {
        /* Simply assign queues (source address may not have shared region) */
        na_sm_addr->tx_queue =
            shared_region ? &shared_region->queue_pairs[queue_pair_idx].tx_queue
                          : NULL;
        na_sm_addr->rx_queue =
            shared_region ? &shared_region->queue_pairs[queue_pair_idx].rx_queue
                          : NULL;

        /* Simply assign notify descriptors */
        na_sm_addr->tx_notify = tx_notify;
        na_sm_addr->rx_notify = rx_notify;
    } else {
        /* Invert queues so that local rx is remote tx */
        na_sm_addr->tx_queue =
            &shared_region->queue_pairs[queue_pair_idx].rx_queue;
        na_sm_addr->rx_queue =
            &shared_region->queue_pairs[queue_pair_idx].tx_queue;

        /* Invert descriptors so that local rx is remote tx */
        na_sm_addr->tx_notify = rx_notify;
        na_sm_addr->rx_notify = tx_notify;
    }

    if (na_sm_endpoint->poll_set && (na_sm_addr->rx_notify > 0)) {
        na_sm_addr->rx_poll_type = NA_SM_POLL_RX_NOTIFY;
        NA_LOG_DEBUG(
            "Registering rx notify %d for polling", na_sm_addr->rx_notify);
        /* Add remote rx notify to poll set */
        ret = na_sm_poll_register(na_sm_endpoint->poll_set,
            na_sm_addr->rx_notify, &na_sm_addr->rx_poll_type);
        NA_CHECK_NA_ERROR(done, ret, "Could not add rx notify to poll set");
    }

    *addr = na_sm_addr;

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_addr_destroy(struct na_sm_endpoint *na_sm_endpoint, const char *username,
    struct na_sm_addr *na_sm_addr)
{
    na_return_t ret = NA_SUCCESS;

    if (na_sm_addr->unexpected) {
        /* Release queue pair */
        na_sm_queue_pair_release(
            na_sm_addr->shared_region, na_sm_addr->queue_pair_idx);
    } else {
        na_sm_cmd_hdr_t cmd_hdr = {.val = 0};

        /* Fill cmd header */
        cmd_hdr.hdr.type = NA_SM_RELEASED;
        cmd_hdr.hdr.pid = (unsigned int) na_sm_endpoint->source_addr->pid;
        cmd_hdr.hdr.id = na_sm_endpoint->source_addr->id & 0xff;
        cmd_hdr.hdr.pair_idx = na_sm_addr->queue_pair_idx & 0xff;

        /* Do not create signals if not waiting */
        if (na_sm_endpoint->poll_set) {
            /* Send events to remote process (ignore error as this is best
             * effort to clean up resources) */
            ret = na_sm_addr_event_send(na_sm_endpoint->sock, username,
                na_sm_addr->pid, na_sm_addr->id, cmd_hdr, -1, -1, NA_TRUE);
            NA_CHECK_NA_ERROR(done, ret, "Could not send addr events");
        } else {
            na_bool_t rc;

            NA_LOG_DEBUG("Pushing cmd with %d for %d/%" SCNu8 "/%" SCNu8
                         " val=%lu",
                cmd_hdr.hdr.type, cmd_hdr.hdr.pid, cmd_hdr.hdr.id,
                cmd_hdr.hdr.pair_idx, cmd_hdr.val);

            /* TODO would be nice to also do that with poll set */
            rc = na_sm_cmd_queue_push(
                &na_sm_addr->shared_region->cmd_queue, cmd_hdr);
            NA_CHECK_ERROR(rc == NA_FALSE, done, ret, NA_AGAIN, "Full queue");
        }

        /* Close shared-memory region */
        ret = na_sm_region_close(username, na_sm_addr->pid, na_sm_addr->id,
            NA_FALSE, na_sm_addr->shared_region);
        NA_CHECK_NA_ERROR(done, ret, "Could not close shared-memory region");
    }

    if (na_sm_addr->tx_notify > 0) {
        ret = na_sm_event_destroy(username, na_sm_addr->pid, na_sm_addr->id,
            na_sm_addr->queue_pair_idx, 't', !na_sm_addr->unexpected,
            na_sm_addr->tx_notify);
        NA_CHECK_NA_ERROR(done, ret, "na_sm_event_destroy() failed");
    }

    if (na_sm_addr->rx_notify > 0) {
        ret = na_sm_poll_deregister(
            na_sm_endpoint->poll_set, na_sm_addr->rx_notify);
        NA_CHECK_NA_ERROR(done, ret, "na_sm_poll_deregister() failed");

        ret = na_sm_event_destroy(username, na_sm_addr->pid, na_sm_addr->id,
            na_sm_addr->queue_pair_idx, 'r', !na_sm_addr->unexpected,
            na_sm_addr->rx_notify);
        NA_CHECK_NA_ERROR(done, ret, "na_sm_event_destroy() failed");
    }

    free(na_sm_addr);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_addr_event_send(int sock, const char *username, pid_t pid, na_uint8_t id,
    na_sm_cmd_hdr_t cmd_hdr, int tx_notify, int rx_notify,
    na_bool_t ignore_error)
{
    struct sockaddr_un addr;
    struct msghdr msg;
    struct cmsghdr *cmsg;
    /* Contains the file descriptors to pass */
    int fds[2] = {tx_notify, rx_notify};
    union {
        /* ancillary data buffer, wrapped in a union in order to ensure
           it is suitably aligned */
        char buf[CMSG_SPACE(sizeof(fds))];
        struct cmsghdr align;
    } u;
    int *fdptr;
    struct iovec iovec[1];
    ssize_t nsend;
    na_return_t ret = NA_SUCCESS;
    int rc;

    /* Generate named socket path */
    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;

    rc = NA_SM_GEN_SOCK_PATH(
        addr.sun_path, NA_SM_MAX_FILENAME, username, pid, id);
    NA_CHECK_ERROR(rc < 0 || rc > NA_SM_MAX_FILENAME, done, ret, NA_OVERFLOW,
        "NA_SM_GEN_SOCK_PATH() failed, rc: %d", rc);
    strcat(addr.sun_path, NA_SM_SOCK_NAME);

    /* Set address of destination */
    msg.msg_name = &addr;
    msg.msg_namelen = (socklen_t) SUN_LEN(&addr);
    msg.msg_flags = 0; /* unused */

    /* Send cmd */
    iovec[0].iov_base = &cmd_hdr;
    iovec[0].iov_len = sizeof(cmd_hdr);
    msg.msg_iov = iovec;
    msg.msg_iovlen = 1;

    if (tx_notify > 0 && rx_notify > 0) {
        /* Send notify event descriptors as ancillary data */
        msg.msg_control = u.buf;
        msg.msg_controllen = sizeof(u.buf);
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(fds));

        /* Initialize the payload */
        fdptr = (int *) CMSG_DATA(cmsg);
        memcpy(fdptr, fds, sizeof(fds));
    } else {
        msg.msg_control = NULL;
        msg.msg_controllen = 0;
    }

    nsend = sendmsg(sock, &msg, 0);
    if (!ignore_error) {
        NA_CHECK_ERROR(nsend == -1, done, ret, na_sm_errno_to_na(errno),
            "sendmsg() failed (%s)", strerror(errno));
    }

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_addr_event_recv(int sock, na_sm_cmd_hdr_t *cmd_hdr, int *tx_notify,
    int *rx_notify, na_bool_t *received)
{
    struct msghdr msg;
    struct cmsghdr *cmsg;
    int *fdptr;
    int fds[2];
    union {
        /* ancillary data buffer, wrapped in a union in order to ensure
           it is suitably aligned */
        char buf[CMSG_SPACE(sizeof(fds))];
        struct cmsghdr align;
    } u;
    ssize_t nrecv;
    struct iovec iovec[1];
    na_return_t ret = NA_SUCCESS;

    /* Ignore address of source */
    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_flags = 0; /* unused */

    /* Recv reserved queue pair index */
    iovec[0].iov_base = cmd_hdr;
    iovec[0].iov_len = sizeof(*cmd_hdr);
    msg.msg_iov = iovec;
    msg.msg_iovlen = 1;

    /* Recv notify event descriptor as ancillary data */
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof u.buf;

    nrecv = recvmsg(sock, &msg, 0);
    if (nrecv == -1) {
        if (likely(errno == EAGAIN)) {
            *received = NA_FALSE;
            goto done;
        } else
            NA_GOTO_ERROR(done, ret, na_sm_errno_to_na(errno),
                "recvmsg() failed (% s)", strerror(errno));
    }

    *received = NA_TRUE;

    /* Retrieve ancillary data */
    cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg) {
        fdptr = (int *) CMSG_DATA(cmsg);
        memcpy(fds, fdptr, sizeof(fds));

        *tx_notify = fds[0];
        *rx_notify = fds[1];
    } else {
        *tx_notify = -1;
        *rx_notify = -1;
    }

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_return_t
na_sm_buf_reserve(struct na_sm_copy_buf *na_sm_copy_buf, unsigned int *index)
{
    hg_util_int64_t bits = 1LL;
    unsigned int i = 0;

    do {
        hg_util_int64_t available =
            hg_atomic_get64(&na_sm_copy_buf->available.val);
        if (!available) {
            /* Nothing available */
            break;
        }
        if ((available & bits) != bits) {
            /* Already reserved */
            hg_atomic_fence();
            i++;
            bits <<= 1;
            continue;
        }

        if (hg_atomic_cas64(
                &na_sm_copy_buf->available.val, available, available & ~bits)) {
#ifdef NA_HAS_DEBUG
            char buf[65] = {'\0'};
            available = hg_atomic_get64(&na_sm_copy_buf->available.val);
            NA_LOG_DEBUG("Reserved bit index %u\n### Available: %s", i,
                lltoa((hg_util_uint64_t) available, buf, 2));
#endif
            *index = i;
            return NA_SUCCESS;
        }
        /* Can't use atomic XOR directly, if there is a race and the cas
         * fails, we should be able to pick the next one available */
    } while (i < NA_SM_NUM_BUFS);

    return NA_AGAIN;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE void
na_sm_buf_release(struct na_sm_copy_buf *na_sm_copy_buf, unsigned int index)
{
    hg_atomic_or64(&na_sm_copy_buf->available.val, 1LL << index);
    NA_LOG_DEBUG("Released bit index %u", index);
}

/*---------------------------------------------------------------------------*/
static NA_INLINE void
na_sm_buf_copy_to(struct na_sm_copy_buf *na_sm_copy_buf, unsigned int index,
    const void *src, size_t n)
{
    hg_thread_spin_lock(&na_sm_copy_buf->buf_locks[index]);
    memcpy(na_sm_copy_buf->buf[index], src, n);
    hg_thread_spin_unlock(&na_sm_copy_buf->buf_locks[index]);
}

/*---------------------------------------------------------------------------*/
static NA_INLINE void
na_sm_buf_copy_from(struct na_sm_copy_buf *na_sm_copy_buf, unsigned int index,
    void *dest, size_t n)
{
    hg_thread_spin_lock(&na_sm_copy_buf->buf_locks[index]);
    memcpy(dest, na_sm_copy_buf->buf[index], n);
    hg_thread_spin_unlock(&na_sm_copy_buf->buf_locks[index]);
}

/*---------------------------------------------------------------------------*/
static void
na_sm_offset_translate(struct na_sm_mem_handle *mem_handle, na_offset_t offset,
    na_size_t length, struct iovec *iov, unsigned long *iovcnt)
{
    unsigned long i, new_start_index = 0;
    na_offset_t new_offset = offset, next_offset = 0;
    na_size_t remaining_len = length;

    /* Get start index and handle offset */
    for (i = 0; i < mem_handle->iovcnt; i++) {
        next_offset += mem_handle->iov[i].iov_len;
        if (offset < next_offset) {
            new_start_index = i;
            break;
        }
        new_offset -= mem_handle->iov[i].iov_len;
    }

    iov[0].iov_base =
        (char *) mem_handle->iov[new_start_index].iov_base + new_offset;
    iov[0].iov_len = MIN(
        remaining_len, mem_handle->iov[new_start_index].iov_len - new_offset);
    remaining_len -= iov[0].iov_len;

    for (i = 1; remaining_len && (i < mem_handle->iovcnt - new_start_index);
         i++) {
        iov[i].iov_base = mem_handle->iov[i + new_start_index].iov_base;
        /* Can only transfer smallest size */
        iov[i].iov_len =
            MIN(remaining_len, mem_handle->iov[i + new_start_index].iov_len);

        /* Decrease remaining len from the len of data */
        remaining_len -= iov[i].iov_len;
    }

    *iovcnt = i;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_progress_sock(struct na_sm_endpoint *na_sm_endpoint, const char *username,
    na_bool_t *progressed)
{
    na_sm_cmd_hdr_t cmd_hdr = {.val = 0};
    int tx_notify = -1, rx_notify = -1;
    na_return_t ret = NA_SUCCESS;

    /* Attempt to receive addr info (events, queue index) */
    ret = na_sm_addr_event_recv(
        na_sm_endpoint->sock, &cmd_hdr, &tx_notify, &rx_notify, progressed);
    NA_CHECK_NA_ERROR(done, ret, "Could not recv addr events");

    if (*progressed) {
        /* Process received cmd, TODO would be nice to use cmd queue */
        ret = na_sm_process_cmd(
            na_sm_endpoint, username, cmd_hdr, tx_notify, rx_notify);
        NA_CHECK_NA_ERROR(done, ret, "Could not process cmd");
    }

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_process_cmd(struct na_sm_endpoint *na_sm_endpoint, const char *username,
    na_sm_cmd_hdr_t cmd_hdr, int tx_notify, int rx_notify)
{
    na_return_t ret = NA_SUCCESS;

    NA_LOG_DEBUG("Processing cmd with %d from %d/%" SCNu8 "/%" SCNu8 " val=%lu",
        cmd_hdr.hdr.type, cmd_hdr.hdr.pid, cmd_hdr.hdr.id & 0xff,
        cmd_hdr.hdr.pair_idx & 0xff, cmd_hdr.val);

    switch (cmd_hdr.hdr.type) {
        case NA_SM_RESERVED: {
            struct na_sm_addr *na_sm_addr = NULL;

            /* Allocate source address */
            ret = na_sm_addr_create(na_sm_endpoint,
                na_sm_endpoint->source_addr->shared_region,
                (pid_t) cmd_hdr.hdr.pid, cmd_hdr.hdr.id, cmd_hdr.hdr.pair_idx,
                tx_notify, rx_notify, NA_TRUE, &na_sm_addr);
            NA_CHECK_NA_ERROR(
                done, ret, "Could not allocate unexpected address");

            /* Add address to list of addresses to poll */
            hg_thread_spin_lock(&na_sm_endpoint->poll_addr_list.lock);
            HG_LIST_INSERT_HEAD(
                &na_sm_endpoint->poll_addr_list.list, na_sm_addr, entry);
            hg_thread_spin_unlock(&na_sm_endpoint->poll_addr_list.lock);
            break;
        }
        case NA_SM_RELEASED: {
            struct na_sm_addr *na_sm_addr = NULL;
            na_bool_t found = NA_FALSE;

            /* Find address from list of addresses to poll */
            hg_thread_spin_lock(&na_sm_endpoint->poll_addr_list.lock);
            HG_LIST_FOREACH (
                na_sm_addr, &na_sm_endpoint->poll_addr_list.list, entry) {
                if ((na_sm_addr->queue_pair_idx == cmd_hdr.hdr.pair_idx) &&
                    (na_sm_addr->pid == (pid_t) cmd_hdr.hdr.pid) &&
                    (na_sm_addr->id == cmd_hdr.hdr.id)) {
                    found = NA_TRUE;
                    break;
                }
            }
            hg_thread_spin_unlock(&na_sm_endpoint->poll_addr_list.lock);

            if (!found) {
                /* Silently ignore if not found */
                NA_LOG_DEBUG(
                    "Could not find address for PID=%d, ID=%u, pair_index=%u",
                    cmd_hdr.hdr.pid, cmd_hdr.hdr.id, cmd_hdr.hdr.pair_idx);
                break;
            }

            if (hg_atomic_decr32(&na_sm_addr->ref_count))
                /* Cannot free yet */
                break;

            NA_LOG_DEBUG("Freeing addr for PID=%d, ID=%d", na_sm_addr->pid,
                na_sm_addr->id);

            /* Remove address from list of addresses to poll */
            hg_thread_spin_lock(&na_sm_endpoint->poll_addr_list.lock);
            HG_LIST_REMOVE(na_sm_addr, entry);
            hg_thread_spin_unlock(&na_sm_endpoint->poll_addr_list.lock);

            /* Destroy source address */
            ret = na_sm_addr_destroy(na_sm_endpoint, username, na_sm_addr);
            NA_CHECK_NA_ERROR(
                done, ret, "Could not allocate unexpected address");

            break;
        }
        default:
            NA_GOTO_ERROR(
                done, ret, NA_INVALID_ARG, "Unknown type of operation");
    }

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_progress_tx_notify(struct na_sm_addr *poll_addr, na_bool_t *progressed)
{
    na_return_t ret = NA_SUCCESS;
    int rc;

    /* Local notification only */
    rc = hg_event_get(poll_addr->tx_notify, (hg_util_bool_t *) progressed);
    NA_CHECK_ERROR(rc != HG_UTIL_SUCCESS, done, ret, na_sm_errno_to_na(errno),
        "Could not get completion notification");

    NA_LOG_DEBUG("Progressed tx notify %d", poll_addr->tx_notify);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_progress_rx_notify(struct na_sm_addr *poll_addr, na_bool_t *progressed)
{
    na_return_t ret = NA_SUCCESS;

    /* Remote notification only */
    ret = na_sm_event_get(poll_addr->rx_notify, progressed);
    NA_CHECK_NA_ERROR(done, ret, "Could not get completion notification");

    NA_LOG_DEBUG("Progressed rx notify %d", poll_addr->rx_notify);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_progress_rx_queue(struct na_sm_endpoint *na_sm_endpoint,
    struct na_sm_addr *poll_addr, na_bool_t *progressed)
{
    na_sm_msg_hdr_t msg_hdr;
    na_return_t ret = NA_SUCCESS;

    /* Look for message in rx queue */
    if (!na_sm_msg_queue_pop(poll_addr->rx_queue, &msg_hdr)) {
        *progressed = NA_FALSE;
        goto done;
    }

    NA_LOG_DEBUG("Found msg in queue");

    /* Process expected and unexpected messages */
    switch (msg_hdr.hdr.type) {
        case NA_CB_SEND_UNEXPECTED:
            ret = na_sm_process_unexpected(&na_sm_endpoint->unexpected_op_queue,
                poll_addr, msg_hdr, &na_sm_endpoint->unexpected_msg_queue);
            NA_CHECK_NA_ERROR(
                done, ret, "Could not make progress on unexpected msg");
            break;
        case NA_CB_SEND_EXPECTED:
            ret = na_sm_process_expected(
                &na_sm_endpoint->expected_op_queue, poll_addr, msg_hdr);
            NA_CHECK_NA_ERROR(
                done, ret, "Could not make progress on expected msg");
            break;
        default:
            NA_GOTO_ERROR(
                done, ret, NA_INVALID_ARG, "Unknown type of operation");
    }

    *progressed = NA_TRUE;

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_process_unexpected(struct na_sm_op_queue *unexpected_op_queue,
    struct na_sm_addr *poll_addr, na_sm_msg_hdr_t msg_hdr,
    struct na_sm_unexpected_msg_queue *unexpected_msg_queue)
{
    struct na_sm_unexpected_info *na_sm_unexpected_info = NULL;
    struct na_sm_op_id *na_sm_op_id = NULL;
    na_return_t ret = NA_SUCCESS;

    NA_LOG_DEBUG("Processing unexpected msg");

    /* Pop op ID from queue */
    hg_thread_spin_lock(&unexpected_op_queue->lock);
    na_sm_op_id = HG_QUEUE_FIRST(&unexpected_op_queue->queue);
    HG_QUEUE_POP_HEAD(&unexpected_op_queue->queue, entry);
    hg_atomic_and32(&na_sm_op_id->status, ~NA_SM_OP_QUEUED);
    hg_thread_spin_unlock(&unexpected_op_queue->lock);

    if (likely(na_sm_op_id)) {
        /* Fill info */
        na_sm_op_id->na_sm_addr = poll_addr;
        hg_atomic_incr32(&na_sm_op_id->na_sm_addr->ref_count);
        na_sm_op_id->info.msg.actual_buf_size =
            (na_size_t) msg_hdr.hdr.buf_size;
        na_sm_op_id->info.msg.tag = (na_tag_t) msg_hdr.hdr.tag;

        /* Copy buffer */
        na_sm_buf_copy_from(&poll_addr->shared_region->copy_bufs,
            msg_hdr.hdr.buf_idx, na_sm_op_id->info.msg.buf.ptr,
            msg_hdr.hdr.buf_size);

        /* Release buffer */
        na_sm_buf_release(
            &poll_addr->shared_region->copy_bufs, msg_hdr.hdr.buf_idx);

        /* Complete operation (no need to notify) */
        ret = na_sm_complete(na_sm_op_id, 0);
        NA_CHECK_NA_ERROR(done, ret, "Could not complete operation");
    } else {
        /* If no error and message arrived, keep a copy of the struct in
         * the unexpected message queue (should rarely happen) */
        na_sm_unexpected_info = (struct na_sm_unexpected_info *) malloc(
            sizeof(struct na_sm_unexpected_info));
        NA_CHECK_ERROR(na_sm_unexpected_info == NULL, done, ret, NA_NOMEM,
            "Could not allocate unexpected info");

        na_sm_unexpected_info->na_sm_addr = poll_addr;
        na_sm_unexpected_info->buf_size = (na_size_t) msg_hdr.hdr.buf_size;
        na_sm_unexpected_info->tag = (na_tag_t) msg_hdr.hdr.tag;

        /* Allocate buf */
        na_sm_unexpected_info->buf = malloc(na_sm_unexpected_info->buf_size);
        NA_CHECK_ERROR(na_sm_unexpected_info->buf == NULL, error, ret, NA_NOMEM,
            "Could not allocate na_sm_unexpected_info buf");

        /* Copy buffer */
        na_sm_buf_copy_from(&poll_addr->shared_region->copy_bufs,
            msg_hdr.hdr.buf_idx, na_sm_unexpected_info->buf,
            msg_hdr.hdr.buf_size);

        /* Release buffer */
        na_sm_buf_release(
            &poll_addr->shared_region->copy_bufs, msg_hdr.hdr.buf_idx);

        /* Otherwise push the unexpected message into our unexpected queue so
         * that we can treat it later when a recv_unexpected is posted */
        hg_thread_spin_lock(&unexpected_msg_queue->lock);
        HG_QUEUE_PUSH_TAIL(
            &unexpected_msg_queue->queue, na_sm_unexpected_info, entry);
        hg_thread_spin_unlock(&unexpected_msg_queue->lock);
    }

done:
    return ret;

error:
    free(na_sm_unexpected_info);
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_process_expected(struct na_sm_op_queue *expected_op_queue,
    struct na_sm_addr *poll_addr, na_sm_msg_hdr_t msg_hdr)
{
    struct na_sm_op_id *na_sm_op_id = NULL;
    na_return_t ret = NA_SUCCESS;

    NA_LOG_DEBUG("Processing expected msg");

    /* Try to match addr/tag */
    hg_thread_spin_lock(&expected_op_queue->lock);
    HG_QUEUE_FOREACH (na_sm_op_id, &expected_op_queue->queue, entry) {
        if (na_sm_op_id->na_sm_addr == poll_addr &&
            na_sm_op_id->info.msg.tag == msg_hdr.hdr.tag) {
            HG_QUEUE_REMOVE(
                &expected_op_queue->queue, na_sm_op_id, na_sm_op_id, entry);
            hg_atomic_and32(&na_sm_op_id->status, ~NA_SM_OP_QUEUED);
            break;
        }
    }
    hg_thread_spin_unlock(&expected_op_queue->lock);

    NA_CHECK_ERROR(
        na_sm_op_id == NULL, done, ret, NA_INVALID_ARG, "Invalid operation ID");
    /* Cannot have an already completed operation ID, TODO add sanity check */

    na_sm_op_id->info.msg.actual_buf_size = msg_hdr.hdr.buf_size;

    /* Copy buffer */
    na_sm_buf_copy_from(&poll_addr->shared_region->copy_bufs,
        msg_hdr.hdr.buf_idx, na_sm_op_id->info.msg.buf.ptr,
        msg_hdr.hdr.buf_size);

    /* Release buffer */
    na_sm_buf_release(
        &poll_addr->shared_region->copy_bufs, msg_hdr.hdr.buf_idx);

    /* Complete operation */
    ret = na_sm_complete(na_sm_op_id, 0);
    NA_CHECK_NA_ERROR(done, ret, "Could not complete operation");

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_process_retries(struct na_sm_op_queue *retry_op_queue)
{
    struct na_sm_op_id *na_sm_op_id = NULL;
    unsigned int buf_idx;
    na_return_t ret = NA_SUCCESS;

    do {
        na_sm_msg_hdr_t msg_hdr;
        na_bool_t rc;

        hg_thread_spin_lock(&retry_op_queue->lock);
        na_sm_op_id = HG_QUEUE_FIRST(&retry_op_queue->queue);
        hg_thread_spin_unlock(&retry_op_queue->lock);

        if (!na_sm_op_id)
            break;

        NA_LOG_DEBUG("Attempting to retry %p", na_sm_op_id);

        /* Try to reserve buffer atomically */
        if (na_sm_buf_reserve(
                &na_sm_op_id->na_sm_addr->shared_region->copy_bufs, &buf_idx) ==
            NA_AGAIN)
            break;

        /* Successfully reserved a buffer, check that the operation has not
         * been canceled in the meantime so that we can dequeue the op from
         * the queue properly. */
        hg_thread_spin_lock(&retry_op_queue->lock);

        if ((hg_atomic_get32(&na_sm_op_id->status) & NA_SM_OP_CANCELED)) {
            hg_thread_spin_unlock(&retry_op_queue->lock);
            na_sm_buf_release(
                &na_sm_op_id->na_sm_addr->shared_region->copy_bufs, buf_idx);
            continue;
        }

        HG_QUEUE_REMOVE(
            &retry_op_queue->queue, na_sm_op_id, na_sm_op_id, entry);
        hg_atomic_and32(&na_sm_op_id->status, ~NA_SM_OP_QUEUED);

        hg_thread_spin_unlock(&retry_op_queue->lock);

        /* Copy buffer */
        na_sm_buf_copy_to(&na_sm_op_id->na_sm_addr->shared_region->copy_bufs,
            buf_idx, na_sm_op_id->info.msg.buf.const_ptr,
            na_sm_op_id->info.msg.buf_size);

        /* Post message to queue */
        msg_hdr.hdr.type = na_sm_op_id->completion_data.callback_info.type;
        msg_hdr.hdr.buf_idx = buf_idx & 0xff;
        msg_hdr.hdr.buf_size = na_sm_op_id->info.msg.buf_size & 0xffff;
        msg_hdr.hdr.tag = na_sm_op_id->info.msg.tag;

        rc = na_sm_msg_queue_push(na_sm_op_id->na_sm_addr->tx_queue, msg_hdr);
        NA_CHECK_ERROR(rc == NA_FALSE, error, ret, NA_AGAIN, "Full queue");

        /* Notify remote if notifications are enabled */
        if (na_sm_op_id->na_sm_addr->tx_notify > 0) {
            ret = na_sm_event_set(na_sm_op_id->na_sm_addr->tx_notify);
            NA_CHECK_NA_ERROR(
                error, ret, "Could not send completion notification");
        }

        /* Immediate completion, add directly to completion queue. */
        ret = na_sm_complete(na_sm_op_id, 0);
        NA_CHECK_NA_ERROR(error, ret, "Could not complete operation");
    } while (1);

    return ret;

error:
    na_sm_buf_release(
        &na_sm_op_id->na_sm_addr->shared_region->copy_bufs, buf_idx);
    hg_atomic_decr32(&na_sm_op_id->na_sm_addr->ref_count);
    hg_atomic_decr32(&na_sm_op_id->ref_count);

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_complete(struct na_sm_op_id *na_sm_op_id, int notify)
{
    struct na_cb_info *callback_info = NULL;
    na_return_t ret = NA_SUCCESS;
    hg_util_int32_t status;

    /* Mark op id as completed before checking for cancelation */
    status = hg_atomic_or32(&na_sm_op_id->status, NA_SM_OP_COMPLETED);

    /* Init callback info */
    callback_info = &na_sm_op_id->completion_data.callback_info;

    /* Check for current status before completing */
    if (status & NA_SM_OP_CANCELED) {
        /* If it was canceled while being processed, set callback ret
         * accordingly */
        NA_LOG_DEBUG("Operation ID %p was canceled", na_sm_op_id);
        callback_info->ret = NA_CANCELED;
    } else
        callback_info->ret = NA_SUCCESS;

    switch (callback_info->type) {
        case NA_CB_SEND_UNEXPECTED:
            break;
        case NA_CB_RECV_UNEXPECTED:
            if (callback_info->ret != NA_SUCCESS) {
                /* In case of cancellation where no recv'd data */
                callback_info->info.recv_unexpected.actual_buf_size = 0;
                callback_info->info.recv_unexpected.source = NA_ADDR_NULL;
                callback_info->info.recv_unexpected.tag = 0;
            } else {
                /* Increment addr ref count */
                hg_atomic_incr32(&na_sm_op_id->na_sm_addr->ref_count);

                /* Fill callback info */
                callback_info->info.recv_unexpected.actual_buf_size =
                    na_sm_op_id->info.msg.actual_buf_size;
                callback_info->info.recv_unexpected.source =
                    (na_addr_t) na_sm_op_id->na_sm_addr;
                callback_info->info.recv_unexpected.tag =
                    na_sm_op_id->info.msg.tag;
            }
            break;
        case NA_CB_SEND_EXPECTED:
            break;
        case NA_CB_RECV_EXPECTED:
            break;
        case NA_CB_PUT:
            break;
        case NA_CB_GET:
            break;
        default:
            NA_GOTO_ERROR(done, ret, NA_INVALID_ARG,
                "Operation type %d not supported", callback_info->type);
    }

    /* Add OP to NA completion queue */
    ret = na_cb_completion_add(
        na_sm_op_id->context, &na_sm_op_id->completion_data);
    NA_CHECK_NA_ERROR(done, ret, "Could not add callback to completion queue");

    /* Notify local completion */
    if (notify > 0) {
        int rc = hg_event_set(notify);
        NA_CHECK_ERROR(rc != HG_UTIL_SUCCESS, done, ret,
            na_sm_errno_to_na(errno), "Could not signal completion");
    }

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE void
na_sm_release(void *arg)
{
    struct na_sm_op_id *na_sm_op_id = (struct na_sm_op_id *) arg;

    NA_CHECK_WARNING(na_sm_op_id && (!(hg_atomic_get32(&na_sm_op_id->status) &
                                        NA_SM_OP_COMPLETED)),
        "Releasing resources from an uncompleted operation");

    if (na_sm_op_id->na_sm_addr) {
        na_sm_addr_free(na_sm_op_id->na_class, na_sm_op_id->na_sm_addr);
        na_sm_op_id->na_sm_addr = NULL;
    }
    na_sm_op_destroy(na_sm_op_id->na_class, na_sm_op_id);
}

/********************/
/* Plugin callbacks */
/********************/

static na_bool_t
na_sm_check_protocol(const char *protocol_name)
{
    na_bool_t accept = NA_FALSE;

    if (!strcmp("sm", protocol_name))
        accept = NA_TRUE;

    return accept;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_initialize(na_class_t *na_class, const struct na_info NA_UNUSED *na_info,
    na_bool_t listen)
{
    static hg_atomic_int32_t sm_id_g = HG_ATOMIC_VAR_INIT(0);
    pid_t pid;
    unsigned int id;
    char *username = NULL;
    na_bool_t no_wait = NA_FALSE;
    na_uint8_t max_contexts = 1; /* Default */
    na_return_t ret = NA_SUCCESS;

    /* Get init info */
    if (na_info->na_init_info) {
        /* Progress mode */
        if (na_info->na_init_info->progress_mode & NA_NO_BLOCK)
            no_wait = NA_TRUE;
        /* Max contexts */
        max_contexts = na_info->na_init_info->max_contexts;
    }

    /* Get PID */
    pid = getpid();

    /* Generate new SM ID */
    id = (unsigned int) hg_atomic_incr32(&sm_id_g) - 1;
    NA_CHECK_ERROR(id > UINT8_MAX, error, ret, NA_OVERFLOW,
        "Reached maximum number of SM instances for this process");

    /* Get username */
    username = getlogin_safe();
    NA_CHECK_ERROR(username == NULL, error, ret, na_sm_errno_to_na(errno),
        "Could not query login name");

    /* Reset errno */
    errno = 0;

    /* Initialize private data */
    na_class->plugin_class = malloc(sizeof(struct na_sm_class));
    NA_CHECK_ERROR(na_class->plugin_class == NULL, error, ret, NA_NOMEM,
        "Could not allocate SM private class");
    memset(na_class->plugin_class, 0, sizeof(struct na_sm_class));
    NA_SM_CLASS(na_class)->no_wait = no_wait;
    NA_SM_CLASS(na_class)->max_contexts = max_contexts;

    /* Copy username */
    NA_SM_CLASS(na_class)->username = strdup(username);
    NA_CHECK_ERROR(NA_SM_CLASS(na_class)->username == NULL, error, ret,
        NA_NOMEM, "Could not dup username");

    NA_LOG_DEBUG(
        "Opening new endpoint for %s with PID=%d, ID=%u", username, pid, id);

    /* Open endpoint */
    ret = na_sm_endpoint_open(&NA_SM_CLASS(na_class)->endpoint, username, pid,
        id & 0xff, listen, no_wait);
    NA_CHECK_NA_ERROR(
        error, ret, "Could not open endpoint for PID=%d, ID=%u", pid, id);

    return ret;

error:
    if (na_class->plugin_class) {
        free(NA_SM_CLASS(na_class)->username);
        free(na_class->plugin_class);
        na_class->plugin_class = NULL;
    }

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_finalize(na_class_t *na_class)
{
    na_return_t ret = NA_SUCCESS;

    if (!na_class->plugin_class)
        goto done;

    NA_LOG_DEBUG("Closing endpoint for %s", NA_SM_CLASS(na_class)->username);

    /* Close endpoint */
    ret = na_sm_endpoint_close(
        &NA_SM_CLASS(na_class)->endpoint, NA_SM_CLASS(na_class)->username);
    NA_CHECK_NA_ERROR(done, ret, "Could not close endpoint");

    free(NA_SM_CLASS(na_class)->username);
    free(na_class->plugin_class);
    na_class->plugin_class = NULL;

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_context_create(
    na_class_t NA_UNUSED *na_class, void **context, na_uint8_t NA_UNUSED id)
{
    na_return_t ret = NA_SUCCESS;

    *context = malloc(sizeof(struct na_sm_context));
    NA_CHECK_ERROR(*context == NULL, done, ret, NA_NOMEM,
        "Could not allocate SM private context");

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_context_destroy(na_class_t NA_UNUSED *na_class, void *context)
{
    free(context);

    return NA_SUCCESS;
}

/*---------------------------------------------------------------------------*/
static void
na_sm_cleanup(void)
{
    char pathname[NA_SM_MAX_FILENAME] = {'\0'};
    char *username = getlogin_safe();
    int rc;

    rc = snprintf(pathname, NA_SM_MAX_FILENAME, "%s/%s_%s", NA_SM_TMP_DIRECTORY,
        NA_SM_SHM_PREFIX, username);
    NA_CHECK_ERROR_NORET(rc < 0 || rc > NA_SM_MAX_FILENAME, done,
        "snprintf() failed, rc: %d", rc);

    /* We need to remove all files first before being able to remove the
     * directories */
    rc = nftw(pathname, na_sm_sock_path_cleanup, NA_SM_CLEANUP_NFDS,
        FTW_PHYS | FTW_DEPTH);
    NA_CHECK_WARNING(
        rc != 0 && errno != ENOENT, "nftw() failed (%s)", strerror(errno));

    rc = nftw(NA_SM_SHM_PATH, na_sm_shm_cleanup, NA_SM_CLEANUP_NFDS, FTW_PHYS);
    NA_CHECK_WARNING(
        rc != 0 && errno != ENOENT, "nftw() failed (%s)", strerror(errno));

done:
    return;
}

/*---------------------------------------------------------------------------*/
static na_op_id_t
na_sm_op_create(na_class_t *na_class)
{
    struct na_sm_op_id *na_sm_op_id = NULL;

    na_sm_op_id = (struct na_sm_op_id *) malloc(sizeof(struct na_sm_op_id));
    NA_CHECK_ERROR_NORET(
        na_sm_op_id == NULL, done, "Could not allocate NA SM operation ID");
    memset(na_sm_op_id, 0, sizeof(struct na_sm_op_id));

    na_sm_op_id->na_class = na_class;
    hg_atomic_init32(&na_sm_op_id->ref_count, 1);
    /* Completed by default */
    hg_atomic_init32(&na_sm_op_id->status, NA_SM_OP_COMPLETED);

    /* Set op ID release callbacks */
    na_sm_op_id->completion_data.plugin_callback = na_sm_release;
    na_sm_op_id->completion_data.plugin_callback_args = na_sm_op_id;

done:
    return (na_op_id_t) na_sm_op_id;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_op_destroy(na_class_t NA_UNUSED *na_class, na_op_id_t op_id)
{
    struct na_sm_op_id *na_sm_op_id = (struct na_sm_op_id *) op_id;

    if (hg_atomic_decr32(&na_sm_op_id->ref_count)) {
        /* Cannot free yet */
        goto done;
    }
    free(na_sm_op_id);

done:
    return NA_SUCCESS;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_addr_lookup(na_class_t *na_class, const char *name, na_addr_t *addr)
{
    struct na_sm_endpoint *na_sm_endpoint = &NA_SM_CLASS(na_class)->endpoint;
    struct na_sm_addr *na_sm_addr = NULL;
    pid_t pid;
    na_uint8_t id;
    na_uint64_t addr_key;
    na_return_t ret = NA_SUCCESS;

    /* Extra info from string */
    ret = na_sm_string_to_addr(name, &pid, &id);
    NA_CHECK_NA_ERROR(done, ret, "Could not convert string to address");

    NA_LOG_DEBUG("Lookup addr for PID=%d, ID=%d", pid, id);

    /* Generate key */
    addr_key = na_sm_addr_to_key(pid, id);

    /* Lookup addr from hash table */
    na_sm_addr = na_sm_addr_map_lookup(&na_sm_endpoint->addr_map, addr_key);
    if (!na_sm_addr) {
        struct na_sm_lookup_args args = {.endpoint = na_sm_endpoint,
            .username = NA_SM_CLASS(na_class)->username,
            .pid = pid,
            .id = id};
        na_return_t na_ret;

        NA_LOG_DEBUG("Addess was not found, attempting to insert it (key=%lu)",
            (long unsigned int) addr_key);

        /* Insert new entry and create new address if needed */
        na_ret = na_sm_addr_map_insert(&na_sm_endpoint->addr_map, addr_key,
            na_sm_addr_lookup_insert_cb, &args, &na_sm_addr);
        NA_CHECK_ERROR(na_ret != NA_SUCCESS && na_ret != NA_EXIST, done, ret,
            na_ret, "Could not insert new address");
    } else {
        NA_LOG_DEBUG(
            "Addess was found (key=%lu)", (long unsigned int) addr_key);
    }

    /* Increment refcount */
    hg_atomic_incr32(&na_sm_addr->ref_count);

    *addr = (na_addr_t) na_sm_addr;

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_addr_free(na_class_t *na_class, na_addr_t addr)
{
    struct na_sm_endpoint *na_sm_endpoint = &NA_SM_CLASS(na_class)->endpoint;
    struct na_sm_addr *na_sm_addr = (struct na_sm_addr *) addr;
    na_return_t ret = NA_SUCCESS;

    if (!na_sm_addr)
        goto done;

    if (hg_atomic_decr32(&na_sm_addr->ref_count))
        /* Cannot free yet */
        goto done;

    NA_LOG_DEBUG(
        "Freeing addr for PID=%d, ID=%d", na_sm_addr->pid, na_sm_addr->id);

    /* Remove address from list of addresses to poll */
    hg_thread_spin_lock(&na_sm_endpoint->poll_addr_list.lock);
    HG_LIST_REMOVE(na_sm_addr, entry);
    hg_thread_spin_unlock(&na_sm_endpoint->poll_addr_list.lock);

    ret = na_sm_addr_destroy(
        na_sm_endpoint, NA_SM_CLASS(na_class)->username, na_sm_addr);
    NA_CHECK_NA_ERROR(done, ret, "Could not destroy address");

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_addr_self(na_class_t *na_class, na_addr_t *addr)
{
    struct na_sm_addr *na_sm_addr = NA_SM_CLASS(na_class)->endpoint.source_addr;
    na_return_t ret = NA_SUCCESS;

    /* Increment refcount */
    hg_atomic_incr32(&na_sm_addr->ref_count);

    *addr = (na_addr_t) na_sm_addr;

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_addr_dup(
    na_class_t NA_UNUSED *na_class, na_addr_t addr, na_addr_t *new_addr)
{
    struct na_sm_addr *na_sm_addr = (struct na_sm_addr *) addr;
    na_return_t ret = NA_SUCCESS;

    /* Increment refcount */
    hg_atomic_incr32(&na_sm_addr->ref_count);

    *new_addr = (na_addr_t) na_sm_addr;

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_bool_t
na_sm_addr_cmp(na_class_t NA_UNUSED *na_class, na_addr_t addr1, na_addr_t addr2)
{
    struct na_sm_addr *na_sm_addr1 = (struct na_sm_addr *) addr1;
    struct na_sm_addr *na_sm_addr2 = (struct na_sm_addr *) addr2;

    return (na_sm_addr1->pid == na_sm_addr2->pid) &&
           (na_sm_addr1->id == na_sm_addr2->id);
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_bool_t
na_sm_addr_is_self(na_class_t *na_class, na_addr_t addr)
{
    return na_sm_addr_cmp(na_class,
        (na_addr_t) NA_SM_CLASS(na_class)->endpoint.source_addr, addr);
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_addr_to_string(na_class_t NA_UNUSED *na_class, char *buf,
    na_size_t *buf_size, na_addr_t addr)
{
    struct na_sm_addr *na_sm_addr = (struct na_sm_addr *) addr;
    na_size_t string_len;
    char addr_string[NA_SM_MAX_FILENAME] = {'\0'};
    na_return_t ret = NA_SUCCESS;
    int rc;

    rc = snprintf(addr_string, NA_SM_MAX_FILENAME, "sm://%d/%" SCNu8,
        na_sm_addr->pid, na_sm_addr->id);
    NA_CHECK_ERROR(rc < 0 || rc > NA_SM_MAX_FILENAME, done, ret, NA_OVERFLOW,
        "snprintf() failed, rc: %d", rc);

    string_len = strlen(addr_string);
    if (buf) {
        NA_CHECK_ERROR(string_len >= *buf_size, done, ret, NA_OVERFLOW,
            "Buffer size too small to copy addr");
        strcpy(buf, addr_string);
    }
    *buf_size = string_len + 1;

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_size_t
na_sm_addr_get_serialize_size(na_class_t NA_UNUSED *na_class, na_addr_t addr)
{
    struct na_sm_addr *na_sm_addr = (struct na_sm_addr *) addr;

    return sizeof(na_sm_addr->pid) + sizeof(na_sm_addr->id);
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_addr_serialize(na_class_t NA_UNUSED *na_class, void *buf,
    na_size_t buf_size, na_addr_t addr)
{
    struct na_sm_addr *na_sm_addr = (struct na_sm_addr *) addr;
    na_uint8_t *p = buf;
    na_size_t len = sizeof(na_sm_addr->pid) + sizeof(na_sm_addr->id);
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(buf_size < len, done, ret, NA_OVERFLOW,
        "Buffer size too small for serializing address");

    /* Encode PID */
    memcpy(p, &na_sm_addr->pid, sizeof(na_sm_addr->pid));
    p += sizeof(na_sm_addr->pid);

    /* Encode ID */
    memcpy(p, &na_sm_addr->id, sizeof(na_sm_addr->id));

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_addr_deserialize(
    na_class_t *na_class, na_addr_t *addr, const void *buf, na_size_t buf_size)
{
    struct na_sm_addr *na_sm_addr = NULL;
    const na_uint8_t *p = buf;
    pid_t pid;
    na_uint8_t id;
    na_size_t len = sizeof(pid) + sizeof(id);
    na_uint64_t addr_key;
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(buf_size < len, done, ret, NA_OVERFLOW,
        "Buffer size too small for serializing address");

    /* Decode PID */
    memcpy(&pid, p, sizeof(pid));
    p += sizeof(pid);

    /* Decode ID */
    memcpy(&id, p, sizeof(id));

    /* Generate key */
    addr_key = na_sm_addr_to_key(pid, id);

    /* Lookup addr from hash table */
    na_sm_addr = na_sm_addr_map_lookup(
        &NA_SM_CLASS(na_class)->endpoint.addr_map, addr_key);
    NA_CHECK_ERROR(
        na_sm_addr == NULL, done, ret, NA_NOENTRY, "Could not find address");

    /* Increment refcount */
    hg_atomic_incr32(&na_sm_addr->ref_count);

    *addr = na_sm_addr;

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_size_t
na_sm_msg_get_max_unexpected_size(const na_class_t NA_UNUSED *na_class)
{
    return NA_SM_UNEXPECTED_SIZE;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_size_t
na_sm_msg_get_max_expected_size(const na_class_t NA_UNUSED *na_class)
{
    return NA_SM_EXPECTED_SIZE;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_tag_t
na_sm_msg_get_max_tag(const na_class_t NA_UNUSED *na_class)
{
    return NA_SM_MAX_TAG;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_msg_send_unexpected(na_class_t *na_class, na_context_t *context,
    na_cb_t callback, void *arg, const void *buf, na_size_t buf_size,
    void NA_UNUSED *plugin_data, na_addr_t dest_addr,
    na_uint8_t NA_UNUSED dest_id, na_tag_t tag, na_op_id_t *op_id)
{
    struct na_sm_op_id *na_sm_op_id = NULL;
    struct na_sm_addr *na_sm_addr = (struct na_sm_addr *) dest_addr;
    unsigned int buf_idx;
    na_bool_t reserved = NA_FALSE;
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(buf_size > NA_SM_UNEXPECTED_SIZE, done, ret, NA_OVERFLOW,
        "Exceeds unexpected size, %d", buf_size);

    /* Check op_id */
    NA_CHECK_ERROR(
        op_id == NULL || op_id == NA_OP_ID_IGNORE || *op_id == NA_OP_ID_NULL,
        done, ret, NA_INVALID_ARG, "Invalid operation ID");

    na_sm_op_id = (struct na_sm_op_id *) *op_id;
    NA_CHECK_ERROR(
        !(hg_atomic_get32(&na_sm_op_id->status) & NA_SM_OP_COMPLETED), done,
        ret, NA_BUSY, "Attempting to use OP ID that was not completed");
    /* Make sure op ID is fully released before re-using it */
    while (hg_atomic_cas32(&na_sm_op_id->ref_count, 1, 2) != HG_UTIL_TRUE)
        cpu_spinwait();

    na_sm_op_id->context = context;
    na_sm_op_id->completion_data.callback_info.type = NA_CB_SEND_UNEXPECTED;
    na_sm_op_id->completion_data.callback = callback;
    na_sm_op_id->completion_data.callback_info.arg = arg;
    hg_atomic_incr32(&na_sm_addr->ref_count);
    na_sm_op_id->na_sm_addr = na_sm_addr;
    hg_atomic_set32(&na_sm_op_id->status, 0);
    /* TODO we assume that buf remains valid (safe because we pre-allocate
     * buffers) */
    na_sm_op_id->info.msg.buf.const_ptr = buf;
    na_sm_op_id->info.msg.buf_size = buf_size;
    na_sm_op_id->info.msg.actual_buf_size = buf_size;
    na_sm_op_id->info.msg.tag = tag;

    /* Try to reserve buffer atomically */
    ret = na_sm_buf_reserve(&na_sm_addr->shared_region->copy_bufs, &buf_idx);
    if (unlikely(ret == NA_AGAIN)) {
        struct na_sm_op_queue *retry_op_queue =
            &NA_SM_CLASS(na_class)->endpoint.retry_op_queue;

        NA_LOG_DEBUG("Pushing %p for retry", na_sm_op_id);

        /* Push op ID to retry queue */
        hg_thread_spin_lock(&retry_op_queue->lock);
        HG_QUEUE_PUSH_TAIL(&retry_op_queue->queue, na_sm_op_id, entry);
        hg_atomic_or32(&na_sm_op_id->status, NA_SM_OP_QUEUED);
        hg_thread_spin_unlock(&retry_op_queue->lock);

        ret = NA_SUCCESS;
    } else {
        na_sm_msg_hdr_t msg_hdr;
        na_bool_t rc;

        /* Successfully reserved a buffer */
        reserved = NA_TRUE;

        /* Reservation succeeded, copy buffer */
        na_sm_buf_copy_to(
            &na_sm_addr->shared_region->copy_bufs, buf_idx, buf, buf_size);

        /* Post message to queue */
        msg_hdr.hdr.type = na_sm_op_id->completion_data.callback_info.type;
        msg_hdr.hdr.buf_idx = buf_idx & 0xff;
        msg_hdr.hdr.buf_size = buf_size & 0xffff;
        msg_hdr.hdr.tag = tag;

        rc = na_sm_msg_queue_push(na_sm_addr->tx_queue, msg_hdr);
        NA_CHECK_ERROR(rc == NA_FALSE, error, ret, NA_AGAIN, "Full queue");

        /* Notify remote if notifications are enabled */
        if (na_sm_addr->tx_notify > 0) {
            ret = na_sm_event_set(na_sm_addr->tx_notify);
            NA_CHECK_NA_ERROR(
                error, ret, "Could not send completion notification");
        }

        /* Immediate completion, add directly to completion queue. */
        ret = na_sm_complete(na_sm_op_id,
            NA_SM_CLASS(na_class)->endpoint.source_addr->tx_notify);
        NA_CHECK_NA_ERROR(error, ret, "Could not complete operation");
    }

done:
    return ret;

error:
    if (reserved)
        na_sm_buf_release(&na_sm_addr->shared_region->copy_bufs, buf_idx);
    hg_atomic_decr32(&na_sm_addr->ref_count);
    hg_atomic_decr32(&na_sm_op_id->ref_count);

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_msg_recv_unexpected(na_class_t *na_class, na_context_t *context,
    na_cb_t callback, void *arg, void *buf, na_size_t buf_size,
    void NA_UNUSED *plugin_data, na_op_id_t *op_id)
{
    struct na_sm_unexpected_msg_queue *unexpected_msg_queue =
        &NA_SM_CLASS(na_class)->endpoint.unexpected_msg_queue;
    struct na_sm_unexpected_info *na_sm_unexpected_info;
    struct na_sm_op_id *na_sm_op_id = NULL;
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(buf_size > NA_SM_UNEXPECTED_SIZE, done, ret, NA_OVERFLOW,
        "Exceeds unexpected size, %d", buf_size);

    /* Check op_id */
    NA_CHECK_ERROR(
        op_id == NULL || op_id == NA_OP_ID_IGNORE || *op_id == NA_OP_ID_NULL,
        done, ret, NA_INVALID_ARG, "Invalid operation ID");

    na_sm_op_id = (struct na_sm_op_id *) *op_id;
    NA_CHECK_ERROR(
        !(hg_atomic_get32(&na_sm_op_id->status) & NA_SM_OP_COMPLETED), done,
        ret, NA_BUSY, "Attempting to use OP ID that was not completed");
    /* Make sure op ID is fully released before re-using it */
    while (hg_atomic_cas32(&na_sm_op_id->ref_count, 1, 2) != HG_UTIL_TRUE)
        cpu_spinwait();

    na_sm_op_id->context = context;
    na_sm_op_id->completion_data.callback_info.type = NA_CB_RECV_UNEXPECTED;
    na_sm_op_id->completion_data.callback = callback;
    na_sm_op_id->completion_data.callback_info.arg = arg;
    na_sm_op_id->na_sm_addr = NULL;
    hg_atomic_set32(&na_sm_op_id->status, 0);
    na_sm_op_id->info.msg.buf.ptr = buf;
    na_sm_op_id->info.msg.buf_size = buf_size;

    /* Look for an unexpected message already received */
    hg_thread_spin_lock(&unexpected_msg_queue->lock);
    na_sm_unexpected_info = HG_QUEUE_FIRST(&unexpected_msg_queue->queue);
    HG_QUEUE_POP_HEAD(&unexpected_msg_queue->queue, entry);
    hg_thread_spin_unlock(&unexpected_msg_queue->lock);
    if (unlikely(na_sm_unexpected_info)) {
        na_sm_op_id->na_sm_addr = na_sm_unexpected_info->na_sm_addr;
        hg_atomic_incr32(&na_sm_op_id->na_sm_addr->ref_count);
        na_sm_op_id->info.msg.actual_buf_size = na_sm_unexpected_info->buf_size;
        na_sm_op_id->info.msg.tag = na_sm_unexpected_info->tag;

        /* Copy buffers */
        memcpy(na_sm_op_id->info.msg.buf.ptr, na_sm_unexpected_info->buf,
            na_sm_unexpected_info->buf_size);

        free(na_sm_unexpected_info->buf);
        free(na_sm_unexpected_info);

        ret = na_sm_complete(na_sm_op_id,
            NA_SM_CLASS(na_class)->endpoint.source_addr->tx_notify);
        NA_CHECK_NA_ERROR(error, ret, "Could not complete operation");
    } else {
        struct na_sm_op_queue *unexpected_op_queue =
            &NA_SM_CLASS(na_class)->endpoint.unexpected_op_queue;

        na_sm_op_id->info.msg.actual_buf_size = 0;
        na_sm_op_id->info.msg.tag = 0;

        /* Nothing has been received yet so add op_id to progress queue */
        hg_thread_spin_lock(&unexpected_op_queue->lock);
        HG_QUEUE_PUSH_TAIL(&unexpected_op_queue->queue, na_sm_op_id, entry);
        hg_atomic_or32(&na_sm_op_id->status, NA_SM_OP_QUEUED);
        hg_thread_spin_unlock(&unexpected_op_queue->lock);
    }

done:
    return ret;

error:
    hg_atomic_decr32(&na_sm_op_id->na_sm_addr->ref_count);
    hg_atomic_decr32(&na_sm_op_id->ref_count);

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_msg_send_expected(na_class_t *na_class, na_context_t *context,
    na_cb_t callback, void *arg, const void *buf, na_size_t buf_size,
    void NA_UNUSED *plugin_data, na_addr_t dest_addr,
    na_uint8_t NA_UNUSED dest_id, na_tag_t tag, na_op_id_t *op_id)
{
    struct na_sm_op_id *na_sm_op_id = NULL;
    struct na_sm_addr *na_sm_addr = (struct na_sm_addr *) dest_addr;
    unsigned int buf_idx;
    na_bool_t reserved = NA_FALSE;
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(buf_size > NA_SM_EXPECTED_SIZE, done, ret, NA_OVERFLOW,
        "Exceeds expected size, %d", buf_size);

    /* Check op_id */
    NA_CHECK_ERROR(
        op_id == NULL || op_id == NA_OP_ID_IGNORE || *op_id == NA_OP_ID_NULL,
        done, ret, NA_INVALID_ARG, "Invalid operation ID");

    na_sm_op_id = (struct na_sm_op_id *) *op_id;
    NA_CHECK_ERROR(
        !(hg_atomic_get32(&na_sm_op_id->status) & NA_SM_OP_COMPLETED), done,
        ret, NA_BUSY, "Attempting to use OP ID that was not completed");
    /* Make sure op ID is fully released before re-using it */
    while (hg_atomic_cas32(&na_sm_op_id->ref_count, 1, 2) != HG_UTIL_TRUE)
        cpu_spinwait();

    na_sm_op_id->context = context;
    na_sm_op_id->completion_data.callback_info.type = NA_CB_SEND_EXPECTED;
    na_sm_op_id->completion_data.callback = callback;
    na_sm_op_id->completion_data.callback_info.arg = arg;
    hg_atomic_incr32(&na_sm_addr->ref_count);
    na_sm_op_id->na_sm_addr = na_sm_addr;
    hg_atomic_set32(&na_sm_op_id->status, 0);
    /* TODO we assume that buf remains valid (safe because we pre-allocate
     * buffers) */
    na_sm_op_id->info.msg.buf.const_ptr = buf;
    na_sm_op_id->info.msg.buf_size = buf_size;
    na_sm_op_id->info.msg.actual_buf_size = buf_size;
    na_sm_op_id->info.msg.tag = tag;

    /* Try to reserve buffer atomically */
    ret = na_sm_buf_reserve(&na_sm_addr->shared_region->copy_bufs, &buf_idx);
    if (unlikely(ret == NA_AGAIN)) {
        struct na_sm_op_queue *retry_op_queue =
            &NA_SM_CLASS(na_class)->endpoint.retry_op_queue;

        NA_LOG_DEBUG("Pushing %p for retry", na_sm_op_id);

        /* Push op ID to retry queue */
        hg_thread_spin_lock(&retry_op_queue->lock);
        HG_QUEUE_PUSH_TAIL(&retry_op_queue->queue, na_sm_op_id, entry);
        hg_atomic_or32(&na_sm_op_id->status, NA_SM_OP_QUEUED);
        hg_thread_spin_unlock(&retry_op_queue->lock);

        ret = NA_SUCCESS;
    } else {
        na_sm_msg_hdr_t msg_hdr;
        na_bool_t rc;

        /* Successfully reserved a buffer */
        reserved = NA_TRUE;

        /* Reservation succeeded, copy buffer */
        na_sm_buf_copy_to(
            &na_sm_addr->shared_region->copy_bufs, buf_idx, buf, buf_size);

        /* Post message to queue */
        msg_hdr.hdr.type = na_sm_op_id->completion_data.callback_info.type;
        msg_hdr.hdr.buf_idx = buf_idx & 0xff;
        msg_hdr.hdr.buf_size = buf_size & 0xffff;
        msg_hdr.hdr.tag = tag;

        rc = na_sm_msg_queue_push(na_sm_addr->tx_queue, msg_hdr);
        NA_CHECK_ERROR(rc == NA_FALSE, error, ret, NA_AGAIN, "Full queue");

        /* Notify remote if notifications are enabled */
        if (na_sm_addr->tx_notify > 0) {
            ret = na_sm_event_set(na_sm_addr->tx_notify);
            NA_CHECK_NA_ERROR(
                error, ret, "Could not send completion notification");
        }

        /* Immediate completion, add directly to completion queue. */
        ret = na_sm_complete(na_sm_op_id,
            NA_SM_CLASS(na_class)->endpoint.source_addr->tx_notify);
        NA_CHECK_NA_ERROR(error, ret, "Could not complete operation");
    }

done:
    return ret;

error:
    if (reserved)
        na_sm_buf_release(&na_sm_addr->shared_region->copy_bufs, buf_idx);
    hg_atomic_decr32(&na_sm_op_id->na_sm_addr->ref_count);
    hg_atomic_decr32(&na_sm_op_id->ref_count);

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_msg_recv_expected(na_class_t *na_class, na_context_t *context,
    na_cb_t callback, void *arg, void *buf, na_size_t buf_size,
    void NA_UNUSED *plugin_data, na_addr_t source_addr,
    na_uint8_t NA_UNUSED source_id, na_tag_t tag, na_op_id_t *op_id)
{
    struct na_sm_op_queue *expected_op_queue =
        &NA_SM_CLASS(na_class)->endpoint.expected_op_queue;
    struct na_sm_op_id *na_sm_op_id = NULL;
    struct na_sm_addr *na_sm_addr = (struct na_sm_addr *) source_addr;
    na_return_t ret = NA_SUCCESS;

    NA_CHECK_ERROR(buf_size > NA_SM_EXPECTED_SIZE, done, ret, NA_OVERFLOW,
        "Exceeds expected size, %d", buf_size);

    /* Check op_id */
    NA_CHECK_ERROR(
        op_id == NULL || op_id == NA_OP_ID_IGNORE || *op_id == NA_OP_ID_NULL,
        done, ret, NA_INVALID_ARG, "Invalid operation ID");

    na_sm_op_id = (struct na_sm_op_id *) *op_id;
    NA_CHECK_ERROR(
        !(hg_atomic_get32(&na_sm_op_id->status) & NA_SM_OP_COMPLETED), done,
        ret, NA_BUSY, "Attempting to use OP ID that was not completed");
    /* Make sure op ID is fully released before re-using it */
    while (hg_atomic_cas32(&na_sm_op_id->ref_count, 1, 2) != HG_UTIL_TRUE)
        cpu_spinwait();

    na_sm_op_id->context = context;
    na_sm_op_id->completion_data.callback_info.type = NA_CB_RECV_EXPECTED;
    na_sm_op_id->completion_data.callback = callback;
    na_sm_op_id->completion_data.callback_info.arg = arg;
    hg_atomic_incr32(&na_sm_addr->ref_count);
    na_sm_op_id->na_sm_addr = na_sm_addr;
    hg_atomic_set32(&na_sm_op_id->status, 0);
    na_sm_op_id->info.msg.buf.ptr = buf;
    na_sm_op_id->info.msg.buf_size = buf_size;
    na_sm_op_id->info.msg.actual_buf_size = 0;
    na_sm_op_id->info.msg.tag = tag;

    /* Expected messages must always be pre-posted, therefore a message should
     * never arrive before that call returns (not completes), simply add
     * op_id to queue */
    hg_thread_spin_lock(&expected_op_queue->lock);
    HG_QUEUE_PUSH_TAIL(&expected_op_queue->queue, na_sm_op_id, entry);
    hg_atomic_or32(&na_sm_op_id->status, NA_SM_OP_QUEUED);
    hg_thread_spin_unlock(&expected_op_queue->lock);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_mem_handle_create(na_class_t NA_UNUSED *na_class, void *buf,
    na_size_t buf_size, unsigned long flags, na_mem_handle_t *mem_handle)
{
    struct na_sm_mem_handle *na_sm_mem_handle = NULL;
    na_return_t ret = NA_SUCCESS;

    na_sm_mem_handle =
        (struct na_sm_mem_handle *) malloc(sizeof(struct na_sm_mem_handle));
    NA_CHECK_ERROR(na_sm_mem_handle == NULL, error, ret, NA_NOMEM,
        "Could not allocate NA SM memory handle");

    na_sm_mem_handle->iov = (struct iovec *) malloc(sizeof(struct iovec));
    NA_CHECK_ERROR(na_sm_mem_handle->iov == NULL, error, ret, NA_NOMEM,
        "Could not allocate iovec");

    na_sm_mem_handle->iov->iov_base = buf;
    na_sm_mem_handle->iov->iov_len = buf_size;
    na_sm_mem_handle->iovcnt = 1;
    na_sm_mem_handle->flags = flags & 0xff;
    na_sm_mem_handle->len = buf_size;

    *mem_handle = (na_mem_handle_t) na_sm_mem_handle;

    return ret;

error:
    if (na_sm_mem_handle) {
        free(na_sm_mem_handle->iov);
        free(na_sm_mem_handle);
    }
    return ret;
}

/*---------------------------------------------------------------------------*/
#ifdef NA_SM_HAS_CMA
static na_return_t
na_sm_mem_handle_create_segments(na_class_t NA_UNUSED *na_class,
    struct na_segment *segments, na_size_t segment_count, unsigned long flags,
    na_mem_handle_t *mem_handle)
{
    struct na_sm_mem_handle *na_sm_mem_handle = NULL;
    na_return_t ret = NA_SUCCESS;
    na_size_t i, iov_max;

    /* Check that we do not exceed IOV_MAX */
    iov_max = (na_size_t) sysconf(_SC_IOV_MAX);
    NA_CHECK_ERROR(segment_count > iov_max, error, ret, NA_INVALID_ARG,
        "Segment count exceeds IOV_MAX limit");

    na_sm_mem_handle =
        (struct na_sm_mem_handle *) malloc(sizeof(struct na_sm_mem_handle));
    NA_CHECK_ERROR(na_sm_mem_handle == NULL, error, ret, NA_NOMEM,
        "Could not allocate NA SM memory handle");

    na_sm_mem_handle->iov =
        (struct iovec *) malloc(segment_count * sizeof(struct iovec));
    NA_CHECK_ERROR(na_sm_mem_handle->iov == NULL, error, ret, NA_NOMEM,
        "Could not allocate iovec");

    na_sm_mem_handle->len = 0;
    for (i = 0; i < segment_count; i++) {
        na_sm_mem_handle->iov[i].iov_base = (void *) segments[i].address;
        na_sm_mem_handle->iov[i].iov_len = segments[i].size;
        na_sm_mem_handle->len += na_sm_mem_handle->iov[i].iov_len;
    }
    na_sm_mem_handle->iovcnt = segment_count;
    na_sm_mem_handle->flags = flags & 0xff;

    *mem_handle = (na_mem_handle_t) na_sm_mem_handle;

    return ret;

error:
    if (na_sm_mem_handle) {
        free(na_sm_mem_handle->iov);
        free(na_sm_mem_handle);
    }
    return ret;
}
#endif

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_mem_handle_free(
    na_class_t NA_UNUSED *na_class, na_mem_handle_t mem_handle)
{
    struct na_sm_mem_handle *na_sm_mem_handle =
        (struct na_sm_mem_handle *) mem_handle;
    na_return_t ret = NA_SUCCESS;

    free(na_sm_mem_handle->iov);
    free(na_sm_mem_handle);

    return ret;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_size_t
na_sm_mem_handle_get_serialize_size(
    na_class_t NA_UNUSED *na_class, na_mem_handle_t mem_handle)
{
    struct na_sm_mem_handle *na_sm_mem_handle =
        (struct na_sm_mem_handle *) mem_handle;
    unsigned long i;
    na_size_t ret = 2 * sizeof(unsigned long) + sizeof(size_t);

    for (i = 0; i < na_sm_mem_handle->iovcnt; i++)
        ret += sizeof(void *) + sizeof(size_t);

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_mem_handle_serialize(na_class_t NA_UNUSED *na_class, void *buf,
    na_size_t NA_UNUSED buf_size, na_mem_handle_t mem_handle)
{
    struct na_sm_mem_handle *na_sm_mem_handle =
        (struct na_sm_mem_handle *) mem_handle;
    char *buf_ptr = (char *) buf;
    na_return_t ret = NA_SUCCESS;
    unsigned long i;

    /* Number of segments */
    memcpy(buf_ptr, &na_sm_mem_handle->iovcnt, sizeof(unsigned long));
    buf_ptr += sizeof(unsigned long);

    /* Flags */
    memcpy(buf_ptr, &na_sm_mem_handle->flags, sizeof(unsigned long));
    buf_ptr += sizeof(unsigned long);

    /* Length */
    memcpy(buf_ptr, &na_sm_mem_handle->len, sizeof(size_t));
    buf_ptr += sizeof(size_t);

    /* Segments */
    for (i = 0; i < na_sm_mem_handle->iovcnt; i++) {
        memcpy(buf_ptr, &na_sm_mem_handle->iov[i].iov_base, sizeof(void *));
        buf_ptr += sizeof(void *);
        memcpy(buf_ptr, &na_sm_mem_handle->iov[i].iov_len, sizeof(size_t));
        buf_ptr += sizeof(size_t);
    }

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_mem_handle_deserialize(na_class_t NA_UNUSED *na_class,
    na_mem_handle_t *mem_handle, const void *buf, NA_UNUSED na_size_t buf_size)
{
    struct na_sm_mem_handle *na_sm_mem_handle = NULL;
    const char *buf_ptr = (const char *) buf;
    na_return_t ret = NA_SUCCESS;
    unsigned long i;

    na_sm_mem_handle =
        (struct na_sm_mem_handle *) malloc(sizeof(struct na_sm_mem_handle));
    NA_CHECK_ERROR(na_sm_mem_handle == NULL, error, ret, NA_NOMEM,
        "Could not allocate NA SM memory handle");
    na_sm_mem_handle->iov = NULL;

    /* Number of segments */
    memcpy(&na_sm_mem_handle->iovcnt, buf_ptr, sizeof(unsigned long));
    buf_ptr += sizeof(unsigned long);
    NA_CHECK_ERROR(na_sm_mem_handle->iovcnt == 0, error, ret, NA_FAULT,
        "NULL segment count");

    /* Flags */
    memcpy(&na_sm_mem_handle->flags, buf_ptr, sizeof(unsigned long));
    buf_ptr += sizeof(unsigned long);

    /* Length */
    memcpy(&na_sm_mem_handle->len, buf_ptr, sizeof(size_t));
    buf_ptr += sizeof(size_t);

    /* Segments */
    na_sm_mem_handle->iov = (struct iovec *) malloc(
        na_sm_mem_handle->iovcnt * sizeof(struct iovec));
    NA_CHECK_ERROR(na_sm_mem_handle->iov == NULL, error, ret, NA_NOMEM,
        "Could not allocate iovec");

    for (i = 0; i < na_sm_mem_handle->iovcnt; i++) {
        memcpy(&na_sm_mem_handle->iov[i].iov_base, buf_ptr, sizeof(void *));
        buf_ptr += sizeof(void *);
        memcpy(&na_sm_mem_handle->iov[i].iov_len, buf_ptr, sizeof(size_t));
        buf_ptr += sizeof(size_t);
    }

    *mem_handle = (na_mem_handle_t) na_sm_mem_handle;

    return ret;

error:
    if (na_sm_mem_handle) {
        free(na_sm_mem_handle->iov);
        free(na_sm_mem_handle);
    }
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_put(na_class_t *na_class, na_context_t *context, na_cb_t callback,
    void *arg, na_mem_handle_t local_mem_handle, na_offset_t local_offset,
    na_mem_handle_t remote_mem_handle, na_offset_t remote_offset,
    na_size_t length, na_addr_t remote_addr, na_uint8_t NA_UNUSED remote_id,
    na_op_id_t *op_id)
{
    struct na_sm_op_id *na_sm_op_id = NULL;
    struct na_sm_mem_handle *na_sm_mem_handle_local =
        (struct na_sm_mem_handle *) local_mem_handle;
    struct na_sm_mem_handle *na_sm_mem_handle_remote =
        (struct na_sm_mem_handle *) remote_mem_handle;
    struct na_sm_addr *na_sm_addr = (struct na_sm_addr *) remote_addr;
    struct iovec *local_iov, *remote_iov;
    struct iovec *local_iovs[IOV_MAX] = {NULL, 0};
    struct iovec *remote_iovs[IOV_MAX] = {NULL, 0};
    unsigned long liovcnt, riovcnt;
    na_return_t ret = NA_SUCCESS;
#if defined(NA_SM_HAS_CMA)
    ssize_t nwrite;
#elif defined(__APPLE__)
    kern_return_t kret;
    mach_port_name_t remote_task;
#endif

#if !defined(NA_SM_HAS_CMA) && !defined(__APPLE__)
    NA_GOTO_ERROR(
        done, ret, NA_OPNOTSUPPORTED, "Not implemented for this platform");
#endif

    switch (na_sm_mem_handle_remote->flags) {
        case NA_MEM_READ_ONLY:
            NA_GOTO_ERROR(done, ret, NA_PERMISSION,
                "Registered memory requires write permission");
            break;
        case NA_MEM_WRITE_ONLY:
        case NA_MEM_READWRITE:
            break;
        default:
            NA_GOTO_ERROR(
                done, ret, NA_INVALID_ARG, "Invalid memory access flag");
    }

    /* Check op_id */
    NA_CHECK_ERROR(
        op_id == NULL || op_id == NA_OP_ID_IGNORE || *op_id == NA_OP_ID_NULL,
        done, ret, NA_INVALID_ARG, "Invalid operation ID");

    na_sm_op_id = (struct na_sm_op_id *) *op_id;
    NA_CHECK_ERROR(
        !(hg_atomic_get32(&na_sm_op_id->status) & NA_SM_OP_COMPLETED), done,
        ret, NA_BUSY, "Attempting to use OP ID that was not completed");
    /* Make sure op ID is fully released before re-using it */
    while (hg_atomic_cas32(&na_sm_op_id->ref_count, 1, 2) != HG_UTIL_TRUE)
        cpu_spinwait();

    na_sm_op_id->context = context;
    na_sm_op_id->completion_data.callback_info.type = NA_CB_PUT;
    na_sm_op_id->completion_data.callback = callback;
    na_sm_op_id->completion_data.callback_info.arg = arg;
    hg_atomic_incr32(&na_sm_addr->ref_count);
    na_sm_op_id->na_sm_addr = na_sm_addr;
    hg_atomic_set32(&na_sm_op_id->status, 0);

    /* Translate local offset, skip this step if not necessary */
    if (local_offset || length != na_sm_mem_handle_local->len) {
        local_iov = (struct iovec *) local_iovs;
        na_sm_offset_translate(
            na_sm_mem_handle_local, local_offset, length, local_iov, &liovcnt);
        NA_LOG_DEBUG("Translated local offsets into %lu segment(s)", liovcnt);
    } else {
        local_iov = na_sm_mem_handle_local->iov;
        liovcnt = na_sm_mem_handle_local->iovcnt;
    }

    /* Translate remote offset, skip this step if not necessary */
    if (remote_offset || length != na_sm_mem_handle_remote->len) {
        remote_iov = (struct iovec *) remote_iovs;
        na_sm_offset_translate(na_sm_mem_handle_remote, remote_offset, length,
            remote_iov, &riovcnt);
        NA_LOG_DEBUG("Translated remote offsets into %lu segment(s)", riovcnt);
    } else {
        remote_iov = na_sm_mem_handle_remote->iov;
        riovcnt = na_sm_mem_handle_remote->iovcnt;
    }

#if defined(NA_SM_HAS_CMA)
    nwrite = process_vm_writev(na_sm_addr->pid, local_iov, liovcnt, remote_iov,
        riovcnt, /* unused */ 0);
    if (unlikely(nwrite < 0)) {
        if ((errno == EPERM) && na_sm_get_ptrace_scope_value()) {
            NA_GOTO_ERROR(error, ret, na_sm_errno_to_na(errno),
                "process_vm_writev() failed (%s):\n",
                "Kernel Yama configuration does not allow cross-memory attach, "
                "either run as root: \n"
                "# /usr/sbin/sysctl kernel.yama.ptrace_scope=0\n"
                "or if set to restricted, add the following call to your "
                "application:\n"
                "prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);\n"
                "See https://www.kernel.org/doc/Documentation/security/Yama.txt"
                " for more details.",
                strerror(errno));
        } else
            NA_GOTO_ERROR(error, ret, na_sm_errno_to_na(errno),
                "process_vm_writev() failed (%s)", strerror(errno));
    }
    NA_CHECK_ERROR((na_size_t) nwrite != length, error, ret, NA_MSGSIZE,
        "Wrote %ld bytes, was expecting %lu bytes", nwrite, length);
#elif defined(__APPLE__)
    kret = task_for_pid(mach_task_self(), na_sm_addr->pid, &remote_task);
    NA_CHECK_ERROR(kret != KERN_SUCCESS, error, ret, NA_PERMISSION,
        "task_for_pid() failed (%s)\n"
        "Permission must be set to access remote memory, please refer to the "
        "documentation for instructions.",
        mach_error_string(kret));
    NA_CHECK_ERROR(liovcnt > 1 || riovcnt > 1, error, ret, NA_OPNOTSUPPORTED,
        "Non-contiguous transfers are not supported");

    kret = mach_vm_write(remote_task, (mach_vm_address_t) remote_iov->iov_base,
        (mach_vm_address_t) local_iov->iov_base,
        (mach_msg_type_number_t) length);
    NA_CHECK_ERROR(kret != KERN_SUCCESS, error, ret, NA_PROTOCOL_ERROR,
        "mach_vm_write() failed (%s)", mach_error_string(kret));
#endif

    /* Immediate completion */
    ret = na_sm_complete(
        na_sm_op_id, NA_SM_CLASS(na_class)->endpoint.source_addr->tx_notify);
    NA_CHECK_NA_ERROR(error, ret, "Could not complete operation");

done:
    return ret;

error:
    hg_atomic_decr32(&na_sm_op_id->na_sm_addr->ref_count);
    hg_atomic_decr32(&na_sm_op_id->ref_count);

    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_get(na_class_t *na_class, na_context_t *context, na_cb_t callback,
    void *arg, na_mem_handle_t local_mem_handle, na_offset_t local_offset,
    na_mem_handle_t remote_mem_handle, na_offset_t remote_offset,
    na_size_t length, na_addr_t remote_addr, na_uint8_t NA_UNUSED remote_id,
    na_op_id_t *op_id)
{
    struct na_sm_op_id *na_sm_op_id = NULL;
    struct na_sm_mem_handle *na_sm_mem_handle_local =
        (struct na_sm_mem_handle *) local_mem_handle;
    struct na_sm_mem_handle *na_sm_mem_handle_remote =
        (struct na_sm_mem_handle *) remote_mem_handle;
    struct na_sm_addr *na_sm_addr = (struct na_sm_addr *) remote_addr;
    struct iovec *local_iov, *remote_iov;
    struct iovec *local_iovs[IOV_MAX] = {NULL, 0};
    struct iovec *remote_iovs[IOV_MAX] = {NULL, 0};
    unsigned long liovcnt, riovcnt;
    na_return_t ret = NA_SUCCESS;
#if defined(NA_SM_HAS_CMA)
    ssize_t nread;
#elif defined(__APPLE__)
    mach_vm_size_t nread;
    kern_return_t kret;
    mach_port_name_t remote_task;
#endif

#if !defined(NA_SM_HAS_CMA) && !defined(__APPLE__)
    NA_GOTO_ERROR(
        done, ret, NA_OPNOTSUPPORTED, "Not implemented for this platform");
#endif

    switch (na_sm_mem_handle_remote->flags) {
        case NA_MEM_WRITE_ONLY:
            NA_GOTO_ERROR(done, ret, NA_PERMISSION,
                "Registered memory requires write permission");
            break;
        case NA_MEM_READ_ONLY:
        case NA_MEM_READWRITE:
            break;
        default:
            NA_GOTO_ERROR(
                done, ret, NA_INVALID_ARG, "Invalid memory access flag");
    }

    na_sm_op_id = (struct na_sm_op_id *) *op_id;
    NA_CHECK_ERROR(
        !(hg_atomic_get32(&na_sm_op_id->status) & NA_SM_OP_COMPLETED), done,
        ret, NA_BUSY, "Attempting to use OP ID that was not completed");
    /* Make sure op ID is fully released before re-using it */
    while (hg_atomic_cas32(&na_sm_op_id->ref_count, 1, 2) != HG_UTIL_TRUE)
        cpu_spinwait();

    na_sm_op_id->context = context;
    na_sm_op_id->completion_data.callback_info.type = NA_CB_GET;
    na_sm_op_id->completion_data.callback = callback;
    na_sm_op_id->completion_data.callback_info.arg = arg;
    hg_atomic_incr32(&na_sm_addr->ref_count);
    na_sm_op_id->na_sm_addr = na_sm_addr;
    hg_atomic_set32(&na_sm_op_id->status, 0);

    /* Translate local offset, skip this step if not necessary */
    if (local_offset || length != na_sm_mem_handle_local->len) {
        local_iov = (struct iovec *) local_iovs;
        na_sm_offset_translate(
            na_sm_mem_handle_local, local_offset, length, local_iov, &liovcnt);
        NA_LOG_DEBUG("Translated local offsets into %lu segment(s)", liovcnt);
    } else {
        local_iov = na_sm_mem_handle_local->iov;
        liovcnt = na_sm_mem_handle_local->iovcnt;
    }

    /* Translate remote offset, skip this step if not necessary */
    if (remote_offset || length != na_sm_mem_handle_remote->len) {
        remote_iov = (struct iovec *) remote_iovs;
        na_sm_offset_translate(na_sm_mem_handle_remote, remote_offset, length,
            remote_iov, &riovcnt);
        NA_LOG_DEBUG("Translated remote offsets into %lu segment(s)", riovcnt);
    } else {
        remote_iov = na_sm_mem_handle_remote->iov;
        riovcnt = na_sm_mem_handle_remote->iovcnt;
    }

#if defined(NA_SM_HAS_CMA)
    nread = process_vm_readv(na_sm_addr->pid, local_iov, liovcnt, remote_iov,
        riovcnt, /* unused */ 0);
    if (unlikely(nread < 0)) {
        if ((errno == EPERM) && na_sm_get_ptrace_scope_value()) {
            NA_GOTO_ERROR(error, ret, na_sm_errno_to_na(errno),
                "process_vm_readv() failed (%s):\n",
                "Kernel Yama configuration does not allow cross-memory attach, "
                "either run as root: \n"
                "# /usr/sbin/sysctl kernel.yama.ptrace_scope=0\n"
                "or if set to restricted, add the following call to your "
                "application:\n"
                "prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);\n"
                "See https://www.kernel.org/doc/Documentation/security/Yama.txt"
                " for more details.",
                strerror(errno));
        } else
            NA_CHECK_ERROR(nread < 0, error, ret, na_sm_errno_to_na(errno),
                "process_vm_readv() failed (%s)", strerror(errno));
    }
#elif defined(__APPLE__)
    kret = task_for_pid(mach_task_self(), na_sm_addr->pid, &remote_task);
    NA_CHECK_ERROR(kret != KERN_SUCCESS, error, ret, NA_PERMISSION,
        "task_for_pid() failed (%s)\n"
        "Permission must be set to access remote memory, please refer to the "
        "documentation for instructions.",
        mach_error_string(kret));
    NA_CHECK_ERROR(liovcnt > 1 || riovcnt > 1, error, ret, NA_OPNOTSUPPORTED,
        "Non-contiguous transfers are not supported");

    kret = mach_vm_read_overwrite(remote_task,
        (mach_vm_address_t) remote_iov->iov_base, length,
        (mach_vm_address_t) local_iov->iov_base, &nread);
    NA_CHECK_ERROR(kret != KERN_SUCCESS, error, ret, NA_PROTOCOL_ERROR,
        "mach_vm_read_overwrite() failed (%s)", mach_error_string(kret));
#endif
#if defined(NA_SM_HAS_CMA) || defined(__APPLE__)
    NA_CHECK_ERROR((na_size_t) nread != length, error, ret, NA_MSGSIZE,
        "Read %ld bytes, was expecting %lu bytes", nread, length);
#endif

    /* Immediate completion */
    ret = na_sm_complete(
        na_sm_op_id, NA_SM_CLASS(na_class)->endpoint.source_addr->tx_notify);
    NA_CHECK_NA_ERROR(error, ret, "Could not complete operation");

done:
    return ret;

error:
    hg_atomic_decr32(&na_sm_op_id->na_sm_addr->ref_count);
    hg_atomic_decr32(&na_sm_op_id->ref_count);

    return ret;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE int
na_sm_poll_get_fd(na_class_t *na_class, na_context_t NA_UNUSED *context)
{
    int fd = -1;

    if (NA_SM_CLASS(na_class)->endpoint.poll_set) {
        fd = hg_poll_get_fd(NA_SM_CLASS(na_class)->endpoint.poll_set);
        NA_CHECK_ERROR_NORET(
            fd == -1, done, "Could not get poll fd from poll set");
    }

done:
    return fd;
}

/*---------------------------------------------------------------------------*/
static NA_INLINE na_bool_t
na_sm_poll_try_wait(na_class_t *na_class, na_context_t NA_UNUSED *context)
{
    struct na_sm_addr *na_sm_addr;

    /* Check whether something is in one of the rx queues */
    hg_thread_spin_lock(&NA_SM_CLASS(na_class)->endpoint.poll_addr_list.lock);
    HG_LIST_FOREACH (na_sm_addr,
        &NA_SM_CLASS(na_class)->endpoint.poll_addr_list.list, entry) {
        if (!na_sm_msg_queue_is_empty(na_sm_addr->rx_queue)) {
            hg_thread_spin_unlock(
                &NA_SM_CLASS(na_class)->endpoint.poll_addr_list.lock);
            return NA_FALSE;
        }
    }
    hg_thread_spin_unlock(&NA_SM_CLASS(na_class)->endpoint.poll_addr_list.lock);

    return NA_TRUE;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_progress(
    na_class_t *na_class, na_context_t *context, unsigned int timeout)
{
    struct na_sm_endpoint *na_sm_endpoint = &NA_SM_CLASS(na_class)->endpoint;
    const char *username = NA_SM_CLASS(na_class)->username;
    struct hg_poll_event *events = NA_SM_CONTEXT(context)->events;
    double remaining =
        timeout / 1000.0; /* Convert timeout in ms into seconds */
    na_return_t ret = NA_TIMEOUT;

    do {
        na_bool_t progressed = NA_FALSE;
        hg_time_t t1, t2;

        if (timeout)
            hg_time_get_current_ms(&t1);

        if (timeout && na_sm_endpoint->poll_set) {
            unsigned int nevents = 0, i;
            /* Just wait on a single event, anything greater may increase
             * latency, and slow down progress, we will not wait next round
             * if something is still in the queues */
            int rc = hg_poll_wait(na_sm_endpoint->poll_set,
                (unsigned int) (remaining * 1000.0), NA_SM_MAX_EVENTS, events,
                &nevents);
            NA_CHECK_ERROR(rc != HG_UTIL_SUCCESS, done, ret,
                na_sm_errno_to_na(errno), "hg_poll_wait() failed");

            /* Process events */
            for (i = 0; i < nevents; i++) {
                struct na_sm_addr *poll_addr = NULL;
                na_bool_t progressed_notify = NA_FALSE;
                na_bool_t progressed_rx = NA_FALSE;

                switch (*(na_sm_poll_type_t *) events[i].data.ptr) {
                    case NA_SM_POLL_SOCK:
                        NA_LOG_DEBUG("NA_SM_POLL_SOCK event");
                        ret = na_sm_progress_sock(
                            na_sm_endpoint, username, &progressed_notify);
                        NA_CHECK_NA_ERROR(done, ret, "Could not progress sock");
                        break;
                    case NA_SM_POLL_TX_NOTIFY:
                        NA_LOG_DEBUG("NA_SM_POLL_TX_NOTIFY event");
                        poll_addr = container_of(events[i].data.ptr,
                            struct na_sm_addr, tx_poll_type);
                        ret = na_sm_progress_tx_notify(
                            poll_addr, &progressed_notify);
                        NA_CHECK_NA_ERROR(
                            done, ret, "Could not progress tx notify");
                        break;
                    case NA_SM_POLL_RX_NOTIFY:
                        NA_LOG_DEBUG("NA_SM_POLL_RX_NOTIFY event");
                        poll_addr = container_of(events[i].data.ptr,
                            struct na_sm_addr, rx_poll_type);

                        ret = na_sm_progress_rx_notify(
                            poll_addr, &progressed_notify);
                        NA_CHECK_NA_ERROR(
                            done, ret, "Could not progress rx notify");

                        ret = na_sm_progress_rx_queue(
                            na_sm_endpoint, poll_addr, &progressed_rx);
                        NA_CHECK_NA_ERROR(
                            done, ret, "Could not progress rx queue");

                        break;
                    default:
                        NA_GOTO_ERROR(done, ret, NA_INVALID_ARG,
                            "Operation type %d not supported",
                            *(na_sm_poll_type_t *) events[i].data.ptr);
                }

                progressed |= (progressed_rx | progressed_notify);
            }
        } else {
            struct na_sm_addr_list *poll_addr_list =
                &na_sm_endpoint->poll_addr_list;
            struct na_sm_addr *poll_addr;

            /* Check whether something is in one of the rx queues */
            hg_thread_spin_lock(&poll_addr_list->lock);
            HG_LIST_FOREACH (poll_addr, &poll_addr_list->list, entry) {
                na_bool_t progressed_rx = NA_FALSE;

                hg_thread_spin_unlock(&poll_addr_list->lock);

                if (na_sm_endpoint->poll_set) {
                    na_bool_t progressed_notify = NA_FALSE;
                    ret =
                        na_sm_progress_rx_notify(poll_addr, &progressed_notify);
                    NA_CHECK_NA_ERROR(
                        done, ret, "Could not progress rx notify");
                    progressed |= progressed_notify;
                }
                ret = na_sm_progress_rx_queue(
                    na_sm_endpoint, poll_addr, &progressed_rx);
                NA_CHECK_NA_ERROR(done, ret, "Could not progress rx queue");
                progressed |= progressed_rx;

                hg_thread_spin_lock(&poll_addr_list->lock);
            }
            hg_thread_spin_unlock(&poll_addr_list->lock);

            /* Look for message in cmd queue (if listening) */
            if (na_sm_endpoint->poll_set) {
                na_bool_t progressed_notify = NA_FALSE;

                ret = na_sm_progress_tx_notify(
                    na_sm_endpoint->source_addr, &progressed_notify);
                NA_CHECK_NA_ERROR(done, ret, "Could not progress tx notify");
                progressed |= progressed_notify;

                if (na_sm_endpoint->source_addr->shared_region) {
                    na_bool_t progressed_sock = NA_FALSE;
                    ret = na_sm_progress_sock(
                        na_sm_endpoint, username, &progressed_sock);
                    NA_CHECK_NA_ERROR(done, ret, "Could not progress sock");
                    progressed |= progressed_sock;
                }
            } else if (na_sm_endpoint->source_addr->shared_region) {
                na_sm_cmd_hdr_t cmd_hdr = {.val = 0};

                while (na_sm_cmd_queue_pop(
                    &na_sm_endpoint->source_addr->shared_region->cmd_queue,
                    &cmd_hdr)) {
                    ret = na_sm_process_cmd(
                        na_sm_endpoint, username, cmd_hdr, -1, -1);
                    NA_CHECK_NA_ERROR(done, ret, "Could not process cmd");
                    progressed |= NA_TRUE;
                }
            }
        }

        /* Process retries */
        ret = na_sm_process_retries(&na_sm_endpoint->retry_op_queue);
        NA_CHECK_NA_ERROR(done, ret, "Could not process retried msgs");

        if (timeout) {
            hg_time_get_current_ms(&t2);
            remaining -= hg_time_diff(t2, t1);
        }

        if (!progressed)
            ret = NA_TIMEOUT; /* Return NA_TIMEOUT if no events */

    } while (remaining > 0 && (ret != NA_SUCCESS));

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_sm_cancel(
    na_class_t *na_class, na_context_t NA_UNUSED *context, na_op_id_t op_id)
{
    struct na_sm_op_queue *op_queue = NULL;
    struct na_sm_op_id *na_sm_op_id = (struct na_sm_op_id *) op_id;
    na_return_t ret = NA_SUCCESS;

    /* Exit if op has already completed */
    if (hg_atomic_or32(&na_sm_op_id->status, NA_SM_OP_CANCELED) &
        NA_SM_OP_COMPLETED)
        goto done;

    NA_LOG_DEBUG("Canceling operation ID %p", na_sm_op_id);

    switch (na_sm_op_id->completion_data.callback_info.type) {
        case NA_CB_RECV_UNEXPECTED:
            /* Must remove op_id from unexpected op queue */
            op_queue = &NA_SM_CLASS(na_class)->endpoint.unexpected_op_queue;
            break;
        case NA_CB_RECV_EXPECTED:
            /* Must remove op_id from unexpected op queue */
            op_queue = &NA_SM_CLASS(na_class)->endpoint.expected_op_queue;
            break;
        case NA_CB_SEND_UNEXPECTED:
        case NA_CB_SEND_EXPECTED:
            /* Must remove op_id from retry op queue */
            op_queue = &NA_SM_CLASS(na_class)->endpoint.retry_op_queue;
            break;
        case NA_CB_PUT:
            /* Nothing */
            break;
        case NA_CB_GET:
            /* Nothing */
            break;
        default:
            NA_GOTO_ERROR(done, ret, NA_INVALID_ARG,
                "Operation type %d not supported",
                na_sm_op_id->completion_data.callback_info.type);
    }

    /* Remove op id from queue it is on */
    if (op_queue) {
        na_bool_t canceled = NA_FALSE;

        hg_thread_spin_lock(&op_queue->lock);
        if (hg_atomic_get32(&na_sm_op_id->status) & NA_SM_OP_QUEUED) {
            HG_QUEUE_REMOVE(&op_queue->queue, na_sm_op_id, na_sm_op_id, entry);
            hg_atomic_and32(&na_sm_op_id->status, ~NA_SM_OP_QUEUED);
            canceled = NA_TRUE;
        }
        hg_thread_spin_unlock(&op_queue->lock);

        /* Cancel op id */
        if (canceled) {
            ret = na_sm_complete(na_sm_op_id,
                NA_SM_CLASS(na_class)->endpoint.source_addr->tx_notify);
            NA_CHECK_NA_ERROR(done, ret, "Could not complete operation");
        }
    }

done:
    return ret;
}
