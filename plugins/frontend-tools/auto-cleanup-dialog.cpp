#include "auto-cleanup-dialog.hpp"
#include "auto-cleanup.hpp"
#include "ui_auto-cleanup.h"

#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <QPushButton>

AutoCleanupDialog::AutoCleanupDialog(QWidget *parent) : QDialog(parent), ui(new Ui::AutoCleanupDialog)
{
	ui->setupUi(this);

	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

	config_t *config = obs_frontend_get_profile_config();

	ui->enableAutoCleanup->setChecked(config_get_bool(config, "AutoCleanup", "Enabled"));
	ui->deleteShortClips->setChecked(config_get_bool(config, "AutoCleanup", "DeleteShortClips"));
	int threshold = (int)config_get_int(config, "AutoCleanup", "ShortClipThreshold");
	if (threshold < 1)
		threshold = 10;
	ui->shortClipThreshold->setValue(threshold);
	QString folder = QString::fromUtf8(config_get_string(config, "AutoCleanup", "RecordingFolder"));
	if (folder.isEmpty())
		folder = QString::fromUtf8(config_get_string(config, "SimpleOutput", "FilePath"));
	ui->recordingFolder->setText(folder);

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
	config_set_int(config, "AutoCleanup", "ShortClipThreshold", ui->shortClipThreshold->value());
	config_set_string(config, "AutoCleanup", "RecordingFolder",
			  ui->recordingFolder->text().toUtf8().constData());

	config_save_safe(config, "tmp", nullptr);

	AutoCleanup::Instance()->LoadConfig();

	accept();
}
