/*
 * The Exynos 9810 Galaxys' physical memory outside XNU's RAM that this port uses (the S9, starlte; the S9+ and the Note9
 * share the map): one table, so that no two users of the lower bank can be given the same bytes again.
 *
 * Why there is such memory at all: the loader gives XNU the bank from 0x880000000, and none of the kernel's own memory
 * is below 4 GB on these phones, while the display's, the camera's and the flash controller's DMA take 32-bit addresses
 * (and IOKit's "below 4 GB" never succeeds there). So each takes a piece of the lower bank that XNU does not manage:
 * the device tree's gap between the panel's UEFI framebuffer and secure_camera, where no reserved-memory node is, and
 * which neither payload layout uses (hakios-starlte's README, the memory table; starlte PR 27). Above 0xF0000000 is
 * the Shannon modem's (modem_if, cp_ram_logging) and ABOX's (abox_rmem): never given to anything but ABOX itself.
 *
 *   0xCC000000 - 0xCD042FFF   the panel's framebuffer, UEFI's (1440 x 2960 x 4)
 *   0xD0000000 - 0xD3FFFFFF   the loader's moved payload and the panic log (xnuloader/loader.c, payload_home)
 *   0xD4000000 - 0xD6FFFFFF   the DECON's scan-out buffers: fb_flip's two, fb_dpp's three (hakios.exynos_dpu.c)
 *   0xD7000000 - 0xD7FFFFFF   the display's pool for fb_zc's surfaces, its first part: one slot of 12 MB
 *   0xD8000000 - 0xD9FFFFFF   the front camera's ring, still and picture (hakios.exynos_camera.c)
 *   0xDA000000 - 0xDA3FFFFF   UFS's own-mode list, descriptors and buffers (hakios.ufshc.c, ufs=3)
 *   0xDA400000 - 0xDBFFFFFF   the display's pool, its second part: two slots
 *   0xDC000000 - 0xDEBFFFFF   Wi-Fi's DMA pool when wifi_dma_at=0xDC000000 is given (hakios.lx.c: 44 MB; off by default)
 *   0xDEC00000 - 0xDFFFFFFF   the display's pool, its third part: one slot (hakios.exynos_dpu.c: four slots in all;
 *                             CoreAnimation keeps two display surfaces)
 *   0xE0000000 - 0xE0FFFFFF   the iris camera's raw frame and picture (hakios.exynos_iris.c; NOT for the display)
 *   0xE1000000 - 0xE18FFFFF   free, and NOT for the display: see below
 *   0xE1900000 -              secure_camera (the device tree's): the end of the gap
 *   0xF0000000 - 0xFB1FFFFF   the modem's: modem_if, cp_ram_logging (the loader's keep_out)
 *   0xFB200000 - 0xFD9FFFFF   ABOX's abox_rmem; its ring at abox_ring_pa= in it (hakios.exynos_abox.c)
 *
 * What is drawn at or above 0xE0000000 does not reach the panel (EXYNOS9810_MEM_DISPLAY_LIMIT). From 3 November
 * (PR 165) fb_zc's pool was 0xDEC00000-0xE18FFFFF, three slots; CoreAnimation's two surfaces took the first two, and
 * the second, 0xDF800000-0xE0A8FFFF, ran over that line 8 MiB in: of a 1125 x 2436 surface at 4544 bytes a row
 * (11.07 MB), the last 24% of the rows. On the panel that part was black on every other frame, the right picture on
 * the others (morning-1103: "the bottom quarter of the screen keeps flashing"), with no error bit in VGF0's IDMA or
 * DPP; with fb_zc=0 (frames copied into the buffers at 0xD4000000) it did not. So every range the display scans out
 * ends at 0xE0000000 at the latest, checked below.
 *
 * Each driver takes its range from here (the display, the camera, UFS) and the checks at the end hold the table together
 * at build time. Until 3 November the display's pool ran from 0xD7000000 to secure_camera (14 slots), across the
 * camera's, UFS's and Wi-Fi's ranges. The loader (xnuloader/loader.c: payload_home, keep_out) and Wi-Fi (wifi_dma_at=,
 * given as a number) and ABOX (abox_rmem, in its own driver) say the same numbers themselves.
 */
#ifndef _SYS_HAKIOS_EXYNOS9810_MEM_H_
#define _SYS_HAKIOS_EXYNOS9810_MEM_H_

