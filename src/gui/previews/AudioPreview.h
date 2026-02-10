#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <QProgressBar>

#include "../utility/AudioPlayer.h"

class QLabel;
class QMouseEvent;
class QPaintEvent;
class QCheckBox;
class QSlider;
class QToolButton;

class FileViewer;

class SeekBar : public QProgressBar {
	Q_OBJECT;

public:
	explicit SeekBar(QWidget* parent);

protected:
	void mousePressEvent(QMouseEvent* event) override;

signals:
	void seek(std::int64_t frame);

private:
	void processMouseEvent(QMouseEvent* event);

	QLabel* label;
};

class AudioPreview : public QWidget {
	Q_OBJECT;

public:
	static inline const QStringList EXTENSIONS {
		".wav",
	};

	explicit AudioPreview(FileViewer* fileViewer_, QWidget* parent = nullptr);
	~AudioPreview() override;

	void setData(const std::vector<std::byte>& data);

protected:
	void paintEvent(QPaintEvent* event) override;

private:
	void setPlaying(bool play);

	FileViewer* fileViewer;

	QToolButton* playPauseButton;
	SeekBar* seekBar;
	QSlider* volumeSlider;
	QLabel* volumeLabel;
	QCheckBox* autoplayCheckbox;
	QLabel* infoLabel;

	bool playing;
	std::vector<std::byte> persistentAudioData;
};
