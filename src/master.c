#include <sys/socket.h>

#include "postgres.h"
#include "utils/varlena.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "common/ip.h"
#include "postmaster/bgworker.h"
#include "postmaster/postmaster.h"

#include "rustica_wamr.h"

typedef struct Socket Socket;

#define TYPE_UNSET 0
#define TYPE_IPC 1
#define TYPE_FRONTEND 2
#define TYPE_BACKEND 3
#define BACKEND_HELLO "RUSTICA!"
#define MAXLISTEN 64
static WaitEventSetEx *rm_wait_set = NULL;
static Socket *sockets;
static int total_sockets = 0;
static bool shutdown_requested = false;
static bool worker_died = false;
static int *idle_workers;
static int idle_head = 0, idle_tail = 0, idle_size = 0;
static int num_workers;
static BackgroundWorkerHandle **worker_handles;

typedef struct Socket {
    char type;
    pgsocket fd;
    int pos;

    uint8_t read_offset;
    uint32_t worker_pid;
} Socket;

static int
listen_frontend(pgsocket *listen_sockets) {
    int success, status, nsockets;
    char *addr_string, *addr;
    List *list;
    ListCell *cell;

    for (int i = 0; i < MAXLISTEN; i++)
        listen_sockets[i] = PGINVALID_SOCKET;

    addr_string = pstrdup(rst_listen_addresses);
    if (!SplitGUCList(addr_string, ',', &list)) {
        ereport(FATAL,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid list syntax in parameter \"%s\"",
                        "listen_addresses")));
    }
    success = 0;
    foreach (cell, list) {
        addr = (char *)lfirst(cell);

        if (strcmp(addr, "*") == 0)
            status = StreamServerPort(AF_UNSPEC,
                                      NULL,
                                      (unsigned short)rst_port,
                                      NULL,
                                      listen_sockets,
                                      MAXLISTEN);
        else
            status = StreamServerPort(AF_UNSPEC,
                                      addr,
                                      (unsigned short)rst_port,
                                      NULL,
                                      listen_sockets,
                                      MAXLISTEN);

        if (status == STATUS_OK) {
            success++;
        }
        else
            ereport(
                WARNING,
                (errmsg("could not create listen socket for \"%s\"", addr)));
    }
    if (!success && list != NIL)
        ereport(FATAL, (errmsg("could not create any TCP/IP sockets")));
    list_free(list);
    pfree(addr_string);
    nsockets = 0;
    while (nsockets < MAXLISTEN && listen_sockets[nsockets] != PGINVALID_SOCKET)
        ++nsockets;
    if (nsockets == 0)
        ereport(FATAL, (errmsg("no socket created for listening")));
    return nsockets;
}

static pgsocket
listen_backend() {
    pgsocket ipc_sock;
    char *socketdir;
    int status;

    ipc_sock = PGINVALID_SOCKET;
    socketdir = rst_ipc_dir == NULL ? DataDir : rst_ipc_dir;
    status = StreamServerPort(AF_UNIX,
                              NULL,
                              (unsigned short)rst_port,
                              socketdir,
                              &ipc_sock,
                              1);
    if (status != STATUS_OK) {
        ereport(
            FATAL,
            (errmsg("could not create Unix-domain socket in directory \"%s\"",
                    socketdir)));
    }
    return ipc_sock;
}

static void
on_sigterm(SIGNAL_ARGS) {
    shutdown_requested = true;
    SetLatch(MyLatch);
}

static void
on_sigusr1(SIGNAL_ARGS) {
    worker_died = true;
    SetLatch(MyLatch);
}

