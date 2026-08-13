// SPDX-FileCopyrightText:  2026 dosbox-automation Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gui/truetype_freetype.h"

#include "augra/log.h"
#include "dosbox.h"
#include "utils/checks.h"

CHECK_NARROWING();

static FT_Library library = {};

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
	const auto result = FT_New_Face(library, file_path_name.c_str(), face_index, face);
	if (result == FT_Err_Ok) {
		return true;
	}

	if (result == FT_Err_Unknown_File_Format) {
		augra::log_warn("ttf",
		                "Unknown font file format, '%s'",
		                file_path_name.c_str());
	} else {
		augra::log_warn("ttf",
		                "Could not load font file '%s'",
		                file_path_name.c_str());
	}
	return false;
}

bool FreeType::DoneFace(const FT_Face face)
{
	const auto result = FT_Done_Face(face);
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
