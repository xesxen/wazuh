/* Copyright (C) 2009 Trend Micro Inc.
 * All right reserved.
 *
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
 */

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#elif defined(__MACH__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <sys/types.h>
#include <sys/event.h>
#endif /* __linux__ */

#include "shared.h"
#include "os_net/os_net.h"
#include "remoted.h"

/* Global variables */
int sender_pool;

// Message handler thread
static void * rem_handler_main(__attribute__((unused)) void * args);

// Key reloader thread
void * rem_keyupdate_main(__attribute__((unused)) void * args);

/* Handle each message received */
static void HandleSecureMessage(char *buffer, int recv_b, struct sockaddr_in *peer_info, int sock_client);

// Close and remove socket from keystore
int _close_sock(keystore * keys, int sock);

/* Handle secure connections */
void HandleSecure()
{
    const int protocol = logr.proto[logr.position];
    int sock_client;
    int n_events = 0;
    char buffer[OS_MAXSTR + 1];
    ssize_t recv_b;
    uint32_t length;
#ifndef WIN32
    struct sockaddr_in6 peer_info;
#else
    struct sockaddr_in peer_info;
#endif

#if defined(__MACH__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    const struct timespec TS_ZERO = { 0, 0 };
    struct timespec ts_timeout = { EPOLL_MILLIS / 1000, (EPOLL_MILLIS % 1000) * 1000000 };
    struct timespec *p_timeout = EPOLL_MILLIS < 0 ? NULL : &ts_timeout;
    int kqueue_fd = 0;
    struct kevent request;
    struct kevent *events = NULL;
#elif defined(__linux__)
    int epoll_fd = 0;
    struct epoll_event request = { .events = 0 };
    struct epoll_event *events = NULL;
#endif /* __MACH__ || __FreeBSD__ || __OpenBSD__ */

    /* Initialize manager */
    manager_init();

    // Initialize messag equeue
    rem_msginit(logr.queue_size);

    /* Create Active Response forwarder thread */
    w_create_thread(update_shared_files, NULL);

    /* Create Active Response forwarder thread */
    w_create_thread(AR_Forward, NULL);

    // Create Request listener thread
    w_create_thread(req_main, NULL);

    // Create State writer thread
    w_create_thread(rem_state_main, NULL);

    /* Create wait_for_msgs threads */

    {
        int i;
        sender_pool = getDefine_Int("remoted", "sender_pool", 1, 64);

        mdebug2("Creating %d sender threads.", sender_pool);

        for (i = 0; i < sender_pool; i++) {
            w_create_thread(wait_for_msgs, NULL);
        }
    }

    // Create message handler thread pool
    {
        int worker_pool = getDefine_Int("remoted", "worker_pool", 1, 16);

        while (worker_pool > 0) {
            w_create_thread(rem_handler_main, NULL);
            worker_pool--;
        }
    }

    /* Connect to the message queue
     * Exit if it fails.
     */
    if ((logr.m_queue = StartMQ(DEFAULTQUEUE, WRITE)) < 0) {
        merror_exit(QUEUE_FATAL, DEFAULTQUEUE);
    }

    minfo(AG_AX_AGENTS, MAX_AGENTS);

    /* Read authentication keys */
    minfo(ENC_READ);
    OS_ReadKeys(&keys, 1, 0, 0);
    OS_StartCounter(&keys);

    // Key reloader thread
    w_create_thread(rem_keyupdate_main, NULL);

    /* Set up peer size */
    logr.peer_size = sizeof(peer_info);

    /* Initialize some variables */
    memset(buffer, '\0', OS_MAXSTR + 1);

    if (protocol == TCP_PROTO) {
#if defined(__MACH__) || defined(__FreeBSD__) || defined(__OpenBSD__)
        os_calloc(MAX_EVENTS, sizeof(struct kevent), events);
        kqueue_fd = kqueue();

        if (kqueue_fd < 0) {
            merror_exit(KQUEUE_ERROR);
        }

        EV_SET(&request, logr.sock, EVFILT_READ, EV_ADD, 0, 0, 0);

        if (kevent(kqueue_fd, &request, 1, NULL, 0, &TS_ZERO) < 0) {
            merror_exit("kevent when adding listening socket: %s (%d)", strerror(errno), errno);
        }
#elif defined(__linux__)
        os_calloc(MAX_EVENTS, sizeof(struct epoll_event), events);
        epoll_fd = epoll_create(MAX_EVENTS);

        if (epoll_fd < 0) {
            merror_exit(EPOLL_ERROR);
        }

        request.events = EPOLLIN;
        request.data.fd = logr.sock;

        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, logr.sock, &request) < 0) {
            merror_exit("epoll when adding listening socket: %s (%d)", strerror(errno), errno);
        }
