/**
  * Touhou Community Reliant Automatic Patcher
  * Tasogare Frontier support plugin
  *
  * ----
  *
  * Image patching for nsml engine.
  */

#include <thcrap.h>
#include "./png.h"
#include "thcrap_tasofro.h"
#include "th155_bmp_font.h"

size_t get_bmp_font_size(const char *fn, json_t *patch, size_t patch_size)
{
	size_t size = 0;

	VLA(char, fn_png, strlen(fn) + 5);
	strcpy(fn_png, fn);
	strcat(fn_png, ".png");

	uint32_t width, height;
	bool IHDR_read = png_image_get_IHDR(fn_png, &width, &height, nullptr);
	if (IHDR_read) {
		size += sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + width * height;
	}

	VLA(char, fn_bin, strlen(fn) + 5);
	strcpy(fn_bin, fn);
	strcat(fn_bin, ".bin");

	size_t bin_size;
	void *bin_data = stack_game_file_resolve(fn_bin, &bin_size);
	if (bin_data) {
		free(bin_data);
		size += bin_size;
	}

	VLA_FREE(fn_png);
	VLA_FREE(fn_bin);
	return size;
}

int patch_bmp_font(void *file_inout, size_t size_out, size_t size_in, const char *fn, json_t *patch)
{
	BITMAPFILEHEADER *bpFile = (BITMAPFILEHEADER*)file_inout;
	BITMAPINFOHEADER *bpInfo = (BITMAPINFOHEADER*)(bpFile + 1);
	BYTE *bpData = (BYTE*)(bpInfo + 1);

	VLA(char, fn_png, strlen(fn) + 5);
	strcpy(fn_png, fn);
	strcat(fn_png, ".png");

	uint32_t width, height;
	uint8_t bpp;
	BYTE** row_pointers = png_image_read(fn_png, &width, &height, &bpp, false);
	VLA_FREE(fn_png);

	if (row_pointers) {
		if (!row_pointers || size_out < sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + width * height) {
			log_print("Destination buffer too small!\n");
			free(row_pointers);
			return -1;
		}

		bpFile->bfType = 0x4D42;
		bpFile->bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + width * height;
		bpFile->bfReserved1 = 0;
		bpFile->bfReserved2 = 0;
		bpFile->bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

		bpInfo->biSize = sizeof(BITMAPINFOHEADER);
		bpInfo->biWidth = width / 4;
		bpInfo->biHeight = height;
		bpInfo->biPlanes = 1;
		bpInfo->biBitCount = 32;
		bpInfo->biCompression = BI_RGB;
		bpInfo->biSizeImage = 0;
		bpInfo->biXPelsPerMeter = 0;
		bpInfo->biYPelsPerMeter = 0;
		bpInfo->biClrUsed = 0;
		bpInfo->biClrImportant = 0;

		for (unsigned int h = 0; h < height; h++) {
			for (unsigned int w = 0; w < width; w++) {
				bpData[h * width + w] = row_pointers[height - h - 1][w];
			}
		}
		free(row_pointers);
	}

	VLA(char, fn_bin, strlen(fn) + 5);
	strcpy(fn_bin, fn);
	strcat(fn_bin, ".bin");

	size_t bin_size;
	void *bin_data = stack_game_file_resolve(fn_bin, &bin_size);
	if (bin_data) {
		if (size_out < sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + bpInfo->biWidth * 4 * bpInfo->biHeight + bin_size) {
			log_print("Destination buffer too small!\n");
			free(bin_data);
			return -1;
		}

		void *metadata = (BYTE*)file_inout + bpFile->bfSize;
		memcpy(metadata, bin_data, bin_size);

		free(bin_data);
	}
	return 1;
}

extern "C" int BP_c_bmpfont_fix_parameters(DWORD xmm0, DWORD xmm1, DWORD xmm2, x86_reg_t *regs, json_t *bp_info)
{
	// Parameters
	// ----------
	const char *string = (const char*)json_object_get_immediate(bp_info, regs, "string");
	size_t string_location = json_object_get_immediate(bp_info, regs, "string_location");
	int *offset = (int*)json_object_get_pointer(bp_info, regs, "offset");
	size_t *c1 = json_object_get_pointer(bp_info, regs, "c1");
	size_t *c2 = json_object_get_pointer(bp_info, regs, "c2");
	// ----------

	if (string_location >= 0x10) {
		string = *(const char**)string;
	}
	string += *offset;

	wchar_t wc[2];
	size_t codepoint_len = CharNextU(string) - string;
	MultiByteToWideCharU(0, 0, string, codepoint_len, wc, 2);
	*c1 = wc[0] >> 8;
	*c2 = wc[0] & 0xFF;

	*offset += codepoint_len - 1;

	return 1;
}
