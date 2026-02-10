#pragma once

#include <functional>
#include <vector>

#include <QDir>
#include <QMainWindow>
#include <vpkpp/vpkpp.h>

#include "dialogs/PackFileOptionsDialog.h"
#include "plugins/previews/IVPKEditPreviewPlugin.h"

class QAction;
class QJsonObject;
class QLabel;
class QLineEdit;
class QMenu;
class QProgressBar;
class QSettings;
class QThread;
class RevpkLogDialog;

struct EntryContextMenuData;
class EntryTree;
class FileViewer;

class Window : public QMainWindow {
	Q_OBJECT;

	friend class SavePackFileWorker;
	friend class ExtractPackFileWorker;

public:
	explicit Window(QWidget* parent = nullptr);

	void newPackFile(std::string_view typeGUID, bool fromDirectory, const QString& startPath, const QString& name, const QString& extension);

	void newBMZ(bool fromDirectory, const QString& startPath = QString());

	void newFGP(bool fromDirectory, const QString& startPath = QString());

	void newFPX(bool fromDirectory, const QString& startPath = QString());

	void newPAK(bool fromDirectory, const QString& startPath = QString());

	void newPCK(bool fromDirectory, const QString& startPath = QString());

	void newVPK(bool fromDirectory, const QString& startPath = QString());

	void newVPK_VTMB(bool fromDirectory, const QString& startPath = QString());

	// Respawn VPK packer (folder -> _dir.vpk + _999.vpk + optional .cam)
	void newVPK_Respawn(const QString& startPath = QString());

	void newWAD3(bool fromDirectory, const QString& startPath = QString());

	void newZIP(bool fromDirectory, const QString& startPath = QString());

	void openDir(const QString& startPath = QString(), const QString& dirPath = QString());

	void openPackFile(const QString& startPath = QString(), const QString& filePath = QString());

	void savePackFile(bool saveAs = false, bool async = true);

	void saveAsPackFile(bool async = true);

	void closePackFile();

	[[nodiscard]] bool isReadOnly() const;

	void setProperties();

	void addFile(bool showOptions, const QString& startDir = QString(), const QString& filePath = QString());

	void addFiles(bool showOptions, const QString& startDir = QString());

	void addDir(bool showOptions, const QString& startDir = QString(), const QString& dirPath = QString());

	bool removeFile(const QString& path);

	void removeDir(const QString& path);

	void requestEntryRemoval(const QString& path) const;

	void editFile(const QString& oldPath);

	void editFileContents(const QString& path, std::vector<std::byte> data);

	void editFileContents(const QString& path, const QString& data);

	void renameFile(const QString& oldPath, const QString& newPath_ = QString());

	void renameDir(const QString& oldPath, const QString& newPath_ = QString());

	void generateKeyPairFiles(const QString& name = QString());

	void signPackFile(const QString& privateKeyLocation = QString());

	[[nodiscard]] std::optional<std::vector<std::byte>> readBinaryEntry(const QString& path) const;

	[[nodiscard]] std::optional<QString> readTextEntry(const QString& path) const;

	[[nodiscard]] QString getLastFileReadError() const;

	void selectEntryInEntryTree(const QString& path) const;

	void selectEntryInFileViewer(const QString& path) const;

	void selectDirInFileViewer(const QString& path, const QList<QString>& subfolders, const QList<QString>& entryPaths) const;

	[[nodiscard]] bool hasEntry(const QString& path) const;

	void selectSubItemInDir(const QString& path) const;

	void extractFile(const QString& path, QString savePath = QString());

	void extractFilesIf(const std::function<bool(const QString&)>& predicate, const QString& savePath = QString());

	void extractDir(const QString& path, const QString& saveDir = QString());

	void extractPaths(const QStringList& paths, const QString& saveDir = QString());

	void createDrag(const QStringList& paths);

	void extractAll(QString saveDir = QString());

	void setDropEnabled(bool dropEnabled_);

	void markModified(bool modified_);

	[[nodiscard]] bool promptUserToKeepModifications();

	[[nodiscard]] bool clearContents();

	void freezeActions(bool freeze, bool freezeCreationActions = true, bool freezeFileViewer = true) const;

	void freezeModifyActions(bool readOnly) const;

	void registerPlugin(const QString& path, QIcon icon, const QJsonObject& metadata);

	void pluginsInitContextMenu(const EntryContextMenuData* contextMenu) const;

	void pluginsUpdateContextMenu(int contextMenuType, const QStringList& paths) const;

	[[nodiscard]] bool hasPackFileLoaded() const { return static_cast<bool>(this->packFile); }

	// These are intentionally lightweight accessors for UI-only needs (previews, conditional UI, etc).
	[[nodiscard]] std::string_view getLoadedPackFileGUID() const;

	[[nodiscard]] QString getLoadedPackFilePath() const;

protected:
	void mousePressEvent(QMouseEvent* event) override;

	void dragEnterEvent(QDragEnterEvent* event) override;

