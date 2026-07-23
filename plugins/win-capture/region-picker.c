#include "region-picker.h"

#include <windows.h>
#include <stdint.h>

#define DIM_ALPHA 130 /* 0-255, alpha of the dim layer outside the selection */
#define BORDER_PX 2
#define WM_PICKER_DONE (WM_APP + 1)

struct picker_state {
	BOOL dragging;
	BOOL cancelled;
	BOOL finished;
	BOOL done;
	POINT origin;      /* screen coords */
	RECT rect;          /* screen coords, normalized */
	HBITMAP dib;
	HDC dib_dc;
	void *bits;
	int dib_w;
	int dib_h;
	int vx;             /* virtual screen origin x */
	int vy;             /* virtual screen origin y */
};

static struct picker_state *get_state(HWND hwnd)
{
	return (struct picker_state *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

static void normalize_rect(POINT a, POINT b, RECT *out)
{
	out->left = a.x < b.x ? a.x : b.x;
	out->top = a.y < b.y ? a.y : b.y;
	out->right = a.x > b.x ? a.x : b.x;
	out->right++;
	out->bottom = a.y > b.y ? a.y : b.y;
	out->bottom++;
}

static inline uint8_t *pixel_at(struct picker_state *st, int x, int y)
{
	/* DIB is bottom-up; flip vertically */
	int row = st->dib_h - 1 - y;
	return (uint8_t *)st->bits + (row * st->dib_w + x) * 4;
}

static void fill_dim(struct picker_state *st)
{
	uint8_t *p = (uint8_t *)st->bits;
	for (int i = 0; i < st->dib_w * st->dib_h; i++) {
		p[i * 4 + 0] = 0;
		p[i * 4 + 1] = 0;
		p[i * 4 + 2] = 0;
		p[i * 4 + 3] = DIM_ALPHA;
	}
}

static void clear_rect(struct picker_state *st, RECT screen_rect)
{
	int x0 = screen_rect.left - st->vx;
	int y0 = screen_rect.top - st->vy;
	int x1 = screen_rect.right - st->vx;
	int y1 = screen_rect.bottom - st->vy;

	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > st->dib_w)
		x1 = st->dib_w;
	if (y1 > st->dib_h)
		y1 = st->dib_h;

	for (int y = y0; y < y1; y++) {
		for (int x = x0; x < x1; x++) {
			uint8_t *p = pixel_at(st, x, y);
			p[0] = 0;
			p[1] = 0;
			p[2] = 0;
			p[3] = 0;
		}
	}
}

static void draw_border(struct picker_state *st, RECT screen_rect)
{
	int x0 = screen_rect.left - st->vx;
	int y0 = screen_rect.top - st->vy;
	int x1 = screen_rect.right - st->vx;
	int y1 = screen_rect.bottom - st->vy;

	for (int t = 0; t < BORDER_PX; t++) {
		int lx = x0 - t;
		int rx = x1 + t - 1;
		int ty = y0 - t;
		int by = y1 + t - 1;

		for (int x = lx; x <= rx; x++) {
			if (x >= 0 && x < st->dib_w) {
				if (ty >= 0 && ty < st->dib_h) {
					uint8_t *p = pixel_at(st, x, ty);
					p[0] = 90;
					p[1] = 200;
					p[2] = 255;
					p[3] = 255;
				}
				if (by >= 0 && by < st->dib_h) {
					uint8_t *p = pixel_at(st, x, by);
					p[0] = 90;
					p[1] = 200;
					p[2] = 255;
					p[3] = 255;
				}
			}
		}
		for (int y = ty; y <= by; y++) {
			if (y >= 0 && y < st->dib_h) {
				if (lx >= 0 && lx < st->dib_w) {
					uint8_t *p = pixel_at(st, lx, y);
					p[0] = 90;
					p[1] = 200;
					p[2] = 255;
					p[3] = 255;
				}
				if (rx >= 0 && rx < st->dib_w) {
					uint8_t *p = pixel_at(st, rx, y);
					p[0] = 90;
					p[1] = 200;
					p[2] = 255;
					p[3] = 255;
				}
			}
		}
	}
}

static void present(HWND hwnd, struct picker_state *st)
{
	POINT dst = {st->vx, st->vy};
	POINT src = {0, 0};
	SIZE size = {st->dib_w, st->dib_h};
	BLENDFUNCTION blend = {.BlendOp = AC_SRC_OVER,
			       .BlendFlags = 0,
			       .SourceConstantAlpha = 255,
			       .AlphaFormat = AC_SRC_ALPHA};
	HDC src_dc = st->dib_dc;
	UpdateLayeredWindow(hwnd, NULL, &dst, &size, src_dc, &src, 0, &blend, ULW_ALPHA);
}

static void render(HWND hwnd)
{
	struct picker_state *st = get_state(hwnd);
	if (!st)
		return;

	fill_dim(st);
	if (st->dragging) {
		clear_rect(st, st->rect);
		draw_border(st, st->rect);
	}
	present(hwnd, st);
}

static LRESULT CALLBACK picker_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	struct picker_state *st = get_state(hwnd);

