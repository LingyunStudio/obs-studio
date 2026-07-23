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

#include "FloatingBall.hpp"

#include <OBSApp.hpp>
#include "OBSBasic.hpp"

#include <qt-wrappers.hpp>

#include <QDateTime>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>

#include <obs-frontend-api.h>
#include <util/config-file.h>

/* Click-through border shown around the captured region while a "draw
 * region" recording session is active. */
class RegionBorder : public QWidget {
public:
	explicit RegionBorder(QWidget *parent = nullptr) : QWidget(parent)
	{
		setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool |
			       Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
		setAttribute(Qt::WA_TranslucentBackground);
		setAttribute(Qt::WA_TransparentForMouseEvents);
		setAttribute(Qt::WA_ShowWithoutActivating);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setPen(QPen(QColor(255, 60, 60), 3));
		p.drawRect(rect().adjusted(2, 2, -3, -3));
	}
};

/* Interactive region frame shown after the initial drag: the user can move
 * it and resize it with the handles. The recording only starts when the
 * confirm button is clicked; Escape or the cancel button aborts. */
class RegionAdjustFrame : public QWidget {
	Q_OBJECT

public:
	explicit RegionAdjustFrame(const QRect &region, QWidget *parent = nullptr) : QWidget(parent), region(region)
	{
		setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
		setAttribute(Qt::WA_TranslucentBackground);
		setMouseTracking(true);
		setFocusPolicy(Qt::StrongFocus);

		toolbar = new QWidget(this);
		QHBoxLayout *layout = new QHBoxLayout(toolbar);
		layout->setContentsMargins(10, 6, 10, 6);
		layout->setSpacing(6);

		sizeLabel = new QLabel(toolbar);
		sizeLabel->setStyleSheet("color: white; font-weight: bold; background: transparent;");
		QPushButton *startButton = new QPushButton(QTStr("FloatingBall.StartRecording"), toolbar);
		QPushButton *cancelButton = new QPushButton(QTStr("Cancel"), toolbar);
		layout->addWidget(sizeLabel, 1);
		layout->addWidget(startButton);
		layout->addWidget(cancelButton);

		connect(startButton, &QPushButton::clicked, this, [this]() { emit confirmed(this->region); });
		connect(cancelButton, &QPushButton::clicked, this, [this]() { emit cancelled(); });

		updateWidgetGeometry();
	}

signals:
	void confirmed(const QRect &region);
	void cancelled();
	void regionChanged(const QRect &region);

protected:
	void paintEvent(QPaintEvent *) override
	{
		static const Hit hits[] = {Hit::TopLeft,    Hit::Top,	Hit::TopRight,	  Hit::Right,
					   Hit::BottomRight, Hit::Bottom, Hit::BottomLeft, Hit::Left};

		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);

		/* toolbar background */
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(30, 30, 30, 225));
		p.drawRoundedRect(toolbar->geometry().adjusted(0, 2, 0, -2), 6, 6);

		/* slight fill of the captured area: makes the whole interior
		 * clickable (fully transparent pixels of a layered window do not
		 * receive mouse input on Windows) and delineates the region */
		p.setBrush(QColor(0, 0, 0, 24));
		p.drawRect(borderRect());

		/* region border */
		p.setPen(QPen(QColor(255, 60, 60), 2));
		p.setBrush(Qt::NoBrush);
		p.drawRect(borderRect());

		/* resize handles */
		p.setPen(QPen(QColor(255, 60, 60), 1));
		p.setBrush(Qt::white);
		for (Hit h : hits)
			p.drawRect(handleRect(h));
	}

	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() != Qt::LeftButton)
			return;

		Hit h = hitTest(event->pos());
		if (h != Hit::None) {
			activeHit = h;
			dragGlobalStart = event->globalPosition().toPoint();
			regionAtDragStart = region;
			event->accept();
		}
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (activeHit == Hit::None) {
			setCursor(cursorForHit(hitTest(event->pos()), event->pos()));
			return;
		}

		const QPoint delta = event->globalPosition().toPoint() - dragGlobalStart;
		if (activeHit == Hit::Inside) {
			region = regionAtDragStart.translated(delta);
		} else {
			region = resizedRegion(regionAtDragStart, activeHit, delta);
		}
		updateWidgetGeometry();
	}

	void mouseReleaseEvent(QMouseEvent *) override
	{
		activeHit = Hit::None;
		unsetCursor();
		emit regionChanged(region);
	}

	void keyPressEvent(QKeyEvent *event) override
	{
		if (event->key() == Qt::Key_Escape)
			emit cancelled();
		else
			QWidget::keyPressEvent(event);
	}

