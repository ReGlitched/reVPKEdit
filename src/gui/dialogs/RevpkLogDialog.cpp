#include "RevpkLogDialog.h"

#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

RevpkLogDialog::RevpkLogDialog(QWidget* parent)
	: QDialog(parent)
	, editor(new QPlainTextEdit(this)) {
	this->setWindowTitle("revpk logs");
	this->setModal(false);
	this->resize(900, 600);

	this->editor->setReadOnly(true);
	this->editor->setWordWrapMode(QTextOption::NoWrap);

	auto* copyBtn = new QPushButton("Copy", this);
	auto* clearBtn = new QPushButton("Clear", this);
	auto* closeBtn = new QPushButton("Close", this);

	QObject::connect(copyBtn, &QPushButton::clicked, this, [this] {
		this->editor->selectAll();
		this->editor->copy();
		this->editor->moveCursor(QTextCursor::End);
	});
	QObject::connect(clearBtn, &QPushButton::clicked, this, [this] {
		this->editor->clear();
	});
	QObject::connect(closeBtn, &QPushButton::clicked, this, &QDialog::hide);

	auto* btnRow = new QHBoxLayout();
	btnRow->addWidget(copyBtn);
	btnRow->addWidget(clearBtn);
	btnRow->addStretch(1);
	btnRow->addWidget(closeBtn);

	auto* layout = new QVBoxLayout();
	layout->addWidget(this->editor, 1);
	layout->addLayout(btnRow);
	this->setLayout(layout);
}

void RevpkLogDialog::setLogText(const QString& text) {
	this->editor->setPlainText(text);
	this->editor->moveCursor(QTextCursor::End);
}

void RevpkLogDialog::appendLogText(const QString& text) {
	this->editor->moveCursor(QTextCursor::End);
	this->editor->insertPlainText(text);
	this->editor->moveCursor(QTextCursor::End);
}

