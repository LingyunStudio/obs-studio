#include <windows.h>

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/dstr.h>
#include <util/threading.h>
#include <util/platform.h>

#include "cursor-capture.h"
#include "region-picker.h"

#define do_log(level, format, ...) \
	blog(level, "[region-capture: '%s'] " format, obs_source_get_name(capture->source), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

/* clang-format off */

#define TEXT_REGION_CAPTURE   obs_module_text("ScreenRegion")
#define TEXT_CAPTURE_CURSOR   obs_module_text("CaptureCursor")
#define TEXT_MONITOR          obs_module_text("Monitor")
#define TEXT_PRIMARY_MONITOR  obs_module_text("PrimaryMonitor")
#define TEXT_FORCE_SDR        obs_module_text("ForceSdr")
#define TEXT_REGION_X         obs_module_text("Region.X")
#define TEXT_REGION_Y         obs_module_text("Region.Y")
#define TEXT_REGION_W         obs_module_text("Region.Width")
#define TEXT_REGION_H         obs_module_text("Region.Height")
#define TEXT_SELECT_REGION    obs_module_text("SelectRegion")
#define TEXT_APPLY_OUTPUT     obs_module_text("ApplyOutputSize")
#define TEXT_SELECT_DISPLAY   obs_module_text("SelectADisplay")
#define TEXT_AUDIO_DESKTOP    obs_module_text("RegionAudio.Desktop")

/* clang-format on */

#define RESET_INTERVAL_SEC 3.0f

#define INVALID_DISPLAY "DUMMY"

struct region_capture {
	obs_source_t *source;
	pthread_mutex_t update_mutex;
	char monitor_id[128];
	char id[128];
	char alt_id[128];
	char monitor_name[64];
	HMONITOR handle;
	bool capture_cursor;
	bool force_sdr;
	bool showing;

	LONG x;        /* monitor origin in virtual desktop */
	LONG y;
	int rot;
	uint32_t width;  /* monitor texture width */
	uint32_t height; /* monitor texture height */

	long region_x;
	long region_y;
	long region_w;
	long region_h;

	gs_duplicator_t *duplicator;
	float reset_timeout;
	struct cursor_data cursor_data;
};

struct region_monitor_info {
	char device_id[128];
	char id[128];
	char alt_id[128];
	char name[128];
	RECT rect;
	HMONITOR handle;
};

/* ------------------------------------------------------------------------- */
/* Monitor enumeration (mirrors duplicator-monitor-capture)                  */

static bool GetMonitorTarget(LPCWSTR device, DISPLAYCONFIG_TARGET_DEVICE_NAME *target)
{
	bool found = false;

	UINT32 numPath, numMode;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numPath, &numMode) == ERROR_SUCCESS) {
		if (!numPath || !numMode) {
			return false;
		}
		DISPLAYCONFIG_PATH_INFO *paths = bmalloc(numPath * sizeof(DISPLAYCONFIG_PATH_INFO));
		DISPLAYCONFIG_MODE_INFO *modes = bmalloc(numMode * sizeof(DISPLAYCONFIG_MODE_INFO));
		if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &numPath, paths, &numMode, modes, NULL) ==
		    ERROR_SUCCESS) {
			for (size_t i = 0; i < numPath; ++i) {
				const DISPLAYCONFIG_PATH_INFO *const path = &paths[i];

				DISPLAYCONFIG_SOURCE_DEVICE_NAME source;
				source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
				source.header.size = sizeof(source);
				source.header.adapterId = path->sourceInfo.adapterId;
				source.header.id = path->sourceInfo.id;
				if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
				    wcscmp(device, source.viewGdiDeviceName) == 0) {
					target->header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
					target->header.size = sizeof(*target);
					target->header.adapterId = path->sourceInfo.adapterId;
					target->header.id = path->targetInfo.id;
					found = DisplayConfigGetDeviceInfo(&target->header) == ERROR_SUCCESS;
					break;
				}
			}
		}

		bfree(modes);
		bfree(paths);
	}

	return found;
}

