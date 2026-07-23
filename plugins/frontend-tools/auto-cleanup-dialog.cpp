#include "auto-cleanup-dialog.hpp"
#include "auto-cleanup.hpp"
#include "ui_auto-cleanup.h"

#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <QLabel>
#include <QPushButton>

static void updateRemuxUI(AutoCleanupDialog *dlg)
{
	config_t *config = obs_frontend_get_profile_config();

	bool autoRemux = config_get_bool(config, "Video", "AutoRemux");
	const char *recFormat = config_get_string(config, "AdvOut", "RecFormat2");
	if (!recFormat || !*recFormat)
		recFormat = config_get_string(config, "SimpleOutput", "RecFormat2");
	bool isMkv = recFormat && strcmp(recFormat, "mkv") == 0;

	if (autoRemux && isMkv) {
		dlg->ui->deleteOriginAfterRemux->setEnabled(true);
		dlg->ui->deleteOriginAfterRemux->setText("检测到您已启用 MKV 录制并自动封装至 MP4，录制完成后自动删除 MKV 原始文件（保留 MP4）");
		dlg->ui->deleteOriginAfterRemux->setChecked(
			config_get_bool(config, "AutoCleanup", "DeleteOriginAfterRemux"));
	} else {
		dlg->ui->deleteOriginAfterRemux->setEnabled(false);
		dlg->ui->deleteOriginAfterRemux->setChecked(false);
		dlg->ui->deleteOriginAfterRemux->setText("（未启用自动封装，此选项不可用）");
	}
}

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

	updateRemuxUI(this);

	ui->applyToAllFormats->setChecked(config_get_bool(config, "AutoCleanup", "ApplyToAllFormats"));

	const char *recPath = config_get_string(config, "AdvOut", "RecFilePath");
	if (!recPath || !*recPath)
		recPath = config_get_string(config, "SimpleOutput", "FilePath");
	if (recPath && *recPath)
		ui->recordingFolder->setText(QString("当前录制路径：%1").arg(QString::fromUtf8(recPath)));
	else
		ui->recordingFolder->setText("当前录制路径：（未设置）");

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
	config_set_bool(config, "AutoCleanup", "ApplyToAllFormats", ui->applyToAllFormats->isChecked());
	config_set_int(config, "AutoCleanup", "ShortClipThreshold", ui->shortClipThreshold->value());

	config_save_safe(config, "tmp", nullptr);

	AutoCleanup::Instance()->LoadConfig();

	accept();
}
