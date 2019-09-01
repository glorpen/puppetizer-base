#include "common.h"

#include <stdio.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "control.h"
#include "client.h"
#include "service.h"
#include "log.h"
#include "spawn.h"

static bool is_halting = FALSE;
static pid_t boot_pid;
static pthread_t halt_thread = 0;

void init_detach_from_terminal()
{
    if (ioctl(STDIN_FILENO, TIOCNOTTY) == -1) {
        log_debug(
            "Unable to detach from controlling tty (errno=%d %s).",
            errno,
            strerror(errno)
        );
    } else {
        /*
        * When the session leader detaches from its controlling tty via
        * TIOCNOTTY, the kernel sends SIGHUP and SIGCONT to the process
        * group (getsid(0) == getpid()).
        * But since we block all signals on start, we shouldn't receive
        * it.
        */
        
        log_debug("Detached from controlling tty");
    }
}

static pid_t init_apply()
{
    pid_t apply_pid = spawn1(PUPPETIZER_APPLY);
    if (apply_pid == -1) {
        fatal("Failed to start puppet apply", ERROR_SPAWN_FAILED);
    }
    return apply_pid;
}

bool init_handle_client_command(control_command_t *command, int socket)
{
    control_reponse_t ret = CMD_RESPONSE_ERROR;
    struct service *svc = service_find_by_name(command->name);

    if (svc == NULL) {
        log_warning("Service %s was not found", command->name);
    } else {
        log_debug("cmd type: %d", command->type);
        switch (command->type) {
            case CMD_START:
                if (is_halting) {
                    log_warning("Ignoring service start request");
                } else {
                    ret = service_start(svc)?CMD_RESPONSE_OK:CMD_RESPONSE_FAILED;
                }
                break;
            case CMD_STOP:
                if (is_halting) {
                    log_warning("Ignoring service stop request");
                } else {
                    ret = service_stop(svc)?CMD_RESPONSE_OK:CMD_RESPONSE_FAILED;
                }
                //TODO: async loop / select for checking if service is stopped and blocking client?
                break;
            case CMD_STATUS:
                ret = svc->state<<4 | CMD_RESPONSE_STATE;
                log_debug("resp: %d", ret);
                break;
            //TODO: CMD_STOP_BLOCK
        }
    }

    return send(socket, &ret, sizeof(control_reponse_t), 0) == sizeof(control_reponse_t);
}

/**
 * Blocks all signals.
 * SIGCHLD, SIGTERM and SIGHUP will be handled by init_loop.
 */
static void init_setup_signals()
{
    sigset_t all_signals;
    sigfillset(&all_signals);
    sigprocmask(SIG_BLOCK, &all_signals, NULL);
}

void init_halt()
{
    uint8_t i;

    if (is_halting) return;

    is_halting = TRUE;
    
    log_debug("Running halt action");
    // run puppet-apply with halt option to stop services
    int ret = spawn2_wait(PUPPETIZER_APPLY, "halt");
    if (ret != 0) {
        log_error("Puppet halt failed with exitcode %d", ret);
    }

    // stop any services that are not stoping
    i = service_stop_all();
    if (i>0) {
        log_warning("Stopping %d outstanding services.", i);
    }
}

static void init_halt_thread()
{
    int ret = pthread_create(&halt_thread, NULL, (void * (*)(void *))init_halt, NULL);

    if (ret != 0) {
        fatal("Halt thread creation failed with %d", ERROR_THREAD_FAILED, ret);
    }
}


static bool init_handle_signal(const struct signalfd_siginfo *info)
{
    int status;
    struct service* svc;
    service_state_t svc_state;
    int retval;

    switch(info->ssi_signo){
        case SIGCHLD:
            waitpid(info->ssi_pid, &status, WNOHANG);
            retval = spawn_retval(status);

            if (boot_pid == info->ssi_pid) {
                if (retval == 0) {
                    log_info("Booting completed");
                } else {
                    log_error("Boot script failed");
                    return FALSE;
                }
            }

            svc = service_find_by_pid(info->ssi_pid);
            if (svc == NULL) {
                log_info("Reaped zombie PID:%d", info->ssi_pid);
            } else {
                svc_state = svc->state;
                service_set_down(svc);

                log_error("Service %s exitted with code %d", svc->name, retval);

                // halt if service exitted unexpectedly or it failed wither errorcode
                if (svc_state != STATE_PENDING_DOWN || retval != 0) {
                    log_debug("Service exitted with code %d when had status %d, halting", retval, svc_state);
                    init_halt_thread();
                }
            }
            break;
        case SIGTERM:
        case SIGINT:
            if (is_halting) {
                log_warning("Ignoring halting request");
            } else {
                log_debug("Halting");
                init_halt_thread();
            }
            break;
        case SIGHUP:
            if (is_halting) {
                log_warning("Ignoring puppet aply request");
            } else {
                log_debug("Running puppet apply");
                init_apply();
            }
            break;
    }

    return TRUE;
}

static int init_create_signal_fd()
{
    int fd_signal;
    sigset_t sigmask;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGTERM);
    sigaddset(&sigmask, SIGINT);
    sigaddset(&sigmask, SIGCHLD);
    sigaddset(&sigmask, SIGHUP);

    // make fd for reading required signals
    fd_signal = signalfd(-1, &sigmask, 0);
    if (fd_signal == -1) {
        fatal_errno("Failed to create signal descriptor", ERROR_FD_FAILED);
    }

    return fd_signal;
}