static void GetMonitorName(HMONITOR handle, char *name, size_t count)
{
	MONITORINFOEXW mi;
	DISPLAYCONFIG_TARGET_DEVICE_NAME target;

	mi.cbSize = sizeof(mi);
	if (GetMonitorInfoW(handle, (LPMONITORINFO)&mi) && GetMonitorTarget(mi.szDevice, &target)) {
		char *friendly_name;
		os_wcs_to_utf8_ptr(target.monitorFriendlyDeviceName, 0, &friendly_name);

		strcpy_s(name, count, friendly_name);
		bfree(friendly_name);
	} else {
		strcpy_s(name, count, "[OBS: Unknown]");
	}
}

static BOOL CALLBACK enum_monitor(HMONITOR handle, HDC hdc, LPRECT rect, LPARAM param)
{
	UNUSED_PARAMETER(hdc);

	struct region_monitor_info *monitor = (struct region_monitor_info *)param;

	bool match = false;

	MONITORINFOEXA mi;
	mi.cbSize = sizeof(mi);
	if (GetMonitorInfoA(handle, (LPMONITORINFO)&mi)) {
		DISPLAY_DEVICEA device;
		device.cb = sizeof(device);
		if (EnumDisplayDevicesA(mi.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME)) {
			match = strcmp(monitor->device_id, device.DeviceID) == 0;
			if (match) {
				strcpy_s(monitor->id, _countof(monitor->id), device.DeviceID);
				strcpy_s(monitor->alt_id, _countof(monitor->alt_id), mi.szDevice);
				GetMonitorName(handle, monitor->name, _countof(monitor->name));
				monitor->rect = *rect;
				monitor->handle = handle;
			}
		}
	}

	return !match;
}

static BOOL CALLBACK enum_monitor_fallback(HMONITOR handle, HDC hdc, LPRECT rect, LPARAM param)
{
	UNUSED_PARAMETER(hdc);

	struct region_monitor_info *monitor = (struct region_monitor_info *)param;

	bool match = false;

	MONITORINFOEXA mi;
	mi.cbSize = sizeof(mi);
	if (GetMonitorInfoA(handle, (LPMONITORINFO)&mi)) {
		match = strcmp(monitor->device_id, mi.szDevice) == 0;
		if (match) {
			strcpy_s(monitor->alt_id, _countof(monitor->alt_id), mi.szDevice);
			GetMonitorName(handle, monitor->name, _countof(monitor->name));
			monitor->rect = *rect;
			monitor->handle = handle;
		}
	}

	return !match;
}

static struct region_monitor_info find_monitor(const char *monitor_id)
{
	struct region_monitor_info monitor = {0};
	strcpy_s(monitor.device_id, _countof(monitor.device_id), monitor_id);
	EnumDisplayMonitors(NULL, NULL, &enum_monitor, (LPARAM)&monitor);
	if (monitor.handle == NULL) {
		EnumDisplayMonitors(NULL, NULL, &enum_monitor_fallback, (LPARAM)&monitor);
	}

	return monitor;
}

static void log_settings(struct region_capture *capture, const char *monitor)
{
	info("update settings:\n"
	     "\tdisplay: %s\n"
	     "\tcursor: %s\n"
	     "\tregion: %ldx%ld @ %ld,%ld\n"
	     "\tid: %s\n"
	     "\talt_id: %s\n"
	     "\tforce SDR: %s",
	     monitor, capture->capture_cursor ? "true" : "false", capture->region_w, capture->region_h,
	     capture->region_x, capture->region_y, capture->id, capture->alt_id, capture->force_sdr ? "true" : "false");
}

static inline void clamp_region_to_monitor(struct region_capture *capture, LONG mw, LONG mh)
{
	/* monitor unknown/disconnected: keep the region as-is */
	if (mw <= 0 || mh <= 0)
		return;

	if (capture->region_x < 0)
		capture->region_x = 0;
	if (capture->region_y < 0)
		capture->region_y = 0;
	if (capture->region_w < 0)
		capture->region_w = 0;
	if (capture->region_h < 0)
		capture->region_h = 0;
	if (capture->region_x > mw)
		capture->region_x = mw;
	if (capture->region_y > mh)
		capture->region_y = mh;
	if (capture->region_x + capture->region_w > mw)
		capture->region_w = mw - capture->region_x;
	if (capture->region_y + capture->region_h > mh)
		capture->region_h = mh - capture->region_y;
}

