#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Color.hpp"
#include "DVIActions.hpp"
#include "DVIToSVG.hpp"
#include "FileFinder.hpp"
#include "Font.hpp"
#include "FontManager.hpp"
#include "FontMap.hpp"
#include "HyperlinkManager.hpp"
#include "Message.hpp"
#include "SpecialManager.hpp"
#include "SVGOutput.hpp"
#include "SVGTree.hpp"
#include "XMLString.hpp"

namespace fs = std::filesystem;

namespace {

std::mutex g_mutex;
bool g_initialized = false;
std::string g_last_error;
FontMap::Snapshot g_default_font_map;

struct ConvertOptions {
	std::string xdv_path;
	std::string output_path;
	std::string page = "1";
	std::string bbox = "papersize";
	bool exact_bbox = false;
	bool no_fonts = false;
};

std::string path_string (const fs::path &path) {
	return path.u8string();
}

void set_last_error (std::string message) {
	g_last_error = std::move(message);
}

bool starts_with (const std::string &text, const std::string &prefix) {
	return text.rfind(prefix, 0) == 0;
}

std::string require_value (int argc, char **argv, int &index, const std::string &option) {
	if (index + 1 >= argc)
		throw std::runtime_error("missing value for " + option);
	return argv[++index];
}

ConvertOptions parse_convert_options (int argc, char **argv) {
	ConvertOptions options;
	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i] ? argv[i] : "";
		if (arg == "--page" || arg == "-p")
			options.page = require_value(argc, argv, i, arg);
		else if (starts_with(arg, "--page="))
			options.page = arg.substr(7);
		else if (starts_with(arg, "-p") && arg.size() > 2)
			options.page = arg.substr(2);
		else if (arg == "--bbox" || arg == "-b")
			options.bbox = require_value(argc, argv, i, arg);
		else if (starts_with(arg, "--bbox="))
			options.bbox = arg.substr(7);
		else if (starts_with(arg, "-b") && arg.size() > 2)
			options.bbox = arg.substr(2);
		else if (arg == "--output" || arg == "-o")
			options.output_path = require_value(argc, argv, i, arg);
		else if (starts_with(arg, "--output="))
			options.output_path = arg.substr(9);
		else if (starts_with(arg, "-o") && arg.size() > 2)
			options.output_path = arg.substr(2);
		else if (arg == "--exact-bbox" || arg == "-e")
			options.exact_bbox = true;
		else if (arg == "--no-fonts" || arg == "-n")
			options.no_fonts = true;
		else if (!arg.empty() && arg[0] == '-')
			throw std::runtime_error("unsupported dvisvgmdaemon option: " + arg);
		else
			options.xdv_path = arg;
	}

	if (options.xdv_path.empty())
		throw std::runtime_error("missing XDV input path");
	if (options.output_path.empty()) {
		fs::path output = fs::u8path(options.xdv_path);
		output.replace_extension(".svg");
		options.output_path = path_string(output);
	}
	return options;
}

bool read_default_font_maps () {
	FontMap::instance().clear();
	bool found = false;
	for (const char *map : {"dvisvgm.map", "ps2pk.map", "pdftex.map", "dvipdfm.map", "psfonts.map"})
		found = FontMap::instance().read(map) || found;
	g_default_font_map = FontMap::instance().snapshot();
	return found;
}

void configure_process (const char *argv0) {
	Message::LEVEL = Message::ERRORS | Message::WARNINGS;
	Message::COLORIZE = false;

	FileFinder::init(argv0 ? argv0 : "dvisvgmdaemon", "dvisvgm", false);
	(void)FileFinder::instance();
	read_default_font_maps();

	DVIToSVG::COMPUTE_PROGRESS = false;
	DVIToSVG::TRACE_MODE = 0;

	SVGTree::CREATE_CSS = true;
	SVGTree::RELATIVE_PATH_CMDS = false;
	SVGTree::MERGE_CHARS = true;
	SVGTree::ADD_COMMENTS = false;
	SVGTree::ZOOM_FACTOR = 1.0;
	SVGTree::EMBED_BITMAP_DATA = false;

	Color::SUPPRESS_COLOR_NAMES = true;
	PhysicalFont::KEEP_TEMP_FILES = false;
	PhysicalFont::METAFONT_MAG = 4;
	XMLString::DECIMAL_PLACES = 6;
}

void reset_document_state () {
	FontManager::instance().reset();
	HyperlinkManager::instance().reset();
	SpecialManager::instance().unregisterHandlers();
	FontMap::instance().restore(g_default_font_map);
	DVIToSVG::setProcessSpecials("", true);
}

int convert_document (const ConvertOptions &options) {
	reset_document_state();

	SVGTree::USE_FONTS = !options.no_fonts;
	SVGTree::CREATE_USE_ELEMENTS = options.no_fonts;
	PhysicalFont::EXACT_BBOX = options.exact_bbox;

	std::ifstream input(fs::u8path(options.xdv_path), std::ios::binary);
	if (!input)
		throw std::runtime_error("cannot open XDV input: " + options.xdv_path);

	SVGOutput svg_output(options.xdv_path, options.output_path, 0);
	DVIToSVG converter(input, svg_output);
	converter.setPageSize(options.bbox);
	converter.setPageTransformation("");
	converter.setUserMessage("");
	converter.convert(options.page);

	if (!fs::exists(fs::u8path(options.output_path)))
		throw std::runtime_error("dvisvgmdaemon did not write SVG output: " + options.output_path);
	return 0;
}

int ensure_initialized_locked (int argc, char **argv) {
	if (g_initialized)
		return 0;
	const char *program = (argc > 0 && argv && argv[0]) ? argv[0] : "dvisvgmdaemon";
	configure_process(program);
	g_initialized = true;
	g_last_error.clear();
	return 0;
}

}  // namespace

extern "C" __declspec(dllexport) int dvisvgmdaemon_init (int argc, char **argv) {
	std::lock_guard<std::mutex> lock(g_mutex);
	try {
		return ensure_initialized_locked(argc, argv);
	}
	catch (const std::exception &ex) {
		set_last_error(ex.what());
		return 1;
	}
}

extern "C" __declspec(dllexport) int dvisvgmdaemon_convert (int argc, char **argv) {
	std::lock_guard<std::mutex> lock(g_mutex);
	try {
		int code = ensure_initialized_locked(argc, argv);
		if (code != 0)
			return code;
		g_last_error.clear();
		return convert_document(parse_convert_options(argc, argv));
	}
	catch (const std::exception &ex) {
		set_last_error(ex.what());
		return 1;
	}
}

extern "C" __declspec(dllexport) int dvisvgmdaemon_shutdown (void) {
	std::lock_guard<std::mutex> lock(g_mutex);
	SpecialManager::instance().unregisterHandlers();
	FontManager::instance().reset();
	HyperlinkManager::instance().reset();
	FontMap::instance().clear();
	g_initialized = false;
	g_last_error.clear();
	return 0;
}

extern "C" __declspec(dllexport) const char *dvisvgmdaemon_last_error_message (void) {
	return g_last_error.c_str();
}

extern "C" __declspec(dllexport) int dlldvisvgmmain (int argc, char **argv) {
	int code = dvisvgmdaemon_init(argc, argv);
	if (code != 0)
		return code;
	return dvisvgmdaemon_convert(argc, argv);
}