static void
startup() {
    pgsocket listen_sockets[MAXLISTEN], ipc_sock;
    int num_listen_sockets;
    Socket *socket;

    pqsignal(SIGTERM, on_sigterm);
    pqsignal(SIGUSR1, on_sigusr1);
    BackgroundWorkerUnblockSignals();

    num_listen_sockets = listen_frontend(&listen_sockets);
    ipc_sock = listen_backend();
    total_sockets = 1 + num_listen_sockets + max_worker_processes;

    sockets = (Socket *)MemoryContextAllocZero(CurrentMemoryContext,
                                               sizeof(Socket) * total_sockets);
    rm_wait_set = CreateWaitEventSetEx(CurrentMemoryContext, total_sockets);
    idle_workers = (int *)MemoryContextAllocZero(CurrentMemoryContext,
                                                 sizeof(int) * total_sockets);
    worker_handles = (BackgroundWorkerHandle *)MemoryContextAllocZero(
        CurrentMemoryContext,
        sizeof(BackgroundWorkerHandle *) * max_worker_processes);

    socket = &sockets[NextWaitEventPos(rm_wait_set)];
    socket->type = TYPE_UNSET;
    socket->fd = -1;
    socket->pos = AddWaitEventToSetEx(rm_wait_set,
                                      WL_LATCH_SET,
                                      PGINVALID_SOCKET,
                                      MyLatch,
                                      socket);
    Assert(socket->pos != -1);

    socket = &sockets[NextWaitEventPos(rm_wait_set)];
    socket->type = TYPE_IPC;
    socket->fd = ipc_sock;
    socket->pos = AddWaitEventToSetEx(rm_wait_set,
                                      WL_SOCKET_ACCEPT,
                                      socket->fd,
                                      NULL,
                                      socket);
    Assert(socket->pos != -1);

    for (int i = 0; i < num_listen_sockets; i++) {
        if (listen_sockets[i] == PGINVALID_SOCKET)
            ereport(FATAL, (errmsg("no socket created for listening")));
        socket = &sockets[NextWaitEventPos(rm_wait_set)];
        socket->type = TYPE_FRONTEND;
        socket->fd = listen_sockets[i];
        socket->pos = AddWaitEventToSetEx(rm_wait_set,
                                          WL_SOCKET_ACCEPT,
                                          socket->fd,
                                          NULL,
                                          socket);
        Assert(socket->pos != -1);
    }
}

static inline void
on_backend_connect(Socket *socket, uint32 events) {
    pgsocket sock;
    SockAddr addr;

    if (!(events & WL_SOCKET_ACCEPT))
        return;

    ereport(DEBUG1,
            (errmsg("Accept backend connection from: fd=%d", socket->fd)));
    addr.salen = sizeof(addr.addr);
    sock = accept(socket->fd, (struct sockaddr *)&addr.addr, &addr.salen);
    if (sock == PGINVALID_SOCKET) {
        ereport(LOG,
                (errcode_for_socket_access(),
                 errmsg("could not accept new connection: %m")));
        pg_usleep(100000L); // wait 0.1 sec
        return;
    }

    socket = &sockets[NextWaitEventPos(rm_wait_set)];
    socket->type = TYPE_BACKEND;
    socket->fd = sock;
    socket->pos = AddWaitEventToSetEx(rm_wait_set,
                                      WL_SOCKET_READABLE | WL_SOCKET_CLOSED,
                                      sock,
                                      NULL,
                                      socket);
    if (socket->pos != -1) {
        ereport(DEBUG1, (errmsg("backend connection received: fd=%d", sock)));
    }
    else {
        ereport(WARNING, (errmsg("too many backend connections: fd=%d", sock)));
        StreamClose(sock);
    }
}

static inline void
on_frontend(Socket *socket, uint32 events) {
    pgsocket sock;
    SockAddr addr;

    if (!(events & WL_SOCKET_ACCEPT))
        return;

    ereport(DEBUG1,
            (errmsg("Accept frontend connection from: fd=%d", socket->fd)));
    addr.salen = sizeof(addr.addr);
    sock = accept(socket->fd, (struct sockaddr *)&addr.addr, &addr.salen);
    if (sock == PGINVALID_SOCKET) {
        ereport(LOG,
                (errcode_for_socket_access(),
                 errmsg("could not accept new connection: %m")));
        pg_usleep(100000L); // wait 0.1 sec
        return;
    }
    if (Log_connections) {
        int ret;
        char remote_host[NI_MAXHOST];
        char remote_port[NI_MAXSERV];
        remote_host[0] = '\0';
        remote_port[0] = '\0';
        ret = pg_getnameinfo_all(&addr.addr,
                                 addr.salen,
                                 remote_host,
                                 sizeof(remote_host),
                                 remote_port,
                                 sizeof(remote_port),
                                 (log_hostname ? 0 : NI_NUMERICHOST)
                                     | NI_NUMERICSERV);
        if (ret != 0)
            ereport(WARNING,
                    (errmsg_internal("pg_getnameinfo_all() failed: %s",
                                     gai_strerror(ret))));
        ereport(LOG,
                (errmsg("connection received: host=%s port=%s",
                        remote_host,
                        remote_port)));
    }
    if (idle_size == 0 && num_workers < max_worker_processes - 2) {
        BackgroundWorker worker;
        BackgroundWorkerHandle **handle;

        snprintf(worker.bgw_name, BGW_MAXLEN, "rustica-%d", num_workers);
        snprintf(worker.bgw_type, BGW_MAXLEN, "rustica worker");
        worker.bgw_flags =
            BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
        worker.bgw_start_time = BgWorkerStart_ConsistentState;
        worker.bgw_restart_time = BGW_NEVER_RESTART;
        snprintf(worker.bgw_library_name, BGW_MAXLEN, "rustica-wamr");
        snprintf(worker.bgw_function_name, BGW_MAXLEN, "rustica_worker");
        worker.bgw_notify_pid = MyProcPid;
        worker.bgw_main_arg = Int32GetDatum(num_workers);

        handle = NULL;
        for (int i = 0; i < max_worker_processes; i++) {
            if (worker_handles[i] == NULL) {
                handle = &worker_handles[i];
                break;
            }
        }
        Assert(handle != NULL);
        if (RegisterDynamicBackgroundWorker(&worker, handle)) {
            num_workers++;
        }
    }
    StreamClose(sock);
}