	switch (msg) {
	case WM_SETCURSOR:
		SetCursor(LoadCursor(NULL, IDC_CROSS));
		return TRUE;

	case WM_LBUTTONDOWN: {
		POINT p;
		GetCursorPos(&p);
		st->dragging = TRUE;
		st->origin = p;
		st->rect.left = p.x;
		st->rect.top = p.y;
		st->rect.right = p.x + 1;
		st->rect.bottom = p.y + 1;
		SetCapture(hwnd);
		render(hwnd);
		return 0;
	}

	case WM_MOUSEMOVE:
		if (st && st->dragging) {
			POINT p;
			GetCursorPos(&p);
			normalize_rect(st->origin, p, &st->rect);
			render(hwnd);
		}
		return 0;

	case WM_LBUTTONUP:
		if (st && st->dragging) {
			ReleaseCapture();
			st->dragging = FALSE;
			st->finished = TRUE;
			st->done = TRUE;
			PostMessageW(hwnd, WM_PICKER_DONE, 0, 0);
		}
		return 0;

	case WM_RBUTTONDOWN:
		if (st) {
			st->cancelled = TRUE;
			st->done = TRUE;
			PostMessageW(hwnd, WM_PICKER_DONE, 0, 0);
		}
		return 0;

	case WM_KEYDOWN:
		if (st && wp == VK_ESCAPE) {
			st->cancelled = TRUE;
			st->done = TRUE;
			PostMessageW(hwnd, WM_PICKER_DONE, 0, 0);
		}
		return 0;

	case WM_CLOSE:
		if (st) {
			st->cancelled = TRUE;
			st->done = TRUE;
			PostMessageW(hwnd, WM_PICKER_DONE, 0, 0);
		}
		return 0;

	case WM_PICKER_DONE:
		return 0;
	}

	return DefWindowProcW(hwnd, msg, wp, lp);
}

static void register_class(void)
{
	static BOOL registered = FALSE;
	if (registered)
		return;

	WNDCLASSEXW wc = {0};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = picker_proc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = L"OBSRegionPicker";
	wc.hCursor = LoadCursor(NULL, IDC_CROSS);
	wc.hbrBackground = NULL;
	RegisterClassExW(&wc);
	registered = TRUE;
}

static void destroy_dib(struct picker_state *st)
{
	if (st->dib) {
		DeleteObject(st->dib);
		st->dib = NULL;
	}
	if (st->dib_dc) {
		DeleteDC(st->dib_dc);
		st->dib_dc = NULL;
	}
	st->bits = NULL;
}

static bool create_dib(struct picker_state *st)
{
	BITMAPINFO bi = {0};
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = st->dib_w;
	bi.bmiHeader.biHeight = st->dib_h; /* bottom-up */
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;

	st->dib_dc = CreateCompatibleDC(NULL);
	if (!st->dib_dc)
		return false;

	st->dib = CreateDIBSection(st->dib_dc, &bi, DIB_RGB_COLORS, &st->bits, NULL, 0);
	if (!st->dib) {
		destroy_dib(st);
		return false;
	}
	SelectObject(st->dib_dc, st->dib);
	return true;
}

bool region_picker_select(struct region_picker_result *out)
{
	if (!out)
		return false;

	register_class();

	struct picker_state st = {0};
	st.vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
	st.vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
	st.dib_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	st.dib_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
	if (st.dib_w <= 0 || st.dib_h <= 0)
		return false;

	if (!create_dib(&st))
		return false;

	HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"OBSRegionPicker",
				   L"OBS Region Picker", WS_POPUP, st.vx, st.vy, st.dib_w, st.dib_h, NULL, NULL,
				   GetModuleHandleW(NULL), NULL);
	if (!hwnd) {
		destroy_dib(&st);
		return false;
	}

	SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&st);
	ShowWindow(hwnd, SW_SHOWNORMAL);
	render(hwnd);
	SetForegroundWindow(hwnd);

	MSG msg;
	while (GetMessageW(&msg, hwnd, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
		if (st.done)
			break;
	}

	bool ok = false;
	if (st.finished && !st.cancelled) {
		long w = st.rect.right - st.rect.left;
		long h = st.rect.bottom - st.rect.top;
		if (w >= 4 && h >= 4) {
			POINT center = {(st.rect.left + st.rect.right) / 2,
					 (st.rect.top + st.rect.bottom) / 2};
			out->monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
			out->x = st.rect.left;
			out->y = st.rect.top;
			out->w = w;
			out->h = h;
			ok = true;
		}
	}

	DestroyWindow(hwnd);
	destroy_dib(&st);

	return ok;
}
