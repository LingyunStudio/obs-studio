#pragma once

#include <stdbool.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

struct region_picker_result {
	HMONITOR monitor;
	long x;
	long y;
	long w;
	long h;
};

/* Shows a fullscreen topmost overlay and lets the user drag a selection
 * rectangle. Blocks (modal) until the user finishes or cancels. Returns true
 * if a selection was made, in which case `out` is filled with the monitor the
 * selection is on and the rectangle in absolute virtual-desktop coordinates. */
extern bool region_picker_select(struct region_picker_result *out);

#ifdef __cplusplus
}
#endif
