#include "AudioPreview.h"

#include <algorithm>
#include <cmath>

#include <QGridLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QLabel>
#include <QMouseEvent>
#include <QProgressBar>
#include <QSlider>
#include <QStyle>
#include <QTime>
#include <QTimer>
#include <QToolButton>

#include "../FileViewer.h"
#include "../utility/Options.h"

SeekBar::SeekBar(QWidget* parent)
		: QProgressBar(parent)
		, label(new QLabel(this)) {
	this->setRange(0, 1000);
	this->setOrientation(Qt::Horizontal);
	this->setTextVisible(false);

	auto* layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);

	this->label->setAlignment(Qt::AlignCenter);
	QObject::connect(this, &QProgressBar::valueChanged, this, [this](int) {
		QString formatter;
		if (AudioPlayer::getLengthInSeconds() >= 60 * 60 * 24) {
			formatter = "dd:hh:mm:ss.zzz";
		} else if (AudioPlayer::getLengthInSeconds() >= 60 * 60) {
			formatter = "hh:mm:ss.zzz";
		} else if (AudioPlayer::getLengthInSeconds() >= 60) {
			formatter = "mm:ss.zzz";
		} else {
			formatter = "ss.zzz";
		}

		const auto text = QString("%1 / %2").arg(
				QTime(0, 0).addMSecs(static_cast<int>(AudioPlayer::getPositionInSeconds() * 1000)).toString(formatter),
				QTime(0, 0).addMSecs(static_cast<int>(AudioPlayer::getLengthInSeconds() * 1000)).toString(formatter));
		this->label->setText(text);
	});
	layout->addWidget(this->label);
}

void SeekBar::mousePressEvent(QMouseEvent* event) {
	this->processMouseEvent(event);
	QWidget::mousePressEvent(event);
}

void SeekBar::processMouseEvent(QMouseEvent* event) {
	if (event->button() != Qt::MouseButton::LeftButton) {
		return;
	}
	event->accept();
	const auto percent = static_cast<double>(event->pos().x()) / static_cast<double>(std::max(1, this->width()));
	emit this->seek(static_cast<std::int64_t>(percent * static_cast<double>(AudioPlayer::getLengthInFrames())));
}

