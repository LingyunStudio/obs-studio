#include "auto-cleanup.hpp"
#include "auto-cleanup-dialog.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QMainWindow>

#include "qt-wrappers.hpp"

AutoCleanup *AutoCleanup::Instance()
{
	static AutoCleanup instance;
	return &instance;
}

AutoCleanup::AutoCleanup(QObject *parent) : QObject(parent) {}

AutoCleanup::~AutoCleanup()
{
	if (pollTimer) {
		pollTimer->stop();
		delete pollTimer;
		pollTimer = nullptr;
	}
}

void AutoCleanup::LoadConfig()
{
	config_t *config = obs_frontend_get_profile_config();

	/* default to deleting short clips (matches the UI checkbox) when the
	 * key has never been written */
	deleteShortClips = config_get_bool(config, "AutoCleanup", "DeleteShortClips");
	if (!config_has_user_value(config, "AutoCleanup", "DeleteShortClips"))
		deleteShortClips = true;

	deleteOriginAfterRemux = config_get_bool(config, "AutoCleanup", "DeleteOriginAfterRemux");
	shortClipThreshold = (int)config_get_int(config, "AutoCleanup", "ShortClipThreshold");

	if (shortClipThreshold < 1)
		shortClipThreshold = 10;
}

void AutoCleanup::OnFrontendEvent(enum obs_frontend_event event, void *param)
{
	AutoCleanup *self = static_cast<AutoCleanup *>(param);

	if (event == OBS_FRONTEND_EVENT_RECORDING_STARTED) {
		self->OnRecordingStarted();

	} else if (event == OBS_FRONTEND_EVENT_RECORDING_PAUSED) {
		self->OnRecordingPaused();

	} else if (event == OBS_FRONTEND_EVENT_RECORDING_UNPAUSED) {
		self->OnRecordingUnpaused();

	} else if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPED) {
		self->OnRecordingStopped();
	}
}

void AutoCleanup::OnRecordingStarted()
{
	recordStartMs = os_gettime_ns() / 1000000;
	pausedDurationMs = 0;
	pauseStartMs = 0;
	recordDurationMs = 0;

	/* a recording started (e.g. via the floating ball) while a remux poll
	 * for the previous recording was still running — abandon it */
	if (pollTimer) {
		pollTimer->stop();
		pollTimer->deleteLater();
		pollTimer = nullptr;
	}
}

void AutoCleanup::OnRecordingPaused()
{
	pauseStartMs = os_gettime_ns() / 1000000;
}

void AutoCleanup::OnRecordingUnpaused()
{
	if (pauseStartMs > 0) {
		pausedDurationMs += (os_gettime_ns() / 1000000) - pauseStartMs;
		pauseStartMs = 0;
	}
}

void AutoCleanup::OnRecordingStopped()
{
	/* finish an in-progress pause before measuring the duration */
	if (pauseStartMs > 0)
		OnRecordingUnpaused();

	qint64 endMs = os_gettime_ns() / 1000000;
	if (recordStartMs > 0)
		recordDurationMs = endMs - recordStartMs - pausedDurationMs;
	else
		recordDurationMs = 0;

	LoadConfig();
	if (!deleteShortClips && !deleteOriginAfterRemux)
		return;

	/* OBS may still be finalising the file when the event fires; probe
	 * half a second later without blocking the UI thread */
	QTimer::singleShot(500, this, &AutoCleanup::ProcessRecording);
}

/* Mirrors the remux decision in OBSBasic::AutoRemux: decides whether OBS will
 * actually remux the recording after it stops and, if so, the exact output
 * path it will produce. Returns an empty path when no remux will happen. */
QString AutoCleanup::ComputeRemuxOutput(bool *willRemux)
{
	if (willRemux)
		*willRemux = false;

	config_t *config = obs_frontend_get_profile_config();
	if (!config_get_bool(config, "Video", "AutoRemux"))
		return {};

	const char *mode = config_get_string(config, "Output", "Mode");
	bool simple = !mode || strcmp(mode, "Simple") == 0;

	/* advanced output with the FFmpeg custom output does not remux */
	if (!simple) {
		const char *recType = config_get_string(config, "AdvOut", "RecType");
		if (recType && astrcmpi(recType, "FFmpeg") == 0)
			return {};
	}

	QFileInfo fi(originPath);
	if (!fi.exists())
		return {};

	QString suffix = fi.suffix();

	/* lossless AVI is never remuxed */
	if (suffix.compare("avi", Qt::CaseInsensitive) == 0)
		return {};

	QString output = originPath;
	output.resize(output.size() - suffix.size());

	const char *format = config_get_string(config, simple ? "SimpleOutput" : "AdvOut", "RecFormat2");

	/* fragmented containers keep their original extension (the file is
	 * written as "<name>.remuxed.<ext>") */
	if (format && strncmp(format, "fragmented", 10) == 0) {
		if (willRemux)
			*willRemux = true;
		return output + "remuxed." + suffix;
	}

	/* ProRes is remuxed into a MOV container */
	const char *encoder = config_get_string(config, "AdvOut", "RecEncoder");
	if (!simple && encoder && strcmp(encoder, "prores") == 0) {
		if (willRemux)
			*willRemux = true;
		return output + "mov";
	}

	if (willRemux)
		*willRemux = true;
	return output + "mp4";
}

