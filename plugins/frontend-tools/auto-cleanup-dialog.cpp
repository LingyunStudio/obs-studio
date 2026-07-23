#include "auto-cleanup-dialog.hpp"
#include "auto-cleanup.hpp"
#include "ui_auto-cleanup.h"

#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <QLabel>
#include <QPushButton>

AutoCleanupDialog::AutoCleanupDialog(QWidget *parent) : QDialog(parent), ui(new Ui::AutoCleanupDialog)
{
	ui->setupUi(this);
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

	config_t *config = obs_frontend_get_profile_config();

	/* ---------- basic ---------- */
	ui->enableAutoCleanup->setChecked(config_get_bool(config, "AutoCleanup", "Enabled"));

	int threshold = (int)config_get_int(config, "AutoCleanup", "ShortClipThreshold");
	ui->shortClipThreshold->setValue(threshold < 1 ? 10 : threshold);
	ui->deleteShortClips->setChecked(config_get_bool(config, "AutoCleanup", "DeleteShortClips"));

	/* ---------- auto-remux section ---------- */
	bool autoRemux = config_get_bool(config, "Video", "AutoRemux");
	const char *modeStr = config_get_string(config, "Output", "Mode");
	bool simple = !modeStr || strcmp(modeStr, "Simple") == 0;
	const char *recFormat = config_get_string(config, simple ? "SimpleOutput" : "AdvOut", "RecFormat2");
	bool isMkv = recFormat && strcmp(recFormat, "mkv") == 0;

	ui->deleteOriginAfterRemux->setEnabled(autoRemux && isMkv);
	ui->deleteOriginAfterRemux->setChecked(config_get_bool(config, "AutoCleanup", "DeleteOriginAfterRemux"));

	if (autoRemux) {
		ui->remuxInfo->setText(QString("录制格式：%1  → 自动封装：MP4（已启用）")
					       .arg(isMkv ? "MKV" : QString::fromUtf8(recFormat)));
	} else {
		ui->remuxInfo->setText("自动封装：未启用");
	}

	/* ---------- recording path (read-only, same logic as OBS) ---------- */
	const char *recPath;
	if (simple) {
		recPath = config_get_string(config, "SimpleOutput", "FilePath");
	} else {
		const char *recType = config_get_string(config, "AdvOut", "RecType");
		recPath = config_get_string(config, "AdvOut",
					    (recType && strcmp(recType, "Standard") == 0) ? "RecFilePath"
										   : "FFFilePath");
	}
	ui->recordingPathLabel->setText(
		QString("当前录制路径：%1").arg(QString::fromUtf8(recPath && *recPath ? recPath : "未设置")));

	QObject::connect(ui->buttonBox->button(QDialogButtonBox::Close), &QPushButton::clicked, this,
			 &AutoCleanupDialog::hide);
	QObject::connect(ui->buttonBox, &QDialogButtonBox::accepted, this, &AutoCleanupDialog::SaveSettings);
	QObject::connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &AutoCleanupDialog::reject);
}

AutoCleanupDialog::~AutoCleanupDialog() = default;

void AutoCleanupDialog::SaveSettings()
{
	config_t *config = obs_frontend_get_profile_config();

	config_set_bool(config, "AutoCleanup", "Enabled", ui->enableAutoCleanup->isChecked());
	config_set_bool(config, "AutoCleanup", "DeleteShortClips", ui->deleteShortClips->isChecked());
	config_set_bool(config, "AutoCleanup", "DeleteOriginAfterRemux", ui->deleteOriginAfterRemux->isChecked());
	config_set_int(config, "AutoCleanup", "ShortClipThreshold", ui->shortClipThreshold->value());

	config_save_safe(config, "tmp", nullptr);

	AutoCleanup::Instance()->LoadConfig();
	accept();
}
