#include <array>
#include <iterator>
#include <ostream>
#include <set>
#include <string>
#include <vector>

#include "algorithm.hpp"
#include "FontWriter.hpp"
#include "utility.hpp"

using namespace std;

bool FontWriter::AUTOHINT_FONTS = false;

const array<FontWriter::FontFormatInfo, 4> FontWriter::_formatInfos {{
	{FontWriter::FontFormat::SVG, "image/svg+xml", "svg", "svg"},
	{FontWriter::FontFormat::TTF, "application/x-font-ttf", "ttf", "truetype"},
	{FontWriter::FontFormat::WOFF, "application/x-font-woff", "woff", "woff"},
	{FontWriter::FontFormat::WOFF2, "application/x-font-woff2", "woff2", "woff2"},
}};

FontWriter::FontFormat FontWriter::toFontFormat (string formatstr) {
	formatstr = util::tolower(formatstr);
	auto it = algo::find_if(_formatInfos, [&](const FontFormatInfo &info) {
		return info.formatstr_short == formatstr;
	});
	return it != _formatInfos.end() ? it->format : FontFormat::UNKNOWN;
}

const FontWriter::FontFormatInfo* FontWriter::fontFormatInfo (FontFormat format) {
	auto it = algo::find_if(_formatInfos, [&](const FontFormatInfo &info) {
		return info.format == format;
	});
	return it != _formatInfos.end() ? &(*it) : nullptr;
}

vector<string> FontWriter::supportedFormats () {
	vector<string> formats;
	algo::transform(_formatInfos, back_inserter(formats), [](const FontFormatInfo &info) {
		return info.formatstr_short;
	});
	return formats;
}

string FontWriter::createFontFile (FontFormat, const set<int>&, GFGlyphTracer::Callback*) const {
	return "";
}

bool FontWriter::writeCSSFontFace (FontFormat, const set<int>&, ostream&, GFGlyphTracer::Callback*) const {
	return false;
}

bool FontWriter::createTTFFile (const string&, const PhysicalFont&, const set<int>&, GFGlyphTracer::Callback*) const {
	return false;
}
