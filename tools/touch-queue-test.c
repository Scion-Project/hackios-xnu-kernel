/* From danstronics/ios-s9plus-exynos core/drivers/input (MIT, "HakiOS authors"); see docs/star2lte-absorb.md. */
#include <assert.h>
#include <stdio.h>
#include "../drivers/input/hakios.touch_queue.h"

static struct hakios_touch queue[512];
static uint32_t head, tail, replaced;
static void post(uint32_t type, uint32_t finger, uint32_t x, int enabled)
{
    struct hakios_touch *slot = hakios_touch_queue_slot(queue, 512, &head, &tail,
        type, finger, enabled, &replaced);
    *slot = (struct hakios_touch){ x, type, finger, x, x };
}
int main(void)
{
    post(HAKIOS_TOUCH_DOWN, 0, 0, 1);
    for (unsigned i = 1; i <= 1000; i++) post(HAKIOS_TOUCH_MOVE, 0, i, 1);
    post(HAKIOS_TOUCH_UP, 0, 1001, 1);
    assert(head == 0 && tail == 3 && replaced == 999);
    assert(queue[0].type == HAKIOS_TOUCH_DOWN);
    assert(queue[1].type == HAKIOS_TOUCH_MOVE && queue[1].x == 1000 && queue[1].time == 1000);
    assert(queue[2].type == HAKIOS_TOUCH_UP);
    head = tail = replaced = 0;
    post(HAKIOS_TOUCH_MOVE, 0, 1, 1);
    post(HAKIOS_TOUCH_MOVE, 1, 2, 1);
    post(HAKIOS_BUTTON_DOWN, 1, 3, 1);
    post(HAKIOS_TOUCH_MOVE, 1, 4, 1);
    assert(tail == 4 && replaced == 0);
    head = tail = replaced = 0;
    post(HAKIOS_TOUCH_MOVE, 0, 1, 0);
    post(HAKIOS_TOUCH_MOVE, 0, 2, 0);
    assert(tail == 2 && replaced == 0 && queue[0].x == 1 && queue[1].x == 2);
    head = tail = UINT32_MAX - 1;
    replaced = 0;
    post(HAKIOS_TOUCH_DOWN, 0, 1, 1);
    post(HAKIOS_TOUCH_MOVE, 0, 2, 1);
    post(HAKIOS_TOUCH_MOVE, 0, 3, 1);
    post(HAKIOS_TOUCH_UP, 0, 4, 1);
    assert(tail - head == 3 && replaced == 1);
    assert(queue[UINT32_MAX % 512].type == HAKIOS_TOUCH_MOVE && queue[UINT32_MAX % 512].x == 3);
    puts("Touch queue: stalled consumer, transitions, different fingers, buttons, disabled mode and counter wrap passed");
}
