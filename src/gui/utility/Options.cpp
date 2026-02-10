#include "Options.h"

#include <QApplication>
#include <QFileInfo>
#include <QMetaType>
#include <QColor>
#include <QStyle>

Q_DECLARE_METATYPE(QStringList)

QSettings* opts = nullptr;

void Options::setupOptions(QSettings& options) {
	if (!options.contains(OPT_STYLE)) {
		options.setValue(OPT_STYLE, QApplication::style()->name());
	}
	QApplication::setStyle(options.value(OPT_STYLE).toString());

	if (!options.contains(OPT_ENTRY_TREE_AUTO_EXPAND)) {
		options.setValue(OPT_ENTRY_TREE_AUTO_EXPAND, false);
	}

	if (!options.contains(OPT_ENTRY_TREE_ALLOW_DIR_DRAG)) {
		options.setValue(OPT_ENTRY_TREE_ALLOW_DIR_DRAG, true);
	}

	if (!options.contains(OPT_ENTRY_TREE_ALLOW_FILE_DRAG)) {
		options.setValue(OPT_ENTRY_TREE_ALLOW_FILE_DRAG, true);
	}

	if (!options.contains(OPT_ENTRY_TREE_AUTO_COLLAPSE)) {
		options.setValue(OPT_ENTRY_TREE_AUTO_COLLAPSE, false);
	}

	if (!options.contains(OPT_ENTRY_TREE_HIDE_ICONS)) {
		options.setValue(OPT_ENTRY_TREE_HIDE_ICONS, false);
	}

	if (!options.contains(OPT_ADVANCED_FILE_PROPS)) {
		options.setValue(OPT_ADVANCED_FILE_PROPS, false);
	}

	if (!options.contains(OPT_LANGUAGE_OVERRIDE)) {
		options.setValue(OPT_LANGUAGE_OVERRIDE, QString{});
	}

	if (!options.contains(OPT_ENABLE_DISCORD_RICH_PRESENCE)) {
		options.setValue(OPT_ENABLE_DISCORD_RICH_PRESENCE, true);
	}

	if (!options.contains(OPT_DISABLE_STEAM_SCANNER)) {
		options.setValue(OPT_DISABLE_STEAM_SCANNER, false);
	}

	if (!options.contains(OPT_AUDIO_PREVIEW_VOLUME)) {
		options.setValue(OPT_AUDIO_PREVIEW_VOLUME, 0.5);
	}

	if (!options.contains(OPT_AUDIO_PREVIEW_AUTOPLAY)) {
		options.setValue(OPT_AUDIO_PREVIEW_AUTOPLAY, false);
	}

	if (!options.contains(OPT_MDL_GRID_ENABLED)) {
		options.setValue(OPT_MDL_GRID_ENABLED, true);
	}

	if (!options.contains(OPT_MDL_GRID_SPACING)) {
		options.setValue(OPT_MDL_GRID_SPACING, 64.0);
	}

	if (!options.contains(OPT_MDL_GRID_EXTENT_CELLS)) {
		options.setValue(OPT_MDL_GRID_EXTENT_CELLS, 10);
	}

	if (!options.contains(OPT_MDL_GRID_MAJOR_EVERY)) {
		options.setValue(OPT_MDL_GRID_MAJOR_EVERY, 5);
	}

	if (!options.contains(OPT_MDL_GRID_MINOR_COLOR)) {
		options.setValue(OPT_MDL_GRID_MINOR_COLOR, QColor(80, 80, 80, 180));
	}

	if (!options.contains(OPT_MDL_GRID_MAJOR_COLOR)) {
		options.setValue(OPT_MDL_GRID_MAJOR_COLOR, QColor(130, 130, 130, 220));
	}

	if (!options.contains(OPT_REVPK_USE_FOR_RESPAWN_PACK_UNPACK)) {
		options.setValue(OPT_REVPK_USE_FOR_RESPAWN_PACK_UNPACK, true);
	}

	if (!options.contains(OPT_REVPK_PATH)) {
		options.setValue(OPT_REVPK_PATH, QString{});
	}

	if (!options.contains(OPT_REVPK_NUM_THREADS)) {
		options.setValue(OPT_REVPK_NUM_THREADS, -1);
	}

	if (!options.contains(OPT_REVPK_COMPRESSION_LEVEL)) {
		options.setValue(OPT_REVPK_COMPRESSION_LEVEL, "default");
	}

	if (!options.contains(STR_OPEN_RECENT)) {
		options.setValue(STR_OPEN_RECENT, QStringList{});
	}

	opts = &options;
}

QSettings* Options::getOptions() {
	return opts;
}

void Options::invert(std::string_view option) {
	set(option, !get<bool>(option));
}
