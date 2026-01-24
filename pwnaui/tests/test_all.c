/*
 * PwnaUI Test Runner
 * Main entry point for running all C unit tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_framework.h"
#include "../src/font.h"
#include "../src/icons.h"
#include "../src/renderer.h"
#include "../src/display.h"
#include "../src/ipc.h"

/* External declarations for test suites */
extern void run_suite_font(void);
extern void run_suite_icons(void);
extern void run_suite_renderer(void);
extern void run_suite_display(void);
extern void run_suite_ipc(void);


int main(int argc, char *argv[]) {
    int run_all = 1;
    const char *suite_filter = NULL;
    
    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("PwnaUI Test Runner\n");
            printf("Usage: %s [OPTIONS] [SUITE]\n\n", argv[0]);
            printf("Options:\n");
            printf("  -h, --help     Show this help message\n");
            printf("  -l, --list     List available test suites\n");
            printf("\nSuites:\n");
            printf("  font           Font rendering tests\n");
            printf("  icons          Icon bitmap tests\n");
            printf("  renderer       Rendering engine tests\n");
            printf("  display        Display driver tests\n");
            printf("  ipc            IPC communication tests\n");
            printf("  all            Run all test suites (default)\n");
            return 0;
        }
        else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0) {
            printf("Available test suites:\n");
            printf("  font\n");
            printf("  icons\n");
            printf("  renderer\n");
            printf("  display\n");
            printf("  ipc\n");
            return 0;
        }
        else {
            suite_filter = argv[i];
            run_all = 0;
        }
    }
    
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║              PwnaUI C Unit Test Suite                     ║\n");
    printf("║                                                           ║\n");
    printf("║  Testing: font, icons, renderer, display, ipc             ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n");
    
    /* Run test suites */
    if (run_all || (suite_filter && strcmp(suite_filter, "font") == 0)) {
        run_suite_font();
    }
    
    if (run_all || (suite_filter && strcmp(suite_filter, "icons") == 0)) {
        run_suite_icons();
    }
    
    if (run_all || (suite_filter && strcmp(suite_filter, "renderer") == 0)) {
        run_suite_renderer();
    }
    
    if (run_all || (suite_filter && strcmp(suite_filter, "display") == 0)) {
        run_suite_display();
    }
    
    if (run_all || (suite_filter && strcmp(suite_filter, "ipc") == 0)) {
        run_suite_ipc();
    }
    
    /* Print summary */
    test_print_summary();
    
    return test_exit_code();
}
