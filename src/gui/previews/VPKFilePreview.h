#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

class QLabel;
class QProgressBar;
class QPushButton;
class QStandardItem;
class QStandardItemModel;
class QThread;
class QTreeView;

class Window;

class VPKFilePreview : public QWidget {
	Q_OBJECT;

public:
	explicit VPKFilePreview(Window* window, QWidget* parent = nullptr);
	~VPKFilePreview() override;

	void setVPKPath(const QString& absolutePath, const QString& relativePath);

private:
	Window* window;

	QLabel* title;
	QLabel* details;
	QLabel* entriesSummary;
	QProgressBar* entriesBusy;
	QTreeView* entriesTree;
	QStandardItemModel* entriesModel;
	QPushButton* openButton;

	QString absPath;

	QThread* entriesThread = nullptr;
	QObject* entriesWorker = nullptr;
	std::uint64_t entriesGeneration = 0;

	void stopEntriesWorker();

	// Directory path (e.g. "shaders/fxc") -> tree item for quick insertion.
	QHash<QString, QStandardItem*> dirItems;

	void addEntryPathToTree(const QString& entryPath);
	void sortTree();
};