#define EXYNOS9810_MEM_FB_PA            0xCC000000ULL   /* UEFI's framebuffer */
#define EXYNOS9810_MEM_FB_END           0xCD043000ULL
#define EXYNOS9810_MEM_PAYLOAD_PA       0xD0000000ULL   /* the moved payload, the panic log */
#define EXYNOS9810_MEM_PAYLOAD_END      0xD4000000ULL
#define EXYNOS9810_MEM_DPU_SCAN_PA      0xD4000000ULL   /* the scan-out buffers */
#define EXYNOS9810_MEM_DPU_SCAN_END     0xD7000000ULL
#define EXYNOS9810_MEM_DPU_POOL0_PA     0xD7000000ULL   /* fb_zc's surfaces: one slot */
#define EXYNOS9810_MEM_DPU_POOL0_END    0xD8000000ULL
#define EXYNOS9810_MEM_CAMERA_PA        0xD8000000ULL   /* the camera's frames */
#define EXYNOS9810_MEM_CAMERA_END       0xDA000000ULL
#define EXYNOS9810_MEM_UFS_PA           0xDA000000ULL   /* UFS's own mode */
#define EXYNOS9810_MEM_UFS_END          0xDA400000ULL
#define EXYNOS9810_MEM_DPU_POOL1_PA     0xDA400000ULL   /* fb_zc's surfaces: two slots */
#define EXYNOS9810_MEM_DPU_POOL1_END    0xDC000000ULL
#define EXYNOS9810_MEM_WIFI_DMA_PA      0xDC000000ULL   /* wifi_dma_at= (optional) */
#define EXYNOS9810_MEM_WIFI_DMA_END     0xDEC00000ULL
#define EXYNOS9810_MEM_DPU_POOL2_PA     0xDEC00000ULL   /* fb_zc's surfaces: one slot */
#define EXYNOS9810_MEM_DPU_POOL2_END    0xE0000000ULL
#define EXYNOS9810_MEM_DISPLAY_LIMIT    0xE0000000ULL   /* nothing the display scans out from here up */
#define EXYNOS9810_MEM_IRIS_PA          0xE0000000ULL   /* the iris camera's frame (the camera's DMA, not the display's) */
#define EXYNOS9810_MEM_IRIS_END         0xE1000000ULL
#define EXYNOS9810_MEM_GAP_END          0xE1900000ULL   /* secure_camera */
#define EXYNOS9810_MEM_MODEM_PA         0xF0000000ULL   /* modem_if, cp_ram_logging: keep out */
#define EXYNOS9810_MEM_MODEM_END        0xFB200000ULL
#define EXYNOS9810_MEM_ABOX_PA          0xFB200000ULL   /* abox_rmem: ABOX's alone */
#define EXYNOS9810_MEM_ABOX_END         0xFDA00000ULL

#define EXYNOS9810_MEM_SIZE(name)       (EXYNOS9810_MEM_##name##_END - EXYNOS9810_MEM_##name##_PA)

/* the table holds together: each range in order, none over the next, all of the gap's in the gap, all below 4 GB */
#define EXYNOS9810_MEM_BEFORE(a, b)     (EXYNOS9810_MEM_##a##_END <= EXYNOS9810_MEM_##b##_PA)
_Static_assert(EXYNOS9810_MEM_BEFORE(FB, PAYLOAD), "exynos9810 memory: UEFI's framebuffer runs into the payload");
_Static_assert(EXYNOS9810_MEM_BEFORE(PAYLOAD, DPU_SCAN), "exynos9810 memory: the payload runs into the display's buffers");
_Static_assert(EXYNOS9810_MEM_BEFORE(DPU_SCAN, DPU_POOL0), "exynos9810 memory: the display's buffers run into its pool");
_Static_assert(EXYNOS9810_MEM_BEFORE(DPU_POOL0, CAMERA), "exynos9810 memory: the display's pool runs into the camera's");
_Static_assert(EXYNOS9810_MEM_BEFORE(CAMERA, UFS), "exynos9810 memory: the camera's runs into UFS's");
_Static_assert(EXYNOS9810_MEM_BEFORE(UFS, DPU_POOL1), "exynos9810 memory: UFS's runs into the display's pool");
_Static_assert(EXYNOS9810_MEM_BEFORE(DPU_POOL1, WIFI_DMA), "exynos9810 memory: the display's pool runs into Wi-Fi's");
_Static_assert(EXYNOS9810_MEM_BEFORE(WIFI_DMA, DPU_POOL2), "exynos9810 memory: Wi-Fi's runs into the display's pool");
_Static_assert(EXYNOS9810_MEM_DPU_SCAN_END <= EXYNOS9810_MEM_DISPLAY_LIMIT &&
    EXYNOS9810_MEM_DPU_POOL0_END <= EXYNOS9810_MEM_DISPLAY_LIMIT && EXYNOS9810_MEM_DPU_POOL1_END <= EXYNOS9810_MEM_DISPLAY_LIMIT &&
    EXYNOS9810_MEM_DPU_POOL2_END <= EXYNOS9810_MEM_DISPLAY_LIMIT, "exynos9810 memory: the display scans out from 0xE0000000 up");
_Static_assert(EXYNOS9810_MEM_BEFORE(DPU_POOL2, IRIS), "exynos9810 memory: the display's pool runs into the iris camera's");
_Static_assert(EXYNOS9810_MEM_IRIS_END <= EXYNOS9810_MEM_GAP_END, "exynos9810 memory: the iris camera's runs into secure_camera");
_Static_assert(EXYNOS9810_MEM_DISPLAY_LIMIT <= EXYNOS9810_MEM_GAP_END, "exynos9810 memory: the display's limit is past secure_camera");
_Static_assert(EXYNOS9810_MEM_GAP_END <= EXYNOS9810_MEM_MODEM_PA, "exynos9810 memory: the gap runs into the modem's");
_Static_assert(EXYNOS9810_MEM_BEFORE(MODEM, ABOX), "exynos9810 memory: the modem's runs into ABOX's");
_Static_assert(EXYNOS9810_MEM_ABOX_END <= 0x100000000ULL, "exynos9810 memory: above 4 GB");
_Static_assert(EXYNOS9810_MEM_SIZE(CAMERA) == (32ULL << 20) && EXYNOS9810_MEM_SIZE(UFS) == (4ULL << 20) &&
    EXYNOS9810_MEM_SIZE(IRIS) == (16ULL << 20) &&
    EXYNOS9810_MEM_SIZE(WIFI_DMA) >= (44ULL << 20), "exynos9810 memory: the sizes are what their drivers lay out");

#endif /* _SYS_HAKIOS_EXYNOS9810_MEM_H_ */
