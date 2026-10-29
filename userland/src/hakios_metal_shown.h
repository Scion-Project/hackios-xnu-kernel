/*
 * Whether this process may be shown the Metal device, by the level of boot-arg metal (sysctl kern.bootargs) and the file
 * HakiosMetal.clients beside the bundle: shared by the two builds of the plug-in, src/HakiosMetal.m and (METAL=vk)
 * src/metalvk/HakiosMetalVK.mm. See hakios_metal_shown.c.
 */
#ifndef HAKIOS_METAL_SHOWN_H
#define HAKIOS_METAL_SHOWN_H
#ifdef __cplusplus
extern "C" {
#endif
/* 1 if shown; say (may be NULL) is told why or why not, one line */
int hakios_metal_shown(void (*say)(const char *line));
/* the path of a file beside the bundle (NAME in the directory that holds HakiosMetal.bundle) into PATH; 1 if it is readable */
int hakios_metal_beside_bundle(const char *name, char *path, unsigned long size);
#ifdef __cplusplus
}
#endif
#endif
