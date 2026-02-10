#include "VPKFilePreview.h"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>

#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardItemModel>
#include <QStandardItem>
#include <QTreeView>
#include <QThread>
#include <QVBoxLayout>

#include "../Window.h"

#include <RespawnVPK.h>
#include <vpkpp/vpkpp.h>

namespace {

constexpr std::uint32_t VPK_SIG = 0x55AA1234u;

[[nodiscard]] bool looksLikeRespawnVpkByName(const QString& absPath) {
	const auto name = QFileInfo{absPath}.fileName().toLower();
	return name.contains("pak000_dir") || name.contains("pak000_");
}

static std::optional<std::array<std::uint8_t, 16>> tryReadHeader16(const QString& absPath) {
	std::ifstream f{absPath.toLocal8Bit().constData(), std::ios::binary};
	if (!f) {
		return std::nullopt;
	}
	std::array<std::uint8_t, 16> b{};
	f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(b.size()));
	if (!f) {
		return std::nullopt;
	}
	return b;
}

static std::uint32_t readU32LE(const std::array<std::uint8_t, 16>& b, std::size_t off) {
	return static_cast<std::uint32_t>(b[off])
		| (static_cast<std::uint32_t>(b[off + 1]) << 8)
		| (static_cast<std::uint32_t>(b[off + 2]) << 16)
		| (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

[[nodiscard]] bool endsWithInsensitive(std::string_view s, std::string_view suffix) {
	if (s.size() < suffix.size()) {
		return false;
	}
	for (std::size_t i = 0; i < suffix.size(); i++) {
		const auto a = static_cast<unsigned char>(s[s.size() - suffix.size() + i]);
		const auto b = static_cast<unsigned char>(suffix[i]);
		if (std::tolower(a) != std::tolower(b)) {
			return false;
		}
	}
	return true;
}

class VPKEntriesWorker final : public QObject {
	Q_OBJECT

public:
	explicit VPKEntriesWorker(QString absPath_, std::uint64_t generation_)
		: absPath(std::move(absPath_))
		, generation(generation_) {}

public slots:
	void run() {
		using namespace vpkpp;

		std::string err;
		std::unique_ptr<PackFile> pf;

		const std::string pathStr = absPath.toLocal8Bit().constData();
		const bool nameLooksLikeDir = ::endsWithInsensitive(pathStr, "_dir.vpk");
		const bool looksRespawn = ::looksLikeRespawnVpkByName(this->absPath);

		// VPK v2 shares the same signature/version header as respawn
		if (looksRespawn) {
			if (auto rvpk = RespawnVPK::open(pathStr)) {
				pf = std::move(rvpk);
			}
		}

		if (!pf && nameLooksLikeDir) {
			// If it isn't Respawn but looks like a directory VPK, fall back to the generic opener.
			pf = PackFile::open(pathStr);
		}

		if (!pf) {
			err = "Unable to open directory VPK to enumerate entries.";
			emit finished(QString::fromLocal8Bit(err.c_str()), this->generation, 0);
			return;
		}

		QStringList chunk;
		chunk.reserve(1024);
		std::uint64_t total = 0;

		pf->runForAllEntries([&](const std::string& entryPath, const Entry&) {
			if (this->cancelled.load(std::memory_order_relaxed)) {
				return;
			}
			total++;
			chunk.push_back(QString::fromUtf8(entryPath.data(), static_cast<qsizetype>(entryPath.size())));
			if (chunk.size() >= 1024) {
				emit chunkReady(chunk, this->generation);
				chunk.clear();
			}
		});

		if (!chunk.isEmpty()) {
			emit chunkReady(chunk, this->generation);
		}
		emit finished({}, this->generation, total);
	}

	void cancel() { this->cancelled.store(true, std::memory_order_relaxed); }

signals:
	void chunkReady(const QStringList& chunk, std::uint64_t generation);
	void finished(const QString& error, std::uint64_t generation, std::uint64_t total);

private:
	QString absPath;
	std::uint64_t generation = 0;
	std::atomic_bool cancelled{false};
};

} // namespace

VPKFilePreview::VPKFilePreview(Window* window_, QWidget* parent)
		: QWidget(parent)
		, window(window_) {
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(12, 12, 12, 12);
	layout->setSpacing(10);

	{
		auto* headerRow = new QHBoxLayout();
		headerRow->setContentsMargins(0, 0, 0, 0);

		this->title = new QLabel(this);
		this->title->setTextFormat(Qt::PlainText);
		this->title->setWordWrap(true);
		QFont f = this->title->font();
		f.setPointSize(f.pointSize() + 2);
		f.setBold(true);
		this->title->setFont(f);
		headerRow->addWidget(this->title, 1);

		this->openButton = new QPushButton(tr("Open"), this);
		headerRow->addWidget(this->openButton, 0, Qt::AlignTop);

		layout->addLayout(headerRow);
	}

	this->details = new QLabel(this);
	this->details->setTextFormat(Qt::MarkdownText);
	this->details->setWordWrap(true);
	layout->addWidget(this->details);

	this->entriesSummary = new QLabel(this);
	this->entriesSummary->setTextFormat(Qt::PlainText);
	this->entriesSummary->setWordWrap(true);
	layout->addWidget(this->entriesSummary);

	this->entriesBusy = new QProgressBar(this);
	this->entriesBusy->setRange(0, 0);
	this->entriesBusy->hide();
	layout->addWidget(this->entriesBusy);

	this->entriesModel = new QStandardItemModel(this);
	this->entriesTree = new QTreeView(this);
	this->entriesTree->setModel(this->entriesModel);
	this->entriesTree->setUniformRowHeights(true);
	this->entriesTree->setAnimated(true);
	this->entriesTree->setHeaderHidden(true);
	this->entriesTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
	layout->addWidget(this->entriesTree, 1);

	QObject::connect(this->openButton, &QPushButton::clicked, this, [this] {
		if (this->absPath.isEmpty()) {
			return;
		}
		// Use the public open path entry-point (no file dialog when filePath is provided).
		this->window->openPackFile({}, this->absPath);
	});
}

VPKFilePreview::~VPKFilePreview() {
	this->stopEntriesWorker();
}

void VPKFilePreview::addEntryPathToTree(const QString& entryPath) {
	// Normalize to VPK-style paths.
	QString p = entryPath;
	p = p.trimmed();
	p.replace('\\', '/');
	while (p.startsWith('/')) {
		p.remove(0, 1);
	}
	if (p.isEmpty()) {
		return;
	}

	// Split into components and build incremental directory keys: "a", "a/b", ...
	const auto parts = p.split('/', Qt::SkipEmptyParts);
	if (parts.isEmpty()) {
		return;
	}

	QStandardItem* parentItem = this->entriesModel->invisibleRootItem();
	QString dirKey;

	for (qsizetype i = 0; i < parts.size(); i++) {
		const bool isLeaf = (i == parts.size() - 1);
		const QString& name = parts[i];

		if (!isLeaf) {
			if (!dirKey.isEmpty()) {
				dirKey += '/';
			}
			dirKey += name;

			auto it = this->dirItems.find(dirKey);
			if (it != this->dirItems.end()) {
				parentItem = it.value();
				continue;
			}

			auto* dirItem = new QStandardItem(name);
			dirItem->setEditable(false);
			dirItem->setData(true, Qt::UserRole + 1); // isDir
			dirItem->setToolTip(dirKey);
			parentItem->appendRow(dirItem);

			this->dirItems.insert(dirKey, dirItem);
			parentItem = dirItem;
			continue;
		}

		// Leaf entry (file)
		auto* fileItem = new QStandardItem(name);
		fileItem->setEditable(false);
		fileItem->setData(false, Qt::UserRole + 1); // isDir
		fileItem->setData(p, Qt::UserRole + 2); // fullPath
		fileItem->setToolTip(p);
		parentItem->appendRow(fileItem);
	}
}

static bool itemLess(QStandardItem* a, QStandardItem* b) {
	const bool aDir = a->data(Qt::UserRole + 1).toBool();
	const bool bDir = b->data(Qt::UserRole + 1).toBool();
	if (aDir != bDir) {
		return aDir; // dirs first
	}
	return QString::compare(a->text(), b->text(), Qt::CaseInsensitive) < 0;
}

static void sortChildrenRec(QStandardItem* parent) {
	const int n = parent->rowCount();
	if (n <= 1) {
		// Still recurse into children.
		for (int i = 0; i < n; i++) {
			if (auto* c = parent->child(i)) {
				sortChildrenRec(c);
			}
		}
		return;
	}

	QVector<QStandardItem*> kids;
	kids.reserve(n);
	for (int i = 0; i < n; i++) {
		kids.push_back(parent->takeChild(i));
	}

	std::sort(kids.begin(), kids.end(), itemLess);
	for (int i = 0; i < kids.size(); i++) {
		parent->setChild(i, kids[i]);
	}

	for (int i = 0; i < parent->rowCount(); i++) {
		if (auto* c = parent->child(i)) {
			sortChildrenRec(c);
		}
	}
}

void VPKFilePreview::sortTree() {
	sortChildrenRec(this->entriesModel->invisibleRootItem());
}

void VPKFilePreview::stopEntriesWorker() {
	if (this->entriesWorker) {
		QMetaObject::invokeMethod(this->entriesWorker, "cancel", Qt::QueuedConnection);
		this->entriesWorker = nullptr;
	}
	if (!this->entriesThread) {
		return;
	}
	this->entriesThread->quit();
	this->entriesThread->wait();
	delete this->entriesThread;
	this->entriesThread = nullptr;
}

void VPKFilePreview::setVPKPath(const QString& absolutePath, const QString& relativePath) {
	this->absPath = absolutePath;
	this->entriesGeneration++;
	this->stopEntriesWorker();
	this->entriesModel->clear();
	this->dirItems.clear();

	const QFileInfo fi{absolutePath};
	const auto sizeBytes = static_cast<qulonglong>(fi.size());

	this->title->setText(relativePath);

	const auto nameLower = fi.fileName().toLower();
	bool isDirVpk = nameLower.endsWith("_dir.vpk");
	const bool looksRespawn = ::looksLikeRespawnVpkByName(absolutePath);
	if (looksRespawn) {
		// Respawn dir can be `_dir.vpk` or `_000.vpk` (TF2)
		isDirVpk = isDirVpk || nameLower.endsWith("_000.vpk");
	}

	QString kind = "VPK file";
	QString extra;

	// Read header for display only (do NOT use it to decide Respawn vs Valve; Valve v2 collides).
	if (const auto header = ::tryReadHeader16(absolutePath)) {
		const auto sig = ::readU32LE(*header, 0);
		const auto ver = ::readU32LE(*header, 4);
		const auto treeLen = ::readU32LE(*header, 8);
		if (sig == VPK_SIG) {
			if (looksRespawn) {
				kind = "Respawn VPK directory";
			} else {
				kind = "Valve VPK directory";
			}
			extra = QString("Header: sig=0x%1, ver=%2, treeLen=%3 bytes")
				.arg(QString::number(sig, 16))
				.arg(ver)
				.arg(treeLen);
		}
	}

	if (!isDirVpk) {
		kind = "VPK archive part";
		extra = "This is likely an archive part. Open the corresponding directory file (`*_dir.vpk` or `*_000.vpk`) to browse contents.";
	}

	QString md;
	md += QString("**Type:** %1  \n").arg(kind);
	md += QString("**Size:** %1 bytes  \n").arg(sizeBytes);
	if (!extra.isEmpty()) {
		md += QString("**Info:** %1").arg(extra);
	}

	this->details->setText(md);
	this->openButton->setEnabled(isDirVpk);

	// Entries list
	this->entriesBusy->hide();
	this->entriesSummary->clear();
	this->entriesTree->setEnabled(false);
	this->entriesTree->collapseAll();

	if (!isDirVpk) {
		this->entriesSummary->setText(tr("This appears to be an archive part. Open the corresponding directory file (*_dir.vpk or *_000.vpk) to list contents."));
		return;
	}

	this->entriesSummary->setText(tr("Listing files..."));
	this->entriesBusy->show();

	const auto generation = this->entriesGeneration;
	auto* worker = new VPKEntriesWorker{absolutePath, generation};
	this->entriesThread = new QThread(this);
	worker->moveToThread(this->entriesThread);
	this->entriesWorker = worker;

	QObject::connect(this->entriesThread, &QThread::started, worker, &VPKEntriesWorker::run);
	QObject::connect(this, &QObject::destroyed, worker, [worker] { worker->cancel(); }, Qt::QueuedConnection);

	QObject::connect(worker, &VPKEntriesWorker::chunkReady, this, [this](const QStringList& chunk, std::uint64_t gen) {
		if (gen != this->entriesGeneration) {
			return;
		}
		for (const auto& p : chunk) {
			this->addEntryPathToTree(p);
		}
	});

	QObject::connect(worker, &VPKEntriesWorker::finished, this, [this, worker](const QString& error, std::uint64_t gen, std::uint64_t total) {
		if (gen != this->entriesGeneration) {
			worker->deleteLater();
			return;
		}

		this->entriesBusy->hide();
		this->sortTree();
		this->entriesTree->setEnabled(true);
		this->entriesTree->expandToDepth(1);

		if (!error.isEmpty()) {
			this->entriesSummary->setText(tr("Failed to list files: %1").arg(error));
		} else {
			this->entriesSummary->setText(tr("%1 files").arg(static_cast<qulonglong>(total)));
		}

		worker->deleteLater();
		this->entriesWorker = nullptr;
		if (this->entriesThread) {
			this->entriesThread->quit();
		}
	});

	QObject::connect(this->entriesThread, &QThread::finished, this, [this] {
		// Thread object is owned by this widget; delete when finished.
		if (!this->entriesThread) {
			return;
		}
		this->entriesThread->deleteLater();
		this->entriesThread = nullptr;
	});

	this->entriesThread->start();
}

#include "VPKFilePreview.moc"
