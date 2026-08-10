#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

class AutoCleanup : public QObject {
	Q_OBJECT

public:
	static AutoCleanup *Instance();
	void LoadConfig();
	static void OnFrontendEvent(enum obs_frontend_event event, void *param);

private:
	explicit AutoCleanup(QObject *parent = nullptr);
	~AutoCleanup() override;

	void OnRecordingStarted();
	void OnRecordingPaused();
	void OnRecordingUnpaused();
	void OnRecordingStopped();
	void ProcessRecording();
	void StartPolling(bool deleteOriginOnComplete, bool deleteBothOnComplete);
	void CheckForRemux();
	void HandleRemuxReady();
	void HandleRemuxTimeout();
	void DeleteWithRetry(const QString &path, int retriesLeft);
	QString ComputeRemuxOutput(bool *willRemux);

	bool deleteShortClips = true;
	bool deleteOriginAfterRemux = false;
	int shortClipThreshold = 10;

	QTimer *pollTimer = nullptr;
	int pollCount = 0;
	qint64 lastSeenSize = -1;

	/* intent of the currently running poll */
	bool pendingOriginDelete = false;
	bool pendingShortDelete = false;

	QString originPath;
	QString remuxPath;
	qint64 recordStartMs = 0;
	qint64 pauseStartMs = 0;
	qint64 pausedDurationMs = 0;
	qint64 recordDurationMs = 0;
};
