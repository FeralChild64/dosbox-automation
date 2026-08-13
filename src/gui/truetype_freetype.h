// SPDX-FileCopyrightText:  2026 dosbox-automation Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_TRUETYPE_FREETYPE_H
#define DOSBOX_TRUETYPE_FREETYPE_H

#include "ft2build.h"
#include FT_FREETYPE_H
#include FT_BBOX_H
#include FT_GLYPH_H
#include FT_OUTLINE_H
#include FT_BITMAP_H

#include "misc/std_filesystem.h"

// Wrappers for the FreeType functions, printing error logs in case of failure

namespace FreeType {

bool Init();
bool Done();

bool NewFace(const std_fs::path& file_path_name, const FT_Long face_index,
             FT_Face* face);
bool DoneFace(const FT_Face face);

FT_UInt GetCharIndex(const FT_Face face, const FT_ULong char_code);

bool OutlineNew(const FT_UInt num_points, const FT_Int num_contours,
                FT_Outline* outline);
bool OutlineRender(FT_Outline* outline, FT_Raster_Params* params);
bool OutlineDone(FT_Outline* outline);

bool OutlineGetBBox(FT_Outline* outline, FT_BBox* bbox);
void OutlineTranslate(const FT_Outline* outline, FT_Pos x_offset, FT_Pos y_offset);

bool LoadGlyph(const FT_Face face, const FT_UInt glyph_index,
               const FT_Int32 load_flags);
bool RenderGlyph(const FT_GlyphSlot slot, const FT_Render_Mode render_mode);
bool GetGlyph(const FT_GlyphSlot slot, FT_Glyph* glyph);
void DoneGlyph(const FT_Glyph glyph);

bool SetPixelSizes(const FT_Face face, const FT_UInt pixel_width,
                   const FT_UInt pixel_height);
void SetTransform(const FT_Face face, FT_Matrix* matrix, FT_Vector* delta);

void BitmapInit(FT_Bitmap* bitmap);
void BitmapDone(FT_Bitmap* bitmap);

} // namespace FreeType

#endif // DOSBOX_TRUETYPE_FREETYPE_H
