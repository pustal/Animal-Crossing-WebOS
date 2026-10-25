/**
 * @file android_glue.h
 * @brief webOS implementation of the platform hooks the port calls under TARGET_ANDROID.
 *
 * The webOS build compiles the port's Android variant (TARGET_ANDROID): its
 * branches are what a TV needs too (OpenGL ES 3, full screen, vsync, Back as
 * the pause key). This header has the same name and functions as the Android
 * port's app/src/main/cpp/android/android_glue.h, so pc_main.c includes it
 * unchanged; the implementations are in webos_glue.c.
 */
#ifndef ACWEBOS_ANDROID_GLUE_H
#define ACWEBOS_ANDROID_GLUE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Called at the top of main(): switches into the app's data folder so the
 * port's relative paths (settings.ini, save/, rom/, shaders/, texture_pack/)
 * resolve there, creates those folders, and sends logs to files.
 */
void android_bootstrap(void);

#ifdef __cplusplus
}
#endif

#endif /* ACWEBOS_ANDROID_GLUE_H */
