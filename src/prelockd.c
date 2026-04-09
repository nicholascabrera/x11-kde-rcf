#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <syslog.h>

#define TRUE 1
#define FALSE 0

typedef enum {
    STATE_ACQUIRE_INHIBITOR,
    STATE_IDLE,
    STATE_LOCK_REQUESTED,
    STATE_LOCK_CONFIRMED,
    STATE_RELEASING_INHIBITOR
} state_t;

typedef enum {
    SIGNAL_ACQUIRE_INHIBITOR,
    SIGNAL_PREPARE_FOR_SLEEP,
    SIGNAL_SESSION_LOCKED,
    SIGNAL_TIMEOUT,
    SIGNAL_RESUME
} signal_t;

typedef struct {
    int lock_delay_s;
    int lock_delay_ms;
    int timeout_s;
    int timeout_ms;
    int verbose;
} config_t;

#define MAX_SIGNALS 16

typedef struct {
    signal_t signals[MAX_SIGNALS];
    int head;
    int tail;
} signal_queue_t;

typedef struct {
    sd_bus *system_bus;
    sd_bus *user_bus;
    int inhibitor_lock_fd;
    
    state_t state;
    config_t config;
    signal_queue_t queue;

    const char *session_id;
    const char *session_path;

    uint64_t lock_request_time;
} application_context_t;

static void pvprintf_state(config_t *c, const char *s) {
    if (c->verbose) {
        syslog(LOG_INFO, "[STATE] %s", s);
        fprintf(stdout, "[STATE] %s\n", s);
    }
}

static void pvprintf_info(config_t *c, const char *s) {
    if (c->verbose) {
        syslog(LOG_INFO, "[INFO] %s", s);
        fprintf(stdout, "[INFO] %s\n", s);
    }
}

static void pvprintf_error(config_t *c, const char *s, int error) {
    if (c->verbose) {
        syslog(LOG_ERR, "[ERROR] %s", s);
        fprintf(stderr, "[ERROR] %s: %s\n", s, strerror(error));
    }
}

// Function to check if the queue is empty
static int no_signals(signal_queue_t *q) {
    return q->head == q->tail;
}

// Function to check if the queue is full
static int max_signals_received(signal_queue_t *q) {
    return ((q->tail + 1) % MAX_SIGNALS) == q->head;
}

// Function to add an element to the queue (Enqueue
// operation)
static void enqueue_signal(signal_queue_t *q, signal_t s) {
    if (max_signals_received(q)) {
        // printf("Signal queue is full\n");
        return;
    }
    q->signals[q->tail] = s;
    q->tail = (q->tail + 1) % MAX_SIGNALS;
}

// Function to remove an element from the queue (Dequeue
// operation)
static int dequeue_signal(signal_queue_t *q, signal_t *s) {
    if (no_signals(q)) {
        // printf("Signal queue is empty\n");
        return 0;
    }

    *s = q->signals[q->head];
    q->head = (q->head + 1) % MAX_SIGNALS;
    return 1;
}

