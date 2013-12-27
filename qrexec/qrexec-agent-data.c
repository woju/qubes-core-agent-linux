/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2013  Marek Marczykowski-Górecki  <marmarek@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <fcntl.h>
#include <libvchan.h>
#include "qrexec.h"
#include "libqrexec-utils.h"
#include "qrexec-agent.h"

#define VCHAN_BUFFER_SIZE 65536

volatile static int child_exited;
int stdout_msg_type = MSG_DATA_STDOUT;
pid_t child_process_pid;

static void sigchld_handler(int x)
{
	child_exited = 1;
	signal(SIGCHLD, sigchld_handler);
}


void no_colon_in_cmd()
{
	fprintf(stderr,
		"cmdline is supposed to be in user:command form\n");
	exit(1);
}

void do_exec(char *cmd)
{
	char buf[strlen(QUBES_RPC_MULTIPLEXER_PATH) + strlen(cmd) - strlen(RPC_REQUEST_COMMAND) + 1];
	char *realcmd = index(cmd, ':');
	if (!realcmd)
		no_colon_in_cmd();
	/* mark end of username and move to command */
	*realcmd = 0;
	realcmd++;
	/* ignore "nogui:" prefix in linux agent */
	if (strncmp(realcmd, "nogui:", 6) == 0)
		realcmd+=6;
	/* replace magic RPC cmd with RPC multiplexer path */
	if (strncmp(realcmd, RPC_REQUEST_COMMAND " ", strlen(RPC_REQUEST_COMMAND)+1)==0) {
		strcpy(buf, QUBES_RPC_MULTIPLEXER_PATH);
		strcpy(buf + strlen(QUBES_RPC_MULTIPLEXER_PATH), realcmd + strlen(RPC_REQUEST_COMMAND));
		realcmd = buf;
	}
	signal(SIGCHLD, SIG_DFL);
	signal(SIGPIPE, SIG_DFL);

	execl("/bin/su", "su", "-", cmd, "-c", realcmd, NULL);
	perror("execl");
	exit(1);
}

int handle_just_exec(char *cmdline)
{
	int fdn, pid;

    switch (pid = fork()) {
        case -1:
            perror("fork");
            return -1;
        case 0:
            fdn = open("/dev/null", O_RDWR);
            fix_fds(fdn, fdn, fdn);
            do_exec(cmdline);
            perror("execl");
            exit(1);
        default:;
    }
	fprintf(stderr, "executed (nowait) %s pid %d\n", cmdline, pid);
    return 0;
}

void send_exit_code(libvchan_t *data_vchan, int status)
{
	struct msg_header hdr;
	hdr.type = MSG_DATA_EXIT_CODE;
	hdr.len = sizeof(status);
	if (libvchan_send(data_vchan, &hdr, sizeof(hdr)) < 0)
		handle_vchan_error("write hdr");
	if (libvchan_send(data_vchan, &status, sizeof(status)) < 0)
		handle_vchan_error("write status");
	fprintf(stderr, "send exit code %d\n", status);
}

/* handle data from specified FD and send over vchan link
 * Return:
 *  -1 - vchan error occurred
 *  0 - EOF received, do not attempt to access this FD again
 *  1 - some data processed, call it again when buffer space and more data
 *      available
 */
int handle_input(libvchan_t *vchan, int fd, int msg_type)
{
	char buf[MAX_DATA_CHUNK];
    int len;
    struct msg_header hdr;

    hdr.type = msg_type;
    while (libvchan_buffer_space(vchan) > sizeof(struct msg_header)) {
        len = libvchan_buffer_space(vchan)-sizeof(struct msg_header);
        if (len > sizeof(buf))
            len = sizeof(buf);
        len = read(fd, buf, len);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 1;
            else
                return -1;
        }
        hdr.len = len;
        if (libvchan_send(vchan, &hdr, sizeof(hdr)) < 0)
            return -1;

        if (len && !write_vchan_all(vchan, buf, len))
            return -1;

        if (len == 0) {
            close(fd);
            return 0;
        }
    }
    return 1;
}

