#pragma once

#include <QDialog>

class QPlainTextEdit;

class RevpkLogDialog final : public QDialog {
	Q_OBJECT;

public:
	explicit RevpkLogDialog(QWidget* parent = nullptr);

	void setLogText(const QString& text);
	void appendLogText(const QString& text);

private:
	QPlainTextEdit* editor;
};

