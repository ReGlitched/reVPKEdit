#include "Window.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ranges>

#include <bsppp/PakLump.h>
#include <kvpp/kvpp.h>
#include <QActionGroup>
#include <QApplication>
#include <QDesktopServices>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMimeData>
#include <QMouseEvent>
#include <QProcess>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPointer>
#include <QSplitter>
#include <QStatusBar>
#include <QDateTime>
#include <QStringDecoder>
#include <QStyleFactory>
#include <QThread>
#include <QTimer>
#include <FileStream.h>
#include <sourcepp/crypto/String.h>
#include <steampp/steampp.h>
#include <vpkpp/vpkpp.h>

#include <Config.h>
#include <RespawnVPKPack.h>
#include <RespawnVPK.h>

#include "dialogs/ControlsDialog.h"
#include "dialogs/CreditsDialog.h"
#include "dialogs/EntryOptionsDialog.h"
#include "dialogs/RevpkLogDialog.h"
#include "dialogs/VerifyChecksumsDialog.h"
#include "dialogs/VerifySignatureDialog.h"
#include "extensions/Folder.h"
#include "utility/DiscordPresence.h"
#include "utility/ImageLoader.h"
#include "utility/Options.h"
#include "utility/TempDir.h"
#include "EntryTree.h"
#include "FileViewer.h"

using namespace kvpp;
using namespace steampp;
using namespace vpkpp;

using BSP = bsppp::PakLump;

namespace {

struct RevpkPackTarget {
	QString locale;
	QString context;
	QString levelName;
	QString manifestStem;
};

[[nodiscard]] bool parseRespawnDirVpkTargetFromPath(const QString& dirVpkPath, RevpkPackTarget& out) {
	// Expected: <locale><context>_<level>.bsp.pak000_dir.vpk
	const auto fsPath = std::filesystem::path{dirVpkPath.toLocal8Bit().constData()};
	const auto filename = fsPath.filename().string();
	const auto suffix = std::string{".bsp.pak000_dir.vpk"};
	if (filename.size() <= suffix.size() || filename.rfind(suffix) != filename.size() - suffix.size()) {
		return false;
	}

	const auto stem = filename.substr(0, filename.size() - suffix.size());
	const auto underscore = stem.find('_');
	if (underscore == std::string::npos || underscore == 0 || underscore + 1 >= stem.size()) {
		return false;
	}

	const auto localeContext = stem.substr(0, underscore);
	const auto level = stem.substr(underscore + 1);

	auto splitLocaleContext = [&](std::string_view ctx) -> bool {
		if (localeContext.size() <= ctx.size()) {
			return false;
		}
		if (localeContext.rfind(ctx) != localeContext.size() - ctx.size()) {
			return false;
		}
		out.context = QString::fromLocal8Bit(std::string{ctx}.c_str());
		out.locale = QString::fromLocal8Bit(localeContext.substr(0, localeContext.size() - ctx.size()).c_str());
		return true;
	};

	if (!splitLocaleContext("client") && !splitLocaleContext("server")) {
		return false;
	}

	out.levelName = QString::fromLocal8Bit(level.c_str());
	out.manifestStem = QString::fromLocal8Bit((localeContext + "_" + level).c_str());
	return true;
}

[[nodiscard]] QString tryFindRevpkExe() {
	// Explicit option path
	if (auto configured = Options::get<QString>(OPT_REVPK_PATH); !configured.isEmpty()) {
		const auto p = QDir::cleanPath(configured);
		if (QFileInfo::exists(p) && QFileInfo{p}.isFile()) {
			return p;
		}
	}

	// Next to the application binary
	const auto appDir = QCoreApplication::applicationDirPath();
	const QString candidate = QDir{appDir}.filePath("revpk.exe");
	if (QFileInfo::exists(candidate) && QFileInfo{candidate}.isFile()) {
		return candidate;
	}

	// PATH lookup
	if (const auto pathEnv = qEnvironmentVariable("PATH"); !pathEnv.isEmpty()) {
		const auto parts = pathEnv.split(';', Qt::SkipEmptyParts);
		for (const auto& dir : parts) {
			const auto c = QDir{dir}.filePath("revpk.exe");
			if (QFileInfo::exists(c) && QFileInfo{c}.isFile()) {
				return c;
			}
		}
	}

	return {};
}

[[nodiscard]] QString tryFindRevpkWorkspaceRootForManifest(const QString& startDir, const QString& manifestStem) {
	auto dir = QDir{startDir};
	for (int i = 0; i < 10; i++) {
		const auto manifestPath = dir.filePath("manifest/" + manifestStem + ".txt");
		if (QFileInfo::exists(manifestPath) && QFileInfo{manifestPath}.isFile()) {
			return dir.absolutePath();
		}
		if (!dir.cdUp()) {
			break;
		}
	}
	return {};
}

[[nodiscard]] bool runRevpk(const QString& revpkExe, const QStringList& args, QString& outMergedLog) {
	outMergedLog.clear();

	QProcess proc;
	proc.setProgram(revpkExe);
	proc.setArguments(args);
	proc.setProcessChannelMode(QProcess::MergedChannels);
	proc.start();
	if (!proc.waitForStarted()) {
		outMergedLog = QString{"Failed to start revpk process: %1"}.arg(proc.errorString());
		return false;
	}
	proc.waitForFinished(-1);
	outMergedLog = QString::fromLocal8Bit(proc.readAll());

	return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
}

[[nodiscard]] QString quoteForDisplay(QString s) {
	if (s.isEmpty()) return "\"\"";
	const bool needsQuotes = s.contains(' ') || s.contains('\t') || s.contains('\n') || s.contains('"');
	if (needsQuotes) {
		s.replace('"', "\\\"");
		return "\"" + s + "\"";
	}
	return s;
}

[[nodiscard]] QString formatRevpkCommandLine(const QString& revpkExe, const QStringList& args) {
	QStringList parts;
	parts.reserve(args.size() + 1);
	parts.push_back(quoteForDisplay(revpkExe));
	for (const auto& a : args) {
		parts.push_back(quoteForDisplay(a));
	}
	return parts.join(' ');
}

struct RevpkRunResult {
	bool started = false;
	bool ok = false;
	int exitCode = -1;
	QProcess::ExitStatus exitStatus = QProcess::CrashExit;
	QString mergedLog;
	QString startError;
};

[[nodiscard]] RevpkRunResult runRevpkLive(
	const QString& revpkExe,
	const QStringList& args,
	const std::function<void(const QString&)>& onText
) {
	RevpkRunResult r{};

	QProcess proc;
	proc.setProgram(revpkExe);
	proc.setArguments(args);
	proc.setProcessChannelMode(QProcess::MergedChannels);

	QEventLoop loop;

	const auto flush = [&] {
		const auto bytes = proc.readAll();
		if (bytes.isEmpty()) return;
		const auto chunk = QString::fromLocal8Bit(bytes);
		r.mergedLog += chunk;
		if (onText) onText(chunk);
	};

	QObject::connect(&proc, &QProcess::readyRead, &loop, [&] { flush(); });
	QObject::connect(&proc, &QProcess::errorOccurred, &loop, [&](QProcess::ProcessError) { flush(); loop.quit(); });
	QObject::connect(&proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &loop, [&](int, QProcess::ExitStatus) {
		flush();
		loop.quit();
	});

	proc.start();
	if (!proc.waitForStarted()) {
		r.started = false;
		r.startError = QString{"Failed to start revpk process: %1"}.arg(proc.errorString());
		return r;
	}
	r.started = true;

	loop.exec();
	flush();

	r.exitCode = proc.exitCode();
	r.exitStatus = proc.exitStatus();
	r.ok = (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0);
	return r;
}

[[nodiscard]] bool looksLikeRespawnVpkByName(const QString& path) {
	// Respawn packedstore archives almost always include `pak000` in the filename:
	//   `englishclient_...bsp.pak000_dir.vpk`, `client_...bsp.pak000_013.vpk`, etc

	// Valve VPKs are typically `pak01_dir.vpk` / `pak01_000.vpk` and should not be
	// parsed by the Respawn reader (they share the same signature and header version but differ in entry metadata)
	const auto name = QFileInfo{path}.fileName().toLower();
	if (name.contains("pak000_dir") || name.contains("pak000_")) {
		return true;
	}
	// Also allow the common explicit prefixes, in case a path is odd but still a Respawn pack
	if (name.startsWith("englishclient") || name.startsWith("englishserver") ||
		name.startsWith("client") || name.startsWith("server")) {
		return true;
	}
	return false;
}

} // namespace

