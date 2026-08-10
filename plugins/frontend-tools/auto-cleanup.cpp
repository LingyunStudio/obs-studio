#include "auto-cleanup.hpp"
#include "auto-cleanup-dialog.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QMainWindow>

#include "qt-wrappers.hpp"

int AutoCleanup::pollCount = 0;

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
	deleteShortClips = config_get_bool(config, "AutoCleanup", "DeleteShortClips");
	deleteOriginAfterRemux = config_get_bool(config, "AutoCleanup", "DeleteOriginAfterRemux");
	shortClipThreshold = (int)config_get_int(config, "AutoCleanup", "ShortClipThreshold");

	if (shortClipThreshold < 1)
		shortClipThreshold = 10;
}

void AutoCleanup::OnFrontendEvent(enum obs_frontend_event event, void *param)
{
	AutoCleanup *self = static_cast<AutoCleanup *>(param);

	if (event == OBS_FRONTEND_EVENT_RECORDING_STARTED) {
		self->recordStartMs = os_gettime_ns() / 1000000;
		self->recordDurationMs = 0;

	} else if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPED) {
		self->recordDurationMs = (os_gettime_ns() / 1000000) - self->recordStartMs;
		self->OnRecordingStopped();
	}
}

void AutoCleanup::OnRecordingStopped()
{
	LoadConfig();
	if (!deleteShortClips && !deleteOriginAfterRemux)
		return;

	/* OBS may still be finalising the file when the event fires;
	 * wait half a second before probing */
	os_sleep_ms(500);

	/* Use the official API to get the exact last recording path */
	char *lastRec = obs_frontend_get_last_recording();
	if (!lastRec || !*lastRec) {
		bfree(lastRec);
		return;
	}
	originPath = QString::fromUtf8(lastRec);
	bfree(lastRec);

	/* try to handle files now — if a remux is pending, defer until the
	 * remuxed MP4 appears */
	TryHandleAfterRemux();
}

void AutoCleanup::TryHandleAfterRemux()
{
	config_t *config = obs_frontend_get_profile_config();

	/* clean up any stale timer from a previous recording */
	if (pollTimer) {
		pollTimer->stop();
		pollTimer->deleteLater();
		pollTimer = nullptr;
	}

	const char *mode = config_get_string(config, "Output", "Mode");
	bool simple = !mode || strcmp(mode, "Simple") == 0;

	bool autoRemux = config_get_bool(config, "Video", "AutoRemux");
	const char *recFormat = config_get_string(config, simple ? "SimpleOutput" : "AdvOut", "RecFormat2");

	remuxPath.clear();

	if (autoRemux && recFormat) {
		QFileInfo fi(originPath);
		QString baseName = fi.completeBaseName();
		QDir recDir = fi.absoluteDir();
		remuxPath = recDir.absoluteFilePath(baseName + ".mp4");
		if (remuxPath != originPath && !QFileInfo::exists(remuxPath)) {
			pollCount = 0;
			pollTimer = new QTimer(this);
			pollTimer->setInterval(1000);
			QObject::connect(pollTimer, &QTimer::timeout, this, &AutoCleanup::CheckForRemux);
			pollTimer->start();
			return;
		}
	}

	/* no remux pending — handle files now */
	HandleFiles();
}

void AutoCleanup::CheckForRemux()
{
	pollCount++;

	if (QFileInfo::exists(remuxPath)) {
		pollTimer->stop();
		pollTimer->deleteLater();
		pollTimer = nullptr;
		HandleFiles();
	} else if (pollCount > 600) {
		pollTimer->stop();
		pollTimer->deleteLater();
		pollTimer = nullptr;
		blog(LOG_WARNING, "[auto-cleanup] Remux timeout: %s never appeared",
		     remuxPath.toUtf8().constData());
		HandleFiles();
	}
}

void AutoCleanup::HandleFiles()
{
	if (recordDurationMs <= 0)
		return;

	bool isShort = recordDurationMs < (qint64)shortClipThreshold * 1000;

	if (isShort && deleteShortClips) {
		DeleteWithRetry(originPath, 15);
		DeleteFile(remuxPath);
		return;
	}

	/* normal recording, auto-remux active: delete original after remux */
	if (!remuxPath.isEmpty() && deleteOriginAfterRemux) {
		DeleteWithRetry(originPath, 30);
	}
}

void AutoCleanup::DeleteFile(const QString &path)
{
	if (path.isEmpty() || !QFile::exists(path))
		return;

	QFile file(path);
	bool ok = file.remove();
	blog(ok ? LOG_INFO : LOG_WARNING,
	     "[auto-cleanup] %s: %s",
	     ok ? "deleted" : "failed to delete", path.toUtf8().constData());
}

void AutoCleanup::DeleteWithRetry(const QString &path, int retriesLeft)
{
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
