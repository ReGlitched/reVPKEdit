#pragma once

#include <QAction>
#include <QMenu>
#include <QObject>
#include <QStyle>

struct EntryContextMenuData : public QObject {
public:
	explicit EntryContextMenuData(bool useRoot, QWidget* parent = nullptr);

	void setReadOnly(bool readOnly) const;

	QMenu* contextMenuFile = nullptr;
	QAction* extractFileAction = nullptr;
	QAction* extractFileConvertPngAction = nullptr;
	QAction* extractFileConvertTgaAction = nullptr;
	QAction* extractFileConvertDdsBc7Action = nullptr;
	QAction* editFileAction = nullptr;
	QAction* copyFilePathAction = nullptr;
	QAction* removeFileAction = nullptr;

	QMenu* contextMenuDir = nullptr;
	QAction* extractDirAction = nullptr;
	QAction* extractDirConvertPngAction = nullptr;
	QAction* extractDirConvertTgaAction = nullptr;
	QAction* extractDirConvertDdsBc7Action = nullptr;
	QAction* addFileToDirAction = nullptr;
	QAction* addDirToDirAction = nullptr;
	QAction* renameDirAction = nullptr;
	QAction* copyDirPathAction = nullptr;
	QAction* removeDirAction = nullptr;

	QMenu* contextMenuSelection = nullptr;
	QAction* extractSelectedAction = nullptr;
	QAction* extractSelectedConvertPngAction = nullptr;
	QAction* extractSelectedConvertTgaAction = nullptr;
	QAction* extractSelectedConvertDdsBc7Action = nullptr;
	QAction* removeSelectedAction = nullptr;

	QMenu* contextMenuAll = nullptr;
	QAction* extractAllAction = nullptr;
	QAction* addFileToRootAction = nullptr;
	QAction* addDirToRootAction = nullptr;
};