private:
	enum class Hit { None, Inside, Left, Top, Right, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };

	static constexpr int MARGIN = 8;
	static constexpr int TOOLBAR_H = 40;
	static constexpr int HANDLE = 10;
	static constexpr int GRAB = 10;
	static constexpr int MIN_SIZE = 32;

	QRect borderRect() const
	{
		return QRect(MARGIN, toolbarAbove ? MARGIN + TOOLBAR_H : MARGIN, region.width(), region.height());
	}

	QRect handleRect(Hit hit) const
	{
		const QRect b = borderRect();
		QPoint c;
		switch (hit) {
		case Hit::TopLeft: c = b.topLeft(); break;
		case Hit::Top: c = QPoint((b.left() + b.right()) / 2, b.top()); break;
		case Hit::TopRight: c = b.topRight(); break;
		case Hit::Right: c = QPoint(b.right(), (b.top() + b.bottom()) / 2); break;
		case Hit::BottomRight: c = b.bottomRight(); break;
		case Hit::Bottom: c = QPoint((b.left() + b.right()) / 2, b.bottom()); break;
		case Hit::BottomLeft: c = b.bottomLeft(); break;
		case Hit::Left: c = QPoint(b.left(), (b.top() + b.bottom()) / 2); break;
		default: return QRect();
		}
		return QRect(c.x() - HANDLE / 2, c.y() - HANDLE / 2, HANDLE, HANDLE);
	}

	Hit hitTest(const QPoint &pos) const
	{
		static const Hit hits[] = {Hit::TopLeft,    Hit::Top,	Hit::TopRight,	  Hit::Right,
					   Hit::BottomRight, Hit::Bottom, Hit::BottomLeft, Hit::Left};
		const QRect b = borderRect();

		/* generous hit zones around the handles */
		for (Hit h : hits) {
			if (handleRect(h).adjusted(-GRAB, -GRAB, GRAB, GRAB).contains(pos))
				return h;
		}

		/* near the border line: resize in the matching direction(s) */
		const QRect outer = b.adjusted(-GRAB, -GRAB, GRAB, GRAB);
		const QRect inner = b.adjusted(GRAB, GRAB, -GRAB, -GRAB);
		if (outer.contains(pos) && !inner.contains(pos)) {
			const bool nearL = qAbs(pos.x() - b.left()) <= GRAB;
			const bool nearR = qAbs(pos.x() - b.right()) <= GRAB;
			const bool nearT = qAbs(pos.y() - b.top()) <= GRAB;
			const bool nearB = qAbs(pos.y() - b.bottom()) <= GRAB;
			if (nearT && nearL)
				return Hit::TopLeft;
			if (nearT && nearR)
				return Hit::TopRight;
			if (nearB && nearL)
				return Hit::BottomLeft;
			if (nearB && nearR)
				return Hit::BottomRight;
			if (nearL)
				return Hit::Left;
			if (nearR)
				return Hit::Right;
			if (nearT)
				return Hit::Top;
			if (nearB)
				return Hit::Bottom;
		}

		/* anywhere inside the region: move */
		if (b.contains(pos))
			return Hit::Inside;
		return Hit::None;
	}

	Qt::CursorShape cursorForHit(Hit hit, const QPoint &pos) const
	{
		const QRect b = borderRect();
		/* on the toolbar strip (above or below the region rect) the cursor
		 * should be a plain arrow, not a resize indicator */
		if (toolbarAbove ? pos.y() < b.top() : pos.y() > b.bottom()) {
			return Qt::ArrowCursor;
		}
		switch (hit) {
		case Hit::Inside: return Qt::SizeAllCursor;
		case Hit::Left:
		case Hit::Right: return Qt::SizeHorCursor;
		case Hit::Top:
		case Hit::Bottom: return Qt::SizeVerCursor;
		case Hit::TopLeft:
		case Hit::BottomRight: return Qt::SizeFDiagCursor;
		case Hit::TopRight:
		case Hit::BottomLeft: return Qt::SizeBDiagCursor;
		default: return Qt::ArrowCursor;
		}
	}

	QRect resizedRegion(const QRect &start, Hit hit, const QPoint &delta) const
	{
		int l = start.left();
		int t = start.top();
		int r = start.right() + 1;
		int b = start.bottom() + 1;

		if (hit == Hit::Left || hit == Hit::TopLeft || hit == Hit::BottomLeft)
			l = qMin(l + delta.x(), r - MIN_SIZE);
		if (hit == Hit::Right || hit == Hit::TopRight || hit == Hit::BottomRight)
			r = qMax(r + delta.x(), l + MIN_SIZE);
		if (hit == Hit::Top || hit == Hit::TopLeft || hit == Hit::TopRight)
			t = qMin(t + delta.y(), b - MIN_SIZE);
		if (hit == Hit::Bottom || hit == Hit::BottomLeft || hit == Hit::BottomRight)
			b = qMax(b + delta.y(), t + MIN_SIZE);

		return QRect(QPoint(l, t), QSize(r - l, b - t));
	}

	void updateWidgetGeometry()
	{
		QScreen *screen = QGuiApplication::screenAt(region.center());
		const QRect avail = screen ? screen->availableGeometry() : region.adjusted(-2000, -2000, 2000, 2000);

		toolbarAbove = (region.bottom() + MARGIN + TOOLBAR_H > avail.bottom()) &&
			       (region.top() - MARGIN - TOOLBAR_H >= avail.top());

		QRect widgetRect = region.adjusted(-MARGIN, -MARGIN, MARGIN, MARGIN);
		if (toolbarAbove)
			widgetRect.setTop(widgetRect.top() - TOOLBAR_H);
		else
			widgetRect.setBottom(widgetRect.bottom() + TOOLBAR_H);
		setGeometry(widgetRect);

		toolbar->setGeometry(0, toolbarAbove ? 0 : widgetRect.height() - TOOLBAR_H, widgetRect.width(),
				     TOOLBAR_H);
		sizeLabel->setText(QString::asprintf("%d x %d", region.width(), region.height()));
		update();
	}

	QRect region;
	QRect regionAtDragStart;
	QPoint dragGlobalStart;
	Hit activeHit = Hit::None;
	bool toolbarAbove = false;
	QWidget *toolbar = nullptr;
	QLabel *sizeLabel = nullptr;
};