static int init_loop()
{
    struct epoll_event ev, events[10];
    int changes = 0;
    uint8_t buffer[sizeof(struct signalfd_siginfo)+128];
    int exit_code = 0;
    status_t status;

    int fd_signal, fd_epoll, fd_control, fd_client;
    uint16_t i;
    struct sockaddr_un saddr_client;
    socklen_t peer_addr_size = sizeof(struct sockaddr_un);
    bool errored;
    
    // make fd for reading required signals
    fd_signal = init_create_signal_fd();

    // make fd for control socket
    status = control_listen(&fd_control, 5);
    if (status != S_OK) {
        fatal("Failed to create listening socket", ERROR_SOCKET_FAILED);
    }
    
    // setup epoll
    fd_epoll = epoll_create1(0);
    if (fd_epoll == -1) {
        fatal_errno("Failed to setup polling", ERROR_EPOLL_FAILED);
    }

    ev.events = EPOLLIN;
    
    ev.data.fd = fd_signal;
    if (epoll_ctl(fd_epoll, EPOLL_CTL_ADD, fd_signal, &ev) == -1) {
        fatal_errno("Failed to setup signal polling", ERROR_EPOLL_FAILED);
    }
    ev.data.fd = fd_control;
    if (epoll_ctl(fd_epoll, EPOLL_CTL_ADD, fd_control, &ev) == -1) {
        fatal_errno("Failed to setup control socket polling", ERROR_EPOLL_FAILED);
    }
    
    for (;;) {
        changes = epoll_wait(fd_epoll, events, 10, 500);
        if (changes == -1) {
            fatal_errno("Could not wait for events", ERROR_EPOLL_WAIT);
        }

        log_debug("loop");

        for (i = 0; i < changes; i++) {
            errored = FALSE;

            if (events[i].data.fd == fd_signal) {
                // there should be sizeof(struct signalfd_siginfo) bytes available to read
                if (read(fd_signal, buffer, sizeof(struct signalfd_siginfo)) != sizeof(struct signalfd_siginfo)) {
                    log_error("Bad signal size info read");
                    exit_code = ERROR_EPOLL_SIGNAL_MESSAGE;
                    break;
                }
                if (!init_handle_signal((struct signalfd_siginfo*)buffer)) {
                    exit_code = ERROR_EPOLL_SIGNAL;
                    break;
                }
            }
            // handle init client
            else if (events[i].data.fd == fd_control) {
                fd_client = accept(fd_control, (struct sockaddr *) &saddr_client, &peer_addr_size);
                //setnonblocking(fd_client);
                ev.data.fd = fd_client;
                if (epoll_ctl(fd_epoll, EPOLL_CTL_ADD, fd_client, &ev) == -1) {
                    // TODO: strerr, errno
                    log_error("Failed to setup control client socket polling");
                    exit_code = ERROR_SOCKET_FAILED;
                    break;
                }
            }
            else {
                // client connections
                status = control_read_command(events[i].data.fd, (control_command_t*)buffer);
                if (status == S_SOCKET_EOF) {
                    // socket is closed
                    errored = TRUE;
                } else {
                    if (status == S_OK) {
                        if (!init_handle_client_command((control_command_t*)buffer, events[i].data.fd)) {
                            log_warning("Failed to handle client message");
                            errored = TRUE;
                        }
                    } else {
                        log_warning("Failed to read client message");
                        errored = TRUE;
                    }
                }

                if (errored) {
                    ev.data.fd = events[i].data.fd;
                    if (epoll_ctl(fd_epoll, EPOLL_CTL_DEL, events[i].data.fd, &ev) == -1) {
                        fatal_errno("Failed to remove client socket polling", ERROR_EPOLL_FAILED);
                    }
                    shutdown(events[i].data.fd, SHUT_RDWR);
                }
            }
        }

        if (is_halting) {
            if (service_count_by_state(STATE_DOWN, TRUE) == 0) {
                log_info("No more services running, exitting");
            }
            break;
        }
    }

    shutdown(fd_control, SHUT_RDWR);
    unlink(PUPPETIZER_CONTROL_SOCKET);

    if (halt_thread != 0) {
        log_debug("Waiting for halt thread to exit");
        pthread_join(halt_thread, NULL);
    }
   
   return exit_code;
}

int init_boot()
{
    boot_pid = init_apply();
    if (boot_pid == -1) {
        fatal("Could not start boot script", ERROR_BOOT_FAILED);
    }

    // loop socket, check apply status, exit if failed
    return init_loop();
    // and signalfd for signals
    // for (;;) {
    //     int signum;
    //     sigwait(&all_signals, &signum);
    //     handle_signal(signum);
    // }
}

int main(int argc, char** argv)
{
    if (argc == 1) {
        log_info("Running init");
        init_setup_signals();
        service_create_all();
        init_detach_from_terminal();
        return init_boot();
    } else {
        return client_main(argc, argv);
    }
}

/*
boot:
    puppet_apply
        odpala sv start nginx => /opt/puppetizer/services/nginx.start
            sv oczekuje sekundę by zobaczyć czy serwis wstał

w przyupadku gdy serwis przestanie działać, odpalamy procedurę halt z exitcode=1

sighup
    odpala puppet_apply
sigterm
    puppet_apply env=halt
        odpala sv nginx stop => /opt/puppetizer/services/nginx.stop <pid>
            init nie czeka na wyjście nginxa
            sv czeka aż stop zadziała
    gdy puppet_apply się zakończy (jakkolwiek), zostanie odpalone "sv * stop"
    init czeka aż wszystkie serwisy sie zakończą, nie przyjmuje więcej komend
    exit 0

nasłuchiwanie na sygnały, zwłaszcza SIGCHLD z waitpid nohang dla "zombie reaper"

musi też odpowiadać na sv nginx status
*/
