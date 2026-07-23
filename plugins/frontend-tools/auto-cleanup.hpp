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

	void OnRecordingStopped();
	void CheckForMp4();
	void HandleFiles();
	void DeleteOriginWithRetry(int retriesLeft);
	void DeleteFile(const QString &path);

	bool enabled = true;
	bool applyToAllFormats = false;
	bool deleteShortClips = true;
	bool deleteOriginAfterRemux = false;
	int shortClipThreshold = 10;

	QTimer *mp4PollTimer = nullptr;

	QString originPath;
	QString mp4Path;
	qint64 recordStartTime = 0;

	static int mp4PollCount;
};