static inline void update_settings(struct region_capture *capture, obs_data_t *settings)
{
	pthread_mutex_lock(&capture->update_mutex);

	struct region_monitor_info monitor = find_monitor(obs_data_get_string(settings, "monitor_id"));

	strcpy_s(capture->monitor_id, _countof(capture->monitor_id), monitor.device_id);
	strcpy_s(capture->id, _countof(capture->id), monitor.id);
	strcpy_s(capture->alt_id, _countof(capture->alt_id), monitor.alt_id);
	strcpy_s(capture->monitor_name, _countof(capture->monitor_name), monitor.name);
	capture->handle = monitor.handle;

	capture->capture_cursor = obs_data_get_bool(settings, "capture_cursor");
	capture->force_sdr = obs_data_get_bool(settings, "force_sdr");

	capture->region_x = (long)obs_data_get_int(settings, "region_x");
	capture->region_y = (long)obs_data_get_int(settings, "region_y");
	capture->region_w = (long)obs_data_get_int(settings, "region_w");
	capture->region_h = (long)obs_data_get_int(settings, "region_h");

	LONG mw = monitor.rect.right - monitor.rect.left;
	LONG mh = monitor.rect.bottom - monitor.rect.top;
	clamp_region_to_monitor(capture, mw, mh);

	if (capture->duplicator) {
		obs_enter_graphics();

		gs_duplicator_destroy(capture->duplicator);
		capture->duplicator = NULL;

		obs_leave_graphics();
	}

	capture->width = 0;
	capture->height = 0;
	capture->x = 0;
	capture->y = 0;
	capture->rot = 0;
	capture->reset_timeout = RESET_INTERVAL_SEC;

	pthread_mutex_unlock(&capture->update_mutex);
}

/* ------------------------------------------------------------------------- */

static const char *region_capture_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return TEXT_REGION_CAPTURE;
}

static void region_actual_destroy(void *data)
{
	struct region_capture *capture = data;

	obs_enter_graphics();

	if (capture->duplicator) {
		gs_duplicator_destroy(capture->duplicator);
		capture->duplicator = NULL;
	}

	cursor_data_free(&capture->cursor_data);

	obs_leave_graphics();

	pthread_mutex_destroy(&capture->update_mutex);

	bfree(capture);
}

static void region_capture_destroy(void *data)
{
	obs_queue_task(OBS_TASK_GRAPHICS, region_actual_destroy, data, false);
}

static void region_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "monitor_id", INVALID_DISPLAY);
	obs_data_set_default_int(settings, "region_x", 0);
	obs_data_set_default_int(settings, "region_y", 0);
	obs_data_set_default_int(settings, "region_w", 0);
	obs_data_set_default_int(settings, "region_h", 0);
	obs_data_set_default_bool(settings, "capture_cursor", true);
	obs_data_set_default_bool(settings, "force_sdr", false);
}

static void region_capture_update(void *data, obs_data_t *settings)
{
	struct region_capture *capture = data;
	update_settings(capture, settings);
	log_settings(capture, capture->monitor_name);
}

static void *region_capture_create(obs_data_t *settings, obs_source_t *source)
{
	struct region_capture *capture;

	capture = bzalloc(sizeof(struct region_capture));
	capture->source = source;

	pthread_mutex_init(&capture->update_mutex, NULL);

	update_settings(capture, settings);
	log_settings(capture, capture->monitor_name);

	return capture;
}

static void reset_capture_data(struct region_capture *capture)
{
	struct gs_monitor_info monitor_info = {0};
	gs_texture_t *texture = gs_duplicator_get_texture(capture->duplicator);

	const int dxgi_index = gs_duplicator_get_monitor_index(capture->handle);
	gs_get_duplicator_monitor_info(dxgi_index, &monitor_info);
	if (texture) {
		capture->width = gs_texture_get_width(texture);
		capture->height = gs_texture_get_height(texture);
	} else {
		capture->width = 0;
		capture->height = 0;
	}
	capture->x = monitor_info.x;
	capture->y = monitor_info.y;
	capture->rot = monitor_info.rotation_degrees;

	/* NOTE: do NOT clamp the region here. The texture is NULL for the first
	 * frame(s) after duplicator creation (AcquireNextFrame timeout), which
	 * would clamp the region to 0x0 and permanently break the source. The
	 * region is only clamped to monitor bounds in update_settings. */
}

