#include <systemd/sd-bus.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

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
    int lock_delay_ms;
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
    sd_bus *bus;
    int inhibitor_lock_fd;
    
    state_t state;
    config_t config;
    signal_queue_t queue;

    const char *session_id;
    const char *session_path;

    uint64_t lock_request_time;
} application_context_t;

static void pvprintf_state(config_t *c, char *s[]) {
    if (c->verbose) {
        fprintf("[STATE] %s\n", s);
    }
}

static void pvprintf_info(config_t *c, char *s[]) {
    if (c->verbose) {
        fprintf("[INFO] %s\n", s);
    }
}

static void pvprintf_error(config_t *c, char *s[], int error) {
    if (c->verbose) {
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

static int acquire_inhibitor(application_context_t *app) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    int fd;

    if (app->inhibitor_lock_fd >= 0) {
        close(app->inhibitor_lock_fd);
    }

    int response = sd_bus_call_method(
        app->bus,
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
        "block"
    );

    if (response < 0) {
        pvprintf_error(&app->config, strcat("Failed to issue method call", error.message), -response);
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
        app->bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "LockSession",
        &error,
        &m,
        "s",
        app->session_id
    );

    if (response < 0){
        pvprintf_error(&app->config, strcat("Failed to issue method call", error.message), -response);
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
    config.lock_delay_ms = 100; //ms
    config.timeout_ms = 1000; //ms
    config.verbose = TRUE;

    app->config = config;

    const char *sid = getenv("XDG_SESSION_ID");

    if (!sid) {
        pvprintf_error(&app->config, "XDG_SESSION_ID not set...", 1);
        return 1;

        // TODO: add handling for multiple sessions as well as a fallback using sd_pid_get_session
    }
 
    app->session_id = sid;
    char *path = malloc(128);
    snprintf(path, 128, "/org/freedesktop/login1/session/%s", sid);
    app->session_path = path;
    app->bus = NULL;
    app->state = STATE_ACQUIRE_INHIBITOR;
    app->inhibitor_lock_fd = -1;

    signal_queue_t queue;
    queue.head = 0;
    queue.tail = 0;

    app->queue = queue;

    int response = sd_bus_open_system(&app->bus);
    if (response < 0) {
        pvprintf_error(&app->config, "Failed to connect to system bus", -response);
        return 1;
    }

    // register a match rule for the PrepareForSleep signal
    response = sd_bus_match_signal(
        app->bus,
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
        app->bus,
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

    if (setup(&app) < 0) {
        pvprintf_error(&app.config, "[ERROR] Failed to setup daemon", 1);
        return 1;
    }

    // the daemon event loop
    while(1) {
        // process our DBus messages, trigger callbacks, enqueue/dequeue signals
        sd_bus_process(app.bus, NULL);

        // send new signals to handle_signals
        signal_t signal;
        while (dequeue_signal(&app.queue, &signal)){
            handle_signal(&app, &signal);
        }

        sd_bus_wait(app.bus, (uint64_t)-1);
    }

    // clean up
    sd_bus_unref(app.bus);
    free(app.session_path);
    return 0;
}