#endif /* __MACH__ || __FreeBSD__ || __OpenBSD__ */
    }

    while (1) {
        /* Receive message  */
        if (protocol == TCP_PROTO) {
#if defined(__MACH__) || defined(__FreeBSD__) || defined(__OpenBSD__)
            n_events = kevent(kqueue_fd, NULL, 0, events, MAX_EVENTS, p_timeout);
#elif defined(__linux__)
            n_events = epoll_wait(epoll_fd, events, MAX_EVENTS, EPOLL_MILLIS);
#endif /* __MACH__ || __FreeBSD__ || __OpenBSD__ */

            if (n_events < 0) {
                if (errno != EINTR) {
                    merror("Waiting for connection: %s (%d)", strerror(errno), errno);
                    sleep(1);
                }

                continue;
            }

            int i;
            for (i = 0; i < n_events; i++) {
#if defined(__MACH__) || defined(__FreeBSD__) || defined(__OpenBSD__)
                int fd = events[i].ident;
#elif defined(__linux__)
                int fd = events[i].data.fd;
#else
                int fd = 0;
#endif /* __MACH__ || __FreeBSD__ || __FreeBSD__ */
                if (fd == logr.sock) {
                    sock_client = accept(logr.sock, (struct sockaddr *)&peer_info, &logr.peer_size);
                    if (sock_client < 0) {
                        merror_exit(ACCEPT_ERROR);
                    }

                    rem_inc_tcp();
#ifndef WIN32
                    {
                        char srcip[IPSIZE + 1] = "UNKNOWN";
                        getnameinfo((struct sockaddr *)&peer_info, sizeof(peer_info), srcip, IPSIZE, NULL, 0, NI_NUMERICHOST);
                        mdebug1("New TCP connection at %s.", srcip);
                    }
#else
                    mdebug1("New TCP connection at %s.", inet_ntoa(peer_info.sin_addr));
#endif
#if defined(__MACH__) || defined(__FreeBSD__) || defined(__OpenBSD__)
                    EV_SET(&request, sock_client, EVFILT_READ, EV_ADD, 0, 0, 0);

                    if (kevent(kqueue_fd, &request, 1, NULL, 0, &TS_ZERO) < 0) {
                        merror("kevent when adding connected socket: %s (%d)", strerror(errno), errno);
                        _close_sock(&keys, sock_client);
                    }
#elif defined(__linux__)
                    request.data.fd = sock_client;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_client, &request) < 0) {
                        merror("epoll when adding connected socket: %s (%d)", strerror(errno), errno);
                        _close_sock(&keys, sock_client);
                    }
#endif /* __MACH__ || __FreeBSD__ || __OpenBSD__ */
                } else {
                    sock_client = fd;
                    recv_b = recv(sock_client, (char*)&length, sizeof(length), MSG_WAITALL);
                    length = wnet_order(length);

                    if (getpeername(sock_client, (struct sockaddr *)&peer_info, &logr.peer_size) < 0) {
                        switch (errno) {
                            case ENOTCONN:
                                mdebug1("TCP peer was disconnected.");
                                break;
                            default:
                                merror("Couldn't get the remote peer information: %s [%d]", strerror(errno), errno);
                        }

                        _close_sock(&keys, sock_client);
                        continue;
                    }

                    /* Nothing received */
                    if (recv_b <= 0 || length > OS_MAXSTR) {
#ifndef WIN32
                        char srcip[IPSIZE + 1] = "UNKNOWN";
                        getnameinfo((struct sockaddr *)&peer_info, sizeof(peer_info), srcip, IPSIZE, NULL, 0, NI_NUMERICHOST);
                        strncpy(srcip, "UNKNOWN", sizeof(srcip));
#else
                        char * srcip = inet_ntoa(peer_info.sin_addr);
#endif
                        switch (recv_b) {
                        case -1:
                            if (errno == ENOTCONN) {
                                mdebug1("TCP peer at %s disconnected (ENOTCONN).", srcip);
                            } else {
                                merror("TCP peer at %s: %s (%d)", srcip, strerror(errno), errno);
                            }

                            break;

                        case 0:
                            mdebug1("TCP peer at %s disconnected.", srcip);
                            break;

                        default:
                            // length > OS_MAXSTR
                            merror("Too big message size from %s.", srcip);
                        }
#ifdef __linux__
                        /* Kernel event is automatically deleted when closed */
                        request.data.fd = sock_client;
                        if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, sock_client, &request) < 0) {
                            merror("epoll when deleting connected socket: %s (%d)", strerror(errno), errno);
                        }
#endif /* __linux__ */

                        _close_sock(&keys, sock_client);
                        continue;
                    }

                    recv_b = recv(sock_client, buffer, length, MSG_WAITALL);

                    if (recv_b != (ssize_t)length) {
#ifndef WIN32
                        {
                            char srcip[IPSIZE + 1] = "UNKNOWN";
                            getnameinfo((struct sockaddr *)&peer_info, sizeof(peer_info), srcip, IPSIZE, NULL, 0, NI_NUMERICHOST);
                            merror("Incorrect message size from %s: expecting %u, got %zd", srcip, length, recv_b);
                        }
#else
                        merror("Incorrect message size from %s: expecting %u, got %zd", inet_ntoa(peer_info.sin_addr), length, recv_b);
#endif

#ifdef __linux__
                        request.data.fd = sock_client;
                        if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, sock_client, &request) < 0) {
                            merror("epoll when deleting connected socket: %s (%d)", strerror(errno), errno);
                        }
