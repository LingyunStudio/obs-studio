/******************************************************************************
    Copyright (C) 2026 by OBS Project

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#pragma once

#include <obs.h>

#include <QPointer>
#include <QPoint>
#include <QTimer>
#include <QWidget>

class RegionBorder;
class RegionAdjustFrame;

/* Small always-on-top floating widget that allows starting/stopping a
 * recording, shows the elapsed recording time, allows switching the current
 * scene, and offers a one-click "draw region and record" flow which
 * automatically creates a temporary scene, records the selected region and
 * cleans everything up when the recording stops. */
class FloatingBall : public QWidget {
	Q_OBJECT

public:
	explicit FloatingBall(QWidget *parent = nullptr);
	~FloatingBall() override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;

private slots:
	void onTick();
	void toggleRecording();
	void startRegionRecord();
	void onRegionChanged(const QRect &region);
	void onRegionConfirmed(const QRect &region);
	void showContextMenu(const QPoint &globalPos);

private:
	QString elapsedText() const;
	void finishRegionRecord();
	bool createTempSceneAndSwitch(const char *monitorId, long rx, long ry, long rw, long rh,
				      long ax, long ay, bool startRecording);
	static void removeLeftoverTempScene();
	static void onFrontendEvent(enum obs_frontend_event event, void *param);

	QPoint dragStartGlobal;
	QPoint dragStartPos;
	bool dragged = false;

	QTimer tickTimer;
	bool recording = false;
	qint64 recordStartMs = 0;

	/* state of an active "draw region" recording session */
	bool regionSession = false;
	qint64 pendingStartMs = 0;
	QString savedScene;
	QPointer<RegionBorder> border;
	QPointer<RegionAdjustFrame> adjustFrame;
	obs_weak_source_t *tempRegionSource = nullptr;
};