static void free_capture_data(struct region_capture *capture)
{
	if (capture->duplicator) {
		gs_duplicator_destroy(capture->duplicator);
		capture->duplicator = NULL;
	}

	cursor_data_free(&capture->cursor_data);

	capture->width = 0;
	capture->height = 0;
	capture->x = 0;
	capture->y = 0;
	capture->rot = 0;
	capture->reset_timeout = 0.0f;
}

static void update_monitor_handle(struct region_capture *capture)
{
	capture->handle = find_monitor(capture->monitor_id).handle;
}

static void region_capture_tick(void *data, float seconds)
{
	struct region_capture *capture = data;

	if (!obs_source_showing(capture->source)) {
		if (capture->showing) {
			obs_enter_graphics();
			free_capture_data(capture);
			obs_leave_graphics();

			capture->showing = false;
		}
		return;
	}

	if (!capture->showing)
		capture->reset_timeout = RESET_INTERVAL_SEC;

	obs_enter_graphics();

	if (!capture->duplicator) {
		capture->reset_timeout += seconds;

		if (capture->reset_timeout >= RESET_INTERVAL_SEC) {
			if (!capture->handle)
				update_monitor_handle(capture);

			if (capture->handle) {
				int dxgi_index = gs_duplicator_get_monitor_index(capture->handle);

				if (dxgi_index == -1) {
					update_monitor_handle(capture);

					if (capture->handle) {
						dxgi_index = gs_duplicator_get_monitor_index(capture->handle);
					}
				}

				if (dxgi_index != -1) {
					capture->duplicator = gs_duplicator_create(dxgi_index);
				}
			}

			capture->reset_timeout = 0.0f;
		}
	}

	if (capture->duplicator) {
		if (capture->capture_cursor)
			cursor_capture(&capture->cursor_data);

		if (!gs_duplicator_update_frame(capture->duplicator)) {
			free_capture_data(capture);

		} else if (capture->width == 0) {
			reset_capture_data(capture);
		}
	}

	obs_leave_graphics();

	if (!capture->showing)
		capture->showing = true;
}

static uint32_t region_capture_width(void *data)
{
	struct region_capture *capture = data;
	long w = capture->region_w;
	return w > 0 ? (uint32_t)w : 0;
}

static uint32_t region_capture_height(void *data)
{
	struct region_capture *capture = data;
	long h = capture->region_h;
	return h > 0 ? (uint32_t)h : 0;
}