FloatingBall::FloatingBall(QWidget *parent) : QWidget(parent)
{
	setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool | Qt::WindowDoesNotAcceptFocus);
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_ShowWithoutActivating);
	setFixedSize(88, 48);
	setToolTip(QTStr("FloatingBall.Tooltip"));

	config_t *config = App()->GetUserConfig();
	int x = (int)config_get_int(config, "BasicWindow", "FloatingBallX");
	int y = (int)config_get_int(config, "BasicWindow", "FloatingBallY");
	if (x != 0 || y != 0)
		move(x, y);
	else
		move(60, 60);

	removeLeftoverTempScene();

	connect(&tickTimer, &QTimer::timeout, this, &FloatingBall::onTick);
	tickTimer.start(1000);

	obs_frontend_add_event_callback(onFrontendEvent, this);
}

FloatingBall::~FloatingBall()
{
	obs_frontend_remove_event_callback(onFrontendEvent, this);

	if (border)
		border->deleteLater();
}

/* ------------------------------------------------------------------------- */

void FloatingBall::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);

	QRectF r = rect().adjusted(1, 1, -1, -1);
	p.setPen(QPen(QColor(255, 255, 255, 70), 1.5));
	p.setBrush(recording ? QColor(200, 45, 45, 225) : QColor(35, 35, 35, 200));
	p.drawRoundedRect(r, height() / 2, height() / 2);

	QFont f = p.font();
	f.setBold(true);
	p.setFont(f);
	p.setPen(Qt::white);
	p.drawText(rect(), Qt::AlignCenter, recording ? elapsedText() : QTStr("FloatingBall.Record"));
}