#endif /* __linux__ */
                        _close_sock(&keys, sock_client);
                    } else {
                        rem_msgpush(buffer, recv_b, (struct sockaddr_in *) &peer_info, sock_client);
                    }
                }
            }
        } else {
            recv_b = recvfrom(logr.sock, buffer, OS_MAXSTR, 0, (struct sockaddr *)&peer_info, &logr.peer_size);

            /* Nothing received */
            if (recv_b <= 0) {
                continue;
            } else {
                rem_msgpush(buffer, recv_b, &peer_info, -1);
            }
        }
    }
}

// Message handler thread
void * rem_handler_main(__attribute__((unused)) void * args) {
    message_t * message;
    char buffer[OS_MAXSTR + 1] = "";
    mdebug1("Message handler thread started.");

    while (1) {
        message = rem_msgpop();
        memcpy(buffer, message->buffer, message->size);
        HandleSecureMessage(buffer, message->size, &message->addr, message->sock);
        rem_msgfree(message);
    }

    return NULL;
}

// Key reloader thread
void * rem_keyupdate_main(__attribute__((unused)) void * args) {
    int seconds;

    mdebug1("Key reloader thread started.");
    seconds = getDefine_Int("remoted", "keyupdate_interval", 1, 3600);

    while (1) {
        mdebug2("Checking for keys file changes.");
        check_keyupdate();
        sleep(seconds);
    }
}