/* handle data from vchan and send it to specified FD
 * Return:
 *  -2 - remote process terminated, do not send more data to it
 *  -1 - vchan error occurred
 *  0 - EOF received, do not attempt to access this FD again
 *  1 - some data processed, call it again when buffer space and more data
 *      available
 */
int handle_remote_data(libvchan_t *data_vchan, int stdin_fd)
{
	struct msg_header hdr;
    char buf[MAX_DATA_CHUNK];
    int status;

    /* TODO: set stdin_fd to non-blocking mode and handle its buffering */
    while (libvchan_data_ready(data_vchan) > 0) {
        if (libvchan_recv(data_vchan, &hdr, sizeof(hdr)) < 0)
            return -1;
        if (hdr.len > MAX_DATA_CHUNK) {
            fprintf(stderr, "Too big data chunk received: %d > %d\n",
                    hdr.len, MAX_DATA_CHUNK);
            return -1;
        }
        if (!read_vchan_all(data_vchan, buf, hdr.len))
            return -1;

        switch (hdr.type) {
            /* handle both directions because this can be either server or client
             * of VM-VM connection */
            case MSG_DATA_STDIN:
            case MSG_DATA_STDOUT:
                if (stdin_fd < 0)
                    /* discard the data */
                    continue;
                if (hdr.len == 0) {
                    close(stdin_fd);
                    stdin_fd = -1;
                    return 0;
                } else {
                    /* FIXME: use buffered write here to prevent deadlock */
                    if (!write_all(stdin_fd, buf, hdr.len)) {
                        if (errno == EPIPE) {
                            close(stdin_fd);
                            stdin_fd = -1;
                        } else {
                            perror("write");
                        }
                        return 0;
                    }
                }
                break;
            case MSG_DATA_STDERR:
                /* stderr of remote service, log locally */
                if (!write_all(2, buf, hdr.len)) {
                    perror("write");
                    /* only log the error */
                }
                break;
            case MSG_DATA_EXIT_CODE:
                /* remote process exited, so there is no sense to send any data
                 * to it */
                status = *(unsigned int *)buf;
                fprintf(stderr, "Remote service process exited with code %d\n", status);
                return -2;
        }
    }
    return 1;
}

void process_child_io(libvchan_t *data_vchan,
        int stdin_fd, int stdout_fd, int stderr_fd)
{
    fd_set rdset, wrset;
    int vchan_fd;
    sigset_t selectmask;
    int child_process_status = -1;
    int ret, max_fd;
    struct timespec zero_timeout = { 0, 0 };

    sigemptyset(&selectmask);
    sigaddset(&selectmask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &selectmask, NULL);
    sigemptyset(&selectmask);

    set_nonblock(stdout_fd);
    set_nonblock(stderr_fd);

    while (1) {
        if (child_exited) {
            pid_t pid;
            int status;
            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                if (pid == child_process_pid) {
                    child_process_status = WEXITSTATUS(status);
                    if (stdin_fd >= 0) {
                        close(stdin_fd);
                        stdin_fd = -1;
                    }
                }
            }
            child_exited = 0;
        }

        /* if all done, exit the loop */
        if ((!child_process_pid || child_process_status > -1) &&
                stdin_fd == -1 && stdout_fd == -1 && stderr_fd == -1) {
            if (child_process_status > -1) {
                send_exit_code(data_vchan, child_process_status);
            }
            break;
        }
        /* otherwise handle the events */

        FD_ZERO(&rdset);
        FD_ZERO(&wrset);
        max_fd = -1;
        vchan_fd = libvchan_fd_for_select(data_vchan);
        if (libvchan_buffer_space(data_vchan) > sizeof(struct msg_header)) {
            if (stdout_fd >= 0) {
                FD_SET(stdout_fd, &rdset);
                if (stdout_fd > max_fd)
                    max_fd = stdout_fd;
            }
            if (stderr_fd >= 0) {
                FD_SET(stderr_fd, &rdset);
                if (stderr_fd > max_fd)
                    max_fd = stderr_fd;
            }
        }
        FD_SET(vchan_fd, &rdset);
        if (vchan_fd > max_fd)
            max_fd = vchan_fd;

        if (libvchan_data_ready(data_vchan) > 0) {
            /* check for other FDs, but exit immediately */
            ret = pselect(max_fd + 1, &rdset, &wrset, NULL, &zero_timeout, &selectmask);
        } else
            ret = pselect(max_fd + 1, &rdset, &wrset, NULL, NULL, &selectmask);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            else {
                perror("pselect");
                /* TODO */
                break;
            }
        }

        /* clear event pending flag */
        if (FD_ISSET(vchan_fd, &rdset)) {
            if (libvchan_wait(data_vchan) < 0)
                handle_vchan_error("wait");
        }

        if (stdout_fd >= 0 && FD_ISSET(stdout_fd, &rdset)) {
            switch (handle_input(data_vchan, stdout_fd, stdout_msg_type)) {
                case -1:
                    handle_vchan_error("send");
                    break;
                case 0:
                    stdout_fd = -1;
                    break;
            }
        }
        if (stderr_fd >= 0 && FD_ISSET(stderr_fd, &rdset)) {
            switch (handle_input(data_vchan, stderr_fd, MSG_DATA_STDERR)) {
                case -1:
                    handle_vchan_error("send");
                    break;
                case 0:
                    stderr_fd = -1;
                    break;
            }
        }
        /* handle_remote_data will check if any data is available */
        switch (handle_remote_data(data_vchan, stdin_fd)) {
            case -1:
                handle_vchan_error("read");
                break;
            case 0:
                stdin_fd = -1;
                break;
            case -2:
                /* remote process exited, no sense in sending more data to it */
                close(stdout_fd);
                stdout_fd = -1;
                close(stderr_fd);
                stderr_fd = -1;
                break;
        }
    }
}