QString FloatingBall::elapsedText() const
{
	qint64 secs = (QDateTime::currentMSecsSinceEpoch() - recordStartMs) / 1000;
	if (secs < 0)
		secs = 0;

	qint64 h = secs / 3600;
	qint64 m = (secs % 3600) / 60;
	qint64 s = secs % 60;

	if (h > 0)
		return QString::asprintf("%lld:%02lld:%02lld", h, m, s);
	return QString::asprintf("%02lld:%02lld", m, s);
}

void FloatingBall::onTick()
{
	bool active = obs_frontend_recording_active();

	if (active && !recording) {
		recording = true;
		recordStartMs = QDateTime::currentMSecsSinceEpoch();
	} else if (!active && recording) {
		recording = false;
	}

	/* a region session whose recording failed to start (e.g. encoder
	 * error) must not leak its temporary scene/canvas */
	if (regionSession && pendingStartMs && !active &&
	    QDateTime::currentMSecsSinceEpoch() - pendingStartMs > 5000) {
		finishRegionRecord();
	}
	if (active)
		pendingStartMs = 0;

	update();
}

/* ------------------------------------------------------------------------- */

void FloatingBall::mousePressEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton) {
		dragStartGlobal = event->globalPosition().toPoint();
		dragStartPos = frameGeometry().topLeft();
		dragged = false;
	} else if (event->button() == Qt::RightButton) {
		showContextMenu(event->globalPosition().toPoint());
	}
}

void FloatingBall::mouseMoveEvent(QMouseEvent *event)
{
	if ((event->buttons() & Qt::LeftButton) == 0)
		return;

	QPoint delta = event->globalPosition().toPoint() - dragStartGlobal;
	if (!dragged && (abs(delta.x()) > 4 || abs(delta.y()) > 4))
		dragged = true;

	if (dragged)
		move(dragStartPos + delta);
}

void FloatingBall::mouseReleaseEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton)
		return;

	if (dragged) {
		config_t *config = App()->GetUserConfig();
		config_set_int(config, "BasicWindow", "FloatingBallX", frameGeometry().topLeft().x());
		config_set_int(config, "BasicWindow", "FloatingBallY", frameGeometry().topLeft().y());
		config_save_safe(config, "tmp", nullptr);
	} else {
		toggleRecording();
	}

	dragged = false;
}

void FloatingBall::toggleRecording()
{
	if (obs_frontend_recording_active())
		obs_frontend_recording_stop();
	else
		obs_frontend_recording_start();
}

void FloatingBall::showContextMenu(const QPoint &globalPos)
{
	QMenu menu(this);

	QAction *actRecord = menu.addAction(recording ? QTStr("FloatingBall.StopRecording")
						      : QTStr("FloatingBall.StartRecording"));
	connect(actRecord, &QAction::triggered, this, &FloatingBall::toggleRecording);

	QAction *actRegion = menu.addAction(QTStr("FloatingBall.DrawRegion"));
	actRegion->setEnabled(!recording && !regionSession);
	connect(actRegion, &QAction::triggered, this, &FloatingBall::startRegionRecord);

	QMenu *sceneMenu = menu.addMenu(QTStr("FloatingBall.Scenes"));
	obs_frontend_source_list scenes = {0};
	obs_frontend_get_scenes(&scenes);
	OBSSourceAutoRelease current = obs_frontend_get_current_scene();
	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *scene = scenes.sources.array[i];
		QAction *a = sceneMenu->addAction(QT_UTF8(obs_source_get_name(scene)));
		a->setCheckable(true);
		a->setChecked(scene == current);
		connect(a, &QAction::triggered, this, [scene]() { obs_frontend_set_current_scene(scene); });
	}
	obs_frontend_source_list_free(&scenes);

	menu.addSeparator();

	QAction *actHide = menu.addAction(QTStr("FloatingBall.Hide"));
	connect(actHide, &QAction::triggered, this, [this]() {
		config_t *config = App()->GetUserConfig();
		config_set_bool(config, "BasicWindow", "FloatingBallEnabled", false);
		config_save_safe(config, "tmp", nullptr);
		hide();
	});

	menu.exec(globalPos);
}

/* ------------------------------------------------------------------------- */
/* "Draw region" recording session                                            */

void FloatingBall::removeLeftoverTempScene()
{
	OBSSourceAutoRelease scene = obs_get_source_by_name(QT_TO_UTF8(QTStr("FloatingBall.TempScene")));
	if (scene)
		obs_source_remove(scene);
}

