/*
 * Transit Terminal Departures Board
 * TTC (Toronto Transit Commission) — Real-Time Display
 *
 * Reads routes from routes.txt and shows a live-updating board with:
 *   - Countdown to next vehicle arrival
 *   - Service alerts (random Delayed / Stalled statuses)
 * - Eastbound / Westbound direction toggle (T key, persisted)
 *
 * Controls:
 * T   Toggle Eastbound / Westbound
 *   Q   Quit
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>

/* ═══════════════════════════════════════════════
 * TTC Colour Scheme  (ANSI escape codes)
 * TTC corporate colours: red #DA291C, white, black
 * ═══════════════════════════════════════════════ */
#define RESET        "\033[0m"
#define BOLD         "\033[1m"
#define RED_BG       "\033[41m"          /* TTC red header               */
#define WHITE_FG     "\033[97m"          /* Bright white text            */
#define YELLOW_FG    "\033[93m"          /* Arrival time / column heads  */
#define GREEN_FG     "\033[92m"          /* On-time status               */
#define RED_FG       "\033[91m"          /* Stalled alert                */
#define DARK_BG      "\033[100m"         /* Dark grey bar                */
#define ALT_ROW_BG   "\033[48;5;234m"   /* Very dark alternate row      */
#define GRAY_FG      "\033[90m"          /* Dim separator text           */
#define CURSOR_HOME  "\033[H"
#define CLEAR_SCR    "\033[2J"
#define HIDE_CURSOR  "\033[?25l"
#define SHOW_CURSOR  "\033[?25h"
#define ERASE_BELOW  "\033[J"

/* ═══════════════════════════════════════════════
 * Configuration
 * ═══════════════════════════════════════════════ */
#define MAX_ROUTES   30
#define NAME_LEN     48
#define BOARD_W      78      /* board width in characters                 */
#define ALERT_PCT    15      /* % chance a new arrival triggers an alert  */
#define STATE_FILE   ".board_state"  /* persisted direction toggle          */
#define STALL_PCT     3      /* % chance (within ALERT_PCT) of stall      */
#define MIN_INTV      4      /* minimum service interval in minutes        */
#define MAX_INTV     15      /* maximum service interval in minutes        */

/* ═══════════════════════════════════════════════
 * Data Structures
 * ═══════════════════════════════════════════════ */
typedef enum { ON_TIME = 0, DELAYED, STALLED } ServiceStatus;

typedef struct {
    char          number[12];
    char          name[NAME_LEN];
    char          term_eb[NAME_LEN];  /* Eastbound terminus / destination */
    char          term_wb[NAME_LEN];  /* Westbound terminus / destination */

    /* Eastbound */
    int           intv_eb;            /* service interval, minutes         */
    time_t        next_eb;            /* epoch of next predicted arrival   */
    ServiceStatus status_eb;
    int           delay_eb;           /* extra delay minutes, if DELAYED   */

    /* Westbound */
    int           intv_wb;
    time_t        next_wb;
    ServiceStatus status_wb;
    int           delay_wb;
} Route;

/* ═══════════════════════════════════════════════
 * Globals
 * ═══════════════════════════════════════════════ */
static Route          routes[MAX_ROUTES];
static int            num_routes = 0;
static int            show_eb    = 1;   /* 1 = Eastbound, 0 = Westbound  */
static struct termios orig_term;
static volatile int   running    = 1;

/* ═══════════════════════════════════════════════
 * Terminal Utilities
 * ═══════════════════════════════════════════════ */
static void restore_terminal(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
    printf(SHOW_CURSOR RESET "\n");
    fflush(stdout);
}

static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
    restore_terminal();
    exit(0);
}

static void set_raw_mode(void)
{
    struct termios raw;
    tcgetattr(STDIN_FILENO, &orig_term);
    raw = orig_term;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON);
    raw.c_cc[VMIN]  = 0;   /* non-blocking read */
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    printf(HIDE_CURSOR);
    fflush(stdout);
}

/* Returns pressed character or -1 if nothing available */
static int read_key(void)
{
    unsigned char c;
    return (read(STDIN_FILENO, &c, 1) == 1) ? (int)c : -1;
}

/* ═══════════════════════════════════════════════
 * Schedule Helpers
 * ═══════════════════════════════════════════════ */
static ServiceStatus random_status(int *delay_out)
{
    int r = rand() % 100;
    if (r < STALL_PCT) {
        *delay_out = 0;
        return STALLED;
    }
    if (r < ALERT_PCT) {
        *delay_out = 2 + rand() % 9;  /* 2–10 min delay */
        return DELAYED;
    }
    *delay_out = 0;
    return ON_TIME;
}

