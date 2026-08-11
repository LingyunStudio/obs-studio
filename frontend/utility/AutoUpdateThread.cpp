#include "AutoUpdateThread.hpp"
#include "ui_OBSUpdate.h"

#include <OBSApp.hpp>
#include <dialogs/OBSUpdate.hpp>
#include <updater/manifest.hpp>
#include <utility/WhatsNewInfoThread.hpp>
#include <utility/update-helpers.hpp>

#include <qt-wrappers.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include "moc_AutoUpdateThread.cpp"

/* ------------------------------------------------------------------------ */

#ifndef WIN_MANIFEST_URL
#define WIN_MANIFEST_URL "https://api.github.com/repos/LingyunStudio/obs-studio/releases/latest"
#endif

#ifndef WIN_BRANCHES_URL
#define WIN_BRANCHES_URL "https://api.github.com/repos/LingyunStudio/obs-studio/releases"
#endif

#ifndef WIN_DEFAULT_BRANCH
#define WIN_DEFAULT_BRANCH "stable"
#endif

#ifndef WIN_UPDATER_URL
#define WIN_UPDATER_URL "https://github.com/LingyunStudio/obs-studio/releases/latest/download/updater.exe"
#endif

/* ------------------------------------------------------------------------ */

using namespace std;
using namespace updater;

extern char *GetConfigPathPtr(const char *name);

static bool ParseUpdateManifest(const char *manifest_data, bool *updatesAvailable, string &notes, string &updateVer,
				const string &branch)
try {
	// Parse GitHub Releases API JSON response (latest release)
	json manifestContents = json::parse(manifest_data);

	// GitHub release tag_name is like "v36.0.1.1-custom" or "36.0.1.1-custom"
	string tagName = manifestContents.value("tag_name", "");
	string body = manifestContents.value("body", "");

	// Remove leading 'v'/'V' if present
	if (!tagName.empty() && (tagName[0] == 'v' || tagName[0] == 'V'))
		tagName = tagName.substr(1);

	if (tagName.empty()) {
		*updatesAvailable = false;
		return true;
	}

	notes = body;
	updateVer = tagName;

	// Version comparison: parse numeric parts from both remote and local version
	auto parseVersion = [](const string &s, vector<int> &parts) {
		parts.clear();
		int v = -1;
		for (char c : s) {
			if (c >= '0' && c <= '9') {
				if (v < 0)
					v = 0;
				v = v * 10 + (c - '0');
			} else {
				if (v >= 0) {
					parts.push_back(v);
					v = -1;
				}
			}
		}
		if (v >= 0)
			parts.push_back(v);
	};

	UNUSED_PARAMETER(branch);

	vector<int> remoteParts, localParts;
	parseVersion(tagName, remoteParts);
	parseVersion(obs_get_version_string(), localParts);

	size_t n = max(remoteParts.size(), localParts.size());
	remoteParts.resize(n, 0);
	localParts.resize(n, 0);

	*updatesAvailable = false;
	for (size_t i = 0; i < n; i++) {
		if (remoteParts[i] > localParts[i]) {
			*updatesAvailable = true;
			break;
		} else if (remoteParts[i] < localParts[i]) {
			break;
		}
	}

	return true;

} catch (string &text) {
	blog(LOG_WARNING, "%s: %s", __FUNCTION__, text.c_str());
	return false;
}

/* ------------------------------------------------------------------------ */

bool GetBranchAndUrl(string &selectedBranch, string &manifestUrl)
{
	const char *config_branch = config_get_string(App()->GetAppConfig(), "General", "UpdateBranch");
	if (!config_branch) {
		return true;
	}

	// For GitHub-based updates, branch selection is limited
	// The "stable" branch corresponds to the latest GitHub release
	selectedBranch = WIN_DEFAULT_BRANCH;
	return true;
}

/* ------------------------------------------------------------------------ */

void AutoUpdateThread::infoMsg(const QString &title, const QString &text)
{
	OBSMessageBox::information(App()->GetMainWindow(), title, text);
}

void AutoUpdateThread::info(const QString &title, const QString &text)
{
	QMetaObject::invokeMethod(this, "infoMsg", Qt::BlockingQueuedConnection, Q_ARG(QString, title),
				  Q_ARG(QString, text));
}

int AutoUpdateThread::queryUpdateSlot(bool localManualUpdate, const QString &text)
{
	OBSUpdate updateDlg(App()->GetMainWindow(), localManualUpdate, text);
	return updateDlg.exec();
}