static void region_capture_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);

	struct region_capture *capture = data;

	if (!capture->duplicator)
		return;
	if (capture->region_w <= 0 || capture->region_h <= 0)
		return;

	gs_texture_t *const texture = gs_duplicator_get_texture(capture->duplicator);
	if (!texture)
		return;

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_enable_blending(false);

	const char *tech_name = "Draw";
	float multiplier = 1.f;
	const enum gs_color_space current_space = gs_get_color_space();
	if (gs_duplicator_get_color_space(capture->duplicator) == GS_CS_709_SCRGB) {
		if (capture->force_sdr) {
			tech_name = "DrawMultiply";
			const float target_nits =
				(current_space == GS_CS_709_SCRGB) ? obs_get_video_sdr_white_level() : 80.f;
			multiplier = target_nits / gs_duplicator_get_sdr_white_level(capture->duplicator);
		} else {
			switch (current_space) {
			case GS_CS_SRGB:
			case GS_CS_SRGB_16F:
				tech_name = "DrawMultiplyTonemap";
				multiplier = 80.f / obs_get_video_sdr_white_level();
				break;
			case GS_CS_709_EXTENDED:
				tech_name = "DrawMultiply";
				multiplier = 80.f / obs_get_video_sdr_white_level();
			}
		}
	} else if (current_space == GS_CS_709_SCRGB) {
		tech_name = "DrawMultiply";
		multiplier = obs_get_video_sdr_white_level() / 80.f;
	}

	gs_effect_t *const opaque_effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);
	gs_eparam_t *multiplier_param = gs_effect_get_param_by_name(opaque_effect, "multiplier");
	gs_effect_set_float(multiplier_param, multiplier);
	gs_eparam_t *image_param = gs_effect_get_param_by_name(opaque_effect, "image");
	gs_effect_set_texture_srgb(image_param, texture);

	while (gs_effect_loop(opaque_effect, tech_name)) {
		gs_draw_sprite_subregion(texture, 0, (uint32_t)capture->region_x, (uint32_t)capture->region_y,
					 (uint32_t)capture->region_w, (uint32_t)capture->region_h);
	}

	gs_enable_blending(true);
	gs_enable_framebuffer_srgb(previous);

	if (capture->capture_cursor) {
		gs_effect_t *const default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);

		while (gs_effect_loop(default_effect, "Draw")) {
			cursor_draw(&capture->cursor_data, -(capture->x + capture->region_x),
				    -(capture->y + capture->region_y), capture->region_w, capture->region_h);
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Properties                                                                 */

static BOOL CALLBACK enum_monitor_props(HMONITOR handle, HDC hdc, LPRECT rect, LPARAM param)
{
	UNUSED_PARAMETER(hdc);
	UNUSED_PARAMETER(rect);

	char monitor_name[64];
	GetMonitorName(handle, monitor_name, sizeof(monitor_name));

	MONITORINFOEXA mi;
	mi.cbSize = sizeof(mi);
	if (GetMonitorInfoA(handle, (LPMONITORINFO)&mi)) {
		obs_property_t *monitor_list = (obs_property_t *)param;
		struct dstr monitor_desc = {0};
		dstr_printf(&monitor_desc, "%s: %dx%d @ %d,%d", monitor_name, mi.rcMonitor.right - mi.rcMonitor.left,
			    mi.rcMonitor.bottom - mi.rcMonitor.top, mi.rcMonitor.left, mi.rcMonitor.top);
		if (mi.dwFlags == MONITORINFOF_PRIMARY)
			dstr_catf(&monitor_desc, " (%s)", TEXT_PRIMARY_MONITOR);

		DISPLAY_DEVICEA device;
		device.cb = sizeof(device);
		if (EnumDisplayDevicesA(mi.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME)) {
			obs_property_list_add_string(monitor_list, monitor_desc.array, device.DeviceID);
		} else {
			obs_property_list_add_string(monitor_list, monitor_desc.array, mi.szDevice);
		}

		dstr_free(&monitor_desc);
	}

	return TRUE;
}

/* Moves the scene item of this source in the current scene to the top-left
 * corner at 1:1 scale so that the captured region fills the whole canvas. */
struct fit_item_search {
	obs_source_t *source;
	obs_sceneitem_t *item;
};

static bool find_sceneitem_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);

	struct fit_item_search *search = param;
	if (obs_sceneitem_get_source(item) == search->source) {
		search->item = item;
		return false;
	}

	return true;
}

static void fit_source_to_canvas(obs_source_t *source)
{
	obs_source_t *scene_source = obs_frontend_get_current_scene();
	if (!scene_source)
		return;

	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (scene) {
		struct fit_item_search search = {source, NULL};
		obs_scene_enum_items(scene, find_sceneitem_cb, &search);

		if (search.item) {
			struct vec2 pos = {0.0f, 0.0f};
			struct vec2 scale = {1.0f, 1.0f};

			obs_sceneitem_set_bounds_type(search.item, OBS_BOUNDS_NONE);
			obs_sceneitem_set_scale(search.item, &scale);
			obs_sceneitem_set_rot(search.item, 0.0f);
			obs_sceneitem_set_pos(search.item, &pos);
		}
	}

	obs_source_release(scene_source);
}