AudioPreview::AudioPreview(FileViewer* fileViewer_, QWidget* parent)
		: QWidget(parent)
		, fileViewer(fileViewer_)
		, playPauseButton(new QToolButton(this))
		, seekBar(new SeekBar(this))
		, volumeSlider(new QSlider(Qt::Horizontal, this))
		, volumeLabel(new QLabel(this))
		, autoplayCheckbox(new QCheckBox(tr("Autoplay"), this))
		, infoLabel(new QLabel(this))
		, playing(false) {
	this->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::MinimumExpanding);
	this->setUpdatesEnabled(true);

	auto* layout = new QGridLayout(this);
	layout->setColumnStretch(0, 20);
	layout->setColumnStretch(2, 20);
	layout->setRowStretch(0, 20);
	layout->setRowStretch(3, 20);
	layout->setSpacing(0);

	auto* controls = new QWidget(this);
	layout->addWidget(controls, 1, 1);

	auto* controlsLayout = new QHBoxLayout(controls);
	controlsLayout->setSpacing(0);

	this->playPauseButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
	this->playPauseButton->setShortcut(Qt::Key_Space);
	QObject::connect(this->playPauseButton, &QToolButton::pressed, this, [this] {
		if (!this->playing && AudioPlayer::getPositionInFrames() == AudioPlayer::getLengthInFrames()) {
			AudioPlayer::seekToFrame(0);
		}
		this->setPlaying(!this->playing);
	});
	controlsLayout->addWidget(this->playPauseButton);

	controlsLayout->addSpacing(4);

	this->seekBar->setFixedWidth(300);
	QObject::connect(this->seekBar, &SeekBar::seek, this, [this](std::int64_t frame) {
		AudioPlayer::seekToFrame(frame);
		if (!this->playing) {
			this->setPlaying(true);
		}
	});
	controlsLayout->addWidget(this->seekBar);

	controlsLayout->addSpacing(8);

	// Volume: persisted (0..1) as a double; slider is 0..100.
	this->volumeSlider->setRange(0, 100);
	const double savedVol = std::clamp(Options::get<double>(OPT_AUDIO_PREVIEW_VOLUME), 0.0, 1.0);
	this->volumeSlider->setValue(static_cast<int>(std::round(savedVol * 100.0)));
	this->volumeSlider->setFixedWidth(120);

	this->volumeLabel->setText(tr("Vol %1%").arg(this->volumeSlider->value()));
	this->volumeLabel->setMinimumWidth(60);

	QObject::connect(this->volumeSlider, &QSlider::valueChanged, this, [this](int v) {
		this->volumeLabel->setText(tr("Vol %1%").arg(v));
		const double vol01 = std::clamp(static_cast<double>(v) / 100.0, 0.0, 1.0);
		Options::set(OPT_AUDIO_PREVIEW_VOLUME, vol01);
		AudioPlayer::setVolume(static_cast<float>(vol01));
	});

	controlsLayout->addWidget(this->volumeLabel);
	controlsLayout->addWidget(this->volumeSlider);

	controlsLayout->addSpacing(8);

	this->autoplayCheckbox->setChecked(Options::get<bool>(OPT_AUDIO_PREVIEW_AUTOPLAY));
	QObject::connect(this->autoplayCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
		Options::set(OPT_AUDIO_PREVIEW_AUTOPLAY, checked);
		// If toggled on while a file is loaded, start playing.
		if (checked && AudioPlayer::initialized() && !this->playing) {
			this->setPlaying(true);
		}
	});
	controlsLayout->addWidget(this->autoplayCheckbox);

	layout->addWidget(this->infoLabel, 2, 1, Qt::AlignHCenter);

	this->setPlaying(false);

	auto* timer = new QTimer(this);
	QObject::connect(timer, &QTimer::timeout, this, [this] {
		if (AudioPlayer::getLengthInFrames() > 0) {
			const double pos = static_cast<double>(AudioPlayer::getPositionInFrames());
			const double len = static_cast<double>(AudioPlayer::getLengthInFrames());
			this->seekBar->setValue(static_cast<int>(std::round((pos / len) * this->seekBar->maximum())));
			if (AudioPlayer::getPositionInFrames() == AudioPlayer::getLengthInFrames()) {
				this->setPlaying(false);
			}
			this->infoLabel->setText(tr("Sample Rate: %1hz\nChannels: %2").arg(AudioPlayer::getSampleRate()).arg(AudioPlayer::getChannelCount()));
		} else {
			this->seekBar->setValue(0);
		}
	});
	timer->start(10);
}

AudioPreview::~AudioPreview() {
	AudioPlayer::deinitAudio();
}

void AudioPreview::setData(const std::vector<std::byte>& data) {
	this->persistentAudioData = data;
	// Stop any current playback before re-init.
	this->setPlaying(false);
	const auto err = AudioPlayer::initAudio(this->persistentAudioData.data(), this->persistentAudioData.size());
	if (!err.isEmpty()) {
		this->fileViewer->showInfoPreview({":/icons/warning.png"}, err);
		this->setPlaying(false);
	} else {
		// Apply persisted volume and honor autoplay setting.
		const double vol01 = std::clamp(Options::get<double>(OPT_AUDIO_PREVIEW_VOLUME), 0.0, 1.0);
		// Keep UI consistent if settings were edited elsewhere.
		const int volPct = static_cast<int>(std::round(vol01 * 100.0));
		if (this->volumeSlider->value() != volPct) {
			this->volumeSlider->setValue(volPct);
		}
		AudioPlayer::setVolume(static_cast<float>(vol01));
		const bool autoplay = Options::get<bool>(OPT_AUDIO_PREVIEW_AUTOPLAY);
		if (this->autoplayCheckbox->isChecked() != autoplay) {
			this->autoplayCheckbox->setChecked(autoplay);
		}
		this->setPlaying(autoplay);
	}
}

void AudioPreview::paintEvent(QPaintEvent* event) {
	QWidget::paintEvent(event);
}

void AudioPreview::setPlaying(bool play) {
	this->playing = play;
	if (this->playing) {
		AudioPlayer::play();
		this->playPauseButton->setIcon(this->style()->standardIcon(QStyle::SP_MediaPause));
	} else {
		AudioPlayer::pause();
		this->playPauseButton->setIcon(this->style()->standardIcon(QStyle::SP_MediaPlay));
	}
}
