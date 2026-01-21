/*
 * Test theme loading
 */
#include <stdio.h>
#include <stdlib.h>
#include "themes.h"

int main(int argc, char *argv[]) {
    const char *theme_name = argc > 1 ? argv[1] : "rick-sanchez";
    
    printf("Initializing theme system...\n");
    if (themes_init(NULL) != 0) {
        fprintf(stderr, "Failed to initialize themes\n");
        return 1;
    }
    
    printf("\nAvailable themes:\n");
    int count = 0;
    char **list = theme_list_available(&count);
    if (list) {
        for (int i = 0; i < count; i++) {
            printf("  - %s\n", list[i]);
        }
        theme_list_free(list);
    }
    
    printf("\nLoading theme '%s'...\n", theme_name);
    theme_t *theme = theme_load(theme_name);
    if (!theme) {
        fprintf(stderr, "Failed to load theme\n");
        return 1;
    }
    
    printf("\nLoaded faces:\n");
    for (int i = 0; i < FACE_STATE_COUNT; i++) {
        if (theme->faces[i].loaded) {
            printf("  %s: %dx%d\n", g_face_state_names[i], 
                   theme->faces[i].width, theme->faces[i].height);
        }
    }
    
    printf("\nSetting as active theme...\n");
    if (theme_set_active(theme_name) != 0) {
        fprintf(stderr, "Failed to set active theme\n");
        return 1;
    }
    
    printf("Theme enabled: %d\n", themes_enabled());
    
    themes_cleanup();
    printf("\nDone!\n");
    return 0;
}
