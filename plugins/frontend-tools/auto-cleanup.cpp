#include "auto-cleanup.hpp"
#include "auto-cleanup-dialog.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QMainWindow>

#include "qt-wrappers.hpp"

int AutoCleanup::mp4PollCount = 0;

AutoCleanup *AutoCleanup::Instance()
{
	static AutoCleanup instance;
	return &instance;
}

AutoCleanup::AutoCleanup(QObject *parent) : QObject(parent) {}

AutoCleanup::~AutoCleanup()
{
	if (mp4PollTimer) {
		mp4PollTimer->stop();
		delete mp4PollTimer;
		mp4PollTimer = nullptr;
	}
}

void AutoCleanup::LoadConfig()
{
	config_t *config = obs_frontend_get_profile_config();
	enabled = config_get_bool(config, "AutoCleanup", "Enabled");
	applyToAllFormats = config_get_bool(config, "AutoCleanup", "ApplyToAllFormats");
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
		self->recordStartTime = os_gettime_ns() / 1000000;

	} else if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPED) {
		self->OnRecordingStopped();
	}
}

void AutoCleanup::OnRecordingStopped()
{
	LoadConfig();
	if (!enabled)
		return;

	config_t *config = obs_frontend_get_profile_config();

	/* determine the recording folder */
	const char *recPath = config_get_string(config, "AdvOut", "RecFilePath");
	if (!recPath || !*recPath)
		recPath = config_get_string(config, "SimpleOutput", "FilePath");
	QString folder = QString::fromUtf8(recPath && *recPath ? recPath : "");
	if (folder.isEmpty())
		return;

	/* look for the most recent file in the recording folder */
	QDir dir(folder);
	QFileInfoList recentFiles = dir.entryInfoList(QDir::Files, QDir::Time);
	if (recentFiles.isEmpty())
		return;

	originPath = recentFiles.first().absoluteFilePath();

	/* detect auto-remux: [Video] AutoRemux=true, format is mkv -> target mp4 */
	bool autoRemux = config_get_bool(config, "Video", "AutoRemux");
	const char *recFormat = config_get_string(config, "AdvOut", "RecFormat2");
	if (!recFormat || !*recFormat)
		recFormat = config_get_string(config, "SimpleOutput", "RecFormat2");

	mp4Path.clear();
	bool waitForRemux = false;

	if (autoRemux && recFormat && strcmp(recFormat, "mkv") == 0) {
		mp4Path = originPath;
		mp4Path.replace(".mkv", ".mp4");
		if (!QFileInfo::exists(mp4Path)) {
			waitForRemux = true;
		}
	}

	if (waitForRemux) {
		/* start polling for the remuxed MP4 to appear */
		mp4PollCount = 0;
		mp4PollTimer = new QTimer(this);
		mp4PollTimer->setInterval(1000);
		QObject::connect(mp4PollTimer, &QTimer::timeout, this, &AutoCleanup::CheckForMp4);
		mp4PollTimer->start();
	} else {
		/* no remux expected; handle files immediately */
		HandleFiles();
	}
}

void AutoCleanup::CheckForMp4()
{
	mp4PollCount++;

	if (QFileInfo::exists(mp4Path)) {
		mp4PollTimer->stop();
		mp4PollTimer->deleteLater();
		mp4PollTimer = nullptr;
		HandleFiles();
	} else if (mp4PollCount > 600) {
		mp4PollTimer->stop();
		mp4PollTimer->deleteLater();
		mp4PollTimer = nullptr;
	}
}

void AutoCleanup::HandleFiles()
{
	qint64 duration = 0;
	if (recordStartTime > 0)
		duration = (os_gettime_ns() / 1000000) - recordStartTime;

	bool isShort = duration > 0 && duration < (qint64)shortClipThreshold * 1000;

	if (isShort && deleteShortClips) {
		/* short clip: delete everything */
		os_sleep_ms(500);
		DeleteFile(originPath);
		DeleteFile(mp4Path);
		return;
	}

	/* normal recording with auto-remux: user opted to delete the original
	 * file after remux, delete it (retry, may be locked until remux finishes) */
	if (!mp4Path.isEmpty() && deleteOriginAfterRemux) {
		DeleteOriginWithRetry(30);
		return;
	}

	/* normal recording, no auto-remux, applyToAllFormats enabled:
	 * delete the original file (no retry needed — OBS has already released it) */
	if (applyToAllFormats && !isShort) {
		os_sleep_ms(250);
		DeleteFile(originPath);
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
	     ok ? "deleted" : "failed to delete",
	     path.toUtf8().constData());
}

void AutoCleanup::DeleteOriginWithRetry(int retriesLeft)
{
	if (retriesLeft <= 0) {
		blog(LOG_WARNING, "[auto-cleanup] Delete origin file failed after retries: %s",
		     originPath.toUtf8().constData());
		return;
	}

	if (!QFile::exists(originPath))
		return;

	QFile file(originPath);
	if (file.remove()) {
		blog(LOG_INFO, "[auto-cleanup] Origin file deleted: %s", originPath.toUtf8().constData());
		return;
	}

	/* still locked; retry after 2 seconds */
	QTimer::singleShot(2000, this, [this, retriesLeft]() { DeleteOriginWithRetry(retriesLeft - 1); });
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
