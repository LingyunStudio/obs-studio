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
	void CheckForRemux();
	void HandleFiles();
	void DeleteWithRetry(const QString &path, int retriesLeft);
	void DeleteFile(const QString &path);

	bool deleteShortClips = true;
	bool deleteOriginAfterRemux = false;
	int shortClipThreshold = 10;

	QTimer *pollTimer = nullptr;
	static int pollCount;

	QString originPath;
	QString remuxPath;
	qint64 recordStartMs = 0;
};