	void dropEvent(QDropEvent* event) override;

	void closeEvent(QCloseEvent* event) override;

signals:
	void themeUpdated();

private:
	QLabel* statusText;
	QProgressBar* statusProgressBar;
	QLineEdit* searchBar;
	EntryTree* entryTree;
	FileViewer* fileViewer;

	QMenu*   createEmptyMenu;
	QMenu*   createFromDirMenu;
	QAction* openAction;
	QAction* openDirAction;
	QMenu*   openRelativeToMenu;
	QMenu*   openRecentMenu;
	QAction* saveAction;
	QAction* saveAsAction;
	QAction* closeFileAction;
	QAction* extractAllAction;
	QAction* extractConvertSelectedPngAction;
	QAction* extractConvertSelectedTgaAction;
	QAction* extractConvertSelectedDdsBc7Action;
	QAction* addFileAction;
	QAction* addDirAction;
	QAction* markModifiedAction;
	QAction* setPropertiesAction;
	QMenu*   toolsPluginInformationMenu;
	QMenu*   toolsGeneralMenu;
	QMenu*   toolsVPKMenu;
	QAction* createFromDirRespawnVpkAction;
	QAction* revpkLogsAction;

	QThread* createPackFileFromDirWorkerThread = nullptr;
	QThread* savePackFileWorkerThread          = nullptr;
	QThread* extractPackFileWorkerThread       = nullptr;
	QThread* scanSteamGamesWorkerThread        = nullptr;

	std::unique_ptr<vpkpp::PackFile> packFile;
	PackFileOptions packFileOptions;

	bool dropEnabled;

	bool loadDir(const QString& path);

	bool loadPackFile(const QString& path);

	bool loadPackFile(const QString& path, std::unique_ptr<vpkpp::PackFile>&& newPackFile);

	void rebuildOpenInMenu();

	void rebuildOpenRecentMenu(const QStringList& paths);

	bool writeEntryToFile(const QString& entryPath, const QString& filepath);

	void appendRevpkLog(const QString& titleLine, const QString& body);
	void appendRevpkLogRaw(const QString& text);
	void showRevpkLogs();
	void revpkBusyEnter();
	void revpkBusyLeave();

	void resetStatusBar();

	RevpkLogDialog* revpkLogDialog = nullptr;
	QString revpkLogText;
	int revpkBusyCount = 0;
};

class IndeterminateProgressWorker : public QObject {
	Q_OBJECT;

public:
	IndeterminateProgressWorker() = default;

	void run(const std::function<void()>& fn);

signals:
	void taskFinished();
};

class SavePackFileWorker : public QObject {
	Q_OBJECT;

public:
	SavePackFileWorker() = default;

	void run(Window* window, const QString& savePath, vpkpp::BakeOptions options, bool async = true);

signals:
	void progressUpdated(int value);
	void taskFinished(bool success);
};

class ExtractPackFileWorker : public QObject {
	Q_OBJECT;

public:
	ExtractPackFileWorker() = default;

	void run(Window* window, const QString& saveDir, const std::function<bool(const QString&)>& predicate);

signals:
	void progressUpdated(int value);
	void taskFinished(bool success, const QString& details);
};

class ScanSteamGamesWorker : public QObject {
	Q_OBJECT;

public:
	ScanSteamGamesWorker() = default;

	void run();

signals:
	void taskFinished(const QList<std::tuple<QString, QIcon, QDir>>& sourceGames);
};

class VPKEditWindowAccess_V3 final : public IVPKEditWindowAccess_V3 {
public:
	explicit VPKEditWindowAccess_V3(Window* window_);

	[[nodiscard]] QSettings* getOptions() const override;

	[[nodiscard]] bool isReadOnly() const override;

	void addFile(bool showOptions, const QString& startDir = QString(), const QString& filePath = QString()) const override;

	void addDir(bool showOptions, const QString& startDir = QString(), const QString& dirPath = QString()) const override;

	void removeFile(const QString& path) const override;

	void removeDir(const QString& path) const override;

	void editFileContents(const QString& path, const QByteArray& data) const override;

	void editFileContents(const QString& path, const QString& data) const override;

	void renameFile(const QString& oldPath, const QString& newPath = QString()) const override;

	void renameDir(const QString& oldPath, const QString& newPath = QString()) const override;

	[[nodiscard]] bool readBinaryEntry(const QString& entryPath, QByteArray& data) const override;

	[[nodiscard]] bool readTextEntry(const QString& entryPath, QString& data) const override;

	void selectEntryInEntryTree(const QString& entryPath) const override;

	[[nodiscard]] bool hasEntry(const QString& entryPath) const override;

	void selectSubItemInDir(const QString& path) const override;

	void extractFile(const QString& path, QString savePath = QString()) const override;

	void extractDir(const QString& path, const QString& saveDir = QString()) const override;

	void extractPaths(const QStringList& paths, const QString& saveDir = QString()) const override;

	void extractAll(QString saveDir = QString()) const override;

private:
	Window* window;
};