static int properties_changed_handler(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    application_context_t *app = userdata;
    const char *interface;
    int response;

    response = sd_bus_message_read(m, "s", &interface);
    if (response < 0) {
        return response;
    }

    // Only care about Session interface
    if (strcmp(interface, "org.freedesktop.login1.Session") != 0) {
        return 0;
    }

    // Enter dict: a{sv}
    response = sd_bus_message_enter_container(m, 'a', "{sv}");
    if (response < 0) {
        return response;
    }

    while ((response = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        const char *property;
        response = sd_bus_message_read(m, "s", &property);
        if (response < 0) {
            return response;
        }

        if (strcmp(property, "LockedHint") == 0) {
            int locked;

            response = sd_bus_message_enter_container(m, 'v', "b");
            if (response < 0) {
                return response;
            }

            response = sd_bus_message_read(m, "b", &locked);
            if (response < 0) {
                return response;
            }

            sd_bus_message_exit_container(m); // exit variant

            if (locked) {
                pvprintf_info(&app->config, "Lock confirmed via signal");

                enqueue_signal(&app->queue, SIGNAL_SESSION_LOCKED);
            }

        } else {
            // skip value
            sd_bus_message_skip(m, "v");
        }

        sd_bus_message_exit_container(m); // exit dict entry
    }

    sd_bus_message_exit_container(m); // exit dict
    return 0;
}

static int prepare_for_sleep_handler(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    application_context_t *app = userdata;
    int sleeping;
    int response;

    printf("[DEBUG] PrepareForSleep signal received\n");

    response = sd_bus_message_read(m, "b", &sleeping);
    if (response < 0) {
        pvprintf_error(&app->config, "Failed to parse parameters", -response);
        return response;
    }

    if (sleeping) {
        enqueue_signal(&app->queue, SIGNAL_PREPARE_FOR_SLEEP);
    } else {
        enqueue_signal(&app->queue, SIGNAL_RESUME);
    }

    return 0;
}

static int get_active_session(application_context_t *app) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;

    int r = sd_bus_call_method(
        app->system_bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "ListSessions",
        &error,
        &m,
        NULL
    );

    if (r < 0) {
        fprintf(stderr, "ListSessions failed: %s\n", error.message);
        return r;
    }

    r = sd_bus_message_enter_container(m, 'a', "(susso)");
    if (r < 0) return r;

    while ((r = sd_bus_message_enter_container(m, 'r', "susso")) > 0) {
        const char *sid, *user, *seat, *path;
        uint32_t uid;

        r = sd_bus_message_read(m, "susso", &sid, &uid, &user, &seat, &path);
        if (r < 0) return r;

        if (strcmp(seat, "seat0") == 0) {
            app->session_id = strdup(sid);
            app->session_path = strdup(path);

            printf("[INFO] Using session %s (%s)\n", sid, path);

            sd_bus_message_exit_container(m);
            break;
        }

        sd_bus_message_exit_container(m);
    }

    sd_bus_message_unref(m);
    return 0;
}

static int acquire_inhibitor(application_context_t *app) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    int fd;

    if (app->inhibitor_lock_fd >= 0) {
        close(app->inhibitor_lock_fd);
        app->inhibitor_lock_fd = -1;
    }

    int response = sd_bus_call_method(
        app->system_bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "Inhibit",
        &error,
        &m,
        "ssss",
        "sleep",
        "prelockd",
        "Lock Screen Before Sleep In Progress",
        "delay"
    );

    if (response < 0) {
        pvprintf_error(&app->config, error.message, -response);
        sd_bus_error_free(&error);
        sd_bus_message_unref(m);
        return response;
    }

    sd_bus_error_free(&error);

    // Read returned file descriptor
    response = sd_bus_message_read(m, "h", &fd);
    if (response < 0) {
        pvprintf_error(&app->config, "Failed to parse inhibitor fd", -response);
        sd_bus_message_unref(m);
        return response;
    }

    app->inhibitor_lock_fd = dup(fd);

    pvprintf_info(&app->config, "Inhibitor acquired");

    sd_bus_message_unref(m);
    return 0;
}

static int lock_session(application_context_t *app) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;

    int response = sd_bus_call_method(
        app->user_bus,
        "org.kde.screensaver",
        "/org/freedesktop/ScreenSaver",
        "org.freedesktop.ScreenSaver",
        "Lock",
        &error,
        &m,
        NULL,
        NULL
    );

    pvprintf_info(&app->config, "Lock Request Sent");

    if (response < 0){
        pvprintf_error(&app->config, error.message, -response);
        sd_bus_error_free(&error);
        sd_bus_message_unref(m);
        return response;
    }

    sd_bus_error_free(&error);
    sd_bus_message_unref(m);
    return 0;
}

