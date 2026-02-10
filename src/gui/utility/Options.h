#pragma once

#include <string_view>

#include <QSettings>

// Options
constexpr std::string_view OPT_STYLE = "style";
constexpr std::string_view OPT_ENTRY_TREE_AUTO_EXPAND = "entry_list_auto_expand";
constexpr std::string_view OPT_ENTRY_TREE_ALLOW_DIR_DRAG = "entry_list_allow_dir_drag";
constexpr std::string_view OPT_ENTRY_TREE_ALLOW_FILE_DRAG = "entry_list_allow_file_drag";
constexpr std::string_view OPT_ENTRY_TREE_AUTO_COLLAPSE = "entry_list_auto_collapse";
constexpr std::string_view OPT_ENTRY_TREE_HIDE_ICONS = "entry_tree_hide_icons";
constexpr std::string_view OPT_ADVANCED_FILE_PROPS = "adv_mode";
constexpr std::string_view OPT_LANGUAGE_OVERRIDE = "language_override";
constexpr std::string_view OPT_ENABLE_DISCORD_RICH_PRESENCE = "enable_discord_rich_presence";
constexpr std::string_view OPT_DISABLE_STEAM_SCANNER = "disable_steam_scanner";

// Audio preview
constexpr std::string_view OPT_AUDIO_PREVIEW_VOLUME = "audio_preview_volume"; // double 0..1
constexpr std::string_view OPT_AUDIO_PREVIEW_AUTOPLAY = "audio_preview_autoplay"; // bool

// MDL preview
constexpr std::string_view OPT_MDL_GRID_ENABLED = "mdl_grid_enabled"; // bool
constexpr std::string_view OPT_MDL_GRID_SPACING = "mdl_grid_spacing"; // double (units between minor lines)
constexpr std::string_view OPT_MDL_GRID_EXTENT_CELLS = "mdl_grid_extent_cells"; // int (how many cells from origin)
constexpr std::string_view OPT_MDL_GRID_MAJOR_EVERY = "mdl_grid_major_every"; // int (major line every N cells)
constexpr std::string_view OPT_MDL_GRID_MINOR_COLOR = "mdl_grid_minor_color"; // QColor
constexpr std::string_view OPT_MDL_GRID_MAJOR_COLOR = "mdl_grid_major_color"; // QColor

// External tools
// If revpk is found (or configured), use it for Respawn VPK full pack/unpack operations.
constexpr std::string_view OPT_REVPK_USE_FOR_RESPAWN_PACK_UNPACK = "revpk_use_for_respawn_pack_unpack";
// Optional explicit path to revpk executable. If empty, we'll try to find it next to the app binary.
constexpr std::string_view OPT_REVPK_PATH = "revpk_path";
// Optional revpk LZHAM helper thread count. Use -1 for "max practical" (revpk default).
constexpr std::string_view OPT_REVPK_NUM_THREADS = "revpk_num_threads";
// Optional revpk compression level string: fastest|faster|default|better|uber
constexpr std::string_view OPT_REVPK_COMPRESSION_LEVEL = "revpk_compression_level";

// Storage
constexpr std::string_view STR_OPEN_RECENT = "open_recent";

namespace Options {

void setupOptions(QSettings& options);

QSettings* getOptions();

template<typename T>
T get(std::string_view option) {
	return getOptions()->value(option).value<T>();
}

template<typename T>
void set(std::string_view option, T value) {
	getOptions()->setValue(option, value);
}

// Only use for booleans!
void invert(std::string_view option);

} // namespace Options