pid_t handle_new_process(int type, int connect_domain, int connect_port,
        char *cmdline, int cmdline_len)
{
    struct service_params *svc_params = (struct service_params*)cmdline;
    libvchan_t *data_vchan;
    pid_t pid;
    int stdin_fd, stdout_fd, stderr_fd;

    if (type == MSG_SERVICE_CONNECT) {
        if (cmdline_len != sizeof(*svc_params)) {
            fprintf(stderr, "Invalid MSG_SERVICE_CONNECT packet (cmdline len %d)\n", cmdline_len);
            return -1;
        }
        sscanf(cmdline, "%d %d %d", &stdin_fd, &stdout_fd, &stderr_fd);
    }

    switch (pid=fork()){
        case -1:
            perror("fork");
            return -1;
        case 0:
            break;
        default:
            if (type == MSG_SERVICE_CONNECT) {
                /* no longer needed in parent process */
                close(stdin_fd);
                close(stdout_fd);
                close(stderr_fd);
            }
            return pid;
    }

    /* child process */
    if (type == MSG_SERVICE_CONNECT) {
        data_vchan = libvchan_server_init(connect_domain, connect_port,
                VCHAN_BUFFER_SIZE, VCHAN_BUFFER_SIZE);
        if (data_vchan)
            libvchan_wait(data_vchan);
    } else {
        data_vchan = libvchan_client_init(connect_domain, connect_port);
    }
    if (!data_vchan) {
        fprintf(stderr, "Data vchan connection failed\n");
        exit(1);
    }
    handle_handshake(data_vchan);

    signal(SIGCHLD, sigchld_handler);

    switch (type) {
        case MSG_JUST_EXEC:
            send_exit_code(data_vchan, handle_just_exec(cmdline));
            libvchan_close(data_vchan);
            break;
        case MSG_EXEC_CMDLINE:
            do_fork_exec(cmdline, &pid, &stdin_fd, &stdout_fd, &stderr_fd);
            fprintf(stderr, "executed %s pid %d\n", cmdline, pid);
            child_process_pid = pid;
            process_child_io(data_vchan, stdin_fd, stdout_fd, stderr_fd);
            break;
        case MSG_SERVICE_CONNECT:
            child_process_pid = 0;
            stdout_msg_type = MSG_DATA_STDIN;
            process_child_io(data_vchan, stdin_fd, stdout_fd, stderr_fd);
            break;
    }
    exit(0);
    /* suppress warning */
    return 0;
}