bool FloatingBall::createTempSceneAndSwitch(const char *monitorId, long rx, long ry, long rw, long rh,
					    long ax, long ay, bool startRecording)
{
	uint64_t outW = (uint64_t)(rw & ~3L);
	uint64_t outH = (uint64_t)(rh & ~1L);
	if (outW < 32 || outH < 32)
		return false;

	/* save the current scene so it can be restored afterwards */
	OBSSourceAutoRelease currentScene = obs_frontend_get_current_scene();
	savedScene = QT_UTF8(obs_source_get_name(currentScene));

	removeLeftoverTempScene();

	/* create the temporary scene with a region capture source */
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "monitor_id", monitorId);
	obs_data_set_int(settings, "region_x", rx);
	obs_data_set_int(settings, "region_y", ry);
	obs_data_set_int(settings, "region_w", rw);
	obs_data_set_int(settings, "region_h", rh);
	obs_data_set_bool(settings, "capture_cursor", true);

	OBSSourceAutoRelease source =
		obs_source_create("region_capture", QT_TO_UTF8(QTStr("FloatingBall.SourceName")), settings, nullptr);
	if (!source)
		return false;

	obs_scene_t *scene = obs_scene_create(QT_TO_UTF8(QTStr("FloatingBall.TempScene")));
	if (!scene)
		return false;

	obs_scene_add(scene, source);
	obs_source_t *sceneSource = obs_scene_get_source(scene);
	obs_frontend_set_current_scene(sceneSource);
	obs_scene_release(scene);

	if (tempRegionSource)
		obs_weak_source_release(tempRegionSource);
	tempRegionSource = obs_source_get_weak_source(source);

	if (startRecording) {
		/* show the region boundary while recording */
		border = new RegionBorder();
		border->setGeometry((int)ax - 4, (int)ay - 4, (int)rw + 8, (int)rh + 8);
		border->show();

		obs_frontend_recording_start();

		regionSession = true;
		pendingStartMs = QDateTime::currentMSecsSinceEpoch();
		recordStartMs = pendingStartMs;
	}

	return true;
}

void FloatingBall::startRegionRecord()
{
	if (regionSession || adjustFrame)
		return;

	if (obs_frontend_recording_active() || obs_frontend_streaming_active() ||
	    obs_frontend_virtualcam_active() || obs_frontend_replay_buffer_active()) {
		OBSMessageBox::warning(this, QTStr("FloatingBall.DrawRegion.Busy.Title"),
				       QTStr("FloatingBall.DrawRegion.Busy.Text"));
		return;
	}

	/* let the user drag-select an initial region (win-capture plugin) */
	calldata_t cd = {0};
	bool called = proc_handler_call(obs_get_proc_handler(), "win_capture_region_picker_select", &cd);
	bool ok = called && calldata_bool(&cd, "success");

	long ax = calldata_int(&cd, "abs_x");
	long ay = calldata_int(&cd, "abs_y");
	long rw = calldata_int(&cd, "width");
	long rh = calldata_int(&cd, "height");
	calldata_free(&cd);

	if (!ok || rw < 1 || rh < 1)
		return;

	/* create the temporary scene immediately so the canvas resizes right away */
	{
		calldata_t cd2 = {0};
		calldata_set_int(&cd2, "x", ax);
		calldata_set_int(&cd2, "y", ay);
		calldata_set_int(&cd2, "width", rw);
		calldata_set_int(&cd2, "height", rh);
		if (proc_handler_call(obs_get_proc_handler(), "win_capture_region_clamp_to_monitor", &cd2) &&
		    calldata_bool(&cd2, "success")) {
			createTempSceneAndSwitch(calldata_string(&cd2, "monitor_id"), calldata_int(&cd2, "x"),
						 calldata_int(&cd2, "y"), calldata_int(&cd2, "width"),
						 calldata_int(&cd2, "height"), ax, ay, false);
		}
		calldata_free(&cd2);
	}

	/* show an adjustable frame around the picked region; the recording only
	 * starts when the user confirms */
	adjustFrame = new RegionAdjustFrame(QRect((int)ax, (int)ay, (int)rw, (int)rh));
	connect(adjustFrame, &RegionAdjustFrame::regionChanged, this, [this](const QRect &absRegion) {
		calldata_t cd = {0};
		calldata_set_int(&cd, "x", (long long)absRegion.x());
		calldata_set_int(&cd, "y", (long long)absRegion.y());
		calldata_set_int(&cd, "width", (long long)absRegion.width());
		calldata_set_int(&cd, "height", (long long)absRegion.height());
		bool called = proc_handler_call(obs_get_proc_handler(), "win_capture_region_clamp_to_monitor", &cd);

		const char *monitorId = called && calldata_bool(&cd, "success")
						? calldata_string(&cd, "monitor_id")
						: nullptr;
		long rx = calldata_int(&cd, "x");
		long ry = calldata_int(&cd, "y");
		long rw = calldata_int(&cd, "width");
		long rh = calldata_int(&cd, "height");

		if (!calldata_bool(&cd, "success") || !monitorId || rw < 1 || rh < 1) {
			calldata_free(&cd);
			return;
		}

		/* update the temp scene's source parameters and canvas resolution live */
		finishRegionRecord();
		createTempSceneAndSwitch(monitorId, rx, ry, rw, rh, absRegion.x(), absRegion.y(), false);
		calldata_free(&cd);
	});
	connect(adjustFrame, &RegionAdjustFrame::confirmed, this, &FloatingBall::onRegionConfirmed);
	connect(adjustFrame, &RegionAdjustFrame::cancelled, this, [this]() {
		adjustFrame->deleteLater();
		adjustFrame = nullptr;
		finishRegionRecord();
	});
	adjustFrame->show();
	adjustFrame->activateWindow();
	adjustFrame->setFocus();
}