static bool apply_region_as_output(struct region_capture *capture, long rw, long rh)
{
	/* obs aligns the output width to a multiple of 4 and the output height
	 * to a multiple of 2 (see obs_reset_video), and encoders require even
	 * dimensions anyway; round here so that base == output (min 32x32) */
	rw &= ~3L;
	rh &= ~1L;

	if (rw < 32 || rh < 32) {
		if (capture)
			warn("Cannot apply output size: region is empty or too small (minimum 32x32). Select a region first.");
		else
			blog(LOG_WARNING, "[region-capture] Cannot apply output size: region is empty or too small (minimum 32x32).");
		return false;
	}

	if (obs_frontend_recording_active() || obs_frontend_streaming_active() ||
	    obs_frontend_virtualcam_active() || obs_frontend_replay_buffer_active()) {
		if (capture)
			warn("Cannot apply output size while recording, streaming, replay buffer, or virtual camera is active.");
		else
			blog(LOG_WARNING, "[region-capture] Cannot apply output size while recording, streaming, replay buffer, or virtual camera is active.");
		return false;
	}

	config_t *config = obs_frontend_get_profile_config();
	if (!config) {
		warn("Cannot apply output size: no active profile config.");
		return false;
	}

	/* remember the normal canvas so the frontend region canvas tracker can
	 * restore it when switching to a scene without a region source */
	config_t *user = obs_frontend_get_user_config();
	if (user && !config_get_bool(user, "BasicWindow", "RegionTrackApplied")) {
		config_set_uint(user, "BasicWindow", "RegionTrackBaseCX", config_get_uint(config, "Video", "BaseCX"));
		config_set_uint(user, "BasicWindow", "RegionTrackBaseCY", config_get_uint(config, "Video", "BaseCY"));
		config_set_uint(user, "BasicWindow", "RegionTrackOutCX", config_get_uint(config, "Video", "OutputCX"));
		config_set_uint(user, "BasicWindow", "RegionTrackOutCY", config_get_uint(config, "Video", "OutputCY"));
		config_set_bool(user, "BasicWindow", "RegionTrackApplied", true);
		config_save_safe(user, "tmp", NULL);
	}

	config_set_uint(config, "Video", "BaseCX", (uint64_t)rw);
	config_set_uint(config, "Video", "BaseCY", (uint64_t)rh);
	config_set_uint(config, "Video", "OutputCX", (uint64_t)rw);
	config_set_uint(config, "Video", "OutputCY", (uint64_t)rh);

	/* persist the video settings (obs_frontend_save only saves the scene
	 * collection, not the profile config) and apply immediately */
	config_save_safe(config, "tmp", NULL);
	obs_frontend_save();
	obs_frontend_reset_video();

	blog(LOG_INFO, "[region-capture] Applied region size %ldx%ld to canvas and output", rw, rh);
	return true;
}

static void ensure_default_audio_in_current_scene(void);

static bool apply_region_and_fit(struct region_capture *capture, long rw, long rh)
{
	if (!apply_region_as_output(capture, rw, rh))
		return false;

	fit_source_to_canvas(capture->source);
	ensure_default_audio_in_current_scene();
	return true;
}

/* Region recordings are usually started in a hurry, so make sure the scene
 * has default audio (desktop + mic) instead of recording silently. */
struct audio_source_search {
	const char *source_id;
	bool found;
};

static bool find_audio_source_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);

	struct audio_source_search *search = param;
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (source && strcmp(obs_source_get_id(source), search->source_id) == 0) {
		search->found = true;
		return false;
	}
	return true;
}

static void ensure_audio_source(obs_scene_t *scene, const char *source_id, const char *name)
{
	struct audio_source_search search = {source_id, false};
	obs_scene_enum_items(scene, find_audio_source_cb, &search);
	if (search.found)
		return;

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "device_id", "default");
	obs_source_t *source = obs_source_create(source_id, name, settings, NULL);
	if (source) {
		obs_scene_add(scene, source);
		obs_source_release(source);
	}
	obs_data_release(settings);
}

static void ensure_default_audio_in_current_scene(void)
{
	obs_source_t *scene_source = obs_frontend_get_current_scene();
	if (!scene_source)
		return;

	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (scene) {
		ensure_audio_source(scene, "wasapi_output_capture", TEXT_AUDIO_DESKTOP);
	}

	obs_source_release(scene_source);
}

static bool get_monitor_id_string(HMONITOR handle, char *monitor_id, size_t monitor_id_size, RECT *rect_out)
{
	MONITORINFOEXA mi;
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoA(handle, (LPMONITORINFO)&mi))
		return false;

	DISPLAY_DEVICEA device;
	device.cb = sizeof(device);
	if (EnumDisplayDevicesA(mi.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME)) {
		strcpy_s(monitor_id, monitor_id_size, device.DeviceID);
	} else {
		strcpy_s(monitor_id, monitor_id_size, mi.szDevice);
	}

	if (rect_out)
		*rect_out = mi.rcMonitor;
	return true;
}