/* ═══════════════════════════════════════════════
 * Load Routes from File
 * ═══════════════════════════════════════════════ */
/* ═══════════════════════════════════════════════
 * Persistence  (saves direction toggle across restarts)
 * ═══════════════════════════════════════════════ */
static void save_state(void)
{
    FILE *fp = fopen(STATE_FILE, "w");
    if (fp) {
        fprintf(fp, "%d\n", show_eb);
        fclose(fp);
    }
}

static void load_state(void)
{
    FILE *fp = fopen(STATE_FILE, "r");
    if (fp) {
        int val;
        if (fscanf(fp, "%d", &val) == 1)
            show_eb = (val != 0) ? 1 : 0;
        fclose(fp);
    }
    /* File absent on first run — default Eastbound stays */
}


static void load_routes(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "Error: cannot open '%s'\n", path);
        exit(EXIT_FAILURE);
    }

    char   line[256];
    time_t now = time(NULL);

    while (fgets(line, (int)sizeof line, fp) && num_routes < MAX_ROUTES) {
        /* Skip comments and blank lines */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        /* Strip trailing newline / CR */
        line[strcspn(line, "\r\n")] = '\0';

        Route *r = &routes[num_routes];

        char *tok = strtok(line, "|");
        if (!tok) continue;
        strncpy(r->number, tok, sizeof r->number - 1);

        tok = strtok(NULL, "|");
        if (!tok) continue;
        strncpy(r->name, tok, sizeof r->name - 1);

        tok = strtok(NULL, "|");
        if (!tok) continue;
        strncpy(r->term_eb, tok, sizeof r->term_eb - 1);

        tok = strtok(NULL, "|");
        if (!tok) continue;
        strncpy(r->term_wb, tok, sizeof r->term_wb - 1);

        /* Randomise service intervals and stagger initial arrivals */
        r->intv_eb = MIN_INTV + rand() % (MAX_INTV - MIN_INTV + 1);
        r->intv_wb = MIN_INTV + rand() % (MAX_INTV - MIN_INTV + 1);

        r->next_eb = now + 1 + (long)(rand() % (r->intv_eb * 60));
        r->next_wb = now + 1 + (long)(rand() % (r->intv_wb * 60));

        r->status_eb = random_status(&r->delay_eb);
        r->status_wb = random_status(&r->delay_wb);

        num_routes++;
    }
    fclose(fp);
}

/* ═══════════════════════════════════════════════
 * Update Countdown Timers
 * ═══════════════════════════════════════════════ */
static void update(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < num_routes; i++) {
        Route *r = &routes[i];

        if (now >= r->next_eb) {
            r->next_eb  = now + (long)(r->intv_eb * 60);
            r->status_eb = random_status(&r->delay_eb);
        }
        if (now >= r->next_wb) {
            r->next_wb  = now + (long)(r->intv_wb * 60);
            r->status_wb = random_status(&r->delay_wb);
        }
    }
}

/* ═══════════════════════════════════════════════
 * Draw Board
 * ═══════════════════════════════════════════════ */

/* Print text left-justified, padded to 'width' with spaces */
static void padprint(const char *text, int width)
{
    int len = (int)strlen(text);
    fputs(text, stdout);
    for (int i = len; i < width; i++) putchar(' ');
}

/* Horizontal rule */
static void hline(char c)
{
    for (int i = 0; i < BOARD_W; i++) putchar(c);
    putchar('\n');
}