void FloatingBall::onRegionChanged(const QRect &region)
{
	/* handled in the lambda connected to RegionAdjustFrame::regionChanged */
	Q_UNUSED(region);
}

void FloatingBall::onRegionConfirmed(const QRect &absRegion)
{
	if (adjustFrame) {
		adjustFrame->deleteLater();
		adjustFrame = nullptr;
	}

	/* the temporary scene and source are already live; just start recording */

	/* show the region boundary while recording */
	if (!border) {
		border = new RegionBorder();
		border->setGeometry(absRegion.adjusted(-4, -4, 4, 4));
		border->show();
	}

	obs_frontend_recording_start();

	regionSession = true;
	pendingStartMs = QDateTime::currentMSecsSinceEpoch();
	recordStartMs = pendingStartMs;
}

void FloatingBall::finishRegionRecord()
{
	if (tempRegionSource) {
		obs_weak_source_release(tempRegionSource);
		tempRegionSource = nullptr;
	}

	if (!regionSession) {
		if (!savedScene.isEmpty()) {
			OBSSourceAutoRelease prev = obs_get_source_by_name(savedScene.toUtf8().constData());
			if (prev)
				obs_frontend_set_current_scene(prev);
		}
		OBSSourceAutoRelease tempScene = obs_get_source_by_name(QT_TO_UTF8(QTStr("FloatingBall.TempScene")));
		if (tempScene)
			obs_source_remove(tempScene);
		savedScene.clear();
	}
	regionSession = false;
	pendingStartMs = 0;

	if (border) {
		border->deleteLater();
		border = nullptr;
	}

	/* switch back to the previous scene (the region canvas tracker
	 * restores the normal canvas on the scene change), then remove the
	 * temporary scene */
	if (!savedScene.isEmpty()) {
		OBSSourceAutoRelease prev = obs_get_source_by_name(savedScene.toUtf8().constData());
		if (prev)
			obs_frontend_set_current_scene(prev);
	}

	OBSSourceAutoRelease tempScene = obs_get_source_by_name(QT_TO_UTF8(QTStr("FloatingBall.TempScene")));
	if (tempScene)
		obs_source_remove(tempScene);
}

void FloatingBall::onFrontendEvent(enum obs_frontend_event event, void *param)
{
	FloatingBall *self = static_cast<FloatingBall *>(param);

	/* frontend events are emitted on the UI thread; handle synchronously
	 * so the cleanup also runs when OBS stops the recording on shutdown */
	if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPED)
		self->finishRegionRecord();
}

#include "FloatingBall.moc"