void AutoCleanup::ProcessRecording()
{
	char *lastRec = obs_frontend_get_last_recording();
	if (!lastRec || !*lastRec) {
		bfree(lastRec);
		return;
	}
	originPath = QString::fromUtf8(lastRec);
	bfree(lastRec);

	/* clean up any stale timer from a previous recording */
	if (pollTimer) {
		pollTimer->stop();
		pollTimer->deleteLater();
		pollTimer = nullptr;
	}

	bool isShort = recordDurationMs > 0 &&
		       recordDurationMs < (qint64)shortClipThreshold * 1000;

	if (isShort && deleteShortClips) {
		/* short clip: the original goes immediately (retries cope with
		 * the remuxer still holding it open); if a remux produces
		 * another file, delete that one once it finishes writing */
		DeleteWithRetry(originPath, 15);

		bool willRemux = false;
		remuxPath = ComputeRemuxOutput(&willRemux);
		if (willRemux && remuxPath != originPath)
			StartPolling(false, true);
		return;
	}

	/* normal recording, auto-remux active: delete the original only after
	 * the remuxed file has actually finished being written */
	if (deleteOriginAfterRemux) {
		bool willRemux = false;
		remuxPath = ComputeRemuxOutput(&willRemux);

		if (!willRemux) {
			/* OBS will not produce a remuxed file — keep the
			 * original rather than deleting the only copy */
			blog(LOG_INFO,
			     "[auto-cleanup] Auto-remux is on but this recording won't be remuxed; "
			     "keeping original: %s",
			     originPath.toUtf8().constData());
			return;
		}

		if (remuxPath == originPath) {
			/* same file (e.g. already mp4) — nothing extra to keep */
			return;
		}

		StartPolling(true, false);
	}
}

void AutoCleanup::StartPolling(bool deleteOriginOnComplete, bool deleteBothOnComplete)
{
	pendingOriginDelete = deleteOriginOnComplete;
	pendingShortDelete = deleteBothOnComplete;
	pollCount = 0;
	lastSeenSize = -1;

	pollTimer = new QTimer(this);
	pollTimer->setInterval(1000);
	connect(pollTimer, &QTimer::timeout, this, &AutoCleanup::CheckForRemux);
	pollTimer->start();
}

void AutoCleanup::CheckForRemux()
{
	pollCount++;

	if (QFileInfo::exists(remuxPath)) {
		/* The file can exist while the remuxer is still writing it.
		 * Wait until its size stops growing for two consecutive polls
		 * before acting on it. */
		qint64 size = QFileInfo(remuxPath).size();
		if (size > 0 && size == lastSeenSize) {
			pollTimer->stop();
			pollTimer->deleteLater();
			pollTimer = nullptr;
			HandleRemuxReady();
			return;
		}
		lastSeenSize = size;
	}

	/* 10 minutes without a stable remuxed file: the remux likely failed;
	 * never delete the original recording in that case */
	if (pollCount > 600) {
		pollTimer->stop();
		pollTimer->deleteLater();
		pollTimer = nullptr;
		HandleRemuxTimeout();
	}
}

void AutoCleanup::HandleRemuxReady()
{
	if (pendingShortDelete) {
		DeleteWithRetry(remuxPath, 15);
	} else if (pendingOriginDelete) {
		DeleteWithRetry(originPath, 30);
	}
}

void AutoCleanup::HandleRemuxTimeout()
{
	blog(LOG_WARNING, "[auto-cleanup] Remux file %s did not finish in time; "
			  "keeping original recording",
	     remuxPath.toUtf8().constData());
}

void AutoCleanup::DeleteWithRetry(const QString &path, int retriesLeft)
{
	if (path.isEmpty())
		return;

	if (retriesLeft <= 0) {
		blog(LOG_WARNING, "[auto-cleanup] Delete failed after retries: %s", path.toUtf8().constData());
		return;
	}

	if (!QFile::exists(path))
		return;

	QFile file(path);
	if (file.remove()) {
		blog(LOG_INFO, "[auto-cleanup] Deleted: %s", path.toUtf8().constData());
		return;
	}

	QTimer::singleShot(2000, this, [this, path, retriesLeft]() { DeleteWithRetry(path, retriesLeft - 1); });
}

/* ------------------------------------------------------------------------- */

static void ShowAutoCleanupDialog()
{
	auto *dlg = new AutoCleanupDialog((QMainWindow *)obs_frontend_get_main_window());
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->show();
}

extern "C" void InitAutoCleanup()
{
	AutoCleanup::Instance()->LoadConfig();
	obs_frontend_add_event_callback(AutoCleanup::OnFrontendEvent, AutoCleanup::Instance());

	QAction *action = (QAction *)obs_frontend_add_tools_menu_qaction(
		obs_module_text("AutoCleanup.MenuTitle"));
	if (action)
		QObject::connect(action, &QAction::triggered, []() { ShowAutoCleanupDialog(); });
}

extern "C" void FreeAutoCleanup()
{
	obs_frontend_remove_event_callback(AutoCleanup::OnFrontendEvent, AutoCleanup::Instance());
}