static inline void
on_backend(Socket *socket, uint32 events) {
    if (events & WL_SOCKET_CLOSED) {
        ereport(DEBUG1, (errmsg("Socket is closed: fd=%d", socket->fd)));
        DeleteWaitEventEx(rm_wait_set, socket->pos);
        StreamClose(socket->fd);
        memset(socket, 0, sizeof(Socket));
    }
    else if (events & WL_SOCKET_READABLE) {
        char buf[12];
        int i;
        Size received;

        if (socket->read_offset >= 12) {
            ModifyWaitEventEx(rm_wait_set, socket->pos, WL_SOCKET_CLOSED, NULL);
            return;
        }
        received = recv(socket->fd, &buf, 12 - socket->read_offset, 0);
        for (i = 0; i < Min(8 - socket->read_offset, received); i++) {
            if (BACKEND_HELLO[socket->read_offset + i] != buf[i]) {
                ereport(LOG,
                        (errmsg("Bad hello from backend: fd=%d", socket->fd)));
                DeleteWaitEventEx(rm_wait_set, socket->pos);
                StreamClose(socket->fd);
                memset(socket, 0, sizeof(Socket));
                return;
            }
        }
        if (received - i > 0) {
            memcpy(((char *)&socket->worker_pid)
                       + Max(0, socket->read_offset - 8),
                   buf + i,
                   received - i);
            if (socket->read_offset + received == 12) {
                ModifyWaitEventEx(rm_wait_set,
                                  socket->pos,
                                  WL_SOCKET_CLOSED,
                                  NULL);
                Assert(idle_size < total_sockets);
                idle_size++;
                idle_workers[idle_tail] = socket->pos;
                idle_tail = (idle_tail + 1) % total_sockets;
                ereport(DEBUG1,
                        (errmsg("Backend %d is idle", socket->worker_pid)));
            }
        }
        socket->read_offset = (uint8_t)(socket->read_offset + received);
    }
}

static void
on_worker_died() {
    int workers = 0;
    for (int i = 0; i < max_worker_processes; i++) {
        BgwHandleStatus status;
        pid_t pid;
        if (worker_handles[i] != NULL) {
            status = GetBackgroundWorkerPid(worker_handles[i], &pid);
            if (status == BGWH_STOPPED) {
                pfree(worker_handles[i]);
                worker_handles[i] = NULL;
            }
            else {
                workers++;
            }
        }
    }
    if (workers < num_workers) {
        ereport(
            DEBUG1,
            (errmsg("%d rustica workers have exited.", num_workers - workers)));
        num_workers = workers;
    }
    else {
        Assert(workers == num_workers);
    }
}

static void
main_loop() {
    WaitEvent events[MAXLISTEN];
    int nevents;
    Socket *socket;

    for (;;) {
        nevents =
            WaitEventSetWaitEx(rm_wait_set, -1, events, lengthof(events), 0);
        for (int i = 0; i < nevents; i++) {
            socket = (Socket *)events[i].user_data;
            if (events[i].events & WL_LATCH_SET) {
                if (shutdown_requested) {
                    return;
                }
                ResetLatch(MyLatch);
                if (worker_died) {
                    worker_died = false;
                    on_worker_died();
                }
            }
            if (socket->type == TYPE_IPC)
                on_backend_connect(socket, events[i].events);
            if (socket->type == TYPE_FRONTEND)
                on_frontend(socket, events[i].events);
            if (socket->type == TYPE_BACKEND)
                on_backend(socket, events[i].events);
        }
    }
}

static void
teardown() {
    ereport(LOG, (errmsg("rustica master shutting down")));
    pfree(worker_handles);
    pfree(idle_workers);
    FreeWaitEventSetEx(rm_wait_set);
    rm_wait_set = NULL;

    for (int i = 0; i < total_sockets; i++) {
        if (sockets[i].type != TYPE_UNSET) {
            sockets[i].type = TYPE_UNSET;
            StreamClose(sockets[i].fd);
        }
    }

    pfree(sockets);
}

PGDLLEXPORT void
rustica_master() {
    startup();
    main_loop();
    teardown();
}
