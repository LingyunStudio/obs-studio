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
	void DeleteMkv(int retriesLeft);
	void DeleteFile(const QString &path);

	bool enabled = true;
	bool deleteShortClips = true;
	int shortClipThreshold = 10;
	QString recordingFolder;

	QTimer *mp4PollTimer = nullptr;

	QString mkvPath;
	QString mp4Path;
	QString originSuffix;
	qint64 recordStartTime = 0;

	static int mp4PollCount;
};
