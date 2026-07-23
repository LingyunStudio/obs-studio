#pragma once

#include <QDialog>
#include <memory>

namespace Ui {
class AutoCleanupDialog;
}

class AutoCleanupDialog : public QDialog {
	Q_OBJECT

public:
	std::unique_ptr<Ui::AutoCleanupDialog> ui;
	AutoCleanupDialog(QWidget *parent);
	~AutoCleanupDialog() override;

	friend void updateRemuxUI(AutoCleanupDialog *);

private slots:
	void SaveSettings();
};