static int setup(application_context_t *app){
    config_t config;
    config.lock_delay_s = 1;
    config.lock_delay_ms = 0;
    config.timeout_s = 1;
    config.timeout_ms = 0;
    config.verbose = TRUE;

    app->config = config;

    // char *sid = NULL;

    // int response = sd_pid_get_session(0, &sid);
    // if (response < 0) {
    //     pvprintf_error(&app->config, "Failed to get session", -response);
    //     return 1;
    // }
 
    // app->session_id = sid;
    char *path = malloc(128);
    snprintf(path, 128, "/org/freedesktop/login1/session/%s", "_31");
    app->session_path = path;
    app->session_id = "1";

    // get_active_session(app);
    app->system_bus = NULL;
    app->user_bus = NULL;
    app->state = STATE_ACQUIRE_INHIBITOR;
    app->inhibitor_lock_fd = -1;

    signal_queue_t queue;
    queue.head = 0;
    queue.tail = 0;

    app->queue = queue;

    int response = sd_bus_open_system(&app->system_bus);
    if (response < 0) {
        pvprintf_error(&app->config, "Failed to connect to system bus", -response);
        return 1;
    }

    response = sd_bus_open_user(&app->user_bus);
    if (response < 0) {
        pvprintf_error(&app->config, "Failed to connect to user bus", -response);
        return 1;
    }

    // register a match rule for the PrepareForSleep signal
    response = sd_bus_match_signal(
        app->system_bus,
        NULL,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "PrepareForSleep",
        prepare_for_sleep_handler,
        app
    );

    if (response < 0) {
        pvprintf_error(&app->config, "Failed to add match", -response);
        return 1;
    }

    response = sd_bus_match_signal(
        app->system_bus,
        NULL,
        "org.freedesktop.login1",
        app->session_path,
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        properties_changed_handler,
        app
    );

    if (response < 0) {
        pvprintf_error(&app->config, "Failed to add match", -response);
        return 1;
    }

    pvprintf_info(&app->config, "prelockd setup complete...");

    enqueue_signal(&app->queue, SIGNAL_ACQUIRE_INHIBITOR);
    return 0;
}

/**
 * Handle FSM logic here
 * Thankfully it's just a loop from start to finish
 */
static void handle_signal(application_context_t *app, signal_t *s) {
    switch (app->state) {
        case STATE_ACQUIRE_INHIBITOR:
            if (*s == SIGNAL_ACQUIRE_INHIBITOR) {
                pvprintf_state(&app->config, "Transitioning to IDLE");
                acquire_inhibitor(app);
                app->state = STATE_IDLE;
            }

            break;
        case STATE_IDLE:
            if (*s == SIGNAL_PREPARE_FOR_SLEEP) {
                pvprintf_state(&app->config, "Transitioning to LOCK_REQUESTED");
                lock_session(app);
                app->state = STATE_LOCK_REQUESTED;
                // TODO: add request timeout
            }

            break;
        case STATE_LOCK_REQUESTED:
            if (*s == SIGNAL_SESSION_LOCKED){
                pvprintf_state(&app->config, "Transitioning to LOCK_CONFIRMED");
                app->state = STATE_LOCK_CONFIRMED;

                struct timespec ts;
                ts.tv_sec = 0;
                ts.tv_nsec = app->config.lock_delay_ms * 1000000L;
                nanosleep(&ts, NULL);

                enqueue_signal(&app->queue, SIGNAL_TIMEOUT);
            }

            break;
        case STATE_LOCK_CONFIRMED:
            if (*s == SIGNAL_TIMEOUT) {
                pvprintf_state(&app->config, "Transitioning to RELEASE_INHIBITOR");
                app->state = STATE_RELEASING_INHIBITOR;
                close(app->inhibitor_lock_fd);
            }

            break;
        case STATE_RELEASING_INHIBITOR:
            if (*s == SIGNAL_RESUME) {
                pvprintf_state(&app->config, "Transitioning to ACQUIRE_INHIBITOR");
                app->state = STATE_ACQUIRE_INHIBITOR;
                enqueue_signal(&app->queue, SIGNAL_ACQUIRE_INHIBITOR);  // enqueue signal so that inhibitor will be acquired
            }

            break;
    }
}

/**
 * I need to register a match rule for PrepareForSleep (start lock)
 * I need to register a match rule for Lock in the current session (confirm lock)
 */
int main(int argc, char *argv[]) {
    application_context_t app = {0};

    if (setup(&app) != 0) {
        pvprintf_error(&app.config, "Failed to setup daemon", 1);
        return 1;
    }

    openlog("prelockd", LOG_PID, LOG_USER);

    // the daemon event loop
    while(1) {
        while (sd_bus_process(app.system_bus, NULL) > 0) {
            // send new signals to handle_signals
            signal_t signal;
            pvprintf_info(&app.config, "Signal Received!");
            while (dequeue_signal(&app.queue, &signal)){
                handle_signal(&app, &signal);
            }
        }

        sd_bus_wait(app.system_bus, (uint64_t)-1);
    }

    closelog();

    // clean up
    sd_bus_unref(app.system_bus);
    free((void *)app.session_path);
    return 0;
}