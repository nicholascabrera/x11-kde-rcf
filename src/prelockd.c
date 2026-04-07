#include <systemd/sd-bus.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
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

// Function to check if the queue is empty
int no_signals(signal_queue_t *q) {
    if ((q->head == q->tail - 1)){
        return TRUE;
    }
    return FALSE;
}

// Function to check if the queue is full
int max_signals_received(signal_queue_t *q) {
    if(q->tail == MAX_SIGNALS) {
        return TRUE;
    }
    return FALSE;
}

// Function to add an element to the queue (Enqueue
// operation)
void enqueue_signal(signal_queue_t *q, signal_t s) {
    if (max_signals_received(q)) {
        printf("Signal queue is full\n");
        return;
    }
    q->signals[q->tail] = s;
    q->tail++;
}

// Function to remove an element from the queue (Dequeue
// operation)
int dequeue_signal(signal_queue_t *q, signal_t *s) {
    if (no_signals(q)) {
        printf("Signal queue is empty\n");
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
                printf("Lock confirmed via signal\n");

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

static int prepare_for_sleep_handler(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error) {
    application_context_t *app = userdata;
    int sleeping;
    int response;

    response = sd_bus_message_read(msg, "b", &sleeping);
    if (response < 0) {
        fprintf(stderr, "Failed to parse parameters: %s\n", strerror(-response));
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
        fprintf(stderr, "Failed to issue method call: %s\n", error.message);
        sd_bus_error_free(&error);
        return response;
    }

    // Read returned file descriptor
    response = sd_bus_message_read(m, "h", &fd);
    if (response < 0) {
        fprintf(stderr, "Failed to parse inhibitor fd: %s\n", strerror(-response));
        sd_bus_message_unref(m);
        return response;
    }

    app->inhibitor_lock_fd = fd;

    printf("[INFO] Inhibitor acquired (fd=%d)\n", app->inhibitor_lock_fd);

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
        fprintf(stderr, "Failed to issue method call: %s\n", error.message);
        return response;
    }

    sd_bus_error_free(&error);
    sd_bus_message_unref(m);
    return 0;
}

static int setup(application_context_t *app){
    int sid = getenv("XDG_SESSION_ID");

    if (!sid) {
        fprintf(stderr, "XDG_SESSION_ID not set...\n");
        return 1;

        // TODO: add handling for multiple sessions as well as a fallback using sd_pid_get_session
    }

    app->session_id = sid;
    app->session_path = "/org/freedesktop/login1/session/" + sid;
    app->bus = NULL;
    app->state = STATE_ACQUIRE_INHIBITOR;
    app->inhibitor_lock_fd = -1;

    config_t config;
    config.lock_delay_ms = 100; //ms
    config.timeout_ms = 1000; //ms
    config.verbose = FALSE;

    app->config = config;

    signal_queue_t queue;
    queue.head = -1;
    queue.tail = 0;

    app->queue = queue;

    int response = sd_bus_open_system(&app->bus);
    if (response < 0) {
        fprintf(stderr, "Failed to connect to system bus: %s\n", strerror(-response));
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
        fprintf(stderr, "Failed to add match: %s\n", strerror(-response));
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
        fprintf(stderr, "Failed to add match: %s\n", strerror(-response));
        return 1;
    }

    printf("prelockd setup complete...");
    return 0;
}

/**
 * Handle FSM logic here
 * Thankfully it's just a loop from start to finish
 */
static void handle_signal(application_context_t *app, signal_t *s) {
    switch (app->state) {
        case STATE_ACQUIRE_INHIBITOR:        
            acquire_inhibitor(app);
            app->state = STATE_IDLE;

            break;
        case STATE_IDLE:
            if (*s == SIGNAL_PREPARE_FOR_SLEEP) {
                lock_session(app);
                app->state = STATE_LOCK_REQUESTED;
                // TODO: add request timeout
            }

            break;
        case STATE_LOCK_REQUESTED:
            if (*s == SIGNAL_SESSION_LOCKED){
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
                app->state = STATE_RELEASING_INHIBITOR;
                close(app->inhibitor_lock_fd);
            }

            break;
        case STATE_RELEASING_INHIBITOR:
            if (*s == SIGNAL_RESUME) {
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
    application_context_t *app;

    setup(app);

    // the daemon event loop
    while(1) {
        // process our DBus messages, trigger callbacks, enqueue/dequeue signals
        sd_bus_process(app->bus, NULL);

        // send new signals to handle_signals
        signal_t signal;
        while (dequeue_signal(&app->queue, &signal)){
            handle_signal(app, &signal);
        }
    }

    // clean up
    sd_bus_unref(app->bus);
    return 0;
}