int AutoUpdateThread::queryUpdate(bool localManualUpdate, const char *text_utf8)
{
	int ret = OBSUpdate::No;
	QString text = text_utf8;
	QMetaObject::invokeMethod(this, "queryUpdateSlot", Qt::BlockingQueuedConnection, Q_RETURN_ARG(int, ret),
				  Q_ARG(bool, localManualUpdate), Q_ARG(QString, text));
	return ret;
}

bool AutoUpdateThread::queryRepairSlot()
{
	QMessageBox::StandardButton res =
		OBSMessageBox::question(App()->GetMainWindow(), QTStr("Updater.RepairConfirm.Title"),
					QTStr("Updater.RepairConfirm.Text"), QMessageBox::Yes | QMessageBox::Cancel);

	return res == QMessageBox::Yes;
}

bool AutoUpdateThread::queryRepair()
{
	bool ret = false;
	QMetaObject::invokeMethod(this, "queryRepairSlot", Qt::BlockingQueuedConnection, Q_RETURN_ARG(bool, ret));
	return ret;
}

void AutoUpdateThread::run()
try {
	string text;
	string branch = WIN_DEFAULT_BRANCH;
	string manifestUrl = WIN_MANIFEST_URL;
	vector<string> extraHeaders;
	bool updatesAvailable = false;

	struct FinishedTrigger {
		inline ~FinishedTrigger() { QMetaObject::invokeMethod(App()->GetMainWindow(), "updateCheckFinished"); }
	} finishedTrigger;

	/* ----------------------------------- *
	 * get branches from server            */

	if (FetchAndVerifyFile("branches", "obs-studio\\updates\\branches.json", WIN_BRANCHES_URL, &text)) {
		App()->SetBranchData(text);
	}

	/* ----------------------------------- *
	 * check branch and get manifest url   */

	if (!GetBranchAndUrl(branch, manifestUrl)) {
		config_set_string(App()->GetAppConfig(), "General", "UpdateBranch", WIN_DEFAULT_BRANCH);
		info(QTStr("Updater.BranchNotFound.Title"), QTStr("Updater.BranchNotFound.Text"));
	}

	/* allow server to know if this was a manual update check in case
	 * we want to allow people to bypass a configured rollout rate */
	if (manualUpdate) {
		extraHeaders.emplace_back("X-OBS2-ManualUpdate: 1");
	}

	/* ----------------------------------- *
	 * get manifest from server            */

	text.clear();
	// GitHub API requires a User-Agent header
	extraHeaders.push_back("User-Agent: obs-studio-custom");
	if (!FetchAndVerifyFile("manifest", "obs-studio\\updates\\manifest.json", manifestUrl.c_str(), &text,
				extraHeaders)) {
		return;
	}

	/* ----------------------------------- *
	 * check manifest for update           */

	string notes;
	string updateVer;

	if (!ParseUpdateManifest(text.c_str(), &updatesAvailable, notes, updateVer, branch)) {
		throw string("Failed to parse manifest");
	}

	if (!updatesAvailable && !repairMode) {
		if (manualUpdate) {
			info(QTStr("Updater.NoUpdatesAvailable.Title"), QTStr("Updater.NoUpdatesAvailable.Text"));
		}
		return;
	} else if (updatesAvailable && repairMode) {
		info(QTStr("Updater.RepairButUpdatesAvailable.Title"), QTStr("Updater.RepairButUpdatesAvailable.Text"));
		return;
	}

	/* ----------------------------------- *
	 * skip this version if set to skip    */

	const char *skipUpdateVer = config_get_string(App()->GetAppConfig(), "General", "SkipUpdateVersion");
	if (!manualUpdate && !repairMode && skipUpdateVer && updateVer == skipUpdateVer) {
		return;
	}

	/* ----------------------------------- *
	 * query user for update               */

	if (repairMode) {
		if (!queryRepair()) {
			return;
		}
	} else {
		int queryResult = queryUpdate(manualUpdate, notes.c_str());

		if (queryResult == OBSUpdate::No) {
			if (!manualUpdate) {
				long long t = (long long)time(nullptr);
				config_set_int(App()->GetAppConfig(), "General", "LastUpdateCheck", t);
			}
			return;

		} else if (queryResult == OBSUpdate::Skip) {
			config_set_string(App()->GetAppConfig(), "General", "SkipUpdateVersion", updateVer.c_str());
			return;
		}
	}

	/* ----------------------------------- *
	 * inform user to download manually    */

	info(QTStr("Updater.UpdateAvailable.Title"),
	     QTStr("Updater.UpdateAvailable.Text") + QString("\n\n") +
	     QString::fromStdString(notes) + QString("\n\n") +
	     QString("https://github.com/LingyunStudio/obs-studio/releases/latest"));

} catch (string &text) {
	blog(LOG_WARNING, "%s: %s", __FUNCTION__, text.c_str());
}
