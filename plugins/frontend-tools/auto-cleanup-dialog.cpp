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

	ui->enableAutoCleanup->setChecked(config_get_bool(config, "AutoCleanup", "Enabled"));

	int threshold = (int)config_get_int(config, "AutoCleanup", "ShortClipThreshold");
	if (threshold < 1)
		threshold = 10;
	ui->shortClipThreshold->setValue(threshold);
	ui->deleteShortClips->setChecked(config_get_bool(config, "AutoCleanup", "DeleteShortClips"));

	/* show current recording path (read-only) */
	const char *recPath = config_get_string(config, "AdvOut", "RecFilePath");
	if (!recPath || !*recPath)
		recPath = config_get_string(config, "SimpleOutput", "FilePath");
	ui->recordingFolder->setText(
		QString("当前录制路径：%1").arg(QString::fromUtf8(recPath && *recPath ? recPath : "（未设置）")));

	/* detect whether auto-remux is active */
	bool autoRemux = config_get_bool(config, "Video", "AutoRemux");
	const char *recFormat = config_get_string(config, "AdvOut", "RecFormat2");
	if (!recFormat || !*recFormat)
		recFormat = config_get_string(config, "SimpleOutput", "RecFormat2");
	bool isMkv = recFormat && strcmp(recFormat, "mkv") == 0;

	ui->deleteOriginAfterRemux->setEnabled(autoRemux && isMkv);
	ui->deleteOriginAfterRemux->setChecked(config_get_bool(config, "AutoCleanup", "DeleteOriginAfterRemux"));

	if (autoRemux && isMkv) {
		ui->origDeleteLabel->setText(
			"MKV → MP4 自动封装：录制完成后删除 MKV 原始文件（保留 MP4）");
	} else {
		ui->origDeleteLabel->setText(
			QString("自动封装后清理：当前 OBS 录制格式为 %1，此选项不可用（需在 OBS 中设置 MKV 格式并勾选自动封装）")
				.arg(QString::fromUtf8(recFormat ? recFormat : "未知")));
	}

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
	config_set_bool(config, "AutoCleanup", "DeleteOriginAfterRemux",
			ui->deleteOriginAfterRemux->isChecked());
	config_set_int(config, "AutoCleanup", "ShortClipThreshold", ui->shortClipThreshold->value());

	config_save_safe(config, "tmp", nullptr);

	AutoCleanup::Instance()->LoadConfig();

	accept();
}