/* Runs the region picker and returns the monitor id, the region clamped to
 * monitor bounds (monitor-relative), and the region's absolute position in
 * virtual-desktop coordinates. */
static bool pick_region(char *monitor_id, size_t monitor_id_size, long *rx, long *ry, long *rw, long *rh,
			long *abs_x, long *abs_y)
{
	struct region_picker_result res;
	if (!region_picker_select(&res))
		return false;

	RECT rc;
	if (!get_monitor_id_string(res.monitor, monitor_id, monitor_id_size, &rc))
		return false;

	long mx = rc.left;
	long my = rc.top;
	long mw = rc.right - rc.left;
	long mh = rc.bottom - rc.top;

	long x = res.x - mx;
	long y = res.y - my;
	long w = res.w;
	long h = res.h;

	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > mw)
		w = mw - x;
	if (y + h > mh)
		h = mh - y;

	if (w < 1 || h < 1) {
		blog(LOG_WARNING, "[region-capture] Selected region is empty after clamping to monitor bounds");
		return false;
	}

	*rx = x;
	*ry = y;
	*rw = w;
	*rh = h;
	/* absolute position of the clamped region in virtual-desktop coordinates */
	*abs_x = mx + x;
	*abs_y = my + y;
	return true;
}

static bool select_region_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);

	struct region_capture *capture = data;
	if (!capture || !capture->source)
		return false;

	char monitor_id[128] = {0};
	long rx, ry, rw, rh, abs_x, abs_y;
	if (!pick_region(monitor_id, _countof(monitor_id), &rx, &ry, &rw, &rh, &abs_x, &abs_y))
		return false;

	obs_data_t *settings = obs_source_get_settings(capture->source);
	obs_data_set_string(settings, "monitor_id", monitor_id);
	obs_data_set_int(settings, "region_x", rx);
	obs_data_set_int(settings, "region_y", ry);
	obs_data_set_int(settings, "region_w", rw);
	obs_data_set_int(settings, "region_h", rh);
	obs_source_update(capture->source, settings);
	obs_data_release(settings);

	/* make the recorded output exactly the selected region */
	apply_region_and_fit(capture, rw, rh);

	return true;
}

/* proc handler for the frontend floating widget: lets the user drag-select a
 * region and reports back monitor id, clamped region and absolute position. */
static void region_picker_proc(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);

	char monitor_id[128] = {0};
	long rx, ry, rw, rh, abs_x, abs_y;
	bool ok = pick_region(monitor_id, _countof(monitor_id), &rx, &ry, &rw, &rh, &abs_x, &abs_y);

	calldata_set_bool(cd, "success", ok);
	if (ok) {
		calldata_set_string(cd, "monitor_id", monitor_id);
		calldata_set_int(cd, "x", rx);
		calldata_set_int(cd, "y", ry);
		calldata_set_int(cd, "width", rw);
		calldata_set_int(cd, "height", rh);
		calldata_set_int(cd, "abs_x", abs_x);
		calldata_set_int(cd, "abs_y", abs_y);

		/* auto-apply the region as canvas/output size */
		apply_region_as_output(NULL, rw, rh);
	}
}

/* proc handler that maps an absolute virtual-desktop rect to the monitor it
 * is on and clamps it to that monitor's bounds. Used by the frontend floating
 * widget after the user has adjusted the region frame. */
static void region_clamp_proc(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);

	long ax = (long)calldata_int(cd, "x");
	long ay = (long)calldata_int(cd, "y");
	long aw = (long)calldata_int(cd, "width");
	long ah = (long)calldata_int(cd, "height");

	POINT center = {ax + aw / 2, ay + ah / 2};
	HMONITOR mon = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);

	char monitor_id[128] = {0};
	RECT rc;
	bool ok = get_monitor_id_string(mon, monitor_id, _countof(monitor_id), &rc);
	if (ok) {
		long mx = rc.left;
		long my = rc.top;
		long mw = rc.right - rc.left;
		long mh = rc.bottom - rc.top;

		long x = ax - mx;
		long y = ay - my;
		long w = aw;
		long h = ah;

		if (x < 0) {
			w += x;
			x = 0;
		}
		if (y < 0) {
			h += y;
			y = 0;
		}
		if (x + w > mw)
			w = mw - x;
		if (y + h > mh)
			h = mh - y;

		ok = w >= 1 && h >= 1;
		if (ok) {
			calldata_set_string(cd, "monitor_id", monitor_id);
			calldata_set_int(cd, "x", x);
			calldata_set_int(cd, "y", y);
			calldata_set_int(cd, "width", w);
			calldata_set_int(cd, "height", h);
			calldata_set_int(cd, "abs_x", mx + x);
			calldata_set_int(cd, "abs_y", my + y);
		}
	}

	calldata_set_bool(cd, "success", ok);
}