Window::Window(QWidget* parent)
		: QMainWindow(parent)
		, dropEnabled(true) {
	this->setWindowTitle(QString{PROJECT_TITLE.data()} + "[*]");
	this->setWindowIcon(QIcon(":/logo.png"));

	const auto showRestartWarning = [this] {
		static bool shownRestartMessage = false;
		if (!shownRestartMessage) {
			QMessageBox::warning(this, tr("Restart Required"), tr("The application must be restarted for these settings to take effect."));
			shownRestartMessage = true;
		}
	};

	// File menu
	auto* fileMenu = this->menuBar()->addMenu(tr("File"));

	this->createEmptyMenu = fileMenu->addMenu(this->style()->standardIcon(QStyle::SP_FileIcon), tr("Create..."));
	this->createEmptyMenu->addAction(this->style()->standardIcon(QStyle::SP_FileIcon), "BMZ", [this] {
		this->newBMZ(false);
	});
	this->createEmptyMenu->addAction(this->style()->standardIcon(QStyle::SP_FileIcon), "FGP", [this] {
		this->newFGP(false);
	});
	this->createEmptyMenu->addAction(this->style()->standardIcon(QStyle::SP_FileIcon), "FPX", [this] {
		this->newFPX(false);
	});
	this->createEmptyMenu->addAction(this->style()->standardIcon(QStyle::SP_FileIcon), "PAK", [this] {
		this->newPAK(false);
	});
	this->createEmptyMenu->addAction(this->style()->standardIcon(QStyle::SP_FileIcon), "PCK", [this] {
		this->newPCK(false);
	});
	this->createEmptyMenu->addAction(this->style()->standardIcon(QStyle::SP_FileIcon), "VPK", [this] {
		this->newVPK(false);
	});
	this->createEmptyMenu->addAction(this->style()->standardIcon(QStyle::SP_FileIcon), "VPK (V:TMB)", [this] {
		this->newVPK_VTMB(false);
	});
	this->createEmptyMenu->addAction(this->style()->standardIcon(QStyle::SP_FileIcon), "WAD3", [this] {
		this->newWAD3(false);
	});
	this->createEmptyMenu->addAction(this->style()->standardIcon(QStyle::SP_FileIcon), "ZIP", [this] {
		this->newZIP(false);
	});

	this->createFromDirMenu = fileMenu->addMenu(this->style()->standardIcon(QStyle::SP_FileIcon), tr("Create from Folder..."));
	this->createFromDirMenu->addAction(this->style()->standardIcon(QStyle::SP_FileIcon), "BMZ", [this] {
		this->newBMZ(true);
	});
	this->createFromDirMenu->addAction(this->style()->standardIcon(QStyle::SP_FileIcon), "FGP", [this] {
		this->newFGP(true);
	});
	this->createFromDirMenu->addAction(this->style()->standardIcon(QStyle::SP_FileIcon), "FPX", [this] {
		this->newFPX(true);
	});
	this->createFromDirMenu->addAction(this->style()->standardIcon(QStyle::SP_FileIcon), "PAK", [this] {
		this->newPAK(true);
	});
	this->createFromDirMenu->addAction(this->style()->standardIcon(QStyle::SP_FileIcon), "PCK", [this] {
		this->newPCK(true);
	});
	this->createFromDirMenu->addAction(this->style()->standardIcon(QStyle::SP_FileIcon), "VPK", [this] {
		this->newVPK(true);
	});
	this->createFromDirRespawnVpkAction = this->createFromDirMenu->addAction(this->style()->standardIcon(QStyle::SP_FileIcon), "VPK (Respawn)", [this] {
		this->newVPK_Respawn();
	});
	this->createFromDirMenu->addAction(this->style()->standardIcon(QStyle::SP_FileIcon), "VPK (V:TMB)", [this] {
		this->newVPK_VTMB(true);
	});
	this->createFromDirMenu->addAction(this->style()->standardIcon(QStyle::SP_FileIcon), "WAD3", [this] {
		this->newWAD3(true);
	});
	this->createFromDirMenu->addAction(this->style()->standardIcon(QStyle::SP_FileIcon), "ZIP", [this] {
		this->newZIP(true);
	});

	this->openAction = fileMenu->addAction(this->style()->standardIcon(QStyle::SP_DirIcon), tr("Open..."), Qt::CTRL | Qt::Key_O, [this] {
		this->openPackFile();
	});

	this->openDirAction = fileMenu->addAction(this->style()->standardIcon(QStyle::SP_DirIcon), tr("Open Folder..."), Qt::CTRL | Qt::SHIFT | Qt::Key_O, [this] {
		this->openDir();
	});

	this->openRelativeToMenu = fileMenu->addMenu(this->style()->standardIcon(QStyle::SP_DirLinkIcon), tr("Open In..."));
	this->rebuildOpenInMenu();

	this->openRecentMenu = fileMenu->addMenu(this->style()->standardIcon(QStyle::SP_DirLinkIcon), tr("Open Recent..."));
	this->rebuildOpenRecentMenu(Options::get<QStringList>(STR_OPEN_RECENT));

	this->saveAction = fileMenu->addAction(this->style()->standardIcon(QStyle::SP_DialogSaveButton), tr("Save"), Qt::CTRL | Qt::Key_S, [this] {
		this->savePackFile();
	});
	this->saveAction->setDisabled(true);

	this->saveAsAction = fileMenu->addAction(this->style()->standardIcon(QStyle::SP_DialogSaveButton), tr("Save As..."), Qt::CTRL | Qt::SHIFT | Qt::Key_S, [this] {
		this->saveAsPackFile();
	});
	this->saveAsAction->setDisabled(true);

	this->closeFileAction = fileMenu->addAction(this->style()->standardIcon(QStyle::SP_BrowserReload), tr("Close"), Qt::CTRL | Qt::Key_W, [this] {
		this->closePackFile();
	});
	this->closeFileAction->setDisabled(true);

	fileMenu->addSeparator();

	fileMenu->addAction(this->style()->standardIcon(QStyle::SP_DialogCancelButton), tr("Exit"), Qt::ALT | Qt::Key_F4, [this] {
		this->close();
	});

	// Edit menu
	auto* editMenu = this->menuBar()->addMenu(tr("Edit"));
	this->extractAllAction = editMenu->addAction(this->style()->standardIcon(QStyle::SP_DialogSaveButton), tr("Extract All"), Qt::CTRL | Qt::Key_E, [this] {
		this->extractAll();
	});
	this->extractAllAction->setDisabled(true);

	auto* extractConvertMenu = editMenu->addMenu(this->style()->standardIcon(QStyle::SP_DialogSaveButton), tr("Extract and Convert Selected"));
	this->extractConvertSelectedPngAction = extractConvertMenu->addAction(tr("PNG..."), [this] {
		const auto paths = this->entryTree->getSelectedEntryPaths();
		if (paths.isEmpty()) {
			QMessageBox::information(this, tr("Info"), tr("No entries selected."));
			return;
		}
		this->entryTree->extractEntriesAndConvertVtf(paths, VTFConvertFormat::PNG);
	});
	this->extractConvertSelectedTgaAction = extractConvertMenu->addAction(tr("TGA..."), [this] {
		const auto paths = this->entryTree->getSelectedEntryPaths();
		if (paths.isEmpty()) {
			QMessageBox::information(this, tr("Info"), tr("No entries selected."));
			return;
		}
		this->entryTree->extractEntriesAndConvertVtf(paths, VTFConvertFormat::TGA);
	});
	this->extractConvertSelectedDdsBc7Action = extractConvertMenu->addAction(tr("DDS (BC7)..."), [this] {
		const auto paths = this->entryTree->getSelectedEntryPaths();
		if (paths.isEmpty()) {
			QMessageBox::information(this, tr("Info"), tr("No entries selected."));
			return;
		}
		this->entryTree->extractEntriesAndConvertVtf(paths, VTFConvertFormat::DDS_BC7);
	});
	this->extractConvertSelectedPngAction->setDisabled(true);
	this->extractConvertSelectedTgaAction->setDisabled(true);
	this->extractConvertSelectedDdsBc7Action->setDisabled(true);

	editMenu->addSeparator();
	this->addFileAction = editMenu->addAction(this->style()->standardIcon(QStyle::SP_FileLinkIcon), tr("Add Files..."), Qt::CTRL | Qt::SHIFT | Qt::Key_A, [this] {
		this->addFiles(true);
	});
	this->addFileAction->setDisabled(true);

	this->addDirAction = editMenu->addAction(this->style()->standardIcon(QStyle::SP_DirLinkIcon), tr("Add Folder..."), Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_A, [this] {
		this->addDir(true);
	});
	this->addDirAction->setDisabled(true);

	this->markModifiedAction = editMenu->addAction(this->style()->standardIcon(QStyle::SP_BrowserReload), tr("Mark Modified"), Qt::CTRL | Qt::Key_M, [this] {
		this->markModified(true);
	});
	this->markModifiedAction->setDisabled(true);

	editMenu->addSeparator();
	this->setPropertiesAction = editMenu->addAction(this->style()->standardIcon(QStyle::SP_FileDialogContentsView), tr("Properties..."), Qt::CTRL | Qt::Key_P, [this] {
		this->setProperties();
	});
	this->setPropertiesAction->setDisabled(true);

	// Options menu
	auto* optionsMenu = this->menuBar()->addMenu(tr("Options"));

	auto* generalMenu = optionsMenu->addMenu(QIcon(":/logo.png"), tr("General..."));
	auto* optionAdvancedMode = generalMenu->addAction(tr("Advanced File Properties"), [] {
		Options::invert(OPT_ADVANCED_FILE_PROPS);
	});
	optionAdvancedMode->setCheckable(true);
	optionAdvancedMode->setChecked(Options::get<bool>(OPT_ADVANCED_FILE_PROPS));

	generalMenu->addSeparator();
	auto* openInEnableAction = generalMenu->addAction(tr("Disable Open In Menu"), [this] {
		Options::invert(OPT_DISABLE_STEAM_SCANNER);
		this->rebuildOpenInMenu();
	});
	openInEnableAction->setCheckable(true);
	openInEnableAction->setChecked(Options::get<bool>(OPT_DISABLE_STEAM_SCANNER));

	auto* languageMenu = optionsMenu->addMenu(this->style()->standardIcon(QStyle::SP_DialogHelpButton), tr("Language..."));
	auto* languageMenuGroup = new QActionGroup(languageMenu);
	languageMenuGroup->setExclusive(true);
	const QVector<QPair<QString, QString>> languageToLocaleMapping = {
		{tr("System Language"), ""},
		{"", ""}, // Separator
		{u8"Bosanski",           "bs_BA"},
		{u8"简体中文",            "zh_CN"},
		{u8"Hrvatski",           "hr"},
		{u8"Nederlands",         "nl"},
		{u8"English",            "en"},
		{u8"Deutsch",            "de"},
		{u8"Italiano",           "it"},
		{u8"日本語",              "ja"},
		{u8"한국인",              "ko"},
		{u8"Polski",             "pl"},
		{u8"Português (Brasil)", "pt_BR"},
		{u8"Русский",            "ru_RU"},
		{u8"Slovenščina",        "sl"},
		{u8"Español",            "es"},
		{u8"Svenska",            "sv"},
		{u8"Tiếng Việt",         "vi"},
	};
	for (const auto& [language, locale] : languageToLocaleMapping) {
		if (language.isEmpty() && locale.isEmpty()) {
			languageMenu->addSeparator();
			continue;
		}
		auto* action = languageMenu->addAction(language, [showRestartWarning, locale_=locale] {
			showRestartWarning();
			Options::set(OPT_LANGUAGE_OVERRIDE, locale_);
		});
		action->setCheckable(true);
		if (locale == Options::get<QString>(OPT_LANGUAGE_OVERRIDE)) {
			action->setChecked(true);
		}
		languageMenuGroup->addAction(action);
	}

	auto* themeMenu = optionsMenu->addMenu(this->style()->standardIcon(QStyle::SP_DesktopIcon), tr("Theme..."));
	auto* themeMenuGroup = new QActionGroup(themeMenu);
	themeMenuGroup->setExclusive(true);
	for (const auto& themeName : QStyleFactory::keys()) {
		auto* action = themeMenu->addAction(themeName, [this, themeName] {
			QApplication::setStyle(themeName);
			Options::set(OPT_STYLE, themeName);
			emit this->themeUpdated();
		});
		action->setCheckable(true);
		if (themeName == Options::get<QString>(OPT_STYLE)) {
			action->setChecked(true);
		}
		themeMenuGroup->addAction(action);
	}

	// Not translating this menu name, the translation is the same everywhere
	auto* discordMenu = optionsMenu->addMenu(QIcon{":/icons/discord.png"}, "Discord...");
	const auto setupDiscordRichPresence = [] {
		DiscordPresence::init("1469963754077814910");
		DiscordPresence::setState("Editing an archive file");
		DiscordPresence::setLargeImageText(PROJECT_TITLE.data());
		DiscordPresence::setLargeImage("icon");
		DiscordPresence::setStartTimestamp(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
		DiscordPresence::setTopButton({"View on GitHub", std::string{PROJECT_HOMEPAGE}});
	};
	auto* discordEnableAction = discordMenu->addAction(tr("Enable Rich Presence"), [setupDiscordRichPresence] {
		Options::invert(OPT_ENABLE_DISCORD_RICH_PRESENCE);
		if (Options::get<bool>(OPT_ENABLE_DISCORD_RICH_PRESENCE)) {
			setupDiscordRichPresence();
		} else {
			DiscordPresence::shutdown();
		}
	});
	discordEnableAction->setCheckable(true);
	discordEnableAction->setChecked(Options::get<bool>(OPT_ENABLE_DISCORD_RICH_PRESENCE));

	if (Options::get<bool>(OPT_ENABLE_DISCORD_RICH_PRESENCE)) {
		setupDiscordRichPresence();
	}
	auto* discordUpdateTimer = new QTimer(this);
	QObject::connect(discordUpdateTimer, &QTimer::timeout, this, &DiscordPresence::update);
	discordUpdateTimer->start(20);

	auto* entryListMenu = optionsMenu->addMenu(this->style()->standardIcon(QStyle::SP_FileDialogDetailedView), tr("Entry Tree..."));
	auto* entryListMenuAutoExpandAction = entryListMenu->addAction(tr("Expand Folder When Selected"), [this] {
		Options::invert(OPT_ENTRY_TREE_AUTO_EXPAND);
		this->entryTree->setAutoExpandDirectoryOnClick(Options::get<bool>(OPT_ENTRY_TREE_AUTO_EXPAND));
	});
	entryListMenuAutoExpandAction->setCheckable(true);
	entryListMenuAutoExpandAction->setChecked(Options::get<bool>(OPT_ENTRY_TREE_AUTO_EXPAND));

	auto* entryListMenuAllowDirDragAction = entryListMenu->addAction(tr("Allow Dragging To Extract Folders"), [this] {
		Options::invert(OPT_ENTRY_TREE_ALLOW_DIR_DRAG);
		this->entryTree->setAutoExpandDirectoryOnClick(Options::get<bool>(OPT_ENTRY_TREE_ALLOW_DIR_DRAG));
	});
	entryListMenuAllowDirDragAction->setCheckable(true);
	entryListMenuAllowDirDragAction->setChecked(Options::get<bool>(OPT_ENTRY_TREE_ALLOW_DIR_DRAG));

	auto* entryListMenuAllowFileDragAction = entryListMenu->addAction(tr("Allow Dragging To Extract Files"), [this] {
		Options::invert(OPT_ENTRY_TREE_ALLOW_FILE_DRAG);
		this->entryTree->setAutoExpandDirectoryOnClick(Options::get<bool>(OPT_ENTRY_TREE_ALLOW_FILE_DRAG));
	});
	entryListMenuAllowFileDragAction->setCheckable(true);
	entryListMenuAllowFileDragAction->setChecked(Options::get<bool>(OPT_ENTRY_TREE_ALLOW_FILE_DRAG));

	auto* entryListMenuAutoCollapseAction = entryListMenu->addAction(tr("Start Collapsed"), [this] {
		Options::invert(OPT_ENTRY_TREE_AUTO_COLLAPSE);
		this->entryTree->setAutoExpandDirectoryOnClick(Options::get<bool>(OPT_ENTRY_TREE_AUTO_COLLAPSE));
	});
	entryListMenuAutoCollapseAction->setCheckable(true);
	entryListMenuAutoCollapseAction->setChecked(Options::get<bool>(OPT_ENTRY_TREE_AUTO_COLLAPSE));

	auto* entryListMenuHideIconsAction = entryListMenu->addAction(tr("Hide Icons"), [this] {
		Options::invert(OPT_ENTRY_TREE_HIDE_ICONS);
		this->entryTree->setAutoExpandDirectoryOnClick(Options::get<bool>(OPT_ENTRY_TREE_HIDE_ICONS));
	});
	entryListMenuHideIconsAction->setCheckable(true);
	entryListMenuHideIconsAction->setChecked(Options::get<bool>(OPT_ENTRY_TREE_HIDE_ICONS));

	// Tools menu
	auto* toolsMenu = this->menuBar()->addMenu(tr("Tools"));

	this->toolsPluginInformationMenu = toolsMenu->addMenu(this->style()->standardIcon(QStyle::SP_FileDialogContentsView), tr("Plugin Information"));
	// populated later in Window::registerPlugin
	toolsMenu->addSeparator();

	this->toolsGeneralMenu = toolsMenu->addMenu(this->style()->standardIcon(QStyle::SP_FileIcon), tr("General"));
	this->toolsGeneralMenu->addAction(this->style()->standardIcon(QStyle::SP_FileDialogContentsView), tr("Verify Checksums"), [this] {
		VerifyChecksumsDialog::showDialog(*this->packFile, this);
	});
	this->toolsGeneralMenu->addAction(this->style()->standardIcon(QStyle::SP_FileDialogContentsView), tr("Verify Signature"), [this] {
		VerifySignatureDialog::showDialog(*this->packFile, this);
	});
	this->toolsGeneralMenu->setDisabled(true);

	this->toolsVPKMenu = toolsMenu->addMenu(this->style()->standardIcon(QStyle::SP_FileIcon), "VPK");
	this->toolsVPKMenu->addAction(this->style()->standardIcon(QStyle::SP_FileIcon), tr("Generate Public/Private Key Files..."), [this] {
		this->generateKeyPairFiles();
	});
	this->toolsVPKMenu->addAction(this->style()->standardIcon(QStyle::SP_FileIcon), tr("Sign File..."), [this] {
		this->signPackFile();
	});
	this->toolsVPKMenu->setDisabled(true);

	// revpk logs (external tool)
	this->revpkLogsAction = this->menuBar()->addAction(tr("revpk logs"));
	QObject::connect(this->revpkLogsAction, &QAction::triggered, this, [this] { this->showRevpkLogs(); });

	// Help menu
	auto* helpMenu = this->menuBar()->addMenu(tr("Help"));
	helpMenu->addAction(this->style()->standardIcon(QStyle::SP_DialogHelpButton), tr("About"), Qt::Key_F1, [this] {
		CreditsDialog::showDialog(this);
	});
	helpMenu->addAction(this->style()->standardIcon(QStyle::SP_DialogHelpButton), tr("About Qt"), Qt::ALT | Qt::Key_F1, [this] {
		QMessageBox::aboutQt(this);
	});
	helpMenu->addAction(this->style()->standardIcon(QStyle::SP_FileDialogListView), tr("Controls"), Qt::Key_F2, [this] {
		ControlsDialog::showDialog(this);
	});

#ifdef DEBUG
	// Debug menu
	auto* debugMenu = this->menuBar()->addMenu(tr("Debug"));

	auto* debugDialogsMenu = debugMenu->addMenu(this->style()->standardIcon(QStyle::SP_FileDialogDetailedView), tr("Dialogs"));
	debugDialogsMenu->addAction("New Entry Dialog (File) [VPK]", [this] {
		(void) EntryOptionsDialog::getEntryOptions(false, false, "test", VPK::GUID, {}, this);
	});
	debugDialogsMenu->addAction("New Entry Dialog (Dir) [VPK]", [this] {
		(void) EntryOptionsDialog::getEntryOptions(false, true, "test", VPK::GUID, {}, this);
	});
	debugDialogsMenu->addAction("Edit Entry Dialog (File) [VPK]", [this] {
		(void) EntryOptionsDialog::getEntryOptions(true, false, "test", VPK::GUID, {}, this);
	});
	debugDialogsMenu->addAction("Edit Entry Dialog (Dir) [VPK]", [this] {
		(void) EntryOptionsDialog::getEntryOptions(true, true, "test", VPK::GUID, {}, this);
	});
	debugDialogsMenu->addAction("New Entry Dialog (File) [ZIP/BSP]", [this] {
		(void) EntryOptionsDialog::getEntryOptions(false, false, "test", ZIP::GUID, {}, this);
	});
	debugDialogsMenu->addAction("New Entry Dialog (Dir) [ZIP/BSP]", [this] {
		(void) EntryOptionsDialog::getEntryOptions(false, true, "test", ZIP::GUID, {}, this);
	});
	debugDialogsMenu->addAction("Edit Entry Dialog (File) [ZIP/BSP]", [this] {
		(void) EntryOptionsDialog::getEntryOptions(true, false, "test", ZIP::GUID, {}, this);
	});
	debugDialogsMenu->addAction("Edit Entry Dialog (Dir) [ZIP/BSP]", [this] {
		(void) EntryOptionsDialog::getEntryOptions(true, true, "test", ZIP::GUID, {}, this);
	});
	debugDialogsMenu->addAction("Create Empty VPK Options Dialog", [this] {
		(void) PackFileOptionsDialog::getForNew(VPK::GUID, false, this);
	});
	debugDialogsMenu->addAction("Create VPK From Folder Options Dialog", [this] {
		(void) PackFileOptionsDialog::getForNew(VPK::GUID, true, this);
	});
	debugDialogsMenu->addAction("PackFile Options Dialog [VPK]", [this] {
		(void) PackFileOptionsDialog::getForEdit(VPK::GUID, {}, this);
	});
	debugDialogsMenu->addAction("PackFile Options Dialog [ZIP/BSP]", [this] {
		(void) PackFileOptionsDialog::getForEdit(ZIP::GUID, {}, this);
	});
#endif

	// Call after the menu is created, it controls the visibility of the save button
	this->markModified(false);

	// Split content into two resizeable panes
	auto* splitter = new QSplitter(Qt::Horizontal, this);
	this->setCentralWidget(splitter);

	// Left pane
	auto* leftPane = new QWidget(splitter);
	auto* leftPaneLayout = new QVBoxLayout(leftPane);
	leftPaneLayout->setContentsMargins(4, 4, 0, 0);

	this->searchBar = new QLineEdit(leftPane);
	this->searchBar->setPlaceholderText(tr("Search..."));
	QObject::connect(this->searchBar, &QLineEdit::textChanged, this, [this] {
		this->entryTree->setSearchQuery(this->searchBar->text());
		this->fileViewer->setSearchQuery(this->searchBar->text());
	});
	leftPaneLayout->addWidget(this->searchBar);

	this->entryTree = new EntryTree(this, leftPane);
	this->entryTree->setAutoExpandDirectoryOnClick(Options::get<bool>(OPT_ENTRY_TREE_AUTO_EXPAND));
	leftPaneLayout->addWidget(this->entryTree);

	splitter->addWidget(leftPane);

	// Right pane
	auto* rightPane = new QWidget(splitter);
	auto* rightPaneLayout = new QVBoxLayout(rightPane);
	rightPaneLayout->setContentsMargins(0, 4, 4, 0);

	this->fileViewer = new FileViewer(this, rightPane);
	rightPaneLayout->addWidget(this->fileViewer);

	splitter->addWidget(rightPane);

	splitter->setStretchFactor(0, 1);
	// todo: qt stretch 20 hack
	splitter->setStretchFactor(1, 20); // qt "stretch factor" can go fuck itself this is a magic number that works

	// Automatically collapse entry tree
	if (Options::get<bool>(OPT_ENTRY_TREE_AUTO_COLLAPSE)) {
		splitter->setSizes({0, splitter->size().width()});
	}

	this->statusText = new QLabel(this->statusBar());
	this->statusProgressBar = new QProgressBar(this->statusBar());

	this->statusBar()->addPermanentWidget(this->statusText, 1);
	this->statusBar()->addPermanentWidget(this->statusProgressBar, 1);

	(void) this->clearContents();

	// Update the theme for anything relying on that
	emit this->themeUpdated();

	// Load the pack file if given one through the command-line or double-clicking a file
	// An error here means shut the application down
	const auto& args = QApplication::arguments();
	if ((args.length() > 1 && QFile::exists(args[1])) && !this->loadPackFile(args[1])) {
		exit(1);
	}
}

void Window::newPackFile(std::string_view typeGUID, bool fromDirectory, const QString& startPath, const QString& name, const QString& extension) {
	if (typeGUID != FPX::GUID && typeGUID != FGP::GUID && typeGUID != PAK::GUID && typeGUID != PCK::GUID && typeGUID != VPK::GUID && typeGUID != VPK_VTMB::GUID && typeGUID != WAD3::GUID && typeGUID != ZIP::GUID) {
		return;
	}
	if (this->isWindowModified() && this->promptUserToKeepModifications()) {
		return;
	}

	auto options = PackFileOptionsDialog::getForNew(typeGUID, fromDirectory, this);
	if (!options) {
		return;
	}

	auto dirPath = fromDirectory ? QFileDialog::getExistingDirectory(this, QObject::tr("Use This Folder"), startPath) : "";
	if (fromDirectory && dirPath.isEmpty()) {
		return;
	}

	QString saveFilePath;
	if (fromDirectory) {
		saveFilePath = std::filesystem::path{dirPath.toLocal8Bit().constData()}.parent_path().string().c_str();
		saveFilePath += QDir::separator();
		if (typeGUID == FPX::GUID || typeGUID == VPK::GUID) {
			saveFilePath += std::filesystem::path{dirPath.toLocal8Bit().constData()}.stem().string().c_str() + (((options->vpk_saveSingleFile || dirPath.endsWith("_dir")) ? "" : "_dir") + extension);
		} else {
			saveFilePath += std::filesystem::path{dirPath.toLocal8Bit().constData()}.stem().string().c_str() + extension;
		}
	}
	auto packFilePath = QFileDialog::getSaveFileName(this, QObject::tr("Save New Pack File"), fromDirectory ? saveFilePath : startPath, name + " (*" + extension + ")");
	if (packFilePath.isEmpty()) {
		return;
	}

	std::unique_ptr<PackFile> out;
	if (typeGUID == FPX::GUID) {
		out = FPX::create(packFilePath.toLocal8Bit().constData());
		if (auto* fpx = dynamic_cast<FPX*>(out.get())) {
			fpx->setChunkSize(options->vpk_chunkSize);
		}
	} else if (typeGUID == FGP::GUID) {
		out = FGP::create(packFilePath.toLocal8Bit().constData());
	} else if (typeGUID == PAK::GUID) {
		out = PAK::create(packFilePath.toLocal8Bit().constData());
	} else if (typeGUID == PCK::GUID) {
		out = PCK::create(packFilePath.toLocal8Bit().constData());
	} else if (typeGUID == VPK::GUID) {
		out = VPK::create(packFilePath.toLocal8Bit().constData(), options->vpk_version);
		if (auto* vpk = dynamic_cast<VPK*>(out.get())) {
			vpk->setChunkSize(options->vpk_chunkSize);
		}
	} else if (typeGUID == VPK_VTMB::GUID) {
		const auto basePath = std::filesystem::path{packFilePath.toLocal8Bit().constData()};
		std::string packFilePathStr = basePath.parent_path().string() + "/pack000" + std::string{VPK_VTMB_EXTENSION};
		packFilePath = packFilePathStr.c_str();
		out = VPK_VTMB::create(packFilePathStr);
	} else if (typeGUID == WAD3::GUID) {
		out = WAD3::create(packFilePath.toLocal8Bit().constData());
	} else if (typeGUID == ZIP::GUID) {
		out = ZIP::create(packFilePath.toLocal8Bit().constData());
	} else {
		return;
	}

	if (!fromDirectory) {
		this->loadPackFile(packFilePath, std::move(out));
		return;
	}

	// Set up progress bar
	this->statusText->hide();
	this->statusProgressBar->show();
	this->statusBar()->show();

	// Show progress bar is busy
	this->statusProgressBar->setValue(0);
	this->statusProgressBar->setRange(0, 0);

	this->freezeActions(true);

	// Set up thread
	this->createPackFileFromDirWorkerThread = new QThread(this);
	auto* worker = new IndeterminateProgressWorker();
	worker->moveToThread(this->createPackFileFromDirWorkerThread);
	QObject::connect(this->createPackFileFromDirWorkerThread, &QThread::started, worker, [worker, packFilePath, dirPath, options_=*options] {
		worker->run([packFilePath, dirPath, options_] {
			if (auto packFile = PackFile::open(packFilePath.toLocal8Bit().constData())) {
				if (packFile->isInstanceOf<FPX>()) {
					if (auto* fpx = dynamic_cast<FPX*>(packFile.get())) {
						fpx->setChunkSize(options_.vpk_chunkSize);
					}
				} else if (packFile->isInstanceOf<VPK>()) {
					if (auto* vpk = dynamic_cast<VPK*>(packFile.get())) {
						vpk->setChunkSize(options_.vpk_chunkSize);
					}
				}
				packFile->addDirectory("", dirPath.toLocal8Bit().constData(), {
					.zip_compressionType = EntryCompressionType::NO_COMPRESS,
					.zip_compressionStrength = 0,
					.vpk_preloadBytes = 0,
					.vpk_saveToDirectory = options_.vpk_saveSingleFile,
				});
				packFile->bake("", {}, nullptr);
			}
		});
	});
	QObject::connect(worker, &IndeterminateProgressWorker::taskFinished, this, [this, packFilePath, options_=*options] {
		// Kill thread
		this->createPackFileFromDirWorkerThread->quit();
		this->createPackFileFromDirWorkerThread->wait();
		delete this->createPackFileFromDirWorkerThread;
		this->createPackFileFromDirWorkerThread = nullptr;

		// loadPackFile freezes them right away again
		// this->freezeActions(false);
		this->loadPackFile(packFilePath);
		if (this->packFile) {
			if (this->packFile->isInstanceOf<FPX>()) {
				if (auto* fpx = dynamic_cast<FPX*>(this->packFile.get())) {
					fpx->setChunkSize(options_.vpk_chunkSize);
				}
			} else if (this->packFile->isInstanceOf<VPK>()) {
				if (auto* vpk = dynamic_cast<VPK*>(this->packFile.get())) {
					vpk->setChunkSize(options_.vpk_chunkSize);
				}
			}
		}
	});
	this->createPackFileFromDirWorkerThread->start();
}

void Window::appendRevpkLog(const QString& titleLine, const QString& body) {
	const auto ts = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
	QString addition = QString{"[%1] %2\n"}.arg(ts, titleLine);
	if (!body.isEmpty()) {
		addition += body;
		if (!addition.endsWith('\n')) {
			addition += '\n';
		}
	}
	addition += '\n';

	this->revpkLogText += addition;
	if (this->revpkLogDialog) {
		this->revpkLogDialog->appendLogText(addition);
	}
}

void Window::appendRevpkLogRaw(const QString& text) {
	if (text.isEmpty()) {
		return;
	}
	this->revpkLogText += text;
	if (this->revpkLogDialog) {
		this->revpkLogDialog->appendLogText(text);
	}
}

void Window::showRevpkLogs() {
	if (!this->revpkLogDialog) {
		this->revpkLogDialog = new RevpkLogDialog(this);
		this->revpkLogDialog->setAttribute(Qt::WA_DeleteOnClose, false);
		this->revpkLogDialog->setLogText(this->revpkLogText);
	}
	this->revpkLogDialog->show();
	this->revpkLogDialog->raise();
	this->revpkLogDialog->activateWindow();
}

void Window::revpkBusyEnter() {
	this->revpkBusyCount++;
	if (this->revpkBusyCount == 1) {
		// Dont freeze the navigation UI; only block conflicting operations
		if (this->extractAllAction) this->extractAllAction->setDisabled(true);
		if (this->extractConvertSelectedPngAction) this->extractConvertSelectedPngAction->setDisabled(true);
		if (this->extractConvertSelectedTgaAction) this->extractConvertSelectedTgaAction->setDisabled(true);
		if (this->extractConvertSelectedDdsBc7Action) this->extractConvertSelectedDdsBc7Action->setDisabled(true);
		if (this->createFromDirRespawnVpkAction) this->createFromDirRespawnVpkAction->setDisabled(true);
	}
}

void Window::revpkBusyLeave() {
	this->revpkBusyCount = std::max(0, this->revpkBusyCount - 1);
	if (this->revpkBusyCount == 0) {
		// Restore defaults; loadPackFile/extractFilesIf may adjust these again
		if (this->extractAllAction) this->extractAllAction->setDisabled(!this->packFile);
		if (this->extractConvertSelectedPngAction) this->extractConvertSelectedPngAction->setDisabled(!this->packFile);
		if (this->extractConvertSelectedTgaAction) this->extractConvertSelectedTgaAction->setDisabled(!this->packFile);
		if (this->extractConvertSelectedDdsBc7Action) this->extractConvertSelectedDdsBc7Action->setDisabled(!this->packFile);
		if (this->createFromDirRespawnVpkAction) this->createFromDirRespawnVpkAction->setDisabled(false);
	}
}

void Window::newBMZ(bool fromDirectory, const QString& startPath) {
	return this->newPackFile(ZIP::GUID, fromDirectory, startPath, "BMZ", ".bmz");
}

void Window::newFGP(bool fromDirectory, const QString& startPath) {
	return this->newPackFile(FGP::GUID, fromDirectory, startPath, "FGP", ".grp");
}

void Window::newFPX(bool fromDirectory, const QString& startPath) {
	return this->newPackFile(FPX::GUID, fromDirectory, startPath, "FPX", ".fpx");
}

void Window::newPAK(bool fromDirectory, const QString& startPath) {
	return this->newPackFile(PAK::GUID, fromDirectory, startPath, "PAK", ".pak");
}

void Window::newPCK(bool fromDirectory, const QString& startPath) {
	return this->newPackFile(PCK::GUID, fromDirectory, startPath, "PCK", ".pck");
}

void Window::newVPK(bool fromDirectory, const QString& startPath) {
	return this->newPackFile(VPK::GUID, fromDirectory, startPath, "VPK", ".vpk");
}

void Window::newVPK_VTMB(bool fromDirectory, const QString& startPath) {
	return this->newPackFile(VPK_VTMB::GUID, fromDirectory, startPath, "VPK (V:TMB)", ".vpk");
}

void Window::newVPK_Respawn(const QString& startPath) {
	// Choose source folder
	auto dirPath = startPath;
	if (dirPath.isEmpty()) {
		dirPath = QFileDialog::getExistingDirectory(this, tr("Open Folder"), QDir{"~/"}.canonicalPath(), QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
	} else {
		dirPath = QFileDialog::getExistingDirectory(this, tr("Open Folder"), dirPath, QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
	}
	if (dirPath.isEmpty()) {
		return;
	}

	// Choose output _dir.vpk path
	auto baseName = std::filesystem::path{dirPath.toLocal8Bit().constData()}.filename().string();
	// If the input folder already ends with `_dir`, don't generate `*_dir_dir.vpk`
	if (!baseName.ends_with("_dir")) {
		baseName += "_dir";
	}
	const auto defaultOut = (std::filesystem::path{dirPath.toLocal8Bit().constData()}.parent_path() / (baseName + ".vpk")).string();

	auto packFilePath = QFileDialog::getSaveFileName(this, tr("Save VPK (Respawn)"), defaultOut.c_str(), "VPK (*.vpk);;All Files (*)");
	if (packFilePath.isEmpty()) {
		return;
	}
	packFilePath.replace('\\', '/');
	if (!packFilePath.endsWith(".vpk")) {
		packFilePath += ".vpk";
	}
	// Normalize the output name to end with `_dir.vpk`
	{
		auto fsPath = std::filesystem::path{packFilePath.toLocal8Bit().constData()};
		auto stem = fsPath.stem().string();
		// Collapse accidental repeats like `*_dir_dir`
		while (stem.ends_with("_dir_dir")) {
			stem.erase(stem.size() - 4); // remove trailing `_dir`
		}
		if (!stem.ends_with("_dir")) {
			stem += "_dir";
		}
		packFilePath = (fsPath.parent_path() / (stem + ".vpk")).string().c_str();
		packFilePath.replace('\\', '/');
	}

	// Common repack mistake: selecting the parent folder that contains a single extracted root folder named like the VPK
	// If the selected input folder contains exactly one child directory matching the output stem, pack that instead
	{
		const auto outStem = std::filesystem::path{packFilePath.toLocal8Bit().constData()}.stem().string();
		const auto candidate = (std::filesystem::path{dirPath.toLocal8Bit().constData()} / outStem).string();
		std::error_code ec;
		if (std::filesystem::is_directory(candidate, ec)) {
			std::size_t fileCount = 0, dirCount = 0;
			for (const auto& it : std::filesystem::directory_iterator{dirPath.toLocal8Bit().constData(), ec}) {
				if (ec) break;
				if (it.is_directory(ec)) dirCount++;
				else if (it.is_regular_file(ec)) fileCount++;
				ec.clear();
			}
			ec.clear();
			if (fileCount == 0 && dirCount == 1) {
				dirPath = candidate.c_str();
				dirPath.replace('\\', '/');
			}
		}
	}

	// Set up progress bar
	this->statusText->hide();
	this->statusProgressBar->show();
	this->statusBar()->show();

	this->statusProgressBar->setValue(0);
	this->statusProgressBar->setRange(0, 0);

	// If we know we will use external revpk, do not freeze navigation UI. Only block conflicting operations
	bool willUseRevpk = false;
	if (Options::get<bool>(OPT_REVPK_USE_FOR_RESPAWN_PACK_UNPACK)) {
		const auto revpkExe = tryFindRevpkExe();
		RevpkPackTarget target{};
		if (!revpkExe.isEmpty() && parseRespawnDirVpkTargetFromPath(packFilePath, target)) {
			const auto workspaceRoot = tryFindRevpkWorkspaceRootForManifest(dirPath, target.manifestStem);
			if (!workspaceRoot.isEmpty()) {
				willUseRevpk = true;
			}
		}
	}

	const bool didFreeze = !willUseRevpk;
	if (didFreeze) {
		this->freezeActions(true);
	} else {
		this->revpkBusyEnter();
	}

	// Set up thread
	this->createPackFileFromDirWorkerThread = new QThread(this);
	auto* worker = new IndeterminateProgressWorker();
	worker->moveToThread(this->createPackFileFromDirWorkerThread);

	struct Result {
		bool ok = false;
		bool usedRevpk = false;
		std::string err;
	};
	auto result = std::make_shared<Result>();

	QPointer<Window> wnd(this);
	QObject::connect(this->createPackFileFromDirWorkerThread, &QThread::started, worker, [worker, packFilePath, dirPath, result, wnd] {
		worker->run([packFilePath, dirPath, result, wnd] {
			try {
				const bool useRevpk = Options::get<bool>(OPT_REVPK_USE_FOR_RESPAWN_PACK_UNPACK);
				const auto revpkExe = useRevpk ? tryFindRevpkExe() : QString{};

				RevpkPackTarget target{};
				if (!revpkExe.isEmpty() && parseRespawnDirVpkTargetFromPath(packFilePath, target)) {
					// Resolve workspace root by locating manifest/<stem>.txt somewhere above the selected folder
					auto workspaceRoot = tryFindRevpkWorkspaceRootForManifest(dirPath, target.manifestStem);
					if (!workspaceRoot.isEmpty()) {
						result->usedRevpk = true;
						auto outDir = QFileInfo{packFilePath}.absoluteDir().absolutePath();
						workspaceRoot.replace('\\', '/');
						outDir.replace('\\', '/');
						const auto threads = Options::get<int>(OPT_REVPK_NUM_THREADS);
						const auto comp = Options::get<QString>(OPT_REVPK_COMPRESSION_LEVEL);

						const QStringList args = {
							"-pack",
							target.locale,
							target.context,
							target.levelName,
							workspaceRoot,
							outDir,
							QString::number(threads),
							comp,
						};

						const auto cmdLine = formatRevpkCommandLine(revpkExe, args);
						if (wnd) {
							QMetaObject::invokeMethod(wnd, [wnd, cmdLine] {
								if (!wnd) return;
								wnd->showRevpkLogs();
								wnd->appendRevpkLog("revpk -pack (running)", cmdLine);
							}, Qt::QueuedConnection);
						}

						const auto emitChunk = [wnd](const QString& chunk) {
							if (!wnd || chunk.isEmpty()) return;
							QMetaObject::invokeMethod(wnd, [wnd, chunk] {
								if (wnd) wnd->appendRevpkLogRaw(chunk);
							}, Qt::QueuedConnection);
						};

						const auto rr = runRevpkLive(revpkExe, args, emitChunk);
						if (!rr.started) {
							result->ok = false;
							result->err = rr.startError.toStdString();
							if (wnd) {
								QMetaObject::invokeMethod(wnd, [wnd, rr] {
									if (wnd) wnd->appendRevpkLog("revpk -pack (failed)", rr.startError);
								}, Qt::QueuedConnection);
							}
							return;
						}

						result->ok = rr.ok;
						result->err = rr.mergedLog.toStdString();
						if (wnd) {
							QMetaObject::invokeMethod(wnd, [wnd, rr] {
								if (!wnd) return;
								const auto status = QString{"ExitCode=%1 ExitStatus=%2"}
									.arg(rr.exitCode)
									.arg(rr.exitStatus == QProcess::NormalExit ? "NormalExit" : "CrashExit");
								wnd->appendRevpkLog(rr.ok ? "revpk -pack (ok)" : "revpk -pack (failed)", status);
							}, Qt::QueuedConnection);
						}
						return;
					}
				}

				// Fallback to internal packer when revpk isnt available or no manifest is present
				result->usedRevpk = false;
				respawn_vpk::PackOptions opts{};
				opts.threadCount = 0;
				// If output name looks like `...pak000_dir.vpk`, use 000 so the archive is `..._000.vpk`
				opts.archiveIndex = respawn_vpk::inferArchiveIndexFromDirVpkPath(packFilePath.toLocal8Bit().constData(), 999);
				std::string err;
				result->ok = respawn_vpk::packDirectoryToRespawnVPK(dirPath.toLocal8Bit().constData(), packFilePath.toLocal8Bit().constData(), opts, &err);
				result->err = std::move(err);
			} catch (const std::exception& e) {
				result->ok = false;
				result->err = std::string{"Exception during packing: "} + e.what();
			} catch (...) {
				result->ok = false;
				result->err = "Unknown exception during packing.";
			}
		});
	});

			QObject::connect(worker, &IndeterminateProgressWorker::taskFinished, this, [this, worker, packFilePath, result, didFreeze, willUseRevpk] {
				// Kill thread
				this->createPackFileFromDirWorkerThread->quit();
				this->createPackFileFromDirWorkerThread->wait();
		delete this->createPackFileFromDirWorkerThread;
		this->createPackFileFromDirWorkerThread = nullptr;

		worker->deleteLater();

		if (!result->ok) {
			if (didFreeze) {
				this->freezeActions(false);
			}
			if (willUseRevpk) {
				this->revpkBusyLeave();
			}
			this->resetStatusBar();
			QMessageBox::critical(this, tr("Error"), tr("Failed to pack Respawn VPK:\n%1").arg(result->err.c_str()));
			return;
		}

		if (willUseRevpk) {
			this->revpkBusyLeave();
		}

		// loadPackFile freezes actions again while it loads
		this->loadPackFile(packFilePath);
	});

	this->createPackFileFromDirWorkerThread->start();
}

void Window::newWAD3(bool fromDirectory, const QString& startPath) {
	return this->newPackFile(WAD3::GUID, fromDirectory, startPath, "WAD3", ".wad");
}

void Window::newZIP(bool fromDirectory, const QString& startPath) {
	return this->newPackFile(ZIP::GUID, fromDirectory, startPath, "ZIP", ".zip");
}

void Window::openDir(const QString& startPath, const QString& dirPath) {
	auto path = dirPath;
	if (path.isEmpty()) {
		path = QFileDialog::getExistingDirectory(this, tr("Open Folder"), startPath);
	}
	if (path.isEmpty()) {
		return;
	}
	this->loadDir(path);
}

void Window::openPackFile(const QString& startPath, const QString& filePath) {
	auto path = filePath;
	if (path.isEmpty()) {
		auto supportedExtensions = PackFile::getOpenableExtensions();
		QString filter = "Supported Files (";
		for (int i = 0; i < supportedExtensions.size(); i++) {
			if (i != 0) {
				filter += ' ';
			}
			filter += ('*' + supportedExtensions[i]).c_str();
		}
		filter += ")";
		path = QFileDialog::getOpenFileName(this, tr("Open Pack File"), startPath, filter);
	}
	if (path.isEmpty()) {
		return;
	}
	this->loadPackFile(path);
}

void Window::savePackFile(bool saveAs, bool async) {
	QString savePath = "";
	if (saveAs) {
		savePath = QFileDialog::getExistingDirectory(this, tr("Save to..."));
		if (savePath.isEmpty()) {
			return;
		}
	}

	// Set up progress bar
	this->statusText->hide();
	this->statusProgressBar->show();

	// Get progress bar maximum
	int progressBarMax = static_cast<int>(this->packFile->getEntryCount());

	// Show progress indicator
	this->statusProgressBar->setRange(0, progressBarMax);
	this->statusProgressBar->setValue(0);

	this->freezeActions(true);

	// Set up thread
	auto* worker = new SavePackFileWorker();
	if (async) {
		this->savePackFileWorkerThread = new QThread(this);
		worker->moveToThread(this->savePackFileWorkerThread);
		QObject::connect(this->savePackFileWorkerThread, &QThread::started, worker, [this, worker, savePath] {
			worker->run(this, savePath, {
				.zip_compressionTypeOverride = this->packFileOptions.compressionType,
				.zip_compressionStrength = this->packFileOptions.compressionStrength,
			});
		});
	}
	QObject::connect(worker, &SavePackFileWorker::progressUpdated, this, [this, progressBarMax](int value) {
		static bool alreadyShownBusy = false;
		if (progressBarMax == value) {
			// Show busy indicator if we haven't already
			if (alreadyShownBusy) {
				return;
			}
			alreadyShownBusy = true;
			this->statusProgressBar->setValue(0);
			this->statusProgressBar->setRange(0, 0);
		} else {
			alreadyShownBusy = false;
			this->statusProgressBar->setRange(0, progressBarMax);
			this->statusProgressBar->setValue(value);
		}
	});
	QObject::connect(worker, &SavePackFileWorker::taskFinished, this, [this, async](bool success) {
		// Kill thread
		if (async) {
			this->savePackFileWorkerThread->quit();
			this->savePackFileWorkerThread->wait();
			delete this->savePackFileWorkerThread;
			this->savePackFileWorkerThread = nullptr;
		}

		this->freezeActions(false);

		this->resetStatusBar();

		if (!success) {
			QMessageBox::warning(this, tr("Could not save!"),
			                     tr("An error occurred while saving changes to the file. Check that you have permission to write to it, and that no other application is using it."));
		} else {
			this->markModified(false);
		}
	});

	if (async) {
		this->savePackFileWorkerThread->start();
	} else {
		worker->run(this, savePath, {
			.zip_compressionTypeOverride = this->packFileOptions.compressionType,
			.zip_compressionStrength = this->packFileOptions.compressionStrength,
		}, false);
	}
}

void Window::saveAsPackFile(bool async) {
	this->savePackFile(true, async);
}

void Window::closePackFile() {
	if (this->clearContents()) {
		this->packFile = nullptr;
	}
}

bool Window::isReadOnly() const {
	return !this->packFile || this->packFile->isReadOnly();
}

void Window::setProperties() {
	auto version = 0u;
	auto chunkSize = 0u;

	if (this->packFile->isInstanceOf<FPX>()) {
		if (auto fpx = dynamic_cast<FPX*>(this->packFile.get())) {
			chunkSize = fpx->getChunkSize();
		}
	} else if (this->packFile->isInstanceOf<VPK>()) {
		if (auto vpk = dynamic_cast<VPK*>(this->packFile.get())) {
			version = vpk->getVersion();
			chunkSize = vpk->getChunkSize();
		}
	}

	auto options = PackFileOptionsDialog::getForEdit(this->packFile->getGUID(), {
		.compressionType = this->packFileOptions.compressionType,
		.compressionStrength = this->packFileOptions.compressionStrength,
		.vpk_version = version,
		.vpk_chunkSize = chunkSize,
	}, this);
	if (!options) {
		return;
	}

	this->packFileOptions.compressionType = options->compressionType;
	this->packFileOptions.compressionStrength = options->compressionStrength;

	if (this->packFile->isInstanceOf<FPX>()) {
		if (auto fpx = dynamic_cast<FPX*>(this->packFile.get())) {
			fpx->setChunkSize(options->vpk_chunkSize);
		}
	} else if (this->packFile->isInstanceOf<VPK>()) {
		if (auto vpk = dynamic_cast<VPK*>(this->packFile.get())) {
			vpk->setVersion(options->vpk_version);
			vpk->setChunkSize(options->vpk_chunkSize);
		}
	}

	this->resetStatusBar();

	this->markModified(true);
}

void Window::addFile(bool showOptions, const QString& startDir, const QString& filePath) {
	auto filepath = filePath;
	if (filepath.isEmpty()) {
		filepath = QFileDialog::getOpenFileName(this, tr("Open File"));
	}
	if (filepath.isEmpty()) {
		return;
	}

	auto prefilledPath = startDir;
	if (!prefilledPath.isEmpty()) {
		prefilledPath += '/';
	}
	prefilledPath += std::filesystem::path{filepath.toLocal8Bit().constData()}.filename().string().c_str();

	QString entryPath = prefilledPath;
	EntryOptions options;

	if (showOptions || Options::get<bool>(OPT_ADVANCED_FILE_PROPS)) {
		auto newEntryOptions = EntryOptionsDialog::getEntryOptions(false, false, prefilledPath, this->packFile->getGUID(), {}, this);
		if (!newEntryOptions) {
			return;
		}
		entryPath = std::get<0>(*newEntryOptions);
		options = std::get<1>(*newEntryOptions);
	}

	if (!this->packFile->isCaseSensitive()) {
		entryPath = entryPath.toLower();
	}

	this->packFile->removeEntry(entryPath.toLocal8Bit().constData());
	this->packFile->addEntry(entryPath.toLocal8Bit().constData(), filepath.toLocal8Bit().constData(), options);
	this->entryTree->addEntry(entryPath);
	if (const auto entry = this->packFile->findEntry(entryPath.toLocal8Bit().constData())) {
		this->fileViewer->addEntry(*entry, entryPath);
	}
	this->markModified(true);
}

void Window::addFiles(bool showOptions, const QString &startDir) {
	// Add multiple files using the multiple file selector
	for (const QString& path : QFileDialog::getOpenFileNames(this, tr("Open Files"))) {
		this->addFile(showOptions, startDir, path);
	}
}

void Window::addDir(bool showOptions, const QString& startDir, const QString& dirPath) {
	auto dirpath = dirPath;
	if (dirpath.isEmpty()) {
		dirpath = QFileDialog::getExistingDirectory(this, tr("Open Folder"));
	}
	if (dirpath.isEmpty()) {
		return;
	}

	auto prefilledPath = startDir;
	if (!prefilledPath.isEmpty()) {
		prefilledPath += '/';
	}
	prefilledPath += std::filesystem::path{dirpath.toLocal8Bit().constData()}.filename().string().c_str();

	QString parentEntryPath = prefilledPath;
	EntryOptions options;

	if (showOptions || Options::get<bool>(OPT_ADVANCED_FILE_PROPS)) {
		auto newEntryOptions = EntryOptionsDialog::getEntryOptions(false, true, prefilledPath, this->packFile->getGUID(), {}, this);
		if (!newEntryOptions) {
			return;
		}
		parentEntryPath = std::get<0>(*newEntryOptions);
		options = std::get<1>(*newEntryOptions);
	}

	QDirIterator it(dirpath, QDir::Files | QDir::Readable, QDirIterator::FollowSymlinks | QDirIterator::Subdirectories);
	while (it.hasNext()) {
		QString subEntryPathFS = it.next();
		QString subEntryPath = parentEntryPath + subEntryPathFS.sliced(dirpath.length());

		if (!this->packFile->isCaseSensitive()) {
			subEntryPath = subEntryPath.toLower();
		}

		this->packFile->removeEntry(subEntryPath.toLocal8Bit().constData());
		this->packFile->addEntry(subEntryPath.toLocal8Bit().constData(), subEntryPathFS.toLocal8Bit().constData(), options);
		this->entryTree->addEntry(subEntryPath);
		if (const auto entry = this->packFile->findEntry(subEntryPath.toLocal8Bit().constData())) {
			this->fileViewer->addEntry(*entry, subEntryPath);
		}
	}
	this->markModified(true);
}

bool Window::removeFile(const QString& path) {
	if (!this->packFile->removeEntry(path.toLocal8Bit().constData())) {
		QMessageBox::critical(this, tr("Error Removing File"), tr("There was an error removing the file at \"%1\"!").arg(path));
		return false;
	}
	this->fileViewer->removeFile(path);
	this->markModified(true);
	return true;
}

void Window::removeDir(const QString& path) {
	this->packFile->removeDirectory(path.toLocal8Bit().constData());
	this->fileViewer->removeDir(path);
	this->markModified(true);
}

void Window::requestEntryRemoval(const QString& path) const {
	this->entryTree->removeEntryByPath(path);
}

void Window::editFile(const QString& oldPath) {
	// Get file data
	auto entry = this->packFile->findEntry(oldPath.toLocal8Bit().constData());
	auto data = this->packFile->readEntry(oldPath.toLocal8Bit().constData());
	if (!entry || !data) {
		QMessageBox::critical(this, tr("Error"), tr("Unable to edit file at \"%1\": could not read file data!").arg(oldPath));
		return;
	}

	// Load existing properties
	EntryCompressionType compressionType = EntryCompressionType::NO_COMPRESS;
	int16_t compressionStrength = 5;
	if (this->packFile->isInstanceOf<BSP>() || this->packFile->isInstanceOf<ZIP>()) {
		if (auto* zip = dynamic_cast<ZIP*>(this->packFile.get())) {
			compressionType = zip->getEntryCompressionType(oldPath.toLocal8Bit().constData());
			compressionStrength = zip->getEntryCompressionStrength(oldPath.toLocal8Bit().constData());
		}
	}

	// Get new properties
	const auto options = EntryOptionsDialog::getEntryOptions(true, false, oldPath, this->packFile->getGUID(), {
		.zip_compressionType = compressionType,
		.zip_compressionStrength = compressionStrength,
		.vpk_preloadBytes = static_cast<uint16_t>(entry->extraData.size()),
		.vpk_saveToDirectory = entry->archiveIndex == VPK_DIR_INDEX,
	}, this);
	if (!options) {
		return;
	}
	const auto [newPath, entryOptions] = *options;

	// Remove file
	this->requestEntryRemoval(oldPath);

	// Add new file with the same info and data at the new path
	this->packFile->addEntry(newPath.toLocal8Bit().constData(), std::move(data.value()), entryOptions);
	this->entryTree->addEntry(newPath);
	if (const auto newEntry = this->packFile->findEntry(newPath.toLocal8Bit().constData())) {
		this->fileViewer->addEntry(*newEntry, newPath);
	}
	this->entryTree->selectEntry(newPath);
	this->markModified(true);
}

void Window::editFileContents(const QString& path, std::vector<std::byte> data) {
	auto entry = this->packFile->findEntry(path.toLocal8Bit().constData());
	if (!entry) {
		return;
	}

	// Load existing properties
	EntryCompressionType compressionType = EntryCompressionType::NO_COMPRESS;
	int16_t compressionStrength = 5;
	if (this->packFile->isInstanceOf<BSP>() || this->packFile->isInstanceOf<ZIP>()) {
		if (auto* zip = dynamic_cast<ZIP*>(this->packFile.get())) {
			compressionType = zip->getEntryCompressionType(path.toLocal8Bit().constData());
			compressionStrength = zip->getEntryCompressionStrength(path.toLocal8Bit().constData());
		}
	}

	this->packFile->removeEntry(path.toLocal8Bit().constData());
	this->packFile->addEntry(path.toLocal8Bit().constData(), std::move(data), {
		.zip_compressionType = compressionType,
		.zip_compressionStrength = compressionStrength,
		.vpk_preloadBytes = static_cast<uint16_t>(entry->extraData.size()),
		.vpk_saveToDirectory = entry->archiveIndex == VPK_DIR_INDEX,
	});
	this->markModified(true);
}

void Window::editFileContents(const QString& path, const QString& data) {
	auto entry = this->packFile->findEntry(path.toLocal8Bit().constData());
	if (!entry) {
		return;
	}
	auto byteData = data.toLocal8Bit();

	// Load existing properties
	EntryCompressionType compressionType = EntryCompressionType::NO_COMPRESS;
	int16_t compressionStrength = 5;
	if (this->packFile->isInstanceOf<BSP>() || this->packFile->isInstanceOf<ZIP>()) {
		if (auto* zip = dynamic_cast<ZIP*>(this->packFile.get())) {
			compressionType = zip->getEntryCompressionType(path.toLocal8Bit().constData());
			compressionStrength = zip->getEntryCompressionStrength(path.toLocal8Bit().constData());
		}
	}

	this->packFile->removeEntry(path.toLocal8Bit().constData());
	this->packFile->addEntry(path.toLocal8Bit().constData(), std::vector<std::byte>{
		reinterpret_cast<std::byte*>(byteData.data()),
		reinterpret_cast<std::byte*>(byteData.data()) + byteData.size(),
	}, {
		.zip_compressionType = compressionType,
		.zip_compressionStrength = compressionStrength,
		.vpk_preloadBytes = static_cast<uint16_t>(entry->extraData.size()),
		.vpk_saveToDirectory = entry->archiveIndex == VPK_DIR_INDEX,
	});
	this->markModified(true);
}

void Window::renameFile(const QString& oldPath, const QString& newPath_) {
	// Get new path
	QString newPath = newPath_;
	if (newPath.isEmpty()) {
		bool ok;
		newPath = QInputDialog::getText(this, tr("Rename File"), tr("The new path:"), QLineEdit::Normal, oldPath, &ok);
		if (!ok || newPath.isEmpty()) {
			return;
		}
	}

	// Get data
	auto entry = this->packFile->findEntry(oldPath.toLocal8Bit().constData());
	if (!entry) {
		return;
	}
	auto entryData = this->packFile->readEntry(oldPath.toLocal8Bit().constData());
	if (!entryData) {
		return;
	}

	// Load existing properties
	auto compressionType = EntryCompressionType::NO_COMPRESS;
	int16_t compressionStrength = 5;
	if (this->packFile->isInstanceOf<BSP>() || this->packFile->isInstanceOf<ZIP>()) {
		if (const auto* zip = dynamic_cast<ZIP*>(this->packFile.get())) {
			compressionType = zip->getEntryCompressionType(oldPath.toLocal8Bit().constData());
			compressionStrength = zip->getEntryCompressionStrength(oldPath.toLocal8Bit().constData());
		}
	}

	// Remove file
	this->requestEntryRemoval(oldPath);

	// Add new file with the same info and data at the new path
	this->packFile->addEntry(newPath.toLocal8Bit().constData(), std::move(entryData.value()), {
		.zip_compressionType = compressionType,
		.zip_compressionStrength = compressionStrength,
		.vpk_preloadBytes = static_cast<uint16_t>(entry->extraData.size()),
		.vpk_saveToDirectory = entry->archiveIndex == VPK_DIR_INDEX,
	});
	this->entryTree->addEntry(newPath);
	if (const auto newEntry = this->packFile->findEntry(newPath.toLocal8Bit().constData())) {
		this->fileViewer->addEntry(*newEntry, newPath);
	}
}

void Window::renameDir(const QString& oldPath, const QString& newPath_) {
	// Get new path
	QString newPath = newPath_;
	if (newPath.isEmpty()) {
		bool ok;
		newPath = QInputDialog::getText(this, tr("Rename Folder"), tr("The new path:"), QLineEdit::Normal, oldPath, &ok);
		if (!ok || newPath.isEmpty()) {
			return;
		}
	}

	// todo: use the new PackFile::renameDirectory method it'll be way faster

	QList<std::pair<std::string, Entry>> entriesToRename;
	this->packFile->runForAllEntries([&oldPath, &entriesToRename](const std::string& path, const Entry& entry) {
		if (path.starts_with((oldPath + '/').toLocal8Bit().constData())) {
			entriesToRename.emplace_back(path, entry);
		}
	});

	QProgressDialog progressDialog(tr("Renaming folder... Aborting this process will not roll back changes made so far."), tr("Abort"), 0, 0, this);
	progressDialog.setWindowTitle(tr("Rename Folder"));
	progressDialog.setWindowModality(Qt::WindowModal);
	for (const auto& path : entriesToRename | std::views::keys) {
		if (progressDialog.wasCanceled()) {
			break;
		}
		this->renameFile(path.c_str(), newPath + path.substr(oldPath.length()).c_str());
	}
	this->markModified(true);
}

void Window::generateKeyPairFiles(const QString& name) {
	auto path = name;
	if (path.isEmpty()) {
		path = QInputDialog::getText(this, tr("Keypair Filename"), tr("Name of the keypair files to generate:"));
		if (path.isEmpty()) {
			return;
		}
		path = (std::filesystem::path{this->packFile->getFilepath()}.parent_path() / path.toLocal8Bit().constData()).string().c_str();
	}
	VPK::generateKeyPairFiles(path.toLocal8Bit().constData());
}

void Window::signPackFile(const QString& privateKeyLocation) {
	auto privateKeyPath = privateKeyLocation;
	if (privateKeyPath.isEmpty()) {
		privateKeyPath = QFileDialog::getOpenFileName(this, tr("Open Private Key File"),
													  std::filesystem::path{this->packFile->getFilepath()}.parent_path().string().c_str(),
													  "Private Key (*.privatekey.vdf);;All Files (*)");
	}
	if (privateKeyPath.isEmpty()) {
		return;
	}
	if (this->packFile->isInstanceOf<VPK>() && dynamic_cast<VPK&>(*this->packFile).sign(privateKeyPath.toLocal8Bit().constData())) {
		QMessageBox::information(this, tr("Success"), tr("Successfully signed the pack file."));
	} else {
		QMessageBox::information(this, tr("Error"), tr("Failed to sign the pack file! Check the file contains both the private key and public key."));
	}
}

std::optional<std::vector<std::byte>> Window::readBinaryEntry(const QString& path) const {
	return this->packFile->readEntry(path.toLocal8Bit().constData());
}

QString Window::getLastFileReadError() const {
	if (!this->packFile) {
		return {};
	}
	if (auto* rvpk = dynamic_cast<const RespawnVPK*>(this->packFile.get())) {
		const auto err = rvpk->getLastError();
		if (!err.empty()) {
			return QString::fromUtf8(err.data(), static_cast<qsizetype>(err.size()));
		}
	}
	return {};
}

std::optional<QString> Window::readTextEntry(const QString& path) const {
	auto binData = this->packFile->readEntry(path.toLocal8Bit().constData());
	if (!binData) {
		return std::nullopt;
	}
	QByteArrayView textBuffer{reinterpret_cast<const char*>(binData->data()), static_cast<qsizetype>(binData->size())};
	auto potentialEncoding = QStringConverter::encodingForData(textBuffer);
	if (!potentialEncoding) {
		return {textBuffer.toByteArray()};
	}
	QStringDecoder utf8Converter{*potentialEncoding};
	return utf8Converter(textBuffer);
}

void Window::selectEntryInEntryTree(const QString& path) const {
	this->entryTree->selectEntry(path);
}

void Window::selectEntryInFileViewer(const QString& path) const {
	this->fileViewer->displayEntry(path);
}

void Window::selectDirInFileViewer(const QString& path, const QList<QString>& subfolders, const QList<QString>& entryPaths) const {
	this->fileViewer->displayDir(path, subfolders, entryPaths, *this->packFile);
}

bool Window::hasEntry(const QString& path) const {
	return this->entryTree->hasEntry(path);
}

void Window::selectSubItemInDir(const QString& path) const {
	this->entryTree->selectSubItem(path);
}

void Window::extractFile(const QString& entryPath, QString savePath) {
	if (savePath.isEmpty()) {
		QString filter;
		if (auto index = entryPath.lastIndexOf('.'); index >= 0) {
			auto fileExt = entryPath.sliced(index); // ".ext"
			auto fileExtPretty = fileExt.toUpper();
			fileExtPretty.remove('.');

			filter = fileExtPretty + " (*" + fileExt + ");;All files (*.*)";
		}
		savePath = QFileDialog::getSaveFileName(this, tr("Extract as..."), entryPath, filter);
	}
	if (savePath.isEmpty()) {
		return;
	}
	this->writeEntryToFile(entryPath, savePath);
}

void Window::extractFilesIf(const std::function<bool(const QString&)>& predicate, const QString& savePath) {
	QString saveDir = savePath;
	if (saveDir.isEmpty()) {
		saveDir = QFileDialog::getExistingDirectory(this, tr("Extract to..."));
	}
	if (saveDir.isEmpty()) {
		return;
	}

	// Set up progress bar
	this->statusText->hide();
	this->statusProgressBar->show();

	// Get progress bar maximum
	int progressBarMax = 0;
	this->packFile->runForAllEntries([&predicate, &progressBarMax](const std::string& path, const Entry& entry) {
		if (predicate(QString(path.c_str()))) {
			progressBarMax++;
		}
	});

	this->statusProgressBar->setRange(0, progressBarMax);
	this->statusProgressBar->setValue(0);

	this->freezeActions(true);

	// Set up thread
	this->extractPackFileWorkerThread = new QThread(this);
	auto* worker = new ExtractPackFileWorker();
	worker->moveToThread(this->extractPackFileWorkerThread);
	QObject::connect(this->extractPackFileWorkerThread, &QThread::started, worker, [this, worker, saveDir, predicate] {
		worker->run(this, saveDir, predicate);
	});
	QObject::connect(worker, &ExtractPackFileWorker::progressUpdated, this, [this](int value) {
		this->statusProgressBar->setValue(value);
	});
	QObject::connect(worker, &ExtractPackFileWorker::taskFinished, this, [this, saveDir](bool noneFailed, const QString& details) {
		// Kill thread
		this->extractPackFileWorkerThread->quit();
		this->extractPackFileWorkerThread->wait();
		delete this->extractPackFileWorkerThread;
		this->extractPackFileWorkerThread = nullptr;

		this->freezeActions(false);

		this->resetStatusBar();

		if (!noneFailed) {
			QString msg = tr(R"(Failed to write some or all files to "%1". Please ensure that a game or another application is not using the file, and that you have sufficient permissions to write to the save location.)").arg(saveDir);
			if (!details.isEmpty()) {
				msg += "\n\n" + details;
			}
			QMessageBox::critical(this, tr("Error"), msg);
		}
	});
	this->extractPackFileWorkerThread->start();
}

void Window::extractDir(const QString& path, const QString& saveDir) {
	this->extractFilesIf([path](const QString& entryPath) { return entryPath.startsWith(path + '/'); }, saveDir);
}

void Window::extractPaths(const QStringList& paths, const QString& saveDir) {
	this->entryTree->extractEntries(paths, saveDir);
}

void Window::createDrag(const QStringList& paths) {
	this->entryTree->createDrag(paths);
}

void Window::extractAll(QString saveDir) {
	if (saveDir.isEmpty()) {
		saveDir = QFileDialog::getExistingDirectory(this, tr("Extract to..."));
	}
	if (saveDir.isEmpty()) {
		return;
	}
	saveDir += '/';
	saveDir += this->packFile->getFilestem().c_str();

	// If this is a Respawn VPK, prefer `revpk` so we also get a matching manifest (flags) that can be used for repacking
	if (this->packFile && this->packFile->isInstanceOf<RespawnVPK>()) {
		const bool useRevpk = Options::get<bool>(OPT_REVPK_USE_FOR_RESPAWN_PACK_UNPACK);
		const auto revpkExe = useRevpk ? tryFindRevpkExe() : QString{};
		if (!revpkExe.isEmpty()) {
			auto vpkPath = this->getLoadedPackFilePath();
			vpkPath.replace('\\', '/');
			saveDir.replace('\\', '/');

			// Set up progress bar
			this->statusText->hide();
			this->statusProgressBar->show();
			this->statusBar()->show();

			this->statusProgressBar->setValue(0);
			this->statusProgressBar->setRange(0, 0);

			// External revpk runs out-of-process; keep navigation UI usable
			this->revpkBusyEnter();

			// Run in background thread
			this->extractPackFileWorkerThread = new QThread(this);
			auto* worker = new IndeterminateProgressWorker();
			worker->moveToThread(this->extractPackFileWorkerThread);

			struct Result {
				bool ok = false;
				QString log;
			};
			auto result = std::make_shared<Result>();

			QPointer<Window> wnd(this);
			QObject::connect(this->extractPackFileWorkerThread, &QThread::started, worker, [worker, revpkExe, vpkPath, saveDir, result, wnd] {
				worker->run([revpkExe, vpkPath, saveDir, result, wnd] {
					// sanitize=1 allows passing a numbered pack file and still unpacking via its dir name
					// (revpk will try to locate a corresponding locale-prefixed dir file)
					const QStringList args = {"-unpack", vpkPath, saveDir, "1"};
					const auto cmdLine = formatRevpkCommandLine(revpkExe, args);
					if (wnd) {
						QMetaObject::invokeMethod(wnd, [wnd, cmdLine] {
							if (!wnd) return;
							wnd->showRevpkLogs();
							wnd->appendRevpkLog("revpk -unpack (running)", cmdLine);
						}, Qt::QueuedConnection);
					}

					const auto emitChunk = [wnd](const QString& chunk) {
						if (!wnd || chunk.isEmpty()) return;
						QMetaObject::invokeMethod(wnd, [wnd, chunk] {
							if (wnd) wnd->appendRevpkLogRaw(chunk);
						}, Qt::QueuedConnection);
					};

					const auto rr = runRevpkLive(revpkExe, args, emitChunk);
					result->ok = rr.ok;
					result->log = rr.started ? rr.mergedLog : rr.startError;
					if (wnd) {
						QMetaObject::invokeMethod(wnd, [wnd, rr] {
							if (!wnd) return;
							if (!rr.started) {
								wnd->appendRevpkLog("revpk -unpack (failed)", rr.startError);
								return;
							}
							const auto status = QString{"ExitCode=%1 ExitStatus=%2"}
								.arg(rr.exitCode)
								.arg(rr.exitStatus == QProcess::NormalExit ? "NormalExit" : "CrashExit");
							wnd->appendRevpkLog(rr.ok ? "revpk -unpack (ok)" : "revpk -unpack (failed)", status);
						}, Qt::QueuedConnection);
					}
				});
			});

			QObject::connect(worker, &IndeterminateProgressWorker::taskFinished, this, [this, worker, result, saveDir] {
				this->extractPackFileWorkerThread->quit();
				this->extractPackFileWorkerThread->wait();
				delete this->extractPackFileWorkerThread;
				this->extractPackFileWorkerThread = nullptr;

				worker->deleteLater();

				this->revpkBusyLeave();
				this->resetStatusBar();

				if (!result->ok) {
					QMessageBox::critical(this, tr("Error"),
						tr(R"(Failed to write some or all files to "%1". Please ensure that a game or another application is not using the file, and that you have sufficient permissions to write to the save location.)")
							.arg(saveDir) + "\n\n" + result->log);
				}
			});

			this->extractPackFileWorkerThread->start();
			return;
		}
	}

	this->extractFilesIf([](const QString&) { return true; }, saveDir);
}

void Window::setDropEnabled(bool dropEnabled_) {
	this->dropEnabled = dropEnabled_;
}

void Window::markModified(bool modified_) {
	if (this->isReadOnly()) {
		return;
	}

	this->setWindowModified(modified_);
	this->saveAction->setDisabled(!this->isWindowModified());
}

bool Window::promptUserToKeepModifications() {
	auto response = QMessageBox::warning(this,
            tr("Save changes?"),
            tr("This file has unsaved changes! Would you like to save these changes first?"),
            QMessageBox::Ok | QMessageBox::Discard | QMessageBox::Cancel);
	switch (response) {
		case QMessageBox::Cancel:
			return true;
		case QMessageBox::Discard:
			return false;
		case QMessageBox::Ok:
			this->savePackFile(false, false);
			return false;
		default:
			break;
	}
	return true;
}

bool Window::clearContents() {
	if (this->isWindowModified() && this->promptUserToKeepModifications()) {
		return false;
	}

	this->statusText->clear();
	this->statusProgressBar->hide();
	this->statusBar()->hide();

	this->searchBar->clear();
	this->searchBar->setDisabled(true);

	this->entryTree->clearContents();
	this->entryTree->setDisabled(true);

	this->fileViewer->clearContents(true);

	this->markModified(false);
	this->freezeActions(true, false); // Leave create/open unfrozen

	return true;
}

void Window::freezeActions(bool freeze, bool freezeCreationActions, bool freezeFileViewer) const {
	this->createEmptyMenu->setDisabled(freeze && freezeCreationActions);
	this->createFromDirMenu->setDisabled(freeze && freezeCreationActions);
	this->openAction->setDisabled(freeze && freezeCreationActions);
	this->openDirAction->setDisabled(freeze && freezeCreationActions);
	this->openRelativeToMenu->setDisabled(freeze && freezeCreationActions);
	this->openRecentMenu->setDisabled(freeze && freezeCreationActions);
	this->saveAction->setDisabled(freeze || !this->isWindowModified());
	this->saveAsAction->setDisabled(freeze);
	this->closeFileAction->setDisabled(freeze);
	this->extractAllAction->setDisabled(freeze);
	this->extractConvertSelectedPngAction->setDisabled(freeze);
	this->extractConvertSelectedTgaAction->setDisabled(freeze);
	this->extractConvertSelectedDdsBc7Action->setDisabled(freeze);
	this->addFileAction->setDisabled(freeze);
	this->addDirAction->setDisabled(freeze);
	this->markModifiedAction->setDisabled(freeze);
	this->setPropertiesAction->setDisabled(freeze);
	this->toolsGeneralMenu->setDisabled(freeze);
	this->toolsVPKMenu->setDisabled(freeze || (!this->packFile || !this->packFile->isInstanceOf<VPK>()));

	this->searchBar->setDisabled(freeze);
	this->entryTree->setDisabled(freeze);
	this->fileViewer->setDisabled(freeze && freezeFileViewer);
}

void Window::freezeModifyActions(bool readOnly) const {
	if (readOnly) {
		this->saveAction->setDisabled(readOnly);
		this->saveAsAction->setDisabled(readOnly);
		this->addFileAction->setDisabled(readOnly);
		this->addDirAction->setDisabled(readOnly);
		this->markModifiedAction->setDisabled(readOnly);
		this->setPropertiesAction->setDisabled(readOnly);
	}
}

void Window::registerPlugin(const QString& path, QIcon icon, const QJsonObject& metadata) {
	const QString nameWithVersion = tr("%1 v%2.%3.%4")
		.arg(metadata["name"].toString("<Unknown Name>"))
		.arg(metadata["versionMajor"].toInt(0))
		.arg(metadata["versionMinor"].toInt(0))
		.arg(metadata["versionPatch"].toInt(0));

	static constexpr auto snakeCaseToHuman = [](const QString& in) {
		QString out;
		bool caps = true;
		for (auto c : in) {
			if (c == '_') {
				c = ' ';
				caps = true;
			} else if (caps) {
				c = c.toUpper();
				caps = false;
			}
			out += c;
		}
		return out;
	};

	QString linksList;
	for (const auto& linkName : metadata["links"].toObject({}).keys()) {
		if (const auto& link = metadata["links"][linkName]; link.isString()) {
			if (linksList.isEmpty()) {
				linksList += tr("#### Links\n");
			}
			linksList += QString{"- [%1](%2)\n"}.arg(snakeCaseToHuman(linkName), link.toString());
		}
	}

	this->toolsPluginInformationMenu->addAction(icon, nameWithVersion, [
		this,
		path,
		pixmap = (icon.isNull() ? QIcon{":/icons/missing.png"} : icon).pixmap({128, 128}),
		nameWithVersion,
		author = metadata["author"].toString("<Unknown Author>"),
		description = metadata["description"].toString("<Empty description.>"),
		linksList
	] {
		QMessageBox box{
			QMessageBox::NoIcon,
			tr("Plugin Information"),
			tr("## %1\n\n*Authored by %5*\n\n%6\n\n%7\n\n#### Location\n`%8`").arg(nameWithVersion).arg(author).arg(description).arg(linksList).arg(path),
			QMessageBox::Close,
			this,
		};
		box.setIconPixmap(pixmap);
		box.setTextFormat(Qt::MarkdownText);
		box.exec();
	});
}

void Window::pluginsInitContextMenu(const EntryContextMenuData* contextMenu) const {
	this->fileViewer->pluginsInitContextMenu(contextMenu);
}

void Window::pluginsUpdateContextMenu(int contextMenuType, const QStringList& paths) const {
	this->fileViewer->pluginsUpdateContextMenu(contextMenuType, paths);
}

void Window::mousePressEvent(QMouseEvent* event) {
	if (event->button() == Qt::BackButton) {
		this->fileViewer->requestNavigateBack();
		event->accept();
	} else if (event->button() == Qt::ForwardButton) {
		this->fileViewer->requestNavigateNext();
		event->accept();
	}
}

void Window::dragEnterEvent(QDragEnterEvent* event) {
	if (!event->mimeData()->hasUrls()) {
		return;
	}

	for (auto& url : event->mimeData()->urls()) {
		if (!url.isLocalFile()) {
			return;
		}
	}

	if (this->fileViewer->isDirPreviewVisible()) {
		// If file viewer is open, it'll just add the files to the open pack file
		event->acceptProposedAction();
	} else if (!this->packFile) {
		// If we don't have a pack file open, and the path is a pack file, we can load it instead
		auto path = event->mimeData()->urls()[0].path();
		auto fileTypes = PackFile::getOpenableExtensions();
		if (std::any_of(fileTypes.begin(), fileTypes.end(), [&path](const std::string& extension) { return path.endsWith(extension.c_str()); })) {
			event->acceptProposedAction();
		}
	}
}

void Window::dropEvent(QDropEvent* event) {
	if (!event->mimeData()->hasUrls()) {
		return;
	}

	if (!this->packFile) {
		// If we don't have a pack file open, try loading the one given (extension's already been verified)
		this->loadPackFile(event->mimeData()->urls()[0].toLocalFile());
		return;
	}

	if (!this->dropEnabled || !this->fileViewer->isDirPreviewVisible()) {
		return;
	}
	for(const QUrl& url : event->mimeData()->urls()) {
		QFileInfo info(url.toLocalFile());
		if (!info.exists()) {
			continue;
		}
		const auto& relativePath = this->fileViewer->getDirPreviewCurrentPath();
		if (info.isFile()) {
			this->addFile(false, relativePath, info.absoluteFilePath());
		} else {
			this->addDir(false, relativePath, info.absoluteFilePath());
		}
	}
}

void Window::closeEvent(QCloseEvent* event) {
	if (this->isWindowModified() && this->promptUserToKeepModifications()) {
		event->ignore();
		return;
	}

	// Delete temp folders
	for (auto& dir : TempDir::createdTempDirs()) {
		dir.removeRecursively();
	}
	TempDir::createdTempDirs().clear();

	// Write location and sizing
	auto* settings = Options::getOptions();
	settings->beginGroup("main_window");
	settings->setValue("geometry", this->saveGeometry());
	settings->setValue("state", this->saveState());
	settings->setValue("maximized", this->isMaximized());
	if (!this->isMaximized()) {
		settings->setValue("position", this->pos());
		settings->setValue("size", this->size());
	}
	settings->endGroup();

	event->accept();
}

bool Window::loadDir(const QString& path)
{
    return this->loadPackFile(
        path,
        Folder::open(path.toLocal8Bit().constData())
    );
}

bool Window::loadPackFile(const QString& path) {
	// Respawn packs are typically opened via the language-specific directory VPK (e.g. englishclient_*_dir.vpk)
	// If the user tries to open a non-language client/server VPK (archive part or even the non-english dir VPK),
	// redirect to the corresponding english* directory VPK when present
	{
		auto toLower = [](std::string s) {
			for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return s;
		};
		auto looksLikeClientOrServerVpk = [&](const std::filesystem::path& p, const std::string& nameLower) -> bool {
			if (p.extension() != ".vpk") {
				return false;
			}
			// Avoid redirect loops
			if (nameLower.rfind("englishclient", 0) == 0 || nameLower.rfind("englishserver", 0) == 0) {
				return false;
			}
			return (nameLower.rfind("client", 0) == 0) || (nameLower.rfind("server", 0) == 0);
		};
		auto tryBuildEnglishDirName = [&](const std::filesystem::path& p, const std::string& nameLower) -> std::optional<std::string> {
			if (!looksLikeClientOrServerVpk(p, nameLower)) {
				return std::nullopt;
			}

			std::string stemLower = toLower(p.stem().string());

			// If this is an archive part (`..._000.vpk`), convert to dir VPK name (`..._dir.vpk`)
			// Examples:
			// `client_frontend.bsp.pak000_000.vpk` -> `client_frontend.bsp.pak000_dir.vpk`
			// `server_mp_rr_...pak000_001.vpk`     -> `server_mp_rr_...pak000_dir.vpk`
			if (stemLower.size() >= 4 && stemLower[stemLower.size() - 4] == '_' &&
				std::isdigit(static_cast<unsigned char>(stemLower[stemLower.size() - 3])) &&
				std::isdigit(static_cast<unsigned char>(stemLower[stemLower.size() - 2])) &&
				std::isdigit(static_cast<unsigned char>(stemLower[stemLower.size() - 1]))) {
				stemLower.erase(stemLower.size() - 4);
				stemLower += "_dir";
			}

			// We only want to redirect to english* dir VPKs
			if (stemLower.size() < 4 || stemLower.rfind("_dir") != stemLower.size() - 4) {
				return std::nullopt;
			}

			return std::string{"english"} + stemLower + ".vpk";
		};

		const auto fsPath = std::filesystem::path{path.toLocal8Bit().constData()};
		const auto nameLower = toLower(fsPath.filename().string());
		if (auto englishName = tryBuildEnglishDirName(fsPath, nameLower)) {
			const auto englishPath = (fsPath.parent_path() / *englishName).string();
			std::error_code ec;
			if (std::filesystem::is_regular_file(englishPath, ec)) {
				return this->loadPackFile(QString::fromLocal8Bit(englishPath.c_str()));
			}
		}
	}

	// Prefer Respawn VPK handler when the header matches, to avoid registry-order issues with Valve VPK
	// Respawn and Valve VPKs share the same signature/version header, so we must gate this on filename heuristics
	// Titanfall 2 can store the directory tree in `..._000.vpk` (no `_dir.vpk`)
	if (path.endsWith(".vpk", Qt::CaseInsensitive) && looksLikeRespawnVpkByName(path)) {
		const std::string pathStr = path.toLocal8Bit().constData();
		if (auto rvpk = RespawnVPK::open(pathStr)) {
			return this->loadPackFile(path, std::move(rvpk));
		}

		// If the user picked a numbered part (e.g. `..._013.vpk`), try opening the corresponding dir file
		// Prefer `_dir.vpk` if present, otherwise fall back to `_000.vpk` (TF2-style)
		const auto p = std::filesystem::path{pathStr};
		const auto stem = p.stem().string();
		if (stem.size() >= 4 && stem[stem.size() - 4] == '_' &&
			std::isdigit(static_cast<unsigned char>(stem[stem.size() - 3])) &&
			std::isdigit(static_cast<unsigned char>(stem[stem.size() - 2])) &&
			std::isdigit(static_cast<unsigned char>(stem[stem.size() - 1]))) {
			const auto baseStem = stem.substr(0, stem.size() - 4);

			const auto dirCandidate = (p.parent_path() / (baseStem + "_dir.vpk")).string();
			if (auto rvpk2 = RespawnVPK::open(dirCandidate)) {
				return this->loadPackFile(QString::fromLocal8Bit(dirCandidate.c_str()), std::move(rvpk2));
			}

			const auto zeroCandidate = (p.parent_path() / (baseStem + "_000.vpk")).string();
			if (auto rvpk3 = RespawnVPK::open(zeroCandidate)) {
				return this->loadPackFile(QString::fromLocal8Bit(zeroCandidate.c_str()), std::move(rvpk3));
			}
		}
	}

	return this->loadPackFile(path, PackFile::open(path.toLocal8Bit().constData(), nullptr, [this](PackFile* packFile_, PackFile::OpenProperty property) -> std::vector<std::byte> {
		if (packFile_->getGUID() == GCF::GUID && property == PackFile::OpenProperty::DECRYPTION_KEY) {
			auto* dialog = new QInputDialog{this};
			dialog->setWindowTitle(tr("Encrypted Pack File"));
			dialog->setLabelText(tr("Decryption key for depot ID %1:").arg(dynamic_cast<GCF*>(packFile_)->getAppID()));
			dialog->setInputMode(QInputDialog::TextInput);
			dialog->setTextEchoMode(QLineEdit::Normal);
			auto* dialogLineEdit = dialog->findChild<QLineEdit*>();
			if (!dialogLineEdit) {
				return {};
			}
			dialogLineEdit->setInputMask("HHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHH");
			dialogLineEdit->setText("00000000000000000000000000000000");
			dialogLineEdit->setMaxLength(32);
			dialogLineEdit->setMinimumWidth(275);
			if (!dialog->exec()) {
				return {};
			}
			auto text = dialog->textValue();
			while (text.length() < 32) {
				text.prepend('0');
			}
			return sourcepp::crypto::decodeHexString(text.toUtf8().constData());
		}
		return {};
	}));
}

bool Window::loadPackFile(const QString& path, std::unique_ptr<vpkpp::PackFile>&& newPackFile) {
	if (!this->clearContents()) {
		return false;
	}
	this->freezeActions(true);

	auto recentPaths = Options::get<QStringList>(STR_OPEN_RECENT);

	QString fixedPath = QDir(path).absolutePath();
	fixedPath.replace('\\', '/');

	this->packFile = std::move(newPackFile);
	if (!this->packFile) {
		// Remove from recent paths if it's there
		if (recentPaths.contains(fixedPath)) {
			recentPaths.removeAt(recentPaths.indexOf(fixedPath));
			Options::set(STR_OPEN_RECENT, recentPaths);
			this->rebuildOpenRecentMenu(recentPaths);
		}

		QMessageBox::critical(this, tr("Error"), tr("Unable to load this file. Please ensure that a game or another application is not using the file."));
		(void) this->clearContents();
		return false;
	}

	// Reset properties that we care about
	this->packFileOptions.compressionType = EntryCompressionType::NO_COMPRESS;
	this->packFileOptions.compressionStrength = 5;

	// Add to recent paths
	QString loadedPath{this->packFile->getFilepath().data()};
	if (!recentPaths.contains(loadedPath)) {
		recentPaths.push_front(loadedPath);
		if (recentPaths.size() > 10) {
			recentPaths.pop_back();
		}
		Options::set(STR_OPEN_RECENT, recentPaths);
		this->rebuildOpenRecentMenu(recentPaths);
	} else if (auto pathIndex = recentPaths.indexOf(loadedPath); pathIndex > 0) {
		recentPaths.remove(pathIndex);
		recentPaths.push_front(loadedPath);
		Options::set(STR_OPEN_RECENT, recentPaths);
		this->rebuildOpenRecentMenu(recentPaths);
	}

	this->statusText->hide();
	this->statusProgressBar->show();
	this->statusBar()->show();

	this->entryTree->loadPackFile(*this->packFile, this->statusProgressBar, [this] {
		this->freezeActions(false);
		this->freezeModifyActions(this->isReadOnly());

		this->resetStatusBar();
	});

	this->fileViewer->setReadOnly(this->packFile->isReadOnly());

	return true;
}

void Window::rebuildOpenInMenu() {
	this->openRelativeToMenu->clear();
	auto* loadingGamesAction = this->openRelativeToMenu->addAction(tr("Loading installed games..."));
	loadingGamesAction->setDisabled(true);

	// Set up thread
	this->scanSteamGamesWorkerThread = new QThread(this);
	auto* worker = new ScanSteamGamesWorker();
	worker->moveToThread(this->scanSteamGamesWorkerThread);
	QObject::connect(this->scanSteamGamesWorkerThread, &QThread::started, worker, [worker] {
		worker->run();
	});
	QObject::connect(worker, &ScanSteamGamesWorker::taskFinished, this, [this](const QList<std::tuple<QString, QIcon, QDir>>& sourceGames) {
		// Add them to the menu
		this->openRelativeToMenu->clear();
		if (!sourceGames.empty()) {
			for (const auto& [gameName, icon, relativeDirectoryPath] : sourceGames) {
				const auto relativeDirectory = relativeDirectoryPath.path();
				this->openRelativeToMenu->addAction(icon, gameName, [this, relativeDirectory] {
					this->openPackFile(relativeDirectory);
				});
			}
		} else {
			auto* noGamesDetectedAction = this->openRelativeToMenu->addAction(tr("No games detected."));
			noGamesDetectedAction->setDisabled(true);
		}

		// Kill thread
		this->scanSteamGamesWorkerThread->quit();
		this->scanSteamGamesWorkerThread->wait();
		delete this->scanSteamGamesWorkerThread;
		this->scanSteamGamesWorkerThread = nullptr;
	});
	this->scanSteamGamesWorkerThread->start();
}

void Window::rebuildOpenRecentMenu(const QStringList& paths) {
	this->openRecentMenu->clear();
	if (paths.empty()) {
		auto* openRecentVPKMenuNoRecentFilesAction = this->openRecentMenu->addAction(tr("No recent files."));
		openRecentVPKMenuNoRecentFilesAction->setDisabled(true);
		return;
	}
	for (int i = 0; i < paths.size(); i++) {
		this->openRecentMenu->addAction(("&%1: \"" + paths[i] + "\"").arg((i + 1) % 10), [this, path=paths[i]] {
			if (std::filesystem::is_directory(path.toLocal8Bit().constData())) {
				this->loadDir(path);
			} else {
				this->loadPackFile(path);
			}
		});
	}
	this->openRecentMenu->addSeparator();
	this->openRecentMenu->addAction(tr("Clear"), [this] {
		Options::set(STR_OPEN_RECENT, QStringList{});
		this->rebuildOpenRecentMenu({});
	});
}

bool Window::writeEntryToFile(const QString& entryPath, const QString& filepath) {
	return this->packFile->extractEntry(entryPath.toLocal8Bit().constData(), filepath.toLocal8Bit().constData());
}

void Window::resetStatusBar() {
	// hack: replace the name of the pack file with something unique and substitute
	// it back later to avoid messing with it when doing translation substitutions
	static constexpr auto* PACK_FILE_NAME_REPLACEMENT = "\03383E7593B3B494FE0873C42BC3FC88DC5\033";
	QString packFileStatus(std::string{*this->packFile}.c_str());
	packFileStatus
		.replace(this->packFile->getTruncatedFilename().c_str(), PACK_FILE_NAME_REPLACEMENT)
		.replace("AppID", tr("AppID"))
		.replace("App Version", tr("App Version"))
		.replace("Godot Version", tr("Godot Version"))
		.replace("Version", tr("Version"))
		.replace("Map Revision", tr("Map Revision"))
		.replace("Addon Name:", tr("Addon Name:"))
		.replace("Embedded", tr("Embedded"))
		.replace("Encrypted", tr("Encrypted"))
		.replace(PACK_FILE_NAME_REPLACEMENT, this->packFile->getTruncatedFilename().c_str());
	this->statusText->setText(' ' + tr("Loaded") + ' ' + packFileStatus);
	this->statusText->show();
	this->statusProgressBar->hide();
}

void IndeterminateProgressWorker::run(const std::function<void()>& fn) {
	fn();
	emit this->taskFinished();
}

void SavePackFileWorker::run(Window* window, const QString& savePath, BakeOptions options, bool async) {
	std::unique_ptr<QEventLoop> loop;
	if (!async) {
		loop = std::make_unique<QEventLoop>();
	}
	int currentEntry = 0;
	bool success = window->packFile->bake(savePath.toLocal8Bit().constData(), options, [this, loop_=loop.get(), &currentEntry](const std::string&, const Entry&) {
		emit this->progressUpdated(++currentEntry);
		if (loop_) {
			loop_->processEvents();
		}
	});
	emit this->taskFinished(success);
}

void ExtractPackFileWorker::run(Window* window, const QString& saveDir, const std::function<bool(const QString&)>& predicate) {
	int currentEntry = 0;
	bool out = true;
	QString details;

	// Manual extraction so we can surface more detail than the boolean PackFile::extractAll result
	try {
		const auto saveDirStd = std::string{saveDir.toLocal8Bit().constData()};
		const std::filesystem::path outputDirPath{saveDirStd};

		std::string firstFailedEntryPath;
		std::string firstFailedReason;

		window->packFile->runForAllEntries([&](const std::string& path, const Entry& entry) {
			(void) entry;
			emit this->progressUpdated(++currentEntry);

			if (!predicate(QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size())))) {
				return;
			}

			const auto savePath = vpkpp::PackFile::escapeEntryPathForWrite(path);
			const auto dstPath = (outputDirPath / std::filesystem::path{savePath}).string();

			// Respawn VPKs can contain very large entries; extract them via streaming to avoid huge allocations
			if (auto* rvpk = dynamic_cast<const RespawnVPK*>(window->packFile.get())) {
				std::string err;
				if (!rvpk->extractEntryToFile(path, dstPath, &err)) {
					out = false;
					if (firstFailedEntryPath.empty()) {
						firstFailedEntryPath = path;
						firstFailedReason = !err.empty() ? err : std::string{rvpk->getLastError()};
						if (firstFailedReason.empty()) {
							firstFailedReason = "failed to extract Respawn entry";
						}
					}
				}
				return;
			}

			// Generic extraction path (read into memory, then write)
			const auto data = window->packFile->readEntry(path);
			if (!data) {
				out = false;
				if (firstFailedEntryPath.empty()) {
					firstFailedEntryPath = path;
					firstFailedReason = "failed to read entry bytes";
				}
				return;
			}

			FileStream stream{dstPath, FileStream::OPT_TRUNCATE | FileStream::OPT_CREATE_IF_NONEXISTENT};
			if (!stream) {
				out = false;
				if (firstFailedEntryPath.empty()) {
					firstFailedEntryPath = path;
					firstFailedReason = "failed to open output path for write: " + dstPath;
				}
				return;
			}

			stream.write(*data);
		});

		if (!out && !firstFailedEntryPath.empty()) {
			details = tr("First failure: %1").arg(QString::fromUtf8(firstFailedEntryPath.data(), static_cast<qsizetype>(firstFailedEntryPath.size())));
			if (!firstFailedReason.empty()) {
				details += "\n" + tr("Reason: %1").arg(QString::fromUtf8(firstFailedReason.data(), static_cast<qsizetype>(firstFailedReason.size())));
			}
		}
	} catch (const std::exception& e) {
		out = false;
		details = tr("Exception during extraction: %1").arg(QString::fromLocal8Bit(e.what()));
	} catch (...) {
		out = false;
		details = tr("Unknown exception during extraction.");
	}

	emit this->taskFinished(out, details);
}

void ScanSteamGamesWorker::run() {
	QList<std::tuple<QString, QIcon, QDir>> sourceGames;

	if (Options::get<bool>(OPT_DISABLE_STEAM_SCANNER)) {
		emit this->taskFinished(sourceGames);
		return;
	}

	Steam steam;
	if (!steam) {
		emit this->taskFinished(sourceGames);
		return;
	}

	// Add Steam games
	for (auto appID : steam.getInstalledApps()) {
		if (!steam.isAppUsingGoldSrcEngine(appID) && !steam.isAppUsingSourceEngine(appID) && !steam.isAppUsingSource2Engine(appID)) {
			continue;
		}
		sourceGames.emplace_back(
				QString::fromUtf8(steam.getAppName(appID).data()),
				QIcon{QPixmap::fromImage(ImageLoader::load(QString::fromStdWString(steam.getAppIconPath(appID).wstring())))},
				QDir{QString::fromStdWString(steam.getAppInstallDir(appID).wstring())});
	}

	// Add mods in the sourcemods directory
	for (const auto& modDir : std::filesystem::directory_iterator{steam.getSourceModDir(), std::filesystem::directory_options::skip_permission_denied | std::filesystem::directory_options::follow_directory_symlink}) {
		if (!modDir.is_directory()) {
			continue;
		}

		const auto gameInfoPath = (modDir.path() / "gameinfo.txt").string();
		if (!std::filesystem::exists(gameInfoPath)) {
			continue;
		}

		std::ifstream gameInfoFile{gameInfoPath};
		auto gameInfoSize = std::filesystem::file_size(gameInfoPath);
		std::string gameInfoData;
		gameInfoData.resize(gameInfoSize);
		gameInfoFile.read(gameInfoData.data(), static_cast<std::streamsize>(gameInfoSize));

		KV1 gameInfoRoot{gameInfoData};
		if (gameInfoRoot.isInvalid()) {
			continue;
		}
		const auto& gameInfo = gameInfoRoot["GameInfo"];
		if (gameInfo.isInvalid()) {
			continue;
		}
		const auto& gameInfoName = gameInfo["game"];
		const auto& gameInfoIconPath = gameInfo["icon"];

		std::string modName;
		if (!gameInfoName.isInvalid()) {
			modName = gameInfoName.getValue();
		} else {
			modName = std::filesystem::path{gameInfoPath}.parent_path().filename().string();
		}

		std::string modIconPath;
		if (!gameInfoIconPath.isInvalid()) {
			if (auto modIconBigPath = (modDir.path() / (std::string{gameInfoIconPath.getValue()} + "_big.tga")); std::filesystem::exists(modIconBigPath)) {
				modIconPath = modIconBigPath.string();
			} else if (auto modIconRegularPath = (modDir.path() / (std::string{gameInfoIconPath.getValue()} + ".tga")); std::filesystem::exists(modIconRegularPath)) {
				modIconPath = modIconRegularPath.string();
			}
		}

		sourceGames.emplace_back(
				QString::fromUtf8(modName.c_str()),
				QIcon{QPixmap::fromImage(ImageLoader::load(QString::fromStdWString(std::filesystem::path{modIconPath}.wstring())))},
				QDir{QString::fromStdWString(modDir.path().wstring())});
	}

	// Replace & with && in game names
	for (auto& games : sourceGames) {
		// Having an & before a character makes that the shortcut character and hides the &, so we need to escape it
		std::get<0>(games).replace("&", "&&");
	}

	// Sort games and return
	std::sort(sourceGames.begin(), sourceGames.end(), [](const auto& lhs, const auto& rhs) {
		return std::get<0>(lhs) < std::get<0>(rhs);
	});
	emit this->taskFinished(sourceGames);
}

VPKEditWindowAccess_V3::VPKEditWindowAccess_V3(Window* window_)
		: IVPKEditWindowAccess_V3(window_)
		, window(window_) {}

QSettings* VPKEditWindowAccess_V3::getOptions() const {
	return Options::getOptions();
}

bool VPKEditWindowAccess_V3::isReadOnly() const {
	return this->window->isReadOnly();
}

void VPKEditWindowAccess_V3::addFile(bool showOptions, const QString& startDir, const QString& filePath) const {
	return this->window->addFile(showOptions, startDir, filePath);
}

void VPKEditWindowAccess_V3::addDir(bool showOptions, const QString& startDir, const QString& dirPath) const {
	return this->window->addDir(showOptions, startDir, dirPath);
}

void VPKEditWindowAccess_V3::removeFile(const QString& path) const {
	return this->window->requestEntryRemoval(path);
}

void VPKEditWindowAccess_V3::removeDir(const QString& path) const {
	return this->window->removeDir(path);
}

void VPKEditWindowAccess_V3::editFileContents(const QString& path, const QByteArray& data) const {
	return this->window->editFileContents(path, std::vector<std::byte>{reinterpret_cast<const std::byte*>(data.data()), reinterpret_cast<const std::byte*>(data.data()) + data.size()});
}

void VPKEditWindowAccess_V3::editFileContents(const QString& path, const QString& data) const {
	return this->window->editFileContents(path, data);
}

void VPKEditWindowAccess_V3::renameFile(const QString& oldPath, const QString& newPath) const {
	return this->window->renameFile(oldPath, newPath);
}

void VPKEditWindowAccess_V3::renameDir(const QString& oldPath, const QString& newPath) const {
	return this->window->renameDir(oldPath, newPath);
}

bool VPKEditWindowAccess_V3::readBinaryEntry(const QString& entryPath, QByteArray& data) const {
	const auto file = this->window->readBinaryEntry(entryPath);
	if (!file) {
		return false;
	}
	data = QByteArray{reinterpret_cast<const char*>(file->data()), static_cast<qlonglong>(file->size())};
	return true;
}

bool VPKEditWindowAccess_V3::readTextEntry(const QString& entryPath, QString& data) const {
	const auto file = this->window->readTextEntry(entryPath);
	if (!file) {
		return false;
	}
	data = *file;
	return true;
}

void VPKEditWindowAccess_V3::selectEntryInEntryTree(const QString& entryPath) const {
	return this->window->selectEntryInEntryTree(entryPath);
}

bool VPKEditWindowAccess_V3::hasEntry(const QString& entryPath) const {
	return this->window->hasEntry(entryPath);
}

void VPKEditWindowAccess_V3::selectSubItemInDir(const QString& path) const {
	return this->window->selectSubItemInDir(path);
}

void VPKEditWindowAccess_V3::extractFile(const QString& path, QString savePath) const {
	return this->window->extractFile(path, savePath);
}

void VPKEditWindowAccess_V3::extractDir(const QString& path, const QString& saveDir) const {
	return this->window->extractDir(path, saveDir);
}

void VPKEditWindowAccess_V3::extractPaths(const QStringList& paths, const QString& saveDir) const {
	return this->window->extractPaths(paths, saveDir);
}

void VPKEditWindowAccess_V3::extractAll(QString saveDir) const {
	return this->window->extractAll(saveDir);
}

std::string_view Window::getLoadedPackFileGUID() const {
	if (!this->packFile) {
		return {};
	}
	return this->packFile->getGUID();
}

QString Window::getLoadedPackFilePath() const {
	if (!this->packFile) {
		return {};
	}
	return QString::fromLocal8Bit(this->packFile->getFilepath().data());
}