static void draw_board(void)
{
    time_t     now = time(NULL);
    struct tm *tm  = localtime(&now);
    char       ts[12];
    strftime(ts, sizeof ts, "%H:%M:%S", tm);

    /* ── Move cursor to top-left (avoids full clear = no flicker) ── */
    fputs(CURSOR_HOME, stdout);

    /* ── TTC Red Header ──────────────────────────────────────────── */
    fputs(RED_BG WHITE_FG BOLD "  ", stdout);
    padprint("TTC  TORONTO TRANSIT COMMISSION", BOARD_W - 4);
    fputs("  \n  ", stdout);
    padprint("REAL-TIME DEPARTURES BOARD", BOARD_W - 4);
    fputs("  \n" RESET, stdout);

    /* ── Direction / Clock Bar ───────────────────────────────────── */
    fputs(DARK_BG WHITE_FG, stdout);
    char dir_bar[64];
    snprintf(dir_bar, sizeof dir_bar, "  Direction: %s  [T] Toggle",
             show_eb ? "EASTBOUND  >>>" : "<<<  WESTBOUND");
    printf("%-*s  Clock: %s  \n" RESET, BOARD_W - 12, dir_bar, ts);

    /* ── Column Headers ──────────────────────────────────────────── */
    fputs(BOLD YELLOW_FG, stdout);
    printf("  %-6s %-18s %-22s %-9s %s\n" RESET,
           "ROUTE", "NAME", "DESTINATION", "ARRIVES", "STATUS");
    fputs(GRAY_FG, stdout);
    hline('-');
    fputs(RESET, stdout);

    /* ── Route Rows ──────────────────────────────────────────────── */
    for (int i = 0; i < num_routes; i++) {
        Route        *r    = &routes[i];
        time_t        next = show_eb ? r->next_eb    : r->next_wb;
        ServiceStatus st   = show_eb ? r->status_eb  : r->status_wb;
        int           dly  = show_eb ? r->delay_eb   : r->delay_wb;
        const char   *term = show_eb ? r->term_eb    : r->term_wb;

        long secs = (long)(next - now);
        int  mins = (secs > 0) ? (int)(secs / 60) : 0;

        /* Arrival string */
        char arr[20];
        if (secs <= 30)     snprintf(arr, sizeof arr, "NOW");
        else if (mins == 0) snprintf(arr, sizeof arr, "< 1 min");
        else if (mins == 1) snprintf(arr, sizeof arr, "1 min");
        else                snprintf(arr, sizeof arr, "%d mins", mins);

        /* Status string + colour */
        char        status_text[32];
        const char *sc;
        if (st == STALLED) {
            strncpy(status_text, "!! STALLED !!", sizeof status_text - 1);
            status_text[sizeof status_text - 1] = '\0';
            sc = RED_FG BOLD;
        } else if (st == DELAYED) {
            snprintf(status_text, sizeof status_text, "+%d min delay", dly);
            sc = YELLOW_FG;
        } else {
            strncpy(status_text, "On Time", sizeof status_text - 1);
            status_text[sizeof status_text - 1] = '\0';
            sc = GREEN_FG;
        }

        /* Alternate row background */
        const char *rbg = (i % 2 == 0) ? "\033[40m" : ALT_ROW_BG;

        fputs(rbg, stdout);
        /* Route number — bold white */
        printf(BOLD WHITE_FG "  %-6s" RESET "%s", r->number, rbg);
        /* Name */
        printf(WHITE_FG " %-18s" RESET "%s", r->name, rbg);
        /* Destination */
        printf(YELLOW_FG " %-22s" RESET "%s", term, rbg);
        /* Arrival */
        printf(WHITE_FG " %-9s" RESET "%s", arr, rbg);
        /* Status */
        printf(" %s%-14s" RESET "\n", sc, status_text);
    }

    /* ── Footer ──────────────────────────────────────────────────── */
    fputs(GRAY_FG, stdout);
    hline('-');
    fputs(RESET, stdout);
    fputs(GRAY_FG "  [T] Toggle Direction   [Q] Quit"
          "   Auto-refreshes every second\n" RESET, stdout);
    fputs(RED_BG WHITE_FG "  ", stdout);
    padprint("ttc.ca  |  @TTCnotices  |  Call 511 for service information",
             BOARD_W - 4);
    fputs("  \n" RESET, stdout);

    /* Erase any leftover content below */
    fputs(ERASE_BELOW, stdout);
    fflush(stdout);
}

/* ═══════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════ */
int main(void)
{
    srand((unsigned)time(NULL));

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    load_routes("routes.txt");
    load_state();            /* restore direction from last session */
    if (num_routes == 0) {
        fprintf(stderr, "No routes loaded from routes.txt\n");
        return EXIT_FAILURE;
    }

    set_raw_mode();

    /* Clear screen once on startup */
    fputs(CLEAR_SCR CURSOR_HOME, stdout);
    fflush(stdout);

    while (running) {
        update();
        draw_board();

        /*
         * Poll keyboard 10× per second; redraw once per full second.
         * This gives a smooth countdown AND responsive key input.
         */
        for (int tick = 0; tick < 10 && running; tick++) {
            int ch = read_key();
            if (ch == 'q' || ch == 'Q') {
                running = 0;
            } else if (ch == 't' || ch == 'T') {
                show_eb = !show_eb;
                save_state();    /* persist the new direction */
                update();
                draw_board();   /* immediate redraw on direction toggle */
            }
            struct timespec _ts = {0, 100000000L}; nanosleep(&_ts, NULL);
        }
    }

    restore_terminal();
    save_state();        /* persist direction on clean exit */
    puts("Thank you for riding the TTC!");
    return EXIT_SUCCESS;
}