void region_capture_register_procs(void)
{
	proc_handler_t *ph = obs_get_proc_handler();
	proc_handler_add(ph, "void win_capture_region_picker_select()", region_picker_proc, NULL);
	proc_handler_add(ph, "void win_capture_region_clamp_to_monitor()", region_clamp_proc, NULL);
}

static bool apply_output_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);

	struct region_capture *capture = data;
	if (!capture)
		return false;

	/* read from settings: a deferred update may not have been applied to
	 * the capture struct yet */
	obs_data_t *settings = obs_source_get_settings(capture->source);
	long rw = (long)obs_data_get_int(settings, "region_w");
	long rh = (long)obs_data_get_int(settings, "region_h");
	obs_data_release(settings);

	apply_region_and_fit(capture, rw, rh);
	return false;
}

static obs_properties_t *region_capture_properties(void *data)
{
	struct region_capture *capture = data;

	obs_properties_t *props = obs_properties_create();

	obs_property_t *monitors = obs_properties_add_list(props, "monitor_id", TEXT_MONITOR, OBS_COMBO_TYPE_LIST,
							    OBS_COMBO_FORMAT_STRING);

	if (capture && strcmp(capture->monitor_id, INVALID_DISPLAY) == 0) {
		obs_property_list_add_string(monitors, TEXT_SELECT_DISPLAY, INVALID_DISPLAY);
		obs_property_list_item_disable(monitors, 0, true);
	}

	EnumDisplayMonitors(NULL, NULL, &enum_monitor_props, (LPARAM)monitors);

	obs_properties_add_int(props, "region_x", TEXT_REGION_X, 0, 65535, 1);
	obs_properties_add_int(props, "region_y", TEXT_REGION_Y, 0, 65535, 1);
	obs_properties_add_int(props, "region_w", TEXT_REGION_W, 0, 65535, 1);
	obs_properties_add_int(props, "region_h", TEXT_REGION_H, 0, 65535, 1);

	obs_properties_add_button2(props, "select_region", TEXT_SELECT_REGION, select_region_clicked, capture);
	obs_properties_add_button2(props, "apply_output", TEXT_APPLY_OUTPUT, apply_output_clicked, capture);

	obs_properties_add_bool(props, "capture_cursor", TEXT_CAPTURE_CURSOR);
	obs_properties_add_bool(props, "force_sdr", TEXT_FORCE_SDR);

	return props;
}

static enum gs_color_space region_capture_get_color_space(void *data, size_t count,
							  const enum gs_color_space *preferred_spaces)
{
	enum gs_color_space capture_space = GS_CS_SRGB;

	struct region_capture *capture = data;
	if (capture->duplicator && !capture->force_sdr) {
		capture_space = gs_duplicator_get_color_space(capture->duplicator);
	}

	enum gs_color_space space = capture_space;
	for (size_t i = 0; i < count; ++i) {
		const enum gs_color_space preferred_space = preferred_spaces[i];
		space = preferred_space;
		if (preferred_space == capture_space)
			break;
	}

	return space;
}

struct obs_source_info region_capture_info = {
	.id = "region_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_SRGB,
	.get_name = region_capture_getname,
	.create = region_capture_create,
	.destroy = region_capture_destroy,
	.video_render = region_capture_render,
	.video_tick = region_capture_tick,
	.update = region_capture_update,
	.get_width = region_capture_width,
	.get_height = region_capture_height,
	.get_defaults = region_capture_defaults,
	.get_properties = region_capture_properties,
	.icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE,
	.video_get_color_space = region_capture_get_color_space,
};
