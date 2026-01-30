/*
 * Theme debug test
 */
#include <stdio.h>
#include "src/themes.h"

int main() {
    printf("Initializing themes...\n");
    int ret = themes_init(NULL);
    printf("themes_init returned: %d\n", ret);
    printf("themes_count: %d\n", themes_count());
    printf("themes_enabled: %d\n", themes_enabled());
    
    printf("\nSetting rick-sanchez active...\n");
    ret = theme_set_active("rick-sanchez");
    printf("theme_set_active returned: %d\n", ret);
    printf("themes_enabled now: %d\n", themes_enabled());
    printf("active theme: %s\n", theme_get_active());
    
    face_bitmap_t *face = theme_get_face(FACE_HAPPY);
    if (face) {
        printf("\nHAPPY face: loaded=%d, %dx%d, stride=%d\n", 
               face->loaded, face->width, face->height, face->stride);
        if (face->bitmap) {
            printf("bitmap ptr: %p\n", (void*)face->bitmap);
            /* Print first few bytes */
            printf("First bytes: ");
            for (int i = 0; i < 8 && i < face->stride; i++) {
                printf("%02x ", face->bitmap[i]);
            }
            printf("\n");
        }
    } else {
        printf("\nHAPPY face: NULL!\n");
    }
    
    themes_cleanup();
    return 0;
}