static void HandleSecureMessage(char *buffer, int recv_b, struct sockaddr_in *peer_info, int sock_client) {
    int agentid;
    int protocol = logr.proto[logr.position];
    char cleartext_msg[OS_MAXSTR + 1];
    char srcmsg[OS_FLSIZE + 1];
    char srcip[IPSIZE + 1];
    char agname[KEYSIZE + 1];
    char *tmp_msg;
    size_t msg_length;

    /* Set the source IP */
#ifndef WIN32
    int ret;
    if(ret = getnameinfo((struct sockaddr *)peer_info, sizeof(struct sockaddr_in6), srcip, sizeof(srcip), NULL, 0, NI_NUMERICHOST) != 0) {
        strncpy(srcip, "UNKNOWN", sizeof(srcip));
    }
#else
    strncpy(srcip, inet_ntoa(peer_info->sin_addr), IPSIZE);
    srcip[IPSIZE] = '\0';
#endif

    /* Initialize some variables */
    memset(cleartext_msg, '\0', OS_MAXSTR + 1);
    memset(srcmsg, '\0', OS_FLSIZE + 1);
    tmp_msg = NULL;

    /* Get a valid agent id */
    if (buffer[0] == '!') {
        tmp_msg = buffer;
        tmp_msg++;

        /* We need to make sure that we have a valid id
         * and that we reduce the recv buffer size
         */
        while (isdigit((int)*tmp_msg)) {
            tmp_msg++;
            recv_b--;
        }

        if (*tmp_msg != '!') {
            merror(ENCFORMAT_ERROR, "(unknown)", srcip);

            if (sock_client >= 0)
                _close_sock(&keys, sock_client);

            return;
        }

        *tmp_msg = '\0';
        tmp_msg++;
        recv_b -= 2;

        key_lock_read();
        agentid = OS_IsAllowedDynamicID(&keys, buffer + 1, srcip);

        if (agentid == -1) {
            int id = OS_IsAllowedID(&keys, buffer + 1);

            if (id < 0) {
                strncpy(agname, "unknown", sizeof(agname));
            } else {
                strncpy(agname, keys.keyentries[id]->name, sizeof(agname));
            }

            key_unlock();

            agname[sizeof(agname) - 1] = '\0';

            mwarn(ENC_IP_ERROR, buffer + 1, srcip, agname);

            if (sock_client >= 0)
                _close_sock(&keys, sock_client);

            return;
        }
    } else {
        key_lock_read();
        agentid = OS_IsAllowedIP(&keys, srcip);

        if (agentid < 0) {
            key_unlock();
            mwarn(DENYIP_WARN, srcip);

            if (sock_client >= 0)
                _close_sock(&keys, sock_client);

            return;
        }
        tmp_msg = buffer;
    }

    /* Decrypt the message */

    tmp_msg = ReadSecMSG(&keys, tmp_msg, cleartext_msg, agentid, recv_b - 1, &msg_length, srcip);

    if (tmp_msg == NULL) {

        /* If duplicated, a warning was already generated */
        key_unlock();
        return;
    }

    /* Check if it is a control message */
    if (IsValidHeader(tmp_msg)) {
        int r = 2;

        key_unlock();
        key_lock_write();

        /* We need to save the peerinfo if it is a control msg */

        memcpy(&keys.keyentries[agentid]->peer_info, peer_info, logr.peer_size);
        r = (protocol == TCP_PROTO) ? OS_AddSocket(&keys, agentid, sock_client) : 2;
        keys.keyentries[agentid]->rcvd = time(0);
        key_unlock();

        switch (r) {
        case 0:
            merror("Couldn't add TCP socket to keystore.");
            break;
        case 1:
            mdebug2("TCP socket already in keystore.");
            break;
        default:
            ;
        }

        save_controlmsg((unsigned)agentid, tmp_msg, msg_length - 3);
        rem_inc_ctrl_msg();
        return;
    }

    /* Generate srcmsg */

    snprintf(srcmsg, OS_FLSIZE, "[%s] (%s) %s", keys.keyentries[agentid]->id,
             keys.keyentries[agentid]->name, keys.keyentries[agentid]->ip->ip);

    key_unlock();

    /* If we can't send the message, try to connect to the
     * socket again. If it not exit.
     */
    if (SendMSG(logr.m_queue, tmp_msg, srcmsg,
                SECURE_MQ) < 0) {
        merror(QUEUE_ERROR, DEFAULTQUEUE, strerror(errno));

        if ((logr.m_queue = StartMQ(DEFAULTQUEUE, WRITE)) < 0) {
            merror_exit(QUEUE_FATAL, DEFAULTQUEUE);
        }
    } else {
        rem_inc_evt();
    }
}

// Close and remove socket from keystore
int _close_sock(keystore * keys, int sock) {
    int retval;

    key_lock_write();
    retval = OS_DeleteSocket(keys, sock);
    key_unlock();

    if (close(sock) == 0) {
        rem_dec_tcp();
    }

    return retval;
}
