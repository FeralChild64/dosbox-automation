// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#include "gui/truetype_freetype.h"

#include "augra/log.h"
#include "dosbox.h"
#include "utils/checks.h"

#include <fstream>
#include <vector>

CHECK_NARROWING();

static FT_Library library = {};

// Once loaded, this cannot be emptied until 'FT_Done_Face' is called
static std::vector<uint8_t> font_file_content = {};

// Limit the maximum supported file size for security/reliability purposes.
// Some fonts (like 'GNU Unifont'), containing a lot of glyphs, can be
// really large, but the limit below should still be sufficient.
constexpr size_t MaxFileSizeBytes = 20 * 1024 * 1024;

static bool evaluate_and_log(const FT_Error result, const char* function_name)
{
	if (result == FT_Err_Ok) {
		return true;
	}

	augra::log_error("ttf", "'%s' returned 0x%04x", function_name, result);
	return false;
}

bool FreeType::Init()
{
	const auto result = FT_Init_FreeType(&library);
	return evaluate_and_log(result, "FT_Init_FreeType");
}

bool FreeType::Done()
{
	const auto result = FT_Done_FreeType(library);
	return evaluate_and_log(result, "FT_Done_FreeType");
}

FT_UInt FreeType::GetCharIndex(const FT_Face face, const FT_ULong char_code)
{
	return FT_Get_Char_Index(face, char_code);
}

bool FreeType::NewFace(const std_fs::path& file_path_name,
                       const FT_Long face_index, FT_Face* face)
{
	// A directory opens fine and reports a bogus size on some file systems
	std::error_code error_code = {};
	if (!std_fs::is_regular_file(file_path_name, error_code) || error_code) {
		augra::log_warn("ttf",
		                "Could not open font file '%s'",
		                file_path_name.string().c_str());
		return false;
	}

	// Open the file
	std::ifstream file_stream = {};
	file_stream.open(file_path_name.string(), std::ios::binary);
	if (!file_stream.is_open()) {
		augra::log_warn("ttf",
		                "Could not open font file '%s'",
		                file_path_name.string().c_str());
		return false;
	}

	// Check file size
	file_stream.seekg(0, std::ios::end);
	const auto end_position = file_stream.tellg();
	file_stream.seekg(0, std::ios::beg);
	// tellg() is -1 on failure, which as a size_t would pass for a huge file
	if (!file_stream || end_position < 0) {
		augra::log_warn("ttf",
		                "Error reading font file '%s'",
		                file_path_name.string().c_str());
		return false;
	}
	const auto file_size = static_cast<size_t>(end_position);
	if (file_size > MaxFileSizeBytes) {
		augra::log_warn("ttf",
		                "Font file '%s' too large",
		                file_path_name.string().c_str());
		return false;
	}

	// Read the file
	font_file_content.resize(file_size);
	font_file_content.shrink_to_fit();
	file_stream.read(reinterpret_cast<char*>(font_file_content.data()), file_size);
	if (file_stream.gcount() != static_cast<std::streamsize>(file_size)) {
		augra::log_warn("ttf",
		                "Error reading font file '%s'",
		                file_path_name.string().c_str());
		font_file_content.clear();
		font_file_content.shrink_to_fit();
		return false;
	}
	file_stream.close();

	const auto result = FT_New_Memory_Face(
		library,
		reinterpret_cast<const FT_Byte*>(font_file_content.data()),
		static_cast<FT_Long>(file_size),
		face_index,
		face);

	if (result == FT_Err_Ok) {
		return true;
	}

	if (result == FT_Err_Unknown_File_Format) {
		augra::log_warn("ttf",
		                "Unknown font file format, '%s'",
		                file_path_name.string().c_str());
	} else {
		augra::log_warn("ttf",
		                "Could not load font file '%s'",
		                file_path_name.string().c_str());
	}

	font_file_content.clear();
	font_file_content.shrink_to_fit();
	return false;
}

bool FreeType::DoneFace(const FT_Face face)
{
	const auto result = FT_Done_Face(face);
	font_file_content.clear();
	font_file_content.shrink_to_fit();
	return evaluate_and_log(result, "FT_Done_Face");
}

bool FreeType::OutlineNew(const FT_UInt num_points, const FT_Int num_contours,
                          FT_Outline* outline)
{
	const auto result = FT_Outline_New(library, num_points, num_contours, outline);
	return evaluate_and_log(result, "FT_Outline_New");
}

bool FreeType::OutlineRender(FT_Outline* outline, FT_Raster_Params* params)
{
	const auto result = FT_Outline_Render(library, outline, params);
	return evaluate_and_log(result, "FT_Outline_Render");
}

bool FreeType::OutlineDone(FT_Outline* outline)
{
	const auto result = FT_Outline_Done(library, outline);
	return evaluate_and_log(result, "FT_Outline_Done");
}

bool FreeType::OutlineGetBBox(FT_Outline* outline, FT_BBox* bbox)
{
	const auto result = FT_Outline_Get_BBox(outline, bbox);
	return evaluate_and_log(result, "FT_Outline_Get_BBox");
}

void FreeType::OutlineTranslate(const FT_Outline* outline,
                                FT_Pos x_offset, FT_Pos y_offset)
{
	FT_Outline_Translate(outline, x_offset, y_offset);
}

bool FreeType::LoadGlyph(const FT_Face face, const FT_UInt glyph_index,
                         const FT_Int32 load_flags)
{
	const auto result = FT_Load_Glyph(face, glyph_index, load_flags);
	return evaluate_and_log(result, "FT_Load_Glyph");
}

bool FreeType::RenderGlyph(const FT_GlyphSlot slot, const FT_Render_Mode render_mode)
{
	const auto result = FT_Render_Glyph(slot, render_mode);
	return evaluate_and_log(result, "FT_Render_Glyph");
}

bool FreeType::GetGlyph(const FT_GlyphSlot slot, FT_Glyph* glyph)
{
	const auto result = FT_Get_Glyph(slot, glyph);
	return evaluate_and_log(result, "FT_Get_Glyph");
}

void FreeType::DoneGlyph(const FT_Glyph glyph)
{
	FT_Done_Glyph(glyph);
}

bool FreeType::SetPixelSizes(const FT_Face face, const FT_UInt pixel_width,
                             const FT_UInt pixel_height)
{
	const auto result = FT_Set_Pixel_Sizes(face, pixel_width, pixel_height);
	return evaluate_and_log(result, "FT_Set_Pixel_Sizes");
}

void FreeType::SetTransform(const FT_Face face, FT_Matrix* matrix, FT_Vector* delta)
{
	FT_Set_Transform(face, matrix, delta);
}

void FreeType::BitmapInit(FT_Bitmap* bitmap)
{
	FT_Bitmap_Init(bitmap);
}

void FreeType::BitmapDone(FT_Bitmap* bitmap)
{
	FT_Bitmap_Done(library, bitmap);
